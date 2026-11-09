#!/usr/bin/env bash
#
# build_linux_libssh2.sh <out-dir> — the libssh2 a Linux package carries in
# itself (the AppImage): libssh2 1.11.1 with every security backport in
# scripts/patches/libssh2/, built as a shared library against this system's
# OpenSSL and zlib. Writes <out-dir>/libssh2.so.1.
#
# A copy inside a package never receives the distribution's security updates,
# so it is not the build system's own library but the same patched release
# the macOS builds link. The patch list is checked the way
# scripts/build_macos_compat_deps.sh checks it: every patch listed in
# SHA256SUMS with its hash, every listed patch applied.
#
#   LIU_LIBSSH2_TARBALL   a libssh2-1.11.1.tar.gz to use instead of the copy
#                         under third_party/macos-deps/src or a download (its
#                         sha256 is checked all the same)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBSSH2_VERSION="1.11.1"
LIBSSH2_SHA256="d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7"
LIBSSH2_URL="https://libssh2.org/download/libssh2-${LIBSSH2_VERSION}.tar.gz"

die() { printf 'build_linux_libssh2: %s\n' "$*" >&2; exit 1; }
sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
    else shasum -a 256 "$1" | awk '{print $1}'
    fi
}

[[ $# -eq 1 ]] || die "usage: build_linux_libssh2.sh <out-dir>"
OUT="$1"
mkdir -p "${OUT}"
for t in cmake cc patch tar; do command -v "${t}" >/dev/null 2>&1 || die "${t} is needed"; done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/liu.libssh2.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

TARBALL="${LIU_LIBSSH2_TARBALL:-}"
if [[ -z "${TARBALL}" && -f "${ROOT_DIR}/third_party/macos-deps/src/libssh2-${LIBSSH2_VERSION}.tar.gz" ]]; then
    TARBALL="${ROOT_DIR}/third_party/macos-deps/src/libssh2-${LIBSSH2_VERSION}.tar.gz"
fi
if [[ -z "${TARBALL}" ]]; then
    command -v curl >/dev/null 2>&1 || die "curl is needed to fetch libssh2 (or set LIU_LIBSSH2_TARBALL)"
    TARBALL="${WORK}/libssh2-${LIBSSH2_VERSION}.tar.gz"
    curl -fsSL --proto '=https' --proto-redir '=https' --retry 3 -o "${TARBALL}" "${LIBSSH2_URL}"
fi
GOT="$(sha256 "${TARBALL}")"
[[ "${GOT}" == "${LIBSSH2_SHA256}" ]] || die "${TARBALL} is sha256 ${GOT}, expected ${LIBSSH2_SHA256}"

tar -xzf "${TARBALL}" -C "${WORK}"
SRC="${WORK}/libssh2-${LIBSSH2_VERSION}"
[[ -d "${SRC}" ]] || die "the tarball holds no libssh2-${LIBSSH2_VERSION}/"

PDIR="${ROOT_DIR}/scripts/patches/libssh2"
SUMS="${PDIR}/SHA256SUMS"
[[ -f "${SUMS}" ]] || die "${SUMS} is missing"
listed=0
while read -r want name; do
    [[ -z "${want}" ]] && continue
    name="${name#\*}"
    [[ -f "${PDIR}/${name}" ]] || die "listed patch ${name} is missing"
    [[ "$(sha256 "${PDIR}/${name}")" == "${want}" ]] || die "${name}: sha256 does not match SHA256SUMS"
    listed=$((listed + 1))
done < "${SUMS}"
applied=0
for f in "${PDIR}"/[0-9]*.patch; do
    name="$(basename "${f}")"
    grep -q "  ${name}$" "${SUMS}" || die "${name} is not listed in SHA256SUMS"
    patch -p1 --no-backup-if-mismatch -d "${SRC}" < "${f}" >/dev/null || die "${name} did not apply"
    applied=$((applied + 1))
done
[[ "${applied}" -eq "${listed}" && "${applied}" -gt 0 ]] || die "applied ${applied} patches, SHA256SUMS lists ${listed}"
printf 'libssh2 %s: applied %d security backports\n' "${LIBSSH2_VERSION}" "${applied}" >&2

cmake -S "${SRC}" -B "${WORK}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-ffile-prefix-map=${WORK}=." \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_STATIC_LIBS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTING=OFF \
    -DENABLE_ZLIB_COMPRESSION=ON \
    -DCRYPTO_BACKEND=OpenSSL >&2
cmake --build "${WORK}/build" -j"$(nproc 2>/dev/null || echo 4)" >&2

LIB="$(find "${WORK}/build" -name 'libssh2.so.1' -print -quit)"
[[ -n "${LIB}" ]] || die "the build made no libssh2.so.1"
cp -L "${LIB}" "${OUT}/libssh2.so.1"
chmod 0755 "${OUT}/libssh2.so.1"
# The library announces its version in its SSH banner.
grep -aq "libssh2_${LIBSSH2_VERSION}" "${OUT}/libssh2.so.1" || die "the library built is not libssh2 ${LIBSSH2_VERSION}"
printf '%s\n' "${OUT}/libssh2.so.1"
