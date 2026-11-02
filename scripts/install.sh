#!/bin/bash
#
# Liu installer — https://github.com/calculusteam/liu/releases/latest/download/install.sh
#
# Downloads the latest signed Liu build from the release feed, verifies it, and
# installs it: on macOS the .app (-> /Applications), on Linux the release
# tarball (-> /opt/liu, or ~/.local/liu without write access to /opt).
#
# Usage:
#   curl -fsSL https://github.com/calculusteam/liu/releases/latest/download/install.sh | bash             # latest
#   curl -fsSL https://github.com/calculusteam/liu/releases/latest/download/install.sh | bash -s 0.1.0    # specific version
#   curl -fsSL https://github.com/calculusteam/liu/releases/latest/download/install.sh | bash -s -- --uninstall   # Linux: remove it again
#   install.sh --help                    # the same, and what --uninstall removes
#
# Env:
#   LIU_FEED_URL          override the appcast feed URL
#   LIU_APP_DIR           macOS install dir (default: /Applications, else ~/Applications)
#   LIU_PREFIX            Linux install prefix (default: /opt/liu, else ~/.local/liu); --uninstall looks there
#   LIU_BIN_DIR           Linux: where the `liu` link goes (default: /usr/local/bin, else ~/.local/bin)
#   LIU_DATA_DIR          Linux: the data dir for icons and the desktop entry (default: /usr/share, else ~/.local/share)
#   LIU_NO_OPEN=1         don't launch Liu / don't run a post-install check
#   LIU_INSECURE_SKIP_SIG=1
#                         EXPLICIT opt-out: install WITHOUT Ed25519 signature
#                         verification when no Ed25519-capable openssl exists.
#                         This is UNSAFE — see the trust model below. Off by default.
#
# ---------------------------------------------------------------------------
# Trust model (mirrors the in-app auto-updater, fail-closed at every stage):
#
#   * https-only on BOTH the feed and the artifact (curl --proto =https
#     --proto-redir =https, plus an explicit artifact-url scheme check), and
#     the artifact host is pinned to the feed host (mirrors the in-app updater)
#     so a tampered feed can't aim the download at an arbitrary server.
#   * The feed is TLS-transported but NOT signature-pinned, so it is treated as
#     untrusted: every security decision below is anchored on the artifact's
#     detached Ed25519 signature, verified against the public key COMPILED INTO
#     this script (and into the app). Controlling the feed or CDN does not let an
#     attacker ship a build that verifies.
#   * Download is size-capped (curl --max-filesize) using the feed-declared size
#     + 5% slack + 64 KiB, clamped to a hard 1 GiB ceiling; the appcast itself is
#     capped at 5 MiB. A defense-in-depth on-disk size re-check follows.
#   * SHA-256 is MANDATORY (integrity).
#   * Ed25519 is MANDATORY wherever a capable openssl exists (Linux system
#     openssl 3.x, Homebrew openssl@3 on macOS). When NO capable verifier exists
#     (stock macOS ships LibreSSL, which lacks `pkeyutl -rawin` for Ed25519) the
#     installer FAILS CLOSED by default and refuses to install. The only way to
#     proceed without a signature check is to consciously set
#     LIU_INSECURE_SKIP_SIG=1, which prints a loud warning. The in-app updater
#     has no such downgrade (it links real libcrypto); this matches its posture
#     as closely as the host toolchain allows.
#   * macOS minimum-OS gate; atomic install swap with rollback on both OSes.
#   * Signed-downgrade guard: an existing install is never replaced by a
#     non-strictly-newer build (read from the extracted Info.plist on macOS, or a
#     VERSION marker on Linux).
#
# Residual gap (inherent to the design, same as the app): because the feed is
# TLS-only and not signed, a feed-level attacker can still FREEZE updates (serve
# a stale "latest") or suppress a "yanked": true flag. Neither yields code
# execution — the Ed25519 anchor and the monotonic re-check prevent that.
# ---------------------------------------------------------------------------

set -e

# --- embedded Ed25519 public key (matches src/core/update_pubkey.h) ----------
# The raw 32-byte DER tail of this PEM is the LIU_UPDATE_PUBKEY array in
# src/core/update_pubkey.h — the two MUST stay in sync across key rotation.
LIU_PUBKEY_PEM="-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEAhfVzMd+q5Xpoawd6s+2vGdJsvlgl7WOO55Nn3gZzbV4=
-----END PUBLIC KEY-----"

FEED_URL="${LIU_FEED_URL:-https://github.com/calculusteam/liu/releases/latest/download/appcast.json}"

# Hard ceiling for any artifact download, identical to the app
# (LIU_UPDATE_HARD_CAP in src/update/updater_macos.m = 1 GiB).
HARD_CAP=1073741824
# The appcast feed itself is bounded to 5 MiB (updater_macos.m caps the feed
# fetch at --max-filesize 5242880).
FEED_CAP=5242880

err()  { printf 'Error: %s\n' "$1" >&2; exit 1; }
note() { printf '%s\n' "$1" >&2; }
warn() { printf 'WARNING: %s\n' "$1" >&2; }

# --- install manifest + uninstall (Linux) -----------------------------------------
# What a Linux install wrote is listed in <prefix>/.install-manifest, one record a
# line, fields separated by a TAB (a path with a TAB or a newline is never
# recorded). `install.sh --uninstall` removes exactly that list:
#   version  <v>                    the version that was installed
#   prefix   <dir>                  the install prefix
#   d  <dir>                        a directory the install had to create
#   f  <path>  <sha256>             a file it wrote; removed only while it still has that content
#   l  <path>  <target>             a symbolic link; removed only while it still points there
#   cache  <dir>                    an icon theme whose cache the install refreshed
TAB="$(printf '\t')"
NL="$(printf '\n_')"; NL="${NL%_}"
MF_NAME=".install-manifest"

usage() {
    cat <<'EOF'
Liu installer

  install.sh [VERSION]                install the latest (or the given) signed build
  install.sh --uninstall [--dry-run]  remove what a previous install.sh run installed (Linux)
  install.sh --help                   this text

Install
  macOS   Liu.app -> /Applications (or ~/Applications).
  Linux   the release tree -> /opt/liu, or ~/.local/liu without write access to /opt;
          a `liu` link in /usr/local/bin or ~/.local/bin; the terminfo entry xterm-liu;
          the icons (<data>/icons/hicolor) and <data>/applications/liu.desktop, where
          <data> is /usr/share, or ~/.local/share without write access to it.
          Everything written is listed in <prefix>/.install-manifest.

Uninstall (Linux)
  Removes what the manifest lists, and nothing else: a file only while it still has the
  content that was installed, a link only while it points into the prefix, a directory
  only if it is empty and the install created it. A prefix holding other files stays,
  with them. An install without a manifest (an older install.sh, or one the in-app
  updater has since rewritten) is removed by its known layout; the prefix must hold
  VERSION and bin/Liu. Installs by apt, dnf or zypper are refused (use the package
  manager); so is anything else that is not a Liu install. Settings and data
  (~/.config/Liu, ~/.local/share/liu) are never touched.
  Looks in /opt/liu and ~/.local/liu, or only in $LIU_PREFIX. With nothing installed it
  says so and exits 0; a $LIU_PREFIX that holds no install, a refusal and a failed
  removal exit 1. --dry-run lists what would be removed and changes nothing.

Environment
  LIU_FEED_URL      override the appcast feed URL
  LIU_APP_DIR       macOS install dir
  LIU_PREFIX        Linux install prefix (also tells --uninstall where to look)
  LIU_BIN_DIR       Linux: where the `liu` link goes (/usr/local/bin, else ~/.local/bin)
  LIU_DATA_DIR      Linux: the data dir for icons and the desktop entry (/usr/share, else ~/.local/share)
  LIU_NO_OPEN=1     don't launch Liu / don't run a post-install check
  LIU_INSECURE_SKIP_SIG=1   install without Ed25519 verification (UNSAFE; see the header)
EOF
}

# sha256 of one file, lowercase hex
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then _sh="$(sha256sum -- "$1")" || return 1
    elif command -v shasum >/dev/null 2>&1; then _sh="$(shasum -a 256 -- "$1")" || return 1
    else return 1
    fi
    printf '%s\n' "${_sh%% *}"
}

# an absolute path that can be written to the manifest and read back
path_ok() {
    case "$1" in /*) ;; *) return 1 ;; esac
    case "$1" in *"$TAB"*|*"$NL"*) return 1 ;; esac
    case "$1" in */..|*/../*|*/.|*/./*|*//*) return 1 ;; esac
    return 0
}

# --- install side: record what is written ------------------------------------------
mf_add() {   # mf_add <kind> <path> [extra]
    [ -n "${MF:-}" ] || return 0
    if ! path_ok "$2"; then
        warn "not recording $2 (an unusual path): install.sh --uninstall will not remove it."
        return 0
    fi
    if [ -n "${3:-}" ]; then printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> "$MF" || true
    else printf '%s\t%s\n' "$1" "$2" >> "$MF" || true
    fi
    return 0
}
mf_file() {   # a file just written: its checksum goes with it
    _h="$(sha256_of "$1" 2>/dev/null)" || _h=""
    if [ -n "$_h" ]; then mf_add f "$1" "$_h"; else warn "couldn't checksum $1: install.sh --uninstall will leave it."; fi
    return 0
}
mf_mkdir_p() {   # mkdir -p that records each directory it had to create, shallowest first
    _missing=""; _d="$1"
    while [ -n "$_d" ] && [ "$_d" != "/" ] && [ "$_d" != "." ] && [ ! -e "$_d" ] && [ ! -L "$_d" ]; do
        _missing="$_d
$_missing"
        _d="$(dirname "$_d")"
    done
    mkdir -p "$1" || return 1
    printf '%s' "$_missing" | while IFS= read -r _m; do
        [ -n "$_m" ] && [ -d "$_m" ] && mf_add d "$_m"
    done
    return 0
}

# where `tic -x` may have put the entry: $TERMINFO, ~/.terminfo and the system directories
ti_dirs() {
    {
        [ -n "${TERMINFO:-}" ] && printf '%s\n' "$TERMINFO"
        [ -n "${HOME:-}" ] && printf '%s\n' "$HOME/.terminfo"
        infocmp -D 2>/dev/null
        printf '%s\n' /etc/terminfo /lib/terminfo /usr/share/terminfo /usr/lib/terminfo /usr/local/share/terminfo /usr/local/lib/terminfo
    } | awk 'NF && !seen[$0]++'
}
ti_state() {   # one line per candidate: dir, dir exists, x/ exists, checksum of x/xterm-liu or -
    ti_dirs | while IFS= read -r _t; do
        _e=-; [ -f "$_t/x/xterm-liu" ] && [ ! -L "$_t/x/xterm-liu" ] && _e="$(sha256_of "$_t/x/xterm-liu" 2>/dev/null || echo -)"
        _a=0; [ -d "$_t" ] && _a=1
        _b=0; [ -d "$_t/x" ] && _b=1
        printf '%s\t%s\t%s\t%s\n' "$_t" "$_a" "$_b" "$_e"
    done
}
ti_record() {   # ti_record <state before>: whatever tic wrote or replaced
    ti_state | while IFS="$TAB" read -r _t _a _b _e; do
        [ "$_e" != "-" ] || continue
        _old="$(awk -F'\t' -v d="$_t" '$1 == d { print $4; exit }' "$1")"
        [ "$_old" != "$_e" ] || continue
        _oa="$(awk -F'\t' -v d="$_t" '$1 == d { print $2; exit }' "$1")"
        _ob="$(awk -F'\t' -v d="$_t" '$1 == d { print $3; exit }' "$1")"
        [ "$_oa" = 1 ] || mf_add d "$_t"
        [ "$_ob" = 1 ] || mf_add d "$_t/x"
        mf_add f "$_t/x/xterm-liu" "$_e"
    done
}

# the finished manifest: the tree, then the records written along the way, then the older
# install's records outside this prefix (a reinstall must not forget what the first made)
mf_finish() {   # mf_finish <prefix> <version> <older manifest or ""> <records file> <out>
    _p="$1"; _v="$2"; _old="$3"; _rec="$4"; _out="$5"
    {
        printf '# liu install manifest 1 -- written by install.sh, read by install.sh --uninstall\n'
        printf 'version\t%s\n' "$_v"
        printf 'prefix\t%s\n' "$_p"
        ( cd "$_p" && find . -mindepth 1 -type d | LC_ALL=C sort ) \
            | awk -v p="$_p" 'index($0, "\t") == 0 { sub(/^\.\//, ""); printf "d\t%s/%s\n", p, $0 }'
        ( cd "$_p" && if command -v sha256sum >/dev/null 2>&1; then find . -type f -exec sha256sum {} +
                      else find . -type f -exec shasum -a 256 {} +; fi ) \
            | LC_ALL=C sort -k2 \
            | awk -v p="$_p" '{ h = $1; sub(/^[0-9a-f]+ [ *]\.\//, ""); if (index($0, "\t") == 0) printf "f\t%s/%s\t%s\n", p, $0, h }'
        ( cd "$_p" && find . -type l ) | while IFS= read -r _l; do
            _l="${_l#./}"; _t="$(readlink "$_p/$_l")" || continue
            path_ok "$_p/$_l" && printf 'l\t%s/%s\t%s\n' "$_p" "$_l" "$_t"
        done
        {
            cat "$_rec"
            [ -n "$_old" ] && awk -F'\t' -v p="$_p/" '($1 == "d" || $1 == "f" || $1 == "l" || $1 == "cache") && substr($2, 1, length(p)) != p' "$_old"
        } | awk -F'\t' '!seen[$1 FS $2]++'
    } > "$_out"
}

# --- uninstall side ----------------------------------------------------------------
UN_REMOVED=0; UN_LEFT=0; UN_FAILED=0; UN_DRY=0
un_say() { printf '%s\n' "$1" >&2; }
un_rm_file() {   # un_rm_file <path> [quiet]
    if [ "$UN_DRY" = 1 ]; then
        [ "${2:-}" = quiet ] || un_say "  would remove $1"
        printf '%s\n' "$1" >> "$UN_TMP/gone"; UN_REMOVED=$((UN_REMOVED + 1)); return 0
    fi
    rm -f -- "$1" 2>/dev/null
    if [ ! -e "$1" ] && [ ! -L "$1" ]; then
        [ "${2:-}" = quiet ] || un_say "  removed $1"
        UN_REMOVED=$((UN_REMOVED + 1))
    else
        un_say "  could not remove $1"; UN_FAILED=$((UN_FAILED + 1))
    fi
    return 0
}
un_rmdir() {   # un_rmdir <dir> [quiet]: a directory goes only if it is a real directory and empty
    [ -d "$1" ] && [ ! -L "$1" ] || return 0
    if [ "$UN_DRY" = 1 ]; then   # empty once everything listed as removed is gone
        _uempty=1
        ls -A "$1" > "$UN_TMP/ls" 2>/dev/null
        while IFS= read -r _ue; do grep -qxF -- "$1/$_ue" "$UN_TMP/gone" || _uempty=0; done < "$UN_TMP/ls"
        if [ "$_uempty" = 1 ]; then
            [ "${2:-}" = quiet ] || un_say "  would remove directory $1"
            printf '%s\n' "$1" >> "$UN_TMP/gone"
        else
            un_say "  would keep directory $1 (not empty)"
        fi
        return 0
    fi
    if rmdir -- "$1" 2>/dev/null; then [ "${2:-}" = quiet ] || un_say "  removed directory $1"
    else un_say "  kept directory $1 (not empty)"; UN_LEFT=$((UN_LEFT + 1)); fi
    return 0
}
un_under() {   # un_under <path> <dir>: the path is inside the directory, by name
    case "$1" in "$2"/*) return 0 ;; esac
    return 1
}
un_inside_phys() {   # the directory holding $1 really is inside the prefix (no link leads out of it)
    _ud="$(cd "$(dirname "$1")" 2>/dev/null && pwd -P)" || return 1
    case "$_ud" in "$UN_PHYS"|"$UN_PHYS"/*) return 0 ;; esac
    return 1
}
un_shape_ok() {   # a path outside the prefix that install.sh could have written
    case "$1" in
        */applications/liu.desktop|*/icons/hicolor/*/apps/liu.png|*/icons/hicolor/icon-theme.cache) return 0 ;;
        */?/xterm-liu|*/??/xterm-liu) return 0 ;;
    esac
    return 1
}
un_file() {   # un_file <path> <sha256>: remove it if it is still the file that was installed
    [ -e "$1" ] || [ -L "$1" ] || return 0
    path_ok "$1" || { un_say "  skipped $1 (not a plain path)"; return 0; }
    if un_under "$1" "$UN_PREFIX"; then
        if ! un_inside_phys "$1"; then un_say "  left $1 (a link leads out of the prefix)"; UN_LEFT=$((UN_LEFT + 1)); return 0; fi
        _uq=quiet
    else
        if ! un_shape_ok "$1"; then un_say "  skipped $1 (not a path install.sh writes)"; UN_LEFT=$((UN_LEFT + 1)); return 0; fi
        _uq=""
    fi
    if [ -L "$1" ] || [ ! -f "$1" ]; then un_say "  left $1 (no longer the file install.sh wrote)"; UN_LEFT=$((UN_LEFT + 1)); return 0; fi
    _uh="$(sha256_of "$1" 2>/dev/null)" || _uh=""
    if [ "$_uh" != "$2" ]; then un_say "  left $1 (changed since it was installed)"; UN_LEFT=$((UN_LEFT + 1)); return 0; fi
    un_rm_file "$1" "$_uq"
    return 0
}
un_link() {   # un_link <path> <target>: remove it if it still points where install.sh pointed it
    [ -L "$1" ] || return 0
    path_ok "$1" || { un_say "  skipped $1 (not a plain path)"; return 0; }
    if un_under "$1" "$UN_PREFIX"; then
        un_inside_phys "$1" || return 0
    else
        case "$1" in */liu) ;; *) un_say "  skipped $1 (not a path install.sh writes)"; UN_LEFT=$((UN_LEFT + 1)); return 0 ;; esac
        if ! un_under "$2" "$UN_PREFIX"; then un_say "  skipped $1 (it does not point into $UN_PREFIX)"; UN_LEFT=$((UN_LEFT + 1)); return 0; fi
    fi
    if [ "$(readlink "$1" 2>/dev/null)" != "$2" ]; then un_say "  left $1 (it points somewhere else now)"; UN_LEFT=$((UN_LEFT + 1)); return 0; fi
    if un_under "$1" "$UN_PREFIX"; then un_rm_file "$1" quiet; else un_rm_file "$1"; fi
    return 0
}
un_pkg_owner() {   # the package manager that owns a file, if dpkg or rpm knows it
    if command -v dpkg >/dev/null 2>&1 && dpkg -S "$1" >/dev/null 2>&1; then echo "dpkg: apt remove liu"; return 0; fi
    if command -v rpm >/dev/null 2>&1 && rpm -qf "$1" >/dev/null 2>&1; then echo "rpm: dnf remove liu (zypper remove liu)"; return 0; fi
    return 1
}
un_refresh_icons() {   # the install refreshed this icon theme's cache: refresh it again
    [ "$UN_DRY" = 1 ] && return 0
    [ -f "$1/icon-theme.cache" ] && [ -f "$1/index.theme" ] || return 0
    command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q "$1" 2>/dev/null
    return 0
}
un_tree() {   # un_tree <dir>: a directory of the prefix, file by file and then folder by folder
    [ -d "$1" ] && [ ! -L "$1" ] || return 0
    if [ "$(cd "$1" && pwd -P)" != "$UN_PHYS/${1##*/}" ]; then un_say "  left $1 (a link leads out of the prefix)"; return 0; fi
    _un="$(find "$1" ! -type d 2>/dev/null | wc -l | tr -d ' ')"
    if [ "$UN_DRY" = 1 ]; then
        un_say "  would remove $1 ($_un files)"; UN_REMOVED=$((UN_REMOVED + _un))
        find "$1" >> "$UN_TMP/gone" 2>/dev/null; return 0
    fi
    find "$1" ! -type d -exec rm -f -- {} + 2>/dev/null
    find "$1" -depth -type d -exec rmdir -- {} + 2>/dev/null
    if [ -e "$1" ] || [ -L "$1" ]; then
        _uleft="$(find "$1" ! -type d 2>/dev/null | wc -l | tr -d ' ')"
        if [ "$_uleft" != 0 ]; then un_say "  could not remove all of $1 ($_uleft files left)"; UN_FAILED=$((UN_FAILED + 1)); fi
        UN_REMOVED=$((UN_REMOVED + _un - _uleft))
    else
        UN_REMOVED=$((UN_REMOVED + _un))
    fi
    return 0
}

# what install.sh puts in the prefix, by name: used where there is no (current) manifest
un_layout() {
    for _ul in bin/Liu bin/liu-notify bin/liu-history VERSION liu.terminfo LICENSE NOTICE THIRD_PARTY_NOTICES.md "$MF_NAME"; do
        if [ -f "$UN_PREFIX/$_ul" ] || [ -L "$UN_PREFIX/$_ul" ]; then
            un_inside_phys "$UN_PREFIX/$_ul" && un_rm_file "$UN_PREFIX/$_ul" quiet
        fi
    done
    un_tree "$UN_PREFIX/assets"
    un_tree "$UN_PREFIX/share"
    un_rmdir "$UN_PREFIX/bin" quiet
}

# outside the prefix, for an install with no manifest: only what provably is Liu's
un_legacy_outside() {
    for _ud in "${LIU_BIN_DIR:-}" /usr/local/bin "${HOME:-}/.local/bin"; do
        [ -n "$_ud" ] && [ -L "$_ud/liu" ] && un_link "$_ud/liu" "$UN_PREFIX/bin/Liu"
    done
    for _ud in "${LIU_DATA_DIR:-}" /usr/share "${HOME:-}/.local/share"; do
        [ -n "$_ud" ] || continue
        _uf="$_ud/applications/liu.desktop"
        if [ -f "$_uf" ] && [ ! -L "$_uf" ] && grep -qxF "Exec=$UN_PREFIX/bin/Liu" "$_uf" && grep -qx 'StartupWMClass=liu' "$_uf"; then
            un_rm_file "$_uf"
        fi
        if [ -d "$UN_PREFIX/share/icons/hicolor" ] && [ -d "$_ud/icons/hicolor" ]; then
            ( cd "$UN_PREFIX/share/icons/hicolor" && find . -type f -name liu.png ) > "$UN_TMP/icons.list" 2>/dev/null
            while IFS= read -r _ui; do
                _ui="${_ui#./}"; _uf="$_ud/icons/hicolor/$_ui"
                if [ -f "$_uf" ] && [ ! -L "$_uf" ] && cmp -s "$_uf" "$UN_PREFIX/share/icons/hicolor/$_ui"; then un_rm_file "$_uf"; fi
            done < "$UN_TMP/icons.list"
            un_refresh_icons "$_ud/icons/hicolor"
        fi
    done
    # the compiled terminfo entry: byte for byte what tic makes of this prefix's liu.terminfo
    # (the install ran the same tic on the same file), and not a package's
    if [ -f "$UN_PREFIX/liu.terminfo" ] && command -v tic >/dev/null 2>&1 \
            && tic -x -o "$UN_TMP/ti" "$UN_PREFIX/liu.terminfo" >/dev/null 2>&1 && [ -f "$UN_TMP/ti/x/xterm-liu" ]; then
        ti_dirs > "$UN_TMP/ti.list"
        while IFS= read -r _ut; do
            _uf="$_ut/x/xterm-liu"
            [ -f "$_uf" ] && [ ! -L "$_uf" ] || continue
            if ! cmp -s "$_uf" "$UN_TMP/ti/x/xterm-liu"; then un_say "  left $_uf (not what this install's liu.terminfo compiles to)"; UN_LEFT=$((UN_LEFT + 1)); continue; fi
            if un_pkg_owner "$_uf" >/dev/null; then un_say "  left $_uf (a package owns it)"; UN_LEFT=$((UN_LEFT + 1)); continue; fi
            un_rm_file "$_uf"
        done < "$UN_TMP/ti.list"
        find "$UN_TMP/ti" ! -type d -exec rm -f -- {} + 2>/dev/null
        find "$UN_TMP/ti" -depth -type d -exec rmdir -- {} + 2>/dev/null
    fi
}

# an install with a manifest: its records, nothing else
un_by_manifest() {
    _um="$UN_PREFIX/$MF_NAME"
    awk -F'\t' '$1 == "f" && NF == 3 { print $2 "\t" $3 }' "$_um" > "$UN_TMP/f.list"
    awk -F'\t' '$1 == "l" && NF == 3 { print $2 "\t" $3 }' "$_um" > "$UN_TMP/l.list"
    awk -F'\t' '$1 == "d" && NF == 2 { print length($2) "\t" $2 }' "$_um" | LC_ALL=C sort -t "$TAB" -k1,1nr -k2 | cut -f2- > "$UN_TMP/d.list"
    awk -F'\t' '$1 == "cache" && NF == 2 { print $2 }' "$_um" > "$UN_TMP/c.list"
    # outside the prefix first (that is what points into it), then the tree
    while IFS="$TAB" read -r _up _uh; do un_under "$_up" "$UN_PREFIX" || un_file "$_up" "$_uh"; done < "$UN_TMP/f.list"
    while IFS="$TAB" read -r _up _ut; do un_under "$_up" "$UN_PREFIX" || un_link "$_up" "$_ut"; done < "$UN_TMP/l.list"
    _ucur="$(tr -d '[:space:]' < "$UN_PREFIX/VERSION" 2>/dev/null)"
    _uver="$(awk -F'\t' '$1 == "version" { print $2; exit }' "$_um")"
    if [ "$_ucur" = "$_uver" ]; then
        while IFS="$TAB" read -r _up _uh; do un_under "$_up" "$UN_PREFIX" && un_file "$_up" "$_uh"; done < "$UN_TMP/f.list"
        while IFS="$TAB" read -r _up _ut; do un_under "$_up" "$UN_PREFIX" && un_link "$_up" "$_ut"; done < "$UN_TMP/l.list"
    else
        un_say "  (the tree is $_ucur, the manifest says $_uver: an update replaced it; removing the tree by its layout)"
        un_layout
    fi
    while IFS= read -r _uc; do un_refresh_icons "$_uc"; done < "$UN_TMP/c.list"
    if [ "$UN_FAILED" = 0 ]; then
        if [ -f "$_um" ] && un_inside_phys "$_um"; then un_rm_file "$_um" quiet; fi
        while IFS= read -r _ud; do
            path_ok "$_ud" || continue
            if un_under "$_ud" "$UN_PREFIX" || [ "$_ud" = "$UN_PREFIX" ]; then un_rmdir "$_ud" quiet; else un_rmdir "$_ud"; fi
        done < "$UN_TMP/d.list"
    else
        un_say "  kept $_um (something could not be removed; run this again once that is fixed)"
    fi
}

un_cleanup() {   # the scratch directory holds only the lists written above
    rm -f -- "$UN_TMP/f.list" "$UN_TMP/l.list" "$UN_TMP/d.list" "$UN_TMP/c.list" "$UN_TMP/icons.list" "$UN_TMP/ti.list" \
        "$UN_TMP/gone" "$UN_TMP/ls"
    rmdir -- "$UN_TMP" 2>/dev/null
    return 0
}

# one prefix. Returns 0 removed, 1 could not, 2 not ours to remove, 3 nothing there.
un_prefix() {
    UN_PREFIX="$1"
    while [ "${UN_PREFIX%/}" != "$UN_PREFIX" ] && [ -n "${UN_PREFIX%/}" ]; do UN_PREFIX="${UN_PREFIX%/}"; done
    if ! path_ok "$UN_PREFIX"; then un_say "Refusing '$UN_PREFIX': not a plain absolute path."; return 2; fi
    case "$UN_PREFIX" in
        /|/usr|/usr/local|/bin|/sbin|/lib|/etc|/var|/home|/opt|/root|/tmp|"${HOME:-/nonexistent}")
            un_say "Refusing $UN_PREFIX: not a Liu install directory."; return 2 ;;
    esac
    if [ -L "$UN_PREFIX" ]; then un_say "Refusing $UN_PREFIX: it is a symbolic link. Set LIU_PREFIX to the directory it leads to."; return 2; fi
    if [ ! -d "$UN_PREFIX" ]; then un_say "No Liu install at $UN_PREFIX."; return 3; fi
    # A Liu tree holds VERSION and bin/Liu; its own manifest also says so (a removal that
    # stopped half way has taken those files already and is finished by running this again).
    if { [ ! -f "$UN_PREFIX/VERSION" ] || [ ! -f "$UN_PREFIX/bin/Liu" ] || [ -L "$UN_PREFIX/bin/Liu" ]; } \
            && ! { [ -f "$UN_PREFIX/$MF_NAME" ] && [ ! -L "$UN_PREFIX/$MF_NAME" ] \
                   && head -n 1 "$UN_PREFIX/$MF_NAME" | grep -q '^# liu install manifest 1 '; }; then
        un_say "Not removing $UN_PREFIX: it has no VERSION and bin/Liu, so it is not a Liu install."; return 2
    fi
    if _uo="$(un_pkg_owner "$UN_PREFIX/bin/Liu")"; then
        un_say "$UN_PREFIX belongs to a package (${_uo%%:*}): remove it with '${_uo#*: }' (as root), not with this script."; return 2
    fi
    if [ "$UN_DRY" != 1 ] && [ ! -w "$UN_PREFIX" ]; then
        un_say "$UN_PREFIX is not writable by you. It was installed by another user, probably root: run this as that user (sudo)."; return 1
    fi
    UN_PHYS="$(cd "$UN_PREFIX" && pwd -P)"
    UN_TMP="$(mktemp -d "${TMPDIR:-/tmp}/liu-uninstall.XXXXXX")" || { un_say "couldn't create a temporary directory."; return 1; }
    UN_REMOVED=0; UN_LEFT=0; UN_FAILED=0; : > "$UN_TMP/gone"
    _uv="$(tr -d '[:space:]' < "$UN_PREFIX/VERSION" 2>/dev/null)"
    if [ "$UN_DRY" = 1 ]; then un_say "Would remove Liu ${_uv} from $UN_PREFIX:"; else un_say "Removing Liu ${_uv} from $UN_PREFIX:"; fi
    if [ -f "$UN_PREFIX/$MF_NAME" ]; then
        if ! head -n 1 "$UN_PREFIX/$MF_NAME" | grep -q '^# liu install manifest 1 '; then
            un_say "Not removing $UN_PREFIX: $MF_NAME is in a format this script does not know."; un_cleanup; return 2
        fi
        if [ "$(awk -F'\t' '$1 == "prefix" { print $2; exit }' "$UN_PREFIX/$MF_NAME")" != "$UN_PREFIX" ]; then
            un_say "Not removing $UN_PREFIX: its manifest was written for another location (was it moved?)."; un_cleanup; return 2
        fi
        un_by_manifest
    else
        un_say "  (no $MF_NAME: an install from before it existed, or one the updater rewrote; removing by the known layout)"
        un_legacy_outside
        un_layout
        un_rmdir "$UN_PREFIX" quiet
    fi
    un_cleanup
    if [ -d "$UN_PREFIX" ] && [ "$UN_DRY" != 1 ] && [ -n "$(ls -A "$UN_PREFIX" 2>/dev/null)" ]; then
        un_say "  $UN_PREFIX stays: it holds files Liu did not install."
    fi
    _usum="$UN_REMOVED files"; [ "$UN_DRY" = 1 ] && _usum="$_usum would be removed" || _usum="$_usum removed"
    [ "$UN_LEFT" = 0 ] || _usum="$_usum, $UN_LEFT left alone"
    un_say "  $_usum."
    [ "$UN_FAILED" = 0 ] || return 1
    return 0
}

uninstall_main() {
    set +e
    _ufound=0; _ubad=0
    if [ -n "${LIU_PREFIX:-}" ]; then
        _uwhere="$LIU_PREFIX"
        _ufound=1
        un_prefix "$LIU_PREFIX" || _ubad=1
    else
        _uwhere="/opt/liu or ${HOME:-~}/.local/liu"
        for _uc in /opt/liu "${HOME:-/nonexistent}/.local/liu"; do
            un_prefix "$_uc"; _urc=$?
            case "$_urc" in 0) _ufound=1 ;; 3) ;; *) _ufound=1; _ubad=1 ;; esac
        done
    fi
    if [ "$_ufound" = 0 ]; then
        un_say "Liu is not installed here (looked in $_uwhere); nothing to remove."
        un_say "Another prefix: LIU_PREFIX=<dir> install.sh --uninstall"
        return 0
    fi
    if [ "$_ubad" = 0 ]; then
        if [ "$UN_DRY" = 1 ]; then
            un_say "Dry run: nothing was changed."
        else
            un_say "Done. Your settings and data (~/.config/Liu, ~/.local/share/liu) were not touched."
            if command -v pgrep >/dev/null 2>&1 && pgrep -x Liu >/dev/null 2>&1; then un_say "A running Liu keeps running until you quit it."; fi
        fi
    fi
    return "$_ubad"
}

# --- command line ------------------------------------------------------------
UNINSTALL=0; TARGET=""
for arg in "$@"; do
    case "$arg" in
        -h|--help) usage; exit 0 ;;
        --uninstall) UNINSTALL=1 ;;
        --dry-run|-n) UN_DRY=1 ;;
        -*) err "Unknown option: $arg (see --help)" ;;
        *) [ -z "$TARGET" ] || err "Unexpected argument: $arg (see --help)"; TARGET="$arg" ;;
    esac
done
[ "$UN_DRY" = 0 ] || [ "$UNINSTALL" = 1 ] || err "--dry-run goes with --uninstall."
if [ "$UNINSTALL" = 1 ]; then
    [ -z "$TARGET" ] || err "--uninstall takes no version."
    [ "$(uname -s)" = "Linux" ] \
        || err "--uninstall removes what this script installs on Linux. On macOS, drag Liu.app (in /Applications or ~/Applications) to the Trash."
    uninstall_main
    exit $?
fi
if [ -n "$TARGET" ] && ! printf '%s' "$TARGET" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+([-+][^[:space:]]+)?$'; then
    err "Invalid version format: $TARGET (expected X.Y.Z)"
fi

# --- platform detection ------------------------------------------------------
# OS token: Darwin -> macos, Linux -> linux. CPU token: arm64 / x86_64.
# PLATFORM = "<os>-<cpu>" (e.g. macos-arm64, linux-x86_64). The feed-match rule
# below also accepts the legacy bare CPU token on macOS for back-compat.
command -v curl >/dev/null 2>&1 || err "curl is required but not installed."

OS_RAW="$(uname -s)"
case "$OS_RAW" in
    Darwin) OS="macos" ;;
    Linux)  OS="linux" ;;
    *) err "Unsupported OS: $OS_RAW (Liu ships for macOS and Linux)." ;;
esac

case "$(uname -m)" in
    arm64|aarch64) CPU="arm64" ;;
    x86_64|amd64)  CPU="x86_64" ;;
    *) err "Unsupported architecture: $(uname -m)" ;;
esac

PLATFORM="$OS-$CPU"

# Each artifact is identified by its os and arch. Feed entries may store os
# and arch as separate keys or include the OS in arch. When os is omitted,
# interpret the entry as macOS, matching updater.c; bare architecture names
# are therefore not served to Linux hosts.

# The feed format uses one key per line and one artifact object per array
# element, so portable awk/sed helpers can read it.

# first "key": "value" (string) in the given blob
feed_str() {
    printf '%s' "$1" | grep -o "\"$2\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" | head -1 \
        | sed -E "s/.*:[[:space:]]*\"([^\"]*)\"/\1/"
}
# first "key": <number> in the given blob
feed_num() {
    printf '%s' "$1" | grep -o "\"$2\"[[:space:]]*:[[:space:]]*[0-9][0-9]*" | head -1 \
        | sed -E "s/.*:[[:space:]]*([0-9]+)/\1/"
}
# the whole version entry for VER (from its "version" line up to — but not
# including — the next "version" line). Used to scope minOS + yanked correctly
# even when the feed carries multiple versions.
version_block() {
    printf '%s' "$1" | awk -v V="\"$2\"" '
        /"version"[[:space:]]*:/ {
            if (active) exit            # reached the next version entry — stop
            active = (index($0, V) > 0)
        }
        active { print }
    '
}
# The artifact object matching (os, arch), scoped to one version entry.
#
# Buffers each `{ … }` object whole and emits it only once BOTH fields have been
# read, because "os" is written before "arch" and a scanner that starts printing
# at the "arch" line has already walked past the field it needs to test. Fields
# are compared with their quotes on, so a bare "arm64" request can never
# substring-match "macos-arm64"/"linux-arm64".
#
# Two accepted spellings, per the note above: os+arch as separate keys, or the
# os folded into the arch token. An object with no "os" key defaults to macos.
artifact_block() {
    printf '%s' "$1" | awk -v WOS="\"$2\"" -v WARCH="\"$3\"" -v WQUAL="\"$4\"" '
        function field(line,   s) {
            if (!match(line, /:[[:space:]]*"[^"]*"/)) return ""
            s = substr(line, RSTART, RLENGTH); sub(/^:[[:space:]]*/, "", s); return s
        }
        /^[[:space:]]*\{[[:space:]]*$/ { n = 0; os = "\"macos\""; arch = ""; inobj = 1 }
        inobj { buf[n++] = $0 }
        inobj && /"os"[[:space:]]*:/   { v = field($0); if (v != "") os   = v }
        inobj && /"arch"[[:space:]]*:/ { v = field($0); if (v != "") arch = v }
        inobj && /^[[:space:]]*\}/ {
            if ((arch == WARCH && os == WOS) || arch == WQUAL) {
                for (i = 0; i < n; i++) print buf[i]
                exit
            }
            inobj = 0
        }
    '
}

# --- fetch + parse the feed --------------------------------------------------
note "Fetching the Liu release feed…"
FEED="$(curl -fsSL --proto '=https' --proto-redir '=https' \
        --max-time 20 --max-filesize "$FEED_CAP" "$FEED_URL")" \
    || err "couldn't reach the update feed ($FEED_URL). No release yet?"

case "$FEED_URL" in https://*) ;; *) err "refusing non-https feed url: $FEED_URL" ;; esac

VER="$TARGET"
[ -n "$VER" ] || VER="$(feed_str "$FEED" latest)"
[ -n "$VER" ] || err "couldn't determine the latest version from the feed."

VBLOCK="$(version_block "$FEED" "$VER")"
[ -n "$VBLOCK" ] || err "version $VER is not present in the feed."

# --- yanked-version check (mirrors updater.c:186) ----------------------------
if printf '%s' "$VBLOCK" | grep -Eq '"yanked"[[:space:]]*:[[:space:]]*true'; then
    err "Liu $VER has been yanked by the publisher — refusing to install."
fi

# --- architecture / platform match -------------------------------------------
BLOCK="$(artifact_block "$VBLOCK" "$OS" "$CPU" "$PLATFORM")"
WANT="$PLATFORM"
if [ -z "$BLOCK" ]; then
    if [ "$OS" = "linux" ]; then
        err "no $PLATFORM build for Liu $VER in the feed (a Linux 'linux-<cpu>' tarball is required; producer follow-up: ship scripts/package_linux_tarball.sh output)."
    else
        err "no $PLATFORM (or legacy $CPU) build for Liu $VER in the feed."
    fi
fi

URL="$(feed_str "$BLOCK" url)"
SHA="$(feed_str "$BLOCK" sha256)"
SIG="$(feed_str "$BLOCK" ed25519)"
SZ="$(feed_num "$BLOCK" size)"
[ -n "$SZ" ] || SZ=0
# minimumSystemVersion lives on the version entry, not per-artifact.
MIN_OS="$(feed_str "$VBLOCK" minimumSystemVersion)"

# --- artifact field presence + types (mirrors updater.c:204-208) -------------
# url, sha256 and ed25519 must all be present. ed25519 is REQUIRED in the feed
# even though a host without a capable verifier may (under explicit opt-out) skip
# the actual check — a feed that omits the signature entirely is rejected.
[ -n "$URL" ] || err "feed is missing the $WANT artifact url."
[ -n "$SHA" ] || err "feed is missing the $WANT artifact sha256."
[ -n "$SIG" ] || err "feed is missing the $WANT artifact ed25519 signature."

# --- https-only artifact url (mirrors updater.c:213) -------------------------
case "$URL" in https://*) ;; *) err "refusing non-https artifact url: $URL" ;; esac

# --- artifact host pinned to the feed host (mirrors updater.c url_host_matches_feed)
# The feed is TLS-only and unsigned, so a tampered feed must not be able to aim
# the download at an arbitrary host (blind forced-request / tracking beacon).
# Ed25519 gates code execution either way; this stops the connection itself.
# "Host" = the full authority segment between "https://" and the first "/",
# compared exactly (userinfo/port tricks like https://liu.software@evil.com or
# :8443 change the segment and are rejected).
host_of() { printf '%s' "$1" | sed -E 's#^https://([^/]*).*#\1#' | tr 'A-Z' 'a-z'; }
FEED_HOST="$(host_of "$FEED_URL")"
URL_HOST="$(host_of "$URL")"
[ -n "$FEED_HOST" ] && [ "$URL_HOST" = "$FEED_HOST" ] \
    || err "refusing artifact host '$URL_HOST' — the feed host is '$FEED_HOST'."

# --- minimum-OS gate (macOS only; mirrors updater.c:188-203) -----------------
# minimumSystemVersion is macOS-specific. On Linux it is effectively ignored
# (a glibc/abi gate would be the right Linux equivalent — producer follow-up).
if [ "$OS" = "macos" ] && [ -n "$MIN_OS" ]; then
    CUR_OS="$(sw_vers -productVersion 2>/dev/null || echo 0)"
    lowest="$(printf '%s\n%s\n' "$MIN_OS" "$CUR_OS" | sort -V | head -1)"
    [ "$lowest" = "$MIN_OS" ] || err "Liu $VER needs macOS $MIN_OS (you have $CUR_OS)."
fi

note "Installing Liu $VER ($WANT)…"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/liu-install.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
# mktemp -d yields a 0700, unpredictable dir on macOS and Linux, matching the
# app's mkdtemp(0700) defense against pre-planted-symlink attacks in shared temp.

# --- size cap (identical formula to the app; updater_macos.m:157-158) --------
# cap = feed-declared size + 5% slack + 64 KiB, clamped to the 1 GiB hard ceiling.
# With no feed size, cap = 1 GiB.
CAP="$HARD_CAP"
if [ "$SZ" -gt 0 ] 2>/dev/null; then
    FC=$((SZ + SZ / 20 + 65536))
    [ "$FC" -lt "$CAP" ] && CAP=$FC
fi

# Artifact filename: .zip on macOS, .tar.gz on Linux. Keep the URL's basename
# extension so the right extractor is chosen below.
case "$URL" in
    *.tar.gz|*.tgz) ART="$TMP/liu-artifact.tar.gz" ;;
    *.zip)          ART="$TMP/liu-artifact.zip" ;;
    *) # fall back by OS rather than guessing
       [ "$OS" = "linux" ] && ART="$TMP/liu-artifact.tar.gz" || ART="$TMP/liu-artifact.zip" ;;
esac

note "  Downloading…"
curl -fL --proto '=https' --proto-redir '=https' \
    --max-time 600 --max-filesize "$CAP" -o "$ART" "$URL" \
    || err "download failed from $URL (or it exceeded the $CAP-byte size cap)."

# --- defense-in-depth: on-disk size re-check (mirrors updater.c:241-250) ------
ART_SZ="$(wc -c < "$ART" | tr -d ' ')"
[ -n "$ART_SZ" ] || err "couldn't stat the downloaded artifact."
[ "$ART_SZ" -le "$CAP" ] || err "downloaded artifact is unexpectedly large ($ART_SZ > $CAP) — aborting."

# --- integrity: SHA-256 (MANDATORY) ------------------------------------------
# macOS ships `shasum`; Linux usually ships `sha256sum` (and sometimes `shasum`).
if command -v sha256sum >/dev/null 2>&1; then
    GOT_SHA="$(sha256sum "$ART" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
    GOT_SHA="$(shasum -a 256 "$ART" | awk '{print $1}')"
else
    err "no sha256 tool found (need sha256sum or shasum)."
fi
# Case-insensitive compare (the app uses strcasecmp; gen_feed.py emits lowercase).
GOT_LC="$(printf '%s' "$GOT_SHA" | tr 'A-Z' 'a-z')"
WANT_LC="$(printf '%s' "$SHA"   | tr 'A-Z' 'a-z')"
[ "$GOT_LC" = "$WANT_LC" ] || err "SHA-256 mismatch — refusing to install (got $GOT_SHA, want $SHA)."
note "  SHA-256 OK."

# --- authenticity: Ed25519 (MANDATORY where a capable openssl exists) --------
# `openssl pkeyutl -verify -rawin` is the only portable CLI that does raw-message
# Ed25519. It works with real OpenSSL 3.x (Linux distro openssl; Homebrew
# openssl@3 at /opt/homebrew/bin or /usr/local/bin). The stock macOS
# /usr/bin/openssl is LibreSSL and CANNOT do this. Each candidate is probed by
# test-loading the PEM with `pkey -pubin -noout` (LibreSSL passes this for some
# keys, so pkeyutl -rawin must additionally be honoured below).
OSSL=""
for c in openssl /opt/homebrew/bin/openssl /usr/local/bin/openssl /usr/bin/openssl; do
    command -v "$c" >/dev/null 2>&1 || [ -x "$c" ] || continue
    if printf '%s\n' "$LIU_PUBKEY_PEM" | "$c" pkey -pubin -noout >/dev/null 2>&1; then
        OSSL="$c"; break
    fi
done

ED_DONE=0
if [ -n "$OSSL" ]; then
    printf '%s\n' "$LIU_PUBKEY_PEM" > "$TMP/pub.pem"
    printf '%s' "$SIG" | "$OSSL" base64 -d -A > "$TMP/sig.bin" 2>/dev/null \
        || err "couldn't base64-decode the Ed25519 signature from the feed."
    # The raw Ed25519 signature must be exactly 64 bytes (mirrors updater.c:261).
    SIG_LEN="$(wc -c < "$TMP/sig.bin" | tr -d ' ')"
    [ "$SIG_LEN" = "64" ] || err "Ed25519 signature is $SIG_LEN bytes, expected 64 — aborting."

    if "$OSSL" pkeyutl -verify -pubin -inkey "$TMP/pub.pem" -rawin \
            -sigfile "$TMP/sig.bin" -in "$ART" >/dev/null 2>&1; then
        note "  Ed25519 signature OK."
        ED_DONE=1
    else
        # A capable openssl that REFUSES the signature is a hard failure — this is
        # the real trust anchor and it is fail-closed (mirrors updater.c:261-263).
        # (If this openssl turned out to be LibreSSL pretending to load the key but
        #  unable to do -rawin, it errors here too; the script then falls through to
        #  the no-capable-verifier policy below rather than installing silently.)
        if "$OSSL" pkeyutl -verify -pubin -inkey "$TMP/pub.pem" -rawin \
                -sigfile "$TMP/sig.bin" -in "$ART" 2>&1 | grep -qi 'unsupported\|rawin\|unknown option\|invalid'; then
            note "  (this openssl can't do raw Ed25519 — treating as no capable verifier)"
            OSSL=""
        else
            err "Ed25519 signature verification FAILED — aborting."
        fi
    fi
fi

if [ "$ED_DONE" != "1" ]; then
    # No capable Ed25519 verifier on this host. FAIL CLOSED by default.
    if [ "${LIU_INSECURE_SKIP_SIG:-0}" = "1" ]; then
        warn "================================================================"
        warn " LIU_INSECURE_SKIP_SIG=1 — installing WITHOUT Ed25519 verification."
        warn " The artifact's authenticity is NOT being checked. You are trusting"
        warn " whoever served the feed over TLS, which is exactly the risk Ed25519"
        warn " exists to remove. Proceed only if you understand this."
        warn "================================================================"
    else
        if [ "$OS" = "macos" ]; then
            err "No Ed25519-capable OpenSSL found (stock macOS ships LibreSSL, which can't verify Ed25519). Install one with: brew install openssl@3 — then re-run. To install anyway WITHOUT signature verification (UNSAFE), set LIU_INSECURE_SKIP_SIG=1."
        else
            err "No Ed25519-capable OpenSSL found. Install OpenSSL 3.x (e.g. apt install openssl / dnf install openssl) and re-run. To install anyway WITHOUT signature verification (UNSAFE), set LIU_INSECURE_SKIP_SIG=1."
        fi
    fi
fi

# ============================================================================
# Platform-specific extract + install
# ============================================================================
if [ "$OS" = "macos" ]; then
    # ----- macOS: .app -> /Applications (atomic staged swap, rollback) -------
    note "  Extracting…"
    ditto -x -k "$ART" "$TMP/x" || err "couldn't extract the archive."
    APP_SRC="$(/usr/bin/find "$TMP/x" -maxdepth 1 -name '*.app' -type d | head -1)"
    [ -n "$APP_SRC" ] || err "no .app found inside the archive."

    # An explicitly-set LIU_APP_DIR is honoured (created if needed) — never
    # silently redirected. Only the DEFAULT (/Applications, which usually needs
    # admin) falls back to ~/Applications.
    if [ -n "${LIU_APP_DIR:-}" ]; then
        APP_DIR="$LIU_APP_DIR"
        mkdir -p "$APP_DIR" 2>/dev/null || true
        { [ -d "$APP_DIR" ] && [ -w "$APP_DIR" ]; } || err "LIU_APP_DIR ($APP_DIR) is not a writable directory."
    else
        APP_DIR="/Applications"
        if [ ! -d "$APP_DIR" ] || [ ! -w "$APP_DIR" ]; then
            APP_DIR="$HOME/Applications"; mkdir -p "$APP_DIR"
        fi
    fi
    DEST="$APP_DIR/Liu.app"

    # --- signed-downgrade guard (mirrors updater.c:274-280) ------------------
    # If an install already exists, refuse a non-strictly-newer build. A fresh
    # install has no "current" and is always allowed.
    if [ -e "$DEST" ]; then
        CUR_V=""
        if [ -f "$DEST/Contents/Info.plist" ]; then
            CUR_V="$(/usr/bin/defaults read "$DEST/Contents/Info.plist" CFBundleShortVersionString 2>/dev/null || true)"
            [ -n "$CUR_V" ] || CUR_V="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$DEST/Contents/Info.plist" 2>/dev/null || true)"
        fi
        NEW_V=""
        if [ -f "$APP_SRC/Contents/Info.plist" ]; then
            NEW_V="$(/usr/bin/defaults read "$APP_SRC/Contents/Info.plist" CFBundleShortVersionString 2>/dev/null || true)"
            [ -n "$NEW_V" ] || NEW_V="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP_SRC/Contents/Info.plist" 2>/dev/null || true)"
        fi
        if [ -n "$CUR_V" ] && [ -n "$NEW_V" ]; then
            # strictly-newer test via sort -V (adequate for dotted numeric
            # versions; prerelease precedence is not perfectly modelled but a
            # downgrade across release numbers is what matters here).
            if [ "$CUR_V" = "$NEW_V" ]; then
                note "  Liu $CUR_V is already installed — reinstalling the same version."
            else
                newest="$(printf '%s\n%s\n' "$CUR_V" "$NEW_V" | sort -V | tail -1)"
                [ "$newest" = "$NEW_V" ] || err "refusing downgrade: installed $CUR_V is newer than $NEW_V."
            fi
        fi
    fi

    # Releases are Developer ID signed, notarized and stapled, so
    # Gatekeeper clears the app on its own — no quarantine stripping needed.

    # Stage in an unpredictable, private same-volume dir, then swap (ditto
    # preserves the signature/xattrs and handles the cross-volume copy from
    # $TMPDIR; the swap is two renames). Using mktemp rather than a predictable
    # "$DEST.new-$$" closes a TOCTOU on a group/world-writable install prefix
    # (e.g. a shared /opt), where a local attacker could otherwise pre-create or
    # swap the staging path between the rm and the mv.
    STAGEDIR="$(mktemp -d "$(dirname "$DEST")/.liu-stage.XXXXXX")" \
        || err "couldn't create a staging dir in $APP_DIR."
    STAGED="$STAGEDIR/$(basename "$DEST")"
    ditto "$APP_SRC" "$STAGED" || { rm -rf "$STAGEDIR"; err "couldn't stage the new app."; }
    OLD=""
    if [ -e "$DEST" ]; then
        OLD="$STAGEDIR/old"
        mv "$DEST" "$OLD" || { rm -rf "$STAGEDIR"; err "couldn't move aside the existing app."; }
    fi
    if ! mv "$STAGED" "$DEST"; then
        [ -n "$OLD" ] && mv "$OLD" "$DEST"   # rollback
        rm -rf "$STAGEDIR"
        err "couldn't move Liu.app into $APP_DIR."
    fi
    rm -rf "$STAGEDIR"

    note ""
    note "Liu $VER installed to $DEST"
    if [ "${LIU_NO_OPEN:-0}" != "1" ]; then
        note "Launching Liu…"
        open "$DEST" 2>/dev/null || note "  (open it from $APP_DIR or Spotlight)"
    else
        note "  Open it from $APP_DIR or Spotlight."
    fi

else
    # ----- Linux: OpenGL single binary + assets -> prefix + PATH + .desktop --
    # The Linux artifact is a signed gzipped tarball containing a top-level Liu/
    # tree: the Liu ELF, the liu-notify helper (must sit beside Liu so the notify
    # hooks resolve <dir-of-Liu>/liu-notify), liu-history, and the assets/ tree
    # (fonts are MANDATORY at runtime — JetBrainsMonoNerdFontMono-Regular.ttf is resolved via
    # <exe_dir>/../assets/fonts). No .metallib (Metal is macOS-only); the GL
    # shaders are embedded C strings in the binary.
    command -v tar >/dev/null 2>&1 || err "tar is required to install on Linux."

    note "  Extracting…"
    mkdir -p "$TMP/x"
    tar -xzf "$ART" -C "$TMP/x" || err "couldn't extract the tarball."

    # Locate the Liu binary inside the extracted tree (be liberal about the
    # top-level dir name the producer chooses).
    LIU_BIN="$(/usr/bin/find "$TMP/x" -maxdepth 3 -type f -name Liu -perm -u+x 2>/dev/null | head -1)"
    [ -n "$LIU_BIN" ] || LIU_BIN="$(/usr/bin/find "$TMP/x" -maxdepth 3 -type f -name Liu 2>/dev/null | head -1)"
    [ -n "$LIU_BIN" ] || err "no 'Liu' binary found inside the tarball."
    SRC_ROOT="$(dirname "$LIU_BIN")"
    [ -d "$SRC_ROOT/assets/fonts" ] || warn "tarball has no assets/fonts/ next to the binary — Liu may fail to start (the default font is mandatory)."

    # --- choose a self-contained prefix --------------------------------------
    # /opt/liu is cleanest because the exe-relative font lookup (<exe_dir>/../
    # assets/fonts) is satisfied with zero env vars: /opt/liu/bin/Liu finds
    # /opt/liu/assets/fonts. Fall back to ~/.local/liu when /opt isn't writable.
    if [ -n "${LIU_PREFIX:-}" ]; then
        PREFIX="$LIU_PREFIX"
    elif [ -w /opt ] 2>/dev/null || ( mkdir -p /opt/liu 2>/dev/null && [ -w /opt/liu ] ); then
        PREFIX="/opt/liu"
    else
        PREFIX="$HOME/.local/liu"
    fi
    # What gets written is recorded as it is (see "install manifest" above), for --uninstall.
    MF="$TMP/install-manifest.records"; : > "$MF"
    mf_mkdir_p "$PREFIX" 2>/dev/null || err "can't create install prefix $PREFIX."
    [ -w "$PREFIX" ] || err "install prefix $PREFIX is not writable."
    # The swap below replaces an existing prefix whole: only a Liu install may be.
    if [ -n "$(ls -A "$PREFIX" 2>/dev/null)" ] && [ ! -f "$PREFIX/VERSION" ] && [ ! -f "$PREFIX/$MF_NAME" ]; then
        err "$PREFIX is not empty and is not a Liu install (no VERSION file), so it is not replaced. Use another LIU_PREFIX, or empty it first."
    fi

    # --- signed-downgrade guard (Linux) --------------------------------------
    # Linux has no Info.plist to read: the guard compares against the VERSION
    # marker at the prefix's root, which the install below writes.
    if [ -f "$PREFIX/VERSION" ]; then
        CUR_V="$(cat "$PREFIX/VERSION" 2>/dev/null | tr -d '[:space:]')"
        if [ -n "$CUR_V" ] && [ "$CUR_V" != "$VER" ]; then
            newest="$(printf '%s\n%s\n' "$CUR_V" "$VER" | sort -V | tail -1)"
            [ "$newest" = "$VER" ] || err "refusing downgrade: installed $CUR_V is newer than $VER."
        fi
    fi

    # --- atomic install: stage a sibling tree, then swap (rollback on fail) ---
    STAGE="$PREFIX.new-$$"
    rm -rf "$STAGE"; mkdir -p "$STAGE/bin"
    # Copy the binary + its siblings (liu-notify, liu-history if present).
    cp -p "$LIU_BIN" "$STAGE/bin/Liu" || err "couldn't stage the Liu binary."
    for helper in liu-notify liu-history; do
        if [ -f "$SRC_ROOT/$helper" ]; then
            cp -p "$SRC_ROOT/$helper" "$STAGE/bin/$helper" || true
            chmod +x "$STAGE/bin/$helper" 2>/dev/null || true
        fi
    done
    chmod +x "$STAGE/bin/Liu"
    # Copy the assets tree so <bin>/../assets/fonts resolves.
    if [ -d "$SRC_ROOT/assets" ]; then
        cp -pR "$SRC_ROOT/assets" "$STAGE/assets" || err "couldn't stage assets/."
    fi
    # share/ carries the compiled terminfo entry the app hands a pane
    # (<prefix>/share/terminfo, TERM=xterm-liu), the icons by name and the
    # desktop entry the packages use; the in-app updater keeps it across an
    # update. Then the terminfo source and a desktop icon, if present.
    if [ -d "$SRC_ROOT/share" ]; then
        cp -pR "$SRC_ROOT/share" "$STAGE/share" || err "couldn't stage share/."
    fi
    [ -f "$SRC_ROOT/liu.terminfo" ] && cp -p "$SRC_ROOT/liu.terminfo" "$STAGE/liu.terminfo" 2>/dev/null || true
    # The licence and the notices travel with the binary.
    for doc in LICENSE NOTICE THIRD_PARTY_NOTICES.md; do
        [ -f "$SRC_ROOT/$doc" ] && cp -p "$SRC_ROOT/$doc" "$STAGE/$doc" 2>/dev/null || true
    done
    ICON_SRC="$(/usr/bin/find "$SRC_ROOT" -maxdepth 2 -type f \( -name 'liu*.png' -o -name 'Liu*.png' \) 2>/dev/null | head -1)"
    if [ -n "$ICON_SRC" ]; then mkdir -p "$STAGE/share"; cp -p "$ICON_SRC" "$STAGE/share/liu.png" 2>/dev/null || true; fi
    printf '%s\n' "$VER" > "$STAGE/VERSION"

    # An earlier install's manifest outlives the swap only as its records outside the prefix.
    OLD_MF=""
    if [ -f "$PREFIX/$MF_NAME" ] && cp "$PREFIX/$MF_NAME" "$TMP/install-manifest.old" 2>/dev/null; then
        OLD_MF="$TMP/install-manifest.old"
    fi

    # Swap: move old aside, move staged into place, restore old on failure.
    OLD=""
    if [ -e "$PREFIX" ]; then OLD="$PREFIX.old-$$"; rm -rf "$OLD"; mv "$PREFIX" "$OLD"; fi
    if ! mv "$STAGE" "$PREFIX"; then
        [ -n "$OLD" ] && mv "$OLD" "$PREFIX"   # rollback
        rm -rf "$STAGE"
        err "couldn't move the new Liu tree into $PREFIX."
    fi
    [ -n "$OLD" ] && rm -rf "$OLD"

    # --- PATH symlink --------------------------------------------------------
    if [ -n "${LIU_BIN_DIR:-}" ]; then
        BINDIR="$LIU_BIN_DIR"
    elif [ -w /usr/local/bin ] 2>/dev/null; then
        BINDIR="/usr/local/bin"
    else
        BINDIR="$HOME/.local/bin"
    fi
    mf_mkdir_p "$BINDIR" 2>/dev/null || true
    if [ -w "$BINDIR" ] 2>/dev/null || [ -d "$BINDIR" ]; then
        if [ -e "$BINDIR/liu" ] && [ ! -L "$BINDIR/liu" ]; then
            warn "$BINDIR/liu exists and is not a link, so it is left alone (add $PREFIX/bin to PATH, or move it away and run this again)."
        elif ln -sf "$PREFIX/bin/Liu" "$BINDIR/liu" 2>/dev/null; then
            mf_add l "$BINDIR/liu" "$PREFIX/bin/Liu" || true
            note "  Symlinked $BINDIR/liu -> $PREFIX/bin/Liu"
        else
            warn "couldn't create the $BINDIR/liu symlink (add $PREFIX/bin to PATH manually)."
        fi
        case ":$PATH:" in
            *":$BINDIR:"*) ;;
            *) warn "$BINDIR is not on your PATH — add it to use the 'liu' command." ;;
        esac
    fi

    # --- terminfo (TERM=xterm-liu, an opt-in) --------------------------------
    # The entry is "xterm-liu" (liu.terminfo says what it adds to
    # xterm-256color). A pane uses it only when the user's config asks
    # ("term": "xterm-liu"); installing it here makes that name resolve.
    # Best-effort: tic may be absent on a minimal box.
    if [ -f "$PREFIX/liu.terminfo" ] && command -v tic >/dev/null 2>&1; then
        # tic decides where the entry goes ($TERMINFO, ~/.terminfo or the system's);
        # what it wrote is found by looking at those places before and after.
        ti_state > "$TMP/terminfo.before" 2>/dev/null || true
        if tic -x "$PREFIX/liu.terminfo" >/dev/null 2>&1; then
            ti_record "$TMP/terminfo.before" || true
            note "  Installed the Liu terminfo entry (xterm-liu)."
        else
            warn "couldn't compile liu.terminfo with tic (TERM=xterm-liu will not resolve)."
        fi
    fi

    # --- icons, by name -------------------------------------------------------
    # The hicolor PNGs go where the icon theme looks (an XDG data dir), so the
    # entry below can say Icon=liu and the app's banners (notify-send
    # --icon=liu) find the same icon. Same place as the .desktop entry.
    if [ -n "${LIU_DATA_DIR:-}" ]; then
        DATADIR="$LIU_DATA_DIR"
    elif [ -w /usr/share/applications ] 2>/dev/null; then
        DATADIR="/usr/share"
    else
        DATADIR="$HOME/.local/share"
    fi
    ICON_NAMED=0
    if [ -d "$PREFIX/share/icons/hicolor" ]; then
        mf_mkdir_p "$DATADIR/icons/hicolor" 2>/dev/null || true
        # the theme's folders the copy below has to create, and the files it writes
        ( cd "$PREFIX/share/icons/hicolor" && find . -mindepth 1 -type d | LC_ALL=C sort ) > "$TMP/icons.dirs"
        ( cd "$PREFIX/share/icons/hicolor" && find . -type f | LC_ALL=C sort ) > "$TMP/icons.files"
        : > "$TMP/icons.newdirs"
        while IFS= read -r _i; do
            [ -e "$DATADIR/icons/hicolor/${_i#./}" ] || [ -L "$DATADIR/icons/hicolor/${_i#./}" ] || printf '%s\n' "${_i#./}" >> "$TMP/icons.newdirs"
        done < "$TMP/icons.dirs"
        CACHE_BEFORE=0; [ -e "$DATADIR/icons/hicolor/icon-theme.cache" ] && CACHE_BEFORE=1
        if cp -pR "$PREFIX/share/icons/hicolor/." "$DATADIR/icons/hicolor/" 2>/dev/null; then
            ICON_NAMED=1
            while IFS= read -r _i; do mf_add d "$DATADIR/icons/hicolor/$_i" || true; done < "$TMP/icons.newdirs"
            while IFS= read -r _i; do mf_file "$DATADIR/icons/hicolor/${_i#./}" || true; done < "$TMP/icons.files"
            # A theme directory with an index keeps a cache; refresh it.
            if [ -f "$DATADIR/icons/hicolor/index.theme" ] && command -v gtk-update-icon-cache >/dev/null 2>&1; then
                gtk-update-icon-cache -q "$DATADIR/icons/hicolor" 2>/dev/null || true
                mf_add cache "$DATADIR/icons/hicolor" || true
                [ "$CACHE_BEFORE" = 1 ] || mf_file "$DATADIR/icons/hicolor/icon-theme.cache" || true
            fi
            note "  Installed the icons under $DATADIR/icons/hicolor"
        else
            warn "couldn't install the icons under $DATADIR/icons/hicolor."
        fi
    fi

    # --- freedesktop .desktop entry ------------------------------------------
    # Liu answers --version and --help and opens no file or folder it is
    # handed, so Exec is the bare command with no field codes: a %F/%U would
    # pass paths the app does not read.
    APPDIR="$DATADIR/applications"
    mf_mkdir_p "$APPDIR" 2>/dev/null || true
    ICON_LINE="Icon=liu"
    if [ "$ICON_NAMED" != "1" ]; then
        ICON_LINE="Icon=$PREFIX/share/liu.png"
        [ -f "$PREFIX/share/liu.png" ] || ICON_LINE="Icon=utilities-terminal"
    fi
    if [ -d "$APPDIR" ] && [ -w "$APPDIR" ]; then
        {
            printf '%s\n' '[Desktop Entry]'
            printf '%s\n' 'Type=Application'
            printf '%s\n' 'Name=Liu'
            printf '%s\n' 'GenericName=Terminal'
            printf '%s\n' 'Comment=AI-assisted terminal'
            printf '%s\n' "Exec=$PREFIX/bin/Liu"
            printf '%s\n' "$ICON_LINE"
            printf '%s\n' 'Terminal=false'
            printf '%s\n' 'Categories=System;TerminalEmulator;'
            printf '%s\n' 'StartupNotify=true'
            # Ties the running window (WM_CLASS res_name "liu") to this entry.
            # Without it a shell has to guess, and the taskbar count badge —
            # addressed as application://liu.desktop — lands on nothing.
            printf '%s\n' 'StartupWMClass=liu'
        } > "$APPDIR/liu.desktop" 2>/dev/null \
            && { mf_file "$APPDIR/liu.desktop" || true; note "  Wrote $APPDIR/liu.desktop"; } \
            || warn "couldn't write the .desktop entry to $APPDIR."
    fi

    # --- the manifest ----------------------------------------------------------
    # Written last, into the new prefix: the tree, what was recorded above, and the
    # records of an earlier install that this one did not write again.
    if mf_finish "$PREFIX" "$VER" "$OLD_MF" "$MF" "$TMP/install-manifest" 2>/dev/null \
            && cp "$TMP/install-manifest" "$PREFIX/$MF_NAME" 2>/dev/null; then
        chmod 644 "$PREFIX/$MF_NAME" 2>/dev/null || true
    else
        warn "couldn't write $PREFIX/$MF_NAME: 'install.sh --uninstall' will remove this install by its known layout."
    fi

    # --- post-install sanity check (headless) ----------------------------------
    # ldd names any library the loader cannot find; then `Liu --version`, which
    # the app answers and exits before it opens a display, and the liu-history
    # CLI. Started bare, Liu would open an X11 window (or exit 1 with no
    # $DISPLAY), so nothing here starts it without a flag it answers.
    if [ "${LIU_NO_OPEN:-0}" != "1" ]; then
        if command -v ldd >/dev/null 2>&1; then
            if ldd "$PREFIX/bin/Liu" 2>/dev/null | grep -qi 'not found'; then
                warn "ldd reports missing shared libraries for Liu:"
                ldd "$PREFIX/bin/Liu" 2>/dev/null | grep -i 'not found' >&2 || true
                warn "Liu links libssh2, OpenSSL 3's libcrypto, libX11, libXrandr, libXi, libOpenGL + libGLX, fontconfig, FreeType, HarfBuzz and D-Bus."
                warn "  Debian/Ubuntu: sudo apt install libssh2-1 libssl3 libx11-6 libxrandr2 libxi6 libopengl0 libglx0 libfontconfig1 libfreetype6 libharfbuzz0b libdbus-1-3"
                warn "  Fedora:        sudo dnf install libssh2 openssl-libs libX11 libXrandr libXi libglvnd-opengl libglvnd-glx fontconfig freetype harfbuzz dbus-libs"
                warn "  (the release's .deb and .rpm install these themselves)"
            else
                note "  Shared-library check OK (ldd)."
            fi
        fi
        if "$PREFIX/bin/Liu" --version >/dev/null 2>&1; then
            note "  $("$PREFIX/bin/Liu" --version 2>/dev/null)"
        fi
        if [ -x "$PREFIX/bin/liu-history" ]; then
            "$PREFIX/bin/liu-history" --help >/dev/null 2>&1 \
                && note "  liu-history CLI responds OK." || true
        fi
    fi

    note ""
    note "Liu $VER installed to $PREFIX"
    note "  Binary: $PREFIX/bin/Liu   (run 'liu' if $BINDIR is on PATH)"
    note "  Liu is an X11 GUI app and needs a running display (\$DISPLAY)."
    note "  Optional niceties: notify-send, xdg-open, espeak-ng, and the fonts-dejavu / fonts-noto packages for wider glyph coverage."
fi
