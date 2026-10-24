#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/jobs.sh"   # liu_jobs: a validated, portable job count

BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
OUTPUT_DIR="${OUTPUT_DIR:-${BUILD_DIR}/package}"
APP_NAME="${APP_NAME:-Liu}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="$(liu_jobs)"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "DMG packaging is only supported on macOS." >&2
    exit 1
fi

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DUSE_VENDORED_SSH_DEPS=ON \
        -DAUTO_DOWNLOAD_SSH_DEPS=ON \
        -DLIU_DEV_HOOKS=OFF
else
    # Configure packaging builds with development drivers and hooks disabled,
    # regardless of the previous build-directory configuration.
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DLIU_DEV_HOOKS=OFF >/dev/null
fi

cmake --build "${BUILD_DIR}" -j"${JOBS}" --target Liu liu-notify liu-history

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

# The gate for the option above: the binary says what it carries, and its text
# must not hold the driver's variable. Either one failing means a binary that
# any process of the user's could drive.
if "${APP_BINARY}" --version | grep -q "dev-hooks"; then
    echo "Refusing to package: ${APP_BINARY} was built with LIU_DEV_HOOKS (the test driver)." >&2
    exit 1
fi
if LC_ALL=C grep -a -q "LIU_E2E_SCRIPT" "${APP_BINARY}"; then
    echo "Refusing to package: ${APP_BINARY} still contains the test driver's variable." >&2
    exit 1
fi

# Version comes from the canonical VERSION file (single source of truth, shared
# with CMake/website/feed). Append +<shorthash> only when this isn't an exact
# tagged release, so dev DMGs are distinguishable but releases stay clean.
VERSION="$(tr -d '[:space:]' < "${ROOT_DIR}/VERSION")"
# Validate VERSION before inserting it into Info.plist or artifact paths.
if [[ ! "${VERSION}" =~ ^[0-9]+(\.[0-9]+){0,2}(-[A-Za-z0-9.]+)?$ ]]; then
    echo "error: VERSION '${VERSION}' is not a valid semver token" >&2; exit 1
fi
# CFBundleVersion must stay monotonic/clean — keep the bare VERSION for the plist.
PLIST_VERSION="${VERSION}"
if ! git -C "${ROOT_DIR}" describe --tags --exact-match >/dev/null 2>&1; then
    SHORTHASH="$(git -C "${ROOT_DIR}" rev-parse --short HEAD 2>/dev/null || echo dev)"
    VERSION="${VERSION}+${SHORTHASH}"
fi
ARCH="$(uname -m)"

STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/${APP_NAME}.dmg.XXXXXX")"
trap 'rm -rf "${STAGE_DIR}"' EXIT

# DMG_ROOT is what the plain-DMG fallback hands to `hdiutil -srcfolder`, so it
# must hold ONLY the shipped payload (the .app plus the /Applications symlink).
# Build-time scratch — the entitlements plist, the notarization submission zip —
# lives in STAGE_DIR one level up, or it would ride along inside the DMG.
DMG_ROOT="${STAGE_DIR}/root"
APP_DIR="${DMG_ROOT}/${APP_NAME}.app"
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

# Bundle liu-history the same way. Liu links its helpers from Contents/MacOS/
# into a folder on every local pane's PATH and skips one that is missing, so a
# bundle without it has no `liu-history` command in the app's own shells.
HISTORY_BINARY="$(dirname "${APP_BINARY}")/liu-history"
if [[ ! -x "${HISTORY_BINARY}" ]]; then
    HISTORY_BINARY="$(find "${BUILD_DIR}" -maxdepth 3 -type f -name liu-history -perm -111 | head -n 1)"
fi
if [[ -n "${HISTORY_BINARY}" && -x "${HISTORY_BINARY}" ]]; then
    cp "${HISTORY_BINARY}" "${APP_DIR}/Contents/MacOS/liu-history"
    chmod +x "${APP_DIR}/Contents/MacOS/liu-history"
else
    echo "error: liu-history binary not found — the bundle would ship without the history CLI." >&2
    exit 1
fi

# Metal shader libraries (LiuShaders.metallib for the renderer,
# LiuLLM.metallib for the local LLM kernels when USE_LOCAL_LLM=ON) must
# sit next to the binary in Contents/MacOS/ — both loaders resolve them
# via the executable's dirname. The glob no-ops on USE_METAL=OFF builds.
APP_BINARY_DIR="$(dirname "${APP_BINARY}")"
for lib in "${APP_BINARY_DIR}"/*.metallib; do
    [[ -f "${lib}" ]] && cp "${lib}" "${APP_DIR}/Contents/MacOS/"
done

# The renderer cannot start without its shader library — the app says so and
# quits, but a package that left it out is not a package. (The loop above
# copies whatever is there and is silent about what is not.)
if grep -q '^USE_METAL:BOOL=ON' "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null \
   && [[ ! -f "${APP_DIR}/Contents/MacOS/LiuShaders.metallib" ]]; then
    echo "error: LiuShaders.metallib is not beside the binary in ${APP_BINARY_DIR} — the packaged app could not draw." >&2
    exit 1
fi

# Ensure packaged binaries do not contain paths from the build machine.
python3 "${ROOT_DIR}/scripts/check_bundle_paths.py" \
    --forbid "${ROOT_DIR}" --forbid "${HOME}" \
    "${APP_DIR}/Contents/MacOS/"* >&2 \
    || { echo "error: a packaged binary names a path on the build machine (above)." >&2; exit 1; }

# A shader library says which macOS it was built for, and one built for a newer macOS
# than the bundle admits to does not load on the ones in between: the app starts, the
# renderer cannot make its pipelines, and the window draws nothing. MIN_MACOS is what the
# plist below declares; a library asking for more stops the packaging here.
MIN_MACOS="11.0"
METALLIBS=("${APP_DIR}/Contents/MacOS/"*.metallib)
if [[ -f "${METALLIBS[0]}" ]]; then
    python3 "${ROOT_DIR}/scripts/check_metallib_target.py" --max-os "${MIN_MACOS}" "${METALLIBS[@]}" >&2 \
        || { echo "error: a Metal library needs a newer macOS than LSMinimumSystemVersion (${MIN_MACOS})." >&2; exit 1; }
fi

# Also copy assets (fonts, icons) so the bundle is standalone
if [[ -d "${ROOT_DIR}/assets" ]]; then
    cp -R "${ROOT_DIR}/assets/" "${APP_DIR}/Contents/assets/"
    # assets/dmg is what the DISK IMAGE is drawn from (its background and the
    # fonts for it); nothing in the app reads it, and it rode along in every
    # bundle for half a megabyte.
    rm -rf "${APP_DIR:?}/Contents/assets/dmg"
fi

# What Liu ships that somebody else wrote travels with it: the libraries
# linked in, the icon sets and the fonts ask for their notices to accompany
# the binary, and before 0.1.0 none did. The file is generated from the tree;
# a stale one (a dependency bumped, a font added) stops the package here.
# The privacy prompts and Services entries are written in three places (this
# script, run_dev_macos.sh, CMake's embedded plist); they must say the same.
python3 "${ROOT_DIR}/scripts/check_info_plists.py" "${ROOT_DIR}" >&2 \
    || { echo "error: the Info.plist sources disagree (see above)." >&2; exit 1; }
python3 "${ROOT_DIR}/scripts/gen_third_party_notices.py" --check >&2 \
    || { echo "error: THIRD_PARTY_NOTICES.md is out of date — run scripts/gen_third_party_notices.py and commit it." >&2; exit 1; }
cp "${ROOT_DIR}/THIRD_PARTY_NOTICES.md" "${ROOT_DIR}/LICENSE" "${ROOT_DIR}/NOTICE" "${APP_DIR}/Contents/Resources/"

# Liu's compiled terminfo entry (TERM=xterm-liu, an opt-in: liu.terminfo says
# why). The app looks for it in Resources/terminfo; a build without tic has
# none and that is not an error.
if [ -d "${BUILD_DIR}/terminfo" ]; then
    rm -rf "${APP_DIR}/Contents/Resources/terminfo"
    mkdir -p "${APP_DIR}/Contents/Resources/terminfo"
    (cd "${BUILD_DIR}/terminfo" && find . -type f ! -name .stamp -print) | while IFS= read -r entry; do
        mkdir -p "${APP_DIR}/Contents/Resources/terminfo/$(dirname "$entry")"
        cp "${BUILD_DIR}/terminfo/${entry}" "${APP_DIR}/Contents/Resources/terminfo/${entry}"
    done
fi

# App icon — the compiled Icon Composer document (packaging/macos/Liu.icon),
# checked in because actool reads a .icon only from Xcode 26 on and the release
# runners are older. Assets.car is what macOS 26 and later draw the icon from,
# per icon style (Default / Dark / Clear / Tinted), found through
# CFBundleIconName; Liu.icns (CFBundleIconFile) is for everything that reads
# only that, the DMG's volume icon below included. Regenerate both with
# scripts/gen_app_icon.py.
cp "${ROOT_DIR}/packaging/macos/Assets.car" "${ROOT_DIR}/packaging/macos/Liu.icns" \
    "${APP_DIR}/Contents/Resources/"

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
    <key>CFBundleIconName</key>
    <string>Liu</string>
    <key>CFBundleShortVersionString</key>
    <string>${PLIST_VERSION}</string>
    <key>CFBundleVersion</key>
    <string>${PLIST_VERSION}</string>
    <key>LSMinimumSystemVersion</key>
    <string>${MIN_MACOS}</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.developer-tools</string>
    <key>NSHumanReadableCopyright</key>
    <string>Copyright © 2026 calculus.team. Apache License 2.0.</string>
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
EOF

# Code-sign the bundle so the Info.plist (incl. the microphone usage string) is
# bound to the signature and TCC has a stable identity to grant mic access
# against. Set CODESIGN_IDENTITY to a "Developer ID Application: …" identity for
# a distributable, notarizable build; it defaults to an ad-hoc signature, which
# works locally but is re-prompted per machine.
#
# The bundle is signed inside-out (nested code first, the .app last) and WITHOUT
# --deep: --deep would stamp the app's identifier and microphone entitlement onto
# the nested liu-notify helper, which Apple's notary service rejects. Each nested
# Mach-O keeps its own identifier; only the top-level app carries the mic
# entitlement.
#
# With CODESIGN_IDENTITY unset the script auto-selects the first "Developer ID
# Application" identity in the keychain, so a machine that has the release cert
# installed produces a distributable build with no extra ceremony, while a
# machine that doesn't still gets the ad-hoc fallback. CODESIGN_IDENTITY=- forces
# ad-hoc explicitly.
#
# CODESIGN_KEYCHAIN names a keychain of its own to take the identity from — not
# the login keychain, where Xcode keeps an identity that can carry the same
# "Developer ID Application: …" name. Every codesign call is then held to it
# (--keychain), and with CODESIGN_IDENTITY unset the identity is that keychain's
# first one, by its SHA-1, so a name shared with another keychain cannot pick the
# wrong one. A locked keychain is opened with the password kept under the
# login-keychain item CODESIGN_KEYCHAIN_ITEM (default liu-devid-keychain);
# nothing secret is printed.
KEYCHAIN_OPTS=()
if [[ -n "${CODESIGN_KEYCHAIN:-}" ]]; then
    if [[ ! -f "${CODESIGN_KEYCHAIN}" ]]; then
        echo "error: CODESIGN_KEYCHAIN ${CODESIGN_KEYCHAIN} does not exist." >&2
        exit 1
    fi
    if kc_pw="$(security find-generic-password -s "${CODESIGN_KEYCHAIN_ITEM:-liu-devid-keychain}" -w 2>/dev/null)"; then
        security unlock-keychain -p "${kc_pw}" "${CODESIGN_KEYCHAIN}" \
            || { echo "error: could not unlock ${CODESIGN_KEYCHAIN}." >&2; exit 1; }
    fi
    unset kc_pw
    KEYCHAIN_OPTS=(--keychain "${CODESIGN_KEYCHAIN}")
    if [[ -z "${CODESIGN_IDENTITY:-}" ]]; then
        CODESIGN_IDENTITY="$(security find-identity -v -p codesigning "${CODESIGN_KEYCHAIN}" 2>/dev/null \
            | awk '/Developer ID Application:/ {print $2; exit}')"
        if [[ -z "${CODESIGN_IDENTITY}" ]]; then
            echo "error: no Developer ID Application identity in ${CODESIGN_KEYCHAIN}." >&2
            exit 1
        fi
    fi
    echo "note: signing from ${CODESIGN_KEYCHAIN} with identity ${CODESIGN_IDENTITY}" >&2
fi
if [[ -z "${CODESIGN_IDENTITY:-}" ]]; then
    CODESIGN_IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null \
        | sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -n 1)"
    if [[ -n "${CODESIGN_IDENTITY}" ]]; then
        echo "note: signing with auto-detected identity: ${CODESIGN_IDENTITY}" >&2
    else
        CODESIGN_IDENTITY="-"
        echo "note: no Developer ID Application identity found — falling back to an ad-hoc signature." >&2
    fi
fi
# Use a distinct bundle identifier for ad-hoc builds. TCC associates permission
# grants with the bundle ID and code requirement; a development build sharing
# the release identifier can invalidate the installed app's permission record.
BUNDLE_ID="com.github.calculusteam.${APP_NAME}"
if [[ "${CODESIGN_IDENTITY}" == "-" ]]; then
    BUNDLE_ID="com.github.calculusteam.${APP_NAME}.adhoc"
    /usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier ${BUNDLE_ID}" \
        "${APP_DIR}/Contents/Info.plist" >/dev/null 2>&1 || \
        echo "warning: could not rewrite CFBundleIdentifier for the ad-hoc build" >&2
    echo "note: ad-hoc build re-identified as ${BUNDLE_ID} so it cannot" >&2
    echo "      invalidate the installed Liu.app's privacy permissions." >&2
fi

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
        ${KEYCHAIN_OPTS[@]+"${KEYCHAIN_OPTS[@]}"} \
        ${RUNTIME_OPTS[@]+"${RUNTIME_OPTS[@]}"} "$@" "${path}" \
        || echo "warning: codesign ${path##*/} failed" >&2
}

# 1) Nested code first: shader libs, then the liu-notify and liu-history
#    helpers (each its own identifier, no mic entitlement — neither records).
for lib in "${APP_DIR}/Contents/MacOS/"*.metallib; do
    [[ -f "${lib}" ]] && sign_one "${lib}"
done
if [[ -f "${APP_DIR}/Contents/MacOS/liu-notify" ]]; then
    sign_one "${APP_DIR}/Contents/MacOS/liu-notify" \
        --identifier "${BUNDLE_ID}.notify"
fi
if [[ -f "${APP_DIR}/Contents/MacOS/liu-history" ]]; then
    sign_one "${APP_DIR}/Contents/MacOS/liu-history" \
        --identifier "${BUNDLE_ID}.history"
fi
# 2) The app bundle last, WITHOUT --deep, carrying the mic entitlement alone.
codesign --force --sign "${CODESIGN_IDENTITY}" \
    ${KEYCHAIN_OPTS[@]+"${KEYCHAIN_OPTS[@]}"} \
    --identifier "${BUNDLE_ID}" \
    ${RUNTIME_OPTS[@]+"${RUNTIME_OPTS[@]}"} \
    ${ENT_OPTS[@]+"${ENT_OPTS[@]}"} \
    "${APP_DIR}" \
    || echo "warning: codesign ${APP_NAME}.app failed (continuing unsigned)" >&2

# A real identity must produce a verifiable signature — a broken seal here would
# be caught by the notary service anyway, but failing now gives a usable error.
if [[ "${CODESIGN_IDENTITY}" != "-" ]]; then
    codesign --verify --deep --strict --verbose=2 "${APP_DIR}" \
        || { echo "error: signature verification failed for ${APP_NAME}.app" >&2; exit 1; }
fi

# Notarization.
#
# Submit the signed app to Apple's notary service and staple the ticket before
# creating downstream artifacts. The ticket supports offline Gatekeeper checks.
#
# Credential precedence: NOTARY_PROFILE; NOTARY_KEY + NOTARY_KEY_ID [+
# NOTARY_ISSUER]; NOTARY_APPLE_ID + NOTARY_PASSWORD + NOTARY_TEAM_ID. Prefer a
# profile or API key in CI because process arguments can expose passwords. NOTARIZE=0
# skips this step. NOTARY_TIMEOUT bounds the wait (default 45 minutes).
NOTARY_ARGS=()
resolve_notary_args() {
    NOTARY_ARGS=()
    if [[ -n "${NOTARY_PROFILE:-}" ]]; then
        NOTARY_ARGS=(--keychain-profile "${NOTARY_PROFILE}")
    elif [[ -n "${NOTARY_KEY:-}" && -n "${NOTARY_KEY_ID:-}" ]]; then
        NOTARY_ARGS=(--key "${NOTARY_KEY}" --key-id "${NOTARY_KEY_ID}")
        [[ -n "${NOTARY_ISSUER:-}" ]] && NOTARY_ARGS+=(--issuer "${NOTARY_ISSUER}")
    elif [[ -n "${NOTARY_APPLE_ID:-}" && -n "${NOTARY_PASSWORD:-}" && -n "${NOTARY_TEAM_ID:-}" ]]; then
        # notarytool has no stdin/@env form for this: the app-specific
        # password is visible in `ps` for the life of the submit.
        # Prefer NOTARY_KEY_ID + NOTARY_KEY_PATH (API key) or a keychain
        # profile; this branch is kept for machines that only have the
        # password and is announced so nobody uses it on a shared box.
        echo "warning: notarising with an Apple ID password on argv; prefer an API key or keychain profile" >&2
        NOTARY_ARGS=(--apple-id "${NOTARY_APPLE_ID}" --password "${NOTARY_PASSWORD}" \
                     --team-id "${NOTARY_TEAM_ID}")
    else
        return 1
    fi
    return 0
}

NOTARIZE_ENABLED=0
if [[ "${CODESIGN_IDENTITY}" == "-" ]]; then
    : # an ad-hoc signature can never be notarized
elif [[ "${NOTARIZE:-1}" == "0" ]]; then
    echo "note: NOTARIZE=0 — skipping notarization." >&2
elif resolve_notary_args; then
    NOTARIZE_ENABLED=1
else
    echo "warning: signed with a Developer ID but no notary credentials in the environment —" >&2
    echo "         the build will still trip Gatekeeper. See scripts/notarize.md." >&2
fi

# notarize_and_staple <submit-path> <staple-path>
# Submits, waits for Apple's verdict, dumps the notary log on failure (it is the
# only place that names the offending binary), then staples the ticket.
notarize_and_staple() {
    local submit="$1" staple="$2" out sid status
    echo "notarizing $(basename "${submit}") — this usually takes a few minutes…" >&2
    if ! out="$(xcrun notarytool submit "${submit}" "${NOTARY_ARGS[@]}" \
                    --wait --timeout "${NOTARY_TIMEOUT:-45m}" --no-progress 2>&1)"; then
        echo "${out}" >&2
        sid="$(printf '%s\n' "${out}" | sed -n 's/.*id: \([0-9a-f-]\{36\}\).*/\1/p' | head -n 1)"
        [[ -n "${sid}" ]] && xcrun notarytool log "${sid}" "${NOTARY_ARGS[@]}" >&2 || true
        echo "error: notarization of $(basename "${submit}") failed." >&2
        return 1
    fi
    echo "${out}" >&2
    # Anchor to the "  status: <verdict>" line notarytool prints under
    # "Processing complete"; unanchored it would also chew up "Current status: …".
    status="$(printf '%s\n' "${out}" | sed -n 's/^ *status: *//p' | tail -n 1)"
    if [[ "${status}" != "Accepted" ]]; then
        sid="$(printf '%s\n' "${out}" | sed -n 's/.*id: \([0-9a-f-]\{36\}\).*/\1/p' | head -n 1)"
        [[ -n "${sid}" ]] && xcrun notarytool log "${sid}" "${NOTARY_ARGS[@]}" >&2 || true
        echo "error: notary service returned status '${status:-unknown}' for $(basename "${submit}")." >&2
        return 1
    fi
    xcrun stapler staple "${staple}" || { echo "error: stapling ${staple##*/} failed." >&2; return 1; }
    xcrun stapler validate "${staple}" >/dev/null \
        || { echo "error: stapled ticket on ${staple##*/} does not validate." >&2; return 1; }
    echo "notarized + stapled: ${staple##*/}" >&2
}

if [[ "${NOTARIZE_ENABLED}" == "1" ]]; then
    # The notary service takes an archive, never a bare bundle. This zip is
    # scratch (STAGE_DIR, outside DMG_ROOT); the shipped zip is rebuilt below
    # from the stapled bundle.
    SUBMIT_ZIP="${STAGE_DIR}/${APP_NAME}-notarize.zip"
    ditto -c -k --sequesterRsrc --keepParent "${APP_DIR}" "${SUBMIT_ZIP}"
    notarize_and_staple "${SUBMIT_ZIP}" "${APP_DIR}" || exit 1
    rm -f "${SUBMIT_ZIP}"
    # What a user's Mac will actually decide. Expect:
    #   accepted / source=Notarized Developer ID
    spctl --assess --type execute --verbose=4 "${APP_DIR}" 2>&1 | sed 's/^/  spctl: /' >&2 || true
fi

mkdir -p "${OUTPUT_DIR}"

# Update-channel artifact: a ditto zip of the SAME signed (and, when enabled,
# stapled) bundle. The auto-updater downloads this (extracts with one `ditto -x`,
# no hdiutil mount) and verifies its Ed25519 signature before installing. The DMG
# below stays the human first-install download. Skip with PACKAGE_ZIP=0.
ZIP_PATH="${OUTPUT_DIR}/${APP_NAME}-${VERSION}-${ARCH}.zip"
if [[ "${PACKAGE_ZIP:-1}" != "0" ]]; then
    ditto -c -k --sequesterRsrc --keepParent "${APP_DIR}" "${ZIP_PATH}"
fi

ln -s /Applications "${DMG_ROOT}/Applications"

DMG_NAME="${APP_NAME}-${VERSION}-${ARCH}.dmg"
DMG_PATH="${OUTPUT_DIR}/${DMG_NAME}"

# Styled installer window. The art (assets/dmg/background.tiff, a HiDPI 1x/2x
# pair) plus the icon layout give the "drag Liu to Applications" window its
# branded look. dmgbuild — which writes the .DS_Store (background + window +
# icon positions) directly — is used rather than Finder/AppleScript: the
# AppleScript `background picture` property is broken on recent macOS (Tahoe
# / 26.x), where icon positions apply but the background is silently dropped.
# Geometry lives in scripts/dmg_build_settings.py and must match the 640x400
# canvas the art is drawn at (see scripts/dmg_render_background.sh).
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
    # Hash-pinned: this runs in the job that holds the signing identity, so
    # PyPI does not get to choose what executes here. No pip upgrade
    # either — the venv's pip is whatever the runner's Python ships.
    "${venv}/bin/pip" install -q --require-hashes \
        -r "${ROOT_DIR}/scripts/dmgbuild-requirements.txt" >/dev/null 2>&1 || return 1
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
            -srcfolder "${DMG_ROOT}" \
            -ov \
            -format UDZO \
            "${DMG_PATH}" >/dev/null
    fi

    # The app inside is already notarized+stapled, but the DMG is its own
    # distributable container: Gatekeeper assesses the disk image the user
    # double-clicks, so it needs its own signature and its own ticket. No
    # --options runtime here — a disk image isn't code, it only gets a seal.
    if [[ "${CODESIGN_IDENTITY}" != "-" ]]; then
        codesign --force --sign "${CODESIGN_IDENTITY}" ${KEYCHAIN_OPTS[@]+"${KEYCHAIN_OPTS[@]}"} \
            --timestamp "${DMG_PATH}" \
            || { echo "error: codesign ${DMG_NAME} failed." >&2; exit 1; }
    fi
    if [[ "${NOTARIZE_ENABLED}" == "1" ]]; then
        notarize_and_staple "${DMG_PATH}" "${DMG_PATH}" || exit 1
        # Disk images are assessed under the 'open' policy, not 'execute'.
        spctl --assess --type open --context context:primary-signature \
            --verbose=4 "${DMG_PATH}" 2>&1 | sed 's/^/  spctl: /' >&2 || true
    fi
fi

# Final line(s) of stdout are the integration handle for release.sh / CI.
[[ "${PACKAGE_DMG:-1}" != "0" ]] && echo "${DMG_PATH}"
[[ "${PACKAGE_ZIP:-1}" != "0" ]] && echo "${ZIP_PATH}"
