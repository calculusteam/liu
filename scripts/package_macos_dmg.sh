#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
OUTPUT_DIR="${OUTPUT_DIR:-${BUILD_DIR}/package}"
APP_NAME="${APP_NAME:-Liu}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "DMG packaging is only supported on macOS." >&2
    exit 1
fi

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DUSE_VENDORED_SSH_DEPS=ON \
        -DAUTO_DOWNLOAD_SSH_DEPS=ON
fi

cmake --build "${BUILD_DIR}" -j"${JOBS}" --target Liu liu-notify

APP_BINARY="${BUILD_DIR}/Liu"
if [[ ! -x "${APP_BINARY}" ]]; then
    if [[ -x "${BUILD_DIR}/${BUILD_TYPE}/Liu" ]]; then
        APP_BINARY="${BUILD_DIR}/${BUILD_TYPE}/Liu"
    else
        APP_BINARY="$(find "${BUILD_DIR}" -maxdepth 3 -type f -name Liu -perm -111 | head -n 1)"
    fi
fi

if [[ -z "${APP_BINARY}" || ! -x "${APP_BINARY}" ]]; then
    echo "Could not find the built Liu binary in ${BUILD_DIR}." >&2
    exit 1
fi

# Version comes from the canonical VERSION file (single source of truth, shared
# with CMake/website/feed). Append +<shorthash> only when this isn't an exact
# tagged release, so dev DMGs are distinguishable but releases stay clean.
VERSION="$(tr -d '[:space:]' < "${ROOT_DIR}/VERSION")"
# CFBundleVersion must stay monotonic/clean — keep the bare VERSION for the plist.
PLIST_VERSION="${VERSION}"
if ! git -C "${ROOT_DIR}" describe --tags --exact-match >/dev/null 2>&1; then
    SHORTHASH="$(git -C "${ROOT_DIR}" rev-parse --short HEAD 2>/dev/null || echo dev)"
    VERSION="${VERSION}+${SHORTHASH}"
fi
ARCH="$(uname -m)"

STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/${APP_NAME}.dmg.XXXXXX")"
trap 'rm -rf "${STAGE_DIR}"' EXIT

APP_DIR="${STAGE_DIR}/${APP_NAME}.app"
mkdir -p "${APP_DIR}/Contents/MacOS" "${APP_DIR}/Contents/Resources" "${APP_DIR}/Contents/assets"

cp "${APP_BINARY}" "${APP_DIR}/Contents/MacOS/${APP_NAME}"
chmod +x "${APP_DIR}/Contents/MacOS/${APP_NAME}"

# Bundle liu-notify next to the main binary. Liu installs notify hooks that
# invoke "<dir-of-Liu>/liu-notify send …" on launch and removes them on quit,
# so the helper must live in Contents/MacOS/. Resolve it the same way as Liu.
NOTIFY_BINARY="$(dirname "${APP_BINARY}")/liu-notify"
if [[ ! -x "${NOTIFY_BINARY}" ]]; then
    NOTIFY_BINARY="$(find "${BUILD_DIR}" -maxdepth 3 -type f -name liu-notify -perm -111 | head -n 1)"
fi
if [[ -n "${NOTIFY_BINARY}" && -x "${NOTIFY_BINARY}" ]]; then
    cp "${NOTIFY_BINARY}" "${APP_DIR}/Contents/MacOS/liu-notify"
    chmod +x "${APP_DIR}/Contents/MacOS/liu-notify"
else
    echo "warning: liu-notify binary not found — hooks won't work in the bundle." >&2
fi

# Metal shader libraries (LiuShaders.metallib for the renderer,
# LiuLLM.metallib for the local LLM kernels when USE_LOCAL_LLM=ON) must
# sit next to the binary in Contents/MacOS/ — both loaders resolve them
# via the executable's dirname. The glob no-ops on USE_METAL=OFF builds.
APP_BINARY_DIR="$(dirname "${APP_BINARY}")"
for lib in "${APP_BINARY_DIR}"/*.metallib; do
    [[ -f "${lib}" ]] && cp "${lib}" "${APP_DIR}/Contents/MacOS/"
done

# Also copy assets (fonts, icons) so the bundle is standalone
if [[ -d "${ROOT_DIR}/assets" ]]; then
    cp -R "${ROOT_DIR}/assets/" "${APP_DIR}/Contents/assets/"
fi

# App icon — generate Liu.icns from assets/appicon.png (the bunny mark) into Resources/.
# CFBundleIconFile in Info.plist points Finder/Dock at it.
if [[ -f "${ROOT_DIR}/assets/appicon.png" ]] && command -v iconutil >/dev/null 2>&1; then
    ICONSET="$(mktemp -d)/Liu.iconset"
    mkdir -p "${ICONSET}"
    for s in 16 32 128 256 512; do
        sips -z "${s}" "${s}" "${ROOT_DIR}/assets/appicon.png" \
            --out "${ICONSET}/icon_${s}x${s}.png" >/dev/null 2>&1
        d=$(( s * 2 ))
        sips -z "${d}" "${d}" "${ROOT_DIR}/assets/appicon.png" \
            --out "${ICONSET}/icon_${s}x${s}@2x.png" >/dev/null 2>&1
    done
    iconutil -c icns "${ICONSET}" -o "${APP_DIR}/Contents/Resources/Liu.icns" || \
        echo "warning: iconutil failed — bundle will use the default icon." >&2
    rm -rf "$(dirname "${ICONSET}")"
fi

cat > "${APP_DIR}/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>${APP_NAME}</string>
    <key>CFBundleIdentifier</key>
    <string>com.github.calculusteam.${APP_NAME}</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>${APP_NAME}</string>
    <key>CFBundleDisplayName</key>
    <string>${APP_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleIconFile</key>
    <string>Liu</string>
    <key>CFBundleShortVersionString</key>
    <string>${PLIST_VERSION}</string>
    <key>CFBundleVersion</key>
    <string>${PLIST_VERSION}</string>
    <key>LSMinimumSystemVersion</key>
    <string>11.0</string>
    <key>NSMicrophoneUsageDescription</key>
    <string>Liu records short audio clips you choose as custom notification sounds.</string>
</dict>
</plist>
EOF

# Code-sign the bundle so the Info.plist (incl. the microphone usage string) is
# bound to the signature and TCC has a stable identity to grant mic access
# against. Set CODESIGN_IDENTITY to a "Developer ID Application: …" identity for
# a distributable, notarizable build; it defaults to an ad-hoc signature, which
# works locally but is re-prompted per machine.
#
# We sign inside-out (nested code first, the .app last) and DO NOT use --deep:
# --deep would stamp the app's identifier and microphone entitlement onto the
# nested liu-notify helper, which Apple's notary service rejects. Each nested
# Mach-O keeps its own identifier; only the top-level app carries the mic
# entitlement.
CODESIGN_IDENTITY="${CODESIGN_IDENTITY:--}"
RUNTIME_OPTS=()
ENT_OPTS=()
if [[ "${CODESIGN_IDENTITY}" != "-" ]]; then
    # A real identity gets the hardened runtime + secure timestamp (both required
    # for notarization) and the audio-input entitlement on the app itself.
    RUNTIME_OPTS=(--options runtime --timestamp)
    ENT_PLIST="${STAGE_DIR}/entitlements.plist"
    cat > "${ENT_PLIST}" <<'ENT'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>com.apple.security.device.audio-input</key><true/>
</dict></plist>
ENT
    ENT_OPTS=(--entitlements "${ENT_PLIST}")
fi

# bash 3.2 (macOS default) errors on "${arr[@]}" for an empty array under
# `set -u`, so expand optional arrays through the ${arr[@]+...} guard.
sign_one() {  # sign_one <path> [extra-flags…]
    local path="$1"; shift
    codesign --force --sign "${CODESIGN_IDENTITY}" \
        ${RUNTIME_OPTS[@]+"${RUNTIME_OPTS[@]}"} "$@" "${path}" \
        || echo "warning: codesign ${path##*/} failed" >&2
}

# 1) Nested code first: shader libs, then the liu-notify helper (its own
#    identifier, no mic entitlement — it only speaks, never records).
for lib in "${APP_DIR}/Contents/MacOS/"*.metallib; do
    [[ -f "${lib}" ]] && sign_one "${lib}"
done
if [[ -f "${APP_DIR}/Contents/MacOS/liu-notify" ]]; then
    sign_one "${APP_DIR}/Contents/MacOS/liu-notify" \
        --identifier "com.github.calculusteam.${APP_NAME}.notify"
fi
# 2) The app bundle last, WITHOUT --deep, carrying the mic entitlement alone.
codesign --force --sign "${CODESIGN_IDENTITY}" \
    --identifier "com.github.calculusteam.${APP_NAME}" \
    ${RUNTIME_OPTS[@]+"${RUNTIME_OPTS[@]}"} \
    ${ENT_OPTS[@]+"${ENT_OPTS[@]}"} \
    "${APP_DIR}" \
    || echo "warning: codesign ${APP_NAME}.app failed (continuing unsigned)" >&2

mkdir -p "${OUTPUT_DIR}"

# Update-channel artifact: a ditto zip of the SAME signed bundle. The auto-
# updater downloads this (extracts with one `ditto -x`, no hdiutil mount) and
# verifies its Ed25519 signature before installing. The DMG below stays the
# human first-install download. Skip with PACKAGE_ZIP=0.
ZIP_PATH="${OUTPUT_DIR}/${APP_NAME}-${VERSION}-${ARCH}.zip"
if [[ "${PACKAGE_ZIP:-1}" != "0" ]]; then
    ditto -c -k --sequesterRsrc --keepParent "${APP_DIR}" "${ZIP_PATH}"
fi

ln -s /Applications "${STAGE_DIR}/Applications"

DMG_NAME="${APP_NAME}-${VERSION}-${ARCH}.dmg"
DMG_PATH="${OUTPUT_DIR}/${DMG_NAME}"

# Styled installer window. The art (assets/dmg/background.tiff, a HiDPI 1x/2x
# pair) plus the icon layout give the "drag Liu to Applications" window its
# branded look. We use dmgbuild — which writes the .DS_Store (background +
# window + icon positions) directly — rather than Finder/AppleScript: the
# AppleScript `background picture` property is broken on recent macOS
# (Tahoe / 26.x), where icon positions apply but the background is silently
# dropped. Geometry lives in scripts/dmg_build_settings.py and must match the
# 640x400 canvas the art is drawn at (see scripts/dmg_render_background.sh).
BG_TIFF="${ROOT_DIR}/assets/dmg/background.tiff"
VOL_ICON="${APP_DIR}/Contents/Resources/Liu.icns"   # reuse the app's icns (may be absent)
DMG_SETTINGS="${ROOT_DIR}/scripts/dmg_build_settings.py"

# resolve_dmgbuild → echo a dmgbuild executable path, or non-zero (no output)
# to mean "fall back to a plain DMG". Honors $DMGBUILD_BIN, then a PATH
# dmgbuild, then a cached build/.dmgvenv, otherwise creates that venv on first
# use (needs python3 + network). Set DMG_PLAIN=1 to skip styling entirely.
resolve_dmgbuild() {
    if [[ -n "${DMGBUILD_BIN:-}" && -x "${DMGBUILD_BIN}" ]]; then echo "${DMGBUILD_BIN}"; return 0; fi
    if command -v dmgbuild >/dev/null 2>&1; then command -v dmgbuild; return 0; fi
    local venv="${BUILD_DIR}/.dmgvenv"
    if [[ -x "${venv}/bin/dmgbuild" ]]; then echo "${venv}/bin/dmgbuild"; return 0; fi
    command -v python3 >/dev/null 2>&1 || return 1
    python3 -m venv "${venv}" >/dev/null 2>&1 || return 1
    "${venv}/bin/pip" install -q --upgrade pip dmgbuild >/dev/null 2>&1 || return 1
    [[ -x "${venv}/bin/dmgbuild" ]] && { echo "${venv}/bin/dmgbuild"; return 0; }
    return 1
}

# make_styled_dmg <volname> <app.app> <out.dmg> → 0 on success, non-zero to fall back.
make_styled_dmg() {
    local volname="$1" app="$2" out="$3" db
    [[ -f "${BG_TIFF}" ]]      || { echo "note: ${BG_TIFF} missing — plain DMG." >&2; return 1; }
    [[ -f "${DMG_SETTINGS}" ]] || { echo "note: ${DMG_SETTINGS} missing — plain DMG." >&2; return 1; }
    db="$(resolve_dmgbuild)"   || { echo "note: dmgbuild unavailable (pip install dmgbuild) — plain DMG." >&2; return 1; }
    # Stale mounts of this volume name wedge dmgbuild: it assumes /Volumes/<vol>,
    # so if the volume is already mounted (forcing a "<vol> 1" mountpoint) the
    # Applications symlink fails with ENOENT. Detach every match first.
    for m in "/Volumes/${volname}" "/Volumes/${volname} "*; do
        [[ -d "${m}" ]] && hdiutil detach "${m}" -force >/dev/null 2>&1
    done
    rm -f "${out}"
    APP_PATH="${app}" BG="${BG_TIFF}" VOLICON="${VOL_ICON}" \
        "${db}" -s "${DMG_SETTINGS}" "${volname}" "${out}" >/dev/null || return 1
    [[ -f "${out}" ]]
}

if [[ "${PACKAGE_DMG:-1}" != "0" ]]; then
    if [[ "${DMG_PLAIN:-0}" != "1" ]] && make_styled_dmg "${APP_NAME}" "${APP_DIR}" "${DMG_PATH}"; then
        : # styled DMG written to ${DMG_PATH}
    else
        echo "warning: styled DMG layout unavailable — writing a plain DMG." >&2
        hdiutil create \
            -volname "${APP_NAME}" \
            -srcfolder "${STAGE_DIR}" \
            -ov \
            -format UDZO \
            "${DMG_PATH}" >/dev/null
    fi
fi

# Final line(s) of stdout are the integration handle for release.sh / CI.
[[ "${PACKAGE_DMG:-1}" != "0" ]] && echo "${DMG_PATH}"
[[ "${PACKAGE_ZIP:-1}" != "0" ]] && echo "${ZIP_PATH}"
