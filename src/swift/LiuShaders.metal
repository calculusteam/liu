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
    float2   _pad;
};

/* =========================================================================
 * Glyph pipeline — instanced rendering
 * One draw call for ALL visible glyphs
 * ========================================================================= */

struct GlyphInstance {
    packed_float2 pos;        // cell top-left pixel
    packed_float4 uv;         // u0, v0, u1, v1
    packed_float3 fg;         // foreground RGB (ignored for color glyphs)
    float         is_color;   // 0.0 = alpha glyph, 1.0 = color emoji
    float         w_cells;    // width in cells: 1.0 normal, 2.0+ for ligatures
    float         alpha;      // per-instance opacity (1.0 = fully opaque)
};

struct GlyphOut {
    float4 position [[position]];
    float2 uv;
    float3 color;
    float  is_color;
    float  alpha;             // forwarded from instance
};

vertex GlyphOut glyph_vertex(
    uint vid [[vertex_id]],
    uint iid [[instance_id]],
    constant float2 *quad [[buffer(0)]],
    constant GlyphInstance *instances [[buffer(1)]],
    constant Uniforms &uniforms [[buffer(2)]]
) {
    GlyphOut out;
    float2 q = quad[vid];
    float2 pos = float2(instances[iid].pos);
    float4 uv = float4(instances[iid].uv);
    float w = instances[iid].w_cells;
    if (w < 0.5) w = 1.0;  /* safety: treat 0 as 1 cell */
    float2 sz = float2(uniforms.cell_size.x * w, uniforms.cell_size.y);
    float2 px = pos + q * sz;
    out.position = uniforms.projection * float4(px, 0.0, 1.0);
    out.uv = mix(uv.xy, uv.zw, q);
    out.color = float3(instances[iid].fg);
    out.is_color = instances[iid].is_color;
    out.alpha = instances[iid].alpha;
    return out;
}

/* Single premultiplied output. Pipeline blend is (ONE, OneMinusSrcAlpha):
 *   out.rgb = fg.rgb * cov + dst.rgb * (1 - cov)
 *   out.a   = cov + dst.a * (1 - cov)
 * Atlas stores grayscale coverage (R8) so a single alpha drives all channels.
 * Foreground RGB arrives linear from the CPU side; framebuffer is sRGB and
 * Metal encodes on write — compositing in linear is gamma-correct. */
fragment float4 glyph_fragment(
    GlyphOut in [[stage_in]],
    texture2d<float> atlas [[texture(0)]],
    texture2d<float> color_atlas [[texture(1)]],
    sampler samp [[sampler(0)]]
) {
    float a = in.alpha;
    if (a <= 0.0) discard_fragment();
    if (in.is_color > 0.5) {
        float4 c = color_atlas.sample(samp, in.uv);
        float ca = c.a * a;
        if (ca < 0.004) discard_fragment();
        return float4(c.rgb * a, ca);
    }
    float cov = atlas.sample(samp, in.uv).r;
    if (cov * a < 0.004) discard_fragment();
    /* Apple-style stem darkening: text rendered in pure linear coverage looks
     * skeletal next to CoreText/AppKit output. A 1/1.43 gamma on the coverage
     * mask thickens thin strokes and matches macOS native text weight, while
     * keeping the math linear-light (the boost happens *before* the linear
     * fg*cov composite that the sRGB framebuffer encodes on write).
     * Gamma is applied to the RAW coverage mask, then per-instance alpha
     * scales the result linearly — so modal fades (a<1) stay perceptually
     * linear instead of being warped by the stem-darkening curve. */
    cov = pow(cov, 1.0 / 1.43);
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
     * deep inside the fill region (where d_fill <= -border_width-0.5) and
     * 0 once we cross into the border zone. */
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
     * inner_d = positive distance inward from the rect outline.
     * Mask is 1 right at the edge, ramps to 0 once we're `inner_size` deep
     * inside. Applied as multiplicative darkening on the premul rgb. */
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
 * Compute pipeline — GPU-driven cell → vertex conversion
 * Reads terminal cell grid, produces glyph instances + bg rects.
 * Eliminates CPU batch building for the terminal grid.
 * ========================================================================= */

/* Cell layout matching C-side Cell struct (16 bytes) */
struct Cell {
    uint   codepoint;
    uint   fg;        /* ANSI index or 0x01RRGGBB truecolor */
    uint   bg;
    ushort flags;
    ushort _pad0;
};

/* Glyph UV lookup entry — one per cached codepoint */
struct GlyphUV {
    packed_float4 uv;  /* u0, v0, u1, v1 */
};

struct ComputeParams {
    uint cols;
    uint rows;
    float origin_x;
    float origin_y;
    float cell_w;
    float cell_h;
    uint  bg_default;    /* BG_DEFAULT value (0) */
    uint  fg_default;    /* FG_DEFAULT value (7) */
};

/* ANSI color → float3 conversion */
static float3 ansi_to_rgb(uint val, constant float4 *palette) {
    if (val & 0x01000000) {
        /* Truecolor: 0x01RRGGBB */
        return float3(
            float((val >> 16) & 0xFF) / 255.0,
            float((val >> 8) & 0xFF) / 255.0,
            float(val & 0xFF) / 255.0
        );
    }
    if (val < 256) return palette[val].rgb;
    return float3(1.0);
}

/* Build glyph instances from terminal cell grid */
kernel void compute_glyphs(
    device const Cell *cells [[buffer(0)]],
    device GlyphInstance *out_glyphs [[buffer(1)]],
    device atomic_uint *glyph_count [[buffer(2)]],
    constant ComputeParams &params [[buffer(3)]],
    constant GlyphUV *uv_table [[buffer(4)]],
    constant float4 *palette [[buffer(5)]],
    constant uint *uv_map_keys [[buffer(6)]],    /* codepoint keys for lookup */
    constant uint &uv_map_count [[buffer(7)]],
    uint2 gid [[thread_position_in_grid]]
) {
    uint col = gid.x;
    uint row = gid.y;
    if (col >= params.cols || row >= params.rows) return;

    Cell c = cells[row * params.cols + col];
    if (c.codepoint <= 32) return;
    if (c.flags & (1 << 9)) return; /* ATTR_WDUMMY */

    /* Lookup UV for this codepoint — linear scan of map (small table) */
    float4 uv = float4(0);
    for (uint i = 0; i < uv_map_count; i++) {
        if (uv_map_keys[i] == c.codepoint) {
            uv = float4(uv_table[i].uv);
            break;
        }
    }
    if (uv.x == 0 && uv.y == 0 && uv.z == 0 && uv.w == 0) return;

    uint idx = atomic_fetch_add_explicit(glyph_count, 1, memory_order_relaxed);
    float2 pos = float2(params.origin_x + float(col) * params.cell_w,
                         params.origin_y + float(row) * params.cell_h);

    bool inverse = (c.flags & (1 << 5)) != 0;
    float3 fg = inverse ? ansi_to_rgb(c.bg, palette) : ansi_to_rgb(c.fg, palette);

    out_glyphs[idx].pos = pos;
    out_glyphs[idx].uv = uv;
    out_glyphs[idx].fg = fg;
    out_glyphs[idx].is_color = 0.0;
    out_glyphs[idx].w_cells = 1.0;
}

/* Build background rects from terminal cell grid.
 * writes RectInstance (one per non-default-bg cell) matching the
 * CPU path; this kernel is compiled but not yet wired at runtime. */
kernel void compute_bg_rects(
    device const Cell *cells [[buffer(0)]],
    device RectInstance *out_instances [[buffer(1)]],
    device atomic_uint *rect_count [[buffer(2)]],
    constant ComputeParams &params [[buffer(3)]],
    constant float4 *palette [[buffer(4)]],
    uint2 gid [[thread_position_in_grid]]
) {
    uint col = gid.x;
    uint row = gid.y;
    if (col >= params.cols || row >= params.rows) return;

    Cell c = cells[row * params.cols + col];
    bool inverse = (c.flags & (1 << 5)) != 0;
    uint bg_val = inverse ? c.fg : c.bg;

    /* Skip default bg (already cleared) */
    if (!inverse && !(bg_val & 0x01000000) && bg_val == params.bg_default) return;

    float3 bg = ansi_to_rgb(bg_val, palette);
    float x = params.origin_x + float(col) * params.cell_w;
    float y = params.origin_y + float(row) * params.cell_h;

    uint idx = atomic_fetch_add_explicit(rect_count, 1, memory_order_relaxed);
    out_instances[idx].pos   = packed_float2(x, y);
    out_instances[idx].size  = packed_float2(params.cell_w, params.cell_h);
    out_instances[idx].color = packed_float4(bg, 1.0);
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
 * The 13-tap kernel uses the standard symmetric weights for sigma~3.0; we
 * exploit linear filtering to halve the tap count by sampling between texels.
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
