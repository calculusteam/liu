#!/usr/bin/env bash
#
# package_linux.sh — the Linux release artifacts: the tarball, a .deb, an .rpm
# and an AppImage, each holding the same three binaries.
#
#   scripts/package_linux.sh                    # the tarball + each format whose tools are here
#   scripts/package_linux.sh --formats tarball,deb,rpm,appimage
#                                               # every format named is required
#
# `build.sh --package` dispatches here on Linux with BUILD_DIR / BUILD_TYPE /
# JOBS in the environment and reports every line at the end of stdout that is a
# file: the artifacts, one per line. Everything else goes to stderr.
#
# The tarball is made first, by package_linux_tarball.sh (which builds, and is
# the contract install.sh and the in-app updater read), and the other three are
# made OUT OF IT: the .deb, the .rpm and the AppImage cannot carry a second
# build. LIU_TARBALL=<path> packages a tarball that already exists instead.
#
# THE INSTALL LAYOUT. One tree, whatever installs it:
#
#   <prefix>/bin/Liu, bin/liu-notify, bin/liu-history
#   <prefix>/assets/                     the fonts are mandatory: <exe>/../assets/fonts
#   <prefix>/share/terminfo/x/xterm-liu  TERM=xterm-liu for a pane: <exe>/../share/terminfo
#   <prefix>/VERSION, liu.terminfo, LICENSE, NOTICE, THIRD_PARTY_NOTICES.md
#
# <prefix> is /opt/liu for the .deb and the .rpm (install.sh's default too) and
# usr/ inside the AppImage. It is the tree install.sh and the updater write
# ($PREFIX/bin/Liu beside $PREFIX/assets), so every resolver that starts from
# the binary's own path (/proc/self/exe: fonts, sounds, the window icon,
# terminfo, liu-notify for the agents' hooks) finds the same files in every
# format. Not /usr/bin/Liu with the data in /usr/share/liu: a pane's PATH is
# rebuilt WITHOUT the directory the running binary is in (ssh_session.c,
# local_env_build), so a binary in /usr/bin would take /usr/bin out of every
# shell Liu opens. And /opt/liu is not writable by the user, so the updater
# leaves a package's files to the package manager (updater_plat_can_autoinstall)
# and offers the release page instead.
#
# What the .deb and the .rpm add outside the prefix:
#
#   /usr/bin/liu, liu-notify, liu-history   links into /opt/liu/bin
#   /usr/share/applications/liu.desktop     Exec=liu, Icon=liu, StartupWMClass=liu
#   /usr/share/icons/hicolor/NxN/apps/liu.png   `notify-send --icon=liu`, the launcher
#   /usr/share/terminfo/x/xterm-liu          `infocmp xterm-liu`, and TERM=xterm-liu
#                                            outside Liu
#   /usr/share/doc/liu/copyright             (the .deb)
#
# The icon cache and the desktop database are refreshed by the distributions'
# own triggers (hicolor-icon-theme, desktop-file-utils), so neither package has
# a maintainer script.
#
# Dependencies are read off the binaries, not written by hand: dpkg-shlibdeps
# names the package that provides each NEEDED library at the version the build
# linked against (the release is built on Ubuntu 22.04, whose names Debian 12
# shares and Ubuntu 24.04's t64 packages Provide), and rpmbuild's ELF
# generator requires each soname with its symbol versions. The one written by
# hand is curl, which Liu RUNS rather than links (/usr/bin/curl: the updater's
# check, Translate and Voice over an API, the model catalog and downloads): a
# hard dependency, since an install without recommended packages
# (--no-install-recommends, no weak deps) has none on a minimal system.
#
# The AppImage carries the prefix as usr/, with AppRun a link to usr/bin/Liu,
# and one library of its own: libssh2, which a desktop seldom has installed.
# Its copy of Liu is given RUNPATH $ORIGIN/../lib to find it — the only change
# from the tarball's binary. Everything else (OpenSSL 3, X11, GL, fontconfig,
# FreeType, HarfBuzz, D-Bus) is the system's, which keeps its security updates,
# and so is /usr/bin/curl, which an AppImage cannot ask for.
# The runtime is AppImage's type2-runtime, pinned by release and sha256 below
# (LIU_APPIMAGE_RUNTIME=<file> uses a copy already here, checked the same way);
# the image is squashfs (zstd) from the distribution's squashfs-tools.
#
# Nothing here signs anything: the tarball (the updater's artifact) is
# Ed25519-signed afterwards, and every file is listed in the signed manifest.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
OUTPUT_DIR="${OUTPUT_DIR:-${BUILD_DIR}/package}"
export BUILD_DIR OUTPUT_DIR

APPIMAGE_RUNTIME_RELEASE="20251108"
APPIMAGE_RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/${APPIMAGE_RUNTIME_RELEASE}"
appimage_runtime_sha256() {
    case "$1" in
        aarch64) echo "00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444" ;;
        x86_64)  echo "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d" ;;
        *)       return 1 ;;
    esac
}

MAINTAINER="calculus.team <hello@liu.software>"
HOMEPAGE="https://liu.software"
SUMMARY="A terminal for AI coding agents"
DESCRIPTION="Liu runs AI coding agents' command-line tools in panes that know each
agent's state, keeps their session history to resume a session or continue it
in another agent, tells you when an agent is waiting for you, and puts SSH and
SFTP, a file browser with an editor and a Markdown reader, and a browser pane
beside them."

note() { printf '%s\n' "$*" >&2; }
die()  { printf 'package_linux: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

[[ "$(uname -s)" == "Linux" ]] || die "the Linux packages can only be made on Linux."

# --- which formats -------------------------------------------------------------
REQUESTED=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --formats)   REQUESTED="${2:?--formats needs a list}"; shift 2 ;;
        --formats=*) REQUESTED="${1#--formats=}"; shift ;;
        -h|--help)   sed -n '3,12p' "$0"; exit 0 ;;
        *)           die "unknown argument: $1 (see --help)" ;;
    esac
done

# What each format needs on this machine.
tools_for() {
    case "$1" in
        tarball)  echo "tar python3" ;;
        deb)      echo "dpkg-deb dpkg-shlibdeps" ;;
        rpm)      echo "rpmbuild" ;;
        appimage) echo "mksquashfs patchelf ldd sha256sum" ;;
    esac
}
missing_tools() {
    local t out=""
    for t in $(tools_for "$1"); do have "$t" || out="${out} ${t}"; done
    printf '%s' "${out# }"
}

FORMATS=()
if [[ -n "${REQUESTED}" ]]; then
    IFS=',' read -r -a FORMATS <<<"${REQUESTED}"
    for f in "${FORMATS[@]}"; do
        case "$f" in tarball|deb|rpm|appimage) ;; *) die "unknown format '$f' (tarball, deb, rpm, appimage)" ;; esac
        m="$(missing_tools "$f")"
        [[ -z "$m" ]] || die "$f was asked for and needs: $m"
    done
else
    FORMATS=(tarball)
    for f in deb rpm appimage; do
        m="$(missing_tools "$f")"
        if [[ -z "$m" ]]; then FORMATS+=("$f"); else note "skipping $f (no $m here)"; fi
    done
fi
want() { local f; for f in "${FORMATS[@]}"; do [[ "$f" == "$1" ]] && return 0; done; return 1; }

# --- 1. the tarball --------------------------------------------------------------
WORK="$(mktemp -d "${TMPDIR:-/tmp}/liu.pkg.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

if [[ -n "${LIU_TARBALL:-}" ]]; then
    TARBALL="${LIU_TARBALL}"
    [[ -f "${TARBALL}" ]] || die "LIU_TARBALL=${TARBALL} is not a file."
else
    # Its build output stays visible; its last line is the tarball's path.
    "${ROOT_DIR}/scripts/package_linux_tarball.sh" | tee "${WORK}/tarball.log" >&2
    TARBALL="$(tail -n 1 "${WORK}/tarball.log")"
    [[ -f "${TARBALL}" ]] || die "package_linux_tarball.sh reported no tarball."
fi
TARBALL="$(cd "$(dirname "${TARBALL}")" && pwd)/$(basename "${TARBALL}")"

NAME="$(basename "${TARBALL}" .tar.gz)"
[[ "${NAME}" =~ ^liu-(.+)-linux-(arm64|x86_64)$ ]] \
    || die "${TARBALL} is not named liu-<version>-linux-<arm64|x86_64>.tar.gz"
PKG_VERSION="${BASH_REMATCH[1]}"
ARCH="${BASH_REMATCH[2]}"
case "${ARCH}" in
    arm64)  DEB_ARCH=arm64; RPM_ARCH=aarch64; APP_ARCH=aarch64 ;;
    x86_64) DEB_ARCH=amd64; RPM_ARCH=x86_64;  APP_ARCH=x86_64 ;;
esac
# A prerelease ("0.2.0-beta.1") sorts BEFORE its release in semver; in dpkg and
# rpm a "-" means something else, and "~" is the spelling that sorts before.
# A "+<hash>" dev suffix is legal in both and sorts after.
SYS_VERSION="${PKG_VERSION//-/\~}"

mkdir -p "${WORK}/x"
tar -xzf "${TARBALL}" -C "${WORK}/x"
TREE="$(find "${WORK}/x" -mindepth 1 -maxdepth 1 -type d)"
[[ -n "${TREE}" && "$(printf '%s\n' "${TREE}" | wc -l)" -eq 1 && -f "${TREE}/Liu" ]] \
    || die "${TARBALL} does not hold one top-level tree with Liu in it."
for need in liu-notify liu-history assets/fonts/JetBrainsMonoNerdFontMono-Regular.ttf VERSION LICENSE NOTICE \
            THIRD_PARTY_NOTICES.md share/applications/liu.desktop \
            share/icons/hicolor/256x256/apps/liu.png; do
    [[ -e "${TREE}/${need}" ]] || die "${TARBALL} has no ${need}."
done
TERMINFO_ENTRY="$(cd "${TREE}/share/terminfo" && find . -type f -name xterm-liu | head -n 1)"
[[ -n "${TERMINFO_ENTRY}" ]] || die "${TARBALL} has no compiled xterm-liu under share/terminfo."
TERMINFO_ENTRY="${TERMINFO_ENTRY#./}"

if have desktop-file-validate; then
    desktop-file-validate "${TREE}/share/applications/liu.desktop" >&2 \
        || die "share/applications/liu.desktop does not validate."
fi

# Every file the packages write gets one date, the last commit's when there is
# one, so a rebuilt package differs only where its contents do.
if [[ -z "${SOURCE_DATE_EPOCH:-}" ]]; then
    SOURCE_DATE_EPOCH="$(git -C "${ROOT_DIR}" log -1 --format=%ct 2>/dev/null || echo 0)"
fi
export SOURCE_DATE_EPOCH

# The prefix tree (see the top of this file) at $1.
stage_prefix() {
    local p="$1"
    mkdir -p "${p}/bin" "${p}/share"
    cp -p "${TREE}/Liu" "${TREE}/liu-notify" "${TREE}/liu-history" "${p}/bin/"
    cp -pR "${TREE}/assets" "${p}/assets"
    cp -pR "${TREE}/share/terminfo" "${p}/share/terminfo"
    cp -p "${TREE}/VERSION" "${TREE}/LICENSE" "${TREE}/NOTICE" "${TREE}/THIRD_PARTY_NOTICES.md" "${p}/"
    [[ -f "${TREE}/liu.terminfo" ]] && cp -p "${TREE}/liu.terminfo" "${p}/"
    return 0
}

# /usr/bin links, the desktop entry, the icons and the terminfo entry under $1.
stage_system() {
    local r="$1"
    mkdir -p "${r}/usr/bin" "${r}/usr/share/applications" "${r}/usr/share/terminfo/$(dirname "${TERMINFO_ENTRY}")"
    ln -s /opt/liu/bin/Liu         "${r}/usr/bin/liu"
    ln -s /opt/liu/bin/liu-notify  "${r}/usr/bin/liu-notify"
    ln -s /opt/liu/bin/liu-history "${r}/usr/bin/liu-history"
    cp -p "${TREE}/share/applications/liu.desktop" "${r}/usr/share/applications/liu.desktop"
    cp -pR "${TREE}/share/icons" "${r}/usr/share/icons"
    cp -p "${TREE}/share/terminfo/${TERMINFO_ENTRY}" "${r}/usr/share/terminfo/${TERMINFO_ENTRY}"
}

# Modes a package manager expects, whatever the umask was, and one date.
normalise_tree() {
    local r="$1"
    find "${r}" -type d -exec chmod 0755 {} +
    find "${r}" -type f -exec chmod 0644 {} +
    find "${r}" -type f \( -name Liu -o -name liu-notify -o -name liu-history \) -path '*/bin/*' -exec chmod 0755 {} +
    find "${r}" -exec touch -h -d "@${SOURCE_DATE_EPOCH}" {} +
}

ARTIFACTS=()
want tarball && ARTIFACTS+=("${TARBALL}")
mkdir -p "${OUTPUT_DIR}"

# --- 2. the .deb ------------------------------------------------------------------
if want deb; then
    R="${WORK}/deb"
    stage_prefix "${R}/opt/liu"
    stage_system "${R}"
    mkdir -p "${R}/usr/share/doc/liu"
    {
        cat "${TREE}/NOTICE"
        printf '\nLiu is licensed under the Apache License 2.0: /opt/liu/LICENSE.\n'
        printf 'What it contains under other licences: /opt/liu/THIRD_PARTY_NOTICES.md.\n'
    } > "${R}/usr/share/doc/liu/copyright"

    # dpkg-shlibdeps reads the package of each NEEDED library out of the dpkg
    # database; it wants a debian/control to name the package it works for.
    mkdir -p "${WORK}/shlibs/debian"
    printf 'Source: liu\n\nPackage: liu\nArchitecture: any\n' > "${WORK}/shlibs/debian/control"
    DEPENDS="$(cd "${WORK}/shlibs" && dpkg-shlibdeps -O \
                 -e"${R}/opt/liu/bin/Liu" -e"${R}/opt/liu/bin/liu-notify" -e"${R}/opt/liu/bin/liu-history" \
                 2>"${WORK}/shlibdeps.err" | sed -n 's/^shlibs:Depends=//p')"
    [[ -n "${DEPENDS}" ]] || { cat "${WORK}/shlibdeps.err" >&2; die "dpkg-shlibdeps named no dependency."; }

    normalise_tree "${R}"
    mkdir -p "${R}/DEBIAN"
    {
        printf 'Package: liu\n'
        printf 'Version: %s\n' "${SYS_VERSION}"
        printf 'Architecture: %s\n' "${DEB_ARCH}"
        printf 'Maintainer: %s\n' "${MAINTAINER}"
        printf 'Installed-Size: %s\n' "$(du -sk --exclude=DEBIAN "${R}" | cut -f1)"
        printf 'Depends: %s, curl\n' "${DEPENDS}"
        printf 'Recommends: libnotify-bin, xdg-utils\n'
        printf 'Suggests: zenity | kdialog, pulseaudio-utils | alsa-utils, espeak-ng\n'
        printf 'Section: x11\n'
        printf 'Priority: optional\n'
        printf 'Homepage: %s\n' "${HOMEPAGE}"
        printf 'Description: %s\n' "${SUMMARY}"
        printf '%s\n' "${DESCRIPTION}" | sed 's/^/ /'
    } > "${R}/DEBIAN/control"
    chmod 0644 "${R}/DEBIAN/control"
    touch -d "@${SOURCE_DATE_EPOCH}" "${R}/DEBIAN" "${R}/DEBIAN/control"

    DEB="${OUTPUT_DIR}/liu_${SYS_VERSION}_${DEB_ARCH}.deb"
    rm -f "${DEB}"
    # xz, not the zstd Ubuntu's dpkg writes by default: Debian's dpkg before
    # 1.21.18 cannot read a zstd member.
    dpkg-deb --root-owner-group -Zxz --build "${R}" "${DEB}" >&2
    dpkg-deb --field "${DEB}" Package Version Architecture Depends >&2
    ARTIFACTS+=("${DEB}")
fi

# --- 3. the .rpm ------------------------------------------------------------------
if want rpm; then
    R="${WORK}/rpmroot"
    stage_prefix "${R}/opt/liu"
    stage_system "${R}"
    normalise_tree "${R}"

    T="${WORK}/rpmbuild"
    mkdir -p "${T}"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
    SPEC="${T}/SPECS/liu.spec"
    {
        printf 'Name:      liu\n'
        printf 'Version:   %s\n' "${SYS_VERSION}"
        printf 'Release:   1\n'
        printf 'Summary:   %s\n' "${SUMMARY}"
        printf 'License:   Apache-2.0\n'
        printf 'URL:       %s\n' "${HOMEPAGE}"
        printf 'Packager:  %s\n' "${MAINTAINER}"
        # The file, not a package name: Fedora's curl and curl-minimal both
        # provide it, and it is the path Liu runs.
        printf 'Requires:  /usr/bin/curl\n'
        # The hicolor directories the icons go into are that package's (with
        # the index.theme that makes Icon=liu resolve); without it, removing
        # Liu left them behind, owned by no package.
        printf 'Requires:  hicolor-icon-theme\n'
        printf 'Recommends: libnotify\nRecommends: xdg-utils\n'
        printf 'Suggests:  zenity\nSuggests:  pulseaudio-utils\nSuggests:  espeak-ng\n'
        # The binaries are the tarball's, already stripped and checked: no
        # debuginfo split, no strip, no build-id links that two versions'
        # packages would both own, nothing rewritten after %install.
        printf '%%global debug_package %%{nil}\n'
        printf '%%global __os_install_post %%{nil}\n'
        printf '%%global __arch_install_post %%{nil}\n'
        printf '%%global _build_id_links none\n'
        printf '%%global _binary_payload w6.xzdio\n'
        printf '%%global _buildhost liu-release\n'
        printf '%%global use_source_date_epoch_as_buildtime 1\n'
        printf '%%global clamp_mtime_to_source_date_epoch 1\n'
        printf '\n%%description\n%s\n' "${DESCRIPTION}"
        printf '\n%%install\nmkdir -p %%{buildroot}\ncp -a "%s"/. %%{buildroot}/\n' "${R}"
        printf '\n%%files\n'
        printf '/opt/liu\n/usr/bin/liu\n/usr/bin/liu-notify\n/usr/bin/liu-history\n'
        printf '/usr/share/applications/liu.desktop\n'
        (cd "${R}" && find usr/share/icons -type f | sort | sed 's|^|/|')
        printf '/usr/share/terminfo/%s\n' "${TERMINFO_ENTRY}"
    } > "${SPEC}"

    rpmbuild -bb --target "${RPM_ARCH}" --define "_topdir ${T}" "${SPEC}" >&2
    BUILT="$(find "${T}/RPMS" -name '*.rpm' -print -quit)"
    [[ -n "${BUILT}" ]] || die "rpmbuild produced no package."
    RPM="${OUTPUT_DIR}/$(basename "${BUILT}")"
    # rpmbuild writes the package 0600; a release asset is world-readable.
    install -m 0644 "${BUILT}" "${RPM}"
    rpm -qp --requires "${RPM}" >&2
    ARTIFACTS+=("${RPM}")
fi

# --- 4. the AppImage ----------------------------------------------------------------
if want appimage; then
    AD="${WORK}/Liu.AppDir"
    stage_prefix "${AD}/usr"
    mkdir -p "${AD}/usr/share/applications" "${AD}/usr/lib"
    cp -p "${TREE}/share/applications/liu.desktop" "${AD}/usr/share/applications/liu.desktop"
    cp -pR "${TREE}/share/icons" "${AD}/usr/share/icons"
    cp -p "${TREE}/share/applications/liu.desktop" "${AD}/liu.desktop"
    cp -p "${TREE}/share/icons/hicolor/256x256/apps/liu.png" "${AD}/liu.png"
    ln -s liu.png "${AD}/.DirIcon"
    ln -s usr/bin/Liu "${AD}/AppRun"

    # libssh2, the library a desktop is least likely to have, rides along; the
    # loader finds it through the RUNPATH given to this copy of Liu only.
    SSH2="$(ldd "${AD}/usr/bin/Liu" | awk '$1 == "libssh2.so.1" { print $3 }')"
    [[ -n "${SSH2}" && -f "${SSH2}" ]] || die "Liu does not resolve libssh2.so.1 on this machine."
    cp -L "${SSH2}" "${AD}/usr/lib/libssh2.so.1"
    patchelf --set-rpath '$ORIGIN/../lib' "${AD}/usr/bin/Liu"
    normalise_tree "${AD}"
    chmod 0755 "${AD}/usr/lib/libssh2.so.1"
    LOADED="$(ldd "${AD}/usr/bin/Liu" | awk '$1 == "libssh2.so.1" { print $3 }')"
    [[ -n "${LOADED}" && "$(readlink -f "${LOADED}")" == "$(readlink -f "${AD}/usr/lib/libssh2.so.1")" ]] \
        || die "the AppImage's Liu loads libssh2 from ${LOADED:-nowhere}, not its own copy."

    RUNTIME="${LIU_APPIMAGE_RUNTIME:-}"
    WANT_SHA="$(appimage_runtime_sha256 "${APP_ARCH}")" || die "no AppImage runtime is pinned for ${APP_ARCH}."
    if [[ -z "${RUNTIME}" ]]; then
        have curl || die "curl is needed to fetch the AppImage runtime (or set LIU_APPIMAGE_RUNTIME)."
        RUNTIME="${WORK}/runtime-${APP_ARCH}"
        curl -fsSL --proto '=https' --proto-redir '=https' --retry 3 \
            -o "${RUNTIME}" "${APPIMAGE_RUNTIME_URL}/runtime-${APP_ARCH}"
    fi
    GOT_SHA="$(sha256sum "${RUNTIME}" | awk '{print $1}')"
    [[ "${GOT_SHA}" == "${WANT_SHA}" ]] \
        || die "the AppImage runtime ${RUNTIME} is sha256 ${GOT_SHA}, pinned ${WANT_SHA} (type2-runtime ${APPIMAGE_RUNTIME_RELEASE})."

    IMAGE="${WORK}/Liu.squashfs"
    mksquashfs "${AD}" "${IMAGE}" -all-root -noappend -no-xattrs -comp zstd -quiet -no-progress >&2
    APPIMAGE="${OUTPUT_DIR}/Liu-${PKG_VERSION}-${APP_ARCH}.AppImage"
    cat "${RUNTIME}" "${IMAGE}" > "${APPIMAGE}"
    chmod 0755 "${APPIMAGE}"
    ARTIFACTS+=("${APPIMAGE}")
fi

note "Liu ${PKG_VERSION} (${ARCH}) packaged for Linux: ${FORMATS[*]}"
printf '%s\n' "${ARTIFACTS[@]}"
