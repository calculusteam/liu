#!/usr/bin/env bash
#
# Bump version + sha256 across every store packaging file from a release
# manifest, so a checksum is never hand-copied.
#
#   scripts/update-manifests.sh [manifest.json]
#
# Default manifest: build/package/manifest.json (written by gen_manifest.py in
# the release pipeline). Pass a path, or set MANIFEST_URL to fetch one (e.g.
# https://liu.software/manifest.json). Rewrites:
#   packaging/homebrew/Casks/liu.rb          (version + per-arch DMG sha256)
#   packaging/aur/liu-bin/PKGBUILD + .SRCINFO(pkgver + per-arch tarball sha)
#   packaging/debian/changelog               (new top entry)
#   packaging/rpm/liu.spec                   (Version)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(tr -d '[:space:]' < "${ROOT}/VERSION")"
MANIFEST="${1:-${ROOT}/build/package/manifest.json}"

if [[ -n "${MANIFEST_URL:-}" ]]; then
    MANIFEST="$(mktemp)"; curl -fsSL "${MANIFEST_URL}" -o "${MANIFEST}"
fi
[[ -f "${MANIFEST}" ]] || { echo "error: manifest not found: ${MANIFEST}" >&2; exit 1; }

# Pull the shas we need out of the manifest (by kind:arch). Missing ones stay
# empty and the corresponding rewrite is skipped (kept as the placeholder).
read_sha() { python3 - "$MANIFEST" "$1" "$2" <<'PY'
import json,sys
m=json.load(open(sys.argv[1]))
for a in m.get("artifacts",[]):
    if a.get("kind")==sys.argv[2] and a.get("arch")==sys.argv[3]:
        print(a.get("sha256","")); break
PY
}

SHA_DMG_ARM=$(read_sha dmg arm64)
SHA_DMG_X64=$(read_sha dmg x86_64)
SHA_TGZ_X64=$(read_sha tarball linux-x86_64)
SHA_TGZ_ARM=$(read_sha tarball linux-arm64)

# In-place rewrites via python (regex; robust vs. sed across BSD/GNU).
pyedit() { python3 - "$@" <<'PY'
import re,sys
path=sys.argv[1]; s=open(path).read()
for i in range(2,len(sys.argv),2):
    pat,repl=sys.argv[i],sys.argv[i+1]
    s=re.sub(pat,repl,s,flags=re.M)
open(path,"w").write(s)
PY
}

# --- Homebrew cask: version + the two sha256 (arm block first, intel second) -
CASK="${ROOT}/packaging/homebrew/Casks/liu.rb"
pyedit "$CASK" '^(\s*version\s+)"[^"]*"' "\\g<1>\"${VERSION}\""
if [[ -n "$SHA_DMG_ARM" ]]; then
  python3 - "$CASK" "$SHA_DMG_ARM" "$SHA_DMG_X64" <<'PY'
import re,sys
p,arm,intel=sys.argv[1],sys.argv[2],sys.argv[3]
s=open(p).read()
# replace the sha256 inside on_arm then on_intel, in order
def repl_block(s, marker, sha):
    i=s.index(marker)
    return s[:i]+re.sub(r'sha256 "[0-9a-f]{64}"', f'sha256 "{sha}"', s[i:], count=1)
s=repl_block(s,'on_arm do',arm)
if intel: s=repl_block(s,'on_intel do',intel)
open(p,'w').write(s)
PY
fi

# --- AUR PKGBUILD: pkgver + per-arch tarball sha; regenerate .SRCINFO ---------
PKGB="${ROOT}/packaging/aur/liu-bin/PKGBUILD"
pyedit "$PKGB" '^(pkgver=).*$' "\\g<1>${VERSION}"
[[ -n "$SHA_TGZ_X64" ]] && pyedit "$PKGB" "(sha256sums_x86_64=\(')[0-9a-f]{64}" "\\g<1>${SHA_TGZ_X64}"
[[ -n "$SHA_TGZ_ARM" ]] && pyedit "$PKGB" "(sha256sums_aarch64=\(')[0-9a-f]{64}" "\\g<1>${SHA_TGZ_ARM}"
SRCINFO="${ROOT}/packaging/aur/liu-bin/.SRCINFO"
if command -v makepkg >/dev/null 2>&1; then
  ( cd "${ROOT}/packaging/aur/liu-bin" && makepkg --printsrcinfo > .SRCINFO )
else
  # No makepkg here — mirror the same fields into the committed .SRCINFO.
  pyedit "$SRCINFO" '^(\tpkgver = ).*$' "\\g<1>${VERSION}"
  pyedit "$SRCINFO" '(downloads/v)[^/]+/liu-[^ ]+linux-x86_64\.tar\.gz' "\\g<1>${VERSION}/liu-${VERSION}-linux-x86_64.tar.gz"
  pyedit "$SRCINFO" '(downloads/v)[^/]+/liu-[^ ]+linux-arm64\.tar\.gz' "\\g<1>${VERSION}/liu-${VERSION}-linux-arm64.tar.gz"
  pyedit "$SRCINFO" '(liu-)[^-]+(-linux-x86_64\.tar\.gz::)' "\\g<1>${VERSION}\\g<2>"
  pyedit "$SRCINFO" '(liu-)[^-]+(-linux-arm64\.tar\.gz::)' "\\g<1>${VERSION}\\g<2>"
  [[ -n "$SHA_TGZ_X64" ]] && pyedit "$SRCINFO" '(sha256sums_x86_64 = )[0-9a-f]{64}' "\\g<1>${SHA_TGZ_X64}"
  [[ -n "$SHA_TGZ_ARM" ]] && pyedit "$SRCINFO" '(sha256sums_aarch64 = )[0-9a-f]{64}' "\\g<1>${SHA_TGZ_ARM}"
fi

# --- RPM spec: Version -------------------------------------------------------
pyedit "${ROOT}/packaging/rpm/liu.spec" '^(Version:\s*).*$' "\\g<1>${VERSION}"

# --- Debian changelog: prepend a new entry if the top isn't this version -----
CHANGELOG="${ROOT}/packaging/debian/changelog"
if ! head -1 "${CHANGELOG}" | grep -q "(${VERSION}-"; then
  if command -v dch >/dev/null 2>&1; then
    ( cd "${ROOT}/packaging" && DEBEMAIL="hello@liu.software" DEBFULLNAME="calculus.team" \
        dch --changelog debian/changelog -v "${VERSION}-1" "New upstream release ${VERSION}." )
  else
    DATE="$(date -u '+%a, %d %b %Y %H:%M:%S +0000')"
    { printf 'liu (%s-1) noble; urgency=medium\n\n  * New upstream release %s.\n\n -- calculus.team <hello@liu.software>  %s\n\n' \
        "${VERSION}" "${VERSION}" "${DATE}"; cat "${CHANGELOG}"; } > "${CHANGELOG}.new"
    mv "${CHANGELOG}.new" "${CHANGELOG}"
  fi
fi

echo "Updated store manifests to ${VERSION} (shas from ${MANIFEST})."
