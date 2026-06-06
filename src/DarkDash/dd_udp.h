/*---------------------------------------------------------------------------
    dd_udp.h -- shared UDP transport for DarkoneCustoms accessories.

    Most DarkoneCustoms mods (Type-D, XBOX-RGB, OXFP) talk over UDP, so the
    socket plumbing lives here once and each device module is just a thin packet
    encoder on top. This first cut provides limited-broadcast send (to
    255.255.255.255 on a given port), which is all Type-D needs -- it listens
    for telemetry and renders the latest. A listen/registry side for device
    discovery (RGB / OXFP presence) can be layered on later.

    Usage:
        Udp_Init()                         once at boot
        Udp_Broadcast(port, data, len)     fire-and-forget datagram to the LAN
        Udp_Shutdown()                     optional, on clean exit
---------------------------------------------------------------------------*/
#ifndef DD_UDP_H
#define DD_UDP_H

#ifdef __cplusplus
extern "C" {
#endif

    void Udp_Init(void);
    void Udp_Shutdown(void);

    /* Send one datagram to 255.255.255.255:port. Returns 1 if the bytes went out,
       0 on any failure (no socket, send error). Non-blocking; never stalls. */
    int  Udp_Broadcast(int port, const void* data, int len);

    /* ---- device discovery --------------------------------------------------- */
    /* Two detection models are handled here:
         - PASSIVE: the device self-advertises (XBOX-RGB broadcasts on 7777). We
           bind a listen socket on its port and watch for a signature substring.
         - ACTIVE:  the device only answers when asked (OXFP replies to a ping on
           32123). We periodically send a probe and watch replies for a signature.
       Either way, a device is "present" if its signature was seen within a
       staleness window. Menus grey out when absent so we never fire control
       packets at a device that isn't there. */
    enum { UDP_DEV_RGB = 0, UDP_DEV_OXFP = 1, UDP_DEV_COUNT = 2 };

    void Udp_DiscoTick(void);          /* pump discovery every frame (background) */
    int  Udp_Present(int dev);         /* 1 if the device was seen recently        */

    /* Send a unicast datagram to a specific device's last-known IP:port (for
       control packets). Returns 1 on success. If we have no address for it yet
       (never seen), falls back to broadcasting on the device's port. */
    int  Udp_SendToDevice(int dev, const void* data, int len);

#ifdef __cplusplus
}
#endif
#endif /* DD_UDP_H */