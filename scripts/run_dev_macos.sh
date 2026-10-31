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
# Every Metal loader resolves its library next to the executable
# (Contents/MacOS, never Resources). Glob rather than naming LiuShaders: the
# LLM and Whisper backends ship their own metallibs and silently fall back to
# the CPU path when theirs is missing, which is a very quiet way to lose the
# GPU in a dev build.
for _lib in "$BUILD"/*.metallib; do
    [[ -f "$_lib" ]] && cp "$_lib" "$APP/Contents/MacOS/"
done
# The sibling binaries belong beside Liu too. liu-notify especially: on every
# launch Liu rewrites ~/.liu/hooks/liu-notify to exec whatever sits in its OWN
# bundle, and that shim is what all six agent CLIs record. Leave it out and a
# dev run points every agent's hook at a path that does not exist — the shim
# then exits 0 in silence by design, so notifications simply stop, with no
# error anywhere and `liu-notify doctor` still reporting "no stale hooks".
for _bin in liu-notify liu-history; do
    [[ -f "$BUILD/$_bin" ]] && cp "$BUILD/$_bin" "$APP/Contents/MacOS/"
done
# Bundle the assets too so a bundled run is self-contained.
if [[ -d "$ROOT/assets" ]]; then
    cp -R "$ROOT/assets" "$APP/Contents/assets"
fi
# The app icon, as the packaged app carries it (scripts/package_macos_dmg.sh
# says what the two files are).
mkdir -p "$APP/Contents/Resources"
cp "$ROOT/packaging/macos/Assets.car" "$ROOT/packaging/macos/Liu.icns" \
    "$APP/Contents/Resources/"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>Liu</string>
    <key>CFBundleIdentifier</key>
    <string>com.github.calculusteam.Liu.dev</string>
    <key>CFBundleName</key>
    <string>Liu</string>
    <key>CFBundleDisplayName</key>
    <string>Liu</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleIconFile</key>
    <string>Liu</string>
    <key>CFBundleIconName</key>
    <string>Liu</string>
    <key>LSMinimumSystemVersion</key>
    <string>11.0</string>
    <key>NSSupportsAutomaticGraphicsSwitching</key>
    <true/>
    <key>NSMicrophoneUsageDescription</key>
    <string>Liu uses the microphone when you dictate a prompt with Voice Input, and when you record a custom notification sound. Speech is transcribed on this Mac by default; if you choose a hosted speech service in the Voice settings, your audio is sent to that service.</string>
    <key>NSLocalNetworkUsageDescription</key>
    <string>Liu connects to hosts on your local network when you open an SSH, Mosh or Telnet session to one, or a page on one in its browser.</string>
    <key>NSAppTransportSecurity</key>
    <dict>
        <key>NSAllowsArbitraryLoadsInWebContent</key>
        <true/>
    </dict>
    <key>NSRemovableVolumesUsageDescription</key>
    <string>Liu opens terminals, browses files and runs git in folders you pick, including ones on external drives.</string>
    <key>NSNetworkVolumesUsageDescription</key>
    <string>Liu opens terminals, browses files and runs git in folders you pick, including ones on network shares.</string>
    <key>NSDesktopFolderUsageDescription</key>
    <string>Liu's file browser only reads the folders you navigate to — your Desktop among them.</string>
    <key>NSDocumentsFolderUsageDescription</key>
    <string>Liu's file browser only reads the folders you navigate to — your Documents among them.</string>
    <key>NSDownloadsFolderUsageDescription</key>
    <string>Liu's file browser only reads the folders you navigate to — your Downloads among them.</string>
    <key>NSServices</key>
    <array>
        <dict>
            <key>NSMenuItem</key>
            <dict>
                <key>default</key>
                <string>New Liu Window Here</string>
            </dict>
            <key>NSMessage</key>
            <string>openLiuWindowHere</string>
            <key>NSPortName</key>
            <string>Liu</string>
            <key>NSRequiredContext</key>
            <dict>
                <key>NSTextContent</key>
                <string>FilePath</string>
            </dict>
            <key>NSSendFileTypes</key>
            <array>
                <string>public.folder</string>
                <string>public.item</string>
            </array>
        </dict>
        <dict>
            <key>NSMenuItem</key>
            <dict>
                <key>default</key>
                <string>New Liu Tab Here</string>
            </dict>
            <key>NSMessage</key>
            <string>openLiuTabHere</string>
            <key>NSPortName</key>
            <string>Liu</string>
            <key>NSRequiredContext</key>
            <dict>
                <key>NSTextContent</key>
                <string>FilePath</string>
            </dict>
            <key>NSSendFileTypes</key>
            <array>
                <string>public.folder</string>
                <string>public.item</string>
            </array>
        </dict>
    </array>
</dict>
</plist>
PLIST

# Sign the .app bundle so TCC associates privacy permissions with a code
# identity. --deep signs the nested LiuShaders.metallib as well. Developer ID
# signatures retain permission identity across rebuilds; ad-hoc signatures are
# tied to a code hash and require new grants when the binary changes.
# LIU_DEV_SIGN_IDENTITY selects the signing identity; set it to "-" to force
# ad-hoc signing.
DEV_IDENTITY="${LIU_DEV_SIGN_IDENTITY:-}"
if [[ -z "$DEV_IDENTITY" ]]; then
    DEV_IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null \
        | sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -1)"
fi
# Give ad-hoc development builds a distinct bundle identifier so their
# permission records cannot conflict with the installed app.
SIGN_BUNDLE_ID="com.github.calculusteam.Liu.dev"
if [[ -z "$DEV_IDENTITY" ]]; then
    DEV_IDENTITY="-"
    SIGN_BUNDLE_ID="com.github.calculusteam.Liu.adhoc"
    /usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier ${SIGN_BUNDLE_ID}" \
        "$APP/Contents/Info.plist" >/dev/null 2>&1 || \
        echo "warning: could not rewrite CFBundleIdentifier" >&2
    echo "note: no Developer ID found — signing ad-hoc under ${SIGN_BUNDLE_ID}," >&2
    echo "      so this build cannot invalidate the installed Liu.app's" >&2
    echo "      permissions. It gets its own prompts (see the comment in $0)." >&2
fi
codesign --force --deep --sign "$DEV_IDENTITY" \
         --identifier "$SIGN_BUNDLE_ID" "$APP" >/dev/null 2>&1 || \
    echo "warning: codesign failed; permission prompts may not appear" >&2

exec open "$APP"
