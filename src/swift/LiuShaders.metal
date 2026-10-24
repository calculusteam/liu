/*
 * Liu — Metal shaders for terminal rendering
 * Two pipelines: instanced glyphs + batched rects
 */

#include <metal_stdlib>
using namespace metal;

/* =========================================================================
 * Shared types
 * ========================================================================= */

struct Uniforms {
    float4x4 projection;
    float2   cell_size;
    float    text_gamma;     /* coverage gamma; 1.0 = passthrough */
    float    min_contrast;   /* WCAG floor; <= 1.0 = off */
    float    blend_correct;  /* 0 = off, 1 = luminance-corrected coverage */
    float    text_weight;    /* manual weight, -2..+2: erode/dilate the mask; 0 = untouched */
    float    _pad[2];        /* to 96 B — must match MetalUniforms in renderer_metal.m */
};

/* sRGB <-> linear, the exact IEC 61966-2-1 curve. Kept identical to
 * srgb_decode() in renderer.h: rects cross the boundary on the CPU and
 * glyphs cross it here, and the two must agree or a glyph and the cell
 * behind it disagree about what "the same colour" means. */
static inline float srgb_to_linear1(float c) {
    return (c <= 0.04045f) ? (c * (1.0f / 12.92f))
                           : pow((c + 0.055f) * (1.0f / 1.055f), 2.4f);
}
static inline float3 srgb_to_linear3(float3 c) {
    return float3(srgb_to_linear1(c.r), srgb_to_linear1(c.g), srgb_to_linear1(c.b));
}
static inline float linear_to_srgb1(float c) {
    return (c <= 0.0031308f) ? (c * 12.92f)
                             : (1.055f * pow(c, 1.0f / 2.4f) - 0.055f);
}
static inline float luminance(float3 lin) {
    return dot(lin, float3(0.2126f, 0.7152f, 0.0722f));
}

/* =========================================================================
 * Glyph pipeline — instanced rendering
 * One draw call for ALL visible glyphs
 * ========================================================================= */

/* 36 bytes; must match GlyphInstance in renderer.h field for field. Colours
 * arrive as 8-bit sRGB and are decoded below — see the struct's comment there
 * for why the decode moved off the CPU. */
struct GlyphInstance {
    packed_float2 pos;        // quad top-left in framebuffer pixels
    packed_float4 rect;       // ax0, ay0, ax1, ay1 — TEXELS for kinds 0/1, 0..1 for kind 2
    uchar4        fg;         // foreground sRGB + per-instance alpha
    uchar4        bg;         // background sRGB + "known" flag in .a
    uchar         tex_kind;   // 0 alpha, 1 colour, 2 image (normalised rect), 3 shared seed
    uchar         w_cells;    // width in cells: 1 normal, 2+ wide / ligature
    uchar2        _pad;
};

/* Texel rects are what the CPU lays glyphs out in, so an instance built
 * against one atlas generation stays right when the frame binds a bigger
 * one that kept the old pixels in place (a grow). The division happens here,
 * against the texture actually bound to THIS frame — which is why the vertex
 * stage takes both atlases. The IMAGE kind carries a normalised rect and is
 * left alone: an image texture is not an atlas and has no shared size. */
constant uchar GLYPH_TEX_ALPHA = 0;
constant uchar GLYPH_TEX_COLOR = 1;
constant uchar GLYPH_TEX_IMAGE = 2;
constant uchar GLYPH_TEX_SEED = 3;

/* The C side asserts the same three numbers (renderer.h). Both halves are
 * built from this repo by the same CMake run, so pinning them here is what
 * makes a field added to one and forgotten in the other a BUILD failure
 * instead of text in the wrong colour. */
static_assert(sizeof(GlyphInstance) == 36, "GlyphInstance must match renderer.h");
static_assert(sizeof(Uniforms) == 96, "Uniforms must match MetalUniforms in renderer_metal.m");

/* All vertices of one glyph share its style and atlas selector. Only position
 * and UV vary across the quad; do not interpolate per-instance constants. */
struct GlyphOut {
    float4 position [[position]];
    float2 uv;
    float3 color [[flat]];             // linear, min-contrast already applied
    float  is_color [[flat]];
    float  alpha [[flat]];             // forwarded from instance
    float  gamma [[flat]];             // coverage gamma, forwarded from uniforms
    float3 bg [[flat]];                // linear background, or 0 when unknown
    float  bg_known [[flat]];          // 0 = no background travelled with this glyph
    float  blend_correct [[flat]];
    float  weight [[flat]];            // manual coverage trim, forwarded from uniforms
};

vertex GlyphOut glyph_vertex(
    uint vid [[vertex_id]],
    uint iid [[instance_id]],
    constant float2 *quad [[buffer(0)]],
    constant GlyphInstance *instances [[buffer(1)]],
    constant Uniforms &uniforms [[buffer(2)]],
    texture2d<float> atlas [[texture(0)]],
    texture2d<float> color_atlas [[texture(1)]],
    texture2d<float> seed_atlas [[texture(2)]]
) {
    GlyphOut out;
    float2 q = quad[vid];
    float2 pos = float2(instances[iid].pos);
    float4 rect = float4(instances[iid].rect);
    uchar kind = instances[iid].tex_kind;
    float w = float(instances[iid].w_cells);
    if (w < 0.5) w = 1.0;  /* safety: treat 0 as 1 cell */
    float2 sz = float2(uniforms.cell_size.x * w, uniforms.cell_size.y);
    float2 px = pos + q * sz;
    out.position = uniforms.projection * float4(px, 0.0, 1.0);
    if (kind == GLYPH_TEX_ALPHA) {
        float2 inv = 1.0f / float2(float(atlas.get_width()), float(atlas.get_height()));
        rect *= inv.xyxy;
    } else if (kind == GLYPH_TEX_COLOR) {
        float2 inv = 1.0f / float2(float(color_atlas.get_width()), float(color_atlas.get_height()));
        rect *= inv.xyxy;
    } else if (kind == GLYPH_TEX_SEED) {
        float2 inv = 1.0f / float2(float(seed_atlas.get_width()), float(seed_atlas.get_height()));
        rect *= inv.xyxy;
    }
    out.uv = mix(rect.xy, rect.zw, q);
    float3 fg = srgb_to_linear3(float3(instances[iid].fg.rgb) * (1.0f / 255.0f));
    float3 bg = srgb_to_linear3(float3(instances[iid].bg.rgb) * (1.0f / 255.0f));
    float bg_known = instances[iid].bg.a > 0 ? 1.0f : 0.0f;

    /* Minimum contrast. A theme (or a program picking its own colours) can put
     * a foreground on a background that is simply not readable; this is the
     * floor below which the glyph is forced to black or white, whichever is
     * further from what it sits on. WCAG's ratio, which is why the 0.05 offsets
     * are there. Off by default and skipped entirely for a glyph whose
     * background never travelled with it. */
    if (uniforms.min_contrast > 1.0f && bg_known > 0.5f && (kind == GLYPH_TEX_ALPHA || kind == GLYPH_TEX_SEED)) {
        float lf = luminance(fg), lb = luminance(bg);
        float ratio = (max(lf, lb) + 0.05f) / (min(lf, lb) + 0.05f);
        if (ratio < uniforms.min_contrast) {
            float white_ratio = 1.05f / (lb + 0.05f);
            float black_ratio = (lb + 0.05f) / 0.05f;
            fg = (white_ratio > black_ratio) ? float3(1.0f) : float3(0.0f);
        }
    }

    out.color = fg;
    out.bg = bg;
    out.bg_known = bg_known;
    out.blend_correct = uniforms.blend_correct;
    out.is_color = kind == GLYPH_TEX_SEED ? 2.0f : (kind == GLYPH_TEX_ALPHA ? 0.0f : 1.0f);
    out.alpha = float(instances[iid].fg.a) * (1.0f / 255.0f);
    out.gamma = uniforms.text_gamma;
    out.weight = uniforms.text_weight;
    return out;
}

/* The manual weight (Settings > Appearance > Text Weight): a real erode / dilate
 * of the RAW coverage mask, not a tone curve.
 *
 * A curve on the coverage (such as cov^(2^-w)) can only move the antialiased
 * edge pixels - an empty pixel stays empty and a full one stays full - so on
 * thick retina stems it barely changes the ink. Weight is geometry, so this
 * reads the neighbours.
 *
 *   w > 0  dilate: a pixel takes d * (its brightest neighbour), d = 0.42 w
 *          (capped 0.84), the diagonals counting 0.7 so a corner rounds
 *          instead of cutting. An edge moves out by about d pixels.
 *   w < 0  erode: a pixel loses e * (how much brighter it is than its dimmest
 *          cardinal neighbour), e = 0.25 |w| (capped 0.5). An edge moves in by
 *          about e pixels.
 *
 * Interior pixels (all neighbours as bright as they are) and empty space are
 * fixed points of both, w == 0 skips the call, and it runs BEFORE the gamma and
 * blend correction because those are tone maps over the geometry. Neighbours
 * are one texel away; the atlas keeps 1 px of padding between glyphs, so an
 * edge never reads a neighbouring glyph. level(0): no implicit derivatives
 * inside a branch. Output pixels outside the glyph's own quad are not drawn,
 * which a cell-sized terminal glyph with its side bearings never reaches. */
static float weight_morph(texture2d<float> mask, sampler s, float2 uv, float cov, float w) {
    float2 t = float2(1.0f / float(mask.get_width()), 1.0f / float(mask.get_height()));
    float l = mask.sample(s, uv + float2(-t.x, 0.0f), level(0)).r;
    float r = mask.sample(s, uv + float2( t.x, 0.0f), level(0)).r;
    float u = mask.sample(s, uv + float2(0.0f, -t.y), level(0)).r;
    float d = mask.sample(s, uv + float2(0.0f,  t.y), level(0)).r;
    if (w > 0.0f) {
        float ul = mask.sample(s, uv + float2(-t.x, -t.y), level(0)).r;
        float ur = mask.sample(s, uv + float2( t.x, -t.y), level(0)).r;
        float dl = mask.sample(s, uv + float2(-t.x,  t.y), level(0)).r;
        float dr = mask.sample(s, uv + float2( t.x,  t.y), level(0)).r;
        float nb = max(max(max(l, r), max(u, d)), 0.7f * max(max(ul, ur), max(dl, dr)));
        return max(cov, min(w * 0.42f, 0.84f) * nb);
    }
    float m = min(min(l, r), min(u, d));
    return cov - min(-w * 0.25f, 0.5f) * max(0.0f, cov - m);
}

/* Single premultiplied output. Pipeline blend is (ONE, OneMinusSrcAlpha):
 *   out.rgb = fg.rgb * cov + dst.rgb * (1 - cov)
 *   out.a   = cov + dst.a * (1 - cov)
 * Atlas stores grayscale coverage (R8) so a single alpha drives all channels.
 * Foreground RGB arrives linear from the CPU side; framebuffer is sRGB and
 * Metal encodes on write — compositing in linear is gamma-correct. */
fragment float4 glyph_fragment(
    GlyphOut in [[stage_in]],
    /* Individual texture arguments at the existing alpha/colour/seed slots.
     * Index the resource before sampling so coverage needs one texture sample. */
    array<texture2d<float>, 3> atlases [[texture(0)]],
    sampler samp [[sampler(0)]],
    /* A SECOND sampler for the colour atlas, always bilinear.
     *
     * samp is switched per batch between nearest (terminal glyphs, pixel-exact
     * by construction) and linear (UI chrome, magnified text), and the colour
     * atlas must not ride whichever the batch gets. The OpenGL backend gives
     * the same atlas its own bilinear texture filter: one atlas, one sampling
     * contract, stated in both backends. */
    sampler color_samp [[sampler(1)]]
) {
    float a = in.alpha;
    if (a <= 0.0) discard_fragment();
    if (in.is_color > 0.5 && in.is_color < 1.5) {
        /* fg multiplies the texel so one uploaded image can be drawn in many
         * colours. Full-colour images (emoji, PNGs, GIF frames) pass fg =
         * (1,1,1) and are unaffected; white premultiplied masks (the Lucide
         * icon pipeline) use it to tint at draw time, which is what keeps a
         * single texture per (icon, size) — recolouring via a re-upload
         * rewrote the shared texture mid-frame and silently repainted every
         * already-encoded draw of that icon. */
        float4 c = atlases[1].sample(color_samp, in.uv);
        float ca = c.a * a;
        if (ca < 0.004) discard_fragment();
        return float4(c.rgb * in.color * a, ca);
    }
    texture2d<float> mask = atlases[in.is_color > 1.5 ? 2u : 0u];
    float cov = mask.sample(samp, in.uv).r;
    /* Weight first, on the raw mask (see weight_morph) - and BEFORE the empty
     * test, because a dilated pixel can be lit by a neighbour while its own
     * coverage is zero. */
    if (in.weight != 0.0f) cov = weight_morph(mask, samp, in.uv, cov, in.weight);
    if (cov * a < 0.004) discard_fragment();
    /* Stem darkening for rasterisers that emit pure geometric coverage: linear
     * coverage looks skeletal next to CoreText/AppKit output, and a 1/gamma
     * curve on the mask thickens thin strokes back to native weight while
     * keeping the math linear-light (the boost happens *before* the linear
     * fg*cov composite that the sRGB framebuffer encodes on write).
     *
     * gamma arrives from the CPU (font_text_gamma) and is 1.0 whenever Core
     * Graphics font smoothing already dilated the outline — thickening twice
     * is what made text read as a soft grey halo instead of solid stems.
     * Applied to the RAW coverage mask; per-instance alpha then scales the
     * result linearly, so modal fades (a<1) stay perceptually linear instead
     * of being warped by the stem-darkening curve. */
    /* A glyph the blend correction below applies to skips it: the correction
     * already lands on the familiar sRGB-space weight from the RAW coverage, and
     * darkening the stems first would make light-on-dark text too heavy. */
    bool blend_corrected = in.blend_correct > 0.5 && in.bg_known > 0.5;
    if (!blend_corrected && in.gamma > 1.001) cov = pow(cov, 1.0 / in.gamma);

    /* Linear-blending weight correction.
     *
     * Compositing in linear light is the physically correct thing to do, and it does not
     * look like what people expect: at half coverage a light glyph on a dark cell lands at
     * 0.664 sRGB where the gamma-incorrect blend everyone is trained on gives 0.500. So
     * correct blending renders light-on-dark HEAVIER and dark-on-light LIGHTER than the
     * familiar result.
     *
     * This keeps the linear composite and corrects the COVERAGE instead: blend the two
     * luminances the wrong way on purpose, then solve for the alpha that lands on that
     * result in linear space. Needs the background, so it is inert for every glyph that did
     * not carry one. On by default: without it, linear blending alone draws light-on-dark
     * terminal text visibly heavier than the familiar result. LIU_TEXT_BLEND=linear turns
     * it off. */
    if (blend_corrected) {
        float fg_l = luminance(in.color);
        float bg_l = luminance(in.bg);
        /* Guard the divide: within a hair of equal luminance the mapping below
         * is numerically meaningless and the correction is a no-op anyway. */
        if (abs(fg_l - bg_l) > 0.001) {
            float blended = linear_to_srgb1(fg_l) * cov +
                            linear_to_srgb1(bg_l) * (1.0 - cov);
            float target = srgb_to_linear1(blended);
            cov = clamp((target - bg_l) / (fg_l - bg_l), 0.0, 1.0);
        }
    }

    cov *= a;
    return float4(in.color * cov, cov);
}

/* =========================================================================
 * Rect pipeline — instanced colored quads
 * One draw call for all batched rects; each instance provides pos/size/color.
 * ========================================================================= */

struct RectInstance {
    packed_float2 pos;    /* top-left pixel */
    packed_float2 size;   /* width, height */
    packed_float4 color;  /* rgba */
};

struct RectOut {
    float4 position [[position]];
    float4 color;
};

vertex RectOut rect_vertex(
    uint vid [[vertex_id]],
    uint iid [[instance_id]],
    constant float2 *quad [[buffer(0)]],
    constant RectInstance *instances [[buffer(1)]],
    constant Uniforms &uniforms [[buffer(2)]]
) {
    RectOut out;
    float2 q = quad[vid];
    float2 pos = float2(instances[iid].pos);
    float2 sz  = float2(instances[iid].size);
    float2 px = pos + q * sz;
    out.position = uniforms.projection * float4(px, 0.0, 1.0);
    out.color = float4(instances[iid].color);
    return out;
}

fragment float4 rect_fragment(RectOut in [[stage_in]]) {
    return in.color;
}

/* =========================================================================
 * Line pipeline — thick anti-aliased segments (graph-view edges).
 * A unit quad is expanded along the segment with a perpendicular offset; the
 * fragment derives 1px edge coverage from the perpendicular distance.
 * ========================================================================= */

struct LineInstance {
    packed_float4 p;        /* x0,y0,x1,y1 (pixels) */
    float         thickness;
    packed_float4 color;    /* rgba (linear) */
};

struct LineOut {
    float4 position [[position]];
    float4 color;
    float  dist;            /* signed perpendicular distance (px) */
    float  halfw;           /* thickness * 0.5 */
};

vertex LineOut line_vertex(
    uint vid [[vertex_id]],
    uint iid [[instance_id]],
    constant float2 *quad [[buffer(0)]],
    constant LineInstance *inst [[buffer(1)]],
    constant Uniforms &uniforms [[buffer(2)]]
) {
    LineOut out;
    float2 q  = quad[vid];
    float4 pp = float4(inst[iid].p);
    float2 p0 = pp.xy, p1 = pp.zw;
    float thick = inst[iid].thickness;
    float2 dir = p1 - p0;
    float len = length(dir);
    float2 d = len > 1e-4 ? dir / len : float2(1.0, 0.0);
    float2 n = float2(-d.y, d.x);
    float margin = thick * 0.5 + 1.0;        /* +1px AA */
    float2 along = mix(p0, p1, q.x);
    float across = (q.y - 0.5) * (2.0 * margin);
    float2 px = along + n * across;
    out.position = uniforms.projection * float4(px, 0.0, 1.0);
    out.color = float4(inst[iid].color);
    out.dist  = across;
    out.halfw = thick * 0.5;
    return out;
}

fragment float4 line_fragment(LineOut in [[stage_in]]) {
    float cov = clamp(in.halfw + 0.5 - abs(in.dist), 0.0, 1.0);
    if (cov <= 0.0) discard_fragment();
    return float4(in.color.rgb, in.color.a * cov);
}

/* =========================================================================
 * Rounded-rect pipeline — SDF + soft drop shadow in a single fragment pass.
 *
 * One quad per rrect, expanded in the vertex shader to cover the visual rect
 * plus the shadow extent. The fragment computes:
 *   - signed distance to the rounded rect (fill mask, 1px AA at edge)
 *   - signed distance to the offset shadow rect (smooth quadratic falloff)
 * Output is premultiplied alpha; pipeline blend is (ONE, OneMinusSourceAlpha).
 * Black-only shadow keeps the math simple — no shadow.rgb term needed.
 * ========================================================================= */

struct RRectInstance {
    packed_float2 pos;          /* visual rect top-left */
    packed_float2 size;         /* visual rect size */
    packed_float4 fill_color;   /* linear RGBA */
    packed_float4 radii;        /* (tl, tr, br, bl) in pixels */
    packed_float4 shadow;       /* (size, alpha, offset_y, offset_x) */
    packed_float4 border_color; /* linear RGBA */
    packed_float4 border;       /* (width, inner_size, inner_alpha, _pad) */
};

struct RRectOut {
    float4 position [[position]];
    float2 frag_pos;            /* fragment pos in rect-local coords */
    float2 rect_size;
    float4 fill_color;
    float4 radii;
    float4 shadow;              /* (size, alpha, offset_y, offset_x) */
    float4 border_color;
    float  border_width;
    float  inner_shadow_size;
    float  inner_shadow_alpha;
};

vertex RRectOut rrect_vertex(
    uint vid [[vertex_id]],
    uint iid [[instance_id]],
    constant float2 *quad [[buffer(0)]],
    constant RRectInstance *instances [[buffer(1)]],
    constant Uniforms &uniforms [[buffer(2)]]
) {
    RRectOut out;
    float2 q = quad[vid];
    float2 pos = float2(instances[iid].pos);
    float2 size = float2(instances[iid].size);
    float4 sh = float4(instances[iid].shadow);
    float shadow_size = sh.x;
    float shadow_offset_y = sh.z;
    float shadow_offset_x = sh.w;

    /* Pad geometry to cover shadow extent + 2px AA margin. Pad asymmetrically
     * along whichever axis the shadow is offset so the quad is no larger than
     * needed but still covers the full penumbra on the offset side. */
    float pad = shadow_size + 2.0;
    float pad_top   = max(0.0, pad - max(0.0,  shadow_offset_y));
    float pad_bot   = pad + max(0.0, shadow_offset_y);
    float pad_left  = max(0.0, pad - max(0.0,  shadow_offset_x));
    float pad_right = pad + max(0.0, shadow_offset_x);

    float2 exp_origin = pos - float2(pad_left, pad_top);
    float2 exp_size   = size + float2(pad_left + pad_right, pad_top + pad_bot);
    float2 px = exp_origin + q * exp_size;

    out.position = uniforms.projection * float4(px, 0.0, 1.0);
    out.frag_pos = px - pos;          /* (0,0) at visual rect top-left */
    out.rect_size = size;
    out.fill_color = float4(instances[iid].fill_color);
    out.radii = float4(instances[iid].radii);
    out.shadow = sh;                  /* (size, alpha, offset_y, offset_x) */
    out.border_color = float4(instances[iid].border_color);
    float4 border_v = float4(instances[iid].border);
    out.border_width = border_v.x;
    out.inner_shadow_size = border_v.y;
    out.inner_shadow_alpha = border_v.z;
    return out;
}

/* iquilezles-style rounded-box SDF, generalised to per-corner radii.
 * `p` is in rect-local coords (origin = top-left, range [0, size]).
 * Negative result = inside, positive = outside. */
static float sdf_rrect(float2 p, float2 size, float4 radii) {
    /* radii: (tl, tr, br, bl) — pick this fragment's corner */
    float2 c = p - size * 0.5;
    float r_top    = (c.x > 0.0) ? radii.y : radii.x;
    float r_bottom = (c.x > 0.0) ? radii.z : radii.w;
    float r        = (c.y > 0.0) ? r_bottom : r_top;
    float2 q = abs(c) - size * 0.5 + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, float2(0.0))) - r;
}

fragment float4 rrect_fragment(RRectOut in [[stage_in]]) {
    float2 p = in.frag_pos;
    float2 size = in.rect_size;

    float d_fill = sdf_rrect(p, size, in.radii);
    /* Outer-edge AA mask: 1 inside, 0 outside, ramp across the edge. */
    float outer_mask = clamp(0.5 - d_fill, 0.0, 1.0);

    /* Bordered path: blend `border_color` in the outer ring and `fill_color`
     * everywhere inside the ring, with AA at both edges. inner_amount is 1
     * deep inside the fill region (where d_fill <= -border_width-0.5) and 0
     * inside the border zone. */
    float fill_a;
    float3 fill_rgb_premul;
    if (in.border_width > 0.5) {
        float inner_amount = clamp(0.5 - (d_fill + in.border_width), 0.0, 1.0);
        float3 col_rgb = mix(in.border_color.rgb * in.border_color.a,
                             in.fill_color.rgb   * in.fill_color.a,
                             inner_amount);
        float  col_a   = mix(in.border_color.a, in.fill_color.a, inner_amount);
        fill_a = col_a * outer_mask;
        fill_rgb_premul = col_rgb * outer_mask;
    } else {
        fill_a = outer_mask * in.fill_color.a;
        fill_rgb_premul = in.fill_color.rgb * fill_a;
    }

    /* Inner shadow: darken the fill near the inside edge of the rect.
     * inner_d = positive distance inward from the rect outline. Mask is 1
     * right at the edge, ramps to 0 at `inner_size` deep inside. Applied
     * as multiplicative darkening on the premul rgb. */
    if (in.inner_shadow_size > 0.5 && in.inner_shadow_alpha > 0.0) {
        float inner_d = -d_fill;
        float inner_t = clamp(inner_d / in.inner_shadow_size, 0.0, 1.0);
        float inner_mask = (1.0 - inner_t) * (1.0 - inner_t);  /* quadratic */
        float dark = inner_mask * in.inner_shadow_alpha;
        fill_rgb_premul *= (1.0 - dark);
    }

    float shadow_a = 0.0;
    float shadow_size = in.shadow.x;
    float shadow_alpha_param = in.shadow.y;
    if (shadow_size > 0.5 && shadow_alpha_param > 0.0) {
        float2 p_shadow = p - float2(in.shadow.w, in.shadow.z);
        float d_shadow = sdf_rrect(p_shadow, size, in.radii);
        /* t in [0,1]: 0 at rect edge, 1 at full extent. Quadratic falloff
         * gives a softer Gaussian-like penumbra without needing exp(). */
        float t = clamp(d_shadow / shadow_size, 0.0, 1.0);
        float falloff = (1.0 - t) * (1.0 - t);
        shadow_a = falloff * shadow_alpha_param;
    }

    /* Premultiplied output. Shadow is pure black, so its rgb term is 0
     * — only its alpha contributes to the composite. Pipeline blend
     * (ONE, OneMinusSrcAlpha) realises:
     *   final.rgb = src.rgb + dst.rgb * (1 - src.a)
     * which is "fill (or fill+border) over shadow over dst" with black shadow. */
    float3 out_rgb = fill_rgb_premul;
    float out_a = fill_a + shadow_a * (1.0 - fill_a);
    if (out_a < 0.001) discard_fragment();
    return float4(out_rgb, out_a);
}

/* =========================================================================
 * Background image pipeline — fullscreen textured quad
 * Renders background wallpaper with configurable opacity and scaling.
 * ========================================================================= */

struct BgImageUniforms {
    float4x4 projection;
    float2   pos;       /* top-left in pixels */
    float2   size;      /* quad size in pixels */
    float2   uv_scale;  /* UV scale for tiling/aspect */
    float2   uv_offset; /* UV offset for centering */
    float    opacity;
    float    _pad[3];
};

struct BgImageOut2 {
    float4 position [[position]];
    float2 uv;
    float  opacity;
};

vertex BgImageOut2 bg_image_vertex2(
    uint vid [[vertex_id]],
    constant float2 *quad [[buffer(0)]],
    constant BgImageUniforms &uniforms [[buffer(1)]]
) {
    BgImageOut2 out;
    float2 q = quad[vid];
    float2 px = uniforms.pos + q * uniforms.size;
    out.position = uniforms.projection * float4(px, 0.0, 1.0);
    out.uv = uniforms.uv_offset + q * uniforms.uv_scale;
    out.opacity = uniforms.opacity;
    return out;
}

fragment float4 bg_image_fragment2(
    BgImageOut2 in [[stage_in]],
    texture2d<float> bg_tex [[texture(0)]],
    sampler samp [[sampler(0)]]
) {
    float4 c = bg_tex.sample(samp, in.uv);
    return float4(c.rgb, c.a * in.opacity);
}

/* =========================================================================
 * Backdrop blur — single-pass 13-tap separable Gaussian approximation
 *
 * The renderer blits the in-progress drawable into a snapshot texture before
 * a modal is drawn; the modal then samples this texture with a Gaussian
 * kernel applied per fragment. Per-corner radii + soft tint overlay let the
 * modal panel be fully described by one rrect-style instance.
 *
 * The 13-tap kernel uses the standard symmetric weights for sigma~3.0, and
 * linear filtering halves the tap count by sampling between texels.
 * Output is premultiplied alpha; pipeline blend (ONE, OneMinusSrcAlpha).
 * ========================================================================= */

struct BlurInstance {
    packed_float2 pos;          /* visual rect top-left in screen pixels */
    packed_float2 size;         /* visual rect size */
    packed_float4 tint;         /* RGBA tint blended on top of the blurred sample */
    packed_float4 radii;        /* (tl, tr, br, bl) in pixels */
    packed_float2 blur_dir;     /* (px_x, px_y) — distance per tap in framebuffer pixels */
    packed_float2 inv_fb_size;  /* (1/fb_w, 1/fb_h) for screen-UV conversion */
};

struct BlurOut {
    float4 position [[position]];
    float2 frag_pos;            /* fragment pos in rect-local coords */
    float2 rect_size;
    float2 screen_uv_origin;    /* uv of the rect's top-left in the snapshot */
    float2 blur_dir;            /* uv-space tap step */
    float4 tint;
    float4 radii;
    float2 inv_size;
};

vertex BlurOut blur_vertex(
    uint vid [[vertex_id]],
    uint iid [[instance_id]],
    constant float2 *quad [[buffer(0)]],
    constant BlurInstance *instances [[buffer(1)]],
    constant Uniforms &uniforms [[buffer(2)]]
) {
    BlurOut out;
    float2 q = quad[vid];
    float2 pos = float2(instances[iid].pos);
    float2 size = float2(instances[iid].size);
    float2 inv = float2(instances[iid].inv_fb_size);
    float2 px = pos + q * size;
    out.position = uniforms.projection * float4(px, 0.0, 1.0);
    out.frag_pos = px - pos;
    out.rect_size = size;
    out.screen_uv_origin = pos * inv;
    out.blur_dir = float2(instances[iid].blur_dir) * inv;
    out.tint = float4(instances[iid].tint);
    out.radii = float4(instances[iid].radii);
    out.inv_size = inv;
    return out;
}

fragment float4 blur_fragment(
    BlurOut in [[stage_in]],
    texture2d<float> snapshot [[texture(0)]],
    sampler samp [[sampler(0)]]
) {
    /* Outer SDF mask so the blur respects the rounded corners. */
    float d = sdf_rrect(in.frag_pos, in.rect_size, in.radii);
    float outer_mask = clamp(0.5 - d, 0.0, 1.0);
    if (outer_mask < 0.001) discard_fragment();

    /* This fragment's UV in the snapshot texture. */
    float2 uv = in.screen_uv_origin + in.frag_pos * in.inv_size;

    /* 13-tap symmetric Gaussian (sigma~3) reduced to 7 samples by exploiting
     * bilinear filtering — each sample reads the weighted average of 2 texels. */
    const float w0 = 0.196482550151;       /* center */
    const float w1 = 0.296906964672;       /* +-1.411764705882 */
    const float w2 = 0.094470395095;       /* +-3.294117647058 */
    const float w3 = 0.010381362401;       /* +-5.176470588235 */
    const float o1 = 1.411764705882;
    const float o2 = 3.294117647058;
    const float o3 = 5.176470588235;

    float2 step = in.blur_dir;
    float3 acc = snapshot.sample(samp, uv).rgb * w0;
    acc += snapshot.sample(samp, uv + step * o1).rgb * w1;
    acc += snapshot.sample(samp, uv - step * o1).rgb * w1;
    acc += snapshot.sample(samp, uv + step * o2).rgb * w2;
    acc += snapshot.sample(samp, uv - step * o2).rgb * w2;
    acc += snapshot.sample(samp, uv + step * o3).rgb * w3;
    acc += snapshot.sample(samp, uv - step * o3).rgb * w3;

    /* Blend tint over the blurred sample (over operator, premul output). */
    float3 base = acc;
    float3 over = mix(base, in.tint.rgb, in.tint.a);
    float3 out_rgb = over * outer_mask;
    return float4(out_rgb, outer_mask);
}
