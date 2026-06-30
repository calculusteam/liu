#!/usr/bin/env bash
#
# Launch the freshly-built dev binary the way macOS wants for privacy-sensitive
# features (microphone, camera, …).
#
# Running ./build/Liu straight from a terminal makes TCC charge the microphone
# request to the *parent* process (the shell / terminal app), so the mic shows
# up as "unavailable" and the permission prompt never appears for Liu. Wrapping
# the binary in a minimal .app bundle and launching it through `open` (launchd)
# makes Liu its own "responsible process", so the system mic prompt appears the
# first time the user clicks Record, and the grant sticks to Liu.
#
# Usage: scripts/run_dev_macos.sh [build-dir]   (default: ./build)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
BIN="$BUILD/Liu"
APP="$BUILD/Liu.app"

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found — build first (cmake --build $BUILD)" >&2
    exit 1
fi

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp "$BIN" "$APP/Contents/MacOS/Liu"
# Metal loader resolves the shader lib next to the executable (Contents/MacOS).
if [[ -f "$BUILD/LiuShaders.metallib" ]]; then
    cp "$BUILD/LiuShaders.metallib" "$APP/Contents/MacOS/"
fi
# Bundle the assets too so a bundled run is self-contained.
if [[ -d "$ROOT/assets" ]]; then
    cp -R "$ROOT/assets" "$APP/Contents/assets"
fi

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>Liu</string>
    <key>CFBundleIdentifier</key>
    <string>com.github.calculusteam.Liu</string>
    <key>CFBundleName</key>
    <string>Liu</string>
    <key>CFBundleDisplayName</key>
    <string>Liu</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>LSMinimumSystemVersion</key>
    <string>11.0</string>
    <key>NSMicrophoneUsageDescription</key>
    <string>Liu records short audio clips you choose as custom notification sounds.</string>
</dict>
</plist>
PLIST

# Ad-hoc sign so the bundle's Info.plist binds to the code signature and there's
# a stable identifier for TCC. --deep is required because the bundled
# LiuShaders.metallib is a nested code object that must be signed too. (Real
# distribution should re-sign with a Developer ID + notarize — see
# scripts/package_macos_dmg.sh.)
codesign --force --deep --sign - --identifier com.github.calculusteam.Liu "$APP" >/dev/null 2>&1 || \
    echo "warning: codesign failed; mic permission prompt may not appear" >&2

exec open "$APP"
