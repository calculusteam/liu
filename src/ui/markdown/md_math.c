/* Liu — minimal native TeX-math engine. See md_math.h. */
#include "ui/markdown/md_math.h"
#include "core/utf8.h"

#include <string.h>
#include <math.h>

/* ---- scratch bump allocator (reset per public call; bounded formulas) ---- */
#define MM_SCRATCH (96 * 1024)
static u8    g_mm_buf[MM_SCRATCH];
static usize g_mm_off;
static void  mm_reset(void) { g_mm_off = 0; }
static void *mm_alloc(usize n) {
    n = (n + 7u) & ~7u;
    if (g_mm_off + n > MM_SCRATCH) return NULL;
    void *p = &g_mm_buf[g_mm_off];
    g_mm_off += n;
    return p;
}

/* ---- box model ---- */
typedef enum { MB_GLYPH, MB_HLIST, MB_FRAC, MB_SCRIPT, MB_SQRT, MB_KERN } MBKind;

typedef struct MBox {
    u8  kind;
    f32 w, h, d;       /* width, ascent (above baseline), descent (below) */
    f32 scale;         /* glyph scale (MB_GLYPH) */
    u32 cp;            /* MB_GLYPH codepoint */
    f32 a, b;          /* SCRIPT: sup_shift, sub_shift. FRAC: axis, rule_thick. SQRT: surd_w, rule_thick */
    struct MBox **kids;
    u32 nkids;
} MBox;

/* Metrics (monospace cell approximation, scaled). */
static f32 g_cw, g_ch;
#define ASCENT_F  0.75f
#define DESCENT_F 0.25f
static inline f32 GA(f32 s) { return ASCENT_F  * g_ch * s; }
static inline f32 GD(f32 s) { return DESCENT_F * g_ch * s; }
static inline f32 AXIS(f32 s) { return 0.26f * g_ch * s; }
/* Child scale for scripts/fraction parts: one step down, clamped. */
static inline f32 sub_scale(f32 s) { return s <= 0.55f ? s : s * 0.72f; }

static MBox *mb_new(u8 kind) {
    MBox *b = mm_alloc(sizeof *b);
    if (b) { memset(b, 0, sizeof *b); b->kind = kind; }
    return b;
}

static MBox *mb_glyph(u32 cp, f32 scale) {
    MBox *b = mb_new(MB_GLYPH);
    if (!b) return NULL;
    b->cp = cp; b->scale = scale;
    b->w = g_cw * scale; b->h = GA(scale); b->d = GD(scale);
    return b;
}

static MBox *mb_kern(f32 w) {
    MBox *b = mb_new(MB_KERN);
    if (b) b->w = w;
    return b;
}

static MBox *mb_hlist(MBox **kids, u32 n) {
    if (n == 1) return kids[0];
    MBox *b = mb_new(MB_HLIST);
    if (!b) return NULL;
    MBox **arr = mm_alloc(n * sizeof(MBox *));
    if (!arr) return NULL;
    f32 w = 0, h = 0, d = 0;
    for (u32 i = 0; i < n; i++) {
        arr[i] = kids[i];
        if (!kids[i]) continue;
        w += kids[i]->w;
        if (kids[i]->h > h) h = kids[i]->h;
        if (kids[i]->d > d) d = kids[i]->d;
    }
    b->kids = arr; b->nkids = n; b->w = w; b->h = h; b->d = d;
    return b;
}

static MBox *mb_frac(MBox *num, MBox *den, f32 scale) {
    MBox *b = mb_new(MB_FRAC);
    if (!b) return NULL;
    MBox **arr = mm_alloc(2 * sizeof(MBox *));
    if (!arr) return NULL;
    arr[0] = num; arr[1] = den;
    b->kids = arr; b->nkids = 2;
    f32 thick = fmaxf(1.0f, g_ch * scale * 0.045f);
    f32 axis  = AXIS(scale);
    f32 gap   = 0.18f * g_ch * scale;
    f32 nw = num ? num->w : 0, dw = den ? den->w : 0;
    f32 nh = num ? (num->h + num->d) : 0;
    f32 dh = den ? (den->h + den->d) : 0;
    b->w = fmaxf(nw, dw) + 0.5f * g_cw * scale;
    b->h = axis + thick * 0.5f + gap + nh;
    b->d = (dh + gap + thick * 0.5f) - axis;
    if (b->d < 0) b->d = 0;
    b->a = axis; b->b = thick; b->scale = scale;   /* scale → gap in draw_box */
    return b;
}

static MBox *mb_script(MBox *base, MBox *sup, MBox *sub) {
    MBox *b = mb_new(MB_SCRIPT);
    if (!b) return NULL;
    MBox **arr = mm_alloc(3 * sizeof(MBox *));
    if (!arr) return NULL;
    arr[0] = base; arr[1] = sup; arr[2] = sub;
    b->kids = arr; b->nkids = 3;
    f32 bh = base ? base->h : GA(1.0f), bd = base ? base->d : GD(1.0f);
    f32 bw = base ? base->w : 0;
    f32 supw = sup ? sup->w : 0, subw = sub ? sub->w : 0;
    b->w = bw + (supw > subw ? supw : subw);
    f32 sup_shift = sup ? (0.5f * bh + 0.10f * g_ch) : 0;
    f32 sub_shift = sub ? (0.30f * bd + 0.18f * g_ch) : 0;
    f32 h = bh, d = bd;
    if (sup && sup_shift + sup->h > h) h = sup_shift + sup->h;
    if (sub && sub_shift + sub->d > d) d = sub_shift + sub->d;
    b->h = h; b->d = d; b->a = sup_shift; b->b = sub_shift;
    return b;
}

static MBox *mb_sqrt(MBox *rad, f32 scale) {
    MBox *b = mb_new(MB_SQRT);
    if (!b) return NULL;
    MBox **arr = mm_alloc(1 * sizeof(MBox *));
    if (!arr) return NULL;
    arr[0] = rad; b->kids = arr; b->nkids = 1;
    f32 surd_w = g_cw * scale;
    f32 thick  = fmaxf(1.0f, g_ch * scale * 0.045f);
    f32 over   = 0.16f * g_ch * scale;     /* gap above radicand for the vinculum */
    f32 rw = rad ? rad->w : g_cw * scale;
    f32 rh = rad ? rad->h : GA(scale);
    f32 rd = rad ? rad->d : GD(scale);
    b->w = surd_w + rw + 0.25f * g_cw * scale;
    b->h = rh + over + thick;
    b->d = rd;
    b->a = surd_w; b->b = thick;
    b->scale = scale;
    return b;
}

/* ---- command tables ---- */
typedef struct { const char *name; u32 cp; } MathSym;
static const MathSym MATH_SYMS[] = {
    {"alpha",0x3B1},{"beta",0x3B2},{"gamma",0x3B3},{"delta",0x3B4},{"epsilon",0x3B5},
    {"varepsilon",0x3B5},{"zeta",0x3B6},{"eta",0x3B7},{"theta",0x3B8},{"vartheta",0x3D1},
    {"iota",0x3B9},{"kappa",0x3BA},{"lambda",0x3BB},{"mu",0x3BC},{"nu",0x3BD},{"xi",0x3BE},
    {"pi",0x3C0},{"varpi",0x3D6},{"rho",0x3C1},{"sigma",0x3C3},{"tau",0x3C4},{"upsilon",0x3C5},
    {"phi",0x3C6},{"varphi",0x3D5},{"chi",0x3C7},{"psi",0x3C8},{"omega",0x3C9},
    {"Gamma",0x393},{"Delta",0x394},{"Theta",0x398},{"Lambda",0x39B},{"Xi",0x39E},
    {"Pi",0x3A0},{"Sigma",0x3A3},{"Upsilon",0x3A5},{"Phi",0x3A6},{"Psi",0x3A8},{"Omega",0x3A9},
    {"sum",0x2211},{"prod",0x220F},{"coprod",0x2210},{"int",0x222B},{"oint",0x222E},
    {"pm",0xB1},{"mp",0x2213},{"times",0xD7},{"div",0xF7},{"cdot",0x22C5},{"ast",0x2217},
    {"cdots",0x22EF},{"ldots",0x2026},{"dots",0x2026},{"vdots",0x22EE},{"ddots",0x22F1},
    {"leq",0x2264},{"le",0x2264},{"geq",0x2265},{"ge",0x2265},{"neq",0x2260},{"ne",0x2260},
    {"approx",0x2248},{"equiv",0x2261},{"cong",0x2245},{"sim",0x223C},{"simeq",0x2243},
    {"ll",0x226A},{"gg",0x226B},{"propto",0x221D},
    {"rightarrow",0x2192},{"to",0x2192},{"leftarrow",0x2190},{"gets",0x2190},
    {"leftrightarrow",0x2194},{"Rightarrow",0x21D2},{"Leftarrow",0x21D0},{"Leftrightarrow",0x21D4},
    {"mapsto",0x21A6},{"uparrow",0x2191},{"downarrow",0x2193},
    {"infty",0x221E},{"partial",0x2202},{"nabla",0x2207},{"forall",0x2200},{"exists",0x2203},
    {"nexists",0x2204},{"emptyset",0x2205},{"varnothing",0x2205},
    {"in",0x2208},{"notin",0x2209},{"ni",0x220B},{"subset",0x2282},{"supset",0x2283},
    {"subseteq",0x2286},{"supseteq",0x2287},{"cup",0x222A},{"cap",0x2229},{"setminus",0x2216},
    {"angle",0x2220},{"perp",0x22A5},{"parallel",0x2225},{"mid",0x2223},
    {"oplus",0x2295},{"ominus",0x2296},{"otimes",0x2297},{"odot",0x2299},
    {"circ",0x2218},{"bullet",0x2219},{"star",0x22C6},{"dagger",0x2020},
    {"prime",0x2032},{"hbar",0x210F},{"ell",0x2113},{"Re",0x211C},{"Im",0x2111},{"wp",0x2118},
    {"aleph",0x2135},{"land",0x2227},{"wedge",0x2227},{"lor",0x2228},{"vee",0x2228},
    {"neg",0xAC},{"lnot",0xAC},{"top",0x22A4},{"bot",0x22A5},{"vdash",0x22A2},{"models",0x22A8},
    {"langle",0x27E8},{"rangle",0x27E9},{"lceil",0x2308},{"rceil",0x2309},
    {"lfloor",0x230A},{"rfloor",0x230B},{"backslash",0x5C},
    {NULL,0}
};
static const char *MATH_FUNCS[] = {
    "sin","cos","tan","cot","sec","csc","sinh","cosh","tanh","coth",
    "log","ln","lg","exp","lim","limsup","liminf","max","min","sup","inf",
    "det","dim","ker","deg","gcd","arg","Pr","mod","arcsin","arccos","arctan",NULL
};

/* ---- parser ---- */
typedef struct { const u8 *s; u32 n; u32 i; } MP;

/* Recursion cap for the math parser. Each level costs a parse_list stack frame
 * (which holds a 256-pointer kids[] array), so cap well below what would blow
 * the C stack on pathological deeply-nested input ({{{...}}}, \frac\frac…). */
#define MM_MAX_NEST 64

static MBox *parse_list(MP *p, f32 scale, bool grouped, int depth);

/* Consume a brace-balanced group body (p->i is just past the opening '{'),
 * used to skip without recursing when the depth cap is hit. */
static void mm_skip_group(MP *p) {
    int b = 1;
    while (p->i < p->n && b > 0) {
        u8 c = p->s[p->i++];
        if (c == '{') b++;
        else if (c == '}') b--;
    }
}

/* ASCII alpha test (no <ctype.h> locale surprises). */
static inline int isalpha_ascii(u8 c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static u32 read_cmd_name(MP *p, char *out, u32 cap) {
    u32 k = 0;
    /* single non-alpha command like \, \{ \| */
    if (p->i < p->n && !isalpha_ascii(p->s[p->i])) {
        if (k < cap - 1) out[k++] = (char)p->s[p->i];
        p->i++;
        out[k] = 0;
        return k;
    }
    while (p->i < p->n && isalpha_ascii(p->s[p->i])) {
        if (k < cap - 1) out[k++] = (char)p->s[p->i];
        p->i++;
    }
    out[k] = 0;
    return k;
}

static MBox *letters_to_hlist(const char *s, u32 n, f32 scale) {
    MBox *kids[64]; u32 nk = 0;
    for (u32 i = 0; i < n && nk < 64; i++) {
        MBox *g = mb_glyph((u32)(u8)s[i], scale);
        if (g) kids[nk++] = g;
    }
    if (nk == 0) return mb_kern(0);
    return mb_hlist(kids, nk);
}

/* Parse one unit: a group {…}, a command, or a single atom. */
static MBox *parse_unit(MP *p, f32 scale, int depth) {
    if (p->i >= p->n) return NULL;
    u8 c = p->s[p->i];

    /* Depth cap: consume a single token flat without descending, so a
     * pathological \frac\frac… or {{{…}}} chain can't blow the C stack. */
    if (depth > MM_MAX_NEST) {
        if (c == '{') { p->i++; mm_skip_group(p); return mb_kern(0); }
        if (c == '\\') { p->i++; char nm[32]; read_cmd_name(p, nm, sizeof nm); return mb_kern(0); }
        u32 cp = 0; u32 adv = utf8_decode(p->s + p->i, p->n - p->i, &cp);
        p->i += adv ? adv : 1;
        return mb_glyph(cp, scale);
    }

    if (c == '{') {
        p->i++;
        return parse_list(p, scale, true, depth + 1);
    }
    if (c == '}') return NULL;  /* caller handles */

    if (c == '\\') {
        p->i++;
        char name[32];
        u32 nl = read_cmd_name(p, name, sizeof name);
        if (nl == 0) return mb_kern(0);

        if (strcmp(name, "frac") == 0 || strcmp(name, "dfrac") == 0 || strcmp(name, "tfrac") == 0) {
            f32 cs = sub_scale(scale);
            MBox *num = parse_unit(p, cs, depth + 1);
            MBox *den = parse_unit(p, cs, depth + 1);
            return mb_frac(num, den, scale);
        }
        if (strcmp(name, "sqrt") == 0) {
            /* optional [index] — parsed and dropped in v1 */
            while (p->i < p->n && (p->s[p->i] == ' ')) p->i++;
            if (p->i < p->n && p->s[p->i] == '[') {
                p->i++;
                while (p->i < p->n && p->s[p->i] != ']') p->i++;
                if (p->i < p->n) p->i++;
            }
            MBox *rad = parse_unit(p, scale, depth + 1);
            return mb_sqrt(rad, scale);
        }
        if (strcmp(name, "left") == 0 || strcmp(name, "right") == 0) {
            /* drop the \left/\right keyword; the following delimiter renders at
             * base size (no auto-sizing in v1). */
            return mb_kern(0);
        }
        if (strcmp(name, ",") == 0)  return mb_kern(0.17f * g_cw * scale);
        if (strcmp(name, ":") == 0)  return mb_kern(0.22f * g_cw * scale);
        if (strcmp(name, ";") == 0)  return mb_kern(0.28f * g_cw * scale);
        if (strcmp(name, "!") == 0)  return mb_kern(-0.12f * g_cw * scale);
        if (strcmp(name, " ") == 0)  return mb_kern(0.30f * g_cw * scale);
        if (strcmp(name, "quad") == 0)  return mb_kern(g_cw * scale);
        if (strcmp(name, "qquad") == 0) return mb_kern(2.0f * g_cw * scale);
        if (strcmp(name, "{") == 0)  return mb_glyph('{', scale);
        if (strcmp(name, "}") == 0)  return mb_glyph('}', scale);
        if (strcmp(name, "%") == 0)  return mb_glyph('%', scale);
        if (strcmp(name, "&") == 0)  return mb_glyph('&', scale);
        if (strcmp(name, "#") == 0)  return mb_glyph('#', scale);
        if (strcmp(name, "_") == 0)  return mb_glyph('_', scale);

        for (const MathSym *m = MATH_SYMS; m->name; m++)
            if (strcmp(name, m->name) == 0) return mb_glyph(m->cp, scale);
        for (const char **f = MATH_FUNCS; *f; f++)
            if (strcmp(name, *f) == 0) return letters_to_hlist(name, nl, scale);

        /* Unknown command: render its name literally so nothing vanishes. */
        return letters_to_hlist(name, nl, scale);
    }

    /* Single codepoint atom. */
    u32 cp = 0;
    u32 adv = utf8_decode(p->s + p->i, p->n - p->i, &cp);
    if (adv == 0) { p->i++; return mb_kern(0); }
    p->i += adv;
    return mb_glyph(cp, scale);
}

static MBox *parse_list(MP *p, f32 scale, bool grouped, int depth) {
    /* Depth cap: consume the rest of this group without building so the caller
     * still makes progress and we don't recurse further. */
    if (depth > MM_MAX_NEST) {
        if (grouped) mm_skip_group(p);
        else p->i = p->n;
        return mb_kern(0);
    }
    MBox *kids[256]; u32 nk = 0;
    while (p->i < p->n && nk < 255) {
        u8 c = p->s[p->i];
        if (grouped && c == '}') { p->i++; break; }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { p->i++; continue; }

        if (c == '^' || c == '_') {
            /* stray script with no base — attach to an empty box */
            u8 which = c; p->i++;
            f32 cs = sub_scale(scale);
            MBox *arg = parse_unit(p, cs, depth + 1);
            MBox *base = (nk > 0) ? kids[nk - 1] : mb_kern(0);
            MBox *sup = NULL, *sub = NULL;
            if (base && base->kind == MB_SCRIPT) {
                sup = base->kids[1]; sub = base->kids[2]; base = base->kids[0];
            }
            if (which == '^') sup = arg; else sub = arg;
            MBox *sc = mb_script(base, sup, sub);
            /* Guard against scratch exhaustion: never store a NULL box. When
             * sc is NULL keep the existing base in kids[] (don't clobber it
             * with NULL), symmetric with the unit-attach path below. */
            if (sc) { if (nk > 0) kids[nk - 1] = sc; else kids[nk++] = sc; }
            continue;
        }

        MBox *u = parse_unit(p, scale, depth + 1);
        if (!u) { if (grouped) continue; else break; }
        /* attach immediately-following scripts to this unit */
        while (p->i < p->n && (p->s[p->i] == '^' || p->s[p->i] == '_')) {
            u8 which = p->s[p->i]; p->i++;
            f32 cs = sub_scale(scale);
            MBox *arg = parse_unit(p, cs, depth + 1);
            MBox *sup = NULL, *sub = NULL;
            if (u && u->kind == MB_SCRIPT) { sup = u->kids[1]; sub = u->kids[2]; u = u->kids[0]; }
            if (which == '^') sup = arg; else sub = arg;
            u = mb_script(u, sup, sub);
            if (!u) break;   /* scratch exhausted — stop attaching, don't deref NULL */
        }
        if (u && nk < 255) kids[nk++] = u;
    }
    if (nk == 0) return mb_kern(0);
    return mb_hlist(kids, nk);
}

/* ---- glyph op buffer (bucketed by scale so set_ui_scale flips are minimal) -- */
typedef struct { u32 cp; f32 x, y, scale; } GOp;
#define MM_MAX_OPS 2048
static GOp  g_ops[MM_MAX_OPS];
static u32  g_nops;
static Renderer *g_r;
static Color g_fg;

static void emit_glyph(u32 cp, f32 x, f32 baseline_y, f32 scale) {
    if (g_nops >= MM_MAX_OPS) return;
    g_ops[g_nops++] = (GOp){ cp, x, baseline_y - GA(scale), scale };  /* y = cell top */
}

/* Walk the box tree, collecting glyph ops and drawing rules immediately. */
static void draw_box(MBox *b, f32 x, f32 baseline_y) {
    if (!b) return;
    switch (b->kind) {
    case MB_KERN: break;
    case MB_GLYPH:
        emit_glyph(b->cp, x, baseline_y, b->scale);
        break;
    case MB_HLIST: {
        f32 cx = x;
        for (u32 i = 0; i < b->nkids; i++) {
            if (!b->kids[i]) continue;
            draw_box(b->kids[i], cx, baseline_y);
            cx += b->kids[i]->w;
        }
        break;
    }
    case MB_SCRIPT: {
        MBox *base = b->kids[0], *sup = b->kids[1], *sub = b->kids[2];
        f32 bw = base ? base->w : 0;
        if (base) draw_box(base, x, baseline_y);
        if (sup)  draw_box(sup, x + bw, baseline_y - b->a);   /* a = sup_shift */
        if (sub)  draw_box(sub, x + bw, baseline_y + b->b);   /* b = sub_shift */
        break;
    }
    case MB_FRAC: {
        MBox *num = b->kids[0], *den = b->kids[1];
        f32 axis = b->a, thick = b->b;
        f32 gap  = 0.18f * g_ch * b->scale;       /* matches mb_frac layout */
        f32 bar_y = baseline_y - axis;
        if (g_r) renderer_draw_rect(g_r, x, bar_y - thick * 0.5f, b->w, thick, g_fg);
        if (num) {
            f32 nx = x + (b->w - num->w) * 0.5f;
            f32 nbl = bar_y - thick * 0.5f - gap - num->d;
            draw_box(num, nx, nbl);
        }
        if (den) {
            f32 dx = x + (b->w - den->w) * 0.5f;
            f32 dbl = bar_y + thick * 0.5f + gap + den->h;
            draw_box(den, dx, dbl);
        }
        break;
    }
    case MB_SQRT: {
        MBox *rad = b->kids[0];
        f32 surd_w = b->a, thick = b->b;
        /* radical glyph at the left, sized to the body */
        emit_glyph(0x221A, x, baseline_y, b->scale);
        f32 over_x = x + surd_w;
        f32 over_y = baseline_y - b->h + thick;          /* vinculum near the top */
        f32 over_w = b->w - surd_w;
        if (g_r) renderer_draw_rect(g_r, over_x, over_y, over_w, thick, g_fg);
        if (rad) draw_box(rad, x + surd_w + 0.12f * g_cw * b->scale, baseline_y);
        break;
    }
    }
}

static MBox *build(const u8 *tex, u32 len, f32 cw, f32 ch, bool display) {
    mm_reset();
    g_cw = cw; g_ch = ch;
    MP p = { tex, len, 0 };
    MBox *root = parse_list(&p, display ? 1.0f : 1.0f, false, 0);
    return root;
}

void md_math_measure(const u8 *tex, u32 len, f32 cw, f32 ch, bool display,
                     f32 *out_w, f32 *out_ascent, f32 *out_descent) {
    f32 w = 0, a = GA(1.0f), d = GD(1.0f);
    if (tex && len) {
        MBox *root = build(tex, len, cw, ch, display);
        if (root) { w = root->w; a = root->h; d = root->d; }
    }
    if (out_w) *out_w = w;
    if (out_ascent) *out_ascent = a;
    if (out_descent) *out_descent = d;
}

void md_math_render(Renderer *r, const u8 *tex, u32 len,
                    f32 x, f32 baseline_y, f32 cw, f32 ch, bool display, Color fg) {
    if (!r || !tex || !len) return;
    MBox *root = build(tex, len, cw, ch, display);
    if (!root) return;

    g_r = r; g_fg = fg; g_nops = 0;
    draw_box(root, x, baseline_y);

    /* Flush any pending rect (fraction bars / vinculums) before glyphs. */
    renderer_flush_rects(r);

    /* Warm + emit glyphs bucketed by scale to keep set_ui_scale flips minimal.
     * Pre-warm everything synchronously so the first frame has real glyphs. */
    for (u32 i = 0; i < g_nops; i++) {
        char buf[5]; u32 n = utf8_encode(g_ops[i].cp, (u8 *)buf); buf[n] = 0;
        font_warm_text_glyphs(&r->font, buf);
    }
    bool done[MM_MAX_OPS] = {0};
    for (u32 i = 0; i < g_nops; i++) {
        if (done[i]) continue;
        f32 s = g_ops[i].scale;
        renderer_set_ui_scale(r, cw * s, ch * s);
        for (u32 j = i; j < g_nops; j++) {
            if (done[j] || g_ops[j].scale != s) continue;
            renderer_push_glyph(r, g_ops[j].x, g_ops[j].y, g_ops[j].cp, fg);
            done[j] = true;
        }
        renderer_flush_glyphs(r);
    }
    renderer_set_ui_scale(r, cw, ch);   /* restore the caller's base scale */
    g_r = NULL;
}
