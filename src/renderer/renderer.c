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
#include "core/memory.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

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

void renderer_init_colors(void) {
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
    /* 216 color cube */
    for (int i = 0; i < 216; i++) {
        int r = (i / 36) % 6, g = (i / 6) % 6, b = i % 6;
        g_ansi_colors[16 + i] = COLOR_RGB(r ? 55+r*40 : 0, g ? 55+g*40 : 0, b ? 55+b*40 : 0);
    }
    /* 24 greyscale */
    for (int i = 0; i < 24; i++) {
        int v = 8 + i * 10;
        g_ansi_colors[232 + i] = COLOR_RGB(v, v, v);
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
    "layout(location=2) in vec4 a_uv;\n"
    "layout(location=3) in vec3 a_fg;\n"
    "layout(location=4) in vec3 a_extra;\n"  /* x=is_color, y=w_cells, z=alpha */
    "uniform mat4 u_projection;\n"
    "uniform vec2 u_cell_size;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_fg;\n"
    "out float v_is_color;\n"
    "out float v_alpha;\n"
    "void main(){\n"
    "  float w = a_extra.y;\n"
    "  if (w < 0.5) w = 1.0;\n"
    "  vec2 sz = vec2(u_cell_size.x * w, u_cell_size.y);\n"
    "  vec2 px = a_pos + a_quad * sz;\n"
    "  gl_Position = u_projection * vec4(px, 0.0, 1.0);\n"
    "  v_uv = mix(a_uv.xy, a_uv.zw, a_quad);\n"
    "  v_fg = vec4(a_fg, 1.0);\n"
    "  v_is_color = a_extra.x;\n"
    "  v_alpha = a_extra.z;\n"
    "}\n";

static const char *glyph_frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec4 v_fg;\n"
    "in float v_is_color;\n"
    "in float v_alpha;\n"
    "uniform sampler2D u_atlas;\n"
    "out vec4 frag_color;\n"
    /* Premultiplied output, matching the Metal glyph_fragment paired with a
     * (ONE, 1-SrcAlpha) blend. The image/icon path feeds PREMULTIPLIED sRGB
     * bitmaps (SF Symbols come from a kCGImageAlphaPremultipliedLast context),
     * so straight (SrcAlpha) blending squared their coverage and rendered icons
     * thin and washed out. For solid-color text this is identical to the old
     * straight-alpha path (rgb*cov composited the same either way). */
    "void main(){\n"
    "  if (v_is_color > 0.5) {\n"
    "    vec4 c = texture(u_atlas, v_uv);\n"
    "    float ca = c.a * v_alpha;\n"
    "    if (ca < 0.004) discard;\n"
    "    frag_color = vec4(c.rgb * v_alpha, ca);\n"  /* premultiplied */
    "    return;\n"
    "  }\n"
    "  float cov = texture(u_atlas, v_uv).r * v_alpha;\n"
    "  if (cov < 0.004) discard;\n"
    "  cov = pow(cov, 1.0/1.43);\n"   /* Apple-style stem darkening (matches Metal) */
    "  frag_color = vec4(v_fg.rgb * cov, cov);\n"     /* premultiplied */
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
#define GL_IMAGE_CACHE_BUDGET (64ull * 1024ull * 1024ull)

typedef struct {
    const void *key;
    u64         generation;
    i32         width;
    i32         height;
    usize       bytes;
    u64         last_used_frame;
    GLuint      texture;
} GLImageCacheEntry;

static GLImageCacheEntry g_image_cache[GL_IMAGE_CACHE_CAP];
static usize g_image_cache_bytes = 0;
static u64 g_frame_serial = 0;

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

bool renderer_init(Renderer *r, f32 dpi_scale) {
    memset(r, 0, sizeof(*r));
    r->dpi_scale = dpi_scale;
    renderer_init_colors();

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
    r->glyph_u_cell_size  = glGetUniformLocation(r->glyph_shader, "u_cell_size");

    /* Unit quad for glyph instances */
    f32 quad[] = { 0,0, 1,0, 1,1, 0,0, 1,1, 0,1 };

    glGenVertexArrays(1, &r->glyph_vao);
    glBindVertexArray(r->glyph_vao);

    /* Shared quad geometry (location 0) */
    glGenBuffers(1, &r->glyph_quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->glyph_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
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
    /* a_uv: vec4 (u0,v0,u1,v1) */
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, (GLsizei)stride, GOFF(u0));
    glEnableVertexAttribArray(2);
    glVertexAttribDivisor(2, 1);
    /* a_fg: vec3 */
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, (GLsizei)stride, GOFF(r));
    glEnableVertexAttribArray(3);
    glVertexAttribDivisor(3, 1);
    /* a_extra: vec3 (is_color, w_cells, alpha) — is_color/w_cells/a are
     * contiguous in GlyphInstance, so one vec3 picks up the per-glyph alpha
     * the Metal backend already honors. */
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, (GLsizei)stride, GOFF(is_color));
    glEnableVertexAttribArray(4);
    glVertexAttribDivisor(4, 1);
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

void renderer_destroy(Renderer *r) {
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
    /* Line pipeline was created in renderer_init but never torn down — leaked a
     * VAO + 2 VBOs + a linked program on every renderer recreate (font reload,
     * DPI / monitor change). */
    if (r->line_shader) glDeleteProgram(r->line_shader);
    if (r->line_vao) glDeleteVertexArrays(1, &r->line_vao);
    if (r->line_quad_vbo) glDeleteBuffers(1, &r->line_quad_vbo);
    if (r->line_instance_vbo) glDeleteBuffers(1, &r->line_instance_vbo);
    for (u32 i = 0; i < GL_IMAGE_CACHE_CAP; i++) {
        if (g_image_cache[i].texture) {
            glDeleteTextures(1, &g_image_cache[i].texture);
            memset(&g_image_cache[i], 0, sizeof(g_image_cache[i]));
        }
    }
    g_image_cache_bytes = 0;
    free(r->glyph_instances);
    free(r->rect_batch);
    free(r->line_batch);
    font_atlas_destroy(&r->font);
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
    r->screen_width  = w;
    r->screen_height = h;
    r->glyph_count   = 0;
    r->rect_batch_count = 0;
    r->rrect_batch_count = 0;
    g_frame_serial++;

    glViewport(0, 0, w, h);
    /* sRGB-correct blending: GPU converts sRGB<->linear automatically */
    glEnable(GL_FRAMEBUFFER_SRGB);
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
        if (r->font.loaded) {
            glUniform2f(r->glyph_u_cell_size, cell_w, cell_h);
            r->gl_glyph_cell_size[0] = cell_w;
            r->gl_glyph_cell_size[1] = cell_h;
        }
        r->gl_glyph_uniforms_version = r->uniforms_version;
    }
}

void renderer_end_frame(Renderer *r) {
    /* Flush any remaining batched draws.
     * rrect flush is a no-op in the OpenGL backend (graceful flat-rect
     * fallback handled inline in renderer_draw_rrect). */
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);
}

void renderer_trim_idle_resources(Renderer *r, f64 now_sec) {
    (void)r;
    (void)now_sec;
}

/* =========================================================================
 * Pass 1 + 3: Rect batching
 * ========================================================================= */

void renderer_draw_rect(Renderer *r, f32 x, f32 y, f32 w, f32 h, Color c) {
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

void renderer_flush_rects(Renderer *r) {
    if (r->rect_batch_count == 0) return;

    /* Rects use standard alpha blending for semi-transparent elements */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(r->rect_shader);
    glBindVertexArray(r->rect_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->rect_instance_vbo);

    /* orphan before upload */
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(r->rect_batch_cap * sizeof(RectInstance)),
                 NULL, GL_STREAM_DRAW);
    usize bytes = r->rect_batch_count * sizeof(RectInstance);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, r->rect_batch);

    /* Single instanced draw call for ALL batched rects */
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)r->rect_batch_count);

    r->rect_batch_count = 0;
}

/* =========================================================================
 * Line batching — thick AA segments (graph-view edges, OpenGL backend)
 * ========================================================================= */

void renderer_draw_line(Renderer *r, f32 x0, f32 y0, f32 x1, f32 y1,
                        f32 thickness, Color c) {
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

void renderer_flush_lines(Renderer *r) {
    if (r->line_batch_count == 0) return;
    if (!r->line_shader) { r->line_batch_count = 0; return; }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(r->line_shader);
    glUniformMatrix4fv(r->line_u_projection, 1, GL_FALSE, r->projection);
    glBindVertexArray(r->line_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->line_instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(r->line_batch_cap * sizeof(LineInstance)),
                 NULL, GL_STREAM_DRAW);
    usize bytes = r->line_batch_count * sizeof(LineInstance);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, r->line_batch);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)r->line_batch_count);
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

    /* Premultiplied alpha output — matches the Metal pipeline's blend. */
    glEnable(GL_BLEND);
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

    /* Restore default non-premul blend so subsequent flush_rects/_glyphs
     * use the expected (SrcAlpha, OneMinusSrcAlpha) factors. */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    r->rrect_batch_count = 0;
}

/* Backdrop blur: not implemented in the OpenGL backend. The capture call
 * is a no-op; the panel render falls through to a plain tinted rrect. */
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

    f32 u0, v0, u1, v1;
    if (!font_get_glyph_uv(&r->font, codepoint, &u0, &v0, &u1, &v1))
        return;

    GlyphInstance *gi = &r->glyph_instances[r->glyph_count++];
    /* Pixel-align glyph position to avoid subpixel blurring */
    gi->x = floorf(x);
    gi->y = floorf(y);
    gi->u0 = u0; gi->v0 = v0;
    gi->u1 = u1; gi->v1 = v1;
    /* sRGB → linear at the CPU → GPU boundary. */
    Color lin = color_linear_from_srgb(fg);
    gi->r = lin.r;
    gi->g = lin.g;
    gi->b = lin.b;
    gi->is_color = 0.0f;
    gi->w_cells = 1.0f;
    gi->a = fg.a;
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

void renderer_flush_glyphs(Renderer *r) {
    /* Layering invariant: rects and rrects must drain before glyphs so a
     * subsequent end_frame flush doesn't paint pending rects on top of the
     * glyphs we're about to render. See the matching comment in
     * renderer_metal.m::renderer_flush_glyphs. */
    renderer_flush_rects(r);
    renderer_flush_rrects(r);

    if (r->glyph_count == 0) return;

    /* Premultiplied-alpha blend, matching the Metal glyph pipeline. The glyph
     * fragment shader outputs premultiplied color (rgb already * coverage), so
     * the source factor is ONE. This is what makes premultiplied icon/SF-Symbol
     * bitmaps composite correctly instead of squaring their coverage. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(r->glyph_shader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->font.texture_id);
    glBindVertexArray(r->glyph_vao);

    /* orphan the old buffer before sub-uploading — tells the driver
     * it can start a new backing store instead of waiting for the GPU to
     * finish reading the previous frame's data (avoids CPU-GPU sync stalls). */
    glBindBuffer(GL_ARRAY_BUFFER, r->glyph_instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(r->glyph_cap * sizeof(GlyphInstance)),
                 NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    (GLsizeiptr)(r->glyph_count * sizeof(GlyphInstance)),
                    r->glyph_instances);

    /* Single instanced draw call for ALL visible glyphs */
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)r->glyph_count);

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
        ? cw * (r->font.cell_height / r->font.cell_width) : ch;
    glUseProgram(r->glyph_shader);
    glUniform2f(r->glyph_u_cell_size, cw, gch);
    r->gl_glyph_cell_size[0] = cw;
    r->gl_glyph_cell_size[1] = gch;
}

void renderer_reset_ui_scale(Renderer *r) {
    renderer_flush_glyphs(r);
    glUseProgram(r->glyph_shader);
    glUniform2f(r->glyph_u_cell_size, r->font.cell_width, r->font.cell_height);
    r->gl_glyph_cell_size[0] = r->font.cell_width;
    r->gl_glyph_cell_size[1] = r->font.cell_height;
}

void renderer_push_scissor(Renderer *r, f32 x, f32 y, f32 w, f32 h) {
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    renderer_flush_glyphs(r);
    r->scissor_active = true;
    r->scissor_x = x; r->scissor_y = y; r->scissor_w = w; r->scissor_h = h;
    /* OpenGL scissor uses bottom-left origin — flip Y */
    i32 gl_y = r->screen_height - (i32)(y + h);
    glEnable(GL_SCISSOR_TEST);
    glScissor((i32)x, gl_y, (i32)w, (i32)h);
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

void renderer_set_clear_color(Renderer *r, f32 red, f32 green, f32 blue, f32 alpha) {
    /* With GL_FRAMEBUFFER_SRGB enabled, glClearColor is linear and the driver
     * encodes to sRGB on clear. Feed it linear. */
    glClearColor(srgb_decode(red), srgb_decode(green), srgb_decode(blue), alpha);
    (void)r;
}

/* =========================================================================
 * Inline image rendering (OpenGL)
 * ========================================================================= */

static void gl_image_cache_release_entry(GLImageCacheEntry *e) {
    if (!e || !e->texture) return;
    if (g_image_cache_bytes >= e->bytes) g_image_cache_bytes -= e->bytes;
    else g_image_cache_bytes = 0;
    glDeleteTextures(1, &e->texture);
    memset(e, 0, sizeof(*e));
}

static GLImageCacheEntry *gl_image_cache_victim(void) {
    GLImageCacheEntry *empty = NULL;
    GLImageCacheEntry *oldest = NULL;
    for (u32 i = 0; i < GL_IMAGE_CACHE_CAP; i++) {
        GLImageCacheEntry *e = &g_image_cache[i];
        if (!e->texture) {
            if (!empty) empty = e;
            continue;
        }
        if (!oldest || e->last_used_frame < oldest->last_used_frame) oldest = e;
    }
    return empty ? empty : oldest;
}

static GLuint gl_image_texture(const void *key, u64 generation, const u8 *pixels,
                               i32 img_w, i32 img_h) {
    if (!key || !pixels || img_w <= 0 || img_h <= 0) return 0;
    /* Hit (key,gen,w,h) match AND drop any stale-generation entries for the
     * same key in a single pass. Without this, an animated GIF (which bumps
     * `generation` per frame) accumulates up to the cache budget before LRU
     * eviction kicks in — visible to the user as RAM that keeps growing for
     * the duration of playback. */
    GLuint hit = 0;
    GLImageCacheEntry *reusable = NULL;
    for (u32 i = 0; i < GL_IMAGE_CACHE_CAP; i++) {
        GLImageCacheEntry *e = &g_image_cache[i];
        if (!e->texture) continue;
        if (e->key != key) continue;
        if (e->generation == generation && e->width == img_w && e->height == img_h) {
            if (!hit) {
                e->last_used_frame = g_frame_serial;
                hit = e->texture;
            } else {
                gl_image_cache_release_entry(e);
            }
        } else if (!reusable && e->width == img_w && e->height == img_h) {
            /* Same key + dimensions, stale gen — animated GIF path. Update
             * the existing texture's pixels with glTexSubImage2D instead of
             * tearing down and reallocating one each frame. */
            reusable = e;
        } else {
            gl_image_cache_release_entry(e);
        }
    }
    if (hit) return hit;
    if (reusable) {
        glBindTexture(GL_TEXTURE_2D, reusable->texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img_w, img_h,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glGenerateMipmap(GL_TEXTURE_2D);   /* refresh mips for the new frame */
        reusable->generation      = generation;
        reusable->last_used_frame = g_frame_serial;
        return reusable->texture;
    }

    usize bytes = (usize)img_w * (usize)img_h * 4;
    while (g_image_cache_bytes + bytes > GL_IMAGE_CACHE_BUDGET) {
        GLImageCacheEntry *victim = gl_image_cache_victim();
        if (!victim || !victim->texture) break;
        gl_image_cache_release_entry(victim);
    }

    GLImageCacheEntry *slot = gl_image_cache_victim();
    if (!slot) return 0;
    gl_image_cache_release_entry(slot);

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    /* Trilinear minification so down-scaled images don't shimmer; mag stays
     * linear. Mips are generated after the upload below. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
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
    glGenerateMipmap(GL_TEXTURE_2D);

    *slot = (GLImageCacheEntry){
        .key = key,
        .generation = generation,
        .width = img_w,
        .height = img_h,
        .bytes = bytes,
        .last_used_frame = g_frame_serial,
        .texture = tex,
    };
    g_image_cache_bytes += bytes;
    return tex;
}

void renderer_draw_image_cached(Renderer *r, const void *cache_key, u64 cache_generation,
                                const u8 *pixels, i32 img_w, i32 img_h,
                                f32 x, f32 y, f32 draw_w, f32 draw_h) {
    if (!pixels || img_w <= 0 || img_h <= 0 || draw_w <= 0 || draw_h <= 0) return;

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);

    GLuint tex = gl_image_texture(cache_key ? cache_key : pixels, cache_generation,
                                  pixels, img_w, img_h);
    if (!tex) return;

    /* Save current texture binding state */
    GLuint old_tex = r->font.texture_id;

    /* Override the atlas texture temporarily to render via glyph pipeline */
    r->font.texture_id = tex;
    glBindTexture(GL_TEXTURE_2D, tex);

    /* Push a single glyph instance covering the image */
    if (r->glyph_count >= r->glyph_cap) renderer_flush_glyphs(r);
    GlyphInstance *gi = &r->glyph_instances[r->glyph_count++];
    gi->x = floorf(x);
    gi->y = floorf(y);
    gi->u0 = 0.0f; gi->v0 = 0.0f;
    gi->u1 = 1.0f; gi->v1 = 1.0f;
    gi->r = 1.0f; gi->g = 1.0f; gi->b = 1.0f;
    gi->is_color = 1.0f;
    gi->w_cells = 1.0f;
    gi->a = 1.0f;

    /* Size the quad to draw_w x draw_h. The glyph vertex shader takes the
     * quad extent from the u_cell_size *uniform* (pos = a_pos + a_quad *
     * vec2(cell.x*w, cell.y)), so it must actually be uploaded. Writing
     * r->ui_cell_w/ch alone never touched the uniform — every image and
     * SF-Symbol icon came out at the stale text cell size: tiny, and
     * stretched to the cell's non-square aspect ("broken icon heights").
     * Capture the current cell size, override it for this one flush, and
     * restore exactly what the surrounding text/UI pass was using. */
    f32 saved_cell[2] = { r->gl_glyph_cell_size[0], r->gl_glyph_cell_size[1] };
    glUseProgram(r->glyph_shader);
    glUniform2f(r->glyph_u_cell_size, draw_w, draw_h);
    r->gl_glyph_cell_size[0] = draw_w;
    r->gl_glyph_cell_size[1] = draw_h;

    renderer_flush_glyphs(r);

    glUseProgram(r->glyph_shader);
    glUniform2f(r->glyph_u_cell_size, saved_cell[0], saved_cell[1]);
    r->gl_glyph_cell_size[0] = saved_cell[0];
    r->gl_glyph_cell_size[1] = saved_cell[1];

    /* Restore original atlas texture */
    r->font.texture_id = old_tex;
    glBindTexture(GL_TEXTURE_2D, old_tex);

}

void renderer_draw_image(Renderer *r, const u8 *pixels, i32 img_w, i32 img_h,
                         f32 x, f32 y, f32 draw_w, f32 draw_h) {
    renderer_draw_image_cached(r, pixels, 0, pixels, img_w, img_h,
                               x, y, draw_w, draw_h);
}

/* Background image stubs for OpenGL (not yet implemented) */
bool renderer_load_background_image(Renderer *r, const char *path, void *gpu_device) {
    (void)r; (void)path; (void)gpu_device;
    fprintf(stderr, "renderer: background image not supported in OpenGL backend\n");
    return false;
}
void renderer_destroy_background_image(Renderer *r) { (void)r; }
void renderer_draw_background_image(Renderer *r) { (void)r; }

#else /* USE_METAL */
/* Placeholder to satisfy ISO C translation unit requirement when Metal is on */
typedef int renderer_tu_sentinel_t;
#endif /* !USE_METAL */
