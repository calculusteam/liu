#!/usr/bin/env bash
#
# Cut a Liu release locally (mirrors .github/workflows/release.yml for a no-CI
# release). Builds, packages the DMG + update zip, Ed25519-signs the zip, merges
# the update feed, writes the store manifest, and publishes to liu.software
# (S3/R2) when creds are present + mirrors to a GitHub Release.
#
# The CANONICAL release path is CI: `git tag v$(cat VERSION) && git push --tags`.
# This script is a local convenience and seeds history from the SAME canonical
# feed (liu.software) as CI, so the two never diverge.
#
#   LIU_ED25519_KEY=/path/to/liu_update_ed25519.key scripts/release.sh
#
# Env:
#   LIU_ED25519_KEY    path to the Ed25519 private key (default: ./liu_update_ed25519.key)
#   LIU_CDN_BASE       canonical host for feed + downloads (default: https://liu.software)
#   CODESIGN_IDENTITY  "Developer ID Application: …" for a notarizable build (default: ad-hoc "-")
#   AWS_S3_BUCKET +    if both set, publish downloads/feed/manifest/install.sh to
#   AWS_ENDPOINT_URL   the R2/S3 bucket (with AWS_ACCESS_KEY_ID/SECRET in env)
#   GH_PUBLISH=1       also run `gh release create` (otherwise just prints it)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(tr -d '[:space:]' < "${ROOT}/VERSION")"
# Validate strictly — VERSION flows into tags, filenames and (historically) a
# shell command; reject anything but a semver-ish token so a poisoned VERSION
# file can't inject shell metacharacters.
if [[ ! "${VERSION}" =~ ^[0-9]+(\.[0-9]+){0,2}(-[A-Za-z0-9.]+)?$ ]]; then
    echo "error: VERSION '${VERSION}' is not a valid semver token" >&2; exit 1
fi
ARCH="$(uname -m)"
TAG="v${VERSION}"
KEY="${LIU_ED25519_KEY:-${ROOT}/liu_update_ed25519.key}"
OUT="${ROOT}/build/package"
REPO="calculusteam/liu"
# Canonical host is liu.software (R2) — both the in-app updater's feed URL and
# the download base. Keep in lockstep with release.yml / CMake so the local and
# CI release paths never seed history from different feeds.
CDN_BASE="${LIU_CDN_BASE:-https://liu.software}"
DL_BASE="${CDN_BASE}/downloads/${TAG}"
FEED_URL="${CDN_BASE}/appcast.json"

[[ -f "${KEY}" ]] || { echo "error: Ed25519 key not found: ${KEY} (set LIU_ED25519_KEY)" >&2; exit 1; }

echo "==> Verifying committed public key matches the signing key"
python3 "${ROOT}/scripts/check_pubkey.py" "${KEY}" "${ROOT}/src/core/update_pubkey.h"

echo "==> Building + packaging (DMG + update zip)"
"${ROOT}/scripts/package_macos_dmg.sh" >/dev/null
ZIP="${OUT}/Liu-${VERSION}-${ARCH}.zip"
DMG="${OUT}/Liu-${VERSION}-${ARCH}.dmg"
[[ -f "${ZIP}" ]] || { echo "error: update zip not produced: ${ZIP}" >&2; exit 1; }

echo "==> Ed25519-signing the update zip"
"${ROOT}/scripts/sign_ed25519.sh" "${ZIP}" "${KEY}"

echo "==> Updating the feed (preserving history from the live feed at ${FEED_URL})"
FEED="${OUT}/appcast.json"
# Same fetch discipline as release.yml: 404 = no prior feed yet (start fresh),
# 200 must parse as JSON, anything else aborts rather than silently dropping the
# version history into a one-entry feed.
code="$(curl -sSL --retry 3 --retry-delay 2 -o "${FEED}" -w '%{http_code}' "${FEED_URL}" || echo 000)"
code="${code: -3}"   # --retry can emit one code per attempt; keep the final one
if [[ "${code}" == "200" ]]; then
    python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "${FEED}" \
        || { echo "error: ${FEED_URL} returned 200 but is not valid JSON — refusing to clobber feed history" >&2; exit 1; }
elif [[ "${code}" == "404" ]]; then
    echo "    no prior feed (HTTP 404) — starting fresh"; rm -f "${FEED}"
else
    echo "error: could not fetch prior appcast.json (HTTP ${code}) — refusing to drop version history" >&2; exit 1
fi
python3 "${ROOT}/scripts/gen_feed.py" --feed "${FEED}" --version "${VERSION}" \
    --build "$(git -C "${ROOT}" rev-parse --short HEAD 2>/dev/null || echo "")" \
    --notes-url "https://github.com/${REPO}/releases/tag/${TAG}" \
    --min-os 11.0 --base-url "${DL_BASE}" \
    --artifact "${ARCH}=${ZIP}"

echo "==> Writing release manifest (sha256 of every artifact, for the stores)"
MANIFEST="${OUT}/manifest.json"
M=(--artifact "zip:${ARCH}=${ZIP}")
[[ -f "${DMG}" ]] && M+=(--artifact "dmg:${ARCH}=${DMG}")
python3 "${ROOT}/scripts/gen_manifest.py" --out "${MANIFEST}" \
    --version "${VERSION}" --base-url "${DL_BASE}" "${M[@]}"

echo
echo "Artifacts in ${OUT}:"
for f in "${DMG}" "${ZIP}" "${ZIP}.sig" "${FEED}" "${MANIFEST}"; do [[ -f "${f}" ]] && echo "  ${f}"; done
echo

# The feed/manifest URLs point at ${CDN_BASE} — the artifacts must live there for
# the in-app updater to resolve them. Publish to R2/S3 when creds are present
# (mirrors release.yml); otherwise this is a build-only run and you must upload
# build/package/* to the bucket yourself (or just use CI).
if [[ -n "${AWS_S3_BUCKET:-}" && -n "${AWS_ENDPOINT_URL:-}" ]]; then
    echo "==> Publishing downloads + feed to ${CDN_BASE} (S3/R2 bucket ${AWS_S3_BUCKET})"
    ep=(--endpoint-url "${AWS_ENDPOINT_URL}")
    aws s3 cp "${OUT}/" "s3://${AWS_S3_BUCKET}/downloads/${TAG}/" --recursive \
        --exclude '*' --include 'Liu-*' --include 'liu-*' \
        --cache-control 'public, max-age=31536000, immutable' "${ep[@]}"
    aws s3 cp "${FEED}" "s3://${AWS_S3_BUCKET}/appcast.json" \
        --content-type application/json --cache-control 'public, max-age=300' "${ep[@]}"
    aws s3 cp "${MANIFEST}" "s3://${AWS_S3_BUCKET}/manifest.json" \
        --content-type application/json --cache-control 'public, max-age=300' "${ep[@]}"
    aws s3 cp "${ROOT}/scripts/install.sh" "s3://${AWS_S3_BUCKET}/install.sh" \
        --content-type 'text/x-shellscript' --cache-control 'public, max-age=300' "${ep[@]}"
else
    echo "note: AWS_S3_BUCKET/AWS_ENDPOINT_URL unset — skipping ${CDN_BASE} publish."
    echo "      The feed points at ${CDN_BASE}; upload build/package/* to the bucket"
    echo "      (or use CI: push tag ${TAG} to run .github/workflows/release.yml)."
fi
echo

if [[ "${GH_PUBLISH:-0}" == "1" ]]; then
    echo "==> Mirroring to GitHub release ${TAG} (notes + source)"
    gh release create "${TAG}" "${DMG}" "${ZIP}" "${ZIP}.sig" "${FEED}" "${MANIFEST}" \
        --title "Liu ${VERSION}" --generate-notes
else
    echo "To mirror to GitHub (creates tag ${TAG} if missing), run:"
    echo "  gh release create ${TAG} ${DMG} ${ZIP} ${ZIP}.sig ${FEED} ${MANIFEST} --title 'Liu ${VERSION}' --generate-notes"
fi
