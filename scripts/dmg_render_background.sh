#!/usr/bin/env bash
# Regenerate the styled-DMG installer-window background from its HTML source.
#
# Produces, from assets/dmg/background.html:
#   assets/dmg/background.png      (1x, 640x400)
#   assets/dmg/background@2x.png   (2x, 1280x800)
#   assets/dmg/background.tiff     (HiDPI multi-rep — what Finder uses)
#
# Rendering is done with native WebKit via scripts/dmg_render_background.swift
# (compiled on the fly), so there is no headless-browser dependency. macOS only.
# Run this whenever background.html changes; the PNGs/TIFF are committed so the
# packaging path (scripts/package_macos_dmg.sh) needs no renderer at build time.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DMG_DIR="${ROOT_DIR}/assets/dmg"
HTML="${DMG_DIR}/background.html"
SWIFT="${ROOT_DIR}/scripts/dmg_render_background.swift"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "DMG background rendering is macOS-only (uses WebKit)." >&2
    exit 1
fi
[[ -f "${HTML}"  ]] || { echo "missing ${HTML}"  >&2; exit 1; }
[[ -f "${SWIFT}" ]] || { echo "missing ${SWIFT}" >&2; exit 1; }

WORK="$(mktemp -d)"
BIN="${WORK}/dmgrender"
trap 'rm -rf "${WORK}"' EXIT
echo "Compiling renderer…"
swiftc -O "${SWIFT}" -o "${BIN}"

# render <out.png> <pixelWidth> — WebKit emits benign "could not create
# directory …/WebKit/…" sandbox warnings to stderr; filter them but keep real
# errors (and the non-zero exit, which set -e catches).
render() {
    local out="$1" px="$2"
    "${BIN}" "${HTML}" "${out}" 640 400 "${px}" \
        2> >(grep -vE 'could not create directory|sandbox extension' >&2 || true)
}

# Logical canvas is 640x400 points (must match the Finder window size in
# package_macos_dmg.sh). Render 1x and 2x by target pixel width.
echo "Rendering background.png (640x400)…"
render "${DMG_DIR}/background.png"    640
echo "Rendering background@2x.png (1280x800)…"
render "${DMG_DIR}/background@2x.png" 1280

# Tag DPI so the pair describes the SAME 640x400-point canvas at 1x/2x: the
# WebKit snapshots come out at 144 dpi, which makes tiffutil reject the pair.
# 72 dpi for the 1x rep, 144 dpi for the 2x rep.
sips -s dpiWidth 72  -s dpiHeight 72  "${DMG_DIR}/background.png"    >/dev/null
sips -s dpiWidth 144 -s dpiHeight 144 "${DMG_DIR}/background@2x.png" >/dev/null

# Fold both into one HiDPI TIFF so Finder shows the crisp @2x rep on Retina.
echo "Combining HiDPI background.tiff…"
tiffutil -cathidpicheck "${DMG_DIR}/background.png" "${DMG_DIR}/background@2x.png" \
    -out "${DMG_DIR}/background.tiff" >/dev/null

echo "Done:"
for f in background.png background@2x.png background.tiff; do
    printf "  %s — " "${f}"
    sips -g pixelWidth -g pixelHeight "${DMG_DIR}/${f}" 2>/dev/null \
        | awk '/pixelWidth/{w=$2}/pixelHeight/{h=$2}END{printf "%sx%s\n", w, h}'
done
