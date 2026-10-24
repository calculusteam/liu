#!/usr/bin/env bash
#
# check_linux_binaries.sh — what a Linux release binary may ask of the machine
# it is copied to.
#
#   scripts/check_linux_binaries.sh [--glibc-floor X.Y] <ELF>...
#
# 1. The glibc floor. A binary records, per library, the newest symbol VERSION
#    it was linked against (.gnu.version_r). Linked on Ubuntu 24.04 it asks for
#    GLIBC_2.38 and the loader refuses to start it on 22.04 / Debian 12 with
#    "version `GLIBC_2.38' not found" — on a desktop launch, with no window and
#    no message. The release is built in an ubuntu:22.04 container (glibc 2.35)
#    so it runs on Ubuntu 22.04+, Debian 12+, Fedora 36+; this fails the build
#    when a binary needs anything newer, and names the symbols that do.
#    LIU_GLIBC_FLOOR overrides the default, for a package made for one machine.
# 2. No RPATH / RUNPATH. A release binary must not search a directory of the
#    build machine (or one relative to wherever the tarball was unpacked) for
#    its libraries; the packages rely on the system's.
# 3. Liu links what makes it the product. Fontconfig, FreeType, HarfBuzz,
#    D-Bus, XRandR and XInput2 are optional at configure time (CMakeLists.txt
#    builds without them, quietly); a release built where one was missing
#    would ship without per-codepoint font fallback, hinting, ligatures, the
#    taskbar badge and the screen-reader bridge, the panel's refresh rate or
#    smooth scrolling.
set -euo pipefail

FLOOR="${LIU_GLIBC_FLOOR:-2.35}"
if [[ "${1:-}" == "--glibc-floor" ]]; then FLOOR="${2:?--glibc-floor needs a version}"; shift 2; fi
[[ $# -gt 0 ]] || { echo "usage: check_linux_binaries.sh [--glibc-floor X.Y] <ELF>..." >&2; exit 2; }
[[ "${FLOOR}" =~ ^[0-9]+\.[0-9]+$ ]] || { echo "error: glibc floor '${FLOOR}' is not X.Y" >&2; exit 2; }
command -v readelf >/dev/null 2>&1 || { echo "error: readelf (binutils) is required" >&2; exit 2; }
command -v objdump >/dev/null 2>&1 || { echo "error: objdump (binutils) is required" >&2; exit 2; }

# sonames Liu must carry (rule 3), each with what is lost without it.
LIU_NEEDS=(
    "libfontconfig.so.1:font fallback per codepoint"
    "libfreetype.so.6:hinted glyph rasterisation"
    "libharfbuzz.so.0:ligature shaping"
    "libdbus-1.so.3:the taskbar badge and the screen-reader bridge (AT-SPI)"
    "libXrandr.so.2:the panel's refresh rate"
    "libXi.so.6:smooth (trackpad) scrolling"
    "libssh2.so.1:SSH and SFTP"
    "libcrypto.so.3:the vault and update verification (OpenSSL 3)"
)

# a <= b for dotted versions.
ver_le() { [[ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -n 1)" == "$1" ]]; }

fails=0
for bin in "$@"; do
    name="$(basename "${bin}")"
    if [[ ! -f "${bin}" ]] || ! readelf -h "${bin}" >/dev/null 2>&1; then
        echo "FAIL ${bin}: not an ELF file" >&2; fails=$((fails + 1)); continue
    fi

    # 1. the newest GLIBC_ version any library is asked for
    newest="$(readelf --version-info --wide "${bin}" \
        | grep -o 'Name: GLIBC_[0-9][0-9.]*' | sed 's/Name: GLIBC_//' | sort -V | tail -n 1)"
    if [[ -z "${newest}" ]]; then
        echo "FAIL ${name}: records no GLIBC_ version (not a dynamically linked glibc binary?)" >&2
        fails=$((fails + 1))
    elif ! ver_le "${newest}" "${FLOOR}"; then
        echo "FAIL ${name}: needs GLIBC_${newest}, above the floor ${FLOOR}. Symbols that ask for more:" >&2
        objdump -T "${bin}" | awk -v floor="${FLOOR}" '
            match($0, /GLIBC_[0-9][0-9.]*/) {
                v = substr($0, RSTART + 6, RLENGTH - 6)
                split(v, a, "."); split(floor, f, ".")
                if (a[1] + 0 > f[1] + 0 || (a[1] + 0 == f[1] + 0 && a[2] + 0 > f[2] + 0))
                    print "    " $NF "  (GLIBC_" v ")"
            }' | sort -u >&2
        fails=$((fails + 1))
    else
        echo "ok   ${name}: newest glibc symbol version ${newest} (floor ${FLOOR})"
    fi

    # 2. no library search path baked in
    rpath="$(readelf -d "${bin}" | grep -E '\((RPATH|RUNPATH)\)' || true)"
    if [[ -n "${rpath}" ]]; then
        echo "FAIL ${name}: carries a library search path: ${rpath}" >&2
        fails=$((fails + 1))
    fi

    # 3. the optional features, in the app
    if [[ "${name}" == "Liu" ]]; then
        needed="$(readelf -d "${bin}" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')"
        missing=0
        for entry in "${LIU_NEEDS[@]}"; do
            so="${entry%%:*}"
            if ! grep -qx "${so}" <<<"${needed}"; then
                echo "FAIL Liu: does not link ${so} — the build would ship without ${entry#*:}" >&2
                missing=$((missing + 1))
            fi
        done
        fails=$((fails + missing))
        if [[ ${missing} -eq 0 ]]; then echo "ok   Liu: links $(tr '\n' ' ' <<<"${needed}")"; fi
    fi
done

if [[ ${fails} -gt 0 ]]; then
    echo "check_linux_binaries: ${fails} problem(s)" >&2
    exit 1
fi
