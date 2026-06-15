/*
 * xv_xbox.h -- X-View USB display class driver for the original Xbox.
 * Team Resurgent / Darkone83
 *
 * This is the Xbox-side counterpart to the PC reference host's device.py: it
 * finds the X-View vendor device (VID/PID 0xCAFE/0x4001) on the USB bus, opens
 * its bulk OUT (0x01) and bulk IN (0x81) endpoints, and exposes synchronous
 * raw transfer entry points. The protocol/client layer (xv_client.*) builds the
 * X-View packets on top of these, mirroring the Python client byte-for-byte.
 *
 * Architecture mirrors the proven xb_cam.cpp camera driver:
 *   - registered as a USB class driver (interface class 0xFF/0x00/0x00) so USBD
 *     accepts + configures the device and calls XViewAddDevice on attach
 *   - the live IUsbDevice* is obtained by actively walking g_DeviceTree and
 *     matching the real VID/PID (AddDevice's pointer is transient)
 *   - transfers use the lock-proof submit (NULL completion + poll Hdr.Status),
 *     NOT a KEVENT wait -- this is the path xcam proved doesn't soft-lock
 *   - DMA buffers are MmAllocateContiguousMemory'd and MmIsAddressValid-gated
 *
 * Build: include after <xtl.h>; depends on the reconstructed "xbox_usb.h" (the
 * XDK USB framework) and the shared "xv_protocol.h". No dbg.h dependency -- all
 * logging is routed through the XvLogFn callback the harness registers.
 */
#ifndef XV_XBOX_H
#define XV_XBOX_H

#include <xtl.h>   /* base types */

#ifdef __cplusplus
extern "C" {
#endif

	/* placeholder dev IDs -- MUST match src/usb_descriptors.c on the firmware */
#define XV_USB_VID            0xCAFE
#define XV_USB_PID            0x4001

/* status codes from Xv_Init(). 0 = success; negatives = failure. */
#define XV_XBOX_OK             0
#define XV_XBOX_NO_DEVICE     (-2)   /* no X-View attached on the bus          */
#define XV_XBOX_OPEN_FAILED   (-3)   /* attached but endpoint bring-up failed  */

/* Optional log sink the harness registers (same shape as xcam's CamLogFn, so
   Xv_SetLog(Dbg_GetSink()) works if you have dbg.h). */
	typedef void (*XvLogFn)(const char* tag, const char* msg);
	void XvXbox_SetLog(XvLogFn fn);

	/* Detect the X-View, open its bulk endpoints. Call once after XInitDevices has
	   had a chance to enumerate (i.e. after the device is plugged + a brief wait).
	   Returns XV_XBOX_OK / XV_XBOX_NO_DEVICE / XV_XBOX_OPEN_FAILED. */
	int  XvXbox_Init(void);

	/* Release endpoints + DMA scratch buffers. */
	void XvXbox_Shutdown(void);

	/* 1 if a device matching VID/PID was found in the tree (even if open failed). */
	int  XvXbox_IsConnected(void);

	/* 1 once both bulk pipes are open and ready for transfers. */
	int  XvXbox_IsReady(void);

	/* still on the bus? 1=yes 0=unplugged. Safe to poll after bring-up. */
	int  XvXbox_StillConnected(void);

	/* Found device identity (valid after a successful or partial Init). 0=ok,-1=no dev. */
	int  XvXbox_GetDevInfo(int* vid, int* pid, int* port);

	/* --- raw synchronous bulk transfers (the seam the client layer uses) --- */
	/* Send `len` bytes out the bulk OUT pipe. Returns bytes sent (==len) or <0. */
	int  XvXbox_SendBulk(const void* data, int len);
	/* Largest single OUT transfer (incl headers) the transport accepts. */
	int  XvXbox_MaxOut(void);
	/* Gather-send: prefix `a` (alen) then body `b` (blen) in one URB. Returns total or <0. */
	int  XvXbox_SendBulk2(const void* a, int alen, const void* b, int blen);
	/* Read up to `maxlen` bytes from the bulk IN pipe into `data`. The buffer is
	   zeroed first, so a short reply leaves trailing zeros; the caller parses the
	   X-View header's length field for the true payload size. Returns the number
	   of bytes requested on success, or <0 on transfer error/timeout. */
	int  XvXbox_RecvBulk(void* data, int maxlen);

#ifdef __cplusplus
}
#endif

#endif /* XV_XBOX_H */