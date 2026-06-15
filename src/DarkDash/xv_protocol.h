#ifndef XV_PROTOCOL_H
#define XV_PROTOCOL_H
/*
 * X-View display protocol  --  shared source of truth (v1 draft).
 *
 * Compiles UNCHANGED in three places:
 *   1. RP2040 firmware            (this project)
 *   2. PC Python reference host   (mirrored / code-generated from this file)
 *   3. Xbox driver library        (C, MSVC2003 / RXDK)
 *
 * Whatever the Python reference host puts on the wire, the Xbox driver must
 * reproduce byte-for-byte. This header is what stops the two drifting.
 *
 * WIRE MODEL
 *   Every transfer = xv_packet_header_t (8 bytes) followed by `length` payload
 *   bytes. All multi-byte fields are LITTLE-ENDIAN (free on RP2040/x86/Xbox).
 *
 *   Small commands: payload is fully buffered then acted on.
 *   Pixel-carrying commands (BLIT_RECT, DEFINE_SPRITE): a small fixed header,
 *   then a pixel stream the firmware writes straight into the framebuffer /
 *   sprite cache as it arrives -- do NOT assume the firmware buffers the whole
 *   payload. Keep the per-command fixed headers exactly as defined here.
 *
 * ACK MODEL
 *   Draw/config commands are fire-and-forget (no reply).
 *   PING -> PONG, QUERY_INFO -> INFO, STATUS_QUERY -> STATUS are the only
 *   request/response pairs. The device may also emit ERROR asynchronously on
 *   the IN endpoint, carrying the seq of the offending command.
 *
 * STRUCT LAYOUT
 *   Every struct below is hand-ordered for natural alignment with NO padding
 *   (explicit _pad fields where needed), so packed/unpacked layouts agree
 *   across GCC / MSVC2003 without packing pragmas.
 */

 /* MSVC2003 (RXDK toolchain) predates <stdint.h>. Provide a tiny shim there. */
#if defined(_MSC_VER) && (_MSC_VER < 1600)
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
#else
#include <stdint.h>
#endif

#define XV_PROTO_MAGIC    0x5856u   /* 'X','V' little-endian            */
#define XV_PROTO_VERSION  0x0001u   /* bump on ANY wire-format change   */

/* RGB565 pack helper -- host and device MUST agree on this. */
#define XV_RGB565(r, g, b) \
    ( (uint16_t)( (((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | (((b) & 0xF8u) >> 3) ) )

/* ------------------------------------------------------------------ header */
typedef struct {
    uint16_t magic;     /* XV_PROTO_MAGIC                              */
    uint8_t  opcode;    /* xv_opcode_t                                 */
    uint8_t  flags;     /* XV_HF_* bitfield                            */
    uint16_t length;    /* payload byte count following this header    */
    uint16_t seq;       /* host-incrementing sequence; echoed in acks  */
} xv_packet_header_t;    /* 8 bytes */

/* header flags */
#define XV_HF_NONE       0x00u
#define XV_HF_WANT_ACK   0x01u   /* request an ack even for a draw op (debug) */

/* BLIT_RECT upscale: the source is nearest-neighbor expanded by N on both axes
   into the framebuffer (encoded in the high nibble of flags). 0 or 1 = native.
   Lets a low-res source fill a large region for a fraction of the wire bytes. */
#define XV_HF_SCALE_SHIFT   4
#define XV_HF_MAKE_SCALE(n) ((uint8_t)(((n) & 0x0Fu) << XV_HF_SCALE_SHIFT))
#define XV_HF_GET_SCALE(f)  ((uint8_t)(((f) >> XV_HF_SCALE_SHIFT) & 0x0Fu))

   /* ----------------------------------------------------------------- opcodes */
typedef enum {
    /* 0x0x  control / handshake */
    XV_OP_PING = 0x00,  /* host->dev, no payload  -> PONG            */
    XV_OP_PONG = 0x01,  /* dev->host, no payload                     */
    XV_OP_QUERY_INFO = 0x02,  /* host->dev, no payload  -> INFO            */
    XV_OP_INFO = 0x03,  /* dev->host, payload = xv_info_t            */
    XV_OP_STATUS_QUERY = 0x04,  /* host->dev, no payload  -> STATUS          */
    XV_OP_STATUS = 0x05,  /* dev->host, payload = xv_status_t          */
    XV_OP_ERROR = 0x06,  /* dev->host (async), payload = xv_error_t   */
    XV_OP_RESET = 0x07,  /* host->dev, clear fb + glyph + sprite cache*/

    /* 0x1x  display config */
    XV_OP_SET_BRIGHTNESS = 0x10, /* payload = xv_brightness_t                */
    XV_OP_POWER = 0x11, /* payload = xv_power_t                     */
    XV_OP_SET_PRESENT_MODE = 0x12, /* payload = xv_present_mode_t              */
    XV_OP_SET_ORIENTATION = 0x13, /* payload = xv_orientation_t (optional)    */
    XV_OP_SET_PANEL = 0x14, /* payload = xv_set_panel_t (runtime preset) */

    /* 0x2x  graphics layer */
    XV_OP_FILL_RECT = 0x20,  /* payload = xv_fill_rect_t                  */
    XV_OP_BLIT_RECT = 0x21,  /* xv_blit_rect_t + w*h RGB565 (streamed)    */
    XV_OP_CLEAR = 0x22,  /* payload = uint16 color (whole framebuffer)*/
    XV_OP_PRESENT = 0x23,  /* xv_rect_t (dirty); length 0 = full flush  */

    /* 0x3x  sprite cache */
    XV_OP_DEFINE_SPRITE = 0x30,  /* xv_define_sprite_t + pixels (streamed)    */
    XV_OP_DRAW_SPRITE = 0x31,  /* payload = xv_draw_sprite_t                */
    XV_OP_FREE_SPRITE = 0x32,  /* payload = uint16 id (0xFFFF = free all)   */

    /* 0x4x  text / glyph layer */
    XV_OP_TEXT_PUT = 0x40,  /* xv_text_put_t + `count` char codes        */
    XV_OP_TEXT_CLEAR = 0x41,  /* xv_text_clear_t (region) or length 0 = all*/
    XV_OP_DEFINE_GLYPH = 0x42,  /* xv_define_glyph_t + bitmap (CGRAM analog) */

    XV_OP__COUNT
} xv_opcode_t;

/* ------------------------------------------------------------ enums / caps */
/* color_format */
#define XV_FMT_RGB565        0x00u

/* caps bitfield (xv_info_t.caps) */
#define XV_CAP_DOUBLE_BUFFER (1u << 0)
#define XV_CAP_GLYPH_CACHE   (1u << 1)
#define XV_CAP_SPRITE_CACHE  (1u << 2)

/* orientation */
#define XV_ROT_0             0x00u
#define XV_ROT_90            0x01u
#define XV_ROT_180           0x02u
#define XV_ROT_270           0x03u

/* power modes */
#define XV_PWR_OFF           0x00u   /* display off                          */
#define XV_PWR_ON            0x01u   /* display on                           */
#define XV_PWR_SLEEP         0x02u   /* panel sleep, backlight off           */

/* present modes */
#define XV_PRESENT_MANUAL    0x00u   /* draws batch; PRESENT flushes (default)*/
#define XV_PRESENT_AUTO      0x01u   /* every draw flushes immediately        */

/* panel preset (xv_set_panel_t.panel) -- re-query INFO after switching */
#define XV_PANEL_A           0x00u   /* 284x76 bar strip                     */
#define XV_PANEL_B           0x01u   /* 320x240 panel (firmware boot default)*/

/* error codes (xv_error_t.code) */
#define XV_ERR_NONE          0x00u
#define XV_ERR_BAD_MAGIC     0x01u
#define XV_ERR_BAD_OPCODE    0x02u
#define XV_ERR_BAD_LENGTH    0x03u
#define XV_ERR_OUT_OF_BOUNDS 0x04u   /* rect/coords outside the panel        */
#define XV_ERR_NO_SPRITE     0x05u   /* DRAW_SPRITE on undefined id          */
#define XV_ERR_CACHE_FULL    0x06u   /* sprite/glyph cache exhausted         */

/* --------------------------------------------------------- device -> host */
typedef struct {
    uint16_t proto_version;     /* XV_PROTO_VERSION the device speaks       */
    uint16_t fw_version;        /* firmware build version (separate)        */
    uint16_t width;             /* active width  in pixels (post-rotation)  */
    uint16_t height;            /* active height in pixels (post-rotation)  */
    uint16_t cols;              /* text grid columns (width / font_w)       */
    uint16_t rows;              /* text grid rows    (height / font_h)      */
    uint8_t  font_w;            /* built-in font cell width                 */
    uint8_t  font_h;            /* built-in font cell height                */
    uint8_t  color_format;      /* XV_FMT_*                                 */
    uint8_t  orientation;       /* XV_ROT_*                                 */
    uint8_t  caps;              /* XV_CAP_* bitfield                        */
    uint8_t  _pad;              /* keep max_payload 16-bit aligned          */
    uint16_t max_payload;       /* largest `length` accepted in one xfer    */
    uint32_t glyph_cache_bytes; /* SRAM reserved for custom glyphs          */
    uint32_t sprite_cache_bytes;/* SRAM reserved for sprites                */
} xv_info_t;                     /* 28 bytes */

typedef struct {
    uint16_t last_seq;          /* last command seq the device processed    */
    uint8_t  flags;             /* reserved                                 */
    uint8_t  last_error;        /* XV_ERR_* of the most recent error        */
    uint16_t dropped;           /* commands dropped (overrun / parse fail)  */
} xv_status_t;                   /* 6 bytes */

typedef struct {
    uint16_t seq;               /* seq of the offending command             */
    uint8_t  code;              /* XV_ERR_*                                 */
    uint8_t  _pad;
} xv_error_t;                    /* 4 bytes */

/* --------------------------------------------------------- host -> device */
typedef struct { uint16_t x, y, w, h; } xv_rect_t;          /* 8 bytes */

typedef struct {
    uint16_t x, y, w, h;
    uint16_t color;             /* RGB565                                   */
} xv_fill_rect_t;                /* 10 bytes */

typedef struct {
    uint16_t x, y, w, h;        /* then w*h RGB565 pixels stream after this */
} xv_blit_rect_t;                /* 8 bytes */

typedef struct {
    uint16_t id;
    uint16_t w, h;
    uint8_t  format;            /* XV_FMT_*                                 */
    uint8_t  _pad;              /* then w*h pixels stream after this        */
} xv_define_sprite_t;            /* 8 bytes */

typedef struct {
    uint16_t id;
    uint16_t x, y;
    uint8_t  flags;             /* reserved (flip/blend later)              */
    uint8_t  _pad;
} xv_draw_sprite_t;              /* 8 bytes */

typedef struct {
    uint8_t  row;
    uint8_t  col;
    uint16_t fg;                /* RGB565                                   */
    uint16_t bg;                /* RGB565                                   */
    uint8_t  count;             /* number of char codes that follow        */
    uint8_t  _pad;              /* then `count` char-code bytes             */
} xv_text_put_t;                 /* 8 bytes */

typedef struct {
    uint8_t  row, col;          /* top-left cell of region                  */
    uint8_t  cols, rows;        /* region size in cells                     */
    uint16_t bg;                /* fill color                               */
} xv_text_clear_t;               /* 6 bytes */

typedef struct {
    uint8_t  slot;              /* glyph slot (CGRAM analog)                */
    uint8_t  _pad;              /* then ceil(font_w*font_h/8) mask bytes    */
} xv_define_glyph_t;             /* 2 bytes */

typedef struct { uint8_t duty; } xv_brightness_t;          /* 0..255 PWM    */
typedef struct { uint8_t mode; } xv_power_t;               /* XV_PWR_*      */
typedef struct { uint8_t mode; } xv_present_mode_t;        /* XV_PRESENT_*  */
typedef struct { uint8_t rot; } xv_orientation_t;         /* XV_ROT_*      */
typedef struct { uint8_t panel; } xv_set_panel_t;          /* XV_PANEL_*    */

#endif /* XV_PROTOCOL_H */