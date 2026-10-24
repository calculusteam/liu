#!/usr/bin/env python3
"""The background field for the installer window of the disk image.

A slow violet haze quantised to six tones by an 8x8 ordered (Bayer) dither,
under a layer of still film grain. The installer window is drawn from
assets/dmg/background.html, and it shows one frame of that field and the grain.

This is a numpy model of the field, one frame of it, and nothing else.

Writes, next to background.html:

  field.png  160 x 100 — ONE PIXEL PER DITHER CELL. The page scales it to the
             640 x 400 canvas with `image-rendering: pixelated`, so a cell is 4
             points square at 1x and 8 pixels at 2x.
  grain.png  200 x 200 RGBA — a grain tile: white or black specks at a
             random alpha of at most 3.5 %, tiled by the page at 200 px.

Both are committed (the packaging path needs no numpy); rerun this, then
scripts/dmg_render_background.sh, when either changes.

    python3 scripts/dmg_render_field.py
"""
import os
import sys

import numpy as np
from PIL import Image

# The canvas is 640 x 400 points; one dither cell is 4 of them, so the field
# is 160 x 100 cells.
CANVAS_W, CANVAS_H, CELL = 640, 400, 4
W, H = CANVAS_W // CELL, CANVAS_H // CELL

# A frame time with good tone coverage, the field's intensity and its
# number of tones.
T = 40.0
INTENSITY = 0.6
LEVELS = 6

# The film grain: a 200 x 200 tile, half the specks white and half black,
# alpha 0 .. 3.5 % of 255.
GRAIN_N = 200
GRAIN_ALPHA = 0.035
GRAIN_SEED = 7


def fract(x):
    return x - np.floor(x)


def hash2(px, py):
    px = fract(px * 123.34)
    py = fract(py * 456.21)
    d = px * (px + 45.32) + py * (py + 45.32)
    px = px + d
    py = py + d
    return fract(px * py)


def vnoise(px, py):
    ix, iy = np.floor(px), np.floor(py)
    fx, fy = px - ix, py - iy
    fx = fx * fx * (3 - 2 * fx)
    fy = fy * fy * (3 - 2 * fy)
    a = hash2(ix, iy)
    b = hash2(ix + 1, iy)
    c = hash2(ix, iy + 1)
    d = hash2(ix + 1, iy + 1)
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy


def fbm(px, py):
    s = 0
    a = .5
    for _ in range(4):
        s = s + a * vnoise(px, py)
        px = px * 2.03 + 17.1
        py = py * 2.03 + 17.1
        a *= .5
    return s


def bayer2(ax, ay):
    return fract(np.floor(ax) * .5 + np.floor(ay) ** 2 * .75)


def bayer4(ax, ay):
    return bayer2(.5 * ax, .5 * ay) * .25 + bayer2(ax, ay)


def bayer8(ax, ay):
    return bayer4(.5 * ax, .5 * ay) * .25 + bayer2(ax, ay)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


# #0A0A0B (ink-950) .. #211A41, six tones.
C0 = np.array([0x0A, 0x0A, 0x0B], dtype=np.float64)
CN = np.array([0x21, 0x1A, 0x41], dtype=np.float64)
PALETTE = np.array([C0 + (CN - C0) * (k / (LEVELS - 1)) for k in range(LEVELS)])


def field(w, h, t, inten):
    """The tone index (0 .. LEVELS-1) of every cell, row 0 at the TOP."""
    gx, gy = np.meshgrid(np.arange(w) + .5, np.arange(h) + .5)
    gy = h - gy                                  # the shader's origin is bottom-left
    ux, uy = gx / w, gy / h
    asp = w / h
    px, py = (ux - .5) * asp, (uy - .5)
    wx = fbm(px * 1.6, py * 1.6 + t * .05)
    wy = fbm(px * 1.6 + 5.2, py * 1.6 - t * .04)
    f = fbm(px * 1.3 + 1.4 * (wx - .5) + t * .03, py * 1.3 + 1.4 * (wy - .5) - t * .02)

    def blob(cx, cy, sx, sy, k):
        dx = (px - cx) * sx
        dy = (py - cy) * sy
        return np.exp(-(dx * dx + dy * dy) * k)

    glow = np.maximum(blob(.30 * asp, .30, .8, 1.1, 2.2), .85 * blob(-.32 * asp, -.34, 1.0, 1.2, 2.6))
    v = smoothstep(.34, .76, f) * (.40 + .60 * glow) * inten
    return np.floor(v * (LEVELS - 1) + bayer8(gx, gy)).clip(0, LEVELS - 1).astype(int)


def grain(n, seed):
    rng = np.random.default_rng(seed)
    rgba = np.zeros((n, n, 4), dtype=np.uint8)
    rgba[..., :3] = np.where(rng.random((n, n)) < .5, 0, 255).astype(np.uint8)[..., None]
    rgba[..., 3] = np.floor(rng.random((n, n)) * GRAIN_ALPHA * 255).astype(np.uint8)
    return rgba


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "dmg")
    q = field(W, H, T, INTENSITY)
    Image.fromarray(np.rint(PALETTE[q]).astype(np.uint8), "RGB").save(os.path.join(out, "field.png"))
    Image.fromarray(grain(GRAIN_N, GRAIN_SEED), "RGBA").save(os.path.join(out, "grain.png"))
    print("field.png %dx%d, tone coverage:" % (W, H),
          " ".join("%d:%.1f%%" % (k, 100 * (q == k).mean()) for k in range(LEVELS)))
    print("grain.png %dx%d" % (GRAIN_N, GRAIN_N))


if __name__ == "__main__":
    sys.exit(main())
