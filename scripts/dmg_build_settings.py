# dmgbuild settings for the styled Liu installer DMG.
#
# Clean layout — no text in the background art. The DMG volume name ("Liu")
# serves as the window title, and Finder draws its own icon labels.
#
# Driven by scripts/package_macos_dmg.sh, which exports:
#   APP_PATH  — path to the staged, signed Liu.app
#   BG        — path to assets/dmg/background.tiff (HiDPI 1x/2x pair)
#   VOLICON   — path to the volume .icns (optional)
#
# We use dmgbuild rather than Finder/AppleScript because the AppleScript
# `background picture` property is broken on recent macOS (Tahoe / 26.x): icon
# positions apply but the background is silently dropped. dmgbuild writes the
# .DS_Store (background, window, icon layout) directly, so it works headless and
# on modern macOS. Geometry must match the 640x400 canvas the art is drawn at
# (see scripts/dmg_render_background.sh / assets/dmg/background.html).
import os

# Monkeypatch ds_store to force white text labels on the dark background
try:
    from ds_store import DSStore
    orig_setitem = DSStore.Partial.__setitem__
    def patched_setitem(self, code, value):
        code_str = code.decode('latin_1') if isinstance(code, bytes) else code
        if self._filename == "." and code_str == "icvp" and isinstance(value, dict):
            # Set background color to dark (#121212 / 255 = 0.0706)
            value["backgroundColorRed"] = 0.0706
            value["backgroundColorGreen"] = 0.0706
            value["backgroundColorBlue"] = 0.0706
        orig_setitem(self, code, value)
    DSStore.Partial.__setitem__ = patched_setitem
except Exception:
    pass

app_path = os.environ['APP_PATH']
appname  = os.path.basename(app_path)        # "Liu.app"

# --- contents ---------------------------------------------------------------
files = [app_path]
symlinks = {'Applications': '/Applications'}

# --- volume icon ------------------------------------------------------------
_volicon = os.environ.get('VOLICON')
if _volicon and os.path.exists(_volicon):
    icon = _volicon

# --- window / view ----------------------------------------------------------
background     = os.environ['BG']
window_rect    = ((220, 140), (640, 400))    # ((x, y), (content w, h))
default_view   = 'icon-view'
icon_size      = 112
text_size      = 12
show_icon_preview = False
include_icon_view_settings = 'auto'
include_list_view_settings = 'auto'

# Icon centers, in content-area points (origin top-left) — aligned with the
# arrow drawn in the background art (y=198, app at x=160, drop target at x=480).
icon_locations = {
    appname:        (160, 198),
    'Applications': (480, 198),
}

# --- image ------------------------------------------------------------------
format = 'UDZO'                              # compressed, read-only
