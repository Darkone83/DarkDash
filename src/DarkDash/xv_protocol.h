#ifndef XV_PROTOCOL_H
#define XV_PROTOCOL_H
/*
 * X-View display protocol  --  shared source of truth.
 *
 * Compiles UNCHANGED in three places:
 *   1. RP2040 firmware            (this project)
 *   2. PC Python reference host   (mirrored / code-generated from this file)
 *   3. Xbox driver library        (C, MSVC2003 / RXDK)
 *
 * ===========================================================================
 *  VERSIONING / COMPATIBILITY CONTRACT  (read before editing)
 * ===========================================================================
 *  v0x0001 = the original immediate-mode set (control/config/graphics/sprite/
 *            text). v0x0002 = additive co-processor extensions (command list,
 *            assets+flash, scenes/animation, geometry, heartbeat/resync).
 *
 *  THE CARDINAL RULE: additive only. Nothing that already existed on the wire
 *  in v1 may be renumbered, resized, or reordered. A v1-only client MUST keep
 *  working against v2 firmware untouched. New features live in unused opcode
 *  space and are gated by capability bits, never by version alone.
 *
 *  - Existing opcodes/structs below are FROZEN. Do not touch them.
 *  - INFO (xv_info_t) is FROZEN at 28 bytes so fixed-size readers never desync.
 *    Extended device parameters live in the SEPARATE xv_caps_t (QUERY_CAPS),
 *    which only v2 clients ever request.
 *  - A device advertises which feature groups it actually implements via the
 *    caps bits; clients check caps and degrade gracefully. Probing an
 *    unimplemented op returns XV_ERR_NOT_SUPPORTED -- never a hang.
 * ===========================================================================
 *
 * WIRE MODEL
 *   Every transfer = xv_packet_header_t (8 bytes) followed by `length` payload
 *   bytes. All multi-byte fields are LITTLE-ENDIAN.
 *
 *   Small commands: payload is fully buffered then acted on.
 *   Pixel/asset-carrying commands (BLIT_RECT, DEFINE_SPRITE, ASSET_DEFINE): a
 *   small fixed header, then a byte stream the firmware consumes as it arrives.
 *   The parser is transfer-boundary agnostic -- chunked transfers reassemble.
 *
 * ACK MODEL
 *   Draw/config commands are fire-and-forget.
 *   Request/response pairs: PING->PONG, QUERY_INFO->INFO, STATUS_QUERY->STATUS,
 *   QUERY_CAPS->CAPS, SYNC->SYNC_ACK. The device may emit ERROR asynchronously
 *   on the IN endpoint, carrying the seq of the offending command.
 */

 /* MSVC2003 (RXDK toolchain) predates <stdint.h>. Provide a tiny shim there. */
#if defined(_MSC_VER) && (_MSC_VER < 1600)
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef signed   short int16_t;
typedef signed   int   int32_t;
#else
#include <stdint.h>
#endif

#define XV_PROTO_MAGIC    0x5856u   /* 'X','V' little-endian             */
#define XV_PROTO_VERSION  0x0002u   /* framing version the device speaks */

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

/* BLIT_RECT upscale: source is nearest-expanded by N on both axes (high nibble
   of flags). 0 or 1 = native. */
#define XV_HF_SCALE_SHIFT   4
#define XV_HF_MAKE_SCALE(n) ((uint8_t)(((n) & 0x0Fu) << XV_HF_SCALE_SHIFT))
#define XV_HF_GET_SCALE(f)  ((uint8_t)(((f) >> XV_HF_SCALE_SHIFT) & 0x0Fu))

   /* ----------------------------------------------------------------- opcodes */
typedef enum {
    /* ===== v1 (FROZEN) ===== */
    /* 0x0x  control / handshake */
    XV_OP_PING = 0x00,  /* host->dev, no payload  -> PONG            */
    XV_OP_PONG = 0x01,  /* dev->host, no payload                     */
    XV_OP_QUERY_INFO = 0x02,  /* host->dev, no payload  -> INFO            */
    XV_OP_INFO = 0x03,  /* dev->host, payload = xv_info_t            */
    XV_OP_STATUS_QUERY = 0x04,  /* host->dev, no payload  -> STATUS          */
    XV_OP_STATUS = 0x05,  /* dev->host, payload = xv_status_t          */
    XV_OP_ERROR = 0x06,  /* dev->host (async), payload = xv_error_t   */
    XV_OP_RESET = 0x07,  /* host->dev, clear fb + glyph + sprite cache*/

    /* ===== v2 control additions ===== */
    XV_OP_HEARTBEAT = 0x08,  /* host->dev, no payload; resets host-gone timer */
    XV_OP_QUERY_CAPS = 0x09,  /* host->dev, no payload  -> CAPS            */
    XV_OP_CAPS = 0x0A,  /* dev->host, payload = xv_caps_t            */
    XV_OP_SYNC = 0x0B,  /* host->dev, payload = xv_sync_t (query)    */
    XV_OP_SYNC_ACK = 0x0C,  /* dev->host, payload = xv_sync_t (state)    */

    /* 0x1x  display config (FROZEN through 0x14) */
    XV_OP_SET_BRIGHTNESS = 0x10, /* payload = xv_brightness_t                */
    XV_OP_POWER = 0x11, /* payload = xv_power_t                     */
    XV_OP_SET_PRESENT_MODE = 0x12, /* payload = xv_present_mode_t              */
    XV_OP_SET_ORIENTATION = 0x13, /* payload = xv_orientation_t (optional)    */
    XV_OP_SET_PANEL = 0x14, /* payload = xv_set_panel_t (XV_PANEL_*)    */

    /* 0x2x  graphics layer (FROZEN) */
    XV_OP_FILL_RECT = 0x20,  /* payload = xv_fill_rect_t                  */
    XV_OP_BLIT_RECT = 0x21,  /* xv_blit_rect_t + w*h RGB565 (streamed)    */
    XV_OP_CLEAR = 0x22,  /* payload = uint16 color (whole framebuffer)*/
    XV_OP_PRESENT = 0x23,  /* xv_rect_t (dirty); length 0 = full flush  */

    /* 0x3x  sprite cache (FROZEN) */
    XV_OP_DEFINE_SPRITE = 0x30,  /* xv_define_sprite_t + pixels (streamed)    */
    XV_OP_DRAW_SPRITE = 0x31,  /* payload = xv_draw_sprite_t                */
    XV_OP_FREE_SPRITE = 0x32,  /* payload = uint16 id (0xFFFF = free all)   */

    /* 0x4x  text / glyph layer (FROZEN) */
    XV_OP_TEXT_PUT = 0x40,  /* xv_text_put_t + `count` char codes        */
    XV_OP_TEXT_CLEAR = 0x41,  /* xv_text_clear_t (region) or length 0 = all*/
    XV_OP_DEFINE_GLYPH = 0x42,  /* xv_define_glyph_t + bitmap (CGRAM analog) */

    /* ===== v2 (additive; each gated by a caps bit) ===== */

    /* 0x5x  command list  (CAP_COMMAND_BUFFER)
       One opcode carrying a packed stream of sub-commands executed in order
       against the current render state. The "GPU command buffer". */
    XV_OP_CMD_LIST = 0x50,  /* payload = packed xv_cmd_* sub-ops         */

    /* 0x6x  geometry / vector / low-poly  (CAP_GEOMETRY_2D / CAP_GEOMETRY_3D) */
    XV_OP_DEFINE_MESH = 0x60,  /* xv_define_mesh_t + verts(+indices) stream */
    XV_OP_DRAW_MESH = 0x61,  /* payload = xv_draw_mesh_t (uses cur matrix)*/
    XV_OP_SET_MATRIX = 0x62,  /* payload = xv_matrix_t (3x4 fixed 16.16)   */
    XV_OP_FREE_MESH = 0x63,  /* payload = uint16 id (0xFFFF = free all)   */

    /* 0x7x  scene / animation  (CAP_SCENE_ENGINE)
       Retained, self-running content. Uploaded once; the device advances it on
       its own clock with zero per-frame host involvement. */
    XV_OP_SCENE_DEFINE = 0x70,  /* xv_scene_define_t + node list (streamed)  */
    XV_OP_SCENE_SHOW = 0x71,  /* payload = uint16 scene id                 */
    XV_OP_SCENE_FREE = 0x72,  /* payload = uint16 id (0xFFFF = free all)   */
    XV_OP_ANIM_DEFINE = 0x73,  /* xv_anim_define_t (tween/sheet/generator)  */
    XV_OP_ANIM_CONTROL = 0x74,  /* xv_anim_control_t (play/stop/loop/seek)   */
    XV_OP_NODE_SET = 0x75,  /* xv_node_set_t (mutate one node property)  */
    XV_OP_SET_GEN_PALETTE = 0x76, /* xv_gen_palette_t + stops: theme the generator LUT */
    XV_OP_SCENE_TEXT = 0x77,  /* xv_scene_text_t + chars: set a scene string slot   */

    /* 0x8x  asset cache / flash  (CAP_FLASH_CACHE)
       Write-once / read-many. Flash is only ever written on an explicit client
       request, never per frame. ASSET_DEFINE target picks RAM or FLASH. */
    XV_OP_ASSET_DEFINE = 0x80,  /* xv_asset_define_t + bytes (streamed)      */
    XV_OP_ASSET_FREE = 0x81,  /* payload = uint16 id (0xFFFF = free all)   */
    XV_OP_ASSET_PERSIST = 0x82,  /* xv_asset_persist_t: copy RAM asset->flash */
    XV_OP_SET_DEFAULT = 0x83,  /* xv_set_default_t: flash-persist boot/idle */

    XV_OP__COUNT
} xv_opcode_t;

/* ------------------------------------------------------------ enums / caps */
/* color_format */
#define XV_FMT_RGB565        0x00u

/* caps bitfield (xv_info_t.caps, 8-bit quick subset). Full 32-bit set is in
   xv_caps_t.caps32 via QUERY_CAPS. */
#define XV_CAP_DOUBLE_BUFFER  (1u << 0)
#define XV_CAP_GLYPH_CACHE    (1u << 1)
#define XV_CAP_SPRITE_CACHE   (1u << 2)
#define XV_CAP_COMMAND_BUFFER (1u << 3)
#define XV_CAP_SCENE_ENGINE   (1u << 4)
#define XV_CAP_FLASH_CACHE    (1u << 5)
#define XV_CAP_GEOMETRY_2D    (1u << 6)
#define XV_CAP_GEOMETRY_3D    (1u << 7)
   /* extended caps (xv_caps_t.caps32 only; bits 0..7 mirror the above) */
#define XV_CAP_HEARTBEAT      (1u << 8)
#define XV_CAP_TILED          (1u << 9)   /* tile-based deferred renderer      */
#define XV_CAP_DMA_FLUSH      (1u << 10)  /* DMA/PIO panel scanout             */
#define XV_CAP_GENERATORS     (1u << 11)  /* on-device plasma/fire/etc.        */
#define XV_CAP_RESYNC         (1u << 12)  /* SYNC/scene-version handshake       */
#define XV_CAP_GEN_PALETTE    (1u << 13)  /* host-settable generator palette ramp */

/* orientation */
#define XV_ROT_0             0x00u
#define XV_ROT_90            0x01u
#define XV_ROT_180           0x02u
#define XV_ROT_270           0x03u

/* power modes */
#define XV_PWR_OFF           0x00u
#define XV_PWR_ON            0x01u
#define XV_PWR_SLEEP         0x02u

/* present modes */
#define XV_PRESENT_MANUAL    0x00u
#define XV_PRESENT_AUTO      0x01u

/* error codes (xv_error_t.code) */
#define XV_ERR_NONE          0x00u
#define XV_ERR_BAD_MAGIC     0x01u
#define XV_ERR_BAD_OPCODE    0x02u
#define XV_ERR_BAD_LENGTH    0x03u
#define XV_ERR_OUT_OF_BOUNDS 0x04u
#define XV_ERR_NO_SPRITE     0x05u
#define XV_ERR_CACHE_FULL    0x06u
/* v2 additions */
#define XV_ERR_NOT_SUPPORTED 0x07u   /* op valid but this fw doesn't implement it */
#define XV_ERR_NO_ASSET      0x08u   /* reference to an undefined asset/mesh/scene*/
#define XV_ERR_FLASH_FULL    0x09u
#define XV_ERR_FLASH_BUSY    0x0Au   /* write deferred; not at a safe point yet    */
#define XV_ERR_BAD_PARAM     0x0Bu

/* --------------------------------------------------------- device -> host */
/* FROZEN: exactly 28 bytes. Never grow this struct. */
typedef struct {
    uint16_t proto_version;     /* XV_PROTO_VERSION the device speaks       */
    uint16_t fw_version;        /* firmware build version (separate)        */
    uint16_t width;             /* active width  in pixels (post-rotation)  */
    uint16_t height;            /* active height in pixels (post-rotation)  */
    uint16_t cols;              /* text grid columns                        */
    uint16_t rows;              /* text grid rows                           */
    uint8_t  font_w;
    uint8_t  font_h;
    uint8_t  color_format;      /* XV_FMT_*                                 */
    uint8_t  orientation;       /* XV_ROT_*                                 */
    uint8_t  caps;              /* XV_CAP_* 8-bit quick subset              */
    uint8_t  _pad;
    uint16_t max_payload;       /* largest `length` accepted in one xfer    */
    uint32_t glyph_cache_bytes;
    uint32_t sprite_cache_bytes;
} xv_info_t;                     /* 28 bytes */

/* v2 extended capabilities (QUERY_CAPS -> CAPS). Only v2 clients request it,
   so it can evolve append-only without risking v1 fixed-size readers. */
typedef struct {
    uint16_t proto_version;
    uint16_t struct_bytes;      /* sizeof(xv_caps_t) the fw sent (fwd-compat) */
    uint32_t caps32;            /* full XV_CAP_* bitfield                     */
    uint16_t tile_w;            /* tile dimensions (0 if not tiled)           */
    uint16_t tile_h;
    uint16_t max_layers;
    uint16_t max_meshes;
    uint16_t max_verts;         /* per mesh                                   */
    uint16_t max_scenes;
    uint16_t max_anims;
    uint16_t max_assets;
    uint32_t ram_asset_bytes;   /* working asset budget in SRAM               */
    uint32_t flash_asset_bytes; /* persistent asset budget in flash           */
} xv_caps_t;                     /* 32 bytes (v2.0); may grow append-only      */

typedef struct {
    uint16_t last_seq;
    uint8_t  flags;
    uint8_t  last_error;
    uint16_t dropped;
} xv_status_t;                   /* 6 bytes (FROZEN) */

typedef struct {
    uint16_t seq;
    uint8_t  code;
    uint8_t  _pad;
} xv_error_t;                    /* 4 bytes (FROZEN) */

/* SYNC: lets a reconnecting host learn whether its uploaded state survived.
   Host sends its known epoch; device replies with its current epoch + what it
   still holds. If epochs differ, the device reset and the host re-uploads. */
typedef struct {
    uint32_t epoch;             /* bumped by the device on every cold start   */
    uint16_t scene_active;      /* currently shown scene id (0xFFFF = none)   */
    uint16_t assets_held;       /* count of live assets                       */
    uint16_t scenes_held;
    uint16_t _pad;
} xv_sync_t;                     /* 12 bytes */

/* --------------------------------------------------------- host -> device */
/* ----- v1 (FROZEN) ----- */
typedef struct { uint16_t x, y, w, h; } xv_rect_t;          /* 8 bytes */

typedef struct {
    uint16_t x, y, w, h;
    uint16_t color;
} xv_fill_rect_t;                /* 10 bytes */

typedef struct {
    uint16_t x, y, w, h;        /* then w*h RGB565 pixels stream after this */
} xv_blit_rect_t;                /* 8 bytes */

typedef struct {
    uint16_t id;
    uint16_t w, h;
    uint8_t  format;
    uint8_t  _pad;              /* then w*h pixels stream after this        */
} xv_define_sprite_t;            /* 8 bytes */

typedef struct {
    uint16_t id;
    uint16_t x, y;
    uint8_t  flags;
    uint8_t  _pad;
} xv_draw_sprite_t;              /* 8 bytes */

typedef struct {
    uint8_t  row;
    uint8_t  col;
    uint16_t fg;
    uint16_t bg;
    uint8_t  count;
    uint8_t  _pad;              /* then `count` char-code bytes             */
} xv_text_put_t;                 /* 8 bytes */

typedef struct {
    uint8_t  row, col;
    uint8_t  cols, rows;
    uint16_t bg;
} xv_text_clear_t;               /* 6 bytes */

typedef struct {
    uint8_t  slot;
    uint8_t  _pad;              /* then ceil(font_w*font_h/8) mask bytes    */
} xv_define_glyph_t;             /* 2 bytes */

typedef struct { uint8_t duty; } xv_brightness_t;
typedef struct { uint8_t mode; } xv_power_t;
typedef struct { uint8_t mode; } xv_present_mode_t;
typedef struct { uint8_t rot; } xv_orientation_t;

#define XV_PANEL_A   0x00u   /* long bar strip            */
#define XV_PANEL_B   0x01u   /* 320x240 test panel        */
typedef struct { uint8_t panel; } xv_set_panel_t;

/* ===================== v2 host -> device payloads ===================== */

/* ----- 0x50 command list -----
   Payload = back-to-back sub-commands. Each sub-command:
     uint8 sub_op; uint8 arg_bytes; <arg_bytes of args>
   executed in order against the current state (color/clip/blend/matrix/target).
   This is the single vocabulary: immediate "fill a rect" is just a one-entry
   list; everything richer is more entries in the same list. */
typedef enum {
    XV_CMD_END = 0x00,   /* terminator (optional)                     */
    XV_CMD_SET_COLOR = 0x01,   /* uint16 rgb565                             */
    XV_CMD_SET_CLIP = 0x02,   /* int16 x,y,w,h (w<0 => clear clip)         */
    XV_CMD_SET_BLEND = 0x03,   /* uint8 mode (0 copy,1 alpha-key,2 add)     */
    XV_CMD_SET_TARGET = 0x04,   /* uint8 layer                               */
    XV_CMD_FILL = 0x05,   /* uint16 x,y,w,h (uses cur color)           */
    XV_CMD_RECT = 0x06,   /* uint16 x,y,w,h (outline, cur color)       */
    XV_CMD_LINE = 0x07,   /* int16 x0,y0,x1,y1                         */
    XV_CMD_HLINE = 0x08,   /* int16 x,y; uint16 w                       */
    XV_CMD_VLINE = 0x09,   /* int16 x,y; uint16 h                       */
    XV_CMD_PIXEL = 0x0A,   /* int16 x,y                                 */
    XV_CMD_SPRITE = 0x0B,   /* uint16 id; int16 x,y                      */
    XV_CMD_TEXT = 0x0C,   /* uint8 row,col,count; bytes codes (cur col)*/
    XV_CMD_MESH = 0x0D,   /* uint16 mesh id (uses cur matrix)          */
    XV_CMD_GRADIENT = 0x0E,   /* uint16 x,y,w,h, c0, c1; uint8 dir         */
    XV_CMD_PRESENT = 0x0F    /* flush dirty (no args)                     */
    /* 0x10..0x7F reserved for future sub-ops */
} xv_cmd_subop_t;

/* ----- 0x6x geometry (fixed-point 16.16 everywhere; no FPU dependency) ----- */
#define XV_MESH_WIREFRAME   0x00u
#define XV_MESH_FLAT        0x01u   /* flat-shaded solid (per-tile z)         */
typedef struct {
    uint16_t id;
    uint16_t vert_count;
    uint16_t index_count;       /* 0 => sequential triangles                  */
    uint8_t  mode;              /* XV_MESH_*                                  */
    uint8_t  flags;
    /* then vert_count * xv_vertex_t, then index_count * uint16 */
} xv_define_mesh_t;              /* 8 bytes */

typedef struct { int32_t x, y, z; uint16_t color; uint16_t _pad; } xv_vertex_t; /* 16 */

typedef struct {
    uint16_t id;
    uint8_t  flags;             /* bit0 backface-cull                         */
    uint8_t  _pad;
} xv_draw_mesh_t;                /* 4 bytes */

typedef struct { int32_t m[12]; } xv_matrix_t; /* 3x4 row-major, 16.16 (48 B) */

/* ----- 0x7x scene / animation ----- */
#define XV_NODE_SPRITE    0x00u
#define XV_NODE_TEXT      0x01u
#define XV_NODE_FILL      0x02u
#define XV_NODE_MESH      0x03u
#define XV_NODE_GENERATOR 0x04u   /* plasma/fire/starfield (CAP_GENERATORS)   */
#define XV_NODE_ASSET     0x05u   /* blit a cached asset                      */
typedef struct {
    uint16_t scene_id;
    uint16_t node_count;
    uint8_t  flags;             /* bit0 = run on host-gone (fallback scene)   */
    uint8_t  _pad;
    /* then node_count * xv_node_t (streamed) */
} xv_scene_define_t;             /* 6 bytes */

typedef struct {
    uint16_t node_id;           /* unique within scene                        */
    uint8_t  type;              /* XV_NODE_*                                  */
    uint8_t  layer;
    int16_t  x, y;
    uint16_t a, b;              /* type-specific (id / color / w / param)     */
    uint16_t c, d;
} xv_node_t;                     /* 16 bytes */

#define XV_ANIM_TWEEN     0x00u   /* interpolate a node property A->B over ms */
#define XV_ANIM_SHEET     0x01u   /* cycle a sprite/asset range at fps        */
#define XV_ANIM_SCROLL    0x02u   /* continuous offset                        */
#define XV_ANIM_ROTATE    0x03u   /* spin a mesh node                         */
#define XV_ANIM_GEN       0x04u   /* drive a generator's params               */
#define XV_EASE_LINEAR    0x00u
#define XV_EASE_IN_OUT    0x01u
#define XV_EASE_OUT       0x02u
typedef struct {
    uint16_t anim_id;
    uint16_t scene_id;
    uint16_t node_id;
    uint8_t  type;              /* XV_ANIM_*                                  */
    uint8_t  prop;              /* which node field is driven                 */
    int32_t  from;
    int32_t  to;
    uint16_t duration_ms;       /* or frame interval for SHEET                */
    uint8_t  ease;              /* XV_EASE_*                                  */
    uint8_t  loop;              /* 0 once, 1 loop, 2 ping-pong                */
} xv_anim_define_t;              /* 20 bytes */

#define XV_ANIMC_PLAY     0x00u
#define XV_ANIMC_STOP     0x01u
#define XV_ANIMC_SEEK     0x02u
typedef struct {
    uint16_t anim_id;
    uint8_t  action;            /* XV_ANIMC_*                                 */
    uint8_t  _pad;
    int32_t  seek_ms;
} xv_anim_control_t;             /* 8 bytes */

typedef struct {
    uint16_t scene_id;
    uint16_t node_id;
    uint8_t  prop;
    uint8_t  _pad;
    int32_t  value;
} xv_node_set_t;                 /* 12 bytes */

/* SET_GEN_PALETTE: a compact theme ramp the firmware interpolates into the
   256-entry generator LUT. Header below, then stop_count x uint16 RGB565 stops.
   2..XV_GEN_PALETTE_MAX_STOPS stops; the default LUT is the built-in rainbow. */
#define XV_GEN_PALETTE_MAX_STOPS 16
typedef struct {
    uint8_t  stop_count;         /* number of RGB565 stops that follow (2..16) */
    uint8_t  _pad;
} xv_gen_palette_t;              /* 2 bytes + stop_count * uint16 */

/* SCENE_TEXT: set the string in a fixed scene-text slot (overwrite in place, no
   allocation). A NODE_TEXT node references the slot by id in its `a` field and is
   redrawn from the slot every scene tick, so updating live text = re-send this op
   with the new bytes (a few bytes), never a scene rebuild. */
typedef struct {
    uint8_t  slot;              /* 0 .. XV_SCENE_STR_SLOTS-1                  */
    uint8_t  count;            /* char count that follows (clamped to MAX)   */
    /* then `count` char-code bytes */
} xv_scene_text_t;              /* 2 bytes + chars */


/* ----- 0x8x asset cache / flash ----- */
#define XV_ASSET_BITMAP   0x00u   /* RGB565 pixels (w*h)                      */
#define XV_ASSET_RAW      0x01u   /* opaque blob                              */
#define XV_TARGET_RAM     0x00u
#define XV_TARGET_FLASH   0x01u   /* persistent; write happens at a safe point*/
typedef struct {
    uint16_t id;
    uint16_t w, h;              /* for BITMAP; else w=byte count lo, h=hi     */
    uint8_t  type;              /* XV_ASSET_*                                 */
    uint8_t  target;            /* XV_TARGET_*                                */
    uint8_t  format;            /* XV_FMT_* for BITMAP                        */
    uint8_t  _pad;
    /* then the asset bytes (streamed) */
} xv_asset_define_t;             /* 10 bytes */

typedef struct {
    uint16_t id;                /* RAM asset to copy into flash               */
    uint16_t _pad;
} xv_asset_persist_t;            /* 4 bytes */

#define XV_DEFAULT_BOOT   0x00u   /* shown at power-on before any host         */
#define XV_DEFAULT_IDLE   0x01u   /* fallback scene when the host goes away    */
typedef struct {
    uint8_t  kind;              /* XV_DEFAULT_*                               */
    uint8_t  _pad;
    uint16_t scene_id;          /* a flash-resident scene id (0xFFFF clears)  */
} xv_set_default_t;              /* 4 bytes */

#endif /* XV_PROTOCOL_H */