#!/usr/bin/env bash
#
# package_linux_tarball.sh — the Linux release artifact.
#
# Counterpart to package_macos_dmg.sh, and driven the same way: `build.sh
# --package` dispatches here on Linux with BUILD_DIR / BUILD_TYPE / JOBS in the
# environment, and reads the artifact path off the LAST line of stdout.
#
# THE ARTIFACT CONTRACT is not this script's to invent — it already has two
# consumers, and they have to agree with it byte for byte or a hand-install and
# an in-app update land differently:
#
#   scripts/install.sh          extracts the tarball, finds `Liu` at maxdepth 3,
#                               and treats ITS directory as the tree root: the
#                               helpers sit beside it, `assets/` sits beside it,
#                               and the terminfo and icon are picked up from
#                               there too.
#   src/update/updater_linux.c  same tree, plus a `VERSION` file, which updater.c
#                               REQUIRES — it re-derives "is this really newer"
#                               from the artifact rather than trusting the feed,
#                               and refuses to install a tarball without one.
#
# So: one top-level directory holding the three ELFs, assets/, VERSION, the
# terminfo and an icon. No bin/ inside the tarball — install.sh is what creates
# $PREFIX/bin, and a bin/ here would leave the fonts one level too far away
# (the default face is resolved as <exe_dir>/../assets/fonts and is mandatory
# at runtime, so getting this wrong is a binary that will not start).
#
# share/ is the part of the tree a desktop reads, laid out as it is installed:
#   share/terminfo/x/xterm-liu         the compiled entry; the app hands it to a
#                                      pane through TERMINFO (ui.c,
#                                      liu_terminfo_dir: <exe>/share/terminfo
#                                      here, <exe>/../share/terminfo once the
#                                      binary is <prefix>/bin/Liu)
#   share/icons/hicolor/NxN/apps/liu.png  the icon by NAME — notify-send
#                                      --icon=liu, the .desktop entry's Icon=liu
#   share/applications/liu.desktop     the entry the packages install
# Both consumers carry share/ whole (install.sh copies it; the updater keeps
# "share" across an update), and scripts/package_linux.sh builds the .deb, the
# .rpm and the AppImage out of THIS tarball, so every format holds the same
# binaries.
#
# Nothing here signs anything: every artifact is Ed25519-signed after the fact
# with scripts/sign_ed25519.sh, as the macOS ones are.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/jobs.sh"   # liu_jobs: a validated, portable job count

BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
OUTPUT_DIR="${OUTPUT_DIR:-${BUILD_DIR}/package}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="$(liu_jobs)"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "The Linux tarball can only be packaged on Linux." >&2
    exit 1
fi

# Configure only if the caller has not already. build.sh always has, and reusing
# its cache is the point — the artifact must contain the binaries that were just
# built and verified, not a second configuration of them.
if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DUSE_METAL=OFF \
        -DLIU_DEV_HOOKS=OFF
fi

cmake --build "${BUILD_DIR}" -j"${JOBS}" --target Liu liu-notify liu-history

# Same version rule as the macOS packager: the canonical VERSION file, with
# +<shorthash> appended only off an exact tag, so a dev tarball is
# distinguishable while a release stays clean. The VERSION file written INTO the
# tree stays bare — updater.c parses it as a semver and compares it, and a
# "+hash" suffix there is not a version it can order.
VERSION="$(tr -d '[:space:]' < "${ROOT_DIR}/VERSION")"
# VERSION flows into output paths; reject anything but a semver token.
if [[ ! "${VERSION}" =~ ^[0-9]+(\.[0-9]+){0,2}(-[A-Za-z0-9.]+)?$ ]]; then
    echo "error: VERSION '${VERSION}' is not a valid semver token" >&2; exit 1
fi
MARKER_VERSION="${VERSION}"
if ! git -C "${ROOT_DIR}" describe --tags --exact-match >/dev/null 2>&1; then
    SHORTHASH="$(git -C "${ROOT_DIR}" rev-parse --short HEAD 2>/dev/null || echo dev)"
    VERSION="${VERSION}+${SHORTHASH}"
fi

case "$(uname -m)" in
    aarch64|arm64) ARCH="arm64" ;;
    x86_64|amd64)  ARCH="x86_64" ;;
    *)             ARCH="$(uname -m)" ;;
esac

NAME="liu-${VERSION}-linux-${ARCH}"
STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/liu.tar.XXXXXX")"
trap 'rm -rf "${STAGE_DIR}"' EXIT
TREE="${STAGE_DIR}/${NAME}"
mkdir -p "${TREE}"

# --- the three binaries, flat -------------------------------------------------
# Liu is mandatory; liu-notify has to sit BESIDE it (the agent hooks resolve
# <dir-of-Liu>/liu-notify by absolute path, so a missing one silently disables
# every notification), and liu-history backs the Agent History CLI.
for exe in Liu liu-notify liu-history; do
    src="${BUILD_DIR}/${exe}"
    if [[ ! -x "${src}" ]]; then
        src="$(find "${BUILD_DIR}" -maxdepth 3 -type f -name "${exe}" -perm -u+x 2>/dev/null | head -n 1)"
    fi
    if [[ -z "${src}" || ! -x "${src}" ]]; then
        echo "Could not find the built ${exe} in ${BUILD_DIR}." >&2
        exit 1
    fi
    cp -p "${src}" "${TREE}/${exe}"
    chmod +x "${TREE}/${exe}"
done

# Strip only in Release: a Debug tarball that has had its symbols removed is
# useless for the backtrace it exists to produce.
if [[ "${BUILD_TYPE}" == "Release" ]] && command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "${TREE}/Liu" "${TREE}/liu-notify" "${TREE}/liu-history" 2>/dev/null || true
fi

# Release archives must not include core/dev_hooks.h. The --version output
# identifies development builds before the app opens a display or reads its
# config directory, and this packaging step rejects them.
LIU_SAYS="$("${TREE}/Liu" --version)"
if [[ "${LIU_SAYS}" == *dev-hooks* ]]; then
    echo "${BUILD_DIR}/Liu was built with LIU_DEV_HOOKS (${LIU_SAYS}) — reconfigure with -DLIU_DEV_HOOKS=OFF." >&2
    exit 1
fi
# What the binaries ask of the machine they are copied to: the glibc floor, no
# baked-in library path, the optional features really linked.
"${ROOT_DIR}/scripts/check_linux_binaries.sh" "${TREE}/Liu" "${TREE}/liu-notify" "${TREE}/liu-history" >&2

# --- assets ------------------------------------------------------------------
# fonts/ is load-bearing rather than decorative, so its absence is fatal here
# instead of a warning at install time on the user's machine.
cp -pR "${ROOT_DIR}/assets" "${TREE}/assets"
if [[ ! -f "${TREE}/assets/fonts/JetBrainsMonoNerdFontMono-Regular.ttf" ]]; then
    echo "assets/fonts/JetBrainsMonoNerdFontMono-Regular.ttf is missing from the staged tree — Liu cannot start without it." >&2
    exit 1
fi
# The DMG backdrop art is macOS packaging material, not runtime data.
rm -rf "${TREE}/assets/dmg"

# --- terminfo, icon, VERSION -------------------------------------------------
# install.sh looks for these by name; each is optional there, so a rename here
# degrades quietly rather than failing, which is why they are copied explicitly
# instead of being swept up by a glob.
[[ -f "${ROOT_DIR}/liu.terminfo" ]] && cp -p "${ROOT_DIR}/liu.terminfo" "${TREE}/liu.terminfo"
# The licence, and the notices of what is linked in and bundled (fonts, icons):
# they travel with the binary. A stale notices file stops the package.
python3 "${ROOT_DIR}/scripts/gen_third_party_notices.py" --check >&2 \
    || { echo "THIRD_PARTY_NOTICES.md is out of date — run scripts/gen_third_party_notices.py and commit it." >&2; exit 1; }
cp -p "${ROOT_DIR}/LICENSE" "${ROOT_DIR}/NOTICE" "${ROOT_DIR}/THIRD_PARTY_NOTICES.md" "${TREE}/"
[[ -f "${ROOT_DIR}/assets/appicon-256.png" ]] && cp -p "${ROOT_DIR}/assets/appicon-256.png" "${TREE}/liu.png"
printf '%s\n' "${MARKER_VERSION}" > "${TREE}/VERSION"

# --- share/: terminfo, icons, desktop entry -----------------------------------
# The compiled entry comes from the build (CMake's liu_terminfo target, which
# Liu depends on, runs tic on liu.terminfo); tic is in every ncurses install, so
# a build without it is a broken build machine, not an optional feature.
if [[ ! -d "${BUILD_DIR}/terminfo" ]] ||
   [[ -z "$(find "${BUILD_DIR}/terminfo" -type f -name xterm-liu -print -quit)" ]]; then
    echo "${BUILD_DIR}/terminfo holds no compiled xterm-liu (was tic missing at configure?)." >&2
    exit 1
fi
mkdir -p "${TREE}/share/terminfo"
(cd "${BUILD_DIR}/terminfo" && find . -type f ! -name .stamp -print) | while IFS= read -r entry; do
    mkdir -p "${TREE}/share/terminfo/$(dirname "${entry}")"
    cp "${BUILD_DIR}/terminfo/${entry}" "${TREE}/share/terminfo/${entry}"
done
python3 "${ROOT_DIR}/scripts/gen_linux_icons.py" "${TREE}/share/icons/hicolor" >&2
mkdir -p "${TREE}/share/applications"
cp "${ROOT_DIR}/packaging/linux/liu.desktop" "${TREE}/share/applications/liu.desktop"

# --- the tarball --------------------------------------------------------------
mkdir -p "${OUTPUT_DIR}"
TARBALL="${OUTPUT_DIR}/${NAME}.tar.gz"
rm -f "${TARBALL}"
# --sort=name + a pinned mtime/owner make the archive reproducible: two builds
# of the same tree produce the same bytes, so the published sha256 can be
# reproduced by anyone. Older tars have neither flag, hence the probe.
TAR_FLAGS=(--owner=0 --group=0 --numeric-owner)
if tar --sort=name --version >/dev/null 2>&1; then
    TAR_FLAGS+=(--sort=name --mtime=@0)
fi
tar -czf "${TARBALL}" -C "${STAGE_DIR}" "${TAR_FLAGS[@]}" "${NAME}"

echo "Liu ${VERSION} (${ARCH}) packaged for Linux."
echo "${TARBALL}"
