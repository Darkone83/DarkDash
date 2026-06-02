#!/usr/bin/env python3
"""
ddf_encoder.py -- DarkDash Font (.ddf) encoder, GUI.

Rasterizes a TrueType/OpenType font into a DarkDash .ddf file that the
dashboard loads at runtime to replace the baked-in "Default" atlas.

WHY 720p-scale sizes:
  The dash authors UI in a 640x480 virtual space and the runtime scales it up
  for 720p displays. If glyphs are rasterized at 480p pixel sizes they get
  upscaled (and look soft/blocky) at 720p. So we rasterize at the 720p pixel
  sizes; at 720p they're 1:1 crisp, and at 480p they cleanly downscale.

  The three logical sizes the dash uses (SMALL/MEDIUM/LARGE = 14/18/24 in
  virtual units) are therefore rasterized at 1.5x: 21 / 27 / 36 px.

GLYPH SET: printable ASCII 32..126 (95 glyphs), matching the engine.

.ddf BINARY FORMAT (little-endian):
  magic        u32   'DDF1' = 0x31464444
  version      u32   = 1
  atlasW       u32
  atlasH       u32
  sizePx[3]    u32   rasterized pixel size of SMALL/MEDIUM/LARGE
  --- then per size (3x), 95 glyph records: ---
  x,y,w,h,advance,bear_y : i32 each   (matches engine GlyphMetrics)
  --- then atlas pixels: ---
  atlasW*atlasH*4 bytes, BGRA byte order (XGSwizzleRect-ready)

Requires: pip install pillow
"""

import struct
import os
import sys

# tkinter (stdlib) first, so we can show GUI errors even if Pillow is missing
try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    HAVE_TK = True
except ImportError:
    HAVE_TK = False

# Pillow is required for the actual work, but import it softly so a double-click
# launch can pop a readable dialog instead of a console window that vanishes.
try:
    from PIL import Image, ImageFont, ImageDraw
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False

DDF_MAGIC   = 0x31464444   # 'DDF1'
DDF_VERSION = 1
FIRST_CH    = 32
LAST_CH     = 126
N_GLYPHS    = LAST_CH - FIRST_CH + 1   # 95

# logical virtual sizes (SMALL/MEDIUM/LARGE) and the 720p raster scale
VIRT_SIZES  = (14, 18, 24)
RASTER_SCALE = 1.5                      # 480p->720p
PAD          = 2                        # px gap between glyphs in the atlas

CANDIDATE_ATLAS = (512, 1024, 2048)


def raster_sizes():
    return [max(1, int(round(v * RASTER_SCALE))) for v in VIRT_SIZES]


def render_size(font_path, px):
    """Render the 95 glyphs at pixel size px. Returns (glyph_imgs, metrics)
    where metrics is a list of dicts: w,h,advance,bear_y (x,y filled at pack)."""
    font = ImageFont.truetype(font_path, px)
    ascent, descent = font.getmetrics()
    glyphs = []
    for code in range(FIRST_CH, LAST_CH + 1):
        ch = chr(code)
        # measure
        try:
            bbox = font.getbbox(ch)
        except Exception:
            bbox = (0, 0, 0, 0)
        x0, y0, x1, y1 = bbox
        w = max(1, x1 - x0)
        h = max(1, y1 - y0)
        # advance
        try:
            adv = int(round(font.getlength(ch)))
        except Exception:
            adv = w
        # render the glyph tightly into its own RGBA image
        img = Image.new("RGBA", (w + 2, h + 2), (0, 0, 0, 0))
        drw = ImageDraw.Draw(img)
        drw.text((-x0 + 1, -y0 + 1), ch, font=font, fill=(255, 255, 255, 255))
        img = img.crop((0, 0, w + 2, h + 2))
        # bear_y: offset from the baseline to the glyph top (negative = above).
        # engine uses -bear_y as ascender; top of glyph is y0 relative to baseline.
        bear_y = y0 - ascent
        glyphs.append({
            "img": img, "w": w, "h": h, "advance": adv, "bear_y": bear_y,
        })
    return glyphs


def pack_atlas(all_sizes, atlas_w, atlas_h):
    """Shelf-pack all glyphs from all 3 sizes into one atlas. Returns the
    Image and fills x,y into each glyph dict, or None if it doesn't fit."""
    atlas = Image.new("RGBA", (atlas_w, atlas_h), (0, 0, 0, 0))
    cx, cy, row_h = PAD, PAD, 0
    for glyphs in all_sizes:
        for g in glyphs:
            gw, gh = g["img"].size
            if cx + gw + PAD > atlas_w:
                cx = PAD
                cy += row_h + PAD
                row_h = 0
            if cy + gh + PAD > atlas_h:
                return None    # doesn't fit; caller tries a bigger atlas
            atlas.paste(g["img"], (cx, cy))
            g["x"] = cx
            g["y"] = cy
            cx += gw + PAD
            if gh > row_h:
                row_h = gh
    return atlas


def build_ddf(font_path):
    """Render + pack + return (bytes, atlas_w, atlas_h, raster_px_list)."""
    px = raster_sizes()
    all_sizes = [render_size(font_path, p) for p in px]

    atlas = None
    aw = ah = 0
    for cand in CANDIDATE_ATLAS:
        atlas = pack_atlas(all_sizes, cand, cand)
        if atlas is not None:
            aw = ah = cand
            break
    if atlas is None:
        raise RuntimeError("Font too large to pack into 2048x2048.")

    # header
    out = bytearray()
    out += struct.pack("<IIII", DDF_MAGIC, DDF_VERSION, aw, ah)
    out += struct.pack("<III", px[0], px[1], px[2])

    # glyph metrics, per size, 95 each
    for glyphs in all_sizes:
        for g in glyphs:
            out += struct.pack("<iiiiii",
                               g["x"], g["y"], g["w"], g["h"],
                               g["advance"], g["bear_y"])

    # atlas pixels, BGRA (swap R<->B from PIL's RGBA)
    rgba = atlas.tobytes()  # RGBA
    bgra = bytearray(len(rgba))
    bgra[0::4] = rgba[2::4]  # B
    bgra[1::4] = rgba[1::4]  # G
    bgra[2::4] = rgba[0::4]  # R
    bgra[3::4] = rgba[3::4]  # A
    out += bgra

    return bytes(out), aw, ah, px, atlas


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------

class App:
    def __init__(self, root):
        self.root = root
        self.font_path = None
        self.preview_img = None
        root.title("DarkDash Font Encoder (.ddf)")
        root.geometry("560x420")

        frm = ttk.Frame(root, padding=12)
        frm.pack(fill="both", expand=True)

        ttk.Label(frm, text="DarkDash Font Encoder",
                  font=("TkDefaultFont", 13, "bold")).pack(anchor="w")
        ttk.Label(frm, text="Rasterizes a TTF/OTF at 720p scale (21/27/36 px) "
                             "into a .ddf for the dashboard.",
                  foreground="#555").pack(anchor="w", pady=(0, 10))

        row1 = ttk.Frame(frm); row1.pack(fill="x", pady=4)
        ttk.Button(row1, text="Choose Font...",
                   command=self.choose).pack(side="left")
        self.lbl_font = ttk.Label(row1, text="(no font selected)")
        self.lbl_font.pack(side="left", padx=8)

        self.canvas = tk.Canvas(frm, height=180, bg="#101810",
                                highlightthickness=1, highlightbackground="#333")
        self.canvas.pack(fill="x", pady=8)

        row2 = ttk.Frame(frm); row2.pack(fill="x", pady=4)
        self.btn_export = ttk.Button(row2, text="Export .ddf...",
                                     command=self.export, state="disabled")
        self.btn_export.pack(side="left")
        self.lbl_status = ttk.Label(frm, text="", foreground="#080")
        self.lbl_status.pack(anchor="w", pady=(8, 0))

    def choose(self):
        path = filedialog.askopenfilename(
            title="Choose a TTF/OTF font",
            filetypes=[("Fonts", "*.ttf *.otf"), ("All files", "*.*")])
        if not path:
            return
        self.font_path = path
        self.lbl_font.config(text=os.path.basename(path))
        self.btn_export.config(state="normal")
        self.preview()

    def preview(self):
        self.canvas.delete("all")
        try:
            font = ImageFont.truetype(self.font_path, 32)
            img = Image.new("RGBA", (520, 160), (16, 24, 16, 255))
            d = ImageDraw.Draw(img)
            d.text((12, 10), "DarkDash 0123456789",
                   font=font, fill=(174, 255, 60, 255))
            d.text((12, 60), "The quick brown fox",
                   font=ImageFont.truetype(self.font_path, 24),
                   fill=(216, 248, 192, 255))
            d.text((12, 100), "jumps over the lazy dog.",
                   font=ImageFont.truetype(self.font_path, 18),
                   fill=(216, 248, 192, 255))
            # tk needs a PhotoImage; convert via PPM in memory
            self.preview_img = self._to_photo(img)
            self.canvas.create_image(0, 0, anchor="nw", image=self.preview_img)
        except Exception as e:
            self.canvas.create_text(12, 12, anchor="nw",
                                    text="preview failed: %s" % e, fill="#c44")

    def _to_photo(self, pil_img):
        import io
        buf = io.BytesIO()
        pil_img.convert("RGB").save(buf, format="PPM")
        return tk.PhotoImage(data=buf.getvalue())

    def export(self):
        if not self.font_path:
            return
        default = os.path.splitext(os.path.basename(self.font_path))[0] + ".ddf"
        out = filedialog.asksaveasfilename(
            title="Save .ddf", defaultextension=".ddf",
            initialfile=default, filetypes=[("DarkDash Font", "*.ddf")])
        if not out:
            return
        try:
            data, aw, ah, px, _atlas = build_ddf(self.font_path)
            with open(out, "wb") as f:
                f.write(data)
            self.lbl_status.config(
                text="Wrote %s  (%dx%d atlas, sizes %s, %d KB)" %
                     (os.path.basename(out), aw, ah, px, len(data) // 1024),
                foreground="#080")
        except Exception as e:
            messagebox.showerror("Export failed", str(e))
            self.lbl_status.config(text="export failed", foreground="#c00")


def main():
    # headless: ddf_encoder.py <font> <out.ddf>
    if len(sys.argv) == 3:
        if not HAVE_PIL:
            print("This tool needs Pillow:  pip install pillow")
            return
        data, aw, ah, px, _ = build_ddf(sys.argv[1])
        with open(sys.argv[2], "wb") as f:
            f.write(data)
        print("Wrote %s (%dx%d, sizes %s, %d KB)" %
              (sys.argv[2], aw, ah, px, len(data) // 1024))
        return

    # double-click / no args -> GUI
    if not HAVE_TK:
        print("Usage (headless): ddf_encoder.py <font.ttf> <out.ddf>")
        print("(install tkinter for the GUI)")
        return

    root = tk.Tk()
    if not HAVE_PIL:
        # show a readable dialog rather than a console that vanishes on double-click
        root.withdraw()
        messagebox.showerror(
            "Missing dependency",
            "DarkDash Font Encoder needs the 'Pillow' library.\n\n"
            "Install it from a command prompt:\n\n"
            "    pip install pillow\n\n"
            "then double-click this tool again.")
        return
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()