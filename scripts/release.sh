#!/usr/bin/env bash
#
# Cut a Liu release on the release Mac. Every signature a release carries is
# made here, and no signing key, certificate or password is ever stored on
# GitHub: the Developer ID identity and the notary profile live in this Mac's
# login keychain, the Ed25519 update key in a 0600 file off the tree on this
# Mac's encrypted disk, with a passphrase-encrypted backup elsewhere. GitHub
# builds the Linux packages (.github/workflows/release.yml: no secret, a
# read-only token) and hosts what this script publishes.
#
# Two runs, so the key is never needed while anything is sent out:
#
#   1. scripts/release.sh            builds this Mac's macOS architecture (DMG +
#      update zip, Developer ID signed, notarized, stapled), collects the other
#      macOS architecture and the Linux packages, Ed25519-signs the update
#      artifacts (the macOS zips and the Linux tarballs), merges ONE appcast.json
#      for both systems and signs it, writes and signs manifest.json, and checks
#      the whole set against the compiled-in public key. Nothing is published.
#   2. GH_PUBLISH=1 scripts/release.sh   checks that set again and publishes it:
#      the tag (by name), the GitHub Release — the canonical update channel; the
#      in-app updater reads releases/latest/download/appcast.json — and the
#      site's version. The key is not read.
#
# Before run 1:
#   a. VERSION bumped and committed, `git tag v<VERSION>` on that commit, and a
#      clean worktree of it (`git worktree add --detach <dir> v<VERSION>`): the
#      packager names files from `git describe --exact-match`, and a working
#      tree can hold uncommitted changes.
#   b. The other macOS architecture, built and packaged in a second worktree;
#      for x86_64 on Apple silicon:
#        cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUSE_VENDORED_SSH_DEPS=ON \
#          -DAUTO_DOWNLOAD_SSH_DEPS=ON -DCMAKE_APPLE_SILICON_PROCESSOR=x86_64 \
#          -DCMAKE_OSX_ARCHITECTURES=x86_64 && cmake --build build -j4
#        NOTARY_PROFILE=liu-notary arch -x86_64 /bin/bash scripts/package_macos_dmg.sh
#      Its build/package folder is LIU_MACOS_OTHER_DIR.
#   c. The Linux packages (per arch: tarball, .deb, .rpm, AppImage), built in
#      ubuntu:22.04 by release.yml once the tag is pushed (`git push origin
#      refs/tags/v<VERSION>`; never --tags) and fetched with
#        gh run download <run-id> -R calculusteam/liu -p 'pkg-linux-*' -D <dir>
#      That <dir> is LIU_LINUX_DIR.
#
#   NOTARY_PROFILE=liu-notary LIU_ED25519_KEY=/path/to/liu_update_ed25519.key \
#   LIU_MACOS_OTHER_DIR=<dir> LIU_LINUX_DIR=<dir> scripts/release.sh
#
# Env:
#   LIU_ED25519_KEY      path to the Ed25519 private key — REQUIRED for run 1,
#                        there is no default location. It is the sole trust
#                        anchor of every installed copy's updater, and
#                        is refused inside the tree, readable by anyone else, or
#                        not matching src/core/update_pubkey.h.
#   NOTARY_PROFILE       `notarytool store-credentials` profile (or NOTARY_KEY +
#                        NOTARY_KEY_ID [+ NOTARY_ISSUER]): a release is notarized
#                        or it is not made.
#   CODESIGN_IDENTITY    "Developer ID Application: …" (default: the keychain's
#                        first); an ad-hoc signature is refused.
#   LIU_MACOS_OTHER_DIR  the other macOS architecture's DMG + zip (b)
#   LIU_LINUX_DIR        the Linux packages, flat or one folder per artifact (c)
#   LIU_SKIP_SECURITY_GATE=1  skip the pre-release fuzz/leak gate (say why)
#   GH_PUBLISH=1         run 2: publish what run 1 made and verified
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
die() { echo "error: $*" >&2; exit 1; }

VERSION="$(tr -d '[:space:]' < "${ROOT}/VERSION")"
# Validate VERSION before using it in tags and filenames. Accept only a
# semver-like token so unexpected characters do not enter artifact names.
if [[ ! "${VERSION}" =~ ^[0-9]+(\.[0-9]+){0,2}(-[A-Za-z0-9.]+)?$ ]]; then
    echo "error: VERSION '${VERSION}' is not a valid semver token" >&2; exit 1
fi
TAG="v${VERSION}"
SYS="${VERSION//-/\~}"   # dpkg and rpm spell a prerelease's "-" as "~" (package_linux.sh)
OUT="${ROOT}/build/package"
REPO="calculusteam/liu"
# Canonical host is GitHub Releases — both the in-app updater's feed URL and
# the download base. Keep in lockstep with CMake's LIU_UPDATE_FEED_URL.
DL_BASE="https://github.com/${REPO}/releases/download/${TAG}"
FEED_URL="https://github.com/${REPO}/releases/latest/download/appcast.json"
FEED="${OUT}/appcast.json"
MANIFEST="${OUT}/manifest.json"

[[ "$(uname -s)" == "Darwin" ]] || die "a release is cut on the release Mac"
case "$(uname -m)" in
    arm64)  ARCH=arm64;  OTHER=x86_64 ;;
    x86_64) ARCH=x86_64; OTHER=arm64 ;;
    *)      die "unsupported architecture $(uname -m)" ;;
esac

TARBALLS=("liu-${VERSION}-linux-arm64.tar.gz" "liu-${VERSION}-linux-x86_64.tar.gz")
LINUX_FILES=("${TARBALLS[@]}"
             "liu_${SYS}_arm64.deb" "liu_${SYS}_amd64.deb"
             "liu-${SYS}-1.aarch64.rpm" "liu-${SYS}-1.x86_64.rpm"
             "Liu-${VERSION}-aarch64.AppImage" "Liu-${VERSION}-x86_64.AppImage")

# Both runs: the release commit, as committed.
git -C "${ROOT}" tag --points-at HEAD | grep -qx "${TAG}" \
    || die "HEAD is not tagged ${TAG}: tag the release commit (an older local ${TAG} that points elsewhere must go first)"
[[ -z "$(git -C "${ROOT}" status --porcelain --untracked-files=no)" ]] \
    || die "the worktree has uncommitted changes: cut the release from a clean worktree of ${TAG}"

if [[ "${GH_PUBLISH:-0}" != "1" ]]; then
    # --- run 1: everything is checked before anything is built ----------------
    KEY="${LIU_ED25519_KEY:-}"
    [[ -n "${KEY}" ]] || die "set LIU_ED25519_KEY to the update key (it has no default location)"
    [[ -f "${KEY}" ]] || die "Ed25519 key not found: ${KEY}"
    KEY="$(cd "$(dirname "${KEY}")" && pwd)/$(basename "${KEY}")"
    case "${KEY}" in "${ROOT}"/*) die "the update key sits inside the tree (${KEY}); keep it off it" ;; esac
    [[ ! -f "${ROOT}/liu_update_ed25519.key" ]] \
        || die "a private key sits in the working tree (${ROOT}/liu_update_ed25519.key); move it off the tree"
    [[ "$(stat -f %Lp "${KEY}")" =~ ^[0-7]00$ ]] || die "${KEY} is readable by others: chmod 600 it"

    if [[ -z "${CODESIGN_IDENTITY:-}" ]]; then
        CODESIGN_IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null \
            | sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -n 1)"
    fi
    [[ -n "${CODESIGN_IDENTITY}" && "${CODESIGN_IDENTITY}" != "-" ]] \
        || die "no Developer ID Application identity in the keychain: a release is Developer ID signed or it is not made"
    export CODESIGN_IDENTITY
    [[ "${NOTARIZE:-1}" != "0" ]] || die "NOTARIZE=0: a release is notarized or it is not made"
    [[ -n "${NOTARY_PROFILE:-}" || ( -n "${NOTARY_KEY:-}" && -n "${NOTARY_KEY_ID:-}" ) ]] \
        || die "set NOTARY_PROFILE (or NOTARY_KEY + NOTARY_KEY_ID): a release is notarized or it is not made"

    OTHER_DIR="${LIU_MACOS_OTHER_DIR:-}"
    LINUX_DIR="${LIU_LINUX_DIR:-}"
    [[ -d "${OTHER_DIR}" ]] || die "set LIU_MACOS_OTHER_DIR to the ${OTHER} build's package folder (see the header)"
    [[ -d "${LINUX_DIR}" ]] || die "set LIU_LINUX_DIR to the Linux packages (see the header)"

    echo "==> Verifying committed public key matches the signing key"
    python3 "${ROOT}/scripts/check_pubkey.py" "${KEY}" "${ROOT}/src/core/update_pubkey.h"

    # Run the security gate before publishing release artifacts.
    # LIU_SKIP_SECURITY_GATE=1 skips the gate.
    if [[ "${LIU_SKIP_SECURITY_GATE:-0}" != "1" ]]; then
        echo "==> Security gate (fuzz replay + fuzz + leak pass)"
        "${ROOT}/scripts/security_gate.sh" --quick || {
            echo "error: security gate failed — fix the crash/leak, or set LIU_SKIP_SECURITY_GATE=1 and say why" >&2
            exit 1
        }
    else
        echo "warning: LIU_SKIP_SECURITY_GATE=1 — the fuzz/leak gate was skipped" >&2
    fi

    echo "==> Building + packaging ${ARCH} (DMG + update zip, signed + notarized)"
    "${ROOT}/scripts/package_macos_dmg.sh" >/dev/null

    echo "==> Collecting the ${OTHER} build and the Linux packages"
    mkdir -p "${OUT}"
    # take <dir> <name>: the one file called <name> under <dir> — flat, or one
    # folder per artifact as `gh run download` lays them out — copied into OUT.
    take() {
        local found
        found="$(find "$1" -maxdepth 2 -type f -name "$2")"
        [[ -n "${found}" ]] || die "$2 is not in $1"
        [[ "${found}" != *$'\n'* ]] || die "more than one $2 in $1"
        [[ "${found}" -ef "${OUT}/$2" ]] || cp -p "${found}" "${OUT}/$2"
    }
    take "${OTHER_DIR}" "Liu-${VERSION}-${OTHER}.dmg"
    take "${OTHER_DIR}" "Liu-${VERSION}-${OTHER}.zip"
    for f in "${LINUX_FILES[@]}"; do take "${LINUX_DIR}" "${f}"; done

    # Both halves, not only the one built here: an ad-hoc or un-notarized DMG
    # from the other worktree would otherwise ride along into the feed.
    echo "==> Checking both macOS halves are signed by ${CODESIGN_IDENTITY} and notarized"
    for a in arm64 x86_64; do
        dmg="${OUT}/Liu-${VERSION}-${a}.dmg"
        [[ -f "${dmg}" && -f "${OUT}/Liu-${VERSION}-${a}.zip" ]] \
            || die "Liu-${VERSION}-${a}.dmg/.zip is missing from ${OUT} (built from the exact tag?)"
        codesign -dvv "${dmg}" 2>&1 | grep -qxF "Authority=${CODESIGN_IDENTITY}" \
            || die "${dmg##*/} is not signed by ${CODESIGN_IDENTITY}"
        xcrun stapler validate "${dmg}" >/dev/null || die "${dmg##*/} carries no notarization ticket"
    done

    echo "==> Ed25519-signing the update artifacts (macOS zips, Linux tarballs)"
    for f in "Liu-${VERSION}-arm64.zip" "Liu-${VERSION}-x86_64.zip" "${TARBALLS[@]}"; do
        "${ROOT}/scripts/sign_ed25519.sh" "${OUT}/${f}" "${KEY}" >/dev/null
    done

    echo "==> Updating the feed (preserving history from the live feed at ${FEED_URL})"
    # 404 = no prior feed yet (start fresh), 200 must be an authenticated feed,
    # anything else aborts rather than silently dropping the version history
    # into a one-entry feed.
    code="$(curl -sSL --retry 3 --retry-delay 2 -o "${FEED}" -w '%{http_code}' "${FEED_URL}" || echo 000)"
    code="${code: -3}"   # --retry can emit one code per attempt; keep the final one
    if [[ "${code}" == "200" ]]; then
        python3 "${ROOT}/scripts/verify_release.py" --feed "${FEED}" \
            --pubkey-header "${ROOT}/src/core/update_pubkey.h" \
            || { echo "error: ${FEED_URL} returned an unauthenticated or invalid feed — refusing to alter history" >&2; exit 1; }
    elif [[ "${code}" == "404" ]]; then
        echo "    no prior feed (HTTP 404) — starting fresh"; rm -f "${FEED}"
    else
        echo "error: could not fetch prior appcast.json (HTTP ${code}) — refusing to drop version history" >&2; exit 1
    fi
    # os:arch, spelled out. Linux first, macOS last: the clients match on
    # (os, arch) and do not care about order, but the site's edge resolver
    # (calculusteam/liu-website, functions/_lib/release.js) files each entry's
    # sha256 and signature under its arch alone, the last one winning.
    python3 "${ROOT}/scripts/gen_feed.py" --feed "${FEED}" --version "${VERSION}" \
        --build "$(git -C "${ROOT}" rev-parse --short HEAD)" \
        --notes-url "https://github.com/${REPO}/releases/tag/${TAG}" \
        --min-os 11.0 --base-url "${DL_BASE}" \
        --artifact "linux:arm64=${OUT}/${TARBALLS[0]}" \
        --artifact "linux:x86_64=${OUT}/${TARBALLS[1]}" \
        --artifact "macos:arm64=${OUT}/Liu-${VERSION}-arm64.zip" \
        --artifact "macos:x86_64=${OUT}/Liu-${VERSION}-x86_64.zip"

    echo "==> Ed25519-signing the feed (the client refuses an unsigned one)"
    python3 "${ROOT}/scripts/sign_feed.py" --feed "${FEED}" --key "${KEY}"

    echo "==> Writing release manifest (sha256 of every artifact)"
    python3 "${ROOT}/scripts/gen_manifest.py" --out "${MANIFEST}" \
        --version "${VERSION}" --base-url "${DL_BASE}" \
        --artifact "dmg:arm64=${OUT}/Liu-${VERSION}-arm64.dmg" \
        --artifact "dmg:x86_64=${OUT}/Liu-${VERSION}-x86_64.dmg" \
        --artifact "zip:arm64=${OUT}/Liu-${VERSION}-arm64.zip" \
        --artifact "zip:x86_64=${OUT}/Liu-${VERSION}-x86_64.zip" \
        --artifact "tarball:linux-arm64=${OUT}/${TARBALLS[0]}" \
        --artifact "tarball:linux-x86_64=${OUT}/${TARBALLS[1]}" \
        --artifact "deb:linux-arm64=${OUT}/liu_${SYS}_arm64.deb" \
        --artifact "deb:linux-x86_64=${OUT}/liu_${SYS}_amd64.deb" \
        --artifact "rpm:linux-arm64=${OUT}/liu-${SYS}-1.aarch64.rpm" \
        --artifact "rpm:linux-x86_64=${OUT}/liu-${SYS}-1.x86_64.rpm" \
        --artifact "appimage:linux-arm64=${OUT}/Liu-${VERSION}-aarch64.AppImage" \
        --artifact "appimage:linux-x86_64=${OUT}/Liu-${VERSION}-x86_64.AppImage" >/dev/null

    echo "==> Ed25519-signing the manifest (manifest trust root)"
    # manifest.json lists a sha256 for every published artifact, including the
    # DMGs and Linux packages the in-app updater never touches. THIS signature is
    # what makes those checksums verifiable by hand, under the same Ed25519 root
    # as the updater feed.
    "${ROOT}/scripts/sign_ed25519.sh" "${MANIFEST}" "${KEY}" >/dev/null
fi

# --- both runs: the exact bytes that will be published --------------------
# Feed and manifest hashes/sizes/URLs, every detached artifact signature and
# the manifest signature must agree with the application's compiled-in key.
echo "==> Verifying the release bundle"
python3 "${ROOT}/scripts/verify_release.py" --package-dir "${OUT}" --version "${VERSION}" \
    --base-url "${DL_BASE}" --pubkey-header "${ROOT}/src/core/update_pubkey.h"

# The release page carries only what users and the updater download: a DMG and
# an update zip per macOS arch, per Linux arch the tarball (updater) and the
# .deb, .rpm and AppImage, and appcast.json — it MUST share the artifacts'
# github.com host, because the updater pins artifact URLs to the feed host.
# No .sig files (embedded in the feed), no manifest pair (lives in the repo).
FILES=()
for a in arm64 x86_64; do FILES+=("${OUT}/Liu-${VERSION}-${a}.dmg" "${OUT}/Liu-${VERSION}-${a}.zip"); done
for f in "${LINUX_FILES[@]}"; do FILES+=("${OUT}/${f}"); done
FILES+=("${FEED}")

if [[ "${GH_PUBLISH:-0}" != "1" ]]; then
    echo
    echo "Verified; nothing has left this Mac. To publish ${TAG}:"
    echo "  GH_PUBLISH=1 scripts/release.sh"
    exit 0
fi

# --- run 2: publish ----------------------------------------------------------
case "$(git -C "${ROOT}" remote get-url origin)" in
    *github.com[:/]"${REPO}" | *github.com[:/]"${REPO}".git) ;;
    *) die "origin is not ${REPO}" ;;
esac

echo "==> Pushing ${TAG} (by name; never --tags)"
local_tag="$(git -C "${ROOT}" rev-parse "refs/tags/${TAG}")"
remote_tag="$(git -C "${ROOT}" ls-remote origin "refs/tags/${TAG}" | awk '{print $1}')"
if [[ -z "${remote_tag}" ]]; then
    git -C "${ROOT}" push origin "refs/tags/${TAG}"
elif [[ "${remote_tag}" != "${local_tag}" ]]; then
    die "origin's ${TAG} is another tag (${remote_tag}); it is not replaced from here"
fi

echo "==> Publishing the GitHub Release ${TAG}"
NAMES=()
for f in "${FILES[@]}"; do NAMES+=("${f##*/}"); done
if gh release view "${TAG}" -R "${REPO}" >/dev/null 2>&1; then
    gh release upload "${TAG}" -R "${REPO}" "${FILES[@]}" --clobber
    # --draft=false matters on re-cuts: deleting a tag demotes its release to a
    # draft, and draft assets 404 on public URLs.
    gh release edit "${TAG}" -R "${REPO}" --title "Liu ${VERSION}" --draft=false
    # --clobber replaces same-name assets but never deletes retired ones — on a
    # re-publish, prune anything not in this set.
    gh release view "${TAG}" -R "${REPO}" --json assets --jq '.assets[].name' | while read -r a; do
        keep=false
        for n in "${NAMES[@]}"; do [[ "${a}" == "${n}" ]] && keep=true && break; done
        [[ "${keep}" == true ]] || gh release delete-asset "${TAG}" "${a}" -R "${REPO}" --yes
    done
else
    gh release create "${TAG}" -R "${REPO}" "${FILES[@]}" --verify-tag --title "Liu ${VERSION}" --generate-notes
fi

# The manifest lives IN the repo rather than on the release page, which keeps
# the published assets down to what users actually download.
mkdir -p "${ROOT}/packaging/manifest"
cp "${MANIFEST}"     "${ROOT}/packaging/manifest/manifest.json"
cp "${MANIFEST}.sig" "${ROOT}/packaging/manifest/manifest.json.sig"

# liu.software stamps the release version into its static HTML from the
# one-line RELEASE_VERSION file of calculusteam/liu-website; committing it there
# runs the site's deploy. After the release, never before it. A stale label is
# cosmetic, so a failure here only warns: the release is already out.
SITE_API="repos/calculusteam/liu-website/contents/RELEASE_VERSION"
if [[ ! "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "warning: '${VERSION}' is not a plain x.y.z version — the site's version is left alone" >&2
else
    have="$(gh api "${SITE_API}?ref=main" --jq '.content' 2>/dev/null | base64 -d 2>/dev/null | tr -d '[:space:]' || true)"
    if [[ "${have}" == "${VERSION}" ]]; then
        echo "liu.software already shows ${VERSION}"
    else
        sha="$(gh api "${SITE_API}?ref=main" --jq '.sha' 2>/dev/null || true)"
        put=(-X PUT "${SITE_API}" -f message="Liu ${VERSION} released" -f branch=main
             -f content="$(printf '%s\n' "${VERSION}" | base64)")
        [[ -n "${sha}" ]] && put+=(-f sha="${sha}")
        if gh api "${put[@]}" >/dev/null; then
            echo "committed RELEASE_VERSION=${VERSION} to calculusteam/liu-website — its deploy is running"
        else
            echo "warning: could not update RELEASE_VERSION in calculusteam/liu-website — liu.software keeps showing the previous version" >&2
        fi
    fi
fi

echo
echo "${TAG} is published. Commit packaging/manifest/manifest.json(.sig) on main."
