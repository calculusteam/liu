#ifndef USE_METAL
/*
 * Liu - GPU terminal renderer (multi-pass)
 *
 * Pass 0: glClear (terminal bg)
 * Pass 1: Cell bg rects (non-default backgrounds)
 * Pass 2: Glyph instances (alpha-blended, single draw call)
 * Pass 3: Decorations (cursor, underline, selection)
 */
#include "renderer/renderer.h"
#include "terminal/palette256.h"
#include "core/percentile.h"
#include "core/memory.h"
#include "core/checked.h"
#include "core/imgproc.h"
#include "core/mem_budget.h"
#include "renderer/image_stats.h"
#include "renderer/retained_batch.h"
#include "core/utf8.h"           /* utf8_is_emoji — colour glyphs ignore fg */
/* Not used directly (the blur probe is in renderer.h so both backends get it),
 * but kept so this TU sees BOTH platform.h's declarations of
 * platform_blur_debug_* and renderer.h's copies — the compiler then catches any
 * drift between them. */
#include "platform/platform.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>   /* the frame probe's GPU-time accumulators */

#ifdef PLATFORM_MACOS
    #include <OpenGL/gl3.h>
#else
    #include <GL/gl.h>
    /* <GL/gl.h> only exposes GL 1.1; map the modern gl* calls below onto
     * runtime-loaded function pointers (gl_loader_init is called by the
     * platform layer once the context is current). */
    #define LIU_GL_REDIRECT
    #include "renderer/gl_loader.h"
#endif

/* =========================================================================
 * ANSI 256 color palette
 * ========================================================================= */

Color g_ansi_colors[256];
f32  g_srgb_decode_lut[256];
bool g_srgb_decode_lut_ready = false;

void renderer_init_colors(void) {
    /* sRGB→linear LUT — see renderer.h for the exactness argument. */
    for (i32 i = 0; i < 256; i++)
        g_srgb_decode_lut[i] = srgb_decode((f32)i * (1.0f / 255.0f));
    g_srgb_decode_lut_ready = true;
    /* Standard 16 */
    g_ansi_colors[0]  = COLOR_RGB(0, 0, 0);
    g_ansi_colors[1]  = COLOR_RGB(205, 49, 49);
    g_ansi_colors[2]  = COLOR_RGB(13, 188, 121);
    g_ansi_colors[3]  = COLOR_RGB(229, 229, 16);
    g_ansi_colors[4]  = COLOR_RGB(36, 114, 200);
    g_ansi_colors[5]  = COLOR_RGB(188, 63, 188);
    g_ansi_colors[6]  = COLOR_RGB(17, 168, 205);
    g_ansi_colors[7]  = COLOR_RGB(229, 229, 229);
    g_ansi_colors[8]  = COLOR_RGB(102, 102, 102);
    g_ansi_colors[9]  = COLOR_RGB(241, 76, 76);
    g_ansi_colors[10] = COLOR_RGB(35, 209, 139);
    g_ansi_colors[11] = COLOR_RGB(245, 245, 67);
    g_ansi_colors[12] = COLOR_RGB(59, 142, 234);
    g_ansi_colors[13] = COLOR_RGB(214, 112, 214);
    g_ansi_colors[14] = COLOR_RGB(41, 184, 219);
    g_ansi_colors[15] = COLOR_RGB(255, 255, 255);
    /* The 216-colour cube and the 24 greys: terminal/palette256.h, which an
     * OSC 4 query is answered from too. */
    for (int i = 16; i < 256; i++) {
        u32 rgb = palette256_rgb(i);
        g_ansi_colors[i] = COLOR_RGB((rgb >> 16) & 0xFFu, (rgb >> 8) & 0xFFu, rgb & 0xFFu);
    }
}

/* =========================================================================
 * Shader helpers
 * ========================================================================= */

static u32 compile_shader(const char *src, GLenum type) {
    u32 s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static u32 link_program(u32 v, u32 f) {
    u32 p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "link: %s\n", log);
        /* Detach + delete the attached shaders so they don't leak on failure. */
        glDetachShader(p, v);
        glDetachShader(p, f);
        glDeleteShader(v);
        glDeleteShader(f);
        glDeleteProgram(p);
        return 0;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

/* =========================================================================
 * Shaders
 * ========================================================================= */

/* Glyph shader: renders alpha-blended text on top of bg.
 * Fragment reads single-channel (red) atlas → applies as alpha to fg color. */
static const char *glyph_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_quad;\n"
    "layout(location=1) in vec2 a_pos;\n"
    "layout(location=2) in vec4 a_rect;\n"   /* texel rect (kind 0/1) or 0..1 (kind 2) */
    /* fg/bg arrive as normalised unsigned bytes in **sRGB** and are decoded
     * here — the GLSL twin of LiuShaders.metal. See GlyphInstance in
     * renderer.h for why the boundary moved off the CPU for glyphs. */
    "layout(location=3) in vec4 a_fg;\n"     /* sRGB rgb + instance alpha */
    "layout(location=4) in vec4 a_bg;\n"     /* sRGB rgb + \"known\" flag */
    "layout(location=5) in vec2 a_extra;\n"  /* x=tex_kind, y=w_cells (0..1 normalised) */
    "uniform mat4 u_projection;\n"
    "uniform vec2 u_cell_size;\n"
    "uniform float u_min_contrast;\n"
    /* The atlases are bound to the VERTEX stage too: the rect arrives in
     * texels and is normalised here against whatever texture this batch
     * samples, so an instance laid out against one atlas size stays right
     * after a grow that kept its pixels in place. Same rule as the Metal
     * vertex function. */
    "uniform sampler2D u_atlas;\n"
    "uniform sampler2D u_color_atlas;\n"
    "out vec2 v_uv;\n"
    "out vec3 v_fg;\n"
    "out float v_is_color;\n"
    "out float v_alpha;\n"
    "out vec3 v_bg;\n"
    "out float v_bg_known;\n"
    "vec3 srgb_to_linear(vec3 c){\n"
    "  return mix(c*(1.0/12.92), pow((c+0.055)*(1.0/1.055), vec3(2.4)),\n"
    "             step(vec3(0.04045), c));\n"
    "}\n"
    "float luminance(vec3 lin){ return dot(lin, vec3(0.2126,0.7152,0.0722)); }\n"
    "void main(){\n"
    "  float w = a_extra.y * 255.0;\n"
    "  if (w < 0.5) w = 1.0;\n"
    "  vec2 sz = vec2(u_cell_size.x * w, u_cell_size.y);\n"
    "  vec2 px = a_pos + a_quad * sz;\n"
    "  gl_Position = u_projection * vec4(px, 0.0, 1.0);\n"
    "  float kind = a_extra.x * 255.0;\n"
    "  vec4 rect = a_rect;\n"
    "  if (kind < 0.5) {\n"
    "    vec2 inv = 1.0 / vec2(textureSize(u_atlas, 0));\n"
    "    rect *= inv.xyxy;\n"
    "  } else if (kind < 1.5) {\n"
    "    vec2 inv = 1.0 / vec2(textureSize(u_color_atlas, 0));\n"
    "    rect *= inv.xyxy;\n"
    "  }\n"
    "  v_uv = mix(rect.xy, rect.zw, a_quad);\n"
    "  vec3 fg = srgb_to_linear(a_fg.rgb);\n"
    "  vec3 bg = srgb_to_linear(a_bg.rgb);\n"
    "  float bg_known = a_bg.a > 0.0 ? 1.0 : 0.0;\n"
    "  v_is_color = kind;\n"
    "  if (u_min_contrast > 1.0 && bg_known > 0.5 && v_is_color < 0.5) {\n"
    "    float lf = luminance(fg), lb = luminance(bg);\n"
    "    float ratio = (max(lf,lb)+0.05)/(min(lf,lb)+0.05);\n"
    "    if (ratio < u_min_contrast) {\n"
    "      fg = ((1.05/(lb+0.05)) > ((lb+0.05)/0.05)) ? vec3(1.0) : vec3(0.0);\n"
    "    }\n"
    "  }\n"
    "  v_fg = fg;\n"
    "  v_bg = bg;\n"
    "  v_bg_known = bg_known;\n"
    "  v_alpha = a_fg.a;\n"
    "}\n";

static const char *glyph_frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec3 v_fg;\n"
    "in float v_is_color;\n"
    "in float v_alpha;\n"
    "in vec3 v_bg;\n"
    "in float v_bg_known;\n"
    "uniform sampler2D u_atlas;\n"
    "uniform sampler2D u_color_atlas;\n"
    "uniform float u_text_gamma;\n"
    "uniform float u_blend_correct;\n"
    "uniform float u_text_weight;\n"
    "out vec4 frag_color;\n"
    /* Premultiplied output, matching the Metal glyph_fragment paired with a
     * (ONE, 1-SrcAlpha) blend. The image/icon path feeds PREMULTIPLIED sRGB
     * bitmaps (SF Symbols come from a kCGImageAlphaPremultipliedLast context),
     * so straight (SrcAlpha) blending would square their coverage and render
     * icons thin and washed out. For solid-color text premultiplied and
     * straight alpha composite the same (rgb*cov either way). */
    "void main(){\n"
    /* 1 = the emoji colour atlas, which lives alongside the alpha atlas and
     * is sampled in the same batch. 2 = the image/icon path, which rebinds
     * u_atlas to its own RGBA texture for the duration of the draw. */
    "  if (v_is_color > 0.5) {\n"
    "    vec4 c = v_is_color < 1.5 ? texture(u_color_atlas, v_uv)\n"
    "                              : texture(u_atlas, v_uv);\n"
    "    float ca = c.a * v_alpha;\n"
    "    if (ca < 0.004) discard;\n"
    /* v_fg multiplies the texel so one texture serves many tints — see the
     * matching note in LiuShaders.metal::glyph_fragment. Full-colour images
     * pass fg=(1,1,1); white masks use it to colour the icon at draw time. */
    "    frag_color = vec4(c.rgb * v_fg * v_alpha, ca);\n"  /* premultiplied */
    "    return;\n"
    "  }\n"
    "  float cov = texture(u_atlas, v_uv).r;\n"
    /* Manual weight — the GLSL twin of LiuShaders.metal::weight_morph: erode /
     * dilate of the RAW mask (a tone curve could only move edge pixels and read
     * as doing nothing), before the empty test because a dilated pixel can be lit
     * by a neighbour while its own coverage is zero. 0 skips it. */
    "  if (u_text_weight != 0.0) {\n"
    "    vec2 t = 1.0 / vec2(textureSize(u_atlas, 0));\n"
    "    float l = textureLod(u_atlas, v_uv + vec2(-t.x, 0.0), 0.0).r;\n"
    "    float r = textureLod(u_atlas, v_uv + vec2( t.x, 0.0), 0.0).r;\n"
    "    float u = textureLod(u_atlas, v_uv + vec2(0.0, -t.y), 0.0).r;\n"
    "    float d = textureLod(u_atlas, v_uv + vec2(0.0,  t.y), 0.0).r;\n"
    "    if (u_text_weight > 0.0) {\n"
    "      float ul = textureLod(u_atlas, v_uv + vec2(-t.x, -t.y), 0.0).r;\n"
    "      float ur = textureLod(u_atlas, v_uv + vec2( t.x, -t.y), 0.0).r;\n"
    "      float dl = textureLod(u_atlas, v_uv + vec2(-t.x,  t.y), 0.0).r;\n"
    "      float dr = textureLod(u_atlas, v_uv + vec2( t.x,  t.y), 0.0).r;\n"
    "      float nb = max(max(max(l, r), max(u, d)), 0.7 * max(max(ul, ur), max(dl, dr)));\n"
    "      cov = max(cov, min(u_text_weight * 0.42, 0.84) * nb);\n"
    "    } else {\n"
    "      float m = min(min(l, r), min(u, d));\n"
    "      cov = cov - min(-u_text_weight * 0.25, 0.5) * max(0.0, cov - m);\n"
    "    }\n"
    "  }\n"
    "  if (cov * v_alpha < 0.004) discard;\n"
    /* Stem darkening, gamma supplied by the CPU (font_text_gamma): 1.0 when the
     * rasteriser already dilated the outline via Core Graphics font smoothing,
     * >1 for the raw-coverage paths. Matches Metal's glyph_fragment — including
     * the operand: the curve belongs on the RAW mask, with per-instance alpha
     * scaling the result afterwards, or a fading modal warps the stem weight. */
    /* ...except on a glyph the blend correction below applies to: it already
     * lands on the sRGB-space weight from the raw mask (see the Metal twin). */
    "  bool corrected = u_blend_correct > 0.5 && v_bg_known > 0.5;\n"
    "  if (!corrected && u_text_gamma > 1.001) cov = pow(cov, 1.0/u_text_gamma);\n"
    /* Linear-blending weight correction — the GLSL twin of the Metal path.
     * Keeps the linear composite and corrects the COVERAGE: correct blending
     * renders light-on-dark heavier and dark-on-light lighter than the
     * sRGB-space blend the eye is trained on, and this restores the familiar
     * weight in both directions. Inert without a background. */
    "  if (corrected) {\n"
    "    float fl = dot(v_fg, vec3(0.2126,0.7152,0.0722));\n"
    "    float bl = dot(v_bg, vec3(0.2126,0.7152,0.0722));\n"
    "    if (abs(fl-bl) > 0.001) {\n"
    "      float fs = fl <= 0.0031308 ? fl*12.92 : 1.055*pow(fl,1.0/2.4)-0.055;\n"
    "      float bs = bl <= 0.0031308 ? bl*12.92 : 1.055*pow(bl,1.0/2.4)-0.055;\n"
    "      float m = fs*cov + bs*(1.0-cov);\n"
    "      float t = m <= 0.04045 ? m*(1.0/12.92) : pow((m+0.055)*(1.0/1.055), 2.4);\n"
    "      cov = clamp((t-bl)/(fl-bl), 0.0, 1.0);\n"
    "    }\n"
    "  }\n"
    "  cov *= v_alpha;\n"
    "  frag_color = vec4(v_fg * cov, cov);\n"     /* premultiplied */
    "}\n";

/* Rounded-rect shader (SDF + soft drop shadow + per-corner radii + border
 * + inner shadow). GLSL port of LiuShaders.metal::rrect_*. Premultiplied
 * alpha output blended with (ONE, OneMinusSourceAlpha). */
static const char *rrect_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_quad;\n"
    "layout(location=1) in vec4 a_pos_size;\n"          /* (x, y, w, h) */
    "layout(location=2) in vec4 a_fill;\n"
    "layout(location=3) in vec4 a_radii;\n"             /* (tl, tr, br, bl) */
    "layout(location=4) in vec4 a_shadow;\n"            /* (size, alpha, off_y, off_x) */
    "layout(location=5) in vec4 a_border_color;\n"
    "layout(location=6) in vec4 a_border;\n"            /* (width, inner_sz, inner_a, _pad) */
    "uniform mat4 u_projection;\n"
    "out vec2 v_frag_pos;\n"
    "out vec2 v_size;\n"
    "out vec4 v_fill;\n"
    "out vec4 v_radii;\n"
    "out vec4 v_shadow;\n"
    "out vec4 v_border_color;\n"
    "out float v_border_width;\n"
    "out float v_inner_size;\n"
    "out float v_inner_alpha;\n"
    "void main(){\n"
    "  vec2 pos = a_pos_size.xy;\n"
    "  vec2 sz  = a_pos_size.zw;\n"
    "  float pad = a_shadow.x + 2.0;\n"
    "  float pad_top   = max(0.0, pad - max(0.0, a_shadow.z));\n"
    "  float pad_bot   = pad + max(0.0, a_shadow.z);\n"
    "  float pad_left  = max(0.0, pad - max(0.0, a_shadow.w));\n"
    "  float pad_right = pad + max(0.0, a_shadow.w);\n"
    "  vec2 origin = pos - vec2(pad_left, pad_top);\n"
    "  vec2 esize  = sz + vec2(pad_left + pad_right, pad_top + pad_bot);\n"
    "  vec2 px = origin + a_quad * esize;\n"
    "  gl_Position = u_projection * vec4(px, 0.0, 1.0);\n"
    "  v_frag_pos = px - pos;\n"
    "  v_size = sz;\n"
    "  v_fill = a_fill;\n"
    "  v_radii = a_radii;\n"
    "  v_shadow = a_shadow;\n"
    "  v_border_color = a_border_color;\n"
    "  v_border_width = a_border.x;\n"
    "  v_inner_size  = a_border.y;\n"
    "  v_inner_alpha = a_border.z;\n"
    "}\n";

static const char *rrect_frag_src =
    "#version 330 core\n"
    "in vec2 v_frag_pos;\n"
    "in vec2 v_size;\n"
    "in vec4 v_fill;\n"
    "in vec4 v_radii;\n"
    "in vec4 v_shadow;\n"
    "in vec4 v_border_color;\n"
    "in float v_border_width;\n"
    "in float v_inner_size;\n"
    "in float v_inner_alpha;\n"
    "out vec4 frag_color;\n"
    "float sdf_rrect(vec2 p, vec2 sz, vec4 radii){\n"
    "  vec2 c = p - sz * 0.5;\n"
    "  float r_top    = (c.x > 0.0) ? radii.y : radii.x;\n"
    "  float r_bottom = (c.x > 0.0) ? radii.z : radii.w;\n"
    "  float r        = (c.y > 0.0) ? r_bottom : r_top;\n"
    "  vec2 q = abs(c) - sz * 0.5 + r;\n"
    "  return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;\n"
    "}\n"
    "void main(){\n"
    "  float d = sdf_rrect(v_frag_pos, v_size, v_radii);\n"
    "  float outer_mask = clamp(0.5 - d, 0.0, 1.0);\n"
    "  float fill_a;\n"
    "  vec3  fill_rgb_premul;\n"
    "  if (v_border_width > 0.5) {\n"
    "    float inner_amount = clamp(0.5 - (d + v_border_width), 0.0, 1.0);\n"
    "    vec3 col_rgb = mix(v_border_color.rgb * v_border_color.a,\n"
    "                       v_fill.rgb         * v_fill.a,\n"
    "                       inner_amount);\n"
    "    float col_a = mix(v_border_color.a, v_fill.a, inner_amount);\n"
    "    fill_a = col_a * outer_mask;\n"
    "    fill_rgb_premul = col_rgb * outer_mask;\n"
    "  } else {\n"
    "    fill_a = outer_mask * v_fill.a;\n"
    "    fill_rgb_premul = v_fill.rgb * fill_a;\n"
    "  }\n"
    "  if (v_inner_size > 0.5 && v_inner_alpha > 0.0) {\n"
    "    float inner_d = -d;\n"
    "    float inner_t = clamp(inner_d / v_inner_size, 0.0, 1.0);\n"
    "    float inner_mask = (1.0 - inner_t) * (1.0 - inner_t);\n"
    "    fill_rgb_premul *= (1.0 - inner_mask * v_inner_alpha);\n"
    "  }\n"
    "  float shadow_a = 0.0;\n"
    "  if (v_shadow.x > 0.5 && v_shadow.y > 0.0) {\n"
    "    vec2 p_shadow = v_frag_pos - vec2(v_shadow.w, v_shadow.z);\n"
    "    float ds = sdf_rrect(p_shadow, v_size, v_radii);\n"
    "    float t = clamp(ds / v_shadow.x, 0.0, 1.0);\n"
    "    shadow_a = (1.0 - t) * (1.0 - t) * v_shadow.y;\n"
    "  }\n"
    "  float out_a = fill_a + shadow_a * (1.0 - fill_a);\n"
    "  if (out_a < 0.001) discard;\n"
    "  frag_color = vec4(fill_rgb_premul, out_a);\n"
    "}\n";

/* Rect shader: solid-colored quads via instancing.
 * a_quad is a unit quad (0..1); a_pos+a_size/a_color are per-instance. */
static const char *rect_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_quad;\n"
    "layout(location=1) in vec2 a_pos;\n"
    "layout(location=2) in vec2 a_size;\n"
    "layout(location=3) in vec4 a_color;\n"
    "uniform mat4 u_projection;\n"
    "out vec4 v_color;\n"
    "void main(){\n"
    "  vec2 px = a_pos + a_quad * a_size;\n"
    "  gl_Position = u_projection * vec4(px, 0.0, 1.0);\n"
    "  v_color = a_color;\n"
    "}\n";

static const char *rect_frag_src =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "out vec4 frag_color;\n"
    "void main(){ frag_color = v_color; }\n";

/* Line pipeline — thick AA segments (graph edges). Expands a unit quad along
 * the segment with a perpendicular offset; fragment derives 1px edge AA. */
static const char *line_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_quad;\n"      /* unit quad 0..1 */
    "layout(location=1) in vec4 a_p;\n"         /* x0,y0,x1,y1 */
    "layout(location=2) in float a_thick;\n"
    "layout(location=3) in vec4 a_color;\n"
    "uniform mat4 u_projection;\n"
    "out vec4 v_color;\n"
    "out float v_dist;\n"
    "out float v_half;\n"
    "void main(){\n"
    "  vec2 p0 = a_p.xy, p1 = a_p.zw;\n"
    "  vec2 dir = p1 - p0;\n"
    "  float len = length(dir);\n"
    "  vec2 d = len > 1e-4 ? dir/len : vec2(1.0,0.0);\n"
    "  vec2 nrm = vec2(-d.y, d.x);\n"
    "  float margin = a_thick*0.5 + 1.0;\n"
    "  vec2 along = mix(p0, p1, a_quad.x);\n"
    "  float across = (a_quad.y - 0.5) * (2.0*margin);\n"
    "  vec2 px = along + nrm*across;\n"
    "  gl_Position = u_projection * vec4(px, 0.0, 1.0);\n"
    "  v_color = a_color; v_dist = across; v_half = a_thick*0.5;\n"
    "}\n";

static const char *line_frag_src =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "in float v_dist;\n"
    "in float v_half;\n"
    "out vec4 frag_color;\n"
    "void main(){\n"
    "  float cov = clamp(v_half + 0.5 - abs(v_dist), 0.0, 1.0);\n"
    "  if (cov <= 0.0) discard;\n"
    "  frag_color = vec4(v_color.rgb, v_color.a * cov);\n"
    "}\n";

/* Background image (wallpaper) pipeline — GLSL port of LiuShaders.metal's
 * bg_image_vertex2 / bg_image_fragment2. One textured quad; the four layout
 * modes are all expressed as a (pos, size, uv_scale, uv_offset) tuple computed
 * on the CPU, exactly as renderer_draw_background_image does on Metal. */
static const char *bg_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_quad;\n"
    "uniform mat4 u_projection;\n"
    "uniform vec4 u_pos_size;\n"   /* xy = top-left px, zw = size px */
    "uniform vec4 u_uv;\n"         /* xy = scale, zw = offset */
    "out vec2 v_uv;\n"
    "void main(){\n"
    "  vec2 px = u_pos_size.xy + a_quad * u_pos_size.zw;\n"
    "  gl_Position = u_projection * vec4(px, 0.0, 1.0);\n"
    "  v_uv = u_uv.zw + a_quad * u_uv.xy;\n"
    "}\n";

static const char *bg_frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_opacity;\n"
    "out vec4 frag_color;\n"
    /* STRAIGHT alpha, not premultiplied — the Metal fragment emits exactly
     * this and is paired with (SrcAlpha, OneMinusSrcAlpha). The flush below
     * sets the same factors so both backends composite the wallpaper
     * identically. */
    "void main(){\n"
    "  vec4 c = texture(u_tex, v_uv);\n"
    "  frag_color = vec4(c.rgb, c.a * u_opacity);\n"
    "}\n";

/* =========================================================================
 * Init / Destroy
 * ========================================================================= */

#define MAX_GLYPHS  16384
#define MAX_RECTS    8192
#define MAX_RRECTS   1024
#define INIT_GLYPHS  2048
#define INIT_RECTS   1024
#define INIT_RRECTS   128
#define MAX_LINES   16384
#define INIT_LINES   1024
#define GL_IMAGE_CACHE_CAP 128
/* Ceiling on resident inline-image textures. Matches METAL_IMAGE_CACHE_BUDGET:
 * the working set of a file browser scrolling a directory of images plus a
 * couple of terminal-inline pictures; anything colder is cheaper to re-upload
 * than to keep pinned in GPU memory for the rest of the session. */
#define GL_IMAGE_CACHE_BUDGET (32ull * 1024ull * 1024ull)
/* Above this a single texture goes to the dedicated `oversize` tier instead of
 * the pool. Deliberately NOT derived from the budget — the coupling
 * METAL_IMAGE_OVERSIZE_BYTES exists to avoid: a threshold that halved with the
 * budget would push ordinary 2-4 MP viewer images onto the oversize path. 16
 * MiB ≈ a 2048x2048 RGBA image. */
#define GL_IMAGE_OVERSIZE_BYTES (16ull * 1024ull * 1024ull)
/* Matches METAL_IMAGE_OVERSIZE_SLOTS / _BUDGET, for the same reason: with
 * ONE slot, two above-threshold images drawn in the same frame would trade
 * it every frame and re-upload both. The residency argument is in
 * renderer_metal.m next to its copy — the two backends must not disagree
 * about this. */
#define GL_IMAGE_OVERSIZE_SLOTS 4
#define GL_IMAGE_OVERSIZE_BUDGET (96ull * 1024ull * 1024ull)
_Static_assert(GL_IMAGE_OVERSIZE_BUDGET > GL_IMAGE_OVERSIZE_BYTES * 2,
               "the oversize budget must hold at least two threshold images");

/* Absolute ceiling on one base-level RGBA image at the renderer boundary. */
#define GL_IMAGE_MAX_BYTES (256ull * 1024ull * 1024ull)

/* The LRU admission loop computes `GL_IMAGE_CACHE_BUDGET - bytes` in unsigned
 * arithmetic, which only means anything while every pooled texture is smaller
 * than the whole budget. Push the oversize threshold past the budget and that
 * subtraction wraps to ~SIZE_MAX, the eviction loop never runs, and the pool
 * grows without bound. Same assertion the Metal backend carries. */
_Static_assert(GL_IMAGE_OVERSIZE_BYTES < GL_IMAGE_CACHE_BUDGET,
               "oversize threshold must stay below the image pool budget");

typedef struct {
    const void *key;
    u64         generation;
    i32         width;
    i32         height;
    usize       bytes;
    u64         last_used_frame;
    f64         last_used_sec;
    GLuint      texture;
} GLImageCacheEntry;

/* One cache per Renderer, hung off `r->gl_image_cache`.
 *
 * GL texture NAMES are per-context, so a process-global cache handed window B
 * a name minted in window A's context — B then binds whatever that number
 * happens to mean in its own context (usually nothing: black). `frame_serial`
 * has to be per-renderer too, or it advances once per WINDOW per frame and the
 * idle-eviction windows below shrink by the window count.
 *
 * `oversize` is a small LRU for textures too large to admit to the pool,
 * mirroring the Metal backend. Held outside `bytes` so one huge image cannot
 * squeeze the ordinary entries out, and reclaimed on the same idle criterion.
 * Without it, everything past the admission limit would be re-uploaded on
 * every frame it is drawn — and with only ONE of it, two such images drawn
 * together did exactly that to each other. */
typedef struct {
    GLImageCacheEntry entries[GL_IMAGE_CACHE_CAP];
    GLImageCacheEntry oversize[GL_IMAGE_OVERSIZE_SLOTS];
    usize             oversize_bytes;
    usize             bytes;
    usize             accounted_bytes;
    u64               frame_serial;
    f64               now_sec;
} GLImageCache;

static void gl_image_account(GLImageCache *ic) {
    if (!ic) return;
    usize total = ic->bytes;
    if (total <= (usize)-1 - ic->oversize_bytes) total += ic->oversize_bytes;
    if (total > ic->accounted_bytes)
        mem_budget_adjust(MEM_IMAGE_GPU, (isize)(total - ic->accounted_bytes));
    else if (ic->accounted_bytes > total)
        mem_budget_adjust(MEM_IMAGE_GPU, -(isize)(ic->accounted_bytes - total));
    ic->accounted_bytes = total;
}
/* Reclaim an idle image texture after this many rendered frames without a draw.
 * Matches METAL_IMAGE_CACHE_IDLE_EVICT. */
#define GL_IMAGE_CACHE_IDLE_SEC 15.0
/* Shorter window for the oversize slot — it is the most expensive single
 * texture and no byte-budget pressure will ever reclaim it. Matches
 * METAL_IMAGE_OVERSIZE_IDLE_EVICT. */
#define GL_IMAGE_OVERSIZE_IDLE_SEC 3.0

/* Defined with the rest of the image cache below; renderer_begin_frame's idle
 * sweep needs it well before that. */
static void gl_image_cache_release_entry(GLImageCache *ic, GLImageCacheEntry *e);
static void gl_oversize_release(GLImageCache *ic, GLImageCacheEntry *e);

/* Defined with the frame functions below, but renderer_destroy runs first in
 * the file and has to give the query objects back. */
static void gl_gpu_timer_destroy(Renderer *r);

static bool renderer_reserve_glyphs(Renderer *r, u32 need) {
    if (r->glyph_cap >= need) return true;
    u32 cap = r->glyph_cap ? r->glyph_cap : INIT_GLYPHS;
    while (cap < need && cap < MAX_GLYPHS) cap *= 2;
    if (cap > MAX_GLYPHS) cap = MAX_GLYPHS;
    if (cap < need) return false;
    GlyphInstance *p = realloc(r->glyph_instances, (usize)cap * sizeof(*p));
    if (!p) return false;
    r->glyph_instances = p;
    r->glyph_cap = cap;
    return true;
}

static bool renderer_reserve_rects(Renderer *r, u32 need) {
    if (r->rect_batch_cap >= need) return true;
    u32 cap = r->rect_batch_cap ? r->rect_batch_cap : INIT_RECTS;
    while (cap < need && cap < MAX_RECTS) cap *= 2;
    if (cap > MAX_RECTS) cap = MAX_RECTS;
    if (cap < need) return false;
    RectInstance *p = realloc(r->rect_batch, (usize)cap * sizeof(*p));
    if (!p) return false;
    r->rect_batch = p;
    r->rect_batch_cap = cap;
    return true;
}

static bool renderer_reserve_rrects(Renderer *r, u32 need) {
    if (r->rrect_batch_cap >= need) return true;
    u32 cap = r->rrect_batch_cap ? r->rrect_batch_cap : INIT_RRECTS;
    while (cap < need && cap < MAX_RRECTS) cap *= 2;
    if (cap > MAX_RRECTS) cap = MAX_RRECTS;
    if (cap < need) return false;
    RRectInstance *p = realloc(r->rrect_batch, (usize)cap * sizeof(*p));
    if (!p) return false;
    r->rrect_batch = p;
    r->rrect_batch_cap = cap;
    return true;
}

static bool renderer_reserve_lines(Renderer *r, u32 need) {
    if (r->line_batch_cap >= need) return true;
    u32 cap = r->line_batch_cap ? r->line_batch_cap : INIT_LINES;
    while (cap < need && cap < MAX_LINES) cap *= 2;
    if (cap > MAX_LINES) cap = MAX_LINES;
    if (cap < need) return false;
    LineInstance *p = realloc(r->line_batch, (usize)cap * sizeof(*p));
    if (!p) return false;
    r->line_batch = p;
    r->line_batch_cap = cap;
    return true;
}

/* The OpenGL backend has no equivalent of Metal's single upload chokepoint:
 * glTexSubImage2D is called at a dozen sites in font.c and the driver's own
 * copy is not observable from here. Answering zero is correct — the counters
 * describe the Metal path and nothing else, which is what renderer.h says —
 * and it keeps ui/app_soak.c's glyph scenario linking on both backends
 * without an #ifdef. */
/* OpenGL needs no drain here, and calling glFinish() would be a real stall for
 * nothing. glTexSubImage2D must behave as if it consumed the client's pixels
 * before it returned, and the driver is required to order it against draws
 * already issued — so unlike Metal's replaceRegion into a shared texture,
 * there is no window in which the GPU reads a cell the CPU is rewriting.
 *
 * true, therefore, and not because nothing happened: the caller asks "is it now
 * safe to overwrite those cells", and on this backend it always was. Answering
 * false would keep the shelf reclaim pending forever on OpenGL. */
bool renderer_gpu_drain(Renderer *r) { (void)r; return true; }

/* "Has the GPU finished every frame it was given?" on OpenGL.
 *
 * GL has no command buffers to count, but it does issue commands in order, so one
 * real glFenceSync placed at the END of each frame answers the same question: if
 * that fence has signalled, every draw of that frame and of all frames before it
 * has completed, and no draw is reading the atlas. A fence is placed even though
 * the driver already orders glTexSubImage2D against issued draws, to keep the two
 * backends on ONE contract rather than leave OpenGL a special case.
 *
 * renderer_gl_frame_fence() is called from renderer_end_frame; there is at most
 * one fence alive, because a newer one subsumes every older one. */
void renderer_gl_frame_fence(Renderer *r) {
    if (!r) return;
    if (r->gl_sync) { glDeleteSync((GLsync)r->gl_sync); r->gl_sync = NULL; }
    GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!sync) return;      /* no fence: renderer_gpu_idle answers conservatively */
    /* Without this the fence can sit unflushed behind commands the driver has not
     * yet sent, and the wait below never satisfies. */
    glFlush();
    r->gl_sync = (void *)sync;
}

bool renderer_drawable_ready(Renderer *r) {
    (void)r;
    return false; /* OpenGL does not acquire drawables asynchronously. */
}

bool renderer_gpu_idle(Renderer *r) {
    if (!r) return true;
    if (!r->gl_sync) {
        /* Either nothing has been drawn yet, or the last fence has already been
         * observed signalled and dropped. Both mean idle. The third case —
         * glFenceSync itself failed — is indistinguishable here, so it answers
         * idle rather than wedging the reclaim forever on a driver that cannot
         * make fences; GL's own ordering guarantee is what carries that case. */
        return true;
    }
    GLenum st = glClientWaitSync((GLsync)r->gl_sync, 0, 0);   /* zero timeout */
    if (st == GL_ALREADY_SIGNALED || st == GL_CONDITION_SATISFIED) {
        glDeleteSync((GLsync)r->gl_sync);
        r->gl_sync = NULL;
        r->gl_frames_done++;
        return true;
    }
    return false;                 /* GL_TIMEOUT_EXPIRED or GL_WAIT_FAILED */
}

/* OpenGL has no asynchronous per-submission OUTCOME to report: a GL error surfaces
 * synchronously through glGetError, and device loss needs ARB_robustness, which is
 * not in the 3.3 core profile this context asks for. So `failed` is always 0 —
 * meaning "this backend cannot answer", NOT "the GL path never fails a frame" —
 * and `ok` counts the frames whose fence has been observed signalled, which is a
 * true count of frames the GPU finished. */
void renderer_gpu_frame_counts(Renderer *r, u64 *out_ok, u64 *out_failed,
                               u32 *out_fail_run) {
    if (out_ok)     *out_ok = r ? r->gl_frames_done : 0;
    if (out_failed) *out_failed = 0;
    /* No failures to run, by the same limitation: with no outcome to report there
     * is no consecutive-failure count either. */
    if (out_fail_run) *out_fail_run = 0;
}

void renderer_metal_upload_stats(u64 *out_uploads, u64 *out_bytes, u64 *out_ns) {
    if (out_uploads) *out_uploads = 0;
    if (out_bytes)   *out_bytes   = 0;
    if (out_ns)      *out_ns      = 0;
}

/* The backend contract's upload half, on OpenGL. font.c has no uploader
 * here (FontAtlas.gpu_uploader stays NULL) and writes its texels with
 * glTexSubImage2D on the context thread, which GL orders ahead of every
 * later draw — the ordering the Metal queue exists to reconstruct. So the
 * three entry points are the immediate path: an upload goes straight to
 * the texture and the ready version is the content version. The stats
 * still count, so a probe reads the same shape on both backends. */
static RendererAtlasStats s_gl_atlas_stats;

bool renderer_atlas_upload(void *uploader, void *texture, i32 x, i32 y, i32 w, i32 h,
                           const void *data, usize src_bytes, u32 content_version) {
    (void)uploader; (void)content_version;
    GLuint tex = (GLuint)(usize)texture;
    if (!tex || !data || x < 0 || y < 0 || w <= 0 || h <= 0) return false;
    /* The R8 atlas is the only texture font.c hands this backend by id;
     * a colour atlas goes through its own glTexSubImage2D in font.c. The
     * source bound is the same rule as the Metal side: refuse to read past
     * what the caller owns. */
    usize need = (usize)w * (usize)h;
    if (need > src_bytes) return false;
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RED, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    s_gl_atlas_stats.records++;
    s_gl_atlas_stats.bytes += need;
    s_gl_atlas_stats.blits++;
    return true;
}

bool renderer_atlas_clear(void *uploader, void *texture, i32 x, i32 y, i32 w, i32 h,
                          u32 content_version) {
    if (w <= 0 || h <= 0) return false;
    usize n = (usize)w * (usize)h;
    u8 *zeros = calloc(1, n);
    if (!zeros) return false;
    bool ok = renderer_atlas_upload(uploader, texture, x, y, w, h, zeros, n, content_version);
    free(zeros);
    return ok;
}

void renderer_atlas_texture_replaced(void *uploader, i32 kind, void *old_tex,
                                     void *new_tex, i32 copy_w, i32 copy_h) {
    /* GL grows by re-creating the texture and re-uploading the whole CPU
     * bitmap in font.c, on the context thread, so there is nothing to
     * order or retire here. */
    (void)uploader; (void)kind; (void)old_tex; (void)new_tex; (void)copy_w; (void)copy_h;
    s_gl_atlas_stats.retired++;
}

void renderer_atlas_stats(const Renderer *r, RendererAtlasStats *out) {
    (void)r;
    if (out) *out = s_gl_atlas_stats;
}

bool renderer_init(Renderer *r, f32 dpi_scale) {
    memset(r, 0, sizeof(*r));
    /* Text knobs, resolved once — see the Renderer comment in renderer.h. Kept
     * identical to the Metal backend's copy in renderer_metal.m.
     *
     * Must run AFTER the memset, or every value computed here would be zeroed
     * on the next line and LIU_TEXT_CONTRAST and LIU_TEXT_BLEND would have no
     * effect. */
    {
        const char *e = getenv("LIU_TEXT_CONTRAST");
        f32 v = e ? (f32)atof(e) : 0.0f;
        r->text_min_contrast = (v > 1.0f) ? v : 0.0f;
        /* Blend correction is ON unless asked for plain linear blending —
         * see renderer.h's text_blend_correct. */
        const char *b = getenv("LIU_TEXT_BLEND");
        r->text_blend_correct = (b && strcmp(b, "linear") == 0) ? 0.0f : 1.0f;
    }
    /* Opaque until told otherwise, matching the Metal backend's init. The
     * memset leaves alpha at 0, which reads as "fully translucent window" to
     * renderer_draw_rrect_translucent on the very first frame. */
    r->clear_color[3] = 1.0f;
    r->dpi_scale = dpi_scale;
    /* Opaque until the frame loop says otherwise: a zeroed value would
     * read as fully transparent and boost glyph alpha on an opaque
     * window, which is exactly the look the boost exists to avoid. */
    r->win_opacity = 1.0f;
    renderer_init_colors();

    /* Per-renderer image cache — GL texture names are context-local, so this
     * must not be shared between windows. ~6 KB; failure is not fatal (the
     * image paths just no-op, see gl_image_texture). */
    r->gl_image_cache = calloc(1, sizeof(GLImageCache));

    /* --- Glyph instanced shader --- */
    u32 gv = compile_shader(glyph_vert_src, GL_VERTEX_SHADER);
    u32 gf = compile_shader(glyph_frag_src, GL_FRAGMENT_SHADER);
    if (!gv || !gf) {
        /* Delete whichever shader did compile so it doesn't leak. */
        if (gv) glDeleteShader(gv);
        if (gf) glDeleteShader(gf);
        return false;
    }
    /* link_program consumes (deletes) gv/gf on both success and failure. */
    r->glyph_shader = link_program(gv, gf);
    if (!r->glyph_shader) return false;

    r->glyph_u_projection = glGetUniformLocation(r->glyph_shader, "u_projection");
    r->glyph_u_atlas      = glGetUniformLocation(r->glyph_shader, "u_atlas");
    r->glyph_u_color_atlas = glGetUniformLocation(r->glyph_shader, "u_color_atlas");
    r->glyph_u_cell_size  = glGetUniformLocation(r->glyph_shader, "u_cell_size");
    r->glyph_u_text_gamma = glGetUniformLocation(r->glyph_shader, "u_text_gamma");
    r->glyph_u_min_contrast = glGetUniformLocation(r->glyph_shader, "u_min_contrast");
    r->glyph_u_blend_correct = glGetUniformLocation(r->glyph_shader, "u_blend_correct");
    r->glyph_u_text_weight = glGetUniformLocation(r->glyph_shader, "u_text_weight");

    /* Unit quad for glyph instances */
    f32 quad[] = { 0,0, 1,0, 1,1, 0,0, 1,1, 0,1 };
    const f32 glyph_quad[] = { 1,0, 1,1, 0,0, 0,1 };

    glGenVertexArrays(1, &r->glyph_vao);
    glBindVertexArray(r->glyph_vao);

    /* Shared quad geometry (location 0) */
    glGenBuffers(1, &r->glyph_quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->glyph_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(glyph_quad), glyph_quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(0);

    /* Per-instance data (locations 1-4) */
    glGenBuffers(1, &r->glyph_instance_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->glyph_instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, INIT_GLYPHS * sizeof(GlyphInstance), NULL, GL_STREAM_DRAW);

    #define GOFF(field) (void*)offsetof(GlyphInstance, field)
    usize stride = sizeof(GlyphInstance);
    /* a_pos: vec2 */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, (GLsizei)stride, GOFF(x));
    glEnableVertexAttribArray(1);
    glVertexAttribDivisor(1, 1);
    /* a_rect: vec4 (ax0,ay0,ax1,ay1) — texels, normalised in the shader */
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, (GLsizei)stride, GOFF(ax0));
    glEnableVertexAttribArray(2);
    glVertexAttribDivisor(2, 1);
    /* a_fg / a_bg: normalised unsigned bytes. GL_TRUE divides by 255 on the
     * way in, so the shader sees the sRGB channel as 0..1 and decodes from
     * there; a_fg.a is the per-instance alpha multiplier and a_bg.a is the
     * "background known" flag. */
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, GOFF(fg));
    glEnableVertexAttribArray(3);
    glVertexAttribDivisor(3, 1);
    glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, GOFF(bg));
    glEnableVertexAttribArray(4);
    glVertexAttribDivisor(4, 1);
    /* a_extra: (is_color, w_cells), also normalised — the shader multiplies by
     * 255 to recover the integer. is_color and w_cells are adjacent in
     * GlyphInstance, which is what lets one attribute cover both. */
    glVertexAttribPointer(5, 2, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, GOFF(tex_kind));
    glEnableVertexAttribArray(5);
    glVertexAttribDivisor(5, 1);
    #undef GOFF

    glBindVertexArray(0);

    if (!renderer_reserve_glyphs(r, INIT_GLYPHS)) return false;

    /* --- Rect shader (instanced) --- */
    u32 rv = compile_shader(rect_vert_src, GL_VERTEX_SHADER);
    u32 rf = compile_shader(rect_frag_src, GL_FRAGMENT_SHADER);
    if (!rv || !rf) {
        /* Delete whichever shader did compile so it doesn't leak. */
        if (rv) glDeleteShader(rv);
        if (rf) glDeleteShader(rf);
        return false;
    }
    /* link_program consumes (deletes) rv/rf on both success and failure. */
    r->rect_shader = link_program(rv, rf);
    if (!r->rect_shader) return false;

    r->rect_u_projection = glGetUniformLocation(r->rect_shader, "u_projection");

    glGenVertexArrays(1, &r->rect_vao);
    glBindVertexArray(r->rect_vao);

    /* Shared unit quad geometry (location 0) */
    glGenBuffers(1, &r->rect_quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->rect_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(0);

    /* Per-instance data (locations 1–3) */
    glGenBuffers(1, &r->rect_instance_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->rect_instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, INIT_RECTS * sizeof(RectInstance), NULL, GL_STREAM_DRAW);

    #define ROFF(field) (void*)offsetof(RectInstance, field)
    GLsizei rstride = (GLsizei)sizeof(RectInstance);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, rstride, ROFF(x));
    glEnableVertexAttribArray(1);
    glVertexAttribDivisor(1, 1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, rstride, ROFF(w));
    glEnableVertexAttribArray(2);
    glVertexAttribDivisor(2, 1);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, rstride, ROFF(r));
    glEnableVertexAttribArray(3);
    glVertexAttribDivisor(3, 1);
    #undef ROFF

    glBindVertexArray(0);

    if (!renderer_reserve_rects(r, INIT_RECTS)) return false;

    /* --- Line shader (instanced AA segments) --- */
    u32 lv = compile_shader(line_vert_src, GL_VERTEX_SHADER);
    u32 lf = compile_shader(line_frag_src, GL_FRAGMENT_SHADER);
    if (lv && lf) {
        r->line_shader = link_program(lv, lf);
        if (r->line_shader) {
            r->line_u_projection = glGetUniformLocation(r->line_shader, "u_projection");
            glGenVertexArrays(1, &r->line_vao);
            glBindVertexArray(r->line_vao);
            glGenBuffers(1, &r->line_quad_vbo);
            glBindBuffer(GL_ARRAY_BUFFER, r->line_quad_vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
            glEnableVertexAttribArray(0);
            glGenBuffers(1, &r->line_instance_vbo);
            glBindBuffer(GL_ARRAY_BUFFER, r->line_instance_vbo);
            glBufferData(GL_ARRAY_BUFFER, INIT_LINES * sizeof(LineInstance), NULL, GL_STREAM_DRAW);
            #define LOFF(f) (void*)offsetof(LineInstance, f)
            GLsizei lstride = (GLsizei)sizeof(LineInstance);
            glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, lstride, LOFF(x0));
            glEnableVertexAttribArray(1); glVertexAttribDivisor(1, 1);
            glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, lstride, LOFF(thickness));
            glEnableVertexAttribArray(2); glVertexAttribDivisor(2, 1);
            glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, lstride, LOFF(r));
            glEnableVertexAttribArray(3); glVertexAttribDivisor(3, 1);
            #undef LOFF
            glBindVertexArray(0);
            renderer_reserve_lines(r, INIT_LINES);
        }
    }

    /* --- Glyph atlas samplers -------------------------------------------
     *
     * Terminal glyphs sample 1:1 and must not be filtered; UI chrome and the
     * sized rasters MAGNIFY (or shrink) on purpose and want bilinear. The two
     * cannot be expressed by the texture object alone, because both draw from
     * the SAME atlas texture. Linear everywhere costs the terminal: a bilinear
     * tap on a quad whose height lands on a half pixel puts the texel grid out
     * of step and smears every horizontal stem, a soft blur rather than a
     * visible misalignment. Nearest everywhere makes the tab strip and sidebar
     * text mushy.
     *
     * Sampler objects (core since 3.3, the version this context asks for)
     * override the texture's own filter state per texture unit, so the choice
     * moves to the draw where it belongs — the same split the Metal backend
     * makes.
     *
     * Nearest is safe on the terminal path because it is pixel-exact by
     * construction on all four axes: the cell is integral (roundf/ceilf in
     * font.c), the glyph origin is floored (ui.c), the pane's vertical padding
     * is truncated to whole pixels (layout.h), and the atlas is a power of two
     * so the normalised UV of an integer box is bit-exact. */
    glGenSamplers(1, &r->glyph_sampler_linear);
    glSamplerParameteri(r->glyph_sampler_linear, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(r->glyph_sampler_linear, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(r->glyph_sampler_linear, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(r->glyph_sampler_linear, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenSamplers(1, &r->glyph_sampler_nearest);
    glSamplerParameteri(r->glyph_sampler_nearest, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(r->glyph_sampler_nearest, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(r->glyph_sampler_nearest, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(r->glyph_sampler_nearest, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* --- Rounded-rect shader (SDF + soft shadow + border + inner shadow) --- */
    u32 rrv = compile_shader(rrect_vert_src, GL_VERTEX_SHADER);
    u32 rrf = compile_shader(rrect_frag_src, GL_FRAGMENT_SHADER);
    if (rrv && rrf) {
        r->rrect_shader = link_program(rrv, rrf);
        if (r->rrect_shader) {
            r->rrect_u_projection = glGetUniformLocation(r->rrect_shader, "u_projection");

            glGenVertexArrays(1, &r->rrect_vao);
            glBindVertexArray(r->rrect_vao);

            /* Shared unit quad (location 0) */
            glGenBuffers(1, &r->rrect_quad_vbo);
            glBindBuffer(GL_ARRAY_BUFFER, r->rrect_quad_vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
            glEnableVertexAttribArray(0);

            /* Per-instance vec4 attributes (locations 1..6). Each maps to one
             * vec4 in the RRectInstance struct — keeps the GPU layout simple. */
            glGenBuffers(1, &r->rrect_instance_vbo);
            glBindBuffer(GL_ARRAY_BUFFER, r->rrect_instance_vbo);
            glBufferData(GL_ARRAY_BUFFER, INIT_RRECTS * sizeof(RRectInstance),
                         NULL, GL_STREAM_DRAW);

            #define RROFF(field) (void*)offsetof(RRectInstance, field)
            GLsizei rrs = (GLsizei)sizeof(RRectInstance);
            glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, rrs, RROFF(x));
            glEnableVertexAttribArray(1); glVertexAttribDivisor(1, 1);
            glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, rrs, RROFF(r));
            glEnableVertexAttribArray(2); glVertexAttribDivisor(2, 1);
            glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, rrs, RROFF(r_tl));
            glEnableVertexAttribArray(3); glVertexAttribDivisor(3, 1);
            glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, rrs, RROFF(shadow_size));
            glEnableVertexAttribArray(4); glVertexAttribDivisor(4, 1);
            glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, rrs, RROFF(border_r));
            glEnableVertexAttribArray(5); glVertexAttribDivisor(5, 1);
            glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, rrs, RROFF(border_width));
            glEnableVertexAttribArray(6); glVertexAttribDivisor(6, 1);
            #undef RROFF

            glBindVertexArray(0);

            if (!renderer_reserve_rrects(r, INIT_RRECTS)) return false;
        }
    }
    /* If init failed, draw_rrect/_bordered/_inset fall back to flat rects. */

    return r->glyph_instances && r->rect_batch;
}

static void gl_retained_destroy(RetainedBatch *batch);
static void gl_retained_trim(Renderer *r, f64 now);

void renderer_destroy(Renderer *r) {
    retained_collect(r, true, gl_retained_destroy);
    if (r->glyph_sampler_linear)  glDeleteSamplers(1, &r->glyph_sampler_linear);
    if (r->glyph_sampler_nearest) glDeleteSamplers(1, &r->glyph_sampler_nearest);
    if (r->gl_sync) { glDeleteSync((GLsync)r->gl_sync); r->gl_sync = NULL; }
    r->glyph_sampler_linear = r->glyph_sampler_nearest = 0;
    if (r->glyph_shader) glDeleteProgram(r->glyph_shader);
    if (r->rect_shader) glDeleteProgram(r->rect_shader);
    if (r->rrect_shader) glDeleteProgram(r->rrect_shader);
    if (r->rrect_vao) glDeleteVertexArrays(1, &r->rrect_vao);
    if (r->rrect_quad_vbo) glDeleteBuffers(1, &r->rrect_quad_vbo);
    if (r->rrect_instance_vbo) glDeleteBuffers(1, &r->rrect_instance_vbo);
    free(r->rrect_batch); r->rrect_batch = NULL;
    if (r->glyph_vao) glDeleteVertexArrays(1, &r->glyph_vao);
    if (r->glyph_quad_vbo) glDeleteBuffers(1, &r->glyph_quad_vbo);
    if (r->glyph_instance_vbo) glDeleteBuffers(1, &r->glyph_instance_vbo);
    if (r->rect_vao) glDeleteVertexArrays(1, &r->rect_vao);
    if (r->rect_quad_vbo) glDeleteBuffers(1, &r->rect_quad_vbo);
    if (r->rect_instance_vbo) glDeleteBuffers(1, &r->rect_instance_vbo);
    /* Tear down the line pipeline created in renderer_init, or every renderer
     * recreate (font reload, DPI / monitor change) leaks a VAO + 2 VBOs + a
     * linked program. */
    if (r->line_shader) glDeleteProgram(r->line_shader);
    if(r->stroke_shader)glDeleteProgram(r->stroke_shader);
    if(r->stroke_vao)glDeleteVertexArrays(1,&r->stroke_vao);
    if (r->line_vao) glDeleteVertexArrays(1, &r->line_vao);
    if (r->line_quad_vbo) glDeleteBuffers(1, &r->line_quad_vbo);
    if (r->line_instance_vbo) glDeleteBuffers(1, &r->line_instance_vbo);
    gl_gpu_timer_destroy(r);
    /* Wallpaper: the texture plus the lazily-built program it is drawn with. */
    renderer_destroy_background_image(r);
    if (r->bg_shader) glDeleteProgram(r->bg_shader);
    if (r->bg_vao) glDeleteVertexArrays(1, &r->bg_vao);
    if (r->bg_quad_vbo) glDeleteBuffers(1, &r->bg_quad_vbo);
    r->bg_shader = r->bg_vao = r->bg_quad_vbo = 0;
    GLImageCache *ic = r->gl_image_cache;
    if (ic) {
        for (u32 i = 0; i < GL_IMAGE_CACHE_CAP; i++) {
            if (ic->entries[i].texture)
                glDeleteTextures(1, &ic->entries[i].texture);
        }
        for (u32 i = 0; i < GL_IMAGE_OVERSIZE_SLOTS; i++) {
            if (ic->oversize[i].texture)
                glDeleteTextures(1, &ic->oversize[i].texture);
        }
        if (ic->accounted_bytes)
            mem_budget_adjust(MEM_IMAGE_GPU, -(isize)ic->accounted_bytes);
        free(ic);
        r->gl_image_cache = NULL;
    }
    free(r->glyph_instances);
    free(r->rect_batch);
    free(r->line_batch);
    font_atlas_destroy(&r->font);
}

/* =========================================================================
 * GPU-time probe (LIU_FRAME_PROBE / bench)
 *
 * The Metal backend reads GPU busy time straight off the command buffer's
 * GPUStartTime/GPUEndTime in its completion handler. OpenGL has no command
 * buffer to ask, so this is the equivalent: one GL_TIME_ELAPSED query around
 * the whole frame, read back a few frames later so nothing ever blocks.
 *
 * Without it, `LIU_FRAME_PROBE=1` would report no GPU time on a non-macOS
 * build — the one number in that line that says whether a slow frame is the
 * app's fault or the GPU's.
 *
 * The fence / gpuwait columns stay zero here and that is correct, not missing:
 * they measure a semaphore wait and a -nextDrawable block, neither of which
 * exists in this backend. GL's equivalent stall lives in the buffer swap, which
 * is the platform layer's, not the renderer's.
 * ========================================================================= */

/* Written by the main loop, read here. Defined in main.c. */
extern _Atomic bool               g_liu_gpu_timing_on;
extern _Atomic unsigned long long g_liu_fprobe_gpu_ns;
extern _Atomic unsigned int       g_liu_fprobe_gpu_cnt;
/* The DISTRIBUTION, which the two counters above cannot answer. main.c reads
 * this for GPU p50/p99; the Metal path feeds it in metal_probe_gpu_time and
 * this backend feeds it here, because the tail is the thing a frame probe
 * exists to show. */
extern LiuHistAtomic              g_liu_fprobe_gpu_hist;

#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif

/* Deep enough that a result is always ready by the time its slot comes round
 * again, so the reader never has to block; shallow enough to stay a rounding
 * error in memory. Mirrors METAL_FRAME_BUFFER_COUNT's role. */
#define GL_GPU_TIMER_SLOTS 4

typedef struct {
    GLuint id[GL_GPU_TIMER_SLOTS];
    bool   pending[GL_GPU_TIMER_SLOTS];   /* issued, result not yet collected */
    u32    head;                          /* slot the next frame writes into */
    bool   open;                          /* a glBeginQuery is outstanding */
    bool   unsupported;                   /* driver has no timer queries */
} GLGpuTimer;

usize renderer_cpu_bytes(const Renderer *r) {
    usize bytes = renderer_batch_cpu_bytes(r);
    if (!r) return bytes;
    if (r->gl_image_cache) bytes += sizeof(GLImageCache);
    if (r->gl_gpu_timer) bytes += sizeof(GLGpuTimer);
    return bytes;
}

/* Timer queries sit in gl_loader's OPTIONAL tier — deliberately, because a
 * missing REQUIRED entry point makes platform_linux.c destroy the window, and
 * the frame probe's gpu-ms column must never be able to do that. So the
 * pointers can legitimately be NULL and every use is gated on this. */
static bool gl_timer_queries_available(void) {
#ifdef LIU_GL_REDIRECT
    return liu_glGenQueries && liu_glDeleteQueries && liu_glBeginQuery &&
           liu_glEndQuery && liu_glGetQueryObjectiv && liu_glGetQueryObjectui64v;
#else
    return true;   /* macOS: prototyped by <OpenGL/gl3.h>, always linked */
#endif
}

static void gl_gpu_timer_destroy(Renderer *r) {
    GLGpuTimer *t = r->gl_gpu_timer;
    if (!t) return;
    /* An open query must be closed before its object is deleted, or the driver
     * is left with a query that can never complete. */
    if (t->open) { glEndQuery(GL_TIME_ELAPSED); t->open = false; }
    for (u32 i = 0; i < GL_GPU_TIMER_SLOTS; i++)
        if (t->id[i]) glDeleteQueries(1, &t->id[i]);
    free(t);
    r->gl_gpu_timer = NULL;
}

/* Collect whatever has finished, then open this frame's query. Called from
 * renderer_begin_frame; paired with gl_gpu_timer_end. */
static void gl_gpu_timer_begin(Renderer *r) {
    bool want = atomic_load_explicit(&g_liu_gpu_timing_on, memory_order_relaxed);
    GLGpuTimer *t = r->gl_gpu_timer;

    if (!want) {
        /* Timing was switched off — give the query objects back rather than
         * holding them for a session that will never read them again. */
        if (t) gl_gpu_timer_destroy(r);
        return;
    }
    if (!gl_timer_queries_available()) return;
    if (!t) {
        t = calloc(1, sizeof(GLGpuTimer));
        if (!t) return;
        r->gl_gpu_timer = t;
    }
    if (t->unsupported) return;

    /* Drain every finished query. Polled, never waited on: asking for the
     * result of a query the GPU has not reached blocks until it does, which
     * would turn a diagnostic into the stall it is meant to measure. */
    for (u32 i = 0; i < GL_GPU_TIMER_SLOTS; i++) {
        if (!t->pending[i] || !t->id[i]) continue;
        GLint ready = 0;
        glGetQueryObjectiv(t->id[i], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) continue;
        unsigned long long ns = 0;
        glGetQueryObjectui64v(t->id[i], GL_QUERY_RESULT, &ns);
        t->pending[i] = false;
        if (ns == 0) continue;
        liu_hist_atomic_add_ns(&g_liu_fprobe_gpu_hist, ns);
        atomic_fetch_add(&g_liu_fprobe_gpu_ns, ns);
        atomic_fetch_add(&g_liu_fprobe_gpu_cnt, 1u);
    }

    /* Skip a frame rather than overwrite a slot whose result never arrived. */
    if (t->pending[t->head]) return;
    if (!t->id[t->head]) {
        glGenQueries(1, &t->id[t->head]);
        if (!t->id[t->head]) { t->unsupported = true; return; }
    }
    glBeginQuery(GL_TIME_ELAPSED, t->id[t->head]);
    t->open = true;
}

static void gl_gpu_timer_end(Renderer *r) {
    GLGpuTimer *t = r->gl_gpu_timer;
    if (!t || !t->open) return;
    glEndQuery(GL_TIME_ELAPSED);
    t->open = false;
    t->pending[t->head] = true;
    t->head = (t->head + 1) % GL_GPU_TIMER_SLOTS;
}

/* =========================================================================
 * Frame
 * ========================================================================= */

static void ortho(f32 *m, f32 w, f32 h) {
    memset(m, 0, 16 * sizeof(f32));
    m[0]  =  2.0f / w;
    m[5]  = -2.0f / h;
    m[10] = -1.0f;
    m[12] = -1.0f;
    m[13] =  1.0f;
    m[15] =  1.0f;
}

void renderer_begin_frame(Renderer *r, i32 w, i32 h) {
    retained_collect(r, false, gl_retained_destroy);
    r->frame_deferred = false;
    r->cursor_blink_in_frame = false;
    r->frame_waiting_drawable = false;
    r->screen_width  = w;
    r->screen_height = h;
    r->glyph_count   = 0;
    r->rect_batch_count = 0;
    r->rrect_batch_count = 0;
    r->last_drawable_wait_sec = 0.0;
#ifdef LIU_RENDER_REPLAY_PROBE
    r->replay_cache_to_renderer_bytes = 0;
    r->replay_cache_to_renderer_ns = 0;
    r->replay_renderer_to_backend_bytes = 0;
    r->replay_renderer_to_backend_ns = 0;
    r->replay_glyph_instances = 0;
    r->replay_rect_instances = 0;
    r->replay_deco_instances = 0;
    r->replay_terminal_panes = 0;
    r->replay_rows_rebuilt = 0;
    r->replay_terminal_upload = false;
#endif
    /* Not flushed by renderer_end_frame (the line pipeline is additive, so a
     * late flush would paint stale coordinates over an unrelated frame). No
     * producer leaks one today — every renderer_draw_line region ends in an
     * unconditional renderer_flush_lines — so this only bounds the damage if
     * one ever does. Do NOT add renderer_flush_lines to end_frame instead: it
     * would DRAW the leaked batch rather than drop it, and inserting it into
     * the rects->rrects->glyphs order breaks the layering invariant. */
    r->line_batch_count = 0;

    gl_gpu_timer_begin(r);

    GLImageCache *ic = r->gl_image_cache;
    if (ic) {
        ic->frame_serial++;
        ic->now_sec = platform_time_sec();
    }

    glViewport(0, 0, w, h);
    /* sRGB-correct blending: GPU converts sRGB<->linear automatically */
    glEnable(GL_FRAMEBUFFER_SRGB);
    /* glClear obeys the scissor box: if a prior frame left the scissor enabled
     * and clipped to a sub-rectangle, the clear wipes only that rectangle and a
     * freshly enlarged window keeps a transparent hole outside it. Reset first
     * so the clear always covers the whole framebuffer. Cheap insurance — the
     * scissor is normally already off by here. */
    if (r->scissor_active) {
        glDisable(GL_SCISSOR_TEST);
        r->scissor_active = false;
    }
    /* Pass 0: clear with bg color (set by caller via glClearColor before this) */
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);

    /* Only recompute projection + bump uniform version when size changes.
     * Cell size also tracked so font reloads / UI scale changes invalidate it. */
    f32 cell_w = r->font.loaded ? r->font.cell_width  : 0.0f;
    f32 cell_h = r->font.loaded ? r->font.cell_height : 0.0f;
    if (w != r->cached_proj_w || h != r->cached_proj_h) {
        ortho(r->projection, (f32)w, (f32)h);
        r->cached_proj_w = w;
        r->cached_proj_h = h;
        r->uniforms_version++;
    }
    if (cell_w != r->cached_cell_w || cell_h != r->cached_cell_h) {
        r->cached_cell_w = cell_w;
        r->cached_cell_h = cell_h;
        r->uniforms_version++;
    }

    /* Only upload uniforms to shaders that have fallen behind the version */
    if (r->gl_rect_uniforms_version != r->uniforms_version) {
        glUseProgram(r->rect_shader);
        glUniformMatrix4fv(r->rect_u_projection, 1, GL_FALSE, r->projection);
        r->gl_rect_uniforms_version = r->uniforms_version;
    }
    if (r->rrect_shader && r->gl_rrect_uniforms_version != r->uniforms_version) {
        glUseProgram(r->rrect_shader);
        glUniformMatrix4fv(r->rrect_u_projection, 1, GL_FALSE, r->projection);
        r->gl_rrect_uniforms_version = r->uniforms_version;
    }
    if (r->gl_glyph_uniforms_version != r->uniforms_version) {
        glUseProgram(r->glyph_shader);
        glUniformMatrix4fv(r->glyph_u_projection, 1, GL_FALSE, r->projection);
        glUniform1i(r->glyph_u_atlas, 0);
        glUniform1i(r->glyph_u_color_atlas, 1);
        glUniform1f(r->glyph_u_text_gamma, font_text_gamma());
        glUniform1f(r->glyph_u_min_contrast, r->text_min_contrast);
        glUniform1f(r->glyph_u_blend_correct, r->text_blend_correct);
        if (r->font.loaded) {
            glUniform2f(r->glyph_u_cell_size, cell_w, cell_h);
            r->gl_glyph_cell_size[0] = cell_w;
            r->gl_glyph_cell_size[1] = cell_h;
        }
        r->gl_glyph_uniforms_version = r->uniforms_version;
    }
}

/* Capture the finished back buffer as RGBA8 before window compositing. The dump
 * includes the color and alpha values produced by the renderer. */
static void gl_debug_dump_backbuffer(Renderer *r, const char *path) {
    i32 w = r->screen_width, h = r->screen_height;
    if (w <= 0 || h <= 0) return;
    usize row = 0, total = 0;
    if (!checked_mul_usize((usize)w, 4u, &row) ||
        !checked_mul_usize(row, (usize)h, &total)) return;

    u8 *buf = malloc(total);
    if (!buf) return;

    /* The scissor is off by here (pop_scissor ran, or it was never pushed), but
     * glReadPixels ignores it either way — what it does obey is the pack
     * alignment, and a width that is not a multiple of 4 would otherwise be
     * read with padded rows. */
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "liu: LIU_DUMP_FRAME could not open %s\n", path);
        free(buf);
        return;
    }
    fprintf(f, "LIUFB %d %d rgba\n", w, h);
    /* GL hands back bottom-up; Metal's dump is top-down. Flip on the way out so
     * the two files mean the same thing. */
    for (i32 y = h - 1; y >= 0; y--) fwrite(buf + (usize)y * row, 1, row, f);
    fclose(f);
    fprintf(stderr, "liu: dumped %dx%d rgba frame to %s\n", w, h, path);
    free(buf);
}

void renderer_gl_frame_fence(Renderer *r);   /* fwd: defined with renderer_gpu_idle */

void renderer_end_frame(Renderer *r) {
    /* Flush any remaining batched draws, in layering order: rects and rrects
     * before glyphs, so a late rect cannot land on top of text already drawn. */
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);

    /* Armed dump, taken after the last draw and before main.c swaps — the
     * pixels that are about to be shown.
     *
     * The env is read ONCE, not per frame. getenv walks the environment array
     * linearly and this is the end of every presented frame, a cost a
     * diagnostic that is off in every real session should not add. Same
     * convention as gl_glyph_nearest(): resolve on first use, keep a small
     * static state machine after that.
     *
     * Frame 0 is an empty terminal — the shell has not printed its prompt yet,
     * so a dump taken there shows nothing worth measuring. Wait a beat
     * (LIU_DUMP_FRAME_AFTER, default ~2s at 60 Hz) before firing. */
    static i32  dump_state = -1;     /* -1 unresolved, 0 disarmed, 1 armed */
    static i32  dump_countdown = 0;
    static char dump_path[4096];
    if (dump_state < 0) {
        const char *p = liu_dev_env("LIU_DUMP_FRAME");
        if (p && *p) {
            snprintf(dump_path, sizeof dump_path, "%s", p);
            const char *after = liu_dev_env("LIU_DUMP_FRAME_AFTER");
            dump_countdown = after ? atoi(after) : 120;
            dump_state = 1;
        } else {
            dump_state = 0;
        }
    }
    if (dump_state == 1 && dump_countdown-- <= 0) {
        dump_state = 0;              /* one shot */
        gl_debug_dump_backbuffer(r, dump_path);
    }

    gl_gpu_timer_end(r);

    /* After the last draw of the frame: the fence renderer_gpu_idle() reads. It
     * has to be placed here and not at the top of the next frame, because "every
     * draw of this frame has finished" is the statement the atlas shelf reclaim
     * needs and only a fence AFTER those draws makes it. */
    renderer_gl_frame_fence(r);
}

usize renderer_reclaim_image_cache(Renderer *r) {
    GLImageCache *ic = r ? r->gl_image_cache : NULL;
    if (!ic) return 0;
    usize before = ic->accounted_bytes;
    f64 now = platform_time_sec();
    /* Under pressure the 15 s idle window is not the question — anything not
     * drawn in the last few frames is cold, and a dropped entry costs a
     * re-upload from the CPU image its owner still holds, never a picture.
     * The 0.1 s grace is what keeps a texture drawn every frame out of it.
     * Same policy and the same grace as the Metal backend's. */
    for (u32 i = 0; i < GL_IMAGE_CACHE_CAP; i++) {
        GLImageCacheEntry *e = &ic->entries[i];
        if (e->texture && e->last_used_sec > 0.0 &&
            now - e->last_used_sec > 0.1)
            gl_image_cache_release_entry(ic, e);
    }
    /* The oversize slot is the single most expensive texture here and no
     * byte-budget pressure inside the pool will ever reclaim it, so pressure
     * has to ask for it explicitly — which is the whole reason this function
     * exists rather than just lowering the idle window. */
    for (u32 i = 0; i < GL_IMAGE_OVERSIZE_SLOTS; i++) {
        GLImageCacheEntry *o = &ic->oversize[i];
        if (o->texture && o->last_used_sec > 0.0 &&
            now - o->last_used_sec > 0.1)
            gl_oversize_release(ic, o);
    }
    return before > ic->accounted_bytes ? before - ic->accounted_bytes : 0;
}

void renderer_trim_idle_resources(Renderer *r, f64 now_sec) {
    if (!r) return;
    if (now_sec <= 0.0) now_sec = platform_time_sec();
    /* Runs on the owning context, even if no image cache was ever created.
     * GL keeps storage used by submitted draws alive after object deletion. */
    retained_collect(r, false, gl_retained_destroy);
    gl_retained_trim(r, now_sec);
    GLImageCache *ic = r->gl_image_cache;
    if (!ic) return;
    ic->now_sec = now_sec;
    for (u32 i = 0; i < GL_IMAGE_CACHE_CAP; i++) {
        GLImageCacheEntry *e = &ic->entries[i];
        if (e->texture && e->last_used_sec > 0.0 &&
            now_sec - e->last_used_sec > GL_IMAGE_CACHE_IDLE_SEC)
            gl_image_cache_release_entry(ic, e);
    }
    for (u32 i = 0; i < GL_IMAGE_OVERSIZE_SLOTS; i++) {
        GLImageCacheEntry *o = &ic->oversize[i];
        if (o->texture && o->last_used_sec > 0.0 &&
            now_sec - o->last_used_sec > GL_IMAGE_OVERSIZE_IDLE_SEC)
            gl_oversize_release(ic, o);
    }
}

/* =========================================================================
 * Pass 1 + 3: Rect batching
 * ========================================================================= */

void renderer_draw_rect(Renderer *r, f32 x, f32 y, f32 w, f32 h, Color c) {
    renderer_blur_probe_draw(r, x, y, w, h, c.a, "rect");
    if (r->rect_batch_count >= r->rect_batch_cap) renderer_flush_rects(r);
    if (r->rect_batch_count >= r->rect_batch_cap &&
        !renderer_reserve_rects(r, r->rect_batch_count + 1)) {
        return;
    }

    /* Pixel-align for crisp edges */
    x = floorf(x); y = floorf(y);
    w = ceilf(w); h = ceilf(h);

    /* sRGB → linear at the CPU → GPU boundary (see renderer.h banner). */
    Color lin = color_linear_from_srgb(c);
    RectInstance *ri = &r->rect_batch[r->rect_batch_count++];
    ri->x = x; ri->y = y;
    ri->w = w; ri->h = h;
    ri->r = lin.r; ri->g = lin.g; ri->b = lin.b; ri->a = lin.a;
}


static void gl_blend_premultiplied(Renderer *r) {
    (void)r;
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void renderer_flush_rects(Renderer *r) {
    if (r->rect_batch_count == 0) return;

    /* Rects use standard alpha blending for semi-transparent elements */
    glEnable(GL_BLEND);
    /* Alpha blends as a straight over (ONE, 1-SrcAlpha) while RGB keeps
     * SrcAlpha: with both on SrcAlpha the alpha channel gets multiplied by
     * coverage twice and lags behind the colour, which on a translucent
     * window shows up as surfaces that look brighter than their own alpha. */
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(r->rect_shader);
    RetainedBatch *retained = r->retained_draw_buffer;
    glBindVertexArray(retained ? retained->vao : r->rect_vao);
    if (!retained) {
    glBindBuffer(GL_ARRAY_BUFFER, r->rect_instance_vbo);

    /* orphan before upload */
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(r->rect_batch_cap * sizeof(RectInstance)),
                 NULL, GL_STREAM_DRAW);
    usize bytes = r->rect_batch_count * sizeof(RectInstance);
#ifdef LIU_RENDER_REPLAY_PROBE
    f64 replay_t0 = r->replay_terminal_upload ? platform_time_sec() : 0.0;
#endif
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, r->rect_batch);
#ifdef LIU_RENDER_REPLAY_PROBE
    if (r->replay_terminal_upload) {
        r->replay_renderer_to_backend_bytes += bytes;
        r->replay_renderer_to_backend_ns +=
            (u64)((platform_time_sec() - replay_t0) * 1e9);
    }
#endif

    }
    /* Single instanced draw call for ALL batched rects */
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)r->rect_batch_count);
    g_liu_draw_calls++; g_liu_draw_instances += r->rect_batch_count;

    r->rect_batch_count = 0;
}

/* =========================================================================
 * Line batching — thick AA segments (graph-view edges, OpenGL backend)
 * ========================================================================= */

void renderer_draw_line(Renderer *r, f32 x0, f32 y0, f32 x1, f32 y1,
                        f32 thickness, Color c) {
    if(r->line_batch_count&&r->line_batch_stroke)renderer_flush_lines(r);
    r->line_batch_stroke=false;
    if (r->line_batch_count >= r->line_batch_cap) renderer_flush_lines(r);
    if (r->line_batch_count >= r->line_batch_cap &&
        !renderer_reserve_lines(r, r->line_batch_count + 1)) {
        return;
    }
    Color lin = color_linear_from_srgb(c);
    LineInstance *li = &r->line_batch[r->line_batch_count++];
    li->x0 = x0; li->y0 = y0; li->x1 = x1; li->y1 = y1;
    li->thickness = thickness < 0.5f ? 0.5f : thickness;
    li->r = lin.r; li->g = lin.g; li->b = lin.b; li->a = lin.a;
}

void renderer_draw_stroke_segment(Renderer *r,f32 x0,f32 y0,f32 x1,f32 y1,
                                 f32 nx0,f32 ny0,f32 nx1,f32 ny1,f32 thickness,Color c) {
    if(r->line_batch_count&&!r->line_batch_stroke)renderer_flush_lines(r);
    if(r->line_batch_count+2>r->line_batch_cap)renderer_flush_lines(r);
    if(r->line_batch_count+2>r->line_batch_cap&&!renderer_reserve_lines(r,r->line_batch_count+2))return;
    Color lin=color_linear_from_srgb(c);
    LineInstance *li=&r->line_batch[r->line_batch_count];
    li[0]=(LineInstance){x0,y0,x1,y1,fmaxf(.5f,thickness),lin.r,lin.g,lin.b,lin.a};
    li[1]=(LineInstance){.x0=nx0,.y0=ny0,.x1=nx1,.y1=ny1};
    r->line_batch_count+=2;r->line_batch_stroke=true;
}

static bool ensure_stroke_shader(Renderer *r) {
    if(r->stroke_shader)return true;
    if(r->stroke_shader_tried)return false;
    r->stroke_shader_tried=true;
    if(!r->line_instance_vbo||!r->line_quad_vbo)return false;
    const char *vs="#version 330 core\n"
    "layout(location=0) in vec2 q;layout(location=1) in vec4 p;layout(location=2) in float width;layout(location=3) in vec4 color;layout(location=4) in vec4 normals;\n"
    "uniform mat4 u_projection;out vec4 v_color;out float v_dist;out float v_radius;\n"
    "void main(){float across=(q.y-.5)*(width+2);vec2 pos=mix(p.xy,p.zw,q.x)+mix(normals.xy,normals.zw,q.x)*across;gl_Position=u_projection*vec4(pos,0,1);v_color=color;v_dist=across;v_radius=width*.5;}\n";
    const char *fs="#version 330 core\nin vec4 v_color;in float v_dist;in float v_radius;out vec4 frag_color;\n"
    "void main(){float aa=max(fwidth(v_dist),.75);float cov=1-smoothstep(v_radius-aa*.5,v_radius+aa*.5,abs(v_dist));frag_color=vec4(v_color.rgb,v_color.a*cov);}\n";
    u32 v=compile_shader(vs,GL_VERTEX_SHADER),f=compile_shader(fs,GL_FRAGMENT_SHADER);
    if(v&&f)r->stroke_shader=link_program(v,f);
    else {if(v)glDeleteShader(v);if(f)glDeleteShader(f);}
    if(!r->stroke_shader)return false;
    r->stroke_u_projection=glGetUniformLocation(r->stroke_shader,"u_projection");
    glGenVertexArrays(1,&r->stroke_vao);
    if(!r->stroke_vao){glDeleteProgram(r->stroke_shader);r->stroke_shader=0;return false;}
    glBindVertexArray(r->stroke_vao);
    glBindBuffer(GL_ARRAY_BUFFER,r->line_quad_vbo);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,0);glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER,r->line_instance_vbo);
    GLsizei stride=2*(GLsizei)sizeof(LineInstance);
    glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,stride,(void*)offsetof(LineInstance,x0));
    glVertexAttribPointer(2,1,GL_FLOAT,GL_FALSE,stride,(void*)offsetof(LineInstance,thickness));
    glVertexAttribPointer(3,4,GL_FLOAT,GL_FALSE,stride,(void*)offsetof(LineInstance,r));
    glVertexAttribPointer(4,4,GL_FLOAT,GL_FALSE,stride,(void*)sizeof(LineInstance));
    for(u32 i=1;i<=4;i++){glEnableVertexAttribArray(i);glVertexAttribDivisor(i,1);}
    glBindVertexArray(0);return true;
}

void renderer_prepare_strokes(Renderer *r){if(r->line_instance_vbo)ensure_stroke_shader(r);}
void renderer_flush_lines(Renderer *r) {
    if (r->line_batch_count == 0) return;
    if(r->line_batch_stroke&&!ensure_stroke_shader(r)) {
        u32 count=r->line_batch_count/2;for(u32 i=0;i<count;i++)r->line_batch[i]=r->line_batch[i*2];
        r->line_batch_count=count;r->line_batch_stroke=false;
    }
    u32 instances=r->line_batch_stroke?r->line_batch_count/2:r->line_batch_count;
    if (!r->line_shader) { r->line_batch_count = 0; return; }

    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(r->line_batch_stroke?r->stroke_shader:r->line_shader);
    glUniformMatrix4fv(r->line_batch_stroke?r->stroke_u_projection:r->line_u_projection, 1, GL_FALSE, r->projection);
    glBindVertexArray(r->line_batch_stroke?r->stroke_vao:r->line_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->line_instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(r->line_batch_cap * sizeof(LineInstance)),
                 NULL, GL_STREAM_DRAW);
    usize bytes = r->line_batch_count * sizeof(LineInstance);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, r->line_batch);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)instances);
    g_liu_draw_calls++; g_liu_draw_instances += instances;
    r->line_batch_count = 0;
}

/* =========================================================================
 * Rounded-rect SDF rendering (OpenGL backend)
 * GLSL port of the Metal pipeline. If the shader failed to compile/link
 * (rrect_shader == 0) the API gracefully degrades to flat rects, keeping
 * the app visually functional on older drivers.
 * ========================================================================= */

void renderer_draw_rrect_bordered(Renderer *r,
    f32 x, f32 y, f32 w, f32 h,
    Color fill, Color border, f32 border_width,
    f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl,
    f32 shadow_size, f32 shadow_alpha,
    f32 shadow_offset_x, f32 shadow_offset_y)
{
    renderer_blur_probe_draw(r, x, y, w, h, fill.a, "rrect");
    if (!r->rrect_shader) {
        if (border_width > 0.5f) {
            renderer_draw_rect(r, x, y, w, h, border);
            renderer_draw_rect(r, x + border_width, y + border_width,
                               w - 2 * border_width, h - 2 * border_width, fill);
        } else {
            renderer_draw_rect(r, x, y, w, h, fill);
        }
        return;
    }
    if (r->rrect_batch_count >= r->rrect_batch_cap) renderer_flush_rrects(r);
    if (r->rrect_batch_count >= r->rrect_batch_cap &&
        !renderer_reserve_rrects(r, r->rrect_batch_count + 1)) {
        return;
    }

    x = floorf(x); y = floorf(y);
    w = ceilf(w);  h = ceilf(h);

    Color lin_fill   = color_linear_from_srgb(fill);
    Color lin_border = color_linear_from_srgb(border);
    RRectInstance *ri = &r->rrect_batch[r->rrect_batch_count++];
    ri->x = x; ri->y = y; ri->w = w; ri->h = h;
    ri->r = lin_fill.r; ri->g = lin_fill.g; ri->b = lin_fill.b; ri->a = lin_fill.a;
    ri->r_tl = r_tl; ri->r_tr = r_tr; ri->r_br = r_br; ri->r_bl = r_bl;
    ri->shadow_size = shadow_size;
    ri->shadow_alpha = shadow_alpha;
    ri->shadow_offset_y = shadow_offset_y;
    ri->shadow_offset_x = shadow_offset_x;
    ri->border_r = lin_border.r; ri->border_g = lin_border.g;
    ri->border_b = lin_border.b; ri->border_a = lin_border.a;
    ri->border_width = border_width;
    ri->inner_shadow_size = 0.0f;
    ri->inner_shadow_alpha = 0.0f;
    ri->_pad0 = 0.0f;
}

void renderer_draw_rrect(Renderer *r,
    f32 x, f32 y, f32 w, f32 h,
    Color fill,
    f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl,
    f32 shadow_size, f32 shadow_alpha,
    f32 shadow_offset_x, f32 shadow_offset_y)
{
    renderer_draw_rrect_bordered(r, x, y, w, h, fill,
        (Color){0, 0, 0, 0}, 0.0f,
        r_tl, r_tr, r_br, r_bl,
        shadow_size, shadow_alpha, shadow_offset_x, shadow_offset_y);
}

void renderer_draw_rrect_inset(Renderer *r,
    f32 x, f32 y, f32 w, f32 h, Color fill, f32 radius,
    f32 inner_size, f32 inner_alpha)
{
    if (!r->rrect_shader) {
        renderer_draw_rect(r, x, y, w, h, fill);
        return;
    }
    if (r->rrect_batch_count >= r->rrect_batch_cap) renderer_flush_rrects(r);
    if (r->rrect_batch_count >= r->rrect_batch_cap &&
        !renderer_reserve_rrects(r, r->rrect_batch_count + 1)) {
        return;
    }

    x = floorf(x); y = floorf(y);
    w = ceilf(w);  h = ceilf(h);

    Color lin_fill = color_linear_from_srgb(fill);
    RRectInstance *ri = &r->rrect_batch[r->rrect_batch_count++];
    ri->x = x; ri->y = y; ri->w = w; ri->h = h;
    ri->r = lin_fill.r; ri->g = lin_fill.g; ri->b = lin_fill.b; ri->a = lin_fill.a;
    ri->r_tl = ri->r_tr = ri->r_br = ri->r_bl = radius;
    ri->shadow_size = ri->shadow_alpha = 0.0f;
    ri->shadow_offset_y = ri->shadow_offset_x = 0.0f;
    ri->border_r = ri->border_g = ri->border_b = ri->border_a = 0.0f;
    ri->border_width = 0.0f;
    ri->inner_shadow_size = inner_size;
    ri->inner_shadow_alpha = inner_alpha;
    ri->_pad0 = 0.0f;
}

void renderer_flush_rrects(Renderer *r) {
    if (r->rrect_batch_count == 0 || !r->rrect_shader) {
        r->rrect_batch_count = 0;
        return;
    }

    /* Premultiplied alpha output — matches the Metal pipeline's blend.
     * renderer_draw_rrect_translucent asks for the alpha channel to be left
     * alone instead, so a translucent window keeps showing through the shape
     * (see renderer.h); RGB blends the same either way. */
    glEnable(GL_BLEND);
    if (r->rrect_keep_dst_alpha)
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    else
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(r->rrect_shader);
    glBindVertexArray(r->rrect_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->rrect_instance_vbo);

    /* orphan + sub-upload (same idiom as flat rect / glyph paths) */
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(r->rrect_batch_cap * sizeof(RRectInstance)),
                 NULL, GL_STREAM_DRAW);
    usize bytes = r->rrect_batch_count * sizeof(RRectInstance);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, r->rrect_batch);

    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)r->rrect_batch_count);
    g_liu_draw_calls++; g_liu_draw_instances += r->rrect_batch_count;

    /* Restore default non-premul blend so subsequent flush_rects/_glyphs
     * use the expected (SrcAlpha, OneMinusSrcAlpha) factors. */
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    r->rrect_batch_count = 0;
}

/* Backdrop blur. Not a platform gap, despite how this reads: the pair has no
 * call site anywhere in the UI, on either backend, and the Metal side is
 * dormant too — its layer runs framebufferOnly, so renderer_blur_capture
 * returns before it can snapshot and every panel falls through to the same
 * flat tinted rrect this does (see renderer_metal.m and the note in
 * platform_macos.m). Writing the two-pass GL gaussian would add an untested
 * path to match a path that is switched off.
 *
 * Whoever wires panels up: capture has to snapshot the default framebuffer
 * into an FBO before the panel draws, and the drawable must stop being
 * framebufferOnly on the Metal side. */
void renderer_blur_capture(Renderer *r) { (void)r; }
void renderer_draw_blur_panel(Renderer *r,
    f32 x, f32 y, f32 w, f32 h, Color tint,
    f32 r_tl, f32 r_tr, f32 r_br, f32 r_bl)
{
    renderer_draw_rrect(r, x, y, w, h, tint,
        r_tl, r_tr, r_br, r_bl, 0, 0, 0, 0);
}

/* =========================================================================
 * Pass 2: Instanced glyph rendering
 * ========================================================================= */

void renderer_push_glyph(Renderer *r, f32 x, f32 y, u32 codepoint, Color fg) {
    if (!r->font.loaded) return;
    if (codepoint <= 32) return;
    if (fg.a <= 0.0f) return;

    if (r->glyph_count >= r->glyph_cap) renderer_flush_glyphs(r);
    if (r->glyph_count >= r->glyph_cap &&
        !renderer_reserve_glyphs(r, r->glyph_count + 1)) {
        return;
    }

    /* Large UI text (markdown headings) draws each glyph well above the atlas
     * cell size. Ask for a raster made at that size rather than magnifying the
     * terminal-sized one — the magnification is what reads as blur. */
    FontGlyphRect rect;
    bool sized = r->ui_cell_w > r->font.cell_width * 1.12f;
#ifdef PLATFORM_LINUX
    /* The chrome also draws SMALLER than a terminal cell — an 8px UI cell out
     * of a 10px atlas cell — and on the stb path that downsample is what makes
     * the tab strip and sidebar text look mushy. font_get_glyph_rect_sized
     * declines anything near 1:1 either way, so this only adds the shrink
     * band. */
    sized = sized || (r->ui_cell_w > 0.0f &&
                      r->ui_cell_w < r->font.cell_width * 0.89f);
#endif
    bool got = sized ? font_get_glyph_rect_sized(&r->font, codepoint,
                                                 (i32)roundf(r->ui_cell_w), &rect)
                     : font_get_glyph_rect(&r->font, codepoint, &rect);
    if (!got) return;

    GlyphInstance *gi = &r->glyph_instances[r->glyph_count++];
    /* Pixel-align glyph position to avoid subpixel blurring.
     * UI text: centre the (aspect-corrected) glyph box on the layout box the
     * caller reserved — see renderer_ui_glyph_dy. No-op for terminal cells. */
    gi->x = floorf(x);
    gi->y = floorf(y + renderer_ui_glyph_dy(r));
    /* The rect names its atlas explicitly (font.c's FontGlyphRect.kind), so
     * an emoji outside the terminal grid — file names, markdown text, host
     * lists, the command palette — takes the colour atlas the same way ui.c's
     * grid cells do. Texels; the vertex shader normalises. */
    bool is_color = (rect.kind == FONT_GLYPH_COLOR);
    gi->tex_kind = is_color ? GLYPH_TEX_COLOR : GLYPH_TEX_ALPHA;
    gi->ax0 = rect.x0; gi->ay0 = rect.y0;
    gi->ax1 = rect.x1; gi->ay1 = rect.y1;
    /* Colours stay sRGB; the vertex shader decodes (see renderer.h). */
    glyph_set_fg(gi, fg, fg.a);
    /* The shader's colour branch MULTIPLIES the texel by fg, which is how a
     * white premultiplied mask gets tinted at draw time. An emoji is already
     * full colour, so it has to pass white or it takes the surrounding text
     * colour with it. */
    if (is_color && utf8_is_emoji(codepoint)) {
        gi->fg[0] = gi->fg[1] = gi->fg[2] = 255;
    }
    /* UI-chrome path: no cell behind the glyph, but it does sit on the window's
     * surface — handing the shader that puts it on the terminal's weight (see
     * glyph_assumed_bg in renderer.h). */
    glyph_set_chrome_bg(gi, r, fg);
    /* Colour emoji are rasterised into a two-cell-wide box (rasterize_color_boxed:
     * ecw = cw * 2), and so are CJK / fullwidth glyphs (rasterize_glyph doubles
     * cw for font_glyph_is_wide). Drawing either into a one-cell quad samples a
     * double-width raster into half the width and squashes it horizontally. */
    gi->w_cells = (is_color ? utf8_char_width(codepoint) >= 2
                            : font_glyph_is_wide(codepoint)) ? 2 : 1;
    gi->_pad[0] = gi->_pad[1] = 0;
}

/* Append path: cached row instances already carry linear RGB. */
void renderer_append_rects(Renderer *r, const RectInstance *src, u32 count) {
    while (count > 0) {
        if (r->rect_batch_count >= r->rect_batch_cap) renderer_flush_rects(r);
        if (r->rect_batch_count >= r->rect_batch_cap &&
            !renderer_reserve_rects(r, r->rect_batch_count + 1)) {
            return;
        }
        u32 avail = r->rect_batch_cap - r->rect_batch_count;
        if (avail > 0 && count > avail && r->rect_batch_cap < MAX_RECTS) {
            (void)renderer_reserve_rects(r, r->rect_batch_count + count);
            avail = r->rect_batch_cap - r->rect_batch_count;
        }
        u32 take = count < avail ? count : avail;
        RectInstance *dst = &r->rect_batch[r->rect_batch_count];
        for (u32 i = 0; i < take; i++) {
            dst[i] = src[i];
        }
        r->rect_batch_count += take;
        src   += take;
        count -= take;
    }
}

void renderer_append_glyphs(Renderer *r, const GlyphInstance *src, u32 count) {
    if (!r->font.loaded) return;
    while (count > 0) {
        if (r->glyph_count >= r->glyph_cap) renderer_flush_glyphs(r);
        if (r->glyph_count >= r->glyph_cap &&
            !renderer_reserve_glyphs(r, r->glyph_count + 1)) {
            return;
        }
        u32 avail = r->glyph_cap - r->glyph_count;
        if (avail > 0 && count > avail && r->glyph_cap < MAX_GLYPHS) {
            (void)renderer_reserve_glyphs(r, r->glyph_count + count);
            avail = r->glyph_cap - r->glyph_count;
        }
        u32 take = count < avail ? count : avail;
        GlyphInstance *dst = &r->glyph_instances[r->glyph_count];
        for (u32 i = 0; i < take; i++) {
            dst[i] = src[i];
        }
        r->glyph_count += take;
        src   += take;
        count -= take;
    }
}

/* Frame-probe counters. renderer_metal.m defines these for the Metal build;
 * this is the OpenGL backend's copy — main.c reads them unconditionally, so
 * without it the binary would not link off macOS. Fed at each of the four
 * instanced draws below so the numbers mean the same thing on both
 * backends. */
u32 g_liu_draw_calls;
u32 g_liu_draw_instances;

/* LIU_GLYPH_FILTER=linear restores the pre-split behaviour (bilinear for
 * terminal glyphs too) — the A/B arm and the revert, same convention as the
 * Metal backend's metal_glyph_nearest() and as LIU_PACE / LIU_PTY_THREAD. */
static bool gl_glyph_nearest(void) {
    static i32 v = -1;
    if (v < 0) {
        const char *e = getenv("LIU_GLYPH_FILTER");
        v = (e && strcmp(e, "linear") == 0) ? 0 : 1;
    }
    return v != 0;
}

/* Armed for the duration of ONE flush by renderer_draw_image_tinted, which
 * borrows the glyph pipeline to draw a bitmap. Neither glyph sampler suits an
 * image: both are unmipmapped, and the nearest one is what a terminal-grid
 * batch gets — so an inline image (Sixel / agent thumbnail), drawn inside the
 * pane with no UI scale active, would be point-sampled instead of using the
 * image texture's bilinear filter. Binding sampler 0 instead lets the image
 * texture's OWN parameters apply, which is what the Metal backend gets by
 * always handing images its bilinear sampler.
 *
 * A plain file-static because this TU is the whole OpenGL backend, the image
 * path is strictly set → flush → clear on one thread, and flush_glyphs cannot
 * recurse into itself. */
static bool s_gl_image_pass = false;

void renderer_flush_glyphs(Renderer *r) {
    /* Layering invariant: rects and rrects must drain before glyphs so a
     * subsequent end_frame flush doesn't paint pending rects on top of the
     * glyphs about to be rendered. See the matching comment in
     * renderer_metal.m::renderer_flush_glyphs. */
    renderer_flush_rects(r);
    renderer_flush_rrects(r);

    if (r->glyph_count == 0) return;

    /* Premultiplied-alpha blend, matching the Metal glyph pipeline. The glyph
     * fragment shader outputs premultiplied color (rgb already * coverage), so
     * the source factor is ONE. This is what makes premultiplied icon/SF-Symbol
     * bitmaps composite correctly instead of squaring their coverage. */
    gl_blend_premultiplied(r);

    glUseProgram(r->glyph_shader);
    /* The manual weight trim is a per-BATCH value (terminal grid vs chrome), so
     * it is set here on every flush rather than with the per-version uniforms.
     * The image/icon pass draws pictures, not text: no trim. */
    glUniform1f(r->glyph_u_text_weight,
                s_gl_image_pass ? 0.0f : renderer_text_weight(r));
    /* Unit 1 is the emoji colour atlas, bound for every glyph batch because a
     * batch mixes text and emoji freely — there is no draw to split on. It is
     * 0 until the first emoji is rasterised, which is fine: nothing samples it
     * until an instance carries is_color == 2. No sampler object goes on this
     * unit: an emoji is a photographic bitmap being scaled down to the cell,
     * which wants the bilinear filter its texture already carries. */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, r->font.color_texture_gl);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->font.texture_id);
    /* `ui_cell_w > 0` is the UI-scale override, i.e. "this batch is chrome or
     * magnified text"; 0 is terminal text drawn at the atlas cell size. That is
     * exactly the split the two samplers exist for — the same test the Metal
     * backend makes. The image/icon path opts out of both, see s_gl_image_pass. */
    glBindSampler(0, s_gl_image_pass
                         ? 0u
                         : ((r->ui_cell_w > 0.0f || !gl_glyph_nearest())
                                ? r->glyph_sampler_linear
                                : r->glyph_sampler_nearest));
    RetainedBatch *retained = r->retained_draw_buffer;
    glBindVertexArray(retained ? retained->vao : r->glyph_vao);

    if (!retained) {
    /* orphan the old buffer before sub-uploading — tells the driver
     * it can start a new backing store instead of waiting for the GPU to
     * finish reading the previous frame's data (avoids CPU-GPU sync stalls). */
    glBindBuffer(GL_ARRAY_BUFFER, r->glyph_instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(r->glyph_cap * sizeof(GlyphInstance)),
                 NULL, GL_STREAM_DRAW);
    usize replay_bytes = (usize)r->glyph_count * sizeof(GlyphInstance);
#ifdef LIU_RENDER_REPLAY_PROBE
    f64 replay_t0 = r->replay_terminal_upload ? platform_time_sec() : 0.0;
#endif
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    (GLsizeiptr)replay_bytes,
                    r->glyph_instances);
#ifdef LIU_RENDER_REPLAY_PROBE
    if (r->replay_terminal_upload) {
        r->replay_renderer_to_backend_bytes += replay_bytes;
        r->replay_renderer_to_backend_ns +=
            (u64)((platform_time_sec() - replay_t0) * 1e9);
    }
#endif

    }
    /* Single instanced draw call for ALL visible glyphs */
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)r->glyph_count);
    g_liu_draw_calls++; g_liu_draw_instances += r->glyph_count;

    /* Release the unit. A sampler object stays bound until something replaces
     * it, and it overrides the texture's own state — leaving it attached would
     * hand this batch's filter choice to whatever samples unit 0 next. */
    glBindSampler(0, 0);

    r->glyph_count = 0;
}

/* =========================================================================
 * Compat / helper API
 * ========================================================================= */

void renderer_draw_glyph(Renderer *r, u32 codepoint, f32 x, f32 y, Color fg, Color bg) {
    (void)bg; /* bg handled separately in multi-pass */
    renderer_push_glyph(r, x, y, codepoint, fg);
}

void renderer_set_ui_scale(Renderer *r, f32 cw, f32 ch) {
    renderer_flush_glyphs(r); /* flush pending glyphs at old size */
    /* Preserve the font's true glyph aspect ratio. UI layout assumes a fixed
     * 1:2 cell, but a glyph fills the font's real atlas cell — taller than 2:1
     * for some fonts (JetBrains Mono is 20x45). Drawing that into the shorter
     * 1:2 cell squishes it vertically; anchor the glyph height to the requested
     * width times the atlas aspect so UI text scales uniformly. For a 1:2 font
     * this equals the requested ch (no change). */
    f32 gch = (r->font.cell_width > 0.0f)
        ? roundf(cw * (r->font.cell_height / r->font.cell_width)) : ch;
    /* Mirror the requested box onto the shared state so renderer_ui_glyph_dy
     * can re-derive the centring offset (the Metal backend keeps the same
     * fields; without this the GL path would draw UI text top-anchored). */
    r->ui_cell_w = cw;
    r->ui_cell_h = ch;
    glUseProgram(r->glyph_shader);
    glUniform2f(r->glyph_u_cell_size, cw, gch);
    r->gl_glyph_cell_size[0] = cw;
    r->gl_glyph_cell_size[1] = gch;
}

void renderer_reset_ui_scale(Renderer *r) {
    renderer_flush_glyphs(r);
    r->ui_cell_w = 0.0f;
    r->ui_cell_h = 0.0f;
    glUseProgram(r->glyph_shader);
    glUniform2f(r->glyph_u_cell_size, r->font.cell_width, r->font.cell_height);
    r->gl_glyph_cell_size[0] = r->font.cell_width;
    r->gl_glyph_cell_size[1] = r->font.cell_height;
}

static void gl_retained_destroy(RetainedBatch *b) {
    GLuint buffer=(GLuint)(usize)b->slots[0].buffer;
    if (buffer) glDeleteBuffers(1,&buffer);
    if (b->vao) glDeleteVertexArrays(1,&b->vao);
    b->owner->stats.live_bytes-=b->slots[0].capacity;
    mem_budget_transient_adjust(MEM_RENDER_GPU,-(isize)b->slots[0].capacity);
    retained_rows_free(&b->slots[0]);
    memset(&b->slots[0],0,sizeof(b->slots[0])); b->vao=0;
}

static void gl_retained_trim(Renderer *r, f64 now) {
    RetainedStore *store=r->retained_store;
    for (RetainedBatch *b=store?store->head:NULL;b;b=b->next) {
        if (!retained_trim_due(b,now)) continue;
        /* Invalidate storage, not the UI handle. The next draw re-uploads its
         * rows at the right capacity; no GPU readback or blocking fence here. */
        gl_retained_destroy(b);
        b->oversized_since=0;
    }
}

void renderer_retained_stats(const Renderer *r, RendererRetainedStats *out) {
    if (!out) return;
    const RetainedStore *s=r?r->retained_store:NULL;
    *out=s?s->stats:(RendererRetainedStats){0};
}

void renderer_set_draw_origin(Renderer *r, f32 x, f32 y) {
    if (r->draw_origin_x==x && r->draw_origin_y==y) return;
    renderer_flush_glyphs(r); renderer_flush_lines(r);
    r->draw_origin_x=x; r->draw_origin_y=y;
    f32 projection[16]; retained_projection(r,projection);
    if (r->glyph_shader) { glUseProgram(r->glyph_shader); glUniformMatrix4fv(r->glyph_u_projection,1,GL_FALSE,projection); }
    if (r->rect_shader) { glUseProgram(r->rect_shader); glUniformMatrix4fv(r->rect_u_projection,1,GL_FALSE,projection); }
    if (r->rrect_shader) { glUseProgram(r->rrect_shader); glUniformMatrix4fv(r->rrect_u_projection,1,GL_FALSE,projection); }
}

static bool gl_retained_vao(Renderer *r, RetainedBatch *b) {
    GLuint buffer=(GLuint)(usize)b->slots[0].buffer;
    if (!buffer) { glGenBuffers(1,&buffer); b->slots[0].buffer=(void *)(usize)buffer; }
    if (!b->vao) glGenVertexArrays(1,&b->vao);
    if (!buffer || !b->vao) return false;
    glBindVertexArray(b->vao);
    glBindBuffer(GL_ARRAY_BUFFER,b->glyphs?r->glyph_quad_vbo:r->rect_quad_vbo);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,NULL);glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER,buffer);
    if (b->glyphs) {
        GLsizei stride=sizeof(GlyphInstance);
#define RET_GATTR(loc,n,type,norm,field) do { \
    glVertexAttribPointer(loc,n,type,norm,stride,(void *)offsetof(GlyphInstance,field)); \
    glEnableVertexAttribArray(loc);glVertexAttribDivisor(loc,1); \
} while(0)
        RET_GATTR(1,2,GL_FLOAT,GL_FALSE,x);
        RET_GATTR(2,4,GL_FLOAT,GL_FALSE,ax0);
        RET_GATTR(3,4,GL_UNSIGNED_BYTE,GL_TRUE,fg);
        RET_GATTR(4,4,GL_UNSIGNED_BYTE,GL_TRUE,bg);
        RET_GATTR(5,2,GL_UNSIGNED_BYTE,GL_TRUE,tex_kind);
#undef RET_GATTR
    } else {
        glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(RectInstance),(void *)offsetof(RectInstance,x));
        glEnableVertexAttribArray(1);glVertexAttribDivisor(1,1);
        glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,sizeof(RectInstance),(void *)offsetof(RectInstance,w));
        glEnableVertexAttribArray(2);glVertexAttribDivisor(2,1);
        glVertexAttribPointer(3,4,GL_FLOAT,GL_FALSE,sizeof(RectInstance),(void *)offsetof(RectInstance,r));
        glEnableVertexAttribArray(3);glVertexAttribDivisor(3,1);
    }
    glBindVertexArray(0);
    return true;
}

bool renderer_draw_retained_rows(Renderer *r, RendererRetainedBatch **handle,
                                 u64 revision, const RendererRows *rows) {
    if (!r || !handle || !rows) return false;
    RetainedBatch *b=retained_get(r,handle,rows->glyphs);
    if (!b) return false;
    RetainedSlot *slot=&b->slots[0];
    if (!slot->valid || slot->revision!=revision) {
        u32 count;usize bytes;
        if (!retained_size(rows,&count,&bytes)) return false;
        if (bytes) {
            if (!b->vao && !gl_retained_vao(r,b)) return false;
            usize capacity=slot->capacity?slot->capacity:4096;
            while (capacity<bytes) {
                if (capacity>SIZE_MAX/2) { capacity=bytes;break; }
                capacity*=2;
            }
            slot->valid=false;
            glBindBuffer(GL_ARRAY_BUFFER,(GLuint)(usize)slot->buffer);
            /* GL 3.3-compatible orphaning gives the driver fresh storage while
             * older frames may still read the previous revision. */
            glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)capacity,NULL,GL_STREAM_DRAW);
            if (glGetError()!=GL_NO_ERROR) return false;
            mem_budget_transient_adjust(MEM_RENDER_GPU,(isize)capacity-(isize)slot->capacity);
            b->owner->stats.live_bytes+=capacity-slot->capacity;slot->capacity=capacity;
            usize offset=0,stride=rows->glyphs?sizeof(GlyphInstance):sizeof(RectInstance);
            for (i32 row=0;row<rows->rows;row++) {
                usize n=(usize)rows->counts[row]*stride;
                if (n) glBufferSubData(GL_ARRAY_BUFFER,(GLintptr)offset,(GLsizeiptr)n,retained_row(rows,row));
                offset+=n;
            }
            if (glGetError()!=GL_NO_ERROR) return false;
        }
        slot->valid=true;slot->revision=revision;slot->count=count;b->api.count=count;
        retained_note_size(b,bytes);
        b->owner->stats.uploads++;b->owner->stats.upload_bytes+=bytes;
#ifdef LIU_RENDER_REPLAY_PROBE
        r->replay_renderer_to_backend_bytes+=bytes;
#endif
    } else b->owner->stats.reuses++;
    if (!slot->count) return true;
    renderer_flush_glyphs(r);renderer_flush_lines(r);
    r->retained_draw_buffer=b;
    if (rows->glyphs) { r->glyph_count=slot->count;renderer_flush_glyphs(r); }
    else { r->rect_batch_count=slot->count;renderer_flush_rects(r); }
    r->retained_draw_buffer=NULL;
    return true;
}

void renderer_push_scissor(Renderer *r, f32 x, f32 y, f32 w, f32 h) {
    if (!r) return;
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);
    i32 sw = r->screen_width > 0 ? r->screen_width : 0;
    i32 sh = r->screen_height > 0 ? r->screen_height : 0;
    f64 x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    if (isfinite(x) && isfinite(y) && isfinite(w) && isfinite(h) &&
        w > 0.0f && h > 0.0f) {
        x0 = fmax((f64)x, 0.0); y0 = fmax((f64)y, 0.0);
        x1 = fmin((f64)x + (f64)w, (f64)sw);
        y1 = fmin((f64)y + (f64)h, (f64)sh);
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
    }
    i32 ix0 = (i32)fmin(floor(x0), (f64)sw);
    i32 iy0 = (i32)fmin(floor(y0), (f64)sh);
    i32 ix1 = (i32)fmin(ceil(x1),  (f64)sw);
    i32 iy1 = (i32)fmin(ceil(y1),  (f64)sh);
    r->scissor_active = true;
    r->scissor_x = (f32)ix0; r->scissor_y = (f32)iy0;
    r->scissor_w = (f32)(ix1 - ix0); r->scissor_h = (f32)(iy1 - iy0);
    /* OpenGL scissor uses bottom-left origin — flip Y */
    i32 gl_y = sh - iy1;
    glEnable(GL_SCISSOR_TEST);
    glScissor(ix0, gl_y, ix1 - ix0, iy1 - iy0);
}

void renderer_pop_scissor(Renderer *r) {
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);
    r->scissor_active = false;
    glDisable(GL_SCISSOR_TEST);
}

void renderer_flush_text(Renderer *r) { renderer_flush_glyphs(r); }
void renderer_flush_cells(Renderer *r) { renderer_flush_glyphs(r); }

void renderer_push_cell(Renderer *r, f32 x, f32 y, u32 codepoint, Color fg, Color bg) {
    (void)bg;
    renderer_push_glyph(r, x, y, codepoint, fg);
}

void renderer_set_gpu(Renderer *r, void *device, void *layer, void *queue) {
    (void)r; (void)device; (void)layer; (void)queue;
}

bool renderer_gpu_ready(const Renderer *r) { (void)r; return true; }
const char *renderer_gpu_error(void) { return ""; }

void renderer_set_clear_color(Renderer *r, f32 red, f32 green, f32 blue, f32 alpha) {
    /* With GL_FRAMEBUFFER_SRGB enabled, glClearColor is linear and the driver
     * encodes to sRGB on clear. Feed it linear — and premultiplied, for the
     * reason spelled out in the Metal backend: the glyph pipeline blends
     * premultiplied, so a straight clear leaves every glyph edge with more
     * colour than its alpha allows and rings the text with a dark outline.
     *
     * Mirrored into r->clear_color as well as handed to GL, because it is READ
     * as state and not only used as a clear. renderer_draw_rrect_translucent()
     * — the tab bar's per-frame path — takes the window's framebuffer alpha
     * from clear_color[3] to decide whether a translucent window is even in
     * play and, if it is, to premultiply the fill it is about to hand the
     * keep-alpha pipeline. Left at its memset zero, that function would read
     * fb_a = 0 on every frame, take the translucent branch unconditionally and
     * multiply each tab pill's RGB by zero, drawing the whole tab bar flat
     * black instead of its theme colour. */
    r->clear_color[0] = srgb_decode(red)   * alpha;
    r->clear_color[1] = srgb_decode(green) * alpha;
    r->clear_color[2] = srgb_decode(blue)  * alpha;
    r->clear_color[3] = alpha;
    glClearColor(r->clear_color[0], r->clear_color[1],
                 r->clear_color[2], r->clear_color[3]);
    /* The STRAIGHT colour, for UI text's assumed surface (renderer.h). */
    renderer_note_text_surface(r, red, green, blue);
}

/* =========================================================================
 * Inline image rendering (OpenGL)
 * ========================================================================= */

static void gl_image_cache_release_entry(GLImageCache *ic, GLImageCacheEntry *e) {
    if (!e || !e->texture) return;
    if (ic->bytes >= e->bytes) ic->bytes -= e->bytes;
    else ic->bytes = 0;
    glDeleteTextures(1, &e->texture);
    memset(e, 0, sizeof(*e));
    gl_image_account(ic);
}

/* --- the oversize LRU ----------------------------------------------------
 * The mirror of metal_oversize_release/_find/_victim, entry for entry. The one
 * behavioural difference from the pool beside it is the same one: this admits
 * an image larger than its whole budget by emptying itself first. */
static void gl_oversize_release(GLImageCache *ic, GLImageCacheEntry *e) {
    if (!e || !e->texture) return;
    if (ic->oversize_bytes >= e->bytes) ic->oversize_bytes -= e->bytes;
    else ic->oversize_bytes = 0;
    glDeleteTextures(1, &e->texture);
    memset(e, 0, sizeof(*e));
    gl_image_account(ic);
}

static GLImageCacheEntry *gl_oversize_find(GLImageCache *ic, const void *key,
                                           i32 w, i32 h) {
    for (u32 i = 0; i < GL_IMAGE_OVERSIZE_SLOTS; i++) {
        GLImageCacheEntry *e = &ic->oversize[i];
        if (e->texture && e->key == key && e->width == w && e->height == h)
            return e;
    }
    return NULL;
}

/* Eviction asks for something to FREE, placement for somewhere to PUT — see
 * metal_oversize_lru for why conflating the two stops the budget working. */
static GLImageCacheEntry *gl_oversize_lru(GLImageCache *ic) {
    GLImageCacheEntry *oldest = NULL;
    for (u32 i = 0; i < GL_IMAGE_OVERSIZE_SLOTS; i++) {
        GLImageCacheEntry *e = &ic->oversize[i];
        if (!e->texture) continue;
        if (!oldest || e->last_used_frame < oldest->last_used_frame) oldest = e;
    }
    return oldest;
}

static GLImageCacheEntry *gl_oversize_victim(GLImageCache *ic) {
    GLImageCacheEntry *empty = NULL, *oldest = NULL;
    for (u32 i = 0; i < GL_IMAGE_OVERSIZE_SLOTS; i++) {
        GLImageCacheEntry *e = &ic->oversize[i];
        if (!e->texture) { if (!empty) empty = e; continue; }
        if (!oldest || e->last_used_frame < oldest->last_used_frame) oldest = e;
    }
    return empty ? empty : oldest;
}

static GLImageCacheEntry *gl_image_cache_victim(GLImageCache *ic) {
    GLImageCacheEntry *empty = NULL;
    GLImageCacheEntry *oldest = NULL;
    for (u32 i = 0; i < GL_IMAGE_CACHE_CAP; i++) {
        GLImageCacheEntry *e = &ic->entries[i];
        if (!e->texture) {
            if (!empty) empty = e;
            continue;
        }
        if (!oldest || e->last_used_frame < oldest->last_used_frame) oldest = e;
    }
    return empty ? empty : oldest;
}

/* Upload `pixels` into a freshly created texture with the standard image
 * parameters. Returns 0 on failure. Shared by the cached and the uncached
 * (oversize) paths so their sampling behaviour cannot drift apart. */
/* `oversize` only classifies the sample. The timer covers exactly the texture
 * create and the pixel write, so image_upload_cpu_ns cannot absorb draw time.
 * Same counters and same classification as the Metal backend, so the two are
 * comparable. */
static GLuint gl_image_upload_new(const u8 *pixels, i32 img_w, i32 img_h,
                                  bool oversize) {
    f64 up0 = platform_time_sec();
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    /* Match Metal's unmipmapped bilinear image sampler. A full mip chain here
     * would add a third to the retained bytes and, for animated images,
     * rebuild every level synchronously on every frame. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* sRGB internal format so the sampler decodes sRGB->linear on read, matching
     * the Metal backend (MTLPixelFormatRGBA8Unorm_sRGB). GL_FRAMEBUFFER_SRGB is
     * enabled, so uploading already-sRGB icon/image bytes as plain GL_RGBA would
     * treat them as linear and re-encode to sRGB on store — a double gamma that
     * washes icons out. SRGB8_ALPHA8 decodes only RGB; alpha stays linear. */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, img_w, img_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    liu_image_stat_upload(oversize, /*is_alloc*/true,
                          (usize)img_w * (usize)img_h * 4u,
                          (u64)((platform_time_sec() - up0) * 1e9));
    return tex;
}

static GLuint gl_image_texture(GLImageCache *ic, const void *key, u64 generation,
                               const u8 *pixels, i32 img_w, i32 img_h) {
    if (!ic || !key || !pixels || img_w <= 0 || img_h <= 0) return 0;
    /* GL_MAX_TEXTURE_SIZE is a driver constant for the life of the context, so
     * it is queried once and kept rather than on entry to every image and icon
     * draw. */
    static GLint max_dim = 0;
    if (max_dim == 0) glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_dim);
    if (max_dim <= 0 || img_w > max_dim || img_h > max_dim) return 0;
    /* Hit (key,gen,w,h) match AND drop any stale-generation entries for the
     * same key in a single pass. Without this, an animated GIF (which bumps
     * `generation` per frame) accumulates up to the cache budget before LRU
     * eviction kicks in — visible to the user as RAM that keeps growing for
     * the duration of playback. */
    GLuint hit = 0;
    GLImageCacheEntry *reusable = NULL;
    for (u32 i = 0; i < GL_IMAGE_CACHE_CAP; i++) {
        GLImageCacheEntry *e = &ic->entries[i];
        if (!e->texture) continue;
        if (e->key != key) continue;
        if (e->generation == generation && e->width == img_w && e->height == img_h) {
            if (!hit) {
                e->last_used_frame = ic->frame_serial;
                e->last_used_sec = ic->now_sec;
                hit = e->texture;
            } else {
                gl_image_cache_release_entry(ic, e);
            }
        } else if (!reusable && e->width == img_w && e->height == img_h) {
            /* Same key + dimensions, stale gen — animated GIF path. Update
             * the existing texture's pixels with glTexSubImage2D instead of
             * tearing down and reallocating one each frame. */
            reusable = e;
        } else {
            gl_image_cache_release_entry(ic, e);
        }
    }
    if (hit) { g_liu_image_stats.hits++; return hit; }
    if (reusable) {
        /* Same key and geometry, newer contents: an animation frame. */
        g_liu_image_stats.restage_count++;
        f64 up0 = platform_time_sec();
        glBindTexture(GL_TEXTURE_2D, reusable->texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img_w, img_h,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        liu_image_stat_upload(false, /*is_alloc*/false,
                              (usize)img_w * (usize)img_h * 4u,
                              (u64)((platform_time_sec() - up0) * 1e9));
        reusable->generation      = generation;
        reusable->last_used_frame = ic->frame_serial;
        reusable->last_used_sec   = ic->now_sec;
        return reusable->texture;
    }

    /* Base-level retained bytes. Inline images deliberately match Metal's
     * unmipmapped bilinear path; animated generations therefore avoid a full
     * synchronous mip rebuild and accounting is exact. A renderer-level hard
     * ceiling comes first: the per-axis
     * GL_MAX_TEXTURE_SIZE check below says nothing about total bytes, and at
     * 16384 per axis that is a gigabyte. Defence in depth — every caller is
     * supposed to have bounded this already. */
    usize bytes;
    if (!img_rgba_bytes_checked(img_w, img_h, &bytes) ||
        bytes > GL_IMAGE_MAX_BYTES) {
        return 0;
    }

    /* Oversize: draw it, uncached.
     *
     * Anything past the whole budget must still be drawn: the file browser
     * decodes images far larger than the pool (its own ceiling is 128 MiB, e.g.
     * a 24 MP photo). And anything between the threshold and the budget must
     * stay out of the pool, where it would evict essentially everything else,
     * only to be evicted itself by the next icon draw — a full re-upload every
     * frame.
     *
     * Both are handled the same way Metal handles them: don't let a huge
     * texture into the pool, and still draw it. */
    if (bytes > GL_IMAGE_OVERSIZE_BYTES) {
        GLImageCacheEntry *o = gl_oversize_find(ic, key, img_w, img_h);
        if (o) {
            if (o->generation != generation) {
                /* Same logical image, new contents (animation frame): restage
                 * in place rather than reallocating. */
                g_liu_image_stats.restage_count++;
                f64 up0 = platform_time_sec();
                glBindTexture(GL_TEXTURE_2D, o->texture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img_w, img_h,
                                GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                liu_image_stat_upload(true, /*is_alloc*/false,
                                      (usize)img_w * (usize)img_h * 4u,
                                      (u64)((platform_time_sec() - up0) * 1e9));
                o->generation = generation;
            } else {
                g_liu_image_stats.hits++;
            }
            o->last_used_frame = ic->frame_serial;
            o->last_used_sec = ic->now_sec;
            return o->texture;
        }
        /* Not resident. Make room by BYTES, oldest first; an image larger
         * than the whole budget still draws, alone. */
        while (ic->oversize_bytes + bytes > GL_IMAGE_OVERSIZE_BUDGET) {
            GLImageCacheEntry *victim = gl_oversize_lru(ic);
            if (!victim) break;   /* nothing left to free — the image is alone */
            g_liu_image_stats.oversize_replace_count++;
            gl_oversize_release(ic, victim);
        }

        GLImageCacheEntry *slot = gl_oversize_victim(ic);
        if (!slot) return 0;
        if (slot->texture) {
            g_liu_image_stats.oversize_replace_count++;
            gl_oversize_release(ic, slot);
        }

        GLuint tex = gl_image_upload_new(pixels, img_w, img_h, true);
        if (!tex) return 0;
        *slot = (GLImageCacheEntry){
            .key = key, .generation = generation,
            .width = img_w, .height = img_h, .bytes = bytes,
            .last_used_frame = ic->frame_serial,
            .last_used_sec = ic->now_sec, .texture = tex,
        };
        ic->oversize_bytes += bytes;
        gl_image_account(ic);
        return tex;
    }

    while (ic->bytes > GL_IMAGE_CACHE_BUDGET - bytes) {
        GLImageCacheEntry *victim = gl_image_cache_victim(ic);
        if (victim && victim->texture) g_liu_image_stats.evict_count++;
        if (!victim || !victim->texture) break;
        gl_image_cache_release_entry(ic, victim);
    }

    GLImageCacheEntry *slot = gl_image_cache_victim(ic);
    if (!slot) return 0;
    gl_image_cache_release_entry(ic, slot);

    GLuint tex = gl_image_upload_new(pixels, img_w, img_h, false);
    if (!tex) return 0;

    *slot = (GLImageCacheEntry){
        .key = key,
        .generation = generation,
        .width = img_w,
        .height = img_h,
        .bytes = bytes,
        .last_used_frame = ic->frame_serial,
        .last_used_sec = ic->now_sec,
        .texture = tex,
    };
    ic->bytes += bytes;
    gl_image_account(ic);
    return tex;
}

void renderer_draw_image_tinted(Renderer *r, const void *cache_key, u64 cache_generation,
                                const u8 *pixels, i32 img_w, i32 img_h,
                                f32 x, f32 y, f32 draw_w, f32 draw_h,
                                Color tint) {
    if (!r || !pixels || img_w <= 0 || img_h <= 0 ||
        draw_w <= 0 || draw_h <= 0) return;

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);

    GLuint tex = gl_image_texture(r->gl_image_cache,
                                  cache_key ? cache_key : pixels, cache_generation,
                                  pixels, img_w, img_h);
    if (!tex) return;

    /* Save current texture binding state */
    GLuint old_tex = r->font.texture_id;

    /* Override the atlas texture temporarily to render via glyph pipeline */
    r->font.texture_id = tex;
    glBindTexture(GL_TEXTURE_2D, tex);

    /* Push a single glyph instance covering the image */
    if (r->glyph_count >= r->glyph_cap) renderer_flush_glyphs(r);
    if (r->glyph_count >= r->glyph_cap &&
        !renderer_reserve_glyphs(r, r->glyph_count + 1)) {
        r->font.texture_id = old_tex;
        glBindTexture(GL_TEXTURE_2D, old_tex);
        return;
    }
    GlyphInstance *gi = &r->glyph_instances[r->glyph_count++];
    gi->x = floorf(x);
    gi->y = floorf(y);
    /* IMAGE kind: a normalised rect, sampled from the rebound u_atlas. */
    gi->ax0 = 0.0f; gi->ay0 = 0.0f;
    gi->ax1 = 1.0f; gi->ay1 = 1.0f;
    glyph_set_fg(gi, tint, tint.a);
    glyph_set_no_bg(gi);
    gi->tex_kind = GLYPH_TEX_IMAGE;
    gi->w_cells = 1;
    gi->_pad[0] = gi->_pad[1] = 0;

    /* Size the quad to draw_w x draw_h. The glyph vertex shader takes the
     * quad extent from the u_cell_size *uniform* (pos = a_pos + a_quad *
     * vec2(cell.x*w, cell.y)), so it must actually be uploaded. Writing
     * r->ui_cell_w/ch alone does not touch the uniform, and every image
     * and SF-Symbol icon would come out at the stale text cell size:
     * tiny, and stretched to the cell's non-square aspect. Capture the
     * current cell size, override it for this one flush, and restore
     * exactly what the surrounding text/UI pass was using. */
    f32 saved_cell[2] = { r->gl_glyph_cell_size[0], r->gl_glyph_cell_size[1] };
    glUseProgram(r->glyph_shader);
    glUniform2f(r->glyph_u_cell_size, draw_w, draw_h);
    r->gl_glyph_cell_size[0] = draw_w;
    r->gl_glyph_cell_size[1] = draw_h;

    /* Armed only across this one flush — the batch holds exactly the image
     * instance pushed above, so no glyph can pick up the image's sampling. */
    s_gl_image_pass = true;
    renderer_flush_glyphs(r);
    s_gl_image_pass = false;

    glUseProgram(r->glyph_shader);
    glUniform2f(r->glyph_u_cell_size, saved_cell[0], saved_cell[1]);
    r->gl_glyph_cell_size[0] = saved_cell[0];
    r->gl_glyph_cell_size[1] = saved_cell[1];

    /* Restore original atlas texture */
    r->font.texture_id = old_tex;
    glBindTexture(GL_TEXTURE_2D, old_tex);
}

void renderer_draw_image_cached(Renderer *r, const void *cache_key, u64 cache_generation,
                                const u8 *pixels, i32 img_w, i32 img_h,
                                f32 x, f32 y, f32 draw_w, f32 draw_h) {
    renderer_draw_image_tinted(r, cache_key, cache_generation, pixels,
                               img_w, img_h, x, y, draw_w, draw_h,
                               (Color){1.0f, 1.0f, 1.0f, 1.0f});
}

void renderer_draw_image(Renderer *r, const u8 *pixels, i32 img_w, i32 img_h,
                         f32 x, f32 y, f32 draw_w, f32 draw_h) {
    renderer_draw_image_cached(r, pixels, 0, pixels, img_w, img_h,
                               x, y, draw_w, draw_h);
}

/* =========================================================================
 * Background image (wallpaper) — load, destroy, draw
 *
 * Port of the Metal path in renderer_metal.m, with one deliberate omission:
 * there is no cross-window share table. Metal can hand every window the same
 * MTLTexture because all of them render on the one process-wide MTLDevice; GL
 * texture NAMES are context-local, so a name minted in window A's context is
 * meaningless in B's — the same reason the inline-image cache is per-renderer.
 * Each window therefore decodes and uploads its own copy.
 * ========================================================================= */

#include "stb_image.h"

/* Ceiling for the wallpaper texture, matching METAL_BG_IMAGE_MAX_BYTES. The
 * driver's own limit is per-axis, but a 16384x16384 RGBA image is 1 GiB — the
 * decode alone would stall the main thread and blow through RSS before GL ever
 * saw it. 128 MiB covers a full 8K wallpaper (7680x4320x4 = 126.6 MiB). */
#define GL_BG_IMAGE_MAX_BYTES (128ull * 1024ull * 1024ull)

/* Lazy-compile the wallpaper program on first use — most sessions never set a
 * background image and should not pay for the shader. Mirrors
 * ensure_bg_image_pipeline(). Returns false once and stays cheap on retry: a
 * failed link leaves bg_shader at 0 and the caller declines the load. */
static bool gl_ensure_bg_pipeline(Renderer *r) {
    if (r->bg_shader) return true;

    u32 v = compile_shader(bg_vert_src, GL_VERTEX_SHADER);
    u32 f = compile_shader(bg_frag_src, GL_FRAGMENT_SHADER);
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return false;
    }
    r->bg_shader = link_program(v, f);   /* consumes v/f either way */
    if (!r->bg_shader) return false;

    r->bg_u_projection = glGetUniformLocation(r->bg_shader, "u_projection");
    r->bg_u_pos_size   = glGetUniformLocation(r->bg_shader, "u_pos_size");
    r->bg_u_uv         = glGetUniformLocation(r->bg_shader, "u_uv");
    r->bg_u_opacity    = glGetUniformLocation(r->bg_shader, "u_opacity");
    r->bg_u_tex        = glGetUniformLocation(r->bg_shader, "u_tex");

    f32 quad[] = { 0,0, 1,0, 1,1, 0,0, 1,1, 0,1 };
    glGenVertexArrays(1, &r->bg_vao);
    glBindVertexArray(r->bg_vao);
    glGenBuffers(1, &r->bg_quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->bg_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    return true;
}

bool renderer_load_background_rgba(Renderer *r, const char *path,
                                   const u8 *pixels, i32 w, i32 h,
                                   void *gpu_device) {
    (void)gpu_device;
    if (!r || !path || !path[0] || !pixels || w <= 0 || h <= 0) return false;
    if (!gl_ensure_bg_pipeline(r)) return false;
    GLint max_dim = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_dim);
    if (max_dim <= 0) max_dim = 2048;
    usize bytes = 0;
    if (w > max_dim || h > max_dim ||
        !img_rgba_bytes_checked(w, h, &bytes) || bytes > GL_BG_IMAGE_MAX_BYTES)
        return false;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return false;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        return false;
    }

    renderer_destroy_background_image(r);
    r->bg_texture = (void *)(usize)tex;
    r->bg_image_w = w;
    r->bg_image_h = h;
    r->bg_gpu_bytes = bytes;
    mem_budget_adjust(MEM_IMAGE_GPU, (isize)bytes);
    fprintf(stderr, "renderer: uploaded background image %dx%d from %s\n", w, h, path);
    return true;
}

bool renderer_load_background_rgba_versioned(Renderer *r, const char *path,
                                             const u8 *pixels, i32 w, i32 h,
                                             void *gpu_device, const FileStamp *source) {
    if (source && !file_stamp_matches(path, source)) return false;
    return renderer_load_background_rgba(r,path,pixels,w,h,gpu_device);
}

bool renderer_load_background_image(Renderer *r, const char *path, void *gpu_device) {
    /* gpu_device is the Metal device; platform_get_gpu_device returns NULL off
     * macOS and GL needs nothing but a current context, so it is ignored here
     * rather than gated on. */
    (void)gpu_device;
    if (!r || !path || !path[0]) return false;
    FileStamp source;
    if (!file_stamp_read(path, &source)) return false;

    /* Build the program before decoding — if it will not compile there is no
     * point spending the decode on a bitmap that can never be drawn. */
    if (!gl_ensure_bg_pipeline(r)) {
        fprintf(stderr, "renderer: background image pipeline unavailable\n");
        return false;
    }

    /* Gate on the HEADER before decoding. stb_image accepts dimensions up to
     * 1<<24 per axis, so without this a 20000x20000 PNG first tries a ~1.6 GB
     * decode allocation and only then hands GL a size the driver refuses —
     * the whole decode and a multi-second main-thread stall, paid for nothing.
     * Same gate metal_load_background_image applies. */
    GLint max_dim = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_dim);
    if (max_dim <= 0) max_dim = 2048;   /* conservative floor if the query fails */

    i32 w = 0, h = 0, channels = 0;
    if (!stbi_info(path, &w, &h, &channels)) {
        fprintf(stderr, "renderer: cannot read background image header: %s\n", path);
        return false;
    }
    usize bg_pixels = 0, bg_bytes = 0;
    if (w <= 0 || h <= 0 || w > max_dim || h > max_dim ||
        !checked_mul_usize((usize)w, (usize)h, &bg_pixels) ||
        !checked_mul_usize(bg_pixels, 4u, &bg_bytes) ||
        bg_bytes > GL_BG_IMAGE_MAX_BYTES) {
        fprintf(stderr, "renderer: background image %dx%d rejected (max %d px/axis, %llu MiB)\n",
                w, h, (int)max_dim,
                (unsigned long long)(GL_BG_IMAGE_MAX_BYTES / (1024ull * 1024ull)));
        return false;
    }

    /* Keep the existing image until decode, revision validation and upload
     * have all succeeded. The upload also rechecks the decoded byte ceiling. */
    w = h = channels = 0;
    u8 *pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) return false;
    bool uploaded = renderer_load_background_rgba_versioned(r,path,pixels,w,h,
                                                           gpu_device,&source);
    stbi_image_free(pixels);
    return uploaded;
}

void renderer_destroy_background_image(Renderer *r) {
    if (!r) return;
    GLuint tex = (GLuint)(usize)r->bg_texture;
    if (tex) glDeleteTextures(1, &tex);
    if (r->bg_gpu_bytes)
        mem_budget_adjust(MEM_IMAGE_GPU, -(isize)r->bg_gpu_bytes);
    r->bg_texture = NULL;
    r->bg_image_w = 0;
    r->bg_image_h = 0;
    r->bg_gpu_bytes = 0;
}

void renderer_draw_background_image(Renderer *r) {
    if (!r || !r->bg_texture || !r->bg_shader) return;
    if (r->bg_opacity <= 0.001f) return;

    f32 screen_w = (f32)r->screen_width;
    f32 screen_h = (f32)r->screen_height;
    f32 img_w = (f32)r->bg_image_w;
    f32 img_h = (f32)r->bg_image_h;
    if (screen_w <= 0.0f || screen_h <= 0.0f || img_w <= 0.0f || img_h <= 0.0f) return;

    /* Mode → (pos, size, uv_scale, uv_offset). Kept line-for-line with the
     * switch in renderer_metal.m::renderer_draw_background_image so the two
     * lay a picture out identically. */
    f32 pos[2], size[2], uv_scale[2] = { 1.0f, 1.0f }, uv_off[2] = { 0.0f, 0.0f };
    switch (r->bg_mode) {
        case 0:  /* stretch */
            pos[0] = 0.0f; pos[1] = 0.0f;
            size[0] = screen_w; size[1] = screen_h;
            break;
        case 1:  /* center */
            pos[0] = (screen_w - img_w) * 0.5f;
            pos[1] = (screen_h - img_h) * 0.5f;
            size[0] = img_w; size[1] = img_h;
            break;
        case 2:  /* tile */
            pos[0] = 0.0f; pos[1] = 0.0f;
            size[0] = screen_w; size[1] = screen_h;
            uv_scale[0] = screen_w / img_w;
            uv_scale[1] = screen_h / img_h;
            break;
        case 3:  /* fill (aspect) */
        default: {
            f32 scale_x = screen_w / img_w;
            f32 scale_y = screen_h / img_h;
            f32 scale = (scale_x > scale_y) ? scale_x : scale_y;
            f32 draw_w = img_w * scale;
            f32 draw_h = img_h * scale;
            pos[0] = (screen_w - draw_w) * 0.5f;
            pos[1] = (screen_h - draw_h) * 0.5f;
            size[0] = draw_w; size[1] = draw_h;
            break;
        }
    }

    /* Straight (non-premultiplied) alpha on all four factors — the Metal
     * pipeline's blend, copied rather than corrected so both backends put the
     * same pixels on screen. */
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(r->bg_shader);
    glUniformMatrix4fv(r->bg_u_projection, 1, GL_FALSE, r->projection);
    glUniform4f(r->bg_u_pos_size, pos[0], pos[1], size[0], size[1]);
    glUniform4f(r->bg_u_uv, uv_scale[0], uv_scale[1], uv_off[0], uv_off[1]);
    glUniform1f(r->bg_u_opacity, r->bg_opacity);
    glUniform1i(r->bg_u_tex, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)(usize)r->bg_texture);
    /* Reset wrap parameters explicitly because a sampler bound to unit 0
     * overrides the texture's wrap mode. */
    glBindSampler(0, 0);

    glBindVertexArray(r->bg_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    g_liu_draw_calls++; g_liu_draw_instances++;

    /* Leave the blend state as the rest of the frame expects it. */
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(0);
}

#else /* USE_METAL */
/* Placeholder to satisfy ISO C translation unit requirement when Metal is on */
typedef int renderer_tu_sentinel_t;
#endif /* !USE_METAL */
