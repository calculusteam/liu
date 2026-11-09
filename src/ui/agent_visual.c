/* Liu — what an agent drew: a diagram, a formula or an SVG picture. See agent_visual.h. */
#include "ui/agent_visual.h"
#include "ui/markdown/md_math.h"
#include "ui/svg_raster.h"
#include "core/utf8.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGV_TEX_CAP    4096u
#define AGV_TEXTS      96u
#define AGV_SVG_EDGE_MAX 2048
#define AGV_SVG_EDGE_MIN 64
#define AGV_CSS_ADVANCE 8.4f       /* a 14 pt face's advance, in CSS pixels */

static void (*g_wake)(void);
static _Atomic u64 g_revision;

void agv_set_wake(void (*wake)(void)) { g_wake = wake; }
u64  agv_revision(void) { return atomic_load_explicit(&g_revision, memory_order_acquire); }

/* ---- the SVG worker ----
 *
 * A job is shared by the visual and its thread: whoever lets go of it last
 * frees it, so a visual destroyed while its picture is being drawn costs
 * nothing but the drawing. */

typedef struct {
    _Atomic i32 refs;
    _Atomic i32 state;              /* 0 drawing, 1 drawn, 2 failed */
    char       *src;
    u32         len;
    i32         long_edge;
    SvgImage    img;
    SvgStatus   status;
    u64         gen;                /* the texture's identity: a pointer is reused, this is not */
} AgvSvgJob;

static _Atomic u64 g_job_gen;

static void job_release(AgvSvgJob *j) {
    if (!j) return;
    if (atomic_fetch_sub_explicit(&j->refs, 1, memory_order_acq_rel) != 1) return;
    free(j->img.rgba);
    free(j->src);
    free(j);
}

static void *job_main(void *arg) {
    AgvSvgJob *j = (AgvSvgJob *)arg;
    SvgLimits lim = { .max_file_bytes = 256u * 1024u, .long_edge = j->long_edge,
                      .max_rgba_bytes = (usize)AGV_SVG_EDGE_MAX * AGV_SVG_EDGE_MAX * 4u, .max_seconds = 0 };
    SvgImage img;
    SvgStatus st = svg_rasterize((const u8 *)j->src, j->len, &lim, &img);
    j->status = st;
    if (st == SVG_OK) j->img = img;
    atomic_store_explicit(&j->state, st == SVG_OK ? 1 : 2, memory_order_release);
    atomic_fetch_add_explicit(&g_revision, 1, memory_order_acq_rel);
    if (g_wake) g_wake();
    job_release(j);
    return NULL;
}

static AgvSvgJob *job_start(const char *src, u32 len, i32 long_edge) {
    AgvSvgJob *j = (AgvSvgJob *)calloc(1, sizeof *j);
    if (!j) return NULL;
    j->src = (char *)malloc(len + 1);
    if (!j->src) { free(j); return NULL; }
    memcpy(j->src, src, len);
    j->src[len] = 0;
    j->len = len;
    j->long_edge = long_edge;
    j->gen = atomic_fetch_add_explicit(&g_job_gen, 1, memory_order_relaxed) + 1;
    atomic_store(&j->refs, 2);
    pthread_t th;
    pthread_attr_t at;
    bool started = false;
    if (pthread_attr_init(&at) == 0) {
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        started = pthread_create(&th, &at, job_main, j) == 0;
        pthread_attr_destroy(&at);
    }
    if (!started) {
        j->status = SVG_ERR_MEMORY;
        atomic_store(&j->state, 2);
        atomic_store(&j->refs, 1);
    }
    return j;
}

/* ---- the visual ---- */

struct AgentVisual {
    AgvType type;
    f32 cw, ch;
    f32 w, h;                       /* extent at (cw, ch) */
    /* diagram */
    MdDiagram *d;
    /* math */
    char *tex;
    u32   tex_len;
    f32   asc;
    /* the same formula in text style: limits beside their operator, so it
     * stands lower (agv_set_text_style); the extent follows the style */
    f32   tw, th, tasc, dw, dh, dasc;
    bool  text_style;
    /* svg */
    AgvSvgJob *job;
    f32   doc_w, doc_h;             /* CSS pixels */
    f32   k;                        /* pixels per CSS pixel at (cw, ch) */
    AgvText *texts;
    u32   ntexts;
    bool  dark_text;
};

static bool starts(const char *s, u32 n, const char *p) {
    u32 k = (u32)strlen(p);
    return n >= k && !memcmp(s, p, k);
}

AgvType agv_type_of(const char *src, u32 len) {
    u32 i = 0;
    while (i < len && (src[i] == ' ' || src[i] == '\n' || src[i] == '\r' || src[i] == '\t')) i++;
    if (starts(src + i, len - i, "$$")) return AGV_MATH;
    if (starts(src + i, len - i, "<svg") || starts(src + i, len - i, "<?xml")) return AGV_SVG;
    return AGV_DIAGRAM;
}

/* ---- math ---- */

static bool has_word(const char *s, u32 n, const char *w) {
    u32 k = (u32)strlen(w);
    for (u32 i = 0; i + k <= n; i++) if (!memcmp(s + i, w, k)) return true;
    return false;
}

bool agv_math_tex(const char *src, u32 len, char *out, u32 cap, u32 *out_len) {
    if (!src || !out || cap < 2) return false;
    u32 o = 0;
    bool any_math = false;
    /* every line but the delimiters, joined by spaces (TeX reads a line break as one) */
    for (u32 i = 0; i < len; ) {
        u32 e = i;
        while (e < len && src[e] != '\n') e++;
        const char *l = src + i;
        u32 n = e - i;
        i = e + 1;
        while (n && (l[0] == ' ' || l[0] == '\t' || l[0] == '\r')) { l++; n--; }
        while (n && (l[n - 1] == ' ' || l[n - 1] == '\t' || l[n - 1] == '\r')) n--;
        if (!n) continue;
        if ((n == 2 && (!memcmp(l, "$$", 2) || !memcmp(l, "\\[", 2) || !memcmp(l, "\\]", 2)))) continue;
        if (o && o + 1 < cap) out[o++] = ' ';
        if (o + n >= cap) return false;
        memcpy(out + o, l, n);
        o += n;
    }
    out[o] = 0;
    if (!o) return false;
    static const char *document[] = { "\\documentclass", "\\usepackage", "\\begin{document}", "\\section",
                                      "\\newcommand", "\\def\\", "\\chapter", "\\maketitle", NULL };
    for (u32 k = 0; document[k]; k++) if (has_word(out, o, document[k])) return false;
    for (u32 k = 0; k < o; k++) {
        char c = out[k];
        if (c == '\\' || c == '^' || c == '_' || c == '=' || c == '{' || c == '+' || c == '<' ||
            c == '>' || c == '&' || (c >= '0' && c <= '9')) { any_math = true; break; }
    }
    if (!any_math) return false;
    if (out_len) *out_len = o;
    return true;
}

/* ---- the SVG's text and size ---- */

static bool sp(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

/* The value of attribute `name` in a tag's attribute text, unquoted. */
static bool attr(const char *a, u32 n, const char *name, char *out, u32 cap) {
    u32 k = (u32)strlen(name);
    for (u32 i = 0; i + k < n; i++) {
        if ((i == 0 || sp(a[i - 1])) && !memcmp(a + i, name, k)) {
            u32 p = i + k;
            while (p < n && sp(a[p])) p++;
            if (p >= n || a[p] != '=') continue;
            p++;
            while (p < n && sp(a[p])) p++;
            if (p >= n || (a[p] != '"' && a[p] != '\'')) continue;
            char q = a[p++];
            u32 e = p;
            while (e < n && a[e] != q) e++;
            u32 l = e - p < cap - 1 ? e - p : cap - 1;
            memcpy(out, a + p, l);
            out[l] = 0;
            return true;
        }
    }
    return false;
}

/* `name: value` inside a style attribute. */
static bool style_prop(const char *style, const char *name, char *out, u32 cap) {
    const char *p = style;
    u32 k = (u32)strlen(name);
    while ((p = strstr(p, name)) != NULL) {
        if ((p == style || p[-1] == ';' || sp(p[-1])) && (p[k] == ':' || sp(p[k]))) {
            const char *v = p + k;
            while (*v && (sp(*v) || *v == ':')) v++;
            u32 l = 0;
            while (v[l] && v[l] != ';' && l + 1 < cap) { out[l] = v[l]; l++; }
            while (l && sp(out[l - 1])) l--;
            out[l] = 0;
            return true;
        }
        p += k;
    }
    return false;
}

/* An attribute or the same property of its style. */
static bool prop(const char *a, u32 n, const char *name, char *out, u32 cap) {
    char style[512];
    if (attr(a, n, "style", style, sizeof style) && style_prop(style, name, out, cap)) return true;
    return attr(a, n, name, out, cap);
}

/* A length in CSS pixels; `em` against `font`. */
static bool length(const char *v, f32 font, f32 *out) {
    char *e = NULL;
    f64 x = strtod(v, &e);
    if (e == v || !isfinite(x)) return false;
    while (*e == ' ') e++;
    if (!strncmp(e, "pt", 2)) x *= 4.0 / 3.0;
    else if (!strncmp(e, "em", 2) || !strncmp(e, "rem", 3)) x *= font;
    else if (!strncmp(e, "%", 1)) return false;
    *out = (f32)x;
    return true;
}

static u32 hexv(char c) {
    return c >= '0' && c <= '9' ? (u32)(c - '0') : c >= 'a' && c <= 'f' ? (u32)(c - 'a' + 10)
         : c >= 'A' && c <= 'F' ? (u32)(c - 'A' + 10) : 0;
}

/* A colour as 0xRRGGBBAA; 0 for none, black for what is not understood. */
static u32 colour(const char *v) {
    while (sp(*v)) v++;
    if (!strncmp(v, "none", 4) || !strncmp(v, "transparent", 11)) return 0;
    if (v[0] == '#') {
        u32 n = 0;
        while (n < 8 && ((v[1 + n] >= '0' && v[1 + n] <= '9') || (v[1 + n] >= 'a' && v[1 + n] <= 'f') ||
                         (v[1 + n] >= 'A' && v[1 + n] <= 'F'))) n++;
        if (n == 3) return (hexv(v[1]) * 17u) << 24 | (hexv(v[2]) * 17u) << 16 | (hexv(v[3]) * 17u) << 8 | 0xFFu;
        if (n >= 6) return (hexv(v[1]) << 28 | hexv(v[2]) << 24 | hexv(v[3]) << 20 | hexv(v[4]) << 16 |
                            hexv(v[5]) << 12 | hexv(v[6]) << 8) | 0xFFu;
        return 0x000000FFu;
    }
    if (!strncmp(v, "rgb", 3)) {
        const char *p = strchr(v, '(');
        f64 c[3] = { 0, 0, 0 };
        for (u32 i = 0; p && *p && *p != ')' && i < 3; i++) {
            char *e = NULL;
            c[i] = strtod(p + 1, &e);
            if (!e || e == p + 1) break;
            p = e;
            if (*p == '%') { c[i] *= 2.55; p++; }
            while (*p && *p != ',' && *p != ' ' && *p != ')') p++;
        }
        u32 r = (u32)fmin(255.0, fmax(0.0, c[0])), g = (u32)fmin(255.0, fmax(0.0, c[1])),
            b = (u32)fmin(255.0, fmax(0.0, c[2]));
        return r << 24 | g << 16 | b << 8 | 0xFFu;
    }
    static const struct { const char *n; u32 c; } names[] = {
        { "white", 0xFFFFFFFF }, { "black", 0x000000FF }, { "red", 0xFF0000FF }, { "green", 0x008000FF },
        { "blue", 0x0000FFFF }, { "gray", 0x808080FF }, { "grey", 0x808080FF }, { "orange", 0xFFA500FF },
        { "purple", 0x800080FF }, { "yellow", 0xFFFF00FF }, { "navy", 0x000080FF }, { "teal", 0x008080FF },
        { "maroon", 0x800000FF }, { "silver", 0xC0C0C0FF }, { "darkgray", 0xA9A9A9FF }, { "darkgrey", 0xA9A9A9FF },
        { "lightgray", 0xD3D3D3FF }, { "lightgrey", 0xD3D3D3FF }, { "dimgray", 0x696969FF }, { "dimgrey", 0x696969FF },
        { "crimson", 0xDC143CFF }, { "steelblue", 0x4682B4FF }, { "darkblue", 0x00008BFF }, { "darkgreen", 0x006400FF },
        { "darkred", 0x8B0000FF }, { "slategray", 0x708090FF }, { "slategrey", 0x708090FF }, { "brown", 0xA52A2AFF },
        { "pink", 0xFFC0CBFF }, { "gold", 0xFFD700FF }, { "cyan", 0x00FFFFFF }, { "magenta", 0xFF00FFFF },
        { "lime", 0x00FF00FF }, { "indigo", 0x4B0082FF }, { "tomato", 0xFF6347FF }, { "coral", 0xFF7F50FF },
        { "darkslategray", 0x2F4F4FFF }, { "royalblue", 0x4169E1FF }, { "seagreen", 0x2E8B57FF },
        { "whitesmoke", 0xF5F5F5FF }, { "ivory", 0xFFFFF0FF }, { "beige", 0xF5F5DCFF }, { NULL, 0 }
    };
    for (u32 i = 0; names[i].n; i++) {
        u32 k = (u32)strlen(names[i].n);
        if (!strncmp(v, names[i].n, k) && !((v[k] >= 'a' && v[k] <= 'z'))) return names[i].c;
    }
    return 0x000000FFu;
}

/* What a text inherits from the elements around it. */
typedef struct {
    f32  a, d, e, f;                 /* x' = a x + e, y' = d y + f */
    bool skewed;                     /* a rotation or a skew: its text is not drawn */
    f32  size;
    u8   anchor, baseline;
    bool bold;
    u32  fill;
} TState;

/* `translate()`, `scale()`, `matrix()` without a turn, composed onto `st`. */
static void transform(const char *v, TState *st) {
    const char *p = v;
    while (*p) {
        while (*p && (sp(*p) || *p == ',')) p++;
        if (!*p) break;
        const char *name = p;
        while (*p && *p != '(') p++;
        if (!*p) break;
        u32 nl = (u32)(p - name);
        while (nl && sp(name[nl - 1])) nl--;
        f64 n[6] = { 0 };
        u32 cnt = 0;
        p++;
        while (*p && *p != ')' && cnt < 6) {
            char *e = NULL;
            f64 x = strtod(p, &e);
            if (e == p) { p++; continue; }
            n[cnt++] = x;
            p = e;
        }
        if (*p == ')') p++;
        f32 a = 1, d = 1, e = 0, f = 0;
        if (nl == 9 && !memcmp(name, "translate", 9)) { e = (f32)n[0]; f = cnt > 1 ? (f32)n[1] : 0; }
        else if (nl == 5 && !memcmp(name, "scale", 5)) { a = (f32)n[0]; d = cnt > 1 ? (f32)n[1] : a; }
        else if (nl == 6 && !memcmp(name, "matrix", 6) && cnt == 6) {
            if (fabs(n[1]) > 1e-6 || fabs(n[2]) > 1e-6) st->skewed = true;
            a = (f32)n[0]; d = (f32)n[3]; e = (f32)n[4]; f = (f32)n[5];
        } else if (nl == 6 && !memcmp(name, "rotate", 6)) {
            if (fabs(n[0]) > 1e-6) st->skewed = true;
        } else st->skewed = true;
        /* the new one applies first: x' = st(this(x)) */
        st->e += st->a * e;
        st->f += st->d * f;
        st->a *= a;
        st->d *= d;
    }
}

static void tstate_apply(const char *a, u32 n, TState *st) {
    char v[256];
    if (attr(a, n, "transform", v, sizeof v)) transform(v, st);
    f32 x;
    if (prop(a, n, "font-size", v, sizeof v) && length(v, st->size, &x) && x > 0) st->size = x;
    if (prop(a, n, "text-anchor", v, sizeof v))
        st->anchor = !strncmp(v, "middle", 6) ? 1 : !strncmp(v, "end", 3) ? 2 : 0;
    if (prop(a, n, "dominant-baseline", v, sizeof v) || prop(a, n, "alignment-baseline", v, sizeof v))
        st->baseline = (!strncmp(v, "middle", 6) || !strncmp(v, "central", 7)) ? 1
                     : (!strncmp(v, "hanging", 7) || !strncmp(v, "text-before-edge", 16)) ? 2 : 0;
    if (prop(a, n, "font-weight", v, sizeof v))
        st->bold = !strncmp(v, "bold", 4) || atoi(v) >= 600;
    if (prop(a, n, "fill", v, sizeof v)) st->fill = colour(v);
}

/* Character data with its entities read and its blanks folded, appended. */
static void text_append(char *dst, u32 cap, const char *s, u32 n) {
    u32 o = (u32)strlen(dst);
    bool space = o > 0 && dst[o - 1] == ' ';
    for (u32 i = 0; i < n && o + 5 < cap; i++) {
        char c = s[i];
        if (sp(c)) { if (o && !space) { dst[o++] = ' '; space = true; } continue; }
        space = false;
        if (c == '&') {
            u32 e = i + 1;
            while (e < n && e - i < 10 && s[e] != ';') e++;
            if (e < n && s[e] == ';') {
                const char *ent = s + i + 1;
                u32 el = e - i - 1;
                u32 cp = 0;
                if (el == 2 && !memcmp(ent, "lt", 2)) cp = '<';
                else if (el == 2 && !memcmp(ent, "gt", 2)) cp = '>';
                else if (el == 3 && !memcmp(ent, "amp", 3)) cp = '&';
                else if (el == 4 && !memcmp(ent, "quot", 4)) cp = '"';
                else if (el == 4 && !memcmp(ent, "apos", 4)) cp = '\'';
                else if (el == 4 && !memcmp(ent, "nbsp", 4)) cp = ' ';
                else if (el > 1 && ent[0] == '#')
                    cp = (u32)(ent[1] == 'x' ? strtoul(ent + 2, NULL, 16) : strtoul(ent + 1, NULL, 10));
                if (cp && cp < 0x110000) {
                    u8 buf[4];
                    u32 k = utf8_encode(cp, buf);
                    if (k && o + k < cap) { memcpy(dst + o, buf, k); o += k; }
                    i = e;
                    continue;
                }
            }
        }
        dst[o++] = c;
    }
    dst[o] = 0;
}

typedef struct { const char *s; u32 n, i; } Xml;

/* The next tag: its name, attribute text and kind, the character data before
 * it in [*text, *text + *tn). False at the end. */
typedef struct { const char *name; u32 nl; const char *attrs; u32 al; bool close, empty; } Tag;

static bool xml_next(Xml *x, const char **text, u32 *tn, Tag *t) {
    const char *s = x->s;
    u32 n = x->n;
    for (;;) {
        u32 lt = x->i;
        while (lt < n && s[lt] != '<') lt++;
        *text = s + x->i;
        *tn = lt - x->i;
        if (lt >= n) { x->i = n; return false; }
        if (lt + 3 < n && !memcmp(s + lt, "<!--", 4)) {
            const char *e = NULL;
            for (u32 k = lt + 4; k + 2 < n; k++) if (!memcmp(s + k, "-->", 3)) { e = s + k; break; }
            x->i = e ? (u32)(e - s) + 3 : n;
            if (*tn) { t->name = NULL; t->nl = 0; return true; }
            continue;
        }
        if (lt + 8 < n && !memcmp(s + lt, "<![CDATA[", 9)) {
            u32 k = lt + 9;
            while (k + 2 < n && memcmp(s + k, "]]>", 3)) k++;
            *text = s + lt + 9;
            *tn = k - lt - 9;
            x->i = k + 3 <= n ? k + 3 : n;
            t->name = NULL; t->nl = 0;
            return true;
        }
        u32 gt = lt + 1;
        char q = 0;
        while (gt < n && (q || s[gt] != '>')) {
            if (q) { if (s[gt] == q) q = 0; }
            else if (s[gt] == '"' || s[gt] == '\'') q = s[gt];
            gt++;
        }
        if (gt >= n) { x->i = n; return false; }
        x->i = gt + 1;
        if (s[lt + 1] == '?' || s[lt + 1] == '!') {
            if (*tn) { t->name = NULL; t->nl = 0; return true; }
            continue;
        }
        t->close = s[lt + 1] == '/';
        u32 p = lt + 1 + (t->close ? 1 : 0);
        t->name = s + p;
        while (p < gt && !sp(s[p]) && s[p] != '/') p++;
        t->nl = (u32)(s + p - t->name);
        t->attrs = s + p;
        t->empty = gt > lt && s[gt - 1] == '/';
        t->al = (u32)(gt - p) - (t->empty ? 1 : 0);
        return true;
    }
}

static bool tag_is(const Tag *t, const char *name) {
    u32 k = (u32)strlen(name);
    if (t->nl == k && !memcmp(t->name, name, k)) return true;
    /* a namespace prefix: `svg:text` */
    return t->nl > k + 1 && t->name[t->nl - k - 1] == ':' && !memcmp(t->name + t->nl - k, name, k);
}

static bool numlist(const char *v, f64 *out, u32 want) {
    const char *p = v;
    for (u32 i = 0; i < want; i++) {
        while (*p && (sp(*p) || *p == ',')) p++;
        char *e = NULL;
        out[i] = strtod(p, &e);
        if (e == p || !isfinite(out[i])) return false;
        p = e;
    }
    return true;
}

bool agv_svg_layout(const char *src, u32 len, f32 *doc_w, f32 *doc_h,
                    AgvText *texts, u32 cap, u32 *ntexts) {
    if (ntexts) *ntexts = 0;
    Xml x = { src, len, 0 };
    const char *text;
    u32 tn;
    Tag t;
    TState stack[32];
    u32 depth = 0;
    bool seen_svg = false;
    f32 vb[4] = { 0, 0, 0, 0 };
    f32 dw = 0, dh = 0;
    /* the open <text>'s run, and where its pen is */
    i32 cur = -1;
    f32 tx = 0, ty = 0;
    u32 nt = 0;
    while (xml_next(&x, &text, &tn, &t)) {
        if (cur >= 0 && tn) text_append(texts[cur].text, sizeof texts[cur].text, text, tn);
        if (!t.name || !t.nl) continue;
        if (t.close) {
            if (tag_is(&t, "text") && cur >= 0) {
                /* trim; a run with nothing to show is dropped */
                char *s = texts[cur].text;
                u32 l = (u32)strlen(s);
                while (l && s[l - 1] == ' ') s[--l] = 0;
                if (!l) nt--;
                cur = -1;
            }
            if (tag_is(&t, "tspan")) {
                if (depth > 1) depth--;
                continue;
            }
            if (depth > 1) depth--;
            continue;
        }
        if (!seen_svg) {
            if (!tag_is(&t, "svg")) continue;
            seen_svg = true;
            char v[128];
            f64 nb[4];
            if (attr(t.attrs, t.al, "viewBox", v, sizeof v) && numlist(v, nb, 4) && nb[2] > 0 && nb[3] > 0)
                for (u32 i = 0; i < 4; i++) vb[i] = (f32)nb[i];
            f32 l;
            if (attr(t.attrs, t.al, "width", v, sizeof v) && length(v, 16, &l) && l > 0) dw = l;
            if (attr(t.attrs, t.al, "height", v, sizeof v) && length(v, 16, &l) && l > 0) dh = l;
            if (vb[2] > 0) {
                if (dw <= 0 && dh <= 0) { dw = vb[2]; dh = vb[3]; }
                else if (dw <= 0) dw = dh * vb[2] / vb[3];
                else if (dh <= 0) dh = dw * vb[3] / vb[2];
            }
            if (!(dw > 0) || !isfinite(dw)) dw = 300;
            if (!(dh > 0) || !isfinite(dh)) dh = 150;
            /* a size no picture has is no picture */
            if (dw > 100000.0f || dh > 100000.0f) return false;
            /* user units to CSS pixels: the viewBox fitted in, kept whole and centred */
            TState root = { 1, 1, 0, 0, false, 16, 0, 0, false, 0x000000FFu };
            if (vb[2] > 0) {
                f32 sc = fminf(dw / vb[2], dh / vb[3]);
                root.a = root.d = sc;
                root.e = (dw - vb[2] * sc) * 0.5f - vb[0] * sc;
                root.f = (dh - vb[3] * sc) * 0.5f - vb[1] * sc;
            }
            tstate_apply(t.attrs, t.al, &root);
            stack[0] = root;
            depth = 1;
            if (t.empty) break;
            continue;
        }
        if (!depth) break;
        TState st = stack[depth - 1];
        tstate_apply(t.attrs, t.al, &st);
        bool is_text = tag_is(&t, "text"), is_span = tag_is(&t, "tspan");
        if (is_text || (is_span && cur >= 0)) {
            char v[64];
            f32 nx = is_text ? 0 : tx, ny = is_text ? 0 : ty, l;
            bool moved = is_text;
            if (attr(t.attrs, t.al, "x", v, sizeof v) && length(v, st.size, &l)) { nx = l; moved = true; }
            if (attr(t.attrs, t.al, "y", v, sizeof v) && length(v, st.size, &l)) { ny = l; moved = true; }
            if (attr(t.attrs, t.al, "dx", v, sizeof v) && length(v, st.size, &l)) { nx += l; moved = true; }
            if (attr(t.attrs, t.al, "dy", v, sizeof v) && length(v, st.size, &l)) { ny += l; moved = true; }
            /* a span moved elsewhere is a run of its own (an empty run before it is dropped) */
            if (moved) {
                if (cur >= 0) {
                    char *s = texts[cur].text;
                    u32 sl = (u32)strlen(s);
                    while (sl && s[sl - 1] == ' ') s[--sl] = 0;
                    if (!sl) nt--;
                }
                cur = -1;
                f32 ox = st.a * nx + st.e, oy = st.d * ny + st.f, os = st.size * fabsf(st.d);
                bool placed = isfinite(ox) && isfinite(oy) && fabsf(ox) < 1e6f && fabsf(oy) < 1e6f &&
                              os > 0.0f && os < 1e4f;
                if (!st.skewed && st.fill && placed && nt < cap) {
                    AgvText *o = &texts[nt];
                    memset(o, 0, sizeof *o);
                    o->x = ox;
                    o->y = oy;
                    o->size = os;
                    o->anchor = st.anchor;
                    o->baseline = st.baseline;
                    o->bold = st.bold;
                    o->rgba = st.fill;
                    cur = (i32)nt++;
                }
            }
            tx = nx; ty = ny;
            if (t.empty) continue;
        }
        if (!t.empty && depth < 32) stack[depth++] = st;
    }
    if (cur >= 0) {
        char *s = texts[cur].text;
        u32 l = (u32)strlen(s);
        while (l && s[l - 1] == ' ') s[--l] = 0;
        if (!l) nt--;
    }
    if (!seen_svg) return false;
    if (doc_w) *doc_w = dw;
    if (doc_h) *doc_h = dh;
    if (ntexts) *ntexts = nt;
    return true;
}

/* ---- create / destroy ---- */

AgentVisual *agv_create(const char *src, u32 len, f32 cw, f32 ch, bool compact) {
    if (!src || !len || !(cw > 0) || !(ch > 0)) return NULL;
    AgentVisual *v = (AgentVisual *)calloc(1, sizeof *v);
    if (!v) return NULL;
    v->type = agv_type_of(src, len);
    v->cw = cw; v->ch = ch;
    switch (v->type) {
    case AGV_DIAGRAM:
        v->d = md_diagram_create_ex((const u8 *)src, len, cw, ch, compact ? MD_DIAGRAM_COMPACT : MD_DIAGRAM_READING);
        if (!v->d) break;
        md_diagram_extent(v->d, &v->w, &v->h);
        return v;
    case AGV_MATH: {
        char *tex = (char *)malloc(AGV_TEX_CAP);
        u32 tl = 0;
        if (!tex || !agv_math_tex(src, len, tex, AGV_TEX_CAP, &tl)) { free(tex); break; }
        v->tex = tex;
        v->tex_len = tl;
        f32 w = 0, a = 0, d = 0;
        md_math_measure((const u8 *)tex, tl, cw, ch, true, &w, &a, &d);
        if (!(w > 0) || !(a + d > 0)) break;
        v->w = v->dw = w; v->h = v->dh = a + d; v->asc = v->dasc = a;
        md_math_measure((const u8 *)tex, tl, cw, ch, false, &w, &a, &d);
        v->tw = w > 0 ? w : v->dw;
        v->th = a + d > 0 ? a + d : v->dh;
        v->tasc = a + d > 0 ? a : v->dasc;
        return v;
    }
    case AGV_SVG: {
        AgvText *texts = (AgvText *)calloc(AGV_TEXTS, sizeof *texts);
        u32 nt = 0;
        if (!texts || !agv_svg_layout(src, len, &v->doc_w, &v->doc_h, texts, AGV_TEXTS, &nt)) { free(texts); break; }
        v->texts = texts;
        v->ntexts = nt;
        for (u32 i = 0; i < nt; i++) {
            u32 c = texts[i].rgba;
            f32 luma = 0.2126f * (f32)(c >> 24) + 0.7152f * (f32)((c >> 16) & 0xFF) + 0.0722f * (f32)((c >> 8) & 0xFF);
            if (luma < 128.0f) v->dark_text = true;
        }
        v->k = cw / AGV_CSS_ADVANCE;
        v->w = v->doc_w * v->k;
        v->h = v->doc_h * v->k;
        f32 longest = fmaxf(v->w, v->h) * (compact ? 1.0f : 2.0f);
        i32 edge = (i32)ceilf(longest);
        if (edge > AGV_SVG_EDGE_MAX) edge = AGV_SVG_EDGE_MAX;
        if (edge < AGV_SVG_EDGE_MIN) edge = AGV_SVG_EDGE_MIN;
        v->job = job_start(src, len, edge);
        if (!v->job) break;
        return v;
    }
    }
    agv_destroy(v);
    return NULL;
}

void agv_destroy(AgentVisual *v) {
    if (!v) return;
    md_diagram_destroy(v->d);
    free(v->tex);
    free(v->texts);
    job_release(v->job);
    free(v);
}

AgvType agv_type(const AgentVisual *v) { return v ? v->type : AGV_DIAGRAM; }

void agv_set_text_style(AgentVisual *v, bool on) {
    if (!v || v->type != AGV_MATH || v->text_style == on) return;
    v->text_style = on;
    v->w = on ? v->tw : v->dw;
    v->h = on ? v->th : v->dh;
    v->asc = on ? v->tasc : v->dasc;
}

void agv_extent(const AgentVisual *v, f32 *w, f32 *h) {
    if (w) *w = v ? v->w : 0;
    if (h) *h = v ? v->h : 0;
}

usize agv_bytes(const AgentVisual *v) {
    if (!v) return 0;
    usize b = sizeof *v + md_diagram_bytes(v->d) + (v->tex ? AGV_TEX_CAP : 0) +
              (v->texts ? AGV_TEXTS * sizeof(AgvText) : 0);
    if (v->job) {
        b += sizeof *v->job + v->job->len;
        if (atomic_load_explicit(&v->job->state, memory_order_acquire) == 1)
            b += (usize)v->job->img.w * (usize)v->job->img.h * 4u;
    }
    return b;
}

void agv_title(const AgentVisual *v, char *out, u32 cap) {
    if (!out || !cap) return;
    if (!v) { out[0] = 0; return; }
    switch (v->type) {
    case AGV_MATH: snprintf(out, cap, "LaTeX \xC2\xB7 formula"); break;
    case AGV_SVG:  snprintf(out, cap, "SVG \xC2\xB7 %.0f \xC3\x97 %.0f", (f64)v->doc_w, (f64)v->doc_h); break;
    default:
        snprintf(out, cap, "%s \xC2\xB7 %s", md_diagram_lang_name(md_diagram_lang_of(v->d)),
                 md_diagram_kind_name(md_diagram_kind_of(v->d)));
        break;
    }
}

bool agv_pending(const AgentVisual *v) {
    return v && v->job && atomic_load_explicit(&v->job->state, memory_order_acquire) == 0;
}

const char *agv_failure(const AgentVisual *v) {
    if (!v || !v->job || atomic_load_explicit(&v->job->state, memory_order_acquire) != 2) return NULL;
    return svg_status_text(v->job->status);
}

/* ---- drawing ---- */

static Color rgba_color(u32 c, f32 alpha) {
    return (Color){ (f32)(c >> 24) / 255.0f, (f32)((c >> 16) & 0xFF) / 255.0f,
                    (f32)((c >> 8) & 0xFF) / 255.0f, (f32)(c & 0xFF) / 255.0f * alpha };
}

/* A message in the middle of the visual's box, in the text's dim ink. */
static void draw_note(Renderer *r, const AgentVisual *v, const MdDiagramInks *inks, const char *msg,
                      f32 x, f32 y, f32 s, f32 alpha) {
    f32 gcw = v->cw * fminf(1.0f, fmaxf(s, 0.6f)), gch = v->ch * (gcw / v->cw);
    i32 n = (i32)utf8_len((const u8 *)msg, strlen(msg));
    f32 tx = x + (v->w * s - (f32)n * gcw) * 0.5f, ty = y + (v->h * s - gch) * 0.5f;
    Color c = inks->text_dim;
    c.a *= alpha;
    renderer_set_ui_scale(r, gcw, gch);
    f32 cx = tx;
    for (u32 i = 0; msg[i]; ) {
        u32 cp;
        u32 k = utf8_decode((const u8 *)msg + i, (u32)strlen(msg + i), &cp);
        if (!k) break;
        i += k;
        renderer_push_glyph(r, cx, ty, cp, c);
        cx += gcw;
    }
    renderer_flush_glyphs(r);
}

static void draw_svg(Renderer *r, const AgentVisual *v, const MdDiagramInks *inks,
                     f32 x, f32 y, f32 s, f32 alpha) {
    AgvSvgJob *j = v->job;
    i32 st = atomic_load_explicit(&j->state, memory_order_acquire);
    if (st == 0) { draw_note(r, v, inks, "Drawing\xE2\x80\xA6", x, y, s, alpha); return; }
    if (st == 2) { draw_note(r, v, inks, svg_status_text(j->status), x, y, s, alpha); return; }
    const SvgImage *im = &j->img;
    f32 w = v->w * s, h = v->h * s;
    /* Dark ink, or dark text on a picture with nothing under it, sits on a
     * light plate, as on the page it was drawn for. */
    bool under = im->rgba && im->w > 1 && im->h > 1 && im->rgba[3] > 200 &&
                 im->rgba[((usize)im->h * (usize)im->w - 1) * 4 + 3] > 200;
    if (im->dark_ink || (v->dark_text && !under)) {
        f32 pad = fmaxf(2.0f, 6.0f * s * v->k);
        Color plate = { 0.96f, 0.96f, 0.965f, alpha };
        renderer_draw_rrect(r, x - pad, y - pad, w + pad * 2, h + pad * 2, plate, pad, pad, pad, pad, 0, 0, 0, 0);
        renderer_flush_rrects(r);
    }
    renderer_draw_image_cached(r, j, j->gen, im->rgba, im->w, im->h, x, y, w, h);
    if (!v->ntexts) return;
    /* the text nanosvg leaves out, in the terminal's face at its size */
    f32 em = r->font.cap_height > 0.0f ? r->font.cap_height / 0.7f : r->font.cell_height / 1.25f;
    if (!(em > 0.0f) || !(r->font.cell_width > 0.0f)) return;
    f32 px_per_css = v->k * s;
    for (u32 i = 0; i < v->ntexts; i++) {
        const AgvText *t = &v->texts[i];
        f32 g = t->size * px_per_css / em;
        f32 gcw = r->font.cell_width * g, gch = r->font.cell_height * g;
        if (gcw < 2.0f) continue;
        i32 cells = 0;
        u32 tl = (u32)strlen(t->text);
        for (u32 k = 0; k < tl; ) {
            u32 cp;
            u32 n = utf8_decode((const u8 *)t->text + k, tl - k, &cp);
            if (!n) break;
            k += n;
            cells += utf8_char_width(cp) >= 2 ? 2 : 1;
        }
        f32 ax = x + t->x * px_per_css, ay = y + t->y * px_per_css;
        f32 tw = (f32)cells * gcw;
        f32 left = t->anchor == 1 ? ax - tw * 0.5f : t->anchor == 2 ? ax - tw : ax;
        f32 asc = r->font.ascent * g;
        f32 top = t->baseline == 1 ? ay - gch * 0.5f : t->baseline == 2 ? ay : ay - asc;
        Color c = rgba_color(t->rgba, alpha);
        renderer_set_ui_scale(r, gcw, gch);
        f32 cx = left;
        for (u32 k = 0; k < tl; ) {
            u32 cp;
            u32 n = utf8_decode((const u8 *)t->text + k, tl - k, &cp);
            if (!n) break;
            k += n;
            renderer_push_glyph(r, cx, top, cp, c);
            cx += gcw * (utf8_char_width(cp) >= 2 ? 2.0f : 1.0f);
        }
        renderer_flush_glyphs(r);
    }
}

void agv_draw(Renderer *r, const AgentVisual *v, const MdDiagramInks *inks,
              f32 x, f32 y, f32 s, f32 alpha) {
    if (!r || !v || !inks || !(s > 0.0f) || !isfinite(x) || !isfinite(y)) return;
    switch (v->type) {
    case AGV_DIAGRAM:
        md_diagram_draw(r, v->d, inks, x, y, s, alpha);
        break;
    case AGV_MATH: {
        Color fg = inks->text;
        fg.a *= alpha;
        md_math_render(r, (const u8 *)v->tex, v->tex_len, x, y + v->asc * s, v->cw * s, v->ch * s,
                       !v->text_style, fg);
        break;
    }
    case AGV_SVG:
        draw_svg(r, v, inks, x, y, s, alpha);
        break;
    }
}
