#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/jobs.sh"   # liu_jobs: a validated, portable job count

DEPS_DIR="${ROOT_DIR}/third_party/macos-deps"
SRC_DIR="${DEPS_DIR}/src"
BUILD_DIR="${DEPS_DIR}/build"
INSTALL_DIR="${DEPS_DIR}/install"
OPENSSL_VERSION="${OPENSSL_VERSION:-3.6.5}"
LIBSSH2_VERSION="${LIBSSH2_VERSION:-1.11.1}"
MACOS_MIN="${MACOS_MIN:-11.0}"
ARCH="${ARCH:-$(uname -m)}"
JOBS="$(liu_jobs)"

OPENSSL_TARBALL="openssl-${OPENSSL_VERSION}.tar.gz"
OPENSSL_URL="https://www.openssl.org/source/${OPENSSL_TARBALL}"
LIBSSH2_TARBALL="libssh2-${LIBSSH2_VERSION}.tar.gz"
LIBSSH2_URL="https://libssh2.org/download/${LIBSSH2_TARBALL}"

# SHA-256 of the exact tarballs vendored under third_party/macos-deps/src. These
# libraries are statically linked into the shipped binary; the vendored copies are
# the source of truth and a download is only the fallback for a tree without them —
# which must produce the same bytes. Bumping a version means updating the hash
# beside it.
#
# OpenSSL 3.6.5 (29 Sep 2026): the sha256 is openssl.org's own .sha256 file and the
# GitHub release's copy, which agree.
OPENSSL_SHA256="${OPENSSL_SHA256:-a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98}"
LIBSSH2_SHA256="${LIBSSH2_SHA256:-d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7}"

# The prefix OpenSSL is configured with — see build_openssl. Not where anything
# is installed: /usr/lib is on the sealed system volume.
OPENSSL_BAKED_PREFIX="/usr/lib/liu-vendored-openssl"

# The install directory holds only what this script builds, so a rebuild starts
# from nothing: a header or library left by an older recipe must not be linked
# beside the new ones.
rm -rf "${INSTALL_DIR}"
mkdir -p "${SRC_DIR}" "${BUILD_DIR}" "${INSTALL_DIR}"

# Treat INSTALL_DIR as a cache keyed by this recipe and the libssh2 patch list. Stamp
# the completed libraries with their combined SHA-256; CMake rebuilds or stops when the
# stamp differs. A failed build leaves no stamp.
deps_stamp() {
    local a b
    a="$(shasum -a 256 "${BASH_SOURCE[0]}" | awk '{print $1}')"
    b="$(shasum -a 256 "${ROOT_DIR}/scripts/patches/libssh2/SHA256SUMS" | awk '{print $1}')"
    printf '%s%s' "${a}" "${b}" | shasum -a 256 | awk '{print $1}'
}
rm -f "${INSTALL_DIR}/.liu-deps-stamp"

verify_sha256() {
    local file="$1"
    local want="$2"
    local got
    got="$(shasum -a 256 "${file}" | awk '{print $1}')"
    if [[ "${got}" != "${want}" ]]; then
        echo "error: ${file}: sha256 ${got} != expected ${want}" >&2
        return 1
    fi
}

download_if_missing() {
    local url="$1"
    local out="$2"
    local sha="$3"
    if [[ ! -f "${out}" ]]; then
        curl -L --fail --retry 3 --proto '=https' --proto-redir '=https' \
             -o "${out}.part" "${url}"
        if ! verify_sha256 "${out}.part" "${sha}"; then
            rm -f "${out}.part"
            return 1
        fi
        mv "${out}.part" "${out}"
    fi
    verify_sha256 "${out}" "${sha}"
}

extract_clean() {
    local tarball="$1"
    local dest="$2"
    rm -rf "${dest}"
    mkdir -p "${dest}"
    tar -xzf "${tarball}" -C "${dest}" --strip-components=1
}

build_openssl() {
    local src="${BUILD_DIR}/openssl-${OPENSSL_VERSION}"
    extract_clean "${SRC_DIR}/${OPENSSL_TARBALL}" "${src}"
    pushd "${src}" >/dev/null
    case "${ARCH}" in
        arm64|aarch64) local openssl_target="darwin64-arm64-cc" ;;
        x86_64|amd64) local openssl_target="darwin64-x86_64-cc" ;;
        *)
            echo "Unsupported ARCH=${ARCH}. Use arm64 or x86_64." >&2
            exit 1
            ;;
    esac
    export CFLAGS="-arch ${ARCH} -mmacosx-version-min=${MACOS_MIN}"
    export CXXFLAGS="${CFLAGS}"
    export LDFLAGS="${CFLAGS}"
    # OpenSSL embeds OPENSSLDIR, ENGINESDIR, and MODULESDIR from its configure
    # prefix and normally reads OPENSSLDIR/openssl.cnf on the first algorithm
    # fetch. If the prefix points into the build volume, shipped binaries can
    # load configuration from that volume and disable algorithms such as SHA-256
    # and Ed25519.
    #
    # Use no-autoload-config and set OPENSSL_BAKED_PREFIX to a path on the
    # sealed system volume. Stage the libraries with DESTDIR and copy them to
    # INSTALL_DIR, where CMake and libssh2 find them. Run
    # scripts/check_vendored_openssl.sh before writing the install stamp.
    local stage="${BUILD_DIR}/openssl-stage"
    rm -rf "${stage}"
    ./Configure "${openssl_target}" no-shared no-tests no-module no-autoload-config \
        --prefix="${OPENSSL_BAKED_PREFIX}" \
        --openssldir="${OPENSSL_BAKED_PREFIX}/ssl"
    make -j"${JOBS}"
    make install_sw DESTDIR="${stage}"
    [[ -f "${stage}${OPENSSL_BAKED_PREFIX}/lib/libcrypto.a" ]] \
        || { echo "error: OpenSSL did not stage under ${stage}${OPENSSL_BAKED_PREFIX}" >&2; exit 1; }
    cp -R "${stage}${OPENSSL_BAKED_PREFIX}/." "${INSTALL_DIR}/"
    rm -rf "${stage}"
    popd >/dev/null
}

# Apply the security backports for libssh2 1.11.1 from scripts/patches/libssh2/. Every
# patch must appear in SHA256SUMS with a matching hash, and every listed patch must
# apply. Remove patches already included in an upstream version from the list.
apply_libssh2_patches() {
    local src="$1"
    local pdir="${ROOT_DIR}/scripts/patches/libssh2"
    local sums="${pdir}/SHA256SUMS"
    [[ -f "${sums}" ]] || { echo "error: ${sums} is missing" >&2; exit 1; }
    local listed=0 applied=0 f name want got
    while read -r want name; do
        [[ -z "${want}" ]] && continue
        name="${name#\*}"
        f="${pdir}/${name}"
        [[ -f "${f}" ]] || { echo "error: listed patch ${name} is missing" >&2; exit 1; }
        got="$(shasum -a 256 "${f}" | awk '{print $1}')"
        [[ "${got}" == "${want}" ]] || { echo "error: ${name}: sha256 ${got} != ${want}" >&2; exit 1; }
        listed=$((listed + 1))
    done < "${sums}"
    for f in "${pdir}"/[0-9]*.patch; do
        name="$(basename "${f}")"
        grep -q "  ${name}$" "${sums}" || { echo "error: ${name} is not listed in SHA256SUMS" >&2; exit 1; }
        patch -p1 --no-backup-if-mismatch -d "${src}" < "${f}" >/dev/null \
            || { echo "error: ${name} did not apply to libssh2-${LIBSSH2_VERSION}" >&2; exit 1; }
        applied=$((applied + 1))
    done
    [[ "${applied}" -eq "${listed}" && "${applied}" -gt 0 ]] \
        || { echo "error: applied ${applied} patches, SHA256SUMS lists ${listed}" >&2; exit 1; }
    echo "libssh2: applied ${applied} security backports"
}

build_libssh2() {
    local src="${BUILD_DIR}/libssh2-${LIBSSH2_VERSION}"
    local bld="${src}/build"
    extract_clean "${SRC_DIR}/${LIBSSH2_TARBALL}" "${src}"
    apply_libssh2_patches "${src}"
    cmake -S "${src}" -B "${bld}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
        -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOS_MIN}" \
        -DCMAKE_C_FLAGS="-ffile-prefix-map=${BUILD_DIR}=." \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_TESTING=OFF \
        -DENABLE_ZLIB_COMPRESSION=ON \
        -DCRYPTO_BACKEND=OpenSSL \
        -DOPENSSL_ROOT_DIR="${INSTALL_DIR}" \
        -DOPENSSL_USE_STATIC_LIBS=ON
    cmake --build "${bld}" -j"${JOBS}"
    cmake --install "${bld}"
}

download_if_missing "${OPENSSL_URL}" "${SRC_DIR}/${OPENSSL_TARBALL}" "${OPENSSL_SHA256}"
download_if_missing "${LIBSSH2_URL}" "${SRC_DIR}/${LIBSSH2_TARBALL}" "${LIBSSH2_SHA256}"
build_openssl
build_libssh2
# Write the install stamp only after confirming that the libraries contain
# no build-machine paths and do not load OpenSSL configuration implicitly.
OPENSSL_BAKED_PREFIX="${OPENSSL_BAKED_PREFIX}" \
    bash "${ROOT_DIR}/scripts/check_vendored_openssl.sh" "${INSTALL_DIR}" "${ARCH}"
deps_stamp > "${INSTALL_DIR}/.liu-deps-stamp"

echo "Built compatible deps into ${INSTALL_DIR}"
