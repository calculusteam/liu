#!/usr/bin/env bash
#
# Ed25519-sign an update artifact and report its integrity metadata.
#
# Usage: scripts/sign_ed25519.sh <artifact> <ed25519-private-key.pem>
#
# Produces <artifact>.sig (base64 of the raw 64-byte detached Ed25519 signature
# over the artifact bytes) and prints three lines the feed generator consumes:
#   sha256 <hex>
#   size   <bytes>
#   sig    <base64>
#
# The private key must NEVER be committed; pass a path to an offline key or the
# CI-materialized ED25519_PRIVATE_KEY secret. The matching public key is in
# src/core/update_pubkey.h and is what the client verifies against.
set -euo pipefail

artifact="${1:?usage: sign_ed25519.sh <artifact> <key.pem>}"
key="${2:?usage: sign_ed25519.sh <artifact> <key.pem>}"

[[ -f "${artifact}" ]] || { echo "error: artifact not found: ${artifact}" >&2; exit 1; }
[[ -f "${key}" ]]      || { echo "error: key not found: ${key}" >&2; exit 1; }

sig_b64="$(openssl pkeyutl -sign -inkey "${key}" -rawin -in "${artifact}" | openssl base64 -A)"
printf '%s' "${sig_b64}" > "${artifact}.sig"

sha256="$(shasum -a 256 "${artifact}" | awk '{print $1}')"
size="$(stat -f%z "${artifact}" 2>/dev/null || stat -c%s "${artifact}")"

echo "sha256 ${sha256}"
echo "size   ${size}"
echo "sig    ${sig_b64}"
