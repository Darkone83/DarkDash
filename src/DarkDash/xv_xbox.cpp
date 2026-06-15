/*===========================================================================
    xv_xbox.cpp -- X-View USB display class driver for the original Xbox
                   Team Resurgent / Darkone83

    Finds the X-View vendor device (CAFE:4001) on the USB bus and opens its
    bulk OUT (0x01) / bulk IN (0x81) pipes, then exposes synchronous transfers.

    Faithful port of xb_cam.cpp. Two detection paths, same as the camera:
      1. device already enumerated as a g_DeviceTree node -> open pipes directly
      2. device sitting on a TI-hub port "connected but not enabled" (USBD did
         not auto-enumerate it) -> ScanHub / ResetPort / BringUpManual, then open
    The ONLY change from the camera is the tail: we open bulk OUT/IN instead of
    bringing up an iso stream.

    Lock-proof submit = NULL completion + poll Hdr.Status (NOT KEVENT wait).
    All DMA buffers MmAllocateContiguousMemory'd + MmIsAddressValid-gated.
===========================================================================*/

#include <xtl.h>
#include "xbox_usb.h"
#include "xv_xbox.h"
#include "xv_protocol.h"

#define XV_TAG "XVIEW"

#define XV_NODE_SIZE      0x20
#define XV_MAX_NODES      32
#define XV_TREE_BASE_OFF  0xE0
#define XV_IDX_NONE       0x80
#define XV_USBD_PENDING   0x40000000u

#define XV_EP_OUT         0x01     /* bulk OUT (host->device)  */
#define XV_EP_IN          0x81     /* bulk IN  (device->host)  */
#define XV_BULK_MAXPKT    64       /* full-speed bulk max packet */

#define XV_TXBUF_SZ       32768    /* contiguous DMA scratch for OUT (blit bands) */
#define XV_RXBUF_SZ       128      /* contiguous DMA scratch for IN  */

/* TI internal hub (the Xbox controller ports hang off this) */
#define XV_TI_HUB_VID     0x0451
#define XV_TI_HUB_PID     0x2046

/* SubmitRequest routes these to OpenDefaultEndpoint / CloseDefaultEndpoint */
#define XV_URB_OPEN_DEFAULT_EP   0x82
#define XV_URB_CLOSE_DEFAULT_EP  0xC3

/* USB hub port feature codes */
#define XV_PORT_RESET     4
#define XV_C_PORT_RESET   20
#define XV_PORT_POWER        8     /* hub port feature: power                 */
#define XV_C_PORT_CONNECTION 16    /* hub port change: connection latch       */
#define XV_PORT_STAT_POWER   0x0100/* wPortStatus bit8: port is powered       */

/*---------------------------------------------------------------------------
    State
---------------------------------------------------------------------------*/
static IUsbDevice* s_dev = 0;
static int         s_present = 0;     /* X-View found (node or manual)       */
static int         s_ready = 0;     /* both pipes open                     */
static int         s_port = -1;
static int         s_vid = -1;
static int         s_pid = -1;
static int         s_addCalls = 0;
static LONG        s_linkLost = 0;     /* set by XViewRemoveDevice (USBD push); plain LONG for Interlocked */
static int         s_removeCalls = 0;
static IUsbDevice* s_pendDev = 0;    /* node handed to us by XViewAddDevice (clean path) */
static LONG        s_pendAdd = 0;    /* 1 = a USBD Add is waiting to be adopted          */
static int         s_manualTries = 0;  /* manual bring-ups attempted -- LEAK GUARD          */
#define XV_MANUAL_CAP 10               /* AllocDevice/Address have no free API on this layer;
                                          cap failed manual bring-ups so the USB node/address
                                          pool can never exhaust (which would force a full
                                          console reboot). A dash restart resets this budget. */
static XvLogFn     s_logfn = 0;

static IUsbDevice* s_hubDev = 0;     /* the TI hub (for port control)       */
static int         s_hubPort = -1;    /* hub port holding the unenumerated X-View */
static int         s_hubNports = 0;    /* hub downstream port count           */
static unsigned char s_portEnabled[9]; /* 1-indexed: 1 = connected+enabled (occupied) */
static unsigned char s_portCand[9];    /* 1-indexed: 1 = connected but NOT enabled (live) */

static void* s_epOut = 0;     /* opened bulk OUT handle */
static void* s_epIn = 0;     /* opened bulk IN  handle */
static ULONG       s_toggleOut = 0;    /* persistent data-toggle storage [V]  */
static ULONG       s_toggleIn = 0;

static unsigned char* s_txbuf = 0;     /* contiguous OUT DMA scratch */
static unsigned char* s_rxbuf = 0;     /* contiguous IN  DMA scratch */

static volatile LONG s_lastRaw = 0;    /* raw SubmitRequest() return, logging */

static void Xv_Log(const char* msg)
{
    if (s_logfn) s_logfn(XV_TAG, msg);
}
static void Xv_LogInt(const char* msg, int v)
{
    char buf[96]; int i = 0, j, n; char tmp[16]; int neg = 0; unsigned int u;
    while (msg[i] && i < 70) { buf[i] = msg[i]; i++; }
    buf[i++] = ' ';
    if (v < 0) { neg = 1; u = (unsigned int)(-v); }
    else u = (unsigned int)v;
    n = 0; if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) buf[i++] = '-';
    for (j = n - 1; j >= 0; j--) buf[i++] = tmp[j];
    buf[i] = 0;
    if (s_logfn) s_logfn(XV_TAG, buf);
}

/*---------------------------------------------------------------------------
    Device tree + safe-read  (mirrors xb_cam.cpp)
---------------------------------------------------------------------------*/
class CDeviceTree {
public:
    char _opaque[0x200];
    IUsbDevice* AllocDevice();   /* ?AllocDevice@CDeviceTree@@QAEPAVIUsbDevice@@XZ */
};
extern CDeviceTree g_DeviceTree;
extern "C" BOOLEAN __stdcall MmIsAddressValid(PVOID VirtualAddress);

/* Xbox kernel contiguous-memory allocators (kernel exports, __stdcall). Declared
   here so this driver is self-contained -- no xbox_kernel.h needed. */
extern "C" PVOID __stdcall MmAllocateContiguousMemory(ULONG NumberOfBytes);
extern "C" VOID  __stdcall MmFreeContiguousMemory(PVOID BaseAddress);

/* framework USB address allocator (used by the manual bring-up path):
   ?USBD_AllocateUsbAddress@@YIEPAU_USBD_HOST_CONTROLLER@@@Z  __fastcall */
struct _USBD_HOST_CONTROLLER;
unsigned char __fastcall USBD_AllocateUsbAddress(struct _USBD_HOST_CONTROLLER* hc);

static int Xv_Readable(const void* p, int len)
{
    if (p == 0) return 0;
    if (!MmIsAddressValid((PVOID)p)) return 0;
    if (!MmIsAddressValid((PVOID)((const char*)p + len - 1))) return 0;
    return 1;
}

static void Xv_ZeroUrb(void* p, int n) { int i; for (i = 0; i < n; i++) ((char*)p)[i] = 0; }

/*---------------------------------------------------------------------------
    Class-driver registration  (interface class 0xFF / sub 0x00 / proto 0x00,
    matching the TinyUSB TUD_VENDOR_DESCRIPTOR our firmware emits)
---------------------------------------------------------------------------*/
DECLARE_XPP_TYPE(XViewType)
USB_DEVICE_TYPE_TABLE_BEGIN(XView)
USB_DEVICE_TYPE_TABLE_ENTRY(&XViewType_TABLE)
USB_DEVICE_TYPE_TABLE_END()
USB_CLASS_DRIVER_DECLARATION(XView, 0xFF, 0x00, 0x00)
#pragma data_seg(".XPP$ClassXView")
USB_CLASS_DECLARATION_POINTER(XView)
#pragma data_seg(".XPP$Data")
#pragma comment(linker, "/include:_XViewDescriptionPointer")

extern "C" VOID XViewInit(IUsbInit* UsbInit)
{
    USB_RESOURCE_REQUIREMENTS rr;
    Xv_Log("XViewInit: framework up");
    if (UsbInit == 0) return;
    rr.ConnectorType = USB_CONNECTOR_TYPE_HIGH_POWER;
    rr.MaxDevices = 1;
    rr.MaxCompositeInterfaces = 1;
    rr.MaxControlEndpoints = 1;
    rr.MaxBulkEndpoints = 2;       /* our OUT + IN */
    rr.MaxInterruptEndpoints = 0;
    rr.MaxControlTDperTransfer = 0;
    rr.MaxBulkTDperTransfer = 0;
    rr.MaxIsochEndpoints = 0;
    rr.MaxIsochMaxBuffers = 0;
    UsbInit->RegisterResources(&rr);
}
extern "C" VOID XViewAddDevice(IUsbDevice* Device)
{
    s_addCalls++;
    if (Device) {
        /* USBD enumerated + matched our vendor class -> a clean, OWNED node (it
           did SET_ADDRESS/SET_CONFIG for us). Stash it so the panel thread can
           adopt it with NO manual bring-up and NO leak. This is the proper
           hotplug-in path -- if USBD fires this post-boot, hotplug "just works". */
        s_pendDev = Device;
        InterlockedExchange(&s_pendAdd, 1);
        Device->AddComplete(USBD_STATUS_SUCCESS);
    }
}
extern "C" VOID XViewRemoveDevice(IUsbDevice* Device)
{
    /* USBD push-notification that a matching (vendor 0xFF) device left the bus.
       Latch it so the panel thread can tear down + rescan immediately, without
       waiting on bulk-transfer timeouts. */
    s_removeCalls++;
    InterlockedExchange(&s_linkLost, 1);
    if (Device) Device->RemoveComplete();
}

/*===========================================================================
    Lock-proof URB submit: NULL completion + poll Hdr.Status. Returns USBD
    status (0 ok), or 0x7FFFFFFF on submit-fail / never-completed.
===========================================================================*/
static ULONG Xv_SubmitPoll(IUsbDevice* dev, PURB urb)
{
    LONG st;
    int  spins;
    urb->Header.Status = (USBD_STATUS)XV_USBD_PENDING;
    urb->Header.CompleteProc = 0;
    urb->Header.CompleteContext = 0;
    st = dev->SubmitRequest(urb);
    s_lastRaw = st;
    if ((*(volatile ULONG*)&urb->Header.Status) != XV_USBD_PENDING)
        return (ULONG)urb->Header.Status;
    if ((ULONG)st != XV_USBD_PENDING)
        return (ULONG)st;
    for (spins = 0; spins < 2000; spins++) {
        if ((*(volatile ULONG*)&urb->Header.Status) != XV_USBD_PENDING) break;
        Sleep(1);
    }
    if ((*(volatile ULONG*)&urb->Header.Status) == XV_USBD_PENDING) return 0x7FFFFFFF;
    return (ULONG)urb->Header.Status;
}

/* generic control transfer */
static ULONG Xv_Control(IUsbDevice* dev, UCHAR bmReqType, UCHAR bReq,
    USHORT wValue, USHORT wIndex, void* buf, USHORT len, UCHAR dir)
{
    URB_CONTROL_TRANSFER urb;
    Xv_ZeroUrb(&urb, sizeof(urb));
    urb.Hdr.Length = (UCHAR)sizeof(URB_CONTROL_TRANSFER);
    urb.Hdr.Function = URB_FUNCTION_CONTROL_TRANSFER;
    urb.EndpointHandle = 0;
    urb.TransferBufferLength = len;
    urb.TransferBuffer = buf;
    urb.TransferDirection = dir;
    urb.ShortTransferOK = 1;
    urb.SetupPacket.bmRequestType = bmReqType;
    urb.SetupPacket.bRequest = bReq;
    urb.SetupPacket.wValue = wValue;
    urb.SetupPacket.wIndex = wIndex;
    urb.SetupPacket.wLength = len;
    return Xv_SubmitPoll(dev, (PURB)&urb);
}

/* real per-device VID/PID via GET_DESCRIPTOR(device,18). Returns 1 on success. */
static int Xv_GetVidPid(IUsbDevice* dev, int* vid, int* pid)
{
    unsigned char buf[18];
    ULONG st;
    int i;
    for (i = 0; i < 18; i++) buf[i] = 0;
    st = Xv_Control(dev, 0x80, 0x06 /*GET_DESCRIPTOR*/, 0x0100 /*device*/, 0,
        buf, 18, USB_TRANSFER_DIRECTION_IN);
    if (st != 0) return 0;
    *vid = (int)buf[8] | ((int)buf[9] << 8);
    *pid = (int)buf[10] | ((int)buf[11] << 8);
    return 1;
}

static int Xv_IsXViewId(int vid, int pid)
{
    return (vid == XV_USB_VID && pid == XV_USB_PID) ? 1 : 0;
}

/*===========================================================================
    Bulk endpoint open + transfer  (full URB union backs each so USBD has room
    to write its HCD area regardless of which arm we use)
===========================================================================*/
static ULONG Xv_OpenBulk(IUsbDevice* dev, int epAddr, void** outHandle, PULONG toggleStore)
{
    URB u;
    ULONG st;
    Xv_ZeroUrb(&u, sizeof(u));
    u.OpenEndpoint.Hdr.Length = (UCHAR)sizeof(URB_OPEN_ENDPOINT);
    u.OpenEndpoint.Hdr.Function = URB_FUNCTION_OPEN_ENDPOINT;
    u.OpenEndpoint.FunctionAddress = 0;
    u.OpenEndpoint.EndpointAddress = (UCHAR)epAddr;
    u.OpenEndpoint.EndpointType = USB_ENDPOINT_TYPE_BULK;     /* 0x02 */
    u.OpenEndpoint.Interval = 0;
    u.OpenEndpoint.DataToggleBits = toggleStore;
    u.OpenEndpoint.MaxPacketSize = XV_BULK_MAXPKT;
    u.OpenEndpoint.LowSpeed = 0;
    Xv_LogInt("OPEN_BULK submit ep", epAddr);
    st = Xv_SubmitPoll(dev, (PURB)&u);
    Xv_LogInt("OPEN_BULK ep st", (int)st);
    Xv_LogInt("  rawret", (int)s_lastRaw);
    if (st == 0) *outHandle = u.OpenEndpoint.EndpointHandle;
    return st;
}

static ULONG Xv_Bulk(IUsbDevice* dev, void* handle, void* buf, ULONG len, UCHAR dir)
{
    URB u;
    Xv_ZeroUrb(&u, sizeof(u));
    u.BulkOrInterruptTransfer.Hdr.Length = (UCHAR)sizeof(URB_BULK_OR_INTERRUPT_TRANSFER);
    u.BulkOrInterruptTransfer.Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    u.BulkOrInterruptTransfer.EndpointHandle = handle;
    u.BulkOrInterruptTransfer.TransferBufferLength = len;
    u.BulkOrInterruptTransfer.TransferBuffer = buf;
    u.BulkOrInterruptTransfer.TransferDirection = dir;
    u.BulkOrInterruptTransfer.ShortTransferOK = 1;
    u.BulkOrInterruptTransfer.InterruptDelay = 0;
    return Xv_SubmitPoll(dev, (PURB)&u);
}

/*===========================================================================
    Open both pipes on s_dev (+ DMA scratch). Used by both detection paths.
===========================================================================*/
static int Xv_OpenPipes(void)
{
    ULONG st;
    if (s_txbuf == 0) s_txbuf = (unsigned char*)MmAllocateContiguousMemory(XV_TXBUF_SZ);
    if (s_rxbuf == 0) s_rxbuf = (unsigned char*)MmAllocateContiguousMemory(XV_RXBUF_SZ);
    if (s_txbuf == 0 || s_rxbuf == 0) { Xv_Log("DMA scratch alloc FAILED"); return XV_XBOX_OPEN_FAILED; }
    Xv_Log("opening bulk pipes");
    st = Xv_OpenBulk(s_dev, XV_EP_OUT, &s_epOut, &s_toggleOut);
    if (st != 0 || s_epOut == 0) { Xv_Log("bulk OUT open failed"); return XV_XBOX_OPEN_FAILED; }
    st = Xv_OpenBulk(s_dev, XV_EP_IN, &s_epIn, &s_toggleIn);
    if (st != 0 || s_epIn == 0) { Xv_Log("bulk IN open failed"); return XV_XBOX_OPEN_FAILED; }
    s_ready = 1;
    Xv_Log("*** X-View bulk pipes OPEN ***");
    return XV_XBOX_OK;
}

/*===========================================================================
    Hub scan + manual bring-up  (verbatim port of xb_cam.cpp -- the proven path
    for a device USBD left "connected but not enabled" on a TI-hub port)
===========================================================================*/
/* Scan a real hub's ports; record one that is connected but NOT enabled. */
static void Xv_ScanHub(IUsbDevice* dev)
{
    unsigned char hd[16];
    unsigned char ps[4];
    int nports, p, i;
    ULONG st;
    for (i = 0; i < 16; i++) hd[i] = 0;
    st = Xv_Control(dev, 0xA0, 0x06 /*GET_DESCRIPTOR*/, 0x2900 /*hub*/, 0,
        hd, 9, USB_TRANSFER_DIRECTION_IN);
    Xv_LogInt("  HUB desc st", (int)st);
    if (st != 0) return;
    nports = (int)hd[2];
    Xv_LogInt("  HUB nPorts", nports);
    if (nports < 1 || nports > 8) return;
    s_hubNports = nports;
    for (p = 1; p <= nports; p++) {
        int status, change;
        for (i = 0; i < 4; i++) ps[i] = 0;
        st = Xv_Control(dev, 0xA3, 0x00 /*GET_STATUS*/, 0, (USHORT)p,
            ps, 4, USB_TRANSFER_DIRECTION_IN);
        if (st != 0) { Xv_LogInt("  port status FAIL @port", p); continue; }
        status = (int)ps[0] | ((int)ps[1] << 8);
        change = (int)ps[2] | ((int)ps[3] << 8);
        /* A device plugged in AFTER boot latches C_PORT_CONNECTION. XInitDevices
           acks this at boot (so the boot path never sees it), but on a live
           hotplug we must clear it ourselves and re-read, or the hub may keep
           reporting a stale (empty) connect bit. THIS is the hotplug fix. */
        if (change & 0x0001) {
            Xv_Log("    connection-change latched -> ack + re-read");
            Xv_Control(dev, 0x23, 0x01 /*CLEAR_FEATURE*/, XV_C_PORT_CONNECTION, (USHORT)p,
                0, 0, USB_TRANSFER_DIRECTION_OUT);
            Sleep(20);
            for (i = 0; i < 4; i++) ps[i] = 0;
            if (Xv_Control(dev, 0xA3, 0x00, 0, (USHORT)p, ps, 4, USB_TRANSFER_DIRECTION_IN) == 0)
                status = (int)ps[0] | ((int)ps[1] << 8);
        }
        /* Ensure the port is powered (no-op if already). An unpowered port can
           never see a hotplugged device. */
        if (!(status & XV_PORT_STAT_POWER)) {
            Xv_Log("    port unpowered -> SET_FEATURE PORT_POWER");
            Xv_Control(dev, 0x23, 0x03 /*SET_FEATURE*/, XV_PORT_POWER, (USHORT)p,
                0, 0, USB_TRANSFER_DIRECTION_OUT);
            Sleep(40);
            for (i = 0; i < 4; i++) ps[i] = 0;
            if (Xv_Control(dev, 0xA3, 0x00, 0, (USHORT)p, ps, 4, USB_TRANSFER_DIRECTION_IN) == 0)
                status = (int)ps[0] | ((int)ps[1] << 8);
        }
        Xv_LogInt("  port", p);
        Xv_LogInt("    status", status);
        Xv_LogInt("    change", change);
        Xv_LogInt("    connected", (status & 0x0001) ? 1 : 0);
        Xv_LogInt("    enabled", (status & 0x0002) ? 1 : 0);
        if (status & 0x0001) {                  /* device present on this port */
            if (status & 0x0002) s_portEnabled[p] = 1;   /* occupied (e.g. gamepad) */
            else {                              /* connected but NOT enabled   */
                s_hubPort = p; s_portCand[p] = 1;
                Xv_LogInt("    ^ candidate (not enabled)", p);
            }
        }
    }
}

/* Reset (and thereby enable) a downstream hub port. 0 = enabled. */
static int Xv_ResetPort(IUsbDevice* hub, int port)
{
    unsigned char ps[4];
    ULONG st;
    int i, tries, status;
    st = Xv_Control(hub, 0x23, 0x03 /*SET_FEATURE*/, XV_PORT_RESET, (USHORT)port,
        0, 0, USB_TRANSFER_DIRECTION_OUT);
    Xv_LogInt("  SET PORT_RESET st", (int)st);
    if (st != 0) return -1;
    status = 0;
    for (tries = 0; tries < 20; tries++) {
        Sleep(15);
        for (i = 0; i < 4; i++) ps[i] = 0;
        st = Xv_Control(hub, 0xA3, 0x00, 0, (USHORT)port, ps, 4, USB_TRANSFER_DIRECTION_IN);
        if (st != 0) continue;
        status = (int)ps[0] | ((int)ps[1] << 8);
        if (status & 0x0002) break;             /* enabled */
    }
    Xv_Control(hub, 0x23, 0x01 /*CLEAR_FEATURE*/, XV_C_PORT_RESET, (USHORT)port,
        0, 0, USB_TRANSFER_DIRECTION_OUT);
    if (status & 0x0002) { Xv_Log("  port enabled (device live at addr 0)"); return 0; }
    Xv_Log("  port did not enable");
    return -2;
}

/* Build a node we OWN for the device on (hub, port). Device stays at addr 0. */
static IUsbDevice* Xv_BuildNode(IUsbDevice* hub, int port)
{
    IUsbDevice* node;
    unsigned char* nb;
    (void)hub;
    node = g_DeviceTree.AllocDevice();
    if (node == 0) { Xv_Log("AllocDevice FAILED"); return 0; }
    if (!Xv_Readable(node, XV_NODE_SIZE)) { Xv_Log("AllocDevice bad ptr"); return 0; }
    nb = (unsigned char*)(void*)node;
    nb[0] = 0xFE;                 /* state: connected (fresh-open path) */
    nb[4] = (unsigned char)port;  /* hub port (bit7=0 -> full speed)    */
    nb[5] = 0;                    /* USB address 0                      */
    nb[6] = 8;                    /* EP0 max packet (safe default)      */
    Xv_Log("Xv_BuildNode: node allocated");
    return node;
}

/* Open / close EP0 (func 0x82 -> OpenDefaultEndpoint, 0xC3 -> close). */
static ULONG Xv_OpenDefaultEP(IUsbDevice* node)
{
    URB u;
    Xv_ZeroUrb(&u, sizeof(u));
    u.Header.Length = (UCHAR)sizeof(URB);
    u.Header.Function = XV_URB_OPEN_DEFAULT_EP;
    return Xv_SubmitPoll(node, (PURB)&u);
}
static ULONG Xv_CloseDefaultEP(IUsbDevice* node)
{
    URB u;
    Xv_ZeroUrb(&u, sizeof(u));
    u.Header.Length = (UCHAR)sizeof(URB);
    u.Header.Function = XV_URB_CLOSE_DEFAULT_EP;
    return Xv_SubmitPoll(node, (PURB)&u);
}

/* Full manual bring-up on our own node, then open the bulk pipes. 0=ok. */
static int Xv_BringUpManual(IUsbDevice* hub, int port)
{
    IUsbDevice* node;
    int vid = -1, pid = -1;
    ULONG st;

    /* LEAK GUARD: a failed manual bring-up leaks the AllocDevice node + USB
       address (no free API here). Stop attempting once the budget is spent so
       repeated post-boot retries can never exhaust the pool. */
    if (s_manualTries >= XV_MANUAL_CAP) {
        Xv_Log("manual bring-up budget spent -- skipping (avoids USB resource leak)");
        return XV_XBOX_OPEN_FAILED;
    }
    s_manualTries++;

    node = Xv_BuildNode(hub, port);
    if (node == 0) return XV_XBOX_OPEN_FAILED;

    st = Xv_OpenDefaultEP(node);
    Xv_LogInt("OPEN_DEFAULT_EP st", (int)st);
    Xv_LogInt("  rawret", (int)s_lastRaw);
    if ((LONG)st < 0) { Xv_Log("EP0 open failed"); return XV_XBOX_OPEN_FAILED; }

    if (!Xv_GetVidPid(node, &vid, &pid)) { Xv_Log("manual GET_DESCRIPTOR returned err (continuing)"); }
    Xv_LogInt("manual VID", vid);
    Xv_LogInt("manual PID", pid);
    /* per directive: do NOT gate on VID/PID here. A device USBD left
       unenumerated may not return clean IDs before it's configured, so we
       bring it up regardless and let the handshake confirm identity. */
    Xv_Log("*** node OWNED @addr0 (PID gate ignored) ***");
    s_dev = node; s_present = 1; s_port = port; s_vid = vid; s_pid = pid;

    /* device is in Default state (addr 0) -> SET_ADDRESS before configuring */
    {
        unsigned char* nb = (unsigned char*)(void*)node;
        void* hc = *(void**)(nb + 0x0C);
        unsigned char addr = USBD_AllocateUsbAddress((struct _USBD_HOST_CONTROLLER*)hc);
        Xv_LogInt("alloc addr", (int)addr);
        st = Xv_Control(node, 0x00, 0x05 /*SET_ADDRESS*/, (USHORT)addr, 0, 0, 0, USB_TRANSFER_DIRECTION_OUT);
        Xv_LogInt("SET_ADDRESS st", (int)st);
        if (st != 0) { Xv_Log("SET_ADDRESS failed"); return XV_XBOX_OPEN_FAILED; }
        Sleep(5);                 /* address-recovery */
        nb[5] = addr;             /* node now targets the new address */
        Xv_CloseDefaultEP(node);  /* re-open EP0 at the new address */
        st = Xv_OpenDefaultEP(node);
        Xv_LogInt("EP0 reopen st", (int)st);
        if ((LONG)st < 0) { Xv_Log("EP0 reopen failed"); return XV_XBOX_OPEN_FAILED; }
    }

    st = Xv_Control(node, 0x00, 0x09 /*SET_CONFIGURATION*/, 1, 0, 0, 0, USB_TRANSFER_DIRECTION_OUT);
    Xv_LogInt("SET_CONFIG st", (int)st);

    /* tail differs from camera: open bulk pipes instead of an iso stream */
    return Xv_OpenPipes();
}

/*===========================================================================
    Active g_DeviceTree walk (detection)  (mirrors xb_cam.cpp)
===========================================================================*/
static char* Xv_GetNodeBase(void)
{
    char* t = (char*)(void*)&g_DeviceTree;
    if (!Xv_Readable(t + XV_TREE_BASE_OFF, 4)) return 0;
    return *(char**)(t + XV_TREE_BASE_OFF);
}
static IUsbDevice* Xv_NodeAt(char* base, int idx)
{
    char* n;
    if (idx < 0 || idx >= XV_MAX_NODES) return 0;
    n = base + idx * XV_NODE_SIZE;
    if (!Xv_Readable(n, XV_NODE_SIZE)) return 0;
    return (IUsbDevice*)(void*)n;
}
static int Xv_NodeIndex(char* base, IUsbDevice* dev)
{
    char* p; int off;
    if (dev == 0) return -1;
    p = (char*)(void*)dev;
    if (p < base) return -1;
    off = (int)(p - base);
    if (off % XV_NODE_SIZE) return -1;
    off /= XV_NODE_SIZE;
    if (off < 0 || off >= XV_MAX_NODES) return -1;
    return off;
}

static void Xv_InspectNode(IUsbDevice* dev, int idx)
{
    int vid = -1, pid = -1;
    int cls;
    const USB_INTERFACE_DESCRIPTOR* id;
    ULONG port;

    Xv_LogInt("inspect node", idx);

    /* root hubs (parent==0x80) are VIRTUAL: ptr is uninitialized and
       SubmitRequest faults. Only talk to real downstream devices. */
    {
        unsigned char parent = ((const unsigned char*)(const void*)dev)[1];
        if (parent == XV_IDX_NONE) { Xv_Log("  root hub - skip"); return; }
    }

    Xv_Log("  GetPort");
    port = dev->GetPort();
    (void)port;
    Xv_Log("  GetVidPid");
    if (Xv_GetVidPid(dev, &vid, &pid)) {
        Xv_LogInt("  VID", vid);
        Xv_LogInt("  PID", pid);
    }

    /* X-View already enumerated as a node? claim it. */
    if (Xv_IsXViewId(vid, pid)) {
        Xv_Log("*** X-View device found (VID/PID match) ***");
        s_dev = dev; s_port = (int)port; s_vid = vid; s_pid = pid; s_present = 1;
        return;
    }

    /* TI internal hub -> scan its ports for the unenumerated X-View */
    if (vid == XV_TI_HUB_VID && pid == XV_TI_HUB_PID) {
        Xv_Log("  TI hub - scanning downstream ports");
        s_hubDev = dev;
        Xv_ScanHub(dev);
    }

    /* SHARED-GLOBAL presence (the proven camera signal): GetInterfaceDescriptor
       returns the LAST-ENUMERATED device's interface, not this node's. If our
       vendor class (0xFF) shows up there, the X-View enumerated -- even with no
       retained node and even if the live hub-port bit looks empty. */
    cls = -1;
    id = dev->GetInterfaceDescriptor();
    if (Xv_Readable((const void*)id, 9) && id->bLength >= 9 && id->bLength < 64)
        cls = (int)id->bInterfaceClass;
    Xv_LogInt("  shared-iface class", cls);
    if (cls == 0xFF) {
        Xv_Log("  *** vendor 0xFF in shared global -> X-View IS on the bus ***");
        s_present = 1;
    }
}

static void Xv_WalkTree(void)
{
    char* base;
    IUsbDevice* stack[XV_MAX_NODES];
    unsigned char visited[XV_MAX_NODES];
    int sp, count, i;

    base = Xv_GetNodeBase();
    if (base == 0) { Xv_Log("walk: base unreadable"); return; }
    Xv_Log("walk: base ok");
    for (i = 0; i < XV_MAX_NODES; i++) visited[i] = 0;
    sp = 0; count = 0;

    for (i = 0; i < XV_MAX_NODES; i++) {
        IUsbDevice* ni = Xv_NodeAt(base, i); unsigned char* b;
        if (ni == 0) continue;
        b = (unsigned char*)(void*)ni;
        if (b[1] == XV_IDX_NONE && b[2] != XV_IDX_NONE && Xv_NodeAt(base, b[2]) != 0) {
            if (sp < XV_MAX_NODES) stack[sp++] = ni;
        }
    }
    { IUsbDevice* n0 = Xv_NodeAt(base, 0); if (n0 && sp < XV_MAX_NODES) stack[sp++] = n0; }

    while (sp > 0 && count < XV_MAX_NODES) {
        IUsbDevice* dev = stack[--sp];
        int idx = Xv_NodeIndex(base, dev);
        unsigned char* b; int ci, guard;
        if (idx < 0 || visited[idx]) continue;
        visited[idx] = 1; count++;
        Xv_InspectNode(dev, idx);
        if (s_dev) return;         /* claimable node handle in hand -- stop */
        b = (unsigned char*)(void*)dev; ci = (int)b[2]; guard = 0;
        while (ci != XV_IDX_NONE && guard < XV_MAX_NODES) {
            IUsbDevice* c = Xv_NodeAt(base, ci); unsigned char* cb; int cidx;
            if (c == 0) break;
            cb = (unsigned char*)(void*)c; cidx = Xv_NodeIndex(base, c);
            if (cidx >= 0 && !visited[cidx] && sp < XV_MAX_NODES) stack[sp++] = c;
            ci = (int)cb[3]; guard++;
        }
    }
    Xv_LogInt("nodes visited", count);
}

/*===========================================================================
    Public API
===========================================================================*/
extern "C" void XvXbox_SetLog(XvLogFn fn) { s_logfn = fn; }

extern "C" int XvXbox_IsConnected(void) { return s_present; }
extern "C" int XvXbox_IsReady(void) { return s_ready; }

/* Authoritative "is the panel still on the bus?" check, callable any time after
   bring-up (unlike the descriptor getters, which are enumeration-time only).
   1 = still connected, 0 = gone. Two signals, cheapest first:
     (a) XViewRemoveDevice latched a removal (USBD push), or
     (b) the hub's live PORT_CONNECTION bit for our port has dropped.
   A transient hub-read failure is treated as "still here" so we never
   false-trip mid-blit. */
extern "C" int XvXbox_StillConnected(void)
{
    /* Disconnect is reported ONLY by the USBD RemoveDevice callback (s_linkLost).
       The earlier hub-port GET_STATUS poll was dropped: s_port means different
       things on the enumerated vs manual path, so it false-tripped on the boot
       panel -> tore down a working display that then could not be rebuilt. If
       RemoveDevice does not fire on this hardware, the panel simply holds its
       last frame on unplug (stable) and the blit-failure fallback in RunPages
       still catches a truly dead link. */
    if (!s_present) return 0;
    if (s_linkLost) return 0;
    return 1;
}

extern "C" int XvXbox_GetDevInfo(int* vid, int* pid, int* port)
{
    if (!s_present) return -1;
    if (vid)  *vid = s_vid;
    if (pid)  *pid = s_pid;
    if (port) *port = s_port;
    return 0;
}

extern "C" int XvXbox_Init(void)
{
    int p;
    Xv_Log("XvXbox_Init: detect + open bulk pipes");
    s_dev = 0; s_present = 0; s_ready = 0;
    InterlockedExchange(&s_linkLost, 0);
    s_port = -1; s_vid = -1; s_pid = -1;
    s_hubDev = 0; s_hubPort = -1; s_hubNports = 0;
    for (p = 0; p < 9; p++) { s_portEnabled[p] = 0; s_portCand[p] = 0; }
    s_epOut = 0; s_epIn = 0; s_toggleOut = 0; s_toggleIn = 0;

    /* CLEAN hotplug path first: if USBD enumerated the device (XViewAddDevice
       fired -- at boot or on a live plug-in), adopt that node directly. No
       manual SET_ADDRESS, no AllocDevice leak. Guard with IsHardwareConnected()
       so we never adopt a node for a device that already left. */
    if (InterlockedExchange(&s_pendAdd, 0) && s_pendDev) {
        if (s_pendDev->IsHardwareConnected()) {
            int vid = -1, pid = -1;
            Xv_Log("adopting USBD-added node (clean hotplug-in, no leak)");
            s_dev = s_pendDev; s_present = 1;
            s_port = (int)s_pendDev->GetHubPort();
            if (Xv_GetVidPid(s_dev, &vid, &pid)) { s_vid = vid; s_pid = pid; }
            return Xv_OpenPipes();
        }
        Xv_Log("pending Add was for a device already gone -- ignoring");
        s_pendDev = 0;
    }

    Xv_WalkTree();

    /* path 1: X-View already enumerated as a claimable node (rare on RXDK) */
    if (s_dev) {
        Xv_LogInt("device FOUND (node), port", s_port);
        return Xv_OpenPipes();
    }

    if (s_present)
        Xv_Log("vendor 0xFF in shared global -- X-View enumerated, bringing up");
    else
        Xv_Log("no 0xFF yet -- active reset-probe (FW soft-connect may be unlatched)");

    /* path 2: ports ScanHub saw connected-but-not-enabled (the live hotplug
       signal) -> reset + manual bring-up. One retry with a settle gives a
       just-powered device (RP2040 still booting its firmware) a second chance. */
    if (s_hubDev && s_hubNports > 0) {
        for (p = 1; p <= s_hubNports; p++) {
            int tries;
            if (!s_portCand[p]) continue;
            Xv_LogInt("candidate (live) hub port", p);
            for (tries = 0; tries < 2; tries++) {
                if (tries) Sleep(150);            /* settle a freshly-powered device */
                if (Xv_ResetPort(s_hubDev, p) == 0) {
                    int rc = Xv_BringUpManual(s_hubDev, p);
                    if (rc == XV_XBOX_OK) return rc;
                }
            }
        }
    }

    /* path 3: actively reset each non-occupied port to force the hub to detect
       and enable a device whose connection the passive XInitDevices pass did not
       latch. The FW now soft-connects regardless of VBUS, so the connection may
       be asserted but unenumerated -- a port reset re-runs detection/enable. Skip
       enabled ports (the gamepad). PID-agnostic bring-up. */
    if (s_hubDev && s_hubNports > 0) {
        for (p = 1; p <= s_hubNports; p++) {
            if (s_portEnabled[p]) continue;     /* skip the gamepad etc. */
            if (p == s_hubPort) continue;       /* already tried above    */
            Xv_LogInt("reset-probe port", p);
            if (Xv_ResetPort(s_hubDev, p) == 0) {
                int rc = Xv_BringUpManual(s_hubDev, p);
                if (rc == XV_XBOX_OK) return rc;
            }
        }
    }

    if (s_present) { Xv_Log("on the bus but could not bring up"); return XV_XBOX_OPEN_FAILED; }
    Xv_Log("no X-View device on the bus");
    return XV_XBOX_NO_DEVICE;
}

extern "C" int XvXbox_SendBulk(const void* data, int len)
{
    ULONG st;
    int i;
    if (!s_ready || s_epOut == 0) return -1;
    if (len < 0 || len > XV_TXBUF_SZ) return -1;
    for (i = 0; i < len; i++) s_txbuf[i] = ((const unsigned char*)data)[i];
    st = Xv_Bulk(s_dev, s_epOut, s_txbuf, (ULONG)len, USB_TRANSFER_DIRECTION_OUT);
    if (st != 0) { Xv_LogInt("bulk OUT st", (int)st); return -1; }
    return len;
}

/* Largest single OUT transfer (incl. all headers) the transport can push.
   The client tiles pixel blits into bands that fit under this. */
extern "C" int XvXbox_MaxOut(void) { return XV_TXBUF_SZ; }

/* Gather-send: command prefix `a` (header + per-command struct) immediately
   followed by body `b` (pixel band), assembled once into the DMA scratch so a
   blit costs a single copy + a single URB. */
extern "C" int XvXbox_SendBulk2(const void* a, int alen, const void* b, int blen)
{
    ULONG st;
    int i, total;
    if (!s_ready || s_epOut == 0) return -1;
    if (alen < 0 || blen < 0) return -1;
    total = alen + blen;
    if (total > XV_TXBUF_SZ) return -1;
    for (i = 0; i < alen; i++) s_txbuf[i] = ((const unsigned char*)a)[i];
    for (i = 0; i < blen; i++) s_txbuf[alen + i] = ((const unsigned char*)b)[i];
    st = Xv_Bulk(s_dev, s_epOut, s_txbuf, (ULONG)total, USB_TRANSFER_DIRECTION_OUT);
    if (st != 0) { Xv_LogInt("bulk OUT2 st", (int)st); return -1; }
    return total;
}

extern "C" int XvXbox_RecvBulk(void* data, int maxlen)
{
    ULONG st;
    int n, i;
    if (!s_ready || s_epIn == 0) return -1;
    n = (maxlen > XV_RXBUF_SZ) ? XV_RXBUF_SZ : maxlen;
    if (n < 0) return -1;
    for (i = 0; i < n; i++) s_rxbuf[i] = 0;
    st = Xv_Bulk(s_dev, s_epIn, s_rxbuf, (ULONG)n, USB_TRANSFER_DIRECTION_IN);
    if (st != 0) { Xv_LogInt("bulk IN st", (int)st); return -1; }
    for (i = 0; i < n; i++) ((unsigned char*)data)[i] = s_rxbuf[i];
    return n;
}

extern "C" void XvXbox_Shutdown(void)
{
    if (s_txbuf) { MmFreeContiguousMemory(s_txbuf); s_txbuf = 0; }
    if (s_rxbuf) { MmFreeContiguousMemory(s_rxbuf); s_rxbuf = 0; }
    s_ready = 0; s_epOut = 0; s_epIn = 0;
    Xv_Log("XvXbox_Shutdown");
}