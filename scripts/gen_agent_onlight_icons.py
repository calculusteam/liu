#!/usr/bin/env python3
"""
Derive the missing surface cut of an agent logo that only ships one.

Liu draws agent logos straight onto the panel body with no tile under them, so
a mark has to survive both the ~0.13 body of a dark theme and the ~0.96 of the
light one. Most logos manage that on their own. A MONOCHROME or TWO-TONE mark
cannot: it reads on exactly one of the two and disappears into the other, which
is why `agent_icons.c` carries a second slot table (`g_slots_on_light`) keyed on
the background's luminance.

Where the vendor publishes both cuts Liu ships theirs verbatim — OpenCode does,
and Command Code's pair comes out of the `command-code` npm package's own VSIX
(`extension/icons/icon-{dark,light}.svg`). Kimi, Continue, Droid, xAI, Pi, Oh My Pi,
Hermes, Qoder and Mastra Code publish one cut each, so theirs are derived here rather than by hand, so the
transformation is inspectable and can be re-run when the source logo changes.

    python3 scripts/gen_agent_onlight_icons.py           # write the PNGs
    python3 scripts/gen_agent_onlight_icons.py --check   # verify, change nothing

Needs Pillow. The output is committed, like src/voice/whisper_vulkan_spv.h —
this is not part of the build.
"""

import sys
import os

try:
    from PIL import Image
except ImportError:  # pragma: no cover - operator-facing
    sys.exit("Pillow is required: pip install Pillow")

ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                      "assets", "agents")

# The tone the existing on-light cuts already sit at: opencode-on-light is
# (71,68,68) and commandcode-on-light (66,66,66), both ~0.26 relative
# luminance, which clears 3x against a 0.955 panel. Matching it keeps the
# derived cuts from standing out beside the vendor-supplied ones.
INK_DARK = 62
# Continue's own two tones, kept exactly so the inversion is a swap rather
# than a new palette.
CONT_LO, CONT_HI = 0.19, 0.94


def luma(r, g, b):
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0


def kimi_on_light(src):
    """Kimi: a flat white mark on transparency with one blue accent square.

    Only the white ink is remapped. The accent is already a mid blue and
    reads on white on its own, and recolouring it would change the logo
    rather than adapt it — so saturation, not position, decides what is
    touched. Alpha is untouched throughout, which is what carries the mark's
    antialiased edges.
    """
    out = Image.new("RGBA", src.size)
    sp, op = src.load(), out.load()
    w, h = src.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = sp[x, y]
            if a == 0:
                op[x, y] = (0, 0, 0, 0)
                continue
            sat = (max(r, g, b) - min(r, g, b)) / 255.0
            if sat > 0.20:
                op[x, y] = (r, g, b, a)          # the blue accent, as drawn
            else:
                op[x, y] = (INK_DARK, INK_DARK, INK_DARK, a)
    return out


def invert_plated(src):
    """A PLATED greyscale logo: a filled badge in one tone, the mark cut out
    of it in the other. Five of these ship in the tree and they fail in two
    mirror-image ways — Continue's badge is white and vanishes into a light
    panel, while Droid and xAI are black and vanish into a dark one.

    Matting the badge away is the wrong fix for both. The mark is a small
    fraction of the artwork (13% for Continue, 11% for xAI), so dissolving the
    badge leaves a thin glyph floating in the icon box while the other theme
    shows a full one — the same logo at two apparent sizes. Inverting keeps
    the badge and swaps which tone fills it, which is exactly what a
    dark/light pair of a plated two-tone logo IS.

    The remap is continuous, so the antialiasing between the two tones
    survives, and it lands on Continue's own two values rather than pure black
    and white — a badge at 1.0 would glare against every panel in the picker.
    All five marks are fully desaturated, so there is no hue to preserve and
    luminance carries everything.
    """
    out = Image.new("RGBA", src.size)
    sp, op = src.load(), out.load()
    w, h = src.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = sp[x, y]
            if a == 0:
                op[x, y] = (0, 0, 0, 0)
                continue
            v = int(round((CONT_LO + (1.0 - luma(r, g, b)) *
                           (CONT_HI - CONT_LO)) * 255))
            v = max(0, min(255, v))
            op[x, y] = (v, v, v, a)
    return out


# Oh My Pi's mark: a flat near-black plate with the Pi glyph in a three-stop diagonal gradient,
# both exactly as `packages/collab-web/public/favicon.svg` of can1357/oh-my-pi writes them
# (viewBox 64, glyph box 14,16 - 50,56, gradient x1,y1 = 0,0 to x2,y2 = 1,1 over that box).
OMP_PLATE = (0x0f, 0x0a, 0x14)
OMP_PLATE_ON_DARK = (232, 230, 240)
OMP_GLYPH_BOX = (14.0, 16.0, 50.0, 56.0)
OMP_STOPS = [(0.0, (0xed, 0x4a, 0xbf)), (0.5, (0x9b, 0x4d, 0xff)), (1.0, (0x5a, 0xd8, 0xe6))]


def omp_gradient(x, y):
    """The glyph's colour at a point of the 64 x 64 viewBox."""
    x0, y0, x1, y1 = OMP_GLYPH_BOX
    t = max(0.0, min(1.0, ((x - x0) / (x1 - x0) + (y - y0) / (y1 - y0)) / 2.0))
    for (ta, ca), (tb, cb) in zip(OMP_STOPS, OMP_STOPS[1:]):
        if t <= tb:
            f = (t - ta) / (tb - ta)
            return tuple(ca[i] + (cb[i] - ca[i]) * f for i in range(3))
    return OMP_STOPS[-1][1]


def relight_omp(src):
    """Oh My Pi: a COLOURED plated mark, so `invert_plated` is the wrong tool.

    The plate is one flat near-black (#0f0a14), which vanishes into a dark panel
    (a dark theme's panel is ~(14,16,22)) and leaves only a gradient
    glyph floating at half the icon's size; the glyph is a vivid pink-to-cyan
    gradient, which a luminance inversion would turn into a muddy negative. Neither
    is needed: every pixel is a mix of the flat plate and the glyph's own colour at
    that spot, and the gradient is known, so the mix is solved exactly
    (t = projection of the pixel on plate -> glyph) and recomposited over a LIGHT plate.
    The glyph passes through unchanged; the plate's alpha (its rounded edge) is kept.
    """
    out = Image.new("RGBA", src.size)
    sp, op = src.load(), out.load()
    w, h = src.size
    P, P2 = OMP_PLATE, OMP_PLATE_ON_DARK
    for y in range(h):
        for x in range(w):
            r, g, b, a = sp[x, y]
            if a == 0:
                op[x, y] = (0, 0, 0, 0)
                continue
            G = omp_gradient((x + 0.5) * 64.0 / w, (y + 0.5) * 64.0 / h)
            d = [G[i] - P[i] for i in range(3)]
            den = sum(v * v for v in d)
            t = sum((c - P[i]) * d[i] for i, c in enumerate((r, g, b))) / den
            t = max(0.0, min(1.0, t))
            op[x, y] = tuple(int(round(P2[i] * (1.0 - t) + G[i] * t)) for i in range(3)) + (a,)
    return out


# (source, derived file, transform, which SURFACE the derived cut serves)
#
# The direction differs per logo because the vendor artwork does. Kimi and
# Continue are drawn for a dark surface, so what is missing is the light one.
# Droid and xAI are the opposite: their black badge already reads on
# white and it is the DARK panel that swallows it, so the derived cut is the
# dark-surface one and the vendor file keeps serving light surfaces. The
# filename says which, and agent_icons.c wires them accordingly.
# Hermes ships as a FLAT BLACK mask: the face and the hair's highlights are holes in it, so on a
# dark panel the black hair is the panel and the drawing is gone. The light-surface cut is the
# supplied black art; the dark-surface cut is the same alpha in one light tone.
FLAT_ON_DARK = (232, 232, 236)


def lighten_flat(src):
    """A flat mask: every texel the same colour and only the alpha carries the drawing,
    so the dark-surface cut is the same alpha filled with a light tone. Texels that are
    not (nearly) the one source colour would carry information a recolour throws away,
    which is why this refuses artwork that is not flat."""
    out = Image.new("RGBA", src.size)
    sp, op = src.load(), out.load()
    w, h = src.size
    seen = None
    for y in range(h):
        for x in range(w):
            r, g, b, a = sp[x, y]
            if a == 0:
                op[x, y] = (0, 0, 0, 0)
                continue
            if a >= 128:
                if seen is None:
                    seen = (r, g, b)
                elif max(abs(r - seen[0]), abs(g - seen[1]), abs(b - seen[2])) > 6:
                    raise ValueError("lighten_flat: the artwork is not a flat mask")
            op[x, y] = FLAT_ON_DARK + (a,)
    return out


DERIVED = [
    ("kimi.png",     "kimi-on-light.png",     kimi_on_light,  "light"),
    ("continue.png", "continue-on-light.png", invert_plated,  "light"),
    ("droid.png",    "droid-on-dark.png",     invert_plated,  "dark"),
    ("xai.png",      "xai-on-dark.png",       invert_plated,  "dark"),
    ("pi.png",       "pi-on-dark.png",        invert_plated,  "dark"),
    ("omp.png",      "omp-on-dark.png",       relight_omp,    "dark"),
    ("hermes.png",   "hermes-on-dark.png",    lighten_flat,   "dark"),
    ("qoder.png",    "qoder-on-light.png",    invert_plated,  "light"),
    ("mastracode.png", "mastracode-on-dark.png", invert_plated, "dark"),
]


def main():
    check = "--check" in sys.argv[1:]
    bad = 0
    for src_name, dst_name, fn, surface in DERIVED:
        src_path = os.path.join(ASSETS, src_name)
        dst_path = os.path.join(ASSETS, dst_name)
        if not os.path.exists(src_path):
            print("missing source: %s" % src_path)
            bad += 1
            continue
        made = fn(Image.open(src_path).convert("RGBA"))

        px = [p for p in made.getdata() if p[3] > 200]
        avg = tuple(sum(c[i] for c in px) // len(px) for i in range(3))
        l = luma(*avg)
        # The panel this cut will actually be drawn on: the picker paints the
        # theme background plus 0.06, which is ~0.13 on the default dark theme
        # and ~1.0 on the light one.
        panel = 0.955 if surface == "light" else 0.131
        hi, lo = max(l, panel), min(l, panel)
        ratio = (hi + 0.05) / (lo + 0.05)
        print("%-24s -> %-5s surface   ink avg=%-15s luma=%.2f  %.2fx"
              % (dst_name, surface, str(avg), l, ratio))
        if ratio < 2.0:
            print("   ^ under 2.0x — the derivation needs revisiting")
            bad += 1

        if check:
            if not os.path.exists(dst_path):
                print("   ^ not generated yet")
                bad += 1
            else:
                have = Image.open(dst_path).convert("RGBA")
                if list(have.getdata()) != list(made.getdata()):
                    print("   ^ committed PNG differs from the derivation")
                    bad += 1
        else:
            made.save(dst_path, optimize=True)

    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
