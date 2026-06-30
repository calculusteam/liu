#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_DIR="${ROOT_DIR}/third_party/macos-deps"
SRC_DIR="${DEPS_DIR}/src"
BUILD_DIR="${DEPS_DIR}/build"
INSTALL_DIR="${DEPS_DIR}/install"
OPENSSL_VERSION="${OPENSSL_VERSION:-3.6.1}"
LIBSSH2_VERSION="${LIBSSH2_VERSION:-1.11.1}"
MACOS_MIN="${MACOS_MIN:-11.0}"
ARCH="${ARCH:-$(uname -m)}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

OPENSSL_TARBALL="openssl-${OPENSSL_VERSION}.tar.gz"
OPENSSL_URL="https://www.openssl.org/source/${OPENSSL_TARBALL}"
LIBSSH2_TARBALL="libssh2-${LIBSSH2_VERSION}.tar.gz"
LIBSSH2_URL="https://libssh2.org/download/${LIBSSH2_TARBALL}"

mkdir -p "${SRC_DIR}" "${BUILD_DIR}" "${INSTALL_DIR}"

download_if_missing() {
    local url="$1"
    local out="$2"
    if [[ ! -f "${out}" ]]; then
        curl -L --fail --retry 3 -o "${out}" "${url}"
    fi
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
    ./Configure "${openssl_target}" no-shared no-tests no-module \
        --prefix="${INSTALL_DIR}" \
        --openssldir="${INSTALL_DIR}/ssl"
    make -j"${JOBS}"
    make install_sw
    popd >/dev/null
}

build_libssh2() {
    local src="${BUILD_DIR}/libssh2-${LIBSSH2_VERSION}"
    local bld="${src}/build"
    extract_clean "${SRC_DIR}/${LIBSSH2_TARBALL}" "${src}"
    cmake -S "${src}" -B "${bld}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
        -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOS_MIN}" \
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

download_if_missing "${OPENSSL_URL}" "${SRC_DIR}/${OPENSSL_TARBALL}"
download_if_missing "${LIBSSH2_URL}" "${SRC_DIR}/${LIBSSH2_TARBALL}"
build_openssl
build_libssh2

echo "Built compatible deps into ${INSTALL_DIR}"
