// The first-run intro, whole: one fragment shader over a screen-sized triangle.
//
// Compiled from source at run time (intro_render_macos.m prepends the artwork's
// geometry as #defines), so the build needs no metallib step for it and a
// machine without the Metal toolchain still builds the app.
//
// Output is premultiplied alpha over a transparent layer: `alpha` is how much
// of the desktop is hidden, and light is added on top of it (rgb may exceed
// alpha — that is what lets the violet read on a bright desktop).
//
// The look: obsidian, one violet, and an 8x8 ordered (Bayer) dither in
// 4-point cells quantising everything soft into six tones — the dithered
// background, used here as the edge of every wave. Nothing is feathered; a
// front is a band of cells switching on in Bayer order.

#include <metal_stdlib>
using namespace metal;

struct IntroVertex { float4 position [[position]]; };

vertex IntroVertex liu_intro_vertex(uint vid [[vertex_id]]) {
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    IntroVertex out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

// Mirrors the array intro_render_macos.m fills.
struct IntroUniforms { float4 v[11]; };

// ---- Palette (sRGB) --------------------------------------------------------
constant float3 OBSIDIAN = float3(0.0392, 0.0392, 0.0431);   // #0A0A0B  ink-950
constant float3 GRAPHITE = float3(0.0863, 0.0863, 0.0941);   // #161618  ink-900
constant float3 PAPER    = float3(0.9686, 0.9686, 0.9725);   // #F7F7F8  ink-50
constant float3 VIOLET   = float3(0.6353, 0.5294, 1.0000);   // #A287FF  violet-500
constant float3 VIOLET_PALE = float3(0.7765, 0.7333, 0.9961); // #C6BBFE violet-300
// The top of the dither ramp (#211A41), as light added over obsidian.
constant float3 HAZE     = float3(0.0902, 0.0627, 0.2118);
// The mark's own two colours. A logo is not recoloured.
constant float3 MARK_FILL = float3(0.7922, 0.4941, 0.9882);  // #CA7EFC
constant float3 MARK_INK  = float3(0.2314, 0.0627, 0.3569);  // #3B105B

// Points per dither cell.
constant float CELL = 4.0;

static inline float cover(float d, float aa) { return clamp(0.5 - d / aa, 0.0, 1.0); }

static inline float hash21(float2 p) {
    p = fract(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

static inline float vnoise(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + float2(1.0, 0.0)), f.x),
               mix(hash21(i + float2(0.0, 1.0)), hash21(i + float2(1.0, 1.0)), f.x), f.y);
}

static float fbm(float2 p) {
    float s = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; i++) {
        s += a * vnoise(p);
        p = p * 2.03 + 17.1;
        a *= 0.5;
    }
    return s;
}

// The site's Bayer matrix, built the same way.
static inline float bayer2(float2 a) { a = floor(a); return fract(a.x * 0.5 + a.y * a.y * 0.75); }
static inline float bayer4(float2 a) { return bayer2(0.5 * a) * 0.25 + bayer2(a); }
static inline float bayer8(float2 a) { return bayer4(0.5 * a) * 0.25 + bayer2(a); }

// Six tones, 0..5: `v` is the soft value, `b` this cell's threshold. 0
// stays 0 and 1 stays 1, so a finished wave is solid.
static inline float quant(float v, float b) {
    return clamp(floor(v * 5.0 + b), 0.0, 5.0) / 5.0;
}

// MotionBackground.vue's `blob`: a soft elliptical falloff.
static inline float blob(float2 p, float2 c, float2 s, float k) {
    float2 d = (p - c) * s;
    return exp(-dot(d, d) * k);
}

static inline float roundBox(float2 p, float2 half_size, float radius) {
    float2 q = abs(p) - half_size + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

#define OVER(c, a) { float a_ = (a); col = (c) * a_ + col * (1.0 - a_); alpha = a_ + alpha * (1.0 - a_); }

fragment float4 liu_intro_fragment(IntroVertex in [[stage_in]],
                                   constant IntroUniforms &u [[buffer(0)]],
                                   texture2d<float> bunnyTex [[texture(0)]],
                                   texture2d<float> capTex [[texture(1)]]) {
    // clamp_to_zero: a blurred (coarse-mip) sample taken outside the artwork
    // must fade to nothing, not smear the edge texel across the screen.
    constexpr sampler tri(filter::linear, mip_filter::linear, address::clamp_to_zero);

    float2 res = u.v[0].xy;        float scale = u.v[0].z;       float time = u.v[0].w;
    float2 C = u.v[1].xy;          float unit = u.v[1].z;        float dim = u.v[1].w;
    float cursor = u.v[2].x;       float prompt = u.v[2].y;
    float zoom = u.v[2].z;         float camera = u.v[2].w;
    float flood = u.v[3].x;        float glow = u.v[3].y;
    float haze = u.v[3].z;         float pulse = u.v[3].w;
    float spark = u.v[4].y;
    float dissolve = u.v[4].z;     float morph = u.v[4].w;
    float frameAlpha = u.v[5].x;   float reveal = u.v[5].y;
    float fade = u.v[5].z;         float targetR = u.v[5].w;
    float2 targetC = u.v[6].xy;    float2 targetH = u.v[6].zw;
    float wholeWindow = u.v[8].x;  float dimStrength = u.v[8].y;
    float wipes = u.v[8].z;        float markAlpha = u.v[8].w;
    float uncover = u.v[9].x;      float plate = u.v[9].y;
    float feature = u.v[9].z;      float caption = u.v[9].w;
    float captionRise = u.v[10].x; float hint = u.v[10].y;

    float2 p = in.position.xy / scale;       // points, y down
    float px = 1.0 / scale;                  // one pixel, in points
    float2 view = res / scale;
    float halfDiag = 0.5 * length(view);

    // The dither cell this pixel is in, and its centre. Every soft value below
    // is evaluated at the centre, so a cell is one flat square.
    float2 cellId = floor(p / CELL);
    float2 pc = (cellId + 0.5) * CELL;
    // Cells are numbered with their origin at the bottom-left, as
    // gl_FragCoord numbers pixels, so the Bayer matrix and the field land on
    // the same cells as a one-pixel-per-cell rendering of the same pattern.
    float2 cells = ceil(view / CELL);
    float2 cellGL = float2(cellId.x, cells.y - 1.0 - cellId.y);
    float b8 = bayer8(cellGL);

    float2 rel = p - C;
    float2 relc = pc - C;
    float strength = min(dimStrength, 1.0);

    // A caption cell, in points, and the mip level that draws it texel for
    // pixel (level 0 on a 2x display, 1 on a 1x one).
    float2 capSize = float2(CAPTION_CELL_W, CAPTION_CELL_H) / CAPTION_SCALE;
    float capLod = max(log2(CAPTION_SCALE / scale), 0.0);
    float capCells = float(CAPTION_COUNT + CAPTION_HINTS);

    // ---- Backdrop: the desktop goes to obsidian behind the intro and comes
    // back after it. The dark arrives as a wave spreading outward from the
    // middle of the screen and lifts as a wave spreading outward from the
    // window's edges (from its middle, most of the wave would pass unseen under
    // the window itself). Each front is a dithered band with a thin violet rim.
    // It is kept apart from everything drawn over it (`dimA`), because that
    // fades at the end while the dark leaves by its own wave.
    float feather = 0.24 * halfDiag;
    float reachIn = length(max(C, view - C)) + feather;
    float rIn = dim * reachIn;
    float rc = length(relc);
    float spread = quant(1.0 - smoothstep(rIn - feather, rIn, rc), b8);

    float dTarget = roundBox(p - targetC, targetH, targetR);
    float rT = max(roundBox(pc - targetC, targetH, targetR), 0.0);
    float reachOut = length(max(max(targetC, view - targetC) - targetH, 0.0)) + feather;
    float rOut = (1.0 - fade) * reachOut;
    // Inside the window the dark is not the wave's to lift: it clears there
    // exactly as the window comes in (`keep`, below). On a skip the wave would
    // otherwise bare the desktop under a half-faded window.
    float inTarget = cover(dTarget, px);
    float held = mix(quant(smoothstep(rOut - feather, rOut, rT), b8), 1.0, inTarget);

    // Nearly all of the desktop hidden, a little more toward the corners. The
    // tones of the field are only a few levels apart, so what leaks through
    // the dark has to be smaller than that or the cells stop being flat
    // squares of the palette: with less dark a white window shows as a grey
    // box and terminal text behind the overlay prints through the texture.
    // Clicks still pass through — the desktop is covered, not taken.
    float depth = dimStrength * (0.95 + 0.03 * smoothstep(0.20, 1.05, length(rel) / halfDiag));
    float dimA = depth * mix(dim * fade, spread * held, wipes);

    float rimWidth = 0.075 * halfDiag;
    float xIn = (rc - (rIn - 0.5 * feather)) / rimWidth;
    float xOut = (rT - (rOut - 0.5 * feather)) / rimWidth;
    float rimV = exp(-xIn * xIn) * sin(3.14159265 * dim)
               + exp(-xOut * xOut) * sin(3.14159265 * fade);
    float3 rim = VIOLET * quant(clamp(rimV, 0.0, 1.0), b8) * 0.11 * wipes * strength;

    // ---- The mark, in the pixels of its file.
    float U = (MARK_Y1 - MARK_Y0) * unit;                 // its height, in points
    float s = max(unit * zoom, 1e-4);                     // points per file pixel
    float2 FACE_C = float2(FACE_CX, FACE_CY);
    float2 MARK_C = 0.5 * float2(MARK_X0 + MARK_X1, MARK_Y0 + MARK_Y1);
    float2 anchor = mix(FACE_C, MARK_C, camera);
    float2 q = rel / s + anchor;
    float2 qc = relc / s + anchor;
    float2 uv = q / float2(BUNNY_W, BUNNY_H);
    // Mip level for one screen pixel, pulled a touch sharp.
    float lod = max(log2(1.0 / (s * scale)) - 0.35, 0.0);

    float3 layers = bunnyTex.sample(tri, uv, level(lod)).rgb;
    float sil = layers.r;
    float ink = min(layers.g, sil);
    float isEye = 1.0 - smoothstep(FACE_SPLIT_X - 8.0, FACE_SPLIT_X + 8.0, q.x);
    float eye = layers.b * isEye;
    float mouth = layers.b * (1.0 - isEye);

    // The body fills in outward from the face, and at the end drains back
    // toward the cursor — the far parts first, the cursor's surroundings last.
    float reach = length(qc - FACE_C);
    float band = 0.34 * FLOOD_REACH;
    float front = flood * (FLOOD_REACH + band);
    float filled = quant(1.0 - smoothstep(front - band, front, reach), b8);
    float2 MOUTH_C = 0.5 * float2(MOUTH_X0 + MOUTH_X1, MOUTH_Y0 + MOUTH_Y1);
    float away = clamp(length(qc - MOUTH_C) / FLOOD_REACH, 0.0, 1.0);
    float gone = quant(clamp(dissolve * 1.8 - 0.8 * (1.0 - away), 0.0, 1.0), b8);
    float body = markAlpha * filled * (1.0 - gone);
    float present = markAlpha * flood * (1.0 - dissolve);    // the same, unquantised

    // Blurred copies of the artwork, from its coarser mips: light around it.
    float3 near = bunnyTex.sample(tri, uv, level(lod + 3.2)).rgb;
    float3 far = bunnyTex.sample(tri, uv, level(lod + 5.2)).rgb;
    // Interpolate prompt and cursor colors across the face split with a
    // 60-pixel smooth transition, for both original and quantized
    // coordinates.
    float faceLit = mix(prompt, cursor, smoothstep(FACE_SPLIT_X - 30.0, FACE_SPLIT_X + 30.0, q.x));
    float faceLitC = mix(prompt, cursor, smoothstep(FACE_SPLIT_X - 30.0, FACE_SPLIT_X + 30.0, qc.x));

    // ---- The field behind everything: the dithered background, evaluated
    // per cell as a one-pixel-per-cell canvas scaled up — a warped fbm, two
    // glows off toward opposite corners, intensity 0.6, a fixed start time
    // and drift, six tones. The settled intro's backdrop is the field's first
    // frame, whole-screen and quiet.
    float siteV = 0.0;
    if (haze > 0.001) {
        float asp = cells.x / cells.y;
        float2 sp = ((cellGL + 0.5) / cells - 0.5) * float2(asp, 1.0);
        float st = 40.0 + time * 0.5;
        float2 w = float2(fbm(sp * 1.6 + float2(0.0, st * 0.05)),
                          fbm(sp * 1.6 + float2(5.2, -st * 0.04)));
        float f = fbm(sp * 1.3 + 1.4 * (w - 0.5) + float2(st * 0.03, -st * 0.02));
        float corners = max(blob(sp, float2(0.30 * asp, 0.30), float2(0.8, 1.1), 2.2),
                            0.85 * blob(sp, float2(-0.32 * asp, -0.34), float2(1.0, 1.2), 2.6));
        siteV = smoothstep(0.34, 0.76, f) * (0.40 + 0.60 * corners) * 0.6 * haze;
    }

    // What the mark adds to it is more of the same thing — a lift in the same
    // six tones, not a glow laid over them: the light it gives off, the light
    // of the bare prompt before there is a body, and the ring as it locks.
    float3 nearC = bunnyTex.sample(tri, qc / float2(BUNNY_W, BUNNY_H), level(lod + 3.2)).rgb;
    float3 farC = bunnyTex.sample(tri, qc / float2(BUNNY_W, BUNNY_H), level(lod + 5.2)).rgb;
    float markLight = (0.30 * nearC.r + 0.45 * farC.r) * glow * present;
    float faceLight = (0.9 * nearC.b + 1.3 * farC.b) * faceLitC * (1.0 - flood) * markAlpha;
    float ring = 0.0;
    if (pulse >= 0.0) {
        float e = 1.0 - pow(1.0 - pulse, 3.0);
        float x = (rc - U * (0.42 + 2.5 * e)) / (U * (0.20 + 0.55 * pulse));
        ring = exp(-x * x) * (1.0 - pulse) * (1.0 - pulse) * 0.75 * markAlpha;
    }
    float field = clamp(siteV + markLight + faceLight + ring, 0.0, 1.0);
    float3 light = HAZE * quant(field, b8) * spread;
    // A little undithered accent right at the mark's edge, so it does not sit
    // on the texture like a sticker.
    light += VIOLET * (0.04 * near.r + 0.03 * far.r) * glow * present * (1.0 - sil);
    light += VIOLET * (0.35 * near.b) * faceLit * (1.0 - flood) * markAlpha;

    // The dark is a shade deeper right around the mark — a shadow under it,
    // with no edge of its own to be seen as a shape.
    dimA += (1.0 - dimA) * 0.45 * far.r * present * dim * strength;

    float3 col = light;
    float alpha = 0.0;

    // The mark: fill and ink, exactly as drawn.
    {
        float a = sil * body;
        float3 premul = (MARK_FILL * (sil - ink) + MARK_INK * ink) * body;
        col = premul + col * (1.0 - a);
        alpha = a + alpha * (1.0 - a);
    }

    // The face on its own, in the terminal's colours — the `>` in its text
    // colour, the `_` in its cursor colour. Before the body arrives it is all
    // there is; at the end the `_` lights up again and outlives the mark.
    float toFrame = smoothstep(0.0, 0.004, morph);
    float bare = (1.0 - filled) * markAlpha;
    OVER(PAPER, eye * prompt * bare);
    OVER(VIOLET, clamp(mouth * (cursor * bare + spark * (1.0 - toFrame)), 0.0, 1.0));
    col += VIOLET * 0.55 * near.b * (1.0 - isEye) * spark * (1.0 - toFrame) * (1.0 - mouth);

    // ---- Handoff: the cursor leaves the mark and grows into the window. It
    // starts as the `_` itself — a filled, fully rounded bar — swells into a
    // block cursor, and as that opens out its violet drains to the terminal's
    // own surface inside a hairline.
    float2 barC = C + (MOUTH_C - MARK_C) * unit;
    float2 barH = 0.5 * float2(MOUTH_X1 - MOUTH_X0, MOUTH_Y1 - MOUTH_Y0) * unit;
    float2 fc = mix(barC, targetC, morph);
    float2 fh = mix(barH, targetH, morph);
    float fr = min(mix(barH.y, targetR, smoothstep(0.0, 0.55, morph)), min(fh.x, fh.y));
    float fth = mix(2.0 * barH.y, 1.5, smoothstep(0.0, 0.30, morph));
    float dF = roundBox(p - fc, fh, fr);
    float inF = cover(dF, px);

    // "press any key to skip" (in the reader's language), small, at the foot
    // of the screen. Drawn before the window is cut out of the overlay, so it
    // never sits on the window.
    if (hint > 0.001) {
        float2 hc = floor(float2(view.x * 0.5, view.y - 36.0) * scale + 0.5) / scale;
        float2 huv = (p - (hc - 0.5 * capSize)) / capSize;
        if (huv.x >= 0.0 && huv.x <= 1.0 && huv.y >= 0.0 && huv.y <= 1.0) {
            float g = capTex.sample(tri, float2(huv.x, (float(CAPTION_COUNT) + huv.y) / capCells),
                                    level(capLod)).g;
            OVER(PAPER, g * 0.40 * hint);
        }
    }

    // The overlay uncovers the window through the frame — or, when there is
    // no frame to grow, through the window's own rectangle. `uncover` trails
    // `reveal` in the full intro: the window is brought to full opacity UNDER
    // the frame's obsidian first, so the desktop is never seen through a
    // half-faded window, and only then does the overlay clear — cell by cell.
    // (Dithered only in the full intro. Wherever the whole rectangle is in
    // play — reduce motion, a skip — it is a plain cross-fade: quantising a
    // skip's first frames drew the window's outline in dots.)
    float lifted = wholeWindow > 0.0 ? uncover : mix(uncover, quant(uncover, b8), wipes);
    float keep = 1.0 - lifted * mix(inF, inTarget, wholeWindow);
    col *= keep;
    alpha *= keep;
    dimA *= keep;
    rim *= keep;

    float frameA = frameAlpha * toFrame;
    col += VIOLET * exp(-max(dF, 0.0) / 22.0) * (1.0 - inF) * 0.24 * frameA;
    // Inside it: violet while it is still a cursor, then the terminal's own
    // surface. The default theme's background is this same obsidian, so when
    // the overlay clears, only the window's contents appear to arrive.
    float opened = smoothstep(0.02, 0.30, morph);
    OVER(mix(VIOLET, OBSIDIAN, opened), inF * frameA * mix(1.0, 0.94, opened) * keep);

    OVER(mix(VIOLET, VIOLET_PALE, morph), inF * (1.0 - cover(dF + fth, px)) * frameA * mix(1.0, 0.80, morph));

    // ---- The tour's caption. By now what is inside the frame is the real
    // window, doing the real thing; all that is drawn over it is a plate at
    // its foot naming the feature — title, one line, the keys being pressed —
    // and a dot per feature. (Not multiplied by `keep`: this is the one thing
    // the overlay still draws INSIDE the window.)
    float pa = plate * frameA * inF;
    if (pa > 0.001) {
        float moves = wipes;
        float2 fo = targetC - targetH;            // the window's top-left corner
        float2 fp = p - fo;                       // this pixel, in the window
        float2 fs = 2.0 * targetH;
        float2 plH = 0.5 * capSize + float2(26.0, 22.0);
        // On a pixel boundary: the caption is drawn texel for pixel.
        float2 plC = floor((fo + float2(fs.x * 0.5, fs.y - 30.0 - plH.y)) * scale + 0.5) / scale - fo;
        float dPl = roundBox(fp - plC, plH, 18.0);
        float inPl = cover(dPl, px);
        OVER(float3(0.0), exp(-max(dPl, 0.0) / 18.0) * 0.50 * (1.0 - inPl) * pa);
        OVER(GRAPHITE, inPl * 0.97 * pa);
        OVER(float3(1.0), inPl * (1.0 - cover(dPl + 1.0, px)) * 0.12 * pa);
        float2 cuv = (fp - (plC - float2(0.5 * capSize.x, plH.y - 14.0))
                      - float2(0.0, (1.0 - captionRise) * 10.0 * moves)) / capSize;
        if (cuv.x >= 0.0 && cuv.x <= 1.0 && cuv.y >= 0.0 && cuv.y <= 1.0) {
            float3 cap = capTex.sample(tri, float2(cuv.x, (feature + cuv.y) / capCells),
                                       level(capLod)).rgb;
            // Key chips: border 0.11, body half that.
            OVER(float3(1.0), cap.b * 0.11 * caption * pa);
            OVER(PAPER, clamp(cap.r + 0.65 * cap.g, 0.0, 1.0) * caption * pa);
        }
        for (int i = 0; i < CAPTION_COUNT; i++) {
            float2 c = float2(plC.x + (float(i) - 0.5 * float(CAPTION_COUNT - 1)) * 12.0,
                              plC.y + plH.y - 12.0);
            float on = 1.0 - clamp(abs(feature - float(i)), 0.0, 1.0);
            OVER(mix(PAPER, VIOLET, on), cover(length(fp - c) - 2.5, px) * mix(0.22, 1.0, on) * pa);
        }
    }

    // What was drawn fades out; the dark under it leaves by its wave.
    col = col * fade + rim;
    alpha *= fade;
    // The dark is obsidian, not black.
    col += OBSIDIAN * dimA * (1.0 - alpha);
    alpha = alpha + dimA * (1.0 - alpha);

    // Film grain: white specks at a very low random alpha, device-pixel
    // fine, and still — grain that moves reads as flicker.
    float2 dev = floor(in.position.xy);
    float speck = step(0.5, hash21(dev)) * hash21(dev + 31.7) * 0.030;
    col += speck * dimA;

    return float4(max(col, 0.0), clamp(alpha, 0.0, 1.0));
}
