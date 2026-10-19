#!/usr/bin/env python3
"""DarkDash Theme Studio — PySide6 theme builder and validator.

Open an existing theme.ini or create a new theme folder. Edit lighting, material,
colors, and named assets without deleting legacy theme configuration. Artwork
preview is an image preview, not a simulated Xbox renderer.

Install: pip install PySide6 pillow
Run:     python theme_builder.py
Validate without the GUI: python theme_builder.py /path/to/theme
"""

import os
import sys
import shutil
import re

try:
    from PIL import Image
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False

# Named artwork dimensions from bundled Default theme; optional, with renderer fallbacks.
SLOTS = [
    ('bar_header', 315, 44, False),
    ('bar_footer', 462, 56, False),
    ('frame_menu_v', 308, 430, False),
    ('frame_menu_h', 486, 189, False),
    ('frame_menu_h_selected', 491, 190, False),
    ('frame_card_blank', 174, 247, False),
    ('frame_card_small', 202, 103, False),
    ('frame_featured', 449, 204, False),
    ('frame_library', 455, 201, False),
    ('frame_media', 338, 187, False),
    ('frame_network', 388, 193, False),
    ('frame_nowplaying', 355, 142, False),
    ('frame_recent', 366, 154, False),
    ('frame_status', 268, 185, False),
    ('frame_storage', 285, 194, False),
    ('card_recent', 261, 186, False),
    ('orb_hero', 388, 334, False),
    ('orb_small', 176, 184, False),
    ('orb_glow', 272, 234, False),
    ('globe', 256, 251, False),
    ('platform_round', 336, 190, False),
    ('platform_long', 452, 144, False),
    ('tab_h_normal', 323, 59, False),
    ('tab_h_selected', 323, 64, False),
    ('tab_v_normal', 266, 279, False),
    ('tab_v_selected', 266, 279, False),
    ('overlay_selection_glow', 423, 109, False),
    ('panel_loading', 286, 94, False),
    ('plaque_datetime', 178, 83, False),
    ('plaque_freespace', 218, 84, False),
    ('plaque_info', 239, 92, False),
    ('plaque_signedin', 222, 89, False),
    ('plaque_toast', 291, 80, False),
    ('tile_cover_small_blank', 164, 247, False),
    ('tile_cover_medium_blank', 200, 269, False),
    ('tile_feature_blank', 354, 277, False),
    ('module_media', 243, 199, False),
    ('module_status', 232, 196, False),
    ('module_storage', 258, 206, False),
    ('icon_games', 104, 115, False),
    ('icon_apps', 105, 114, False),
    ('icon_media', 112, 113, False),
    ('icon_network', 113, 113, False),
    ('icon_saves', 114, 114, False),
    ('icon_settings', 115, 113, False),
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

# Renderer-supported 3D stage parameters. Optional tint colors inherit accent.
LIGHTING_PROFILES = {
    "liquid": {"emissive_level": 100, "specular_level": 100, "arc_level": 100,
               "motion_level": 100, "pulse_level": 100, "specular_power": 20},
    "hologram": {"emissive_level": 155, "specular_level": 45, "arc_level": 100,
                 "motion_level": 65, "pulse_level": 100, "specular_power": 30},
    "metal": {"emissive_level": 45, "specular_level": 145, "arc_level": 35,
              "motion_level": 55, "pulse_level": 40, "specular_power": 46},
}
LIGHTING_COLORS = [
    ("ambient", "Ambient light"), ("key", "Main light"),
    ("emissive", "Orb emission"), ("beam", "Platform beam"),
    ("arcs", "Energy arcs"),
]
LIGHTING_LEVELS = [
    ("ambient_level", "Ambient strength", 0, 200),
    ("key_level", "Main light strength", 0, 200),
    ("emissive_level", "Emission strength", 0, 200),
    ("specular_level", "Reflections", 0, 200),
    ("beam_level", "Beam strength", 0, 200),
    ("arc_level", "Arc intensity", 0, 200),
    ("motion_level", "Motion", 0, 200),
    ("pulse_level", "Pulse", 0, 200),
    ("specular_power", "Reflection sharpness", 2, 96),
]


def default_lighting(material="liquid"):
    material = material if material in LIGHTING_PROFILES else "liquid"
    settings = {"material": material, "ambient": "303030", "key": "FFFFFF",
                "emissive": "", "beam": "", "arcs": "",
                "ambient_level": 100, "key_level": 100, "beam_level": 100}
    settings.update(LIGHTING_PROFILES[material])
    return settings


def validate_colors(palette, glow_color, lighting=None):
    lighting = lighting or default_lighting()
    for key, val in palette.items():
        if not re.fullmatch(r"[0-9A-Fa-f]{6}", str(val)):
            raise ValueError("Palette %s needs six hexadecimal digits (RRGGBB)." % key)
    if not re.fullmatch(r"[0-9A-Fa-f]{6}", str(glow_color)):
        raise ValueError("Glow color needs six hexadecimal digits (RRGGBB).")
    if lighting.get("material", "liquid") not in LIGHTING_PROFILES:
        raise ValueError("Select a supported 3D material style.")
    for key, _ in LIGHTING_COLORS:
        val = str(lighting.get(key, "")).strip().lstrip("#")
        if val and not re.fullmatch(r"[0-9A-Fa-f]{6}", val):
            raise ValueError("%s needs six hexadecimal digits or an empty value." % key)
    for key, _, low, high in LIGHTING_LEVELS:
        try:
            raw = str(lighting.get(key, default_lighting(lighting.get("material"))[key]))
            if not re.fullmatch(r"\d+", raw):
                raise ValueError()
            val = int(raw)
        except (ValueError, TypeError):
            raise ValueError("%s needs a whole number." % key.replace("_", " "))
        if not low <= val <= high:
            raise ValueError("%s must be between %d and %d." % (key.replace("_", " "), low, high))


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
                glow_intensity, use_bg, asset_src, do_resize, lighting=None,
                theme_dir_override=None, update_background=True):
    """Create <location>/<safe_name>/ with assets/ (+ raw/), import the chosen
    art into slot files, and write theme.ini.

    asset_src: dict slot_name -> source image path (or missing/None to skip).
               include key "bg" for the painted background.
    Returns (theme_dir, report_lines, ok).
    """
    report = []
    ok = True

    folder = safe_folder_name(name)
    theme_dir = theme_dir_override or os.path.join(location, folder)
    prior = parse_ini_text(read_ini_text(os.path.join(theme_dir, "theme.ini"))[0])
    asset_dir = prior.get("manifest", {}).get("asset_dir", "assets").strip() or "assets"
    if os.path.isabs(asset_dir) or ".." in asset_dir.replace("\\", "/").split("/"):
        raise ValueError("Invalid theme asset directory")
    assets = os.path.join(theme_dir, asset_dir)
    raw = os.path.join(assets, "raw")

    os.makedirs(raw, exist_ok=True)   # creates theme_dir, assets, raw in one go
    report.append("Folder: %s" % theme_dir)
    report.append("  assets/        created")
    report.append("  assets/raw/    created (icon overrides)")
    report.append("")

    # Import selected named assets, preserve unselected existing artwork.
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
                    glow_intensity, use_bg and have_bg, lighting,
                    update_background=update_background)
    report.append("")
    report.append("Wrote %s" % ini)
    return (theme_dir, report, ok)


def validate(theme_dir):
    """Return (lines, ok). lines = human-readable per-slot report."""
    prior = parse_ini_text(read_ini_text(os.path.join(theme_dir, "theme.ini"))[0])
    asset_dir = prior.get("manifest", {}).get("asset_dir", "assets").strip() or "assets"
    if os.path.isabs(asset_dir) or ".." in asset_dir.replace("\\", "/").split("/"):
        return ["ERROR: invalid theme asset directory"], False
    assets = os.path.join(theme_dir, asset_dir)
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


# ---------------------------------------------------------------------------
# Lossless INI editing and GUI
# ---------------------------------------------------------------------------


def read_ini_text(path):
    """Return (text, encoding, newline); preserve original encoding/line endings."""
    if not os.path.isfile(path):
        return "", "utf-8", "\n"
    content = open(path, "rb").read()
    if content.startswith(b"\xef\xbb\xbf"):
        encoding = "utf-8-sig"
    else:
        try:
            content.decode("utf-8")
            encoding = "utf-8"
        except UnicodeDecodeError:
            encoding = "cp1252"
    text = content.decode(encoding)
    newline = "\r\n" if b"\r\n" in content else "\n"
    return text, encoding, newline


def parse_ini_text(text):
    """Small read-only parser; all comments and unknown keys survive writes."""
    data = {}
    section = None
    for line in text.splitlines():
        raw = line.strip()
        if not raw or raw[0] in ";#":
            continue
        match = re.match(r"^\[([^]]+)\]", raw)
        if match:
            section = match.group(1).lower()
            data.setdefault(section, {})
            continue
        if section and "=" in line:
            key, value = line.split("=", 1)
            key = key.strip().lower()
            if key:
                data[section][key] = value.split(";", 1)[0].strip()
    return data


def _patch_section(lines, section_name, keyvalues, newline):
    """Modify only requested keys; unknown content, comments and order remain."""
    section_re = re.compile(r"^\s*\[([^]]+)\]\s*(?:[;#].*)?$")
    begin = end = None
    for i, line in enumerate(lines):
        m = section_re.match(line.rstrip("\r\n"))
        if m and m.group(1).strip().lower() == section_name.lower():
            begin = i + 1
            break
    if begin is None:
        if all(v is None for v in keyvalues.values()):
            return
        if lines and lines[-1].strip():
            lines.append(newline)
        lines.append("[%s]%s" % (section_name, newline))
        for key, val in keyvalues.items():
            if val is not None:
                lines.append("%-19s = %s%s" % (key, val, newline))
        return
    end = len(lines)
    for i in range(begin, len(lines)):
        if section_re.match(lines[i].rstrip("\r\n")):
            end = i
            break
    # Process each known key without dropping any other settings.
    for key, val in keyvalues.items():
        entry_re = re.compile(r"^(\s*" + re.escape(key) + r"\s*=\s*)([^;\r\n]*)([;].*)?$", re.I)
        matched = False
        for i in range(begin, end):
            text = lines[i].rstrip("\r\n")
            m = entry_re.match(text)
            if not m:
                continue
            matched = True
            if val is None:
                del lines[i]
                end -= 1
            else:
                # Keep the original inline comment and surrounding layout.
                before = m.group(1)
                original_value = m.group(2)
                comment = m.group(3) or ""
                spacing = original_value[len(original_value.rstrip()):]
                lines[i] = "%s%s%s%s%s" % (before, val, spacing, comment, newline)
            break
        if not matched and val is not None:
            lines.insert(end, "%-19s = %s%s" % (key, val, newline))
            end += 1


def update_ini_text(original, values, newline="\n"):
    lines = original.splitlines(keepends=True)
    if lines and not lines[-1].endswith(("\n", "\r")):
        lines[-1] += newline
    if not lines:
        lines = ["; DarkDash theme -- generated by theme_builder.py" + newline, newline]
    for section, props in values.items():
        _patch_section(lines, section, props, newline)
    return "".join(lines)


def write_ini(theme_dir, name, author, palette, glow_on, glow_color,
              glow_intensity, use_bg, lighting=None, update_background=True):
    """Save only supported settings; retain every unrelated theme.ini entry."""
    lighting = dict(default_lighting(), **(lighting or {}))
    validate_colors(palette, glow_color, lighting)
    if any(c in name for c in "\r\n=;") or any(c in author for c in "\r\n=;"):
        raise ValueError("Theme name and author cannot include line breaks, = or ;.")
    p = os.path.join(theme_dir, "theme.ini")
    old, encoding, newline = read_ini_text(p)
    old_config = parse_ini_text(old)
    manifest = {"name": name, "author": author}
    if "manifest" not in old_config:
        manifest.update({"schema_version": "1", "base_resolution": "640x480",
                         "asset_dir": "assets"})
    updates = {
        "manifest": manifest,
        "palette": {k: str(palette[k]).upper() for k in DEFAULT_PALETTE},
        "glow": {"enabled": str(int(bool(glow_on))),
                 "color": glow_color.upper(), "intensity": str(int(glow_intensity))},
        "stage.lighting": {"material": lighting["material"]},
    }
    # Do not discard unrelated background keys like parallax. Existing
    # custom filenames are preserved if they still point to existing art.
    background = old_config.get("background", {})
    bg_image = background.get("image", "").strip()
    if update_background:
        if use_bg:
            if not bg_image or bg_image.lower() == "none":
                bg_image = (old_config.get("manifest", {}).get("asset_dir", "assets").strip() or "assets") + "/bg.png"
            updates["background"] = {"module": "static", "image": bg_image}
        else:
            updates["background"] = {"module": "none"}
    for key, _ in LIGHTING_COLORS:
        color = str(lighting.get(key, "")).strip().lstrip("#").upper()
        updates["stage.lighting"][key] = color or None
    for key, _, _, _ in LIGHTING_LEVELS:
        updates["stage.lighting"][key] = str(int(lighting[key]))
    # Optional flat-sprite emissive mask; never remove another [assets] role.
    asset_dir = old_config.get("manifest", {}).get("asset_dir", "assets").strip() or "assets"
    mask_file = os.path.join(theme_dir, asset_dir, "orb_glow.png")
    if os.path.isfile(mask_file):
        updates["assets"] = {"stage.orb.glow": "orb_glow"}

    output = update_ini_text(old, updates, newline)
    with open(p, "wb") as f:
        f.write(output.encode(encoding))
    return p



# ---------------------------------------------------------------------------
# PySide6 desktop interface
# ---------------------------------------------------------------------------

try:
    from PySide6.QtCore import Qt, QSize
    from PySide6.QtGui import QColor, QPixmap
    from PySide6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QGridLayout, QLabel, QLineEdit, QPushButton, QFileDialog, QMessageBox,
        QTabWidget, QGroupBox, QScrollArea, QCheckBox, QComboBox, QSpinBox,
        QPlainTextEdit, QColorDialog, QSplitter, QFrame, QTreeWidget,
        QTreeWidgetItem, QSlider, QFormLayout
    )
    HAVE_QT = True
except ImportError:
    HAVE_QT = False

STYLE = """
QWidget { background: #141821; color: #e6e9f0; font-size: 12px; }
QMainWindow { background: #10131b; }
QGroupBox { border: 1px solid #353e50; border-radius: 9px; margin-top: 13px; padding: 15px 9px 9px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; left: 11px; padding: 0 6px; color: #c5afea; }
QLineEdit, QSpinBox, QComboBox, QPlainTextEdit, QTreeWidget { background:#1c2330; border:1px solid #374153; border-radius:6px; padding:6px; }
QLineEdit:focus,QSpinBox:focus,QComboBox:focus,QTreeWidget:focus { border:1px solid #a976de; }
QPushButton { background:#303a4e; border:1px solid #46526b; border-radius:7px; padding:8px 12px; }
QPushButton:hover { background:#465576; } QPushButton:pressed { background:#5a6083; }
QPushButton#primary { background:#65439a; border-color:#8964c1; font-weight:600; }
QTabWidget::pane { border:1px solid #343d50; border-radius:8px; }
QTabBar::tab { padding:10px 17px; background:#1e2532; border-radius:6px; margin:3px; }
QTabBar::tab:selected { background:#554076; }
QCheckBox { spacing:8px; }
QScrollArea { border:none; }
QSlider::groove:horizontal { height:5px; background:#3f4759; border-radius:2px; }
QSlider::handle:horizontal { width:13px; margin:-5px 0; background:#be94ee; border-radius:6px; }
"""


def safe_img(path):
    try:
        return QPixmap(path) if os.path.isfile(path) else QPixmap()
    except Exception:
        return QPixmap()


class ThemeWindow(QMainWindow if HAVE_QT else object):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("DarkDash Theme Builder")
        self.resize(1200, 820)
        self.setMinimumSize(930, 630)
        self.location = None
        self.edit_dir = None
        self.loaded_bg_choice = None
        self.asset_src = {}
        self.asset_dir = "assets"
        self.slot_items = {}
        self.palette_entries = {}
        self.light_colors = {}
        self.light_levels = {}
        self.setStyleSheet(STYLE)
        root = QWidget(); self.setCentralWidget(root)
        page = QVBoxLayout(root); page.setContentsMargins(17, 13, 17, 12)
        head = QHBoxLayout()
        title = QLabel("DarkDash  /  Theme Studio"); title.setStyleSheet("font-size:20px;font-weight:700;color:#f1e5ff")
        head.addWidget(title); head.addStretch()
        for label, action in (("New", self.new_theme),("Open Theme", self.open_existing),("Build Location", self.choose_location)):
            btn=QPushButton(label); btn.clicked.connect(action); head.addWidget(btn)
        page.addLayout(head)
        bar=QHBoxLayout(); self.name=QLineEdit("My Theme"); self.author=QLineEdit()
        bar.addWidget(QLabel("Theme name")); bar.addWidget(self.name,3)
        bar.addWidget(QLabel("Author")); bar.addWidget(self.author,2)
        page.addLayout(bar)
        self.path_label=QLabel("Choose a build location or open an existing theme")
        self.path_label.setStyleSheet("color:#a1acc2")
        page.addWidget(self.path_label)
        self.name.textChanged.connect(self.refresh_path)
        self.tabs=QTabWidget(); page.addWidget(self.tabs,1)
        self.build_artwork_tab(); self.build_colors_tab(); self.build_stage_tab()
        self.report=QPlainTextEdit(); self.report.setReadOnly(True); self.report.setMaximumHeight(105)
        self.report.setPlaceholderText("Build and validation results appear here")
        page.addWidget(self.report)
        foot=QHBoxLayout()
        self.status=QLabel("Ready"); self.status.setStyleSheet("color:#a8b8ca")
        foot.addWidget(self.status,1)
        for label,func in (("Validate",self.do_validate),("Save Settings",self.do_save_ini),("Build Theme",self.do_build)):
            btn=QPushButton(label); btn.clicked.connect(func)
            if label=="Build Theme":btn.setObjectName("primary")
            foot.addWidget(btn)
        page.addLayout(foot)

    def area(self):
        outer=QScrollArea();outer.setWidgetResizable(True)
        body=QWidget(); lay=QVBoxLayout(body);lay.setContentsMargins(14,14,14,14);lay.setSpacing(12)
        outer.setWidget(body);return outer,lay

    def build_artwork_tab(self):
        tab=QWidget();layout=QHBoxLayout(tab)
        left=QVBoxLayout();layout.addLayout(left,3)
        info=QLabel("Select a named asset to preview or replace it. All artwork is optional; missing slots use dashboard fallbacks.")
        info.setWordWrap(True); info.setStyleSheet("color:#aeb8ce")
        left.addWidget(info)
        self.asset_tree=QTreeWidget();self.asset_tree.setHeaderLabels(["Asset", "Reference", "Status"])
        self.asset_tree.setColumnWidth(0,190);self.asset_tree.setColumnWidth(1,100)
        groups={}
        for name,w,h,_ in SLOTS+[('bg',640,480,False)]:
            category = ('Icons' if name.startswith('icon_') else 'Decorative frames' if name.startswith(('frame_','card_','tile_','tab_')) else 'Stage' if name.startswith(('orb_','globe','platform')) else 'Panels and plaques')
            group=groups.get(category)
            if group is None:
                group=QTreeWidgetItem([category]);self.asset_tree.addTopLevelItem(group);groups[category]=group
            item=QTreeWidgetItem([name,f'{w} × {h}','Not assigned']);group.addChild(item)
            self.slot_items[name]=item
        for group in groups.values():group.setExpanded(True)
        left.addWidget(self.asset_tree,1)
        options=QHBoxLayout();self.resize_art=QCheckBox("Resize imports to reference size (may distort)")
        options.addWidget(self.resize_art);left.addLayout(options)
        right=QVBoxLayout();layout.addLayout(right,2)
        self.preview=QLabel("Select an asset to preview")
        self.preview.setMinimumSize(260,280);self.preview.setAlignment(Qt.AlignCenter)
        self.preview.setStyleSheet("background:#0b1019;border:1px solid #394253;border-radius:9px")
        right.addWidget(self.preview,1)
        self.preview_info=QLabel("Artwork preview · alpha shown against dark background")
        self.preview_info.setWordWrap(True);right.addWidget(self.preview_info)
        buttons=QHBoxLayout()
        for label,fn in (("Choose Image",self.pick_asset),("Clear Assignment",self.clear_asset)):
            b=QPushButton(label);b.clicked.connect(fn);buttons.addWidget(b)
        right.addLayout(buttons)
        self.use_bg=QCheckBox("Use painted background when available");right.addWidget(self.use_bg)
        self.asset_tree.currentItemChanged.connect(lambda *_:self.update_preview())
        self.tabs.addTab(tab,"Artwork & Assets")

    def build_colors_tab(self):
        tab,layout=self.area()
        group=QGroupBox("Theme palette");grid=QGridLayout(group)
        for i,(key,default) in enumerate(DEFAULT_PALETTE.items()):
            field=QLineEdit(default);field.setMaxLength(7);self.palette_entries[key]=field
            button=QPushButton("Pick");button.clicked.connect(lambda _,e=field:self.pick_color(e))
            row=i//2;col=(i%2)*3
            grid.addWidget(QLabel(key.replace('_',' ').title()),row,col)
            grid.addWidget(field,row,col+1);grid.addWidget(button,row,col+2)
        layout.addWidget(group)
        glow=QGroupBox("Glow and background");g=QGridLayout(glow)
        self.glow_on=QCheckBox("Enable glow");self.glow_on.setChecked(True)
        self.glow_color=QLineEdit(DEFAULT_PALETTE['glow']);self.glow_amount=QSpinBox();self.glow_amount.setRange(0,100);self.glow_amount.setValue(100)
        g.addWidget(self.glow_on,0,0);g.addWidget(QLabel('Glow color'),0,1);g.addWidget(self.glow_color,0,2)
        b=QPushButton('Pick');b.clicked.connect(lambda:self.pick_color(self.glow_color));g.addWidget(b,0,3)
        g.addWidget(QLabel('Glow intensity'),1,0);g.addWidget(self.glow_amount,1,1)
        layout.addWidget(glow);layout.addStretch()
        self.tabs.addTab(tab,"Colors & Effects")

    def build_stage_tab(self):
        tab,layout=self.area()
        mat=QGroupBox("3D material");line=QHBoxLayout(mat)
        self.material=QComboBox();self.material.addItems(['liquid','hologram','metal'])
        line.addWidget(QLabel('Material style'));line.addWidget(self.material,1)
        reset=QPushButton('Use Material Defaults');reset.clicked.connect(self.reset_stage);line.addWidget(reset)
        layout.addWidget(mat)
        colors=QGroupBox('Lighting colors · blank inherits theme palette');grid=QGridLayout(colors)
        for i,(key,label) in enumerate(LIGHTING_COLORS):
            field=QLineEdit(default_lighting()[key]);self.light_colors[key]=field
            b=QPushButton('Pick');b.clicked.connect(lambda _,e=field:self.pick_color(e))
            grid.addWidget(QLabel(label),i,0);grid.addWidget(field,i,1);grid.addWidget(b,i,2)
        layout.addWidget(colors)
        levels=QGroupBox('Light, reflection and motion');grid=QGridLayout(levels)
        for i,(key,label,minimum,maximum) in enumerate(LIGHTING_LEVELS):
            control=QSpinBox();control.setRange(minimum,maximum);control.setValue(default_lighting()[key]);self.light_levels[key]=control
            grid.addWidget(QLabel(label),i,0);grid.addWidget(control,i,1)
        layout.addWidget(levels);layout.addStretch()
        self.tabs.addTab(tab,"3D Stage")

    def reset_stage(self):
        defaults=default_lighting(self.material.currentText())
        for key,field in self.light_colors.items():field.setText(str(defaults[key]))
        for key,field in self.light_levels.items():field.setValue(defaults[key])

    def refresh_path(self):
        dest=self.target()
        self.path_label.setText(dest or 'Choose a build location or open an existing theme')

    def target(self):
        if self.edit_dir:return self.edit_dir
        return os.path.join(self.location,safe_folder_name(self.name.text())) if self.location else None

    def choose_location(self):
        directory=QFileDialog.getExistingDirectory(self,'Choose build location',self.location or os.getcwd())
        if directory:self.location=directory;self.edit_dir=None;self.refresh_path()

    def open_existing(self):
        folder=QFileDialog.getExistingDirectory(self,'Open theme folder',self.edit_dir or self.location or os.getcwd())
        if not folder:return
        ini=os.path.join(folder,'theme.ini')
        if not os.path.isfile(ini):QMessageBox.warning(self,'Not a theme','The selected folder has no theme.ini');return
        data=parse_ini_text(read_ini_text(ini)[0]);meta=data.get('manifest',{})
        self.name.setText(meta.get('name') or os.path.basename(folder));self.author.setText(meta.get('author',''))
        self.asset_dir=meta.get('asset_dir','assets').strip() or 'assets'
        for key,field in self.palette_entries.items():field.setText(data.get('palette',{}).get(key,DEFAULT_PALETTE[key]))
        glow=data.get('glow',{});self.glow_on.setChecked(glow.get('enabled','1')!='0')
        self.glow_color.setText(glow.get('color',DEFAULT_PALETTE['glow']))
        try:self.glow_amount.setValue(int(glow.get('intensity','100')))
        except ValueError:self.glow_amount.setValue(100)
        self.use_bg.setChecked(data.get('background',{}).get('module','none').lower()=='static')
        self.loaded_bg_choice=self.use_bg.isChecked()
        light=data.get('stage.lighting',{});material=light.get('material','liquid').lower()
        if material not in LIGHTING_PROFILES:material='liquid'
        self.material.setCurrentText(material);defaults=default_lighting(material)
        for key,field in self.light_colors.items():field.setText(light.get(key,defaults[key]))
        for key,field in self.light_levels.items():
            try:field.setValue(int(light.get(key,defaults[key])))
            except ValueError:field.setValue(defaults[key])
        self.edit_dir=folder;self.location=os.path.dirname(folder);self.asset_src.clear()
        self.update_asset_status();self.update_preview();self.refresh_path()
        self.status.setText('Theme opened. All unrelated INI settings are preserved on save.')

    def new_theme(self):
        self.edit_dir=None;self.asset_src.clear();self.asset_dir='assets';self.loaded_bg_choice=None
        self.name.setText('My Theme');self.author.clear()
        for key,field in self.palette_entries.items():field.setText(DEFAULT_PALETTE[key])
        self.glow_on.setChecked(True);self.glow_color.setText(DEFAULT_PALETTE['glow'])
        self.glow_amount.setValue(100);self.use_bg.setChecked(False)
        self.material.setCurrentText('liquid');self.reset_stage();self.update_asset_status();self.update_preview();self.refresh_path()
        self.status.setText('New theme')

    def selected_slot(self):
        item=self.asset_tree.currentItem()
        if not item:return None
        for slot,ref in self.slot_items.items():
            if item is ref:return slot
        return None

    def slot_file(self,slot):
        if slot in self.asset_src:return self.asset_src[slot]
        target=self.target()
        if target:
            candidate=os.path.join(target,self.asset_dir,slot+'.png')
            if os.path.isfile(candidate):return candidate
        return None

    def update_asset_status(self):
        for slot,item in self.slot_items.items():
            src=self.slot_file(slot)
            if not src:item.setText(2,'Optional · not set');continue
            if slot in self.asset_src:item.setText(2,'Ready to import')
            else:item.setText(2,'Existing')

    def update_preview(self):
        slot=self.selected_slot()
        if not slot:self.preview.setText('Select an artwork slot');return
        src=self.slot_file(slot)
        pix=safe_img(src) if src else QPixmap()
        if pix.isNull():self.preview.clear();self.preview.setText('No artwork assigned');self.preview_info.setText(slot+' · Optional asset');return
        self.preview.setPixmap(pix.scaled(QSize(365,365),Qt.KeepAspectRatio,Qt.SmoothTransformation))
        self.preview_info.setText(f'{slot}  ·  {pix.width()} × {pix.height()}  ·  {os.path.basename(src)}')

    def pick_asset(self):
        slot=self.selected_slot()
        if not slot:QMessageBox.information(self,'Select slot','Select an artwork slot first.');return
        file,_=QFileDialog.getOpenFileName(self,'Choose artwork','', 'Images (*.png *.jpg *.jpeg *.bmp *.gif *.tga);;All files (*)')
        if file:
            self.asset_src[slot]=file
            if slot=='bg':self.use_bg.setChecked(True)
            self.update_asset_status();self.update_preview()

    def clear_asset(self):
        slot=self.selected_slot()
        if slot and slot in self.asset_src:
            del self.asset_src[slot]
            self.update_asset_status();self.update_preview()

    def pick_color(self,field):
        color=QColorDialog.getColor(QColor('#'+(field.text().strip().lstrip('#') or 'FFFFFF')),self,'Choose color')
        if color.isValid():field.setText(color.name()[1:].upper())

    def settings(self):
        palette={k:field.text().strip().lstrip('#').upper() for k,field in self.palette_entries.items()}
        lighting={'material':self.material.currentText()}
        lighting.update({k:e.text().strip().lstrip('#').upper() for k,e in self.light_colors.items()})
        lighting.update({k:v.value() for k,v in self.light_levels.items()})
        glow=self.glow_color.text().strip().lstrip('#').upper()
        validate_colors(palette,glow,lighting)
        if not self.name.text().strip():raise ValueError('Enter a theme name')
        return palette,lighting,glow

    def update_background(self):
        return not (self.edit_dir and 'bg' not in self.asset_src and self.loaded_bg_choice==self.use_bg.isChecked())

    def display_report(self,lines):
        self.report.setPlainText('\n'.join(lines))

    def do_build(self):
        target=self.target()
        if not target:QMessageBox.warning(self,'No location','Choose a build location first');return
        try:
            palette,lighting,glow=self.settings()
            path,lines,ok=build_theme(self.location,self.name.text(),self.author.text(),palette,self.glow_on.isChecked(),glow,self.glow_amount.value(),self.use_bg.isChecked(),self.asset_src,self.resize_art.isChecked(),lighting,theme_dir_override=target,update_background=self.update_background())
            self.display_report(lines)
            self.status.setText(('Built: ' if ok else 'Built with warnings: ')+path)
            self.asset_src.clear();self.update_asset_status();self.update_preview()
        except Exception as e:QMessageBox.critical(self,'Build failed',str(e))

    def do_save_ini(self):
        target=self.target()
        if not target:QMessageBox.warning(self,'No location','Choose a build location first');return
        try:
            palette,lighting,glow=self.settings()
            os.makedirs(target,exist_ok=True)
            existing=parse_ini_text(read_ini_text(os.path.join(target,'theme.ini'))[0])
            current_img=existing.get('background',{}).get('image','')
            bg=os.path.isfile(os.path.join(target,self.asset_dir,'bg.png')) or bool(current_img)
            saved=write_ini(target,self.name.text(),self.author.text(),palette,self.glow_on.isChecked(),glow,self.glow_amount.value(),self.use_bg.isChecked() and bg,lighting,update_background=self.update_background())
            self.status.setText('Settings saved: '+saved)
        except Exception as e:QMessageBox.critical(self,'Save failed',str(e))

    def do_validate(self):
        target=self.target()
        if not target or not os.path.isdir(target):self.display_report(['Open or build a theme first']);return
        lines,ok=validate(target)
        self.display_report(lines)
        self.status.setText('Validation completed' if ok else 'Validation found issues')


def main():
    if len(sys.argv)==2 and os.path.isdir(sys.argv[1]):
        lines,ok=validate(sys.argv[1]);print('\n'.join(lines));return 0 if ok else 1
    if not HAVE_QT:
        print('Install the PySide6 desktop UI with: pip install PySide6 pillow')
        return 1
    app=QApplication(sys.argv);app.setStyle('Fusion')
    window=ThemeWindow();window.show()
    return app.exec()

if __name__=='__main__':
    sys.exit(main())
