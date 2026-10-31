/*
 * The device around an emulated viewport painted as hardware, with Core
 * Graphics: the metal frame lit from above with its dark outer edge, its
 * polished highlight and the chamfer into the glass; antenna bands across a
 * phone's frame; the front glass with a lit edge and a faint sheen; side
 * buttons shaded as the rounded parts they are; the front camera, flash,
 * earpiece grille and home button in the bezel; a folding device's hinge; a
 * laptop's deck and its opening notch; a monitor's neck and foot.
 *
 * Bodies and glass take continuous-curvature corners: the straight edge eases
 * into the arc over about 1.53 radii instead of meeting a circle at a point,
 * which is how the hardware is cut.
 *
 * Only the ring around the screen is painted, as up to four bands that cover
 * everything outside a rectangle inside the screen: the screen itself is
 * filled by the Metal painter (its strips, cutout and status bar) and covered
 * by the web view, so a laptop's or a monitor's large display costs nothing
 * here. The bands are kept for the last two layouts asked for.
 */
#include "browser/browser.h"

#include <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { f32 x, y, w, h, r[4]; } ArtRect;   /* pixels, y down */
typedef struct { f32 r, g, b; } ArtRgb;

static ArtRgb art_rgb(u32 v) {
    ArtRgb c = { (f32)((v >> 16) & 255) / 255.0f, (f32)((v >> 8) & 255) / 255.0f,
                 (f32)(v & 255) / 255.0f };
    return c;
}

/* Toward white for t > 0, toward black for t < 0. */
static ArtRgb art_shade(ArtRgb c, f32 t) {
    if (t >= 0.0f) {
        c.r += (1.0f - c.r) * t; c.g += (1.0f - c.g) * t; c.b += (1.0f - c.b) * t;
    } else {
        c.r *= 1.0f + t; c.g *= 1.0f + t; c.b *= 1.0f + t;
    }
    return c;
}

static f32 art_luma(ArtRgb c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

static ArtRect art_rect(const BrowserDeviceShape *s, f32 dpi) {
    ArtRect a = { s->x * dpi, s->y * dpi, s->w * dpi, s->h * dpi,
                  { s->r[0] * dpi, s->r[1] * dpi, s->r[2] * dpi, s->r[3] * dpi } };
    return a;
}

static ArtRect art_inset(ArtRect a, f32 d) {
    a.x += d; a.y += d; a.w -= 2.0f * d; a.h -= 2.0f * d;
    if (a.w < 0.0f) a.w = 0.0f;
    if (a.h < 0.0f) a.h = 0.0f;
    for (i32 i = 0; i < 4; i++) a.r[i] = a.r[i] > d ? a.r[i] - d : 0.0f;
    return a;
}

static ArtRect art_circle(f32 cx, f32 cy, f32 d) {
    f32 r = d * 0.5f;
    ArtRect a = { cx - r, cy - r, d, d, { r, r, r, r } };
    return a;
}

/* One corner, from the incoming edge to the outgoing one: `v` is the corner's
 * vertex, `a` the incoming edge's direction and `b` the outgoing one's. A
 * point (s, t) of the corner's curve is s radii back along the incoming edge
 * and t radii along the outgoing one. */
static void art_corner(CGMutablePathRef p, f32 vx, f32 vy, f32 ax, f32 ay,
                       f32 bx, f32 by, f32 r, bool smooth) {
#define PT(s, t) (CGFloat)(vx - (s) * r * ax + (t) * r * bx), \
                 (CGFloat)(vy - (s) * r * ay + (t) * r * by)
    if (!(r > 0.0f)) {
        CGPathAddLineToPoint(p, NULL, vx, vy);
        return;
    }
    if (!smooth) {
        CGPathAddLineToPoint(p, NULL, PT(1.0f, 0.0f));
        CGPathAddArcToPoint(p, NULL, vx, vy, PT(0.0f, 1.0f), r);
        return;
    }
    CGPathAddLineToPoint(p, NULL, PT(1.52866483f, 0.0f));
    CGPathAddCurveToPoint(p, NULL, PT(1.08849323f, 0.0f), PT(0.86840689f, 0.0f),
                          PT(0.63149399f, 0.07491100f));
    CGPathAddCurveToPoint(p, NULL, PT(0.37282392f, 0.16905899f), PT(0.16906013f, 0.37282401f),
                          PT(0.07491176f, 0.63149399f));
    CGPathAddCurveToPoint(p, NULL, PT(0.0f, 0.86840701f), PT(0.0f, 1.08849299f),
                          PT(0.0f, 1.52866483f));
#undef PT
}

/* A rounded rectangle, radii top-left, top-right, bottom-right, bottom-left.
 * Smooth corners need 1.53 radii of straight edge each; a shape too short for
 * them takes circular ones, and radii that do not fit are scaled down. */
static void art_add_rect(CGMutablePathRef p, ArtRect a, bool smooth) {
    if (!(a.w > 0.0f) || !(a.h > 0.0f)) return;
    f32 r[4];
    for (i32 i = 0; i < 4; i++) r[i] = a.r[i] > 0.0f ? a.r[i] : 0.0f;
    const f32 len[4] = { a.w, a.h, a.w, a.h };   /* top, right, bottom, left */
    f32 k = smooth ? 1.52866483f : 1.0f;
    for (i32 e = 0; e < 4 && smooth; e++)
        if (k * (r[e] + r[(e + 1) & 3]) > len[e]) { smooth = false; k = 1.0f; }
    f32 f = 1.0f;
    for (i32 e = 0; e < 4; e++) {
        f32 need = k * (r[e] + r[(e + 1) & 3]);
        if (need > len[e] && need > 0.0f) f = fminf(f, len[e] / need);
    }
    for (i32 i = 0; i < 4; i++) r[i] *= f;
    f32 x0 = a.x, y0 = a.y, x1 = a.x + a.w, y1 = a.y + a.h;
    CGPathMoveToPoint(p, NULL, x0 + k * r[BDEV_TL], y0);
    art_corner(p, x1, y0, 1.0f, 0.0f, 0.0f, 1.0f, r[BDEV_TR], smooth);
    art_corner(p, x1, y1, 0.0f, 1.0f, -1.0f, 0.0f, r[BDEV_BR], smooth);
    art_corner(p, x0, y1, -1.0f, 0.0f, 0.0f, -1.0f, r[BDEV_BL], smooth);
    art_corner(p, x0, y0, 0.0f, -1.0f, 1.0f, 0.0f, r[BDEV_TL], smooth);
    CGPathCloseSubpath(p);
}

static CGColorSpaceRef art_space(void) {
    static CGColorSpaceRef s;
    if (!s) s = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    return s;
}

static void art_fill(CGContextRef c, ArtRect a, bool smooth, ArtRgb col, f32 alpha) {
    if (!(a.w > 0.0f) || !(a.h > 0.0f) || !(alpha > 0.0f)) return;
    CGMutablePathRef p = CGPathCreateMutable();
    art_add_rect(p, a, smooth);
    CGContextSetRGBFillColor(c, col.r, col.g, col.b, alpha);
    CGContextAddPath(c, p);
    CGContextFillPath(c);
    CGPathRelease(p);
}

/* Clip to a shape, or to the ring between two shapes (`inner` with w == 0
 * means none). Paired with CGContextRestoreGState. */
static void art_clip(CGContextRef c, ArtRect outer, ArtRect inner, bool smooth) {
    CGContextSaveGState(c);
    CGMutablePathRef p = CGPathCreateMutable();
    art_add_rect(p, outer, smooth);
    art_add_rect(p, inner, smooth);
    CGContextAddPath(c, p);
    CGContextEOClip(c);
    CGPathRelease(p);
}

/* A linear gradient from (x0, y0) to (x1, y1) over the clip: `n` stops of
 * colour, alpha and location. Past its end it keeps the last stop's colour,
 * or, with `stop`, paints nothing (cheaper, for a gradient that fades out). */
typedef struct { ArtRgb c; f32 a, at; } ArtStop;
static void art_gradient_ex(CGContextRef c, f32 x0, f32 y0, f32 x1, f32 y1,
                            const ArtStop *s, i32 n, bool stop) {
    CGFloat comp[4 * 8], loc[8];
    if (n > 8) n = 8;
    for (i32 i = 0; i < n; i++) {
        comp[4 * i + 0] = s[i].c.r; comp[4 * i + 1] = s[i].c.g;
        comp[4 * i + 2] = s[i].c.b; comp[4 * i + 3] = s[i].a;
        loc[i] = s[i].at;
    }
    CGGradientRef g = CGGradientCreateWithColorComponents(art_space(), comp, loc, (size_t)n);
    if (!g) return;
    CGContextDrawLinearGradient(c, g, CGPointMake(x0, y0), CGPointMake(x1, y1),
                                kCGGradientDrawsBeforeStartLocation |
                                (stop ? 0 : kCGGradientDrawsAfterEndLocation));
    CGGradientRelease(g);
}

static void art_gradient(CGContextRef c, f32 x0, f32 y0, f32 x1, f32 y1,
                         const ArtStop *s, i32 n) {
    art_gradient_ex(c, x0, y0, x1, y1, s, n, false);
}

/* A radial gradient from a point to a circle around (cx, cy). */
static void art_radial(CGContextRef c, f32 fx, f32 fy, f32 cx, f32 cy, f32 rad,
                       const ArtStop *s, i32 n) {
    CGFloat comp[4 * 8], loc[8];
    if (n > 8) n = 8;
    for (i32 i = 0; i < n; i++) {
        comp[4 * i + 0] = s[i].c.r; comp[4 * i + 1] = s[i].c.g;
        comp[4 * i + 2] = s[i].c.b; comp[4 * i + 3] = s[i].a;
        loc[i] = s[i].at;
    }
    CGGradientRef g = CGGradientCreateWithColorComponents(art_space(), comp, loc, (size_t)n);
    if (!g) return;
    CGContextDrawRadialGradient(c, g, CGPointMake(fx, fy), 0.0, CGPointMake(cx, cy), rad,
                                kCGGradientDrawsAfterEndLocation);
    CGGradientRelease(g);
}

static const ArtRgb ART_WHITE = { 1.0f, 1.0f, 1.0f };
static const ArtRgb ART_BLACK = { 0.0f, 0.0f, 0.0f };

/* The metal around the glass, from the outside in: a dark edge where the side
 * turns away, a polished highlight that is brightest on top, the frame's own
 * colour lit from above and a little from the left, and a dark chamfer into
 * the glass. */
static void art_frame(CGContextRef c, ArtRect body, ArtRect glass, f32 rim, ArtRgb frame) {
    bool light = art_luma(frame) > 0.5f;
    ArtStop base[] = { { art_shade(frame, 0.16f), 1.0f, 0.0f }, { frame, 1.0f, 0.45f },
                       { art_shade(frame, -0.18f), 1.0f, 1.0f } };
    art_clip(c, body, glass, true);
    art_gradient(c, 0.0f, body.y, 0.0f, body.y + body.h, base, 3);
    ArtStop sheen[] = { { ART_WHITE, light ? 0.30f : 0.22f, 0.0f }, { ART_WHITE, 0.0f, 0.42f },
                        { ART_BLACK, 0.0f, 0.62f }, { ART_BLACK, 0.20f, 1.0f } };
    art_gradient(c, body.x, body.y, body.x + body.w * 0.6f, body.y + body.h, sheen, 4);
    CGContextRestoreGState(c);

    f32 edge = fmaxf(0.6f, rim * 0.13f);
    f32 hi = fmaxf(0.7f, rim * 0.20f);
    f32 chamfer = fmaxf(0.6f, rim * 0.17f);
    art_clip(c, body, art_inset(body, edge), true);
    art_fill(c, body, true, art_shade(frame, light ? -0.48f : -0.62f), 0.92f);
    CGContextRestoreGState(c);

    art_clip(c, art_inset(body, edge), art_inset(body, edge + hi), true);
    ArtStop hl[] = { { ART_WHITE, light ? 0.85f : 0.62f, 0.0f },
                     { ART_WHITE, light ? 0.40f : 0.22f, 0.5f },
                     { ART_WHITE, light ? 0.55f : 0.30f, 1.0f } };
    art_gradient(c, 0.0f, body.y, 0.0f, body.y + body.h, hl, 3);
    CGContextRestoreGState(c);

    art_clip(c, art_inset(body, rim - chamfer), glass, true);
    art_fill(c, art_inset(body, rim - chamfer), true, art_shade(frame, -0.55f), 0.85f);
    CGContextRestoreGState(c);
}

/* Antenna bands across a phone's frame: thin breaks near both ends of the
 * long sides. */
static void art_bands(CGContextRef c, ArtRect body, ArtRect glass, f32 rim, f32 unit,
                      ArtRgb frame) {
    f32 bw = fmaxf(1.0f, unit * 1.2f);
    bool tall = body.h >= body.w;
    bool light = art_luma(frame) > 0.5f;
    ArtRgb col = art_shade(frame, light ? -0.38f : 0.10f);
    art_clip(c, body, glass, true);
    for (i32 i = 0; i < 2; i++) {
        f32 at = i == 0 ? 0.105f : 0.895f;
        ArtRect a, b;
        if (tall) {
            f32 y = body.y + body.h * at - bw * 0.5f;
            a = (ArtRect){ body.x - 1.0f, y, rim + 2.0f, bw, { 0, 0, 0, 0 } };
            b = (ArtRect){ body.x + body.w - rim - 1.0f, y, rim + 2.0f, bw, { 0, 0, 0, 0 } };
        } else {
            f32 x = body.x + body.w * at - bw * 0.5f;
            a = (ArtRect){ x, body.y - 1.0f, bw, rim + 2.0f, { 0, 0, 0, 0 } };
            b = (ArtRect){ x, body.y + body.h - rim - 1.0f, bw, rim + 2.0f, { 0, 0, 0, 0 } };
        }
        art_fill(c, a, false, col, light ? 0.85f : 0.70f);
        art_fill(c, b, false, col, light ? 0.85f : 0.70f);
    }
    CGContextRestoreGState(c);
}

/* The front glass: a touch lighter at the top, with a faint sheen from the
 * top-left. */
static void art_glass(CGContextRef c, ArtRect glass, ArtRgb col) {
    bool white = art_luma(col) > 0.5f;
    art_clip(c, glass, (ArtRect){ 0 }, true);
    ArtStop g[] = { { art_shade(col, white ? 0.35f : 0.03f), 1.0f, 0.0f },
                    { col, 1.0f, 0.55f }, { art_shade(col, white ? -0.05f : 0.0f), 1.0f, 1.0f } };
    art_gradient(c, 0.0f, glass.y, 0.0f, glass.y + glass.h, g, 3);
    ArtStop sh[] = { { ART_WHITE, white ? 0.30f : 0.04f, 0.0f }, { ART_WHITE, 0.0f, 1.0f } };
    art_gradient_ex(c, glass.x, glass.y, glass.x + glass.w * 0.55f, glass.y + glass.h * 0.30f,
                    sh, 2, true);
    CGContextRestoreGState(c);
}

/* The glass's edge, where it meets the frame and catches the light. */
static void art_glass_edge(CGContextRef c, ArtRect glass, ArtRgb col, f32 unit) {
    bool white = art_luma(col) > 0.5f;
    f32 e = fmaxf(0.7f, unit * 0.7f);
    art_clip(c, glass, art_inset(glass, e), true);
    ArtStop eg[] = { { white ? ART_BLACK : ART_WHITE, white ? 0.10f : 0.20f, 0.0f },
                     { white ? ART_BLACK : ART_WHITE, white ? 0.16f : 0.06f, 1.0f } };
    art_gradient(c, 0.0f, glass.y, 0.0f, glass.y + glass.h, eg, 2);
    CGContextRestoreGState(c);
}

/* A side button, shaded across its thickness as a rounded part standing out
 * of the frame: dark at its outer face's edge, lit along its crown, darker
 * where it meets the body. */
static void art_key(CGContextRef c, ArtRect k, ArtRect body, ArtRgb frame) {
    if (!(k.w > 0.0f) || !(k.h > 0.0f)) return;
    bool upright = k.h >= k.w;
    f32 kcx = k.x + k.w * 0.5f, kcy = k.y + k.h * 0.5f;
    f32 bcx = body.x + body.w * 0.5f, bcy = body.y + body.h * 0.5f;
    f32 x0, y0, x1, y1;   /* outer face to body */
    if (upright) {
        bool left = kcx < bcx;
        y0 = y1 = kcy;
        x0 = left ? k.x : k.x + k.w;
        x1 = left ? k.x + k.w : k.x;
    } else {
        bool top = kcy < bcy;
        x0 = x1 = kcx;
        y0 = top ? k.y : k.y + k.h;
        y1 = top ? k.y + k.h : k.y;
    }
    art_clip(c, k, (ArtRect){ 0 }, false);
    ArtStop s[] = { { art_shade(frame, -0.45f), 1.0f, 0.0f }, { art_shade(frame, 0.28f), 1.0f, 0.38f },
                    { art_shade(frame, 0.04f), 1.0f, 0.72f }, { art_shade(frame, -0.30f), 1.0f, 1.0f } };
    art_gradient(c, x0, y0, x1, y1, s, 4);
    ArtStop l[] = { { ART_WHITE, 0.12f, 0.0f }, { ART_WHITE, 0.0f, 0.5f }, { ART_BLACK, 0.16f, 1.0f } };
    art_gradient(c, 0.0f, k.y, 0.0f, k.y + k.h, l, 3);
    CGContextRestoreGState(c);
}

/* A camera lens set in the bezel: a dark ring, the lens's coated glass
 * deepest at its rim, and a small glint at the top-left. */
static void art_lens(CGContextRef c, ArtRect l, ArtRgb glass) {
    if (!(l.w > 1.0f)) return;
    bool white = art_luma(glass) > 0.5f;
    f32 d = l.w, cx = l.x + d * 0.5f, cy = l.y + d * 0.5f;
    ArtRect ring = art_circle(cx, cy, d);
    art_clip(c, ring, (ArtRect){ 0 }, false);
    ArtStop rs[] = { { white ? art_rgb(0xb4b4b0) : art_rgb(0x2b2c31), 1.0f, 0.0f },
                     { white ? art_rgb(0x8e8e8a) : art_rgb(0x111215), 1.0f, 1.0f } };
    art_gradient(c, 0.0f, ring.y, 0.0f, ring.y + ring.h, rs, 2);
    CGContextRestoreGState(c);
    ArtRect lens = art_circle(cx, cy, d * 0.64f);
    art_clip(c, lens, (ArtRect){ 0 }, false);
    ArtStop ls[] = { { art_rgb(0x3a4a70), 1.0f, 0.0f }, { art_rgb(0x141a2c), 1.0f, 0.55f },
                     { art_rgb(0x05060a), 1.0f, 1.0f } };
    art_radial(c, cx - d * 0.08f, cy - d * 0.08f, cx, cy, d * 0.32f, ls, 3);
    CGContextRestoreGState(c);
    ArtStop gs[] = { { ART_WHITE, 0.75f, 0.0f }, { ART_WHITE, 0.0f, 1.0f } };
    f32 gx = cx - d * 0.12f, gy = cy - d * 0.12f;
    art_clip(c, lens, (ArtRect){ 0 }, false);
    art_radial(c, gx, gy, gx, gy, d * 0.13f, gs, 2);
    ArtStop g2[] = { { art_rgb(0x8fb0ff), 0.35f, 0.0f }, { art_rgb(0x8fb0ff), 0.0f, 1.0f } };
    art_radial(c, cx + d * 0.10f, cy + d * 0.11f, cx + d * 0.10f, cy + d * 0.11f, d * 0.08f, g2, 2);
    CGContextRestoreGState(c);
}

/* The earpiece: a slot through the glass with its mesh in shadow, and the
 * glass's edge catching the light along its bottom. */
static void art_speaker(CGContextRef c, ArtRect s, ArtRgb glass, f32 unit) {
    if (!(s.w > 0.0f) || !(s.h > 0.0f)) return;
    bool white = art_luma(glass) > 0.5f;
    f32 e = fmaxf(0.7f, unit * 0.6f);
    ArtRect lip = s;
    lip.y += e;
    art_fill(c, lip, false, ART_WHITE, white ? 0.65f : 0.10f);
    art_fill(c, s, false, white ? art_rgb(0x9a9a96) : art_rgb(0x2a2a2e), 1.0f);
    art_clip(c, s, (ArtRect){ 0 }, false);
    /* The mesh, where the slot is tall enough to show it: rows of holes
     * catching a little light. */
    f32 thin = fminf(s.w, s.h);
    if (thin >= 5.0f) {
        f32 step = fmaxf(1.6f, thin / 4.5f), dot = step * 0.30f;
        CGContextSetRGBFillColor(c, 1.0, 1.0, 1.0, white ? 0.30 : 0.22);
        for (f32 y = s.y + step * 0.5f; y < s.y + s.h; y += step)
            for (f32 x = s.x + step * 0.5f; x < s.x + s.w; x += step)
                CGContextFillEllipseInRect(c, CGRectMake(x - dot, y - dot, 2.0f * dot, 2.0f * dot));
    }
    ArtStop sh[] = { { ART_BLACK, 0.70f, 0.0f }, { ART_BLACK, 0.0f, 0.75f } };
    if (s.h >= s.w) {
        sh[0].a = 0.55f;
        art_gradient(c, s.x, 0.0f, s.x + s.w, 0.0f, sh, 2);
    } else {
        art_gradient(c, 0.0f, s.y, 0.0f, s.y + s.h, sh, 2);
    }
    CGContextRestoreGState(c);
}

/* An LED flash behind its lens: a pale disc, brightest at its middle, with
 * the ring where the lens's steps are cut and a dark seat around it. */
static void art_flash(CGContextRef c, ArtRect f) {
    if (!(f.w > 1.0f)) return;
    f32 d = f.w, cx = f.x + d * 0.5f, cy = f.y + d * 0.5f;
    art_fill(c, art_circle(cx, cy, d), false, art_rgb(0x2a2a28), 1.0f);
    ArtRect disc = art_circle(cx, cy, d * 0.86f);
    art_clip(c, disc, (ArtRect){ 0 }, false);
    ArtStop ds[] = { { art_rgb(0xfffbea), 1.0f, 0.0f }, { art_rgb(0xece6cf), 1.0f, 0.55f },
                     { art_rgb(0xbdb7a0), 1.0f, 1.0f } };
    art_radial(c, cx - d * 0.06f, cy - d * 0.08f, cx, cy, d * 0.43f, ds, 3);
    CGContextRestoreGState(c);
    art_clip(c, art_circle(cx, cy, d * 0.60f), art_circle(cx, cy, d * 0.60f - fmaxf(0.7f, d * 0.05f)),
             false);
    art_fill(c, art_circle(cx, cy, d * 0.60f), false, art_rgb(0x8f8a75), 0.45f);
    CGContextRestoreGState(c);
}

/* A folding device's hinge seen along the edge it folds at: a dark spine
 * with two polished rods down its length, and at each end a metal cap, a
 * cylinder lit along its crown. Shaded across the hinge, whichever way it
 * runs. */
static void art_hinge(CGContextRef c, ArtRect h, const ArtRect caps[2], f32 unit) {
    if (!(h.w > 0.0f) || !(h.h > 0.0f)) return;
    bool down = h.h >= h.w;   /* runs top to bottom */
    f32 a0 = down ? h.x : h.y, across = down ? h.w : h.h;
#define ACROSS(t0, t1) (down ? (t0) : 0.0f), (down ? 0.0f : (t0)), \
                       (down ? (t1) : 0.0f), (down ? 0.0f : (t1))
    art_clip(c, h, (ArtRect){ 0 }, false);
    ArtStop sp[] = { { art_rgb(0x0b0b0c), 1.0f, 0.0f }, { art_rgb(0x262626), 1.0f, 0.5f },
                     { art_rgb(0x0b0b0c), 1.0f, 1.0f } };
    art_gradient(c, ACROSS(a0, a0 + across), sp, 3);
    f32 rod = fmaxf(0.8f, unit * 1.6f);
    for (i32 i = 0; i < 2; i++) {
        f32 at = a0 + across * (i == 0 ? 0.30f : 0.70f) - rod * 0.5f;
        ArtRect r = down ? (ArtRect){ at, h.y, rod, h.h, { 0, 0, 0, 0 } }
                         : (ArtRect){ h.x, at, h.w, rod, { 0, 0, 0, 0 } };
        art_clip(c, r, (ArtRect){ 0 }, false);
        ArtStop rs[] = { { art_rgb(0x3c3c3b), 1.0f, 0.0f }, { art_rgb(0xd9d9d4), 1.0f, 0.45f },
                         { art_rgb(0x4a4a48), 1.0f, 1.0f } };
        art_gradient(c, ACROSS(at, at + rod), rs, 3);
        CGContextRestoreGState(c);
    }
    CGContextRestoreGState(c);

    for (i32 i = 0; i < 2; i++) {
        ArtRect k = caps[i];
        if (!(k.w > 0.0f) || !(k.h > 0.0f)) continue;
        f32 k0 = down ? k.x : k.y, kn = down ? k.w : k.h;
        f32 l0 = down ? k.y : k.x, ln = down ? k.h : k.w;
        art_clip(c, k, (ArtRect){ 0 }, false);
        ArtStop cs[] = { { art_rgb(0x262625), 1.0f, 0.0f }, { art_rgb(0x8d8d89), 1.0f, 0.20f },
                         { art_rgb(0xf2f2ee), 1.0f, 0.34f }, { art_rgb(0x6e6e6a), 1.0f, 0.56f },
                         { art_rgb(0x3a3a38), 1.0f, 0.80f }, { art_rgb(0x1c1c1b), 1.0f, 1.0f } };
        art_gradient(c, ACROSS(k0, k0 + kn), cs, 6);
        ArtStop ends[] = { { ART_BLACK, 0.45f, 0.0f }, { ART_BLACK, 0.0f, 0.10f },
                           { ART_BLACK, 0.0f, 0.90f }, { ART_BLACK, 0.45f, 1.0f } };
        if (down) art_gradient(c, 0.0f, l0, 0.0f, l0 + ln, ends, 4);
        else      art_gradient(c, l0, 0.0f, l0 + ln, 0.0f, ends, 4);
        CGContextRestoreGState(c);
    }
#undef ACROSS
}

/* A home button: a ring in the frame's metal, lit on top, around a shallow
 * dish of glass that is shaded at its top and catches light at its bottom. */
static void art_home(CGContextRef c, ArtRect h, ArtRgb frame, ArtRgb glass) {
    if (!(h.w > 2.0f)) return;
    f32 d = h.w, cx = h.x + d * 0.5f, cy = h.y + d * 0.5f;
    f32 ring = fmaxf(1.0f, d * 0.05f);
    ArtRect outer = art_circle(cx, cy, d), dish = art_circle(cx, cy, d - 2.0f * ring);
    art_clip(c, outer, dish, false);
    ArtStop rs[] = { { art_shade(frame, 0.55f), 1.0f, 0.0f }, { art_shade(frame, 0.10f), 1.0f, 0.5f },
                     { art_shade(frame, -0.35f), 1.0f, 1.0f } };
    art_gradient(c, 0.0f, outer.y, 0.0f, outer.y + outer.h, rs, 3);
    CGContextRestoreGState(c);
    art_fill(c, dish, false, glass, 1.0f);
    art_clip(c, dish, (ArtRect){ 0 }, false);
    ArtStop ds[] = { { ART_BLACK, 0.55f, 0.0f }, { ART_BLACK, 0.0f, 0.45f },
                     { ART_WHITE, 0.0f, 0.70f }, { ART_WHITE, 0.09f, 1.0f } };
    art_gradient(c, 0.0f, dish.y, 0.0f, dish.y + dish.h, ds, 4);
    CGContextRestoreGState(c);
}

/* A laptop's deck seen from the front: its top edge catching the light, the
 * front face falling into shadow, and the notch the lid is opened by. */
static void art_deck(CGContextRef c, ArtRect base, ArtRect scoop, ArtRgb col, f32 unit) {
    if (!(base.w > 0.0f) || !(base.h > 0.0f)) return;
    art_clip(c, base, (ArtRect){ 0 }, false);
    ArtStop s[] = { { art_shade(col, 0.55f), 1.0f, 0.0f }, { art_shade(col, 0.18f), 1.0f, 0.14f },
                    { col, 1.0f, 0.50f }, { art_shade(col, -0.34f), 1.0f, 1.0f } };
    art_gradient(c, 0.0f, base.y, 0.0f, base.y + base.h, s, 4);
    ArtStop side[] = { { ART_BLACK, 0.16f, 0.0f }, { ART_BLACK, 0.0f, 0.12f },
                       { ART_BLACK, 0.0f, 0.88f }, { ART_BLACK, 0.16f, 1.0f } };
    art_gradient(c, base.x, 0.0f, base.x + base.w, 0.0f, side, 4);
    f32 hl = fmaxf(0.8f, unit * 0.8f);
    art_fill(c, (ArtRect){ base.x, base.y, base.w, hl, { 0, 0, 0, 0 } }, false, ART_WHITE, 0.55f);
    CGContextRestoreGState(c);
    if (scoop.w > 0.0f && scoop.h > 0.0f) {
        /* A recess: in shadow under the deck's top edge, its lower rim
         * catching the light. */
        ArtRect lip = scoop;
        lip.y += fmaxf(0.8f, unit * 0.8f);
        art_fill(c, lip, false, art_shade(col, 0.45f), 1.0f);
        art_clip(c, scoop, (ArtRect){ 0 }, false);
        ArtStop ss[] = { { art_shade(col, -0.30f), 1.0f, 0.0f }, { art_shade(col, -0.04f), 1.0f, 1.0f } };
        art_gradient(c, 0.0f, scoop.y, 0.0f, scoop.y + scoop.h, ss, 2);
        CGContextRestoreGState(c);
    }
}

/* A monitor's stand: the neck a flat plate with rounded sides, in the head's
 * shadow at its top, the foot lit along its top edge. */
static void art_stand(CGContextRef c, ArtRect neck, ArtRect foot, ArtRgb col, f32 unit) {
    if (neck.w > 0.0f && neck.h > 0.0f) {
        art_clip(c, neck, (ArtRect){ 0 }, false);
        ArtStop s[] = { { art_shade(col, -0.24f), 1.0f, 0.0f }, { art_shade(col, 0.04f), 1.0f, 0.14f },
                        { art_shade(col, 0.12f), 1.0f, 0.50f }, { art_shade(col, 0.02f), 1.0f, 0.86f },
                        { art_shade(col, -0.26f), 1.0f, 1.0f } };
        art_gradient(c, neck.x, 0.0f, neck.x + neck.w, 0.0f, s, 5);
        f32 fall = fminf(1.0f, unit * 26.0f / neck.h);
        ArtStop sh[] = { { ART_BLACK, 0.55f, 0.0f }, { ART_BLACK, 0.0f, fall } };
        art_gradient(c, 0.0f, neck.y, 0.0f, neck.y + neck.h, sh, 2);
        CGContextRestoreGState(c);
    }
    if (foot.w > 0.0f && foot.h > 0.0f) {
        art_clip(c, foot, (ArtRect){ 0 }, false);
        ArtStop s[] = { { art_shade(col, 0.50f), 1.0f, 0.0f }, { art_shade(col, 0.12f), 1.0f, 0.22f },
                        { art_shade(col, -0.30f), 1.0f, 1.0f } };
        art_gradient(c, 0.0f, foot.y, 0.0f, foot.y + foot.h, s, 3);
        ArtStop side[] = { { ART_BLACK, 0.18f, 0.0f }, { ART_BLACK, 0.0f, 0.15f },
                           { ART_BLACK, 0.0f, 0.85f }, { ART_BLACK, 0.18f, 1.0f } };
        art_gradient(c, foot.x, 0.0f, foot.x + foot.w, 0.0f, side, 4);
        CGContextRestoreGState(c);
    }
}

/* Whether a part reaches the band being painted, with room for its
 * antialiased edge. */
static bool art_meets(ArtRect a, CGRect band) {
    return a.w > 0.0f && a.h > 0.0f &&
           a.x - 2.0f < band.origin.x + band.size.width && band.origin.x < a.x + a.w + 2.0f &&
           a.y - 2.0f < band.origin.y + band.size.height && band.origin.y < a.y + a.h + 2.0f;
}

/* Strips that hold everything between `outer`'s outline and the largest
 * rectangle inside `inner` (inset by 0.3 of its largest radius, its corners
 * are on or inside the arcs). A ring painted under each strip alone is
 * rasterised over that strip, not over the whole device. */
static i32 art_ring_strips(ArtRect outer, ArtRect inner, CGRect out[4]) {
    f32 rmax = 0.0f;
    for (i32 i = 0; i < 4; i++) rmax = fmaxf(rmax, inner.r[i]);
    f32 in = rmax * 0.30f + 1.0f;
    f32 ix0 = inner.x + in, iy0 = inner.y + in;
    f32 ix1 = inner.x + inner.w - in, iy1 = inner.y + inner.h - in;
    f32 ox0 = outer.x - 2.0f, oy0 = outer.y - 2.0f;
    f32 ox1 = outer.x + outer.w + 2.0f, oy1 = outer.y + outer.h + 2.0f;
    if (ix1 - ix0 < 1.0f || iy1 - iy0 < 1.0f || ix0 < ox0 || iy0 < oy0 || ix1 > ox1 || iy1 > oy1) {
        out[0] = CGRectMake(ox0, oy0, ox1 - ox0, oy1 - oy0);
        return 1;
    }
    out[0] = CGRectMake(ox0, oy0, ox1 - ox0, iy0 - oy0);
    out[1] = CGRectMake(ox0, iy1, ox1 - ox0, oy1 - iy1);
    out[2] = CGRectMake(ox0, iy0, ix0 - ox0, iy1 - iy0);
    out[3] = CGRectMake(ix1, iy0, ox1 - ix1, iy1 - iy0);
    return 4;
}

/* The whole device, in the layout's pixels; the context's transform puts the
 * band being painted (`band`, layout pixels) at its origin. Parts that do not
 * reach the band are left out. */
static void art_paint(CGContextRef c, const BrowserDeviceLayout *d, f32 dpi, CGRect band) {
    ArtRgb frame = art_rgb(d->frame_rgb), glass_c = art_rgb(d->glass_rgb);
    ArtRgb base_c = art_rgb(d->base_rgb);
    f32 unit = d->scale * dpi;   /* pixels per CSS pixel */
    f32 rim = d->rim * dpi;
    ArtRect body = art_rect(&d->body, dpi), glass = art_rect(&d->glass, dpi);

    if (d->kind == BDEV_MONITOR && (art_meets(art_rect(&d->neck, dpi), band) ||
                                    art_meets(art_rect(&d->base, dpi), band)))
        art_stand(c, art_rect(&d->neck, dpi), art_rect(&d->base, dpi), base_c, unit);
    for (i32 i = 0; i < d->n_keys; i++)
        if (art_meets(art_rect(&d->keys[i], dpi), band))
            art_key(c, art_rect(&d->keys[i], dpi), body, frame);

    CGRect strips[4];
    i32 ns = art_ring_strips(body, glass, strips);
    for (i32 i = 0; i < ns; i++) {
        if (!CGRectIntersectsRect(strips[i], band)) continue;
        CGContextSaveGState(c);
        CGContextClipToRect(c, strips[i]);
        art_frame(c, body, glass, rim, frame);
        if (d->kind == BDEV_PHONE && !(d->hinge.w > 0.0f))
            art_bands(c, body, glass, rim, unit, frame);
        CGContextRestoreGState(c);
    }
    art_glass(c, glass, glass_c);
    for (i32 i = 0; i < ns; i++) {
        if (!CGRectIntersectsRect(strips[i], band)) continue;
        CGContextSaveGState(c);
        CGContextClipToRect(c, strips[i]);
        art_glass_edge(c, glass, glass_c, unit);
        CGContextRestoreGState(c);
    }

    if (art_meets(art_rect(&d->hinge, dpi), band)) {
        ArtRect caps[2] = { art_rect(&d->knuckles[0], dpi), art_rect(&d->knuckles[1], dpi) };
        art_hinge(c, art_rect(&d->hinge, dpi), caps, unit);
    }
    if (art_meets(art_rect(&d->speaker, dpi), band))
        art_speaker(c, art_rect(&d->speaker, dpi), glass_c, unit);
    if (art_meets(art_rect(&d->flash, dpi), band))
        art_flash(c, art_rect(&d->flash, dpi));
    if (art_meets(art_rect(&d->lens, dpi), band))
        art_lens(c, art_rect(&d->lens, dpi), glass_c);
    if (art_meets(art_rect(&d->home, dpi), band))
        art_home(c, art_rect(&d->home, dpi), frame, glass_c);
    if (d->kind == BDEV_LAPTOP && art_meets(art_rect(&d->base, dpi), band))
        art_deck(c, art_rect(&d->base, dpi), art_rect(&d->scoop, dpi), base_c, unit);
}

/* The whole pixel the device's extent starts in: parts are kept relative to
 * it, so a device moved by whole pixels (a pane resized around a device
 * shown at its own size) is not painted again. */
static void art_origin(const BrowserDeviceLayout *d, f32 dpi, i32 *ox, i32 *oy) {
    *ox = (i32)floorf(d->ext_x * dpi);
    *oy = (i32)floorf(d->ext_y * dpi);
}

/* What the paint depends on: every shape it reads in pixels from the
 * origin, the colours, the scale. Compared within a small tolerance, not
 * hashed: a layout moved by whole pixels comes out of the float arithmetic a
 * hair different, and a hash would tell it apart. */
#define ART_SIG 160
typedef struct { i32 n; f32 v[ART_SIG]; } ArtSig;

static void art_sig_of(const BrowserDeviceLayout *d, f32 dpi, ArtSig *g) {
    i32 ox, oy;
    art_origin(d, dpi, &ox, &oy);
    g->n = 0;
#define PUSH(f) do { if (g->n < ART_SIG) g->v[g->n++] = (f32)(f); } while (0)
    /* An absent part (w == 0) was never placed: it signs as nothing. */
#define PUSHS(s) do { bool on_ = (s).w > 0.0f; \
                      PUSH(on_ ? (s).x * dpi - (f32)ox : 0.0f); \
                      PUSH(on_ ? (s).y * dpi - (f32)oy : 0.0f); \
                      PUSH((s).w * dpi); PUSH((s).h * dpi); \
                      for (i32 q_ = 0; q_ < 4; q_++) PUSH((s).r[q_] * dpi); } while (0)
    PUSH(d->kind); PUSH(d->cut); PUSH(d->frame_rgb); PUSH(d->glass_rgb); PUSH(d->base_rgb);
    PUSH(dpi); PUSH(d->scale * dpi); PUSH(d->rim * dpi);
    PUSH(d->ext_x * dpi - (f32)ox); PUSH(d->ext_y * dpi - (f32)oy);
    PUSH(d->ext_w * dpi); PUSH(d->ext_h * dpi);
    PUSHS(d->body); PUSHS(d->glass); PUSHS(d->screen); PUSHS(d->lens); PUSHS(d->speaker);
    PUSHS(d->home); PUSHS(d->base); PUSHS(d->scoop); PUSHS(d->neck);
    PUSHS(d->flash); PUSHS(d->hinge); PUSHS(d->knuckles[0]); PUSHS(d->knuckles[1]);
    PUSH(d->n_keys);
    for (i32 i = 0; i < d->n_keys && i < BDEV_MAX_KEYS; i++) PUSHS(d->keys[i]);
#undef PUSHS
#undef PUSH
}

static bool art_sig_same(const ArtSig *a, const ArtSig *b) {
    if (a->n != b->n) return false;
    for (i32 i = 0; i < a->n; i++)
        if (!(fabsf(a->v[i] - b->v[i]) <= 1.0f / 256.0f)) return false;
    return true;
}

#define ART_CACHE 2
#define ART_MAX_DIM 8192
#define ART_MAX_PIXELS (24u * 1024u * 1024u)

typedef struct {
    bool  valid;
    ArtSig sig;
    u64   generation;   /* new for every paint */
    u64   used;
    i32   n;
    u8   *pixels[BROWSER_ART_PARTS];
    i32   x[BROWSER_ART_PARTS], y[BROWSER_ART_PARTS];   /* from the origin */
    i32   w[BROWSER_ART_PARTS], h[BROWSER_ART_PARTS];
} ArtEntry;
static ArtEntry g_art[ART_CACHE];
static u64 g_art_tick, g_art_serial;

static void art_entry_free(ArtEntry *e) {
    for (i32 i = 0; i < BROWSER_ART_PARTS; i++) {
        free(e->pixels[i]);
        e->pixels[i] = NULL;
    }
    e->valid = false;
    e->n = 0;
}

/* The bands that cover everything drawn outside a rectangle inside the
 * screen. The rectangle keeps clear of the screen's rounded corners: inset by
 * 0.3 of the largest radius, its corners are on or inside the arcs. */
static i32 art_bands_of(const BrowserDeviceLayout *d, f32 dpi, i32 x[], i32 y[], i32 w[], i32 h[]) {
    i32 x0 = (i32)floorf(d->ext_x * dpi) - 2, y0 = (i32)floorf(d->ext_y * dpi) - 2;
    i32 x1 = (i32)ceilf((d->ext_x + d->ext_w) * dpi) + 2;
    i32 y1 = (i32)ceilf((d->ext_y + d->ext_h) * dpi) + 2;
    f32 rmax = 0.0f;
    for (i32 i = 0; i < 4; i++) rmax = fmaxf(rmax, d->screen.r[i]);
    f32 in = (rmax * 0.30f + 2.0f) * dpi;
    i32 hx0 = (i32)ceilf(d->screen.x * dpi + in), hy0 = (i32)ceilf(d->screen.y * dpi + in);
    i32 hx1 = (i32)floorf((d->screen.x + d->screen.w) * dpi - in);
    i32 hy1 = (i32)floorf((d->screen.y + d->screen.h) * dpi - in);
    if (hx1 - hx0 < 4 || hy1 - hy0 < 4 || hx0 < x0 || hy0 < y0 || hx1 > x1 || hy1 > y1) {
        x[0] = x0; y[0] = y0; w[0] = x1 - x0; h[0] = y1 - y0;
        return 1;
    }
    i32 n = 0;
    const i32 bx[4] = { x0, x0, x0, hx1 }, by[4] = { y0, hy1, hy0, hy0 };
    const i32 bw[4] = { x1 - x0, x1 - x0, hx0 - x0, x1 - hx1 };
    const i32 bh[4] = { hy0 - y0, y1 - hy1, hy1 - hy0, hy1 - hy0 };
    for (i32 i = 0; i < 4; i++) {
        if (bw[i] <= 0 || bh[i] <= 0) continue;
        x[n] = bx[i]; y[n] = by[i]; w[n] = bw[i]; h[n] = bh[i];
        n++;
    }
    return n;
}

static bool art_render(const BrowserDeviceLayout *d, f32 dpi, ArtEntry *e) {
    i32 n = art_bands_of(d, dpi, e->x, e->y, e->w, e->h);
    u64 total = 0;
    for (i32 i = 0; i < n; i++) {
        if (e->w[i] > ART_MAX_DIM || e->h[i] > ART_MAX_DIM) return false;
        total += (u64)e->w[i] * (u64)e->h[i];
    }
    if (n <= 0 || total > ART_MAX_PIXELS) return false;
    for (i32 i = 0; i < n; i++) {
        usize stride = (usize)e->w[i] * 4;
        u8 *px = calloc((usize)e->h[i], stride);
        if (!px) return false;
        e->pixels[i] = px;
        CGContextRef c = CGBitmapContextCreate(px, (size_t)e->w[i], (size_t)e->h[i], 8, stride,
                                               art_space(),
                                               (CGBitmapInfo)kCGImageAlphaPremultipliedLast |
                                               kCGBitmapByteOrder32Big);
        if (!c) return false;
        CGContextSetShouldAntialias(c, true);
        CGContextSetAllowsAntialiasing(c, true);
        /* y down, the band's top-left at the origin. Rows are stored top
         * first, as the renderer reads them. */
        CGContextTranslateCTM(c, 0.0, (CGFloat)e->h[i]);
        CGContextScaleCTM(c, 1.0, -1.0);
        CGContextTranslateCTM(c, -(CGFloat)e->x[i], -(CGFloat)e->y[i]);
        art_paint(c, d, dpi, CGRectMake(e->x[i], e->y[i], e->w[i], e->h[i]));
        CGContextFlush(c);
        CGContextRelease(c);
    }
    i32 ox, oy;
    art_origin(d, dpi, &ox, &oy);
    for (i32 i = 0; i < n; i++) {
        e->x[i] -= ox;
        e->y[i] -= oy;
    }
    e->n = n;
    return true;
}

bool browser_native_device_art(const BrowserDeviceLayout *d, f32 dpi, BrowserDeviceArt *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!d || !d->on || !(dpi > 0.0f) || !isfinite(dpi) || !(d->body.w > 0.0f)) return false;
    ArtSig sig;
    art_sig_of(d, dpi, &sig);
    ArtEntry *e = NULL;
    for (i32 i = 0; i < ART_CACHE && !e; i++)
        if (g_art[i].valid && art_sig_same(&g_art[i].sig, &sig)) e = &g_art[i];
    if (!e) {
        e = &g_art[0];
        for (i32 i = 1; i < ART_CACHE; i++)
            if (!g_art[i].valid || (e->valid && g_art[i].used < e->used)) e = &g_art[i];
        art_entry_free(e);
        if (!art_render(d, dpi, e)) {
            art_entry_free(e);
            return false;
        }
        e->sig = sig;
        e->generation = ++g_art_serial;
        e->valid = true;
    }
    e->used = ++g_art_tick;
    i32 ox, oy;
    art_origin(d, dpi, &ox, &oy);
    out->generation = e->generation;
    out->n = e->n;
    for (i32 i = 0; i < e->n; i++) {
        out->part[i].pixels = e->pixels[i];
        out->part[i].x = e->x[i] + ox;
        out->part[i].y = e->y[i] + oy;
        out->part[i].w = e->w[i];
        out->part[i].h = e->h[i];
    }
    return true;
}
