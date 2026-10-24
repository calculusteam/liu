#!/usr/bin/env bash
# Check two properties of the vendored OpenSSL and libssh2 libraries used by
# release builds:
#
# 1. The libraries do not embed paths from the build environment.
# 2. OpenSSL does not read a configuration file unless explicitly configured to
#    do so. A file under OPENSSLDIR or $OPENSSL_CONF can disable built-in
#    algorithms, including SHA-256 and Ed25519.
#
# Usage: check_vendored_openssl.sh <install dir> [arm64|x86_64]
set -euo pipefail

INSTALL_DIR="${1:?usage: check_vendored_openssl.sh <install dir> [arch]}"
ARCH="${2:-$(uname -m)}"
BAKED="${OPENSSL_BAKED_PREFIX:-/usr/lib/liu-vendored-openssl}"

work="$(mktemp -d "${TMPDIR:-/tmp}/liu-ossl-check.XXXXXX")"
trap 'rm -rf "${work}"' EXIT

fail() { echo "check_vendored_openssl: $*" >&2; exit 1; }

# --- 1. no build-machine path -------------------------------------------------
for lib in libcrypto.a libssl.a libssh2.a; do
    f="${INSTALL_DIR}/lib/${lib}"
    [[ -f "${f}" ]] || fail "${f} is missing"
    hits="$(strings -a "${f}" | grep -E '^/(Volumes|Users|home|private/tmp|tmp|private/var/folders|var/folders)/' | sort -u | head -n 5 || true)"
    if [[ -n "${hits}" ]]; then
        echo "${hits}" >&2
        fail "${lib} names a path on the build machine (above)"
    fi
done

# --- 2. the directories compiled in, and no config read unasked ---------------
cat > "${work}/probe.c" <<'PROBE'
#include <stdio.h>
#include <string.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "dirs") == 0) {
        printf("%s\n%s\n%s\n", OpenSSL_version(OPENSSL_DIR),
               OpenSSL_version(OPENSSL_ENGINES_DIR), OpenSSL_version(OPENSSL_MODULES_DIR));
        return 0;
    }
    /* What the updater, the vault and SSH need first. */
    EVP_MD *md = EVP_MD_fetch(NULL, "SHA256", NULL);
    EVP_PKEY_CTX *ed = EVP_PKEY_CTX_new_from_name(NULL, "ED25519", NULL);
    printf("sha256=%s ed25519=%s\n", md ? "ok" : "MISSING", ed ? "ok" : "MISSING");
    return (md && ed) ? 0 : 3;
}
PROBE
cc -arch "${ARCH}" -O1 -I"${INSTALL_DIR}/include" "${work}/probe.c" \
   "${INSTALL_DIR}/lib/libcrypto.a" -o "${work}/probe" \
   || fail "could not build the probe against ${INSTALL_DIR}"

run_probe() {   # an x86_64 probe on Apple Silicon goes through Rosetta
    if [[ "${ARCH}" != "$(uname -m)" && "$(uname -s)" == "Darwin" ]]; then
        arch "-${ARCH}" "${work}/probe" "$@"
    else
        "${work}/probe" "$@"
    fi
}

dirs="$(run_probe dirs)" || fail "the probe does not run for ${ARCH}"
while IFS= read -r line; do
    case "${line}" in
        *"\"${BAKED}/"*) ;;
        *) fail "a compiled-in directory is not under ${BAKED}: ${line}" ;;
    esac
done <<< "${dirs}"

run_probe fetch >/dev/null || fail "the library cannot provide SHA-256/Ed25519 at all"

# A configuration that leaves only the null provider active. A library that
# loads it has no algorithms left; ours must not even open it.
cat > "${work}/hostile.cnf" <<'CNF'
openssl_conf = init
[init]
providers = prov
[prov]
null = nullsect
[nullsect]
activate = 1
CNF
out="$(OPENSSL_CONF="${work}/hostile.cnf" run_probe fetch)" \
    || fail "the library read a configuration file nobody asked for (${out:-no output})"

echo "check_vendored_openssl: ok (${ARCH}; dirs under ${BAKED}; no config autoload)"
