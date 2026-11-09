#!/usr/bin/env bash
#
# Cut a Liu release on the release Mac. Every signature a release carries is
# made here, and no signing key, certificate or password is ever stored on
# GitHub: the Developer ID identity and the notary profile live in this Mac's
# keychains, the Ed25519 update key in a 0600 file off the tree on this Mac's
# encrypted disk, with a passphrase-encrypted backup elsewhere. GitHub builds
# the Linux packages (.github/workflows/release.yml: no secret, a read-only
# token) and hosts what this script publishes.
#
# Two runs, so the key is never needed while anything is sent out:
#
#   1. scripts/release.sh            builds and packages BOTH macOS
#      architectures (DMG + update zip, Developer ID signed, notarized,
#      stapled; the other one in a worktree of its own), collects the Linux
#      packages when the release ships Linux, Ed25519-signs the update
#      artifacts, merges ONE appcast.json over the live feed and signs it,
#      writes and signs manifest.json, and checks the whole set against the
#      compiled-in public key. Nothing is published.
#   2. GH_PUBLISH=1 scripts/release.sh   checks that set again and publishes it:
#      the tag (by name), a DRAFT GitHub Release with every asset, the assets'
#      digests compared with the files here, then the release made public and
#      latest, the live feed URL read back, and the site's version. The key is
#      not read.
#
#   LIU_RELEASE_REHEARSAL=1 scripts/release.sh   the whole of run 1 without
#      notarization and with a throwaway update key, into build/release-rehearsal:
#      what a release would build, sign and verify, on any commit with a local
#      tag. It can never be published (its signatures are not the release key's).
#
# Before run 1:
#   - VERSION bumped and committed, `git tag v<VERSION>` on that commit, and a
#     clean worktree of it (`git worktree add --detach <dir> v<VERSION>`): the
#     packager names files from `git describe --exact-match`, and a working
#     tree can hold uncommitted changes.
#   - When the release ships Linux (LIU_RELEASE_PLATFORMS=macos,linux): the tag
#     pushed by name (`git push origin refs/tags/v<VERSION>`; never --tags) and
#     release.yml's run for it finished; run 1 downloads its packages itself.
#
# Env (a file of plain assignments, $HOME/.liu-release/release.env or
# LIU_RELEASE_ENV, is read first when it exists: identities, not secrets):
#   LIU_ED25519_KEY      path to the Ed25519 private key — REQUIRED for run 1,
#                        there is no default location. It is the sole trust
#                        anchor of every installed copy's updater, and is
#                        refused inside the tree, readable by anyone else, or
#                        not matching src/core/update_pubkey.h.
#   CODESIGN_IDENTITY    the Developer ID Application identity, a name or its
#                        SHA-1 — REQUIRED (the login keychain can hold more
#                        than one; an ad-hoc signature is refused)
#   CODESIGN_KEYCHAIN    the keychain that holds it, when not the login one
#   NOTARY_PROFILE       `notarytool store-credentials` profile (or NOTARY_KEY +
#                        NOTARY_KEY_ID [+ NOTARY_ISSUER]): a release is notarized
#                        or it is not made
#   LIU_RELEASE_PLATFORMS  macos (default) or macos,linux
#   LIU_RELEASE_WORK     where the other architecture's worktree, the Linux
#                        download and the logs go (default build/release)
#   LIU_MACOS_OTHER_DIR  the other macOS architecture's DMG + zip, already
#                        built (an Intel Mac cannot build arm64)
#   LIU_LINUX_DIR        the Linux packages, already downloaded
#   LIU_RELEASE_NOTES    run 2: a Markdown file for the release page (default:
#                        GitHub's generated notes)
#   LIU_SKIP_SECURITY_GATE=1  skip the pre-release fuzz/leak gate (say why)
#   GH_PUBLISH=1         run 2: publish what run 1 made and verified
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
die() { echo "error: $*" >&2; exit 1; }
step() { echo "==> $*"; }

ENV_FILE="${LIU_RELEASE_ENV:-${HOME}/.liu-release/release.env}"
if [[ -f "${ENV_FILE}" ]]; then
    [[ "$(stat -f %Lp "${ENV_FILE}")" =~ ^[0-7]00$ ]] || die "${ENV_FILE} is readable by others: chmod 600 it"
    set -a
    # shellcheck disable=SC1090
    . "${ENV_FILE}"
    set +a
fi

VERSION="$(tr -d '[:space:]' < "${ROOT}/VERSION")"
# Validate VERSION before using it in tags and filenames. Accept only a
# semver-like token so unexpected characters do not enter artifact names.
if [[ ! "${VERSION}" =~ ^[0-9]+(\.[0-9]+){0,2}(-[A-Za-z0-9.]+)?$ ]]; then
    die "VERSION '${VERSION}' is not a valid semver token"
fi
TAG="v${VERSION}"
SYS="${VERSION//-/\~}"   # dpkg and rpm spell a prerelease's "-" as "~" (package_linux.sh)
REPO="calculusteam/liu"
# Canonical host is GitHub Releases — both the in-app updater's feed URL and
# the download base. Keep in lockstep with CMake's LIU_UPDATE_FEED_URL.
DL_BASE="https://github.com/${REPO}/releases/download/${TAG}"
FEED_URL="https://github.com/${REPO}/releases/latest/download/appcast.json"
REHEARSAL="${LIU_RELEASE_REHEARSAL:-0}"
PUBLISH="${GH_PUBLISH:-0}"
[[ "${REHEARSAL}" == "1" && "${PUBLISH}" == "1" ]] && die "a rehearsal is never published"
# The release set, apart from the packager's own build/package.
if [[ "${REHEARSAL}" == "1" ]]; then OUT="${ROOT}/build/release-rehearsal"; else OUT="${ROOT}/build/release-set"; fi
WORK="${LIU_RELEASE_WORK:-${ROOT}/build/release}"
FEED="${OUT}/appcast.json"
MANIFEST="${OUT}/manifest.json"
PUBKEY_H="${ROOT}/src/core/update_pubkey.h"

PLATFORMS="${LIU_RELEASE_PLATFORMS:-macos}"
case "${PLATFORMS}" in
    macos) SHIP_LINUX=0 ;;
    macos,linux | linux,macos) SHIP_LINUX=1; PLATFORMS="macos,linux" ;;
    *) die "LIU_RELEASE_PLATFORMS is macos or macos,linux, not '${PLATFORMS}'" ;;
esac

[[ "$(uname -s)" == "Darwin" ]] || die "a release is cut on the release Mac"
case "$(uname -m)" in
    arm64)  ARCH=arm64;  OTHER=x86_64 ;;
    x86_64) ARCH=x86_64; OTHER=arm64 ;;
    *)      die "unsupported architecture $(uname -m)" ;;
esac

# What the release page carries. The macOS files are named Liu-v<VERSION>-<arch>
# (the site's download links and the published releases say so); the packager
# writes Liu-<VERSION>-<arch>, and they are renamed as they are collected.
mac_dmg() { echo "Liu-v${VERSION}-$1.dmg"; }
mac_zip() { echo "Liu-v${VERSION}-$1.zip"; }
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
COMMIT="$(git -C "${ROOT}" rev-parse "${TAG}^{commit}")"

# The common name of the signing identity (a name stays a name; a SHA-1 is
# looked up in its keychain), and whether a signed file's leaf certificate is it.
identity_name() {
    if [[ "${CODESIGN_IDENTITY}" =~ ^[0-9A-Fa-f]{40}$ ]]; then
        security find-identity -v -p codesigning ${CODESIGN_KEYCHAIN:+"${CODESIGN_KEYCHAIN}"} 2>/dev/null \
            | awk -v h="${CODESIGN_IDENTITY}" 'toupper($2) == toupper(h) { sub(/^[^"]*"/, ""); sub(/"[^"]*$/, ""); print; exit }'
    else
        echo "${CODESIGN_IDENTITY}"
    fi
}
signed_by_identity() {
    local file="$1" scratch
    if [[ "${CODESIGN_IDENTITY}" =~ ^[0-9A-Fa-f]{40}$ ]]; then
        scratch="$(mktemp -d "${TMPDIR:-/tmp}/liu-cert.XXXXXX")"
        (cd "${scratch}" && codesign -d --extract-certificates "${file}" >/dev/null 2>&1) || { rm -rf "${scratch}"; return 1; }
        local leaf; leaf="$(shasum -a 1 "${scratch}/codesign0" 2>/dev/null | awk '{print toupper($1)}')"
        rm -rf "${scratch}"
        [[ "${leaf}" == "$(tr '[:lower:]' '[:upper:]' <<<"${CODESIGN_IDENTITY}")" ]]
    else
        codesign -dvv "${file}" 2>&1 | grep -qxF "Authority=${CODESIGN_IDENTITY}"
    fi
}

if [[ "${PUBLISH}" != "1" ]]; then
    # --- run 1: everything that can refuse is asked before anything is built --
    step "Checking what the release needs (${PLATFORMS}$([[ "${REHEARSAL}" == 1 ]] && echo ', rehearsal'))"
    mkdir -p "${WORK}"
    free_gb="$(df -g "${ROOT}" | awk 'NR==2 {print $4}')"
    [[ "${free_gb}" -ge 10 ]] || die "${free_gb} GB free next to the tree: two macOS builds and their packages need 10"

    if [[ "${REHEARSAL}" == "1" ]]; then
        # A throwaway update key and the header that names it: the rehearsal's
        # feed, manifest and artifact signatures verify against it, and against
        # nothing a client has.
        KEY="${WORK}/rehearsal_ed25519.key"
        rm -f "${KEY}"; (umask 077; openssl genpkey -algorithm ed25519 -out "${KEY}" 2>/dev/null)
        VERIFY_H="${WORK}/rehearsal_pubkey.h"
        { echo "/* rehearsal key: not the release's */"
          echo "static const unsigned char LIU_UPDATE_PUBKEY[32] = {"
          openssl pkey -in "${KEY}" -pubout -outform DER | tail -c 32 | xxd -i
          echo "};"; } > "${VERIFY_H}"
    else
        KEY="${LIU_ED25519_KEY:-}"
        [[ -n "${KEY}" ]] || die "set LIU_ED25519_KEY to the update key (it has no default location)"
        [[ -f "${KEY}" ]] || die "Ed25519 key not found: ${KEY}"
        KEY="$(cd "$(dirname "${KEY}")" && pwd)/$(basename "${KEY}")"
        case "${KEY}" in "${ROOT}"/*) die "the update key sits inside the tree (${KEY}); keep it off it" ;; esac
        [[ ! -f "${ROOT}/liu_update_ed25519.key" ]] \
            || die "a private key sits in the working tree (${ROOT}/liu_update_ed25519.key); move it off the tree"
        [[ "$(stat -f %Lp "${KEY}")" =~ ^[0-7]00$ ]] || die "${KEY} is readable by others: chmod 600 it"
        python3 "${ROOT}/scripts/check_pubkey.py" "${KEY}" "${PUBKEY_H}" >/dev/null \
            || die "the key does not match src/core/update_pubkey.h"
        VERIFY_H="${PUBKEY_H}"
    fi

    [[ -n "${CODESIGN_IDENTITY:-}" && "${CODESIGN_IDENTITY}" != "-" ]] \
        || die "set CODESIGN_IDENTITY to the Developer ID Application identity (a name or its SHA-1): a release is Developer ID signed or it is not made"
    [[ -z "${CODESIGN_KEYCHAIN:-}" || -f "${CODESIGN_KEYCHAIN}" ]] || die "CODESIGN_KEYCHAIN ${CODESIGN_KEYCHAIN} does not exist"
    IDENTITY_NAME="$(identity_name)"
    [[ "${IDENTITY_NAME}" == "Developer ID Application: "* ]] \
        || die "'${CODESIGN_IDENTITY}' is not a Developer ID Application identity in ${CODESIGN_KEYCHAIN:-the keychain search list}"
    echo "    signing as ${IDENTITY_NAME}"
    export CODESIGN_IDENTITY CODESIGN_KEYCHAIN

    if [[ "${REHEARSAL}" == "1" ]]; then
        export NOTARIZE=0
    else
        [[ "${NOTARIZE:-1}" != "0" ]] || die "NOTARIZE=0: a release is notarized or it is not made"
        if [[ -n "${NOTARY_PROFILE:-}" ]]; then
            xcrun notarytool history --keychain-profile "${NOTARY_PROFILE}" >/dev/null 2>&1 \
                || die "the notary profile ${NOTARY_PROFILE} does not answer (xcrun notarytool history): store it again"
        elif [[ -z "${NOTARY_KEY:-}" || -z "${NOTARY_KEY_ID:-}" ]]; then
            die "set NOTARY_PROFILE (or NOTARY_KEY + NOTARY_KEY_ID): a release is notarized or it is not made"
        fi
    fi

    OTHER_DIR="${LIU_MACOS_OTHER_DIR:-}"
    [[ -z "${OTHER_DIR}" || -d "${OTHER_DIR}" ]] || die "LIU_MACOS_OTHER_DIR ${OTHER_DIR} is not a folder"
    [[ -n "${OTHER_DIR}" || "${ARCH}" == "arm64" ]] \
        || die "an Intel Mac cannot build arm64: set LIU_MACOS_OTHER_DIR to the arm64 build's package folder"

    # The live feed: its history is carried into this release's. 404 = no feed
    # yet (start fresh), 200 must be an authenticated feed, anything else stops
    # here rather than silently dropping the version history.
    rm -rf "${OUT}"; mkdir -p "${OUT}"
    LIVE_FEED="${WORK}/live-appcast.json"
    code="$(curl -sSL --retry 3 --retry-delay 2 -o "${LIVE_FEED}" -w '%{http_code}' "${FEED_URL}" || echo 000)"
    code="${code: -3}"   # --retry can emit one code per attempt; keep the final one
    if [[ "${code}" == "200" ]]; then
        python3 "${ROOT}/scripts/verify_release.py" --feed "${LIVE_FEED}" --pubkey-header "${PUBKEY_H}" >/dev/null \
            || die "${FEED_URL} returned an unauthenticated or invalid feed — refusing to alter history"
        echo "    live feed verified"
    elif [[ "${code}" == "404" ]]; then
        echo "    no prior feed (HTTP 404) — starting fresh"; rm -f "${LIVE_FEED}"
    else
        die "could not fetch the live appcast.json (HTTP ${code}) — refusing to drop version history"
    fi

    LINUX_DIR="${LIU_LINUX_DIR:-}"
    if [[ "${SHIP_LINUX}" == "1" && -z "${LINUX_DIR}" ]]; then
        command -v gh >/dev/null || die "gh is needed to fetch the Linux packages (or set LIU_LINUX_DIR)"
        # The run release.yml made for this tag, at this commit, and green.
        run_id="$(gh run list -R "${REPO}" --workflow release.yml --branch "${TAG}" --limit 20 \
            --json databaseId,headSha,conclusion \
            --jq "[.[] | select(.headSha == \"${COMMIT}\" and .conclusion == \"success\")][0].databaseId // empty" 2>/dev/null || true)"
        [[ -n "${run_id}" ]] || die "no successful release.yml run for ${TAG} at ${COMMIT:0:12}: push the tag by name (Actions must be on) and wait for it, or set LIU_LINUX_DIR"
        LINUX_DIR="${WORK}/linux"
        rm -rf "${LINUX_DIR}"; mkdir -p "${LINUX_DIR}"
        step "Downloading the Linux packages of release.yml run ${run_id}"
        gh run download "${run_id}" -R "${REPO}" -p 'pkg-linux-*' -D "${LINUX_DIR}"
        for d in "${LINUX_DIR}"/pkg-linux-*; do
            [[ -f "${d}/SHA256SUMS" ]] || die "${d##*/} carries no SHA256SUMS"
            (cd "${d}" && shasum -a 256 -c --quiet SHA256SUMS) || die "${d##*/} does not match its SHA256SUMS"
        done
    fi
    [[ "${SHIP_LINUX}" == "0" || -d "${LINUX_DIR}" ]] || die "LIU_LINUX_DIR ${LINUX_DIR} is not a folder"

    # Run the security gate before building release artifacts. It lives with
    # the tests, which a clean worktree of the tag does not carry.
    if [[ "${LIU_SKIP_SECURITY_GATE:-0}" != "1" ]]; then
        [[ -x "${ROOT}/scripts/security_gate.sh" ]] \
            || die "scripts/security_gate.sh is not in this worktree (the tests stay local): run it in the development tree on this commit, then LIU_SKIP_SECURITY_GATE=1"
        step "Security gate (fuzz replay + fuzz + leak pass)"
        "${ROOT}/scripts/security_gate.sh" --quick \
            || die "security gate failed — fix the crash/leak, or set LIU_SKIP_SECURITY_GATE=1 and say why"
    else
        echo "warning: LIU_SKIP_SECURITY_GATE=1 — the fuzz/leak gate was skipped" >&2
    fi

    step "Building + packaging ${ARCH} (DMG + update zip, signed$([[ "${REHEARSAL}" == 1 ]] || echo ' + notarized'))"
    "${ROOT}/scripts/package_macos_dmg.sh" > "${WORK}/${ARCH}-package.log" 2>&1 \
        || { tail -25 "${WORK}/${ARCH}-package.log" >&2; die "packaging ${ARCH} failed (${WORK}/${ARCH}-package.log)"; }
    HERE_DIR="${ROOT}/build/package"

    if [[ -z "${OTHER_DIR}" ]]; then
        # The other architecture in a worktree of its own: the vendored
        # dependencies' install folder is one per tree, rebuilt per architecture.
        WT="${WORK}/worktree-${OTHER}"
        if [[ -d "${WT}" ]]; then
            [[ "$(git -C "${WT}" rev-parse HEAD)" == "${COMMIT}" ]] \
                || die "${WT} holds another commit: git worktree remove --force ${WT}"
            [[ -z "$(git -C "${WT}" status --porcelain --untracked-files=no)" ]] || die "${WT} has uncommitted changes"
        else
            git -C "${ROOT}" worktree add --detach "${WT}" "${TAG}" >/dev/null
        fi
        # the dependencies' sources: hash-checked by the deps script, so a copy saves only the download
        mkdir -p "${WT}/third_party/macos-deps/src"
        cp -n "${ROOT}"/third_party/macos-deps/src/*.tar.gz "${WT}/third_party/macos-deps/src/" 2>/dev/null || true
        step "Building + packaging ${OTHER} in ${WT}"
        cmake -S "${WT}" -B "${WT}/build" -DCMAKE_BUILD_TYPE=Release -DUSE_VENDORED_SSH_DEPS=ON \
            -DAUTO_DOWNLOAD_SSH_DEPS=ON -DLIU_DEV_HOOKS=OFF -DLIU_DEV_CODESIGN=OFF \
            -DCMAKE_APPLE_SILICON_PROCESSOR="${OTHER}" -DCMAKE_OSX_ARCHITECTURES="${OTHER}" \
            > "${WORK}/${OTHER}-configure.log" 2>&1 || die "configuring ${OTHER} failed (${WORK}/${OTHER}-configure.log)"
        # -DCMAKE_OSX_ARCHITECTURES alone keeps this Mac's processor, and with it
        # this architecture's assembly files.
        grep -hq "CMAKE_SYSTEM_PROCESSOR \"${OTHER}\"" "${WT}"/build/CMakeFiles/*/CMakeSystem.cmake \
            || die "CMake did not take ${OTHER} as the host (${WT}/build/CMakeFiles/*/CMakeSystem.cmake)"
        # The packager names its files from `uname -m`; the DMG tool this
        # architecture's run set up is used again.
        DMG_TOOL=()
        [[ -x "${ROOT}/build/.dmgvenv/bin/dmgbuild" ]] && DMG_TOOL=(DMGBUILD_BIN="${ROOT}/build/.dmgvenv/bin/dmgbuild")
        env BUILD_DIR="${WT}/build" ${DMG_TOOL[@]+"${DMG_TOOL[@]}"} \
            arch "-${OTHER}" /bin/bash "${WT}/scripts/package_macos_dmg.sh" > "${WORK}/${OTHER}-package.log" 2>&1 \
            || { tail -25 "${WORK}/${OTHER}-package.log" >&2; die "packaging ${OTHER} failed (${WORK}/${OTHER}-package.log)"; }
        OTHER_DIR="${WT}/build/package"
    fi

    step "Collecting the release set in ${OUT}"
    # take <dir> <name> <as>: the one file called <name> under <dir> — flat, or
    # one folder per artifact as `gh run download` lays them out — copied in as <as>.
    take() {
        local found
        found="$(find "$1" -maxdepth 2 -type f -name "$2")"
        [[ -n "${found}" ]] || die "$2 is not in $1 (built from the exact tag?)"
        [[ "${found}" != *$'\n'* ]] || die "more than one $2 in $1"
        cp -p "${found}" "${OUT}/$3"
    }
    for a in "${ARCH}" "${OTHER}"; do
        dir="${HERE_DIR}"; [[ "${a}" == "${OTHER}" ]] && dir="${OTHER_DIR}"
        take "${dir}" "Liu-${VERSION}-${a}.dmg" "$(mac_dmg "${a}")"
        take "${dir}" "Liu-${VERSION}-${a}.zip" "$(mac_zip "${a}")"
    done
    if [[ "${SHIP_LINUX}" == "1" ]]; then
        for f in "${LINUX_FILES[@]}"; do take "${LINUX_DIR}" "${f}" "${f}"; done
    fi

    # Both halves, not only the one built here: an ad-hoc or un-notarized DMG
    # from another worktree would otherwise ride along into the feed.
    step "Checking both macOS halves"
    for a in arm64 x86_64; do
        dmg="${OUT}/$(mac_dmg "${a}")"
        signed_by_identity "${dmg}" || die "${dmg##*/} is not signed by ${IDENTITY_NAME}"
        if [[ "${REHEARSAL}" == "1" ]]; then
            echo "    ${dmg##*/}: signed by ${IDENTITY_NAME}, not notarized (rehearsal)"
        else
            xcrun stapler validate "${dmg}" >/dev/null || die "${dmg##*/} carries no notarization ticket"
            spctl -a -t open --context context:primary-signature "${dmg}" 2>/dev/null \
                || die "${dmg##*/} is not accepted by Gatekeeper"
            echo "    ${dmg##*/}: signed by ${IDENTITY_NAME}, notarized, accepted"
        fi
        # the app inside is the architecture its name says
        scratch="$(mktemp -d "${TMPDIR:-/tmp}/liu-zip.XXXXXX")"
        ditto -x -k "${OUT}/$(mac_zip "${a}")" "${scratch}"
        archs="$(lipo -archs "${scratch}/Liu.app/Contents/MacOS/Liu" 2>/dev/null || true)"
        rm -rf "${scratch}"
        [[ "${archs}" == "${a}" ]] || die "$(mac_zip "${a}") holds a Liu for '${archs}', not ${a}"
    done

    step "Ed25519-signing the update artifacts"
    UPDATES=("$(mac_zip arm64)" "$(mac_zip x86_64)")
    [[ "${SHIP_LINUX}" == "1" ]] && UPDATES+=("${TARBALLS[@]}")
    for f in "${UPDATES[@]}"; do "${ROOT}/scripts/sign_ed25519.sh" "${OUT}/${f}" "${KEY}" >/dev/null; done

    step "Writing the feed over the live one's history"
    [[ -f "${LIVE_FEED}" ]] && cp "${LIVE_FEED}" "${FEED}"
    # os:arch, spelled out. Linux first, macOS last: the clients match on
    # (os, arch) and do not care about order, but the site's edge resolver
    # (calculusteam/liu-website) files each entry's sha256 and signature under
    # its arch alone, the last one winning.
    FEED_ARTS=()
    if [[ "${SHIP_LINUX}" == "1" ]]; then
        FEED_ARTS+=(--artifact "linux:arm64=${OUT}/${TARBALLS[0]}" --artifact "linux:x86_64=${OUT}/${TARBALLS[1]}")
    fi
    FEED_ARTS+=(--artifact "macos:arm64=${OUT}/$(mac_zip arm64)" --artifact "macos:x86_64=${OUT}/$(mac_zip x86_64)")
    python3 "${ROOT}/scripts/gen_feed.py" --feed "${FEED}" --version "${VERSION}" \
        --build "$(git -C "${ROOT}" rev-parse --short HEAD)" \
        --notes-url "https://github.com/${REPO}/releases/tag/${TAG}" \
        --min-os 11.0 --base-url "${DL_BASE}" "${FEED_ARTS[@]}"
    python3 "${ROOT}/scripts/sign_feed.py" --feed "${FEED}" --key "${KEY}"

    step "Writing and signing the manifest (sha256 of every artifact)"
    MAN_ARTS=()
    for a in arm64 x86_64; do
        MAN_ARTS+=(--artifact "dmg:${a}=${OUT}/$(mac_dmg "${a}")" --artifact "zip:${a}=${OUT}/$(mac_zip "${a}")")
    done
    if [[ "${SHIP_LINUX}" == "1" ]]; then
        MAN_ARTS+=(--artifact "tarball:linux-arm64=${OUT}/${TARBALLS[0]}"
                   --artifact "tarball:linux-x86_64=${OUT}/${TARBALLS[1]}"
                   --artifact "deb:linux-arm64=${OUT}/liu_${SYS}_arm64.deb"
                   --artifact "deb:linux-x86_64=${OUT}/liu_${SYS}_amd64.deb"
                   --artifact "rpm:linux-arm64=${OUT}/liu-${SYS}-1.aarch64.rpm"
                   --artifact "rpm:linux-x86_64=${OUT}/liu-${SYS}-1.x86_64.rpm"
                   --artifact "appimage:linux-arm64=${OUT}/Liu-${VERSION}-aarch64.AppImage"
                   --artifact "appimage:linux-x86_64=${OUT}/Liu-${VERSION}-x86_64.AppImage")
    fi
    python3 "${ROOT}/scripts/gen_manifest.py" --out "${MANIFEST}" --version "${VERSION}" --base-url "${DL_BASE}" \
        --pubkey-header "${VERIFY_H}" "${MAN_ARTS[@]}" >/dev/null
    # manifest.json lists a sha256 for every published artifact, including the
    # DMGs and Linux packages the in-app updater never touches. THIS signature is
    # what makes those checksums verifiable by hand, under the same Ed25519 root
    # as the updater feed.
    "${ROOT}/scripts/sign_ed25519.sh" "${MANIFEST}" "${KEY}" >/dev/null
    [[ "${REHEARSAL}" == "1" ]] && echo "rehearsal: signed with a throwaway key; never to be published" > "${OUT}/REHEARSAL"
else
    VERIFY_H="${PUBKEY_H}"
    [[ ! -f "${OUT}/REHEARSAL" ]] || die "${OUT} holds a rehearsal"
fi

# --- both runs: the exact bytes that will be published --------------------
# Feed and manifest hashes/sizes/URLs, every detached artifact signature and
# the manifest signature must agree with the application's compiled-in key
# (the rehearsal's own key in a rehearsal).
step "Verifying the release set"
python3 "${ROOT}/scripts/verify_release.py" --package-dir "${OUT}" --version "${VERSION}" \
    --base-url "${DL_BASE}" --pubkey-header "${VERIFY_H}" --platforms "${PLATFORMS}"

# The release page carries only what users and the updater download: a DMG and
# an update zip per macOS arch, per Linux arch (when shipped) the tarball
# (updater) and the .deb, .rpm and AppImage, and appcast.json — it MUST share
# the artifacts' github.com host, because the updater pins artifact URLs to the
# feed host. No .sig files (embedded in the feed), no manifest pair (lives in
# the repo).
FILES=()
for a in arm64 x86_64; do FILES+=("${OUT}/$(mac_dmg "${a}")" "${OUT}/$(mac_zip "${a}")"); done
if [[ "${SHIP_LINUX}" == "1" ]]; then for f in "${LINUX_FILES[@]}"; do FILES+=("${OUT}/${f}"); done; fi
FILES+=("${FEED}")
(cd "${OUT}" && shasum -a 256 "${FILES[@]##*/}") > "${OUT}/SHA256SUMS"

if [[ "${PUBLISH}" != "1" ]]; then
    echo
    cat "${OUT}/SHA256SUMS"
    echo
    if [[ "${REHEARSAL}" == "1" ]]; then
        echo "Rehearsal verified in ${OUT}; it is signed with a throwaway key and is never published."
    else
        echo "Verified; nothing has left this Mac. To publish ${TAG}:"
        echo "  GH_PUBLISH=1 scripts/release.sh"
    fi
    exit 0
fi

# --- run 2: publish ----------------------------------------------------------
case "$(git -C "${ROOT}" remote get-url origin)" in
    *github.com[:/]"${REPO}" | *github.com[:/]"${REPO}".git) ;;
    *) die "origin is not ${REPO}" ;;
esac
command -v gh >/dev/null || die "gh is needed to publish"
gh auth status >/dev/null 2>&1 || die "gh is not logged in"

step "Pushing ${TAG} (by name; never --tags)"
local_tag="$(git -C "${ROOT}" rev-parse "refs/tags/${TAG}")"
remote_tag="$(git -C "${ROOT}" ls-remote origin "refs/tags/${TAG}" | awk '{print $1}')"
if [[ -z "${remote_tag}" ]]; then
    git -C "${ROOT}" push origin "refs/tags/${TAG}"
elif [[ "${remote_tag}" != "${local_tag}" ]]; then
    die "origin's ${TAG} is another tag (${remote_tag}); it is not replaced from here"
fi

NAMES=()
for f in "${FILES[@]}"; do NAMES+=("${f##*/}"); done
NOTES=(--generate-notes)
if [[ -n "${LIU_RELEASE_NOTES:-}" ]]; then
    [[ -f "${LIU_RELEASE_NOTES}" ]] || die "LIU_RELEASE_NOTES ${LIU_RELEASE_NOTES} does not exist"
    NOTES=(--notes-file "${LIU_RELEASE_NOTES}")
fi

# A draft first: nobody (and no updater) sees the release until every asset is
# up and matches the bytes verified here.
step "Uploading ${#FILES[@]} assets to a draft of ${TAG}"
if gh release view "${TAG}" -R "${REPO}" >/dev/null 2>&1; then
    gh release upload "${TAG}" -R "${REPO}" "${FILES[@]}" --clobber
    # --clobber replaces same-name assets but never deletes retired ones — on a
    # re-publish, prune anything not in this set.
    gh release view "${TAG}" -R "${REPO}" --json assets --jq '.assets[].name' | while read -r a; do
        keep=false
        for n in "${NAMES[@]}"; do [[ "${a}" == "${n}" ]] && keep=true && break; done
        [[ "${keep}" == true ]] || gh release delete-asset "${TAG}" "${a}" -R "${REPO}" --yes
    done
else
    gh release create "${TAG}" -R "${REPO}" "${FILES[@]}" --verify-tag --draft --title "Liu ${VERSION}" "${NOTES[@]}"
fi

step "Comparing the uploaded assets with the files here"
release_json="$(gh api "repos/${REPO}/releases?per_page=30" --jq "[.[] | select(.tag_name == \"${TAG}\")][0]")"
[[ -n "${release_json}" && "${release_json}" != "null" ]] || die "the release ${TAG} is not listed"
for f in "${FILES[@]}"; do
    n="${f##*/}"
    asset="$(jq -c --arg n "${n}" '.assets[] | select(.name == $n)' <<<"${release_json}")"
    [[ -n "${asset}" ]] || die "${n} is not on the release"
    [[ "$(jq -r '.size' <<<"${asset}")" == "$(stat -f%z "${f}")" ]] || die "${n}: the uploaded size differs"
    digest="$(jq -r '.digest // empty' <<<"${asset}")"
    if [[ -n "${digest}" ]]; then
        [[ "${digest}" == "sha256:$(shasum -a 256 "${f}" | awk '{print $1}')" ]] || die "${n}: the uploaded bytes differ"
    else
        echo "warning: GitHub gave no digest for ${n}; its size matches" >&2
    fi
done
echo "    every asset matches"

step "Publishing ${TAG} (public, latest)"
# --draft=false matters on re-cuts too: deleting a tag demotes its release to a
# draft, and draft assets 404 on public URLs.
gh release edit "${TAG}" -R "${REPO}" --title "Liu ${VERSION}" --draft=false --latest

# The updater reads the latest release's appcast.json; GitHub's redirect for
# releases/latest can name the previous release for a while after a publish.
step "Reading the live feed back"
live_ok=0
for _ in $(seq 1 30); do
    if curl -fsSL "${FEED_URL}" -o "${WORK}/live-after.json" 2>/dev/null &&
       [[ "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("latest", ""))' "${WORK}/live-after.json" 2>/dev/null)" == "${VERSION}" ]]; then
        live_ok=1; break
    fi
    sleep 20
done
if [[ "${live_ok}" == "1" ]]; then
    cmp -s "${WORK}/live-after.json" "${FEED}" && echo "    ${FEED_URL} serves this release's feed" \
        || die "${FEED_URL} names ${VERSION} but is not the feed verified here"
else
    echo "warning: ${FEED_URL} did not name ${VERSION} within 10 minutes (GitHub's latest redirect); check it again" >&2
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
