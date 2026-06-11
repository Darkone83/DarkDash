#ifndef DD_TYPEDART_H
#define DD_TYPEDART_H
/*---------------------------------------------------------------------------
    dd_typedart.h -- push cover art to discovered Type-D / Type-D XL displays.

    The dash letterbox-fits a title's art to each present panel's native size --
    480x480 for the XL (id 5), 240x240 for the regular Type-D units (ids 1-4) --
    packs it as RGB565 (little-endian, host order), and streams it over a raw TCP
    socket to the device's image port (50580). The device holds it as a "Now
    Playing" background with a live telemetry strip until told to resume.

    Multi-unit: a launch pushes to every discovered, art-enabled unit, largest
    payload first (480 then 240) so the heavy XL frame gets the earliest, freshest
    send window before the launch tears down networking.

    Transport mirrors the Winsock idiom in dd_update.cpp. Device IPs come from the
    shared discovery layer (dd_udp), latched from each unit's UDP 50502 beacon --
    so a push only works once a device has been seen. See the function
    declarations below for the full API.
---------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C" {
#endif

    /* Panel geometry the device expects. */
#define TYPEDART_W   480
#define TYPEDART_H   480

    /* Send a w*h RGB565 (little-endian) frame to the discovered Type-D. Returns
       1 on success, 0 if no device is known yet or the transfer failed. */
    int TypeDArt_SendFrame(const unsigned short* px, int w, int h);

    /* Build a 480x480 colour-bars test frame and send it. For bring-up. */
    int TypeDArt_SendTestPattern(void);

    /* Load the title's resource-pack cover art (opencase.png -> poster.jpg),
       letterbox-fit it to each present panel's native size (480 XL / 240 Type-D)
       as RGB565, and push it. Returns 1 on success, 0 if the title has no pack
       art, no device is known, or the send failed.
       (Pack-less titles -- XBE title image -- are a separate fallback.) */
    int TypeDArt_SendArtFor(const char* xbePath);

    /* Ask the device to drop the held art and resume its slideshow. */
    int TypeDArt_Resume(void);

    /* 1 if a Type-D has advertised itself recently (its IP is known). */
    int TypeDArt_Present(void);

    /* 1 if at least one present unit has its art toggle enabled (XL Art for the
       XL, Type-D Art for ids 1-4) -- i.e. a launch push would actually send. */
    int TypeDArt_WillSend(void);

    /* Call every frame. Once per boot, when the device is first discovered,
       sends Resume so it drops any art held from the previous session. */
    void TypeDArt_BootResumeTick(void);

#ifdef __cplusplus
}
#endif
#endif /* DD_TYPEDART_H */