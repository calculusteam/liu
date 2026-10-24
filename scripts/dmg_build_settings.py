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
# dmgbuild is used rather than Finder/AppleScript because the AppleScript
# `background picture` property is broken on recent macOS (Tahoe / 26.x): icon
# positions apply but the background is silently dropped. dmgbuild writes the
# .DS_Store (background, window, icon layout) directly, so it works headless and
# on modern macOS. Geometry must match the 640x400 canvas the art is drawn at
# (see scripts/dmg_render_background.sh / assets/dmg/background.html).
import os

# Monkeypatch ds_store to force a dark Finder background matching the art.
# Icon label TEXT COLOR is NOT set here — modern macOS (Big Sur+/Tahoe) ignores
# .DS_Store textColor* keys; labels are instead baked into the background art
# (see assets/dmg/background.html) so they stay readable regardless of system
# appearance.
try:
    from ds_store import DSStore
    orig_setitem = DSStore.Partial.__setitem__
    def patched_setitem(self, code, value):
        code_str = code.decode('latin_1') if isinstance(code, bytes) else code
        # dmgbuild also stores the background as a bookmark (pBBk) holding a
        # volume-relative path. Finder on macOS 26 reads that bookmark first,
        # cannot resolve it, and then draws the window with neither the
        # picture nor the colour from icvp. Without it Finder resolves
        # icvp's backgroundImageAlias, which points at the same file.
        if self._filename == "." and code_str == "pBBk":
            return
        if self._filename == "." and code_str == "icvp" and isinstance(value, dict):
            value["backgroundColorRed"]   = 10 / 255   # #0A0A0B, the window colour behind the art
            value["backgroundColorGreen"] = 10 / 255
            value["backgroundColorBlue"]  = 11 / 255
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
