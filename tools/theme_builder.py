#!/usr/bin/env python3
"""
theme_builder.py -- DarkDash theme builder / validator, GUI.

A DarkDash theme is just a folder:

    <name>/
        theme.ini
        assets/
            bar_header.png        (300x40)
            bar_footer.png        (624x32)
            frame_menu_v.png      (272x384)
            orb_hero.png          (272x234)
            platform_round.png    (pedestal art)
            overlay_selection_glow.png
            bg.png                (640x480, optional painted background)
            raw/                  (optional per-game icon overrides)

This tool *builds* that structure for you: pick a build location and a name,
assign an image to each slot, set the palette / glow / background, and hit
Build. It creates <location>\\<name>\\assets (and assets\\raw), copies your
chosen art into the correctly-named slot files, and writes a correct theme.ini.
The engine consumes folders directly -- just copy the result to D:\\themes\\.

The engine matches assets by FIXED filename (Theme_Asset("bar_header") loads
assets\\bar_header.png), so the slot names below are not negotiable -- that's
why the builder renames your imported art to the slot name. theme.ini itself
only carries [manifest]/[palette]/[glow]/[background]; the assets are not
listed there, by design.

Requires: pip install pillow  (for asset import/convert + size validation;
ini writing is stdlib). Without Pillow the builder can still copy art that is
already .png, but cannot convert other formats or resize.
"""

import os
import sys
import shutil

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk, colorchooser
    HAVE_TK = True
except ImportError:
    HAVE_TK = False

try:
    from PIL import Image
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False

# slot name -> (recommended W, H, required?)
SLOTS = [
    ("bar_header",             300,  40, True),
    ("bar_footer",             624,  32, True),
    ("frame_menu_v",           272, 384, True),
    ("orb_hero",               272, 234, True),
    ("platform_round",         256, 120, True),
    ("overlay_selection_glow", 220,  40, False),
]
BG_SLOT = ("bg", 640, 480)   # optional painted background

DEFAULT_PALETTE = {
    "accent":     "7FE000",
    "accent_dim": "4C8A00",
    "glow":       "AEFF3C",
    "text":       "D8F8C0",
    "text_dim":   "7FA060",
    "bg":         "060A06",
}


def safe_folder_name(name):
    """Make a filesystem-safe theme folder name from a display name."""
    keep = []
    for ch in (name or "").strip():
        if ch.isalnum() or ch in (" ", "-", "_", "."):
            keep.append(ch)
    out = "".join(keep).strip(" .")
    out = out.replace(" ", "_")
    return out or "MyTheme"


def import_asset(src, dst_path, size, do_resize):
    """Copy/convert a source image into dst_path as PNG.

    Returns a short status string. Preserves alpha (frames/orb/glow rely on it).
    Falls back to a raw byte copy for .png sources when Pillow is unavailable.
    """
    if HAVE_PIL:
        im = Image.open(src)
        if im.mode != "RGBA":
            im = im.convert("RGBA")
        if do_resize and size and im.size != tuple(size):
            im = im.resize(tuple(size), Image.LANCZOS)
        im.save(dst_path, "PNG")
        gw, gh = Image.open(dst_path).size
        return "imported %dx%d" % (gw, gh)
    # no Pillow: only a straight copy of an existing PNG is safe
    if os.path.splitext(src)[1].lower() != ".png":
        raise RuntimeError("install Pillow to import non-PNG art (%s)"
                           % os.path.basename(src))
    shutil.copyfile(src, dst_path)
    return "copied (Pillow not installed; size unchecked)"


def build_theme(location, name, author, palette, glow_on, glow_color,
                glow_intensity, use_bg, asset_src, do_resize):
    """Create <location>/<safe_name>/ with assets/ (+ raw/), import the chosen
    art into slot files, and write theme.ini.

    asset_src: dict slot_name -> source image path (or missing/None to skip).
               include key "bg" for the painted background.
    Returns (theme_dir, report_lines, ok).
    """
    report = []
    ok = True

    folder = safe_folder_name(name)
    theme_dir = os.path.join(location, folder)
    assets = os.path.join(theme_dir, "assets")
    raw = os.path.join(assets, "raw")

    os.makedirs(raw, exist_ok=True)   # creates theme_dir, assets, raw in one go
    report.append("Folder: %s" % theme_dir)
    report.append("  assets/        created")
    report.append("  assets/raw/    created (icon overrides)")
    report.append("")

    # import the six slot assets
    for nm, w, h, req in SLOTS:
        src = asset_src.get(nm)
        dst = os.path.join(assets, nm + ".png")
        if src:
            try:
                note = import_asset(src, dst, (w, h), do_resize)
                report.append("  %-26s %s" % (nm + ".png", note))
            except Exception as e:
                ok = False
                report.append("  %-26s FAILED: %s" % (nm + ".png", e))
        elif os.path.isfile(dst):
            report.append("  %-26s kept existing" % (nm + ".png"))
        else:
            if req:
                ok = False
            mark = "MISSING (required)" if req else "skipped (optional)"
            report.append("  %-26s %s" % (nm + ".png", mark))

    # painted background
    bw, bh = BG_SLOT[1], BG_SLOT[2]
    bg_src = asset_src.get("bg")
    bg_dst = os.path.join(assets, "bg.png")
    have_bg = False
    if use_bg:
        if bg_src:
            try:
                note = import_asset(bg_src, bg_dst, (bw, bh), do_resize)
                report.append("  %-26s %s" % ("bg.png", note))
                have_bg = True
            except Exception as e:
                ok = False
                report.append("  %-26s FAILED: %s" % ("bg.png", e))
        elif os.path.isfile(bg_dst):
            report.append("  %-26s kept existing" % "bg.png")
            have_bg = True
        else:
            report.append("  %-26s WANTED but none chosen" % "bg.png")

    # theme.ini (background written only if we actually have a bg image)
    ini = write_ini(theme_dir, name, author, palette, glow_on, glow_color,
                    glow_intensity, use_bg and have_bg)
    report.append("")
    report.append("Wrote %s" % ini)
    return (theme_dir, report, ok)


def validate(theme_dir):
    """Return (lines, ok). lines = human-readable per-slot report."""
    assets = os.path.join(theme_dir, "assets")
    lines = []
    ok = True
    if not os.path.isdir(assets):
        return (["ERROR: no 'assets' sub-folder found"], False)

    for name, w, h, req in SLOTS:
        path = os.path.join(assets, name + ".png")
        if not os.path.isfile(path):
            mark = "MISSING (required)" if req else "missing (optional)"
            if req:
                ok = False
            lines.append("  %-26s %s" % (name + ".png", mark))
            continue
        if HAVE_PIL:
            try:
                im = Image.open(path)
                gw, gh = im.size
                note = "OK" if (gw, gh) == (w, h) else \
                       "size %dx%d (expected %dx%d)" % (gw, gh, w, h)
            except Exception as e:
                note = "unreadable: %s" % e
                ok = False
        else:
            note = "present (install Pillow to check size)"
        lines.append("  %-26s %s" % (name + ".png", note))

    # optional background
    bgp = os.path.join(assets, BG_SLOT[0] + ".png")
    if os.path.isfile(bgp):
        lines.append("  %-26s present (painted background)" % (BG_SLOT[0] + ".png"))
    return (lines, ok)


def write_ini(theme_dir, name, author, palette, glow_on, glow_color,
              glow_intensity, use_bg):
    p = os.path.join(theme_dir, "theme.ini")
    L = []
    L.append("; DarkDash theme -- generated by theme_builder.py")
    L.append("")
    L.append("[manifest]")
    L.append("schema_version  = 1")
    L.append("name            = %s" % name)
    L.append("author          = %s" % author)
    L.append("base_resolution = 640x480")
    L.append("asset_dir       = assets")
    L.append("")
    L.append("[palette]")
    for k in ("accent", "accent_dim", "glow", "text", "text_dim", "bg"):
        L.append("%-11s = %s" % (k, palette.get(k, DEFAULT_PALETTE[k])))
    L.append("")
    L.append("[glow]")
    L.append("enabled   = %d" % (1 if glow_on else 0))
    L.append("color     = %s" % glow_color)
    L.append("intensity = %d" % glow_intensity)
    L.append("")
    L.append("[background]")
    if use_bg:
        L.append("module = static")
        L.append("image  = assets/bg.png")
    else:
        L.append("module = none")
    L.append("")
    with open(p, "w") as f:
        f.write("\n".join(L) + "\n")
    return p


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------

class App:
    def __init__(self, root):
        self.root = root
        self.location = None             # build parent dir
        self.palette = dict(DEFAULT_PALETTE)
        self.asset_src = {}              # slot -> chosen source path
        self.asset_lbls = {}             # slot -> status Label
        root.title("DarkDash Theme Builder")
        root.geometry("700x800")
        root.minsize(680, 620)

        frm = ttk.Frame(root, padding=12)
        frm.pack(fill="both", expand=True)

        ttk.Label(frm, text="DarkDash Theme Builder",
                  font=("TkDefaultFont", 13, "bold")).pack(anchor="w")
        ttk.Label(frm, text="Pick a location and name, assign art to each slot, "
                            "set options, then Build.",
                  foreground="#555").pack(anchor="w", pady=(0, 8))

        # build location + name
        loc = ttk.Frame(frm); loc.pack(fill="x", pady=4)
        ttk.Button(loc, text="Build Location...",
                   command=self.choose_location).pack(side="left")
        self.lbl_loc = ttk.Label(loc, text="(none)")
        self.lbl_loc.pack(side="left", padx=8)

        meta = ttk.Frame(frm); meta.pack(fill="x", pady=4)
        ttk.Label(meta, text="Name").grid(row=0, column=0, sticky="w")
        self.e_name = ttk.Entry(meta, width=28); self.e_name.grid(row=0, column=1, padx=6)
        self.e_name.insert(0, "My Theme")
        self.e_name.bind("<KeyRelease>", lambda _e: self.refresh_target())
        ttk.Label(meta, text="Author").grid(row=0, column=2, sticky="w")
        self.e_auth = ttk.Entry(meta, width=20); self.e_auth.grid(row=0, column=3, padx=6)
        self.lbl_target = ttk.Label(frm, text="", foreground="#557")
        self.lbl_target.pack(anchor="w")

        # assets
        ass = ttk.LabelFrame(frm, text="Assets (assigned art is renamed to the slot)", padding=8)
        ass.pack(fill="x", pady=6)
        rows = SLOTS + [(BG_SLOT[0], BG_SLOT[1], BG_SLOT[2], False)]
        for i, (nm, w, h, req) in enumerate(rows):
            tag = nm + ("  (required)" if req else "  (optional)")
            ttk.Label(ass, text=tag, width=30).grid(row=i, column=0, sticky="w", pady=1)
            ttk.Label(ass, text="%dx%d" % (w, h), width=9,
                      foreground="#777").grid(row=i, column=1, sticky="w")
            ttk.Button(ass, text="Choose...", width=9,
                       command=lambda s=nm: self.pick_asset(s)).grid(row=i, column=2, padx=4)
            lb = ttk.Label(ass, text="(none)", foreground="#999")
            lb.grid(row=i, column=3, sticky="w", padx=4)
            self.asset_lbls[nm] = lb

        self.resize_on = tk.IntVar(value=0)
        ttk.Checkbutton(frm, text="Resize imported art to recommended size on import "
                                  "(may distort aspect ratio)",
                        variable=self.resize_on).pack(anchor="w", pady=(2, 4))

        # palette swatches
        pal = ttk.LabelFrame(frm, text="Palette (RRGGBB)", padding=8)
        pal.pack(fill="x", pady=6)
        self.pal_entries = {}
        keys = ["accent", "accent_dim", "glow", "text", "text_dim", "bg"]
        for i, k in enumerate(keys):
            ttk.Label(pal, text=k).grid(row=i // 2, column=(i % 2) * 3, sticky="w", padx=2)
            e = ttk.Entry(pal, width=8); e.grid(row=i // 2, column=(i % 2) * 3 + 1, padx=2, pady=2)
            e.insert(0, self.palette[k])
            self.pal_entries[k] = e
            ttk.Button(pal, text="...", width=3,
                       command=lambda kk=k: self.pick_color(kk)).grid(
                       row=i // 2, column=(i % 2) * 3 + 2, padx=2)

        # glow
        gl = ttk.LabelFrame(frm, text="Glow", padding=8)
        gl.pack(fill="x", pady=6)
        self.glow_on = tk.IntVar(value=1)
        ttk.Checkbutton(gl, text="Enabled", variable=self.glow_on).grid(row=0, column=0, sticky="w")
        ttk.Label(gl, text="Color").grid(row=0, column=1, padx=4)
        self.e_glow = ttk.Entry(gl, width=8); self.e_glow.grid(row=0, column=2)
        self.e_glow.insert(0, "AEFF3C")
        ttk.Button(gl, text="...", width=3, command=self.pick_glow).grid(row=0, column=3, padx=2)
        ttk.Label(gl, text="Intensity").grid(row=0, column=4, padx=4)
        self.glow_i = tk.IntVar(value=100)
        ttk.Scale(gl, from_=0, to=100, variable=self.glow_i, length=120).grid(row=0, column=5)

        # background toggle
        bg = ttk.Frame(frm); bg.pack(fill="x", pady=4)
        self.use_bg = tk.IntVar(value=0)
        ttk.Checkbutton(bg, text="Use painted background (writes assets/bg.png into the theme)",
                        variable=self.use_bg).pack(side="left")

        # --- bottom action bar, pinned to the BOTTOM of the window so the
        #     buttons are always on screen. Packed before the report box so the
        #     report shrinks when the window is small -- the buttons never do. ---
        try:
            ttk.Style().configure("Build.TButton",
                                   font=("TkDefaultFont", 11, "bold"), padding=(16, 9))
        except Exception:
            pass

        botbar = ttk.Frame(frm)
        botbar.pack(side="bottom", fill="x", pady=(8, 0))

        self.lbl_status = ttk.Label(botbar, text="", foreground="#080",
                                    font=("TkDefaultFont", 10, "bold"))
        self.lbl_status.pack(side="bottom", anchor="w", pady=(6, 0))

        actions = ttk.Frame(botbar)
        actions.pack(side="bottom", fill="x")
        ttk.Button(actions, text="Build Theme  (Ctrl+B)", style="Build.TButton",
                   command=self.do_build).pack(side="left")
        ttk.Button(actions, text="Save theme.ini  (Ctrl+S)",
                   command=self.do_save_ini).pack(side="left", padx=8)
        ttk.Button(actions, text="Validate",
                   command=self.do_validate).pack(side="left")

        # report box fills the space between the form above and the action bar,
        # and is the thing that gives up room when the window is short
        self.txt = tk.Text(frm, height=7, width=80, bg="#0c120c", fg="#bdd",
                           highlightthickness=1, highlightbackground="#333")
        self.txt.pack(side="bottom", fill="both", expand=True, pady=(6, 0))

        # keyboard shortcuts for the two commit actions
        self.root.bind("<Control-b>", lambda _e: self.do_build())
        self.root.bind("<Control-B>", lambda _e: self.do_build())
        self.root.bind("<Control-s>", lambda _e: self.do_save_ini())
        self.root.bind("<Control-S>", lambda _e: self.do_save_ini())

        self.refresh_target()

    # --- helpers ---------------------------------------------------------
    def refresh_target(self):
        folder = safe_folder_name(self.e_name.get())
        if self.location:
            self.lbl_target.config(text="-> %s" % os.path.join(self.location, folder))
        else:
            self.lbl_target.config(text="-> (choose a build location)  folder will be: %s" % folder)

    def choose_location(self):
        d = filedialog.askdirectory(title="Choose where to build the theme folder")
        if not d:
            return
        self.location = d
        self.lbl_loc.config(text=d)
        self.refresh_target()

    def pick_asset(self, slot):
        types = [("Images", "*.png *.jpg *.jpeg *.bmp *.gif *.tga"), ("All files", "*.*")]
        f = filedialog.askopenfilename(title="Choose art for '%s'" % slot, filetypes=types)
        if not f:
            return
        self.asset_src[slot] = f
        self.asset_lbls[slot].config(text=os.path.basename(f), foreground="#0a0")
        if slot == "bg":
            self.use_bg.set(1)

    def pick_color(self, key):
        c = colorchooser.askcolor()[1]
        if c:
            self.pal_entries[key].delete(0, "end")
            self.pal_entries[key].insert(0, c.lstrip("#").upper())

    def pick_glow(self):
        c = colorchooser.askcolor()[1]
        if c:
            self.e_glow.delete(0, "end")
            self.e_glow.insert(0, c.lstrip("#").upper())

    def _palette(self):
        return {k: self.pal_entries[k].get().strip().lstrip("#").upper()
                for k in self.pal_entries}

    # --- actions ---------------------------------------------------------
    def do_build(self):
        if not self.location:
            messagebox.showwarning("No location", "Choose a build location first.")
            return
        if not self.e_name.get().strip():
            messagebox.showwarning("No name", "Enter a theme name.")
            return
        try:
            theme_dir, report, ok = build_theme(
                self.location, self.e_name.get(), self.e_auth.get(),
                self._palette(), self.glow_on.get(),
                self.e_glow.get().strip().lstrip("#").upper(),
                int(self.glow_i.get()), self.use_bg.get(),
                self.asset_src, bool(self.resize_on.get()))
        except Exception as e:
            messagebox.showerror("Build failed", str(e))
            return
        self.txt.delete("1.0", "end")
        self.txt.insert("end", "\n".join(report) + "\n")
        if ok:
            self.lbl_status.config(text="Built OK -> " + theme_dir, foreground="#080")
        else:
            self.lbl_status.config(text="Built with missing required slots -- see report",
                                   foreground="#a60")

    def do_save_ini(self):
        """Quick-save: (re)write theme.ini into the target folder without touching
        imported art -- handy for tweaking palette/glow/background on a built theme."""
        if not self.location:
            messagebox.showwarning("No location", "Choose a build location first.")
            return
        if not self.e_name.get().strip():
            messagebox.showwarning("No name", "Enter a theme name.")
            return
        target = os.path.join(self.location, safe_folder_name(self.e_name.get()))
        try:
            os.makedirs(os.path.join(target, "assets", "raw"), exist_ok=True)
            has_bg = os.path.isfile(os.path.join(target, "assets", "bg.png"))
            p = write_ini(target, self.e_name.get(), self.e_auth.get(),
                          self._palette(), self.glow_on.get(),
                          self.e_glow.get().strip().lstrip("#").upper(),
                          int(self.glow_i.get()),
                          bool(self.use_bg.get()) and has_bg)
            self.lbl_status.config(text="Saved " + p, foreground="#080")
        except Exception as e:
            messagebox.showerror("Save failed", str(e))

    def do_validate(self):
        self.txt.delete("1.0", "end")
        folder = safe_folder_name(self.e_name.get())
        target = os.path.join(self.location, folder) if self.location else None
        if not target or not os.path.isdir(target):
            self.txt.insert("end", "Build the theme first (or set a location whose "
                                   "folder already exists).\n")
            return
        lines, ok = validate(target)
        self.txt.insert("end", "Asset slots in %s:\n" % target)
        self.txt.insert("end", "\n".join(lines))
        self.txt.insert("end", "\n\n" + ("All required slots present.\n" if ok
                                         else "Missing required slots -- theme may render with gaps.\n"))


def main():
    # headless: theme_builder.py <theme_dir>   (validate only)
    if len(sys.argv) == 2:
        lines, ok = validate(sys.argv[1])
        print("\n".join(lines))
        print("OK" if ok else "MISSING REQUIRED SLOTS")
        return
    if not HAVE_TK:
        print("Usage (headless validate): theme_builder.py <theme_dir>")
        print("The interactive builder needs Tk (python3-tk).")
        return
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()