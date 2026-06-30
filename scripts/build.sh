#!/usr/bin/env bash
#
# liu — interactive build TUI.
#
# Scans the host (OS, CPU arch, toolchain, GPU backend, dependencies), picks the
# right build configuration automatically, lets you confirm or tweak it, then
# runs the build with live output. Works on macOS and Linux (bash).
#
# Usage:
#   scripts/build.sh            # interactive TUI
#   scripts/build.sh --yes      # non-interactive: build with the recommended config
#   scripts/build.sh --print    # just print the system scan + recommended config
#   scripts/build.sh --release  # force Release (default is auto: Release)
#   scripts/build.sh --debug    # force Debug
#   scripts/build.sh --native   # tune for this CPU (-DLIU_NATIVE_ARCH=ON)
#   scripts/build.sh --package  # build + package the release artifact for this
#                               # OS (macOS: .dmg, Linux: .tar.gz); forces Release
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---- pretty output (no-ops when not a TTY) ---------------------------------
if [[ -t 1 ]]; then
    B=$'\033[1m'; DIM=$'\033[2m'; R=$'\033[0m'
    GRN=$'\033[32m'; YEL=$'\033[33m'; RED=$'\033[31m'; CYN=$'\033[36m'; BLU=$'\033[34m'
else
    B=''; DIM=''; R=''; GRN=''; YEL=''; RED=''; CYN=''; BLU=''
fi
ok()   { printf '  %s✓%s %s\n' "$GRN" "$R" "$*"; }
miss() { printf '  %s•%s %s\n' "$YEL" "$R" "$*"; }
bad()  { printf '  %s✗%s %s\n' "$RED" "$R" "$*"; }
hr()   { printf '%s────────────────────────────────────────────────────────%s\n' "$DIM" "$R"; }
have() { command -v "$1" >/dev/null 2>&1; }

# ---- argument parsing ------------------------------------------------------
MODE="tui"; FORCE_TYPE=""; FORCE_NATIVE=""; DO_PACKAGE="OFF"
for a in "$@"; do
    case "$a" in
        --yes|-y)  MODE="yes" ;;
        --print)   MODE="print" ;;
        --release) FORCE_TYPE="Release" ;;
        --debug)   FORCE_TYPE="Debug" ;;
        --native)  FORCE_NATIVE="ON" ;;
        --package) DO_PACKAGE="ON"; [[ "$MODE" == "tui" ]] && MODE="yes" ;;
        -h|--help) sed -n '3,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $a (see --help)" >&2; exit 2 ;;
    esac
done

# ===========================================================================
# 1. Scan the host
# ===========================================================================
OS="$(uname -s)"; ARCH="$(uname -m)"
case "$OS" in
    Darwin) PLATFORM="macOS" ;;
    Linux)  PLATFORM="Linux" ;;
    *)      PLATFORM="$OS" ;;
esac
case "$ARCH" in
    arm64|aarch64) ARCH_N="arm64" ;;
    x86_64|amd64)  ARCH_N="x86_64" ;;
    i?86|armv7*|armhf)
        echo "Unsupported CPU: ${ARCH} — Liu requires a 64-bit CPU (arm64 or x86_64)." >&2
        exit 2 ;;
    *)             ARCH_N="$ARCH" ;;
esac
# SIMD story per target: hand-written ARM64 NEON (Apple Silicon) and x86-64
# SSE2 (any x86_64 with clang/gcc); everything else runs the portable C.
if [[ "$ARCH_N" == "arm64" && "$PLATFORM" == "macOS" ]]; then
    SIMD="ARM64 NEON asm (hand-written hot paths)"
elif [[ "$ARCH_N" == "x86_64" ]]; then
    SIMD="x86-64 SSE2 asm (hand-written hot paths)"
else
    SIMD="portable C fallbacks (no hand-written asm for ${ARCH_N})"
fi
if have nproc; then NCPU="$(nproc)"; elif have sysctl; then NCPU="$(sysctl -n hw.ncpu)"; else NCPU=4; fi

CMAKE_VER="$(have cmake && cmake --version | head -1 | awk '{print $3}' || echo '')"
CC_BIN="${CC:-$(have cc && echo cc || (have clang && echo clang) || (have gcc && echo gcc) || echo '')}"
CC_VER="$([[ -n "$CC_BIN" ]] && "$CC_BIN" --version 2>/dev/null | head -1 || echo '')"

# GPU backend: Metal on macOS when the metal toolchain is present, else OpenGL.
GPU="OpenGL"; USE_METAL="OFF"
if [[ "$PLATFORM" == "macOS" ]] && xcrun -sdk macosx -find metal >/dev/null 2>&1; then
    GPU="Metal"; USE_METAL="ON"
fi

# Dependency probes (advisory — CMake does the authoritative find).
dep_status() { # name  pkgconfig-mod  header-or-cmd
    local label="$1" mod="$2"
    if have pkg-config && pkg-config --exists "$mod" 2>/dev/null; then
        echo "$(pkg-config --modversion "$mod" 2>/dev/null)"
    else echo ""; fi
}
LIBSSH2_V="$(dep_status libssh2 libssh2)"
OPENSSL_V="$(dep_status openssl libcrypto)"
X11_V="$(dep_status x11 x11)"

# ===========================================================================
# 2. Recommend a configuration
# ===========================================================================
BUILD_TYPE="${FORCE_TYPE:-Release}"
NATIVE="${FORCE_NATIVE:-OFF}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"

print_scan() {
    printf '\n%s liu build %s— host scan\n' "$B" "$R"; hr
    printf '  %sPlatform%s   %s %s(%s)%s\n' "$B" "$R" "$PLATFORM" "$DIM" "$ARCH_N" "$R"
    printf '  %sCPUs%s       %s\n' "$B" "$R" "$NCPU"
    [[ -n "$CMAKE_VER" ]] && ok "cmake $CMAKE_VER"        || bad "cmake — REQUIRED, not found"
    [[ -n "$CC_BIN"    ]] && ok "compiler: $CC_BIN ${DIM}${CC_VER}${R}" || bad "C compiler — REQUIRED, not found"
    printf '  %sGPU%s        %s %s\n' "$B" "$R" "$GPU" "$([[ $GPU == Metal ]] && echo "$DIM(native)$R" || echo "$DIM(GL 3.3 core)$R")"
    printf '  %sSIMD%s       %s\n' "$B" "$R" "$SIMD"
    echo "  ${B}Dependencies${R}"
    [[ -n "$LIBSSH2_V" ]] && ok "libssh2 $LIBSSH2_V" || miss "libssh2 — CMake will search system/vendored paths"
    if [[ "$PLATFORM" == "Linux" ]]; then
        [[ -n "$OPENSSL_V" ]] && ok "openssl/libcrypto $OPENSSL_V" || miss "libcrypto — needed for vault (install libssl-dev)"
        [[ -n "$X11_V"     ]] && ok "X11 $X11_V"                   || miss "X11 — needed (install libx11-dev libxcursor-dev)"
    fi
    hr
    printf '  %sRecommended%s  type=%s%s%s  metal=%s%s%s  native=%s%s%s  package=%s%s%s  jobs=%s\n' \
        "$B" "$R" "$CYN" "$BUILD_TYPE" "$R" "$CYN" "$USE_METAL" "$R" "$CYN" "$NATIVE" "$R" "$CYN" "$DO_PACKAGE" "$R" "$NCPU"
    printf '  %sBuild dir%s    %s\n' "$DIM" "$R" "$BUILD_DIR"
    if [[ "$PLATFORM" == "macOS" ]]; then
        printf '  %sPackager%s     scripts/package_macos_dmg.sh  →  Liu-<ver>-%s.dmg + update zip\n' "$DIM" "$R" "$ARCH_N"
    elif [[ "$PLATFORM" == "Linux" ]]; then
        printf '  %sPackager%s     scripts/package_linux_tarball.sh  →  liu-<ver>-linux-%s.tar.gz\n' "$DIM" "$R" "$ARCH_N"
    fi
    hr
}

print_scan

if [[ -z "$CMAKE_VER" || -z "$CC_BIN" ]]; then
    bad "Missing a required tool (cmake / C compiler). Install it and re-run."
    exit 1
fi
[[ "$MODE" == "print" ]] && exit 0

# ===========================================================================
# 3. Interactive menu (skipped with --yes)
# ===========================================================================
if [[ "$MODE" == "tui" && -t 0 ]]; then
    while :; do
        printf '%s  [b]%s build with recommended   %s[t]%s toggle Debug/Release   %s[n]%s toggle native-arch   %s[m]%s toggle Metal/GL   %s[p]%s build + package   %s[q]%s quit\n' \
            "$B" "$R" "$B" "$R" "$B" "$R" "$B" "$R" "$B" "$R" "$B" "$R"
        printf '  > '
        read -r -n1 key; echo
        case "$key" in
            b|B|'') break ;;
            t|T) [[ "$BUILD_TYPE" == "Release" ]] && BUILD_TYPE="Debug" || BUILD_TYPE="Release"; print_scan ;;
            n|N) [[ "$NATIVE" == "ON" ]] && NATIVE="OFF" || NATIVE="ON"; print_scan ;;
            m|M) [[ "$USE_METAL" == "ON" ]] && USE_METAL="OFF" || USE_METAL="ON"; print_scan ;;
            p|P) DO_PACKAGE="ON"; break ;;
            q|Q) echo "aborted."; exit 0 ;;
            *) printf '%s  ? unknown key%s\n' "$DIM" "$R" ;;
        esac
    done
fi

# Release artifacts must never ship Debug binaries or host-tuned codegen: a
# -march=native build crashes on any older CPU it's downloaded to.
if [[ "$DO_PACKAGE" == "ON" ]]; then
    if [[ "$BUILD_TYPE" != "Release" ]]; then
        printf '%s!%s packaging forces Release (was %s)\n' "$YEL" "$R" "$BUILD_TYPE"
        BUILD_TYPE="Release"
    fi
    if [[ "$NATIVE" == "ON" ]]; then
        printf '%s!%s packaging forces a portable baseline — disabling native-arch tuning\n' "$YEL" "$R"
        NATIVE="OFF"
    fi
    if [[ "$PLATFORM" != "macOS" && "$PLATFORM" != "Linux" ]]; then
        bad "no packager for ${PLATFORM} (only macOS and Linux are supported)."
        exit 1
    fi
fi

# ===========================================================================
# 4. Configure + build
# ===========================================================================
printf '\n%s==>%s Configuring (%s, Metal=%s, native=%s)…\n' "$BLU" "$R" "$BUILD_TYPE" "$USE_METAL" "$NATIVE"
cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DUSE_METAL="$USE_METAL" \
    -DLIU_NATIVE_ARCH="$NATIVE"

printf '\n%s==>%s Building with %s jobs…\n' "$BLU" "$R" "$NCPU"
cmake --build "$BUILD_DIR" -j"$NCPU"

BIN="$BUILD_DIR/Liu"
echo
if [[ -x "$BIN" ]]; then
    SZ="$(ls -lh "$BIN" | awk '{print $5}')"
    printf '%s✓ Build complete:%s %s %s(%s)%s\n' "$GRN" "$R" "$BIN" "$DIM" "$SZ" "$R"
    printf '  run it:  %s%s%s\n' "$B" "$BIN" "$R"
else
    bad "Build finished but $BIN was not produced — check the log above."
    exit 1
fi

# ===========================================================================
# 5. Package (optional) — dispatch to the per-OS release packager. Both
#    packagers reuse this script's configured cache via BUILD_DIR, so the
#    artifact contains exactly the binaries built above (Release, portable).
# ===========================================================================
if [[ "$DO_PACKAGE" == "ON" ]]; then
    printf '\n%s==>%s Packaging (%s %s)…\n' "$BLU" "$R" "$PLATFORM" "$ARCH_N"
    case "$PLATFORM" in
        macOS) PKG_LOG="$(BUILD_DIR="$BUILD_DIR" BUILD_TYPE="$BUILD_TYPE" JOBS="$NCPU" \
                   "$ROOT/scripts/package_macos_dmg.sh")" ;;
        Linux) PKG_LOG="$(BUILD_DIR="$BUILD_DIR" BUILD_TYPE="$BUILD_TYPE" JOBS="$NCPU" \
                   "$ROOT/scripts/package_linux_tarball.sh")" ;;
    esac
    # Both packagers end stdout with the artifact path(s) (macOS: dmg + update
    # zip; Linux: tarball). Report every trailing line that is a real file.
    FOUND=""
    while IFS= read -r line; do
        # if-form on purpose: a bare `[[ ]] &&` here would make the while loop
        # (and the script, via set -e) fail when the last line isn't a file.
        if [[ -f "$line" ]]; then
            printf '%s✓ Package ready:%s %s\n' "$GRN" "$R" "$line"; FOUND=1
        fi
    done <<< "$PKG_LOG"
    if [[ -z "$FOUND" ]]; then
        bad "packager finished but no artifact path was reported — check the log above."
        exit 1
    fi
fi
