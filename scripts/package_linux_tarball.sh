#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------------------------
# Liu — Linux tarball packager
#
# Builds a Release Liu (OpenGL/X11) + the liu-notify and liu-history helpers,
# then stages them with the runtime assets into a self-contained tree and
# tar.gz's it. The output is the exact artifact scripts/install.sh's Linux
# path expects (see install.sh ~line 184 and ~line 410):
#
#   liu-linux-<cpu>/                  <- top-level dir (the producer's choice)
#     Liu                            <- the GL/X11 ELF (argv-ignoring GUI)
#     liu-notify                     <- notify helper; MUST sit beside Liu so the
#                                       hooks resolve <dir-of-Liu>/liu-notify
#     liu-history                    <- agent-history CLI (takes args, --help OK)
#     liu.terminfo                   <- TERM=Liu source, tic-compiled by installer
#     liu.png                        <- desktop icon (installer matches liu*.png)
#     liu.desktop                    <- freedesktop entry (Exec is the bare cmd;
#                                       Liu ignores argv, so NO %F/%U field codes)
#     VERSION                        <- monotonic marker for the downgrade guard
#     assets/                        <- fonts (MANDATORY), sounds, icons, …
#       fonts/JetBrainsMono-Regular.ttf   resolved via <exe_dir>/../assets/fonts
#
# install.sh finds Liu with `find -name Liu`, then copies everything that sits
# in dirname(Liu): assets/, liu-notify, liu-history, liu.terminfo, the icon.
# Crucially it expects assets/ to be a SIBLING of the binary inside the tarball
# (it copies $SRC_ROOT/assets), so we keep the flat layout above rather than the
# /opt/liu split (bin/ + assets/) the installer materializes on disk.
#
# The feed arch token MUST be "linux-<cpu>" (install.sh rejects a bare
# "arm64"/"x86_64" for Linux). We name the tarball liu-<ver>-linux-<cpu>.tar.gz;
# gen_feed.py should publish it under arch "linux-<cpu>".
#
# Output (stdout, last line) is the tarball path — the integration handle for
# release.sh / gen_feed.py, mirroring package_macos_dmg.sh.
#
# Env:
#   BUILD_DIR    cmake build dir            (default: <root>/build_linux)
#   OUTPUT_DIR   where the tarball lands    (default: <build>/package)
#   BUILD_TYPE   cmake build type           (default: Release)
#   JOBS         parallel build jobs        (default: nproc)
#   STRIP=0      keep debug symbols (don't strip the shipped binaries)
#
# Linux build dependencies (see "Build deps" note at the bottom of this file):
#   cmake and a C11 compiler — clang/gcc's integrated assembler handles the
#   hand-written SIMD (x86-64 SSE2 *_x86_64.S on x86_64; arm64 NEON on ARM); no
#   separate NASM is needed. Plus the dev packages for: libssh2 (+ openssl/
#   libcrypto + zlib), X11, and OpenGL/GLX. Debian/Ubuntu one-liner:
#     sudo apt install build-essential cmake pkg-config \
#          libssh2-1-dev libssl-dev zlib1g-dev \
#          libx11-dev libgl1-mesa-dev
# ---------------------------------------------------------------------------

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build_linux}"
OUTPUT_DIR="${OUTPUT_DIR:-${BUILD_DIR}/package}"
APP_NAME="${APP_NAME:-Liu}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Linux tarball packaging is only supported on Linux." >&2
    exit 1
fi

err()  { printf 'Error: %s\n' "$1" >&2; exit 1; }
note() { printf '%s\n' "$1" >&2; }

command -v cmake >/dev/null 2>&1 || err "cmake is required."
command -v tar   >/dev/null 2>&1 || err "tar is required."

# --- CPU token (must match install.sh's `uname -m` -> arm64 / x86_64) ---------
case "$(uname -m)" in
    arm64|aarch64) CPU="arm64" ;;
    x86_64|amd64)  CPU="x86_64" ;;
    *) err "Unsupported architecture: $(uname -m)" ;;
esac
ARCH="linux-${CPU}"   # the feed arch token install.sh searches for

# --- configure (force the OpenGL/X11 backend; Metal is macOS-only) ------------
# On UNIX the CMakeLists branch picks platform_linux.c + X11 + OpenGL::GL
# automatically; USE_METAL only takes effect on APPLE, so no flag is needed to
# disable it here. We do a fresh configure if there's no cache.
if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    note "Configuring (${BUILD_TYPE}) in ${BUILD_DIR}…"
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
fi

note "Building Liu + liu-notify + liu-history (-j${JOBS})…"
cmake --build "${BUILD_DIR}" -j"${JOBS}" --target Liu liu-notify liu-history

# --- locate the three binaries (single-config generator => directly in build) -
find_bin() {  # find_bin <name>
    local n="$1" p=""
    if [[ -x "${BUILD_DIR}/${n}" ]]; then p="${BUILD_DIR}/${n}"; fi
    [[ -n "${p}" ]] || p="$(find "${BUILD_DIR}" -maxdepth 3 -type f -name "${n}" -perm -111 2>/dev/null | head -n 1)"
    printf '%s' "${p}"
}

LIU_BIN="$(find_bin "${APP_NAME}")"
[[ -n "${LIU_BIN}" && -x "${LIU_BIN}" ]] || err "could not find the built ${APP_NAME} binary in ${BUILD_DIR}."
NOTIFY_BIN="$(find_bin liu-notify)"
HISTORY_BIN="$(find_bin liu-history)"
[[ -n "${NOTIFY_BIN}"  && -x "${NOTIFY_BIN}"  ]] || note "warning: liu-notify not found — notify hooks won't work in the tarball."
[[ -n "${HISTORY_BIN}" && -x "${HISTORY_BIN}" ]] || note "warning: liu-history not found — the agent-history CLI won't ship."

# --- version (shared VERSION file; +<shorthash> when not on an exact tag) -----
VERSION="$(tr -d '[:space:]' < "${ROOT_DIR}/VERSION")"
# The on-disk VERSION marker must stay clean for install.sh's downgrade guard
# (it sort -V's it), so keep the bare value for the marker file.
MARKER_VERSION="${VERSION}"
if ! git -C "${ROOT_DIR}" describe --tags --exact-match >/dev/null 2>&1; then
    SHORTHASH="$(git -C "${ROOT_DIR}" rev-parse --short HEAD 2>/dev/null || echo dev)"
    VERSION="${VERSION}+${SHORTHASH}"
fi

# --- stage the self-contained tree -------------------------------------------
STAGE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/${APP_NAME}.tar.XXXXXX")"
trap 'rm -rf "${STAGE_ROOT}"' EXIT

TREE_NAME="liu-${ARCH}"            # top-level dir inside the tarball
PKG="${STAGE_ROOT}/${TREE_NAME}"
mkdir -p "${PKG}"

cp "${LIU_BIN}" "${PKG}/${APP_NAME}"
chmod +x "${PKG}/${APP_NAME}"
[[ -n "${NOTIFY_BIN}"  && -x "${NOTIFY_BIN}"  ]] && { cp "${NOTIFY_BIN}"  "${PKG}/liu-notify";  chmod +x "${PKG}/liu-notify"; }
[[ -n "${HISTORY_BIN}" && -x "${HISTORY_BIN}" ]] && { cp "${HISTORY_BIN}" "${PKG}/liu-history"; chmod +x "${PKG}/liu-history"; }

# Strip by default — these are -O3 LTO release binaries; debug info bloats the
# download. STRIP=0 keeps symbols for a profiling build.
if [[ "${STRIP:-1}" != "0" ]] && command -v strip >/dev/null 2>&1; then
    strip "${PKG}/${APP_NAME}" 2>/dev/null || true
    [[ -f "${PKG}/liu-notify"  ]] && strip "${PKG}/liu-notify"  2>/dev/null || true
    [[ -f "${PKG}/liu-history" ]] && strip "${PKG}/liu-history" 2>/dev/null || true
fi

# Assets (fonts are MANDATORY at runtime; the installer warns if absent).
[[ -d "${ROOT_DIR}/assets" ]] || err "assets/ tree not found at ${ROOT_DIR}/assets — fonts are mandatory."
cp -R "${ROOT_DIR}/assets" "${PKG}/assets"
[[ -f "${PKG}/assets/fonts/JetBrainsMono-Regular.ttf" ]] \
    || note "warning: assets/fonts/JetBrainsMono-Regular.ttf is missing — Liu may fail to start."

# Terminfo (TERM=Liu). install.sh copies $SRC_ROOT/liu.terminfo and tic -x's it.
[[ -f "${ROOT_DIR}/liu.terminfo" ]] && cp "${ROOT_DIR}/liu.terminfo" "${PKG}/liu.terminfo" \
    || note "warning: liu.terminfo not found — TERM=Liu won't resolve after install."

# Desktop icon. install.sh's ICON_SRC matches liu*.png / Liu*.png at maxdepth 2,
# so a top-level liu.png in the tree is found. assets/appicon.png is the bunny mark.
if [[ -f "${ROOT_DIR}/assets/appicon.png" ]]; then
    cp "${ROOT_DIR}/assets/appicon.png" "${PKG}/liu.png"
elif [[ -f "${ROOT_DIR}/assets/liu-logo.png" ]]; then
    cp "${ROOT_DIR}/assets/liu-logo.png" "${PKG}/liu.png"
else
    note "warning: no assets/appicon.png/assets/liu-logo.png — the .desktop entry will fall back to a generic icon."
fi

# A .desktop file. install.sh writes its OWN .desktop pointing at the final
# install prefix (it can't know that until install time), so this one is a
# convenience for anyone who untars and runs in place. Liu ignores argv, hence
# no %F/%U field codes; Exec is relative-resolved by the desktop env's PATH only
# if symlinked, so we leave Exec as the bare name and document the caveat.
cat > "${PKG}/liu.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Liu
GenericName=Terminal
Comment=AI-assisted terminal
Exec=Liu
Icon=liu
Terminal=false
Categories=System;TerminalEmulator;
StartupNotify=true
EOF

# Version marker (install.sh's Linux downgrade guard reads $PREFIX/VERSION).
printf '%s\n' "${MARKER_VERSION}" > "${PKG}/VERSION"

# --- tar it up ---------------------------------------------------------------
mkdir -p "${OUTPUT_DIR}"
TARBALL="${OUTPUT_DIR}/liu-${VERSION}-${ARCH}.tar.gz"
note "Packaging ${TARBALL}…"
# -C the stage root so the archive carries the clean liu-linux-<cpu>/ prefix and
# nothing of the mktemp path. Deterministic-ish: sort entries, drop owner names.
tar --sort=name \
    --owner=0 --group=0 --numeric-owner \
    -czf "${TARBALL}" -C "${STAGE_ROOT}" "${TREE_NAME}" \
    || err "tar failed."

note ""
note "Liu ${VERSION} (${ARCH}) packaged."
note "  Tarball:    ${TARBALL}"
note "  Feed arch:  ${ARCH}    (publish under this token; install.sh requires linux-<cpu>)"
note "  Top dir:    ${TREE_NAME}/   (Liu + liu-notify + liu-history + assets/ + liu.terminfo + liu.png)"

# Last stdout line = the integration handle for release.sh / gen_feed.py.
echo "${TARBALL}"
