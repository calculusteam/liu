/* Liu — minimal native TeX-math engine. See md_math.h. */
#include "ui/markdown/md_math.h"
#include "core/checked.h"
#include "core/utf8.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- scratch bump allocator (reset per public call; bounded formulas) ----
 * Every constructor propagates a NULL out of mm_alloc, so a formula that
 * outgrows the arena truncates instead of corrupting anything. The size is set
 * by the grids \begin{…} admits: one cell is a box subtree of its own, so a
 * matrix at the row/col caps is what decides this number, not a scalar. */
#define MM_SCRATCH (256 * 1024)
/* Heap, not .bss, and allocated lazily: a session that never renders a formula
 * never pays the 256 KiB at all, and one that does pays it once and keeps it
 * for the rest of the process (the buffer is reset, not freed, per call — the
 * lifetime is unchanged, only the moment it starts).
 *
 * The allocation lives in mm_reset() rather than in mm_alloc() because
 * mm_avail() is consulted by parse_matrix *before* the first box is allocated
 * (a formula that opens with \begin{…} reaches the reserve check first), and an
 * mm_avail() of 0 there would truncate the grid. mm_reset() runs once per
 * public entry point, ahead of all parsing, so the buffer is in place before
 * anything asks how much room is left. mm_alloc() still fails closed on a NULL
 * buffer, which routes an allocation failure into the same truncate-the-formula
 * path that running the arena dry already takes. */
static u8   *g_mm_buf;
static usize g_mm_off;
static void  mm_reset(void) {
    g_mm_off = 0;
    if (!g_mm_buf) g_mm_buf = (u8 *)malloc(MM_SCRATCH);
}
static usize mm_avail(void) { return g_mm_buf ? MM_SCRATCH - g_mm_off : 0; }
static void *mm_alloc(usize n) {
    usize aligned, next;
    if (!g_mm_buf) return NULL;
    if (!checked_add_usize(n, 7u, &aligned)) return NULL;
    aligned &= ~(usize)7u;
    if (!checked_add_usize(g_mm_off, aligned, &next) || next > MM_SCRATCH)
        return NULL;
    void *p = &g_mm_buf[g_mm_off];
    g_mm_off = next;
    return p;
}

/* ---- box model ---- */
typedef enum {
    MB_GLYPH, MB_HLIST, MB_FRAC, MB_SCRIPT, MB_SQRT, MB_KERN,
    MB_FENCE,    /* \left…\right — content between two auto-sized delimiters */
    MB_ACCENT,   /* \hat \bar \overline … — a mark (or rule) over a nucleus */
    MB_LIMITS,   /* big operator with its scripts stacked above/below         */
    MB_MATRIX    /* \begin{…} grid; its delimiters come from a wrapping FENCE */
} MBKind;

/* Row/column caps. Overflowing either truncates the grid: cells past the cap
 * are scanned (so the parser stays in sync with \\ and \end) but not built.
 * MM_GRID_RESERVE is the arena headroom a grid stops accepting cells at. It is
 * only the first half of the guarantee: the check happens *between* cells, and
 * one cell can allocate far more than the reserve on its own, so it cannot by
 * itself promise the geometry will still fit. grid_rows_that_fit() closes that
 * gap by dropping trailing rows until the layout provably fits what is left —
 * running the arena dry mid-grid must cost the grid's tail, never the whole
 * formula (a NULL here propagates out of parse_list and truncates everything). */
#define MM_MAX_ROWS 64
#define MM_MAX_COLS 32
#define MM_GRID_RESERVE (24 * 1024)
/* Caps the cell scan of a pathological soup of separators. Every iteration of
 * the grid loop consumes at least one byte, so this bounds the work only. */
#define MM_MAX_CELL_SCAN 4096

/* Grid geometry for MB_MATRIX, hung off MBox.aux. kids[] is row-major
 * rows*cols and holds NULL for the ragged tail of a short row. */
typedef struct {
    f32 *colw;          /* cols entries — column widths                      */
    f32 *rowa, *rowd;   /* rows entries — per-row ascent / descent           */
    f32  hgap, vgap;
    u16  rows, cols;
    u8   left_align;
} MMat;

typedef struct MBox {
    struct MBox **kids;
    void *aux;         /* MB_MATRIX: MMat */
    f32 w, h, d;       /* width, ascent (above baseline), descent (below) */
    f32 scale;         /* glyph scale (MB_GLYPH) / ambient scale (composites) */
    f32 a, b, c;       /* per-kind geometry — documented at each constructor */
    u32 cp;            /* MB_GLYPH codepoint / MB_ACCENT mark / FENCE left */
    u32 cp2;           /* MB_FENCE right delimiter */
    u32 nkids;
    u8  kind;
    u8  limits;        /* nucleus wants stacked limits in display style */
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
    if (!kids || n == 0) return mb_kern(0);
    if (n == 1) return kids[0];
    MBox *b = mb_new(MB_HLIST);
    if (!b) return NULL;
    usize bytes;
    if (!checked_mul_usize((usize)n, sizeof(MBox *), &bytes)) return NULL;
    MBox **arr = mm_alloc(bytes);
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

/* Big operator with its scripts stacked over and under it (display style).
 * a = the gap between the operator and each script. */
static MBox *mb_limits(MBox *op, MBox *sup, MBox *sub, f32 scale) {
    MBox *b = mb_new(MB_LIMITS);
    if (!b) return NULL;
    MBox **arr = mm_alloc(3 * sizeof(MBox *));
    if (!arr) return NULL;
    arr[0] = op; arr[1] = sup; arr[2] = sub;
    b->kids = arr; b->nkids = 3;
    f32 gap = 0.14f * g_ch * scale;
    f32 w = op ? op->w : 0;
    if (sup && sup->w > w) w = sup->w;
    if (sub && sub->w > w) w = sub->w;
    b->w = w;
    b->h = (op ? op->h : GA(scale)) + (sup ? gap + sup->h + sup->d : 0);
    b->d = (op ? op->d : GD(scale)) + (sub ? gap + sub->h + sub->d : 0);
    b->a = gap; b->scale = scale;
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

/* Render vertical bars as rules because scaled glyphs have uneven thickness
 * and the available math font has no extensible-glyph recipes. Brackets,
 * parentheses, and braces have no equivalent rule, so render them as enlarged
 * glyphs. */
static inline bool delim_is_bar(u32 cp) {
    return cp == '|' || cp == 0x2016 || cp == 0x2225 || cp == 0x2223;
}

/* Horizontal slot a delimiter occupies at draw scale `ds`. 0 for the invisible
 * "." form, which still groups and still drives the sizing. */
static f32 delim_width(u32 cp, f32 ds) {
    if (cp == 0) return 0;
    if (delim_is_bar(cp)) return (cp == 0x2016 ? 0.62f : 0.40f) * g_cw * ds;
    return g_cw * ds;
}

/* Largest a delimiter may grow. Uniform scaling widens as it heightens, so an
 * unbounded fence would push the formula off the measure column. */
#define MM_MAX_DELIM_SCALE 3.0f

/* \left…\right. a = axis, c = delimiter draw scale, cp/cp2 = the two glyphs. */
static MBox *mb_fence(MBox *body, u32 lcp, u32 rcp, f32 scale) {
    MBox *b = mb_new(MB_FENCE);
    if (!b) return NULL;
    MBox **arr = mm_alloc(sizeof(MBox *));
    if (!arr) return NULL;
    arr[0] = body; b->kids = arr; b->nkids = 1;
    f32 axis = AXIS(scale);
    f32 bh = body ? body->h : GA(scale);
    f32 bd = body ? body->d : GD(scale);
    /* Size from the larger half about the axis so a lopsided body still gets a
     * fence centred on it, the way TeX picks a delimiter variant. */
    f32 half = fmaxf(bh - axis, bd + axis);
    f32 need = 2.0f * half + 0.12f * g_ch * scale;
    f32 ds = need / g_ch;              /* a glyph box is exactly g_ch*ds tall */
    if (ds < scale) ds = scale;
    if (ds > MM_MAX_DELIM_SCALE) ds = MM_MAX_DELIM_SCALE;
    b->cp = lcp; b->cp2 = rcp; b->a = axis; b->c = ds; b->scale = scale;
    f32 half_glyph = 0.5f * g_ch * ds;
    b->w = delim_width(lcp, ds) + (body ? body->w : 0) + delim_width(rcp, ds);
    b->h = fmaxf(bh, axis + half_glyph);
    b->d = fmaxf(bd, half_glyph - axis);
    if (b->d < 0) b->d = 0;
    return b;
}

/* Accent over a nucleus. cp == 0 means \overline: a rule, not a glyph.
 * a = gap over the nucleus, b = ink height, c = the mark's draw scale. */
static MBox *mb_accent(MBox *nuc, u32 cp, f32 rel, f32 scale) {
    MBox *b = mb_new(MB_ACCENT);
    if (!b) return NULL;
    MBox **arr = mm_alloc(sizeof(MBox *));
    if (!arr) return NULL;
    arr[0] = nuc; b->kids = arr; b->nkids = 1;
    f32 as  = scale * rel;
    f32 gap = 0.06f * g_ch * scale;
    /* Only the mark's *ink* counts towards the ascent. A modifier letter sits
     * in the top third of its box, and charging the whole box would double the
     * height of every accented atom and break the surrounding baseline. */
    f32 ink = (cp == 0) ? fmaxf(1.0f, g_ch * scale * 0.045f) : 0.30f * g_ch * as;
    b->w = nuc ? nuc->w : g_cw * scale;
    b->h = (nuc ? nuc->h : GA(scale)) + gap + ink;
    b->d = nuc ? nuc->d : GD(scale);
    b->cp = cp; b->a = gap; b->b = ink; b->c = as; b->scale = scale;
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
/* Upright names that carry their scripts underneath in display style, the way
 * \lim_{x \to 0} is set. Everything else in MATH_FUNCS takes side scripts. */
static const char *MATH_FUNC_LIMITS[] = {
    "lim","limsup","liminf","max","min","sup","inf","det","gcd","Pr",NULL
};
/* Big operators. `limits` says the scripts stack in display style — the
 * integrals keep theirs at the side, which is the standard TeX convention. */
typedef struct { const char *name; u32 cp; u8 limits; } MathBigOp;
static const MathBigOp MATH_BIGOPS[] = {
    {"sum",0x2211,1},{"prod",0x220F,1},{"coprod",0x2210,1},
    {"bigcup",0x22C3,1},{"bigcap",0x22C2,1},{"bigvee",0x22C1,1},{"bigwedge",0x22C0,1},
    {"bigoplus",0x2A01,1},{"bigotimes",0x2A02,1},{"bigodot",0x2A00,1},{"biguplus",0x2A04,1},
    {"int",0x222B,0},{"oint",0x222E,0},{"iint",0x222C,0},{"iiint",0x222D,0},
    {NULL,0,0}
};
/* Accents. `rel` is the mark's size relative to the nucleus; the "wide"
 * variants only draw bigger, since a glyph cannot be stretched horizontally
 * without font-level variants. cp 0 == draw a rule (\overline). */
typedef struct { const char *name; u32 cp; f32 rel; } MathAccent;
static const MathAccent MATH_ACCENTS[] = {
    {"hat",0x2C6,0.75f},{"widehat",0x2C6,1.00f},{"check",0x2C7,0.75f},
    {"tilde",0x2DC,0.75f},{"widetilde",0x2DC,1.00f},{"bar",0xAF,0.75f},
    {"dot",0x2D9,0.75f},{"ddot",0xA8,0.75f},{"breve",0x2D8,0.75f},
    {"acute",0xB4,0.75f},{"grave",0x60,0.75f},{"mathring",0x2DA,0.75f},
    /* A combining arrow has no advance in the atlas, so \vec draws the plain
     * arrow small enough to read as a mark. */
    {"vec",0x2192,0.60f},
    {"overline",0,1.00f},
    {NULL,0,0}
};
/* Grid environments. Unknown names fall back to the first entry so a formula
 * in an environment not modelled here still shows its cells. */
typedef struct { const char *name; u32 left, right; u8 left_align, small; } MatrixEnv;
static const MatrixEnv MATRIX_ENVS[] = {
    {"matrix",0,0,0,0},
    {"pmatrix",'(',')',0,0},
    {"bmatrix",'[',']',0,0},
    {"Bmatrix",'{','}',0,0},
    {"vmatrix",'|','|',0,0},
    {"Vmatrix",0x2016,0x2016,0,0},
    {"smallmatrix",0,0,0,1},
    {"cases",'{',0,1,0},          /* brace on the left only, columns flush left */
    {"array",0,0,0,0},
    {"aligned",0,0,1,0},{"align",0,0,1,0},{"align*",0,0,1,0},{"split",0,0,1,0},
    {"gathered",0,0,0,0},{"gather",0,0,0,0},{"gather*",0,0,0,0},
    {NULL,0,0,0,0}
};

/* ---- parser ---- */
typedef struct { const u8 *s; u32 n; u32 i; bool display; } MP;

/* Recursion cap for the math parser. Each level costs a parse_list stack frame
 * (which holds a 256-pointer kids[] array), so cap well below what would blow
 * the C stack on pathological deeply-nested input ({{{...}}}, \frac\frac…).
 * \left and \begin descend through the same counter, so it bounds fence and
 * environment nesting too. */
#define MM_MAX_NEST 64

/* Stop conditions for parse_list. */
#define MM_STOP_BRACE 0x1u   /* '}' ends the list and is consumed        */
#define MM_STOP_CELL  0x2u   /* '&', '\\' and \end end it (not consumed) */
#define MM_STOP_RIGHT 0x4u   /* \right ends it (not consumed)            */

static MBox *parse_list(MP *p, f32 scale, u32 stop, int depth);

/* Consume a brace-balanced group body (p->i is just past the opening '{'),
 * used to skip without recursing when the depth cap is hit. */
static void mm_skip_group(MP *p) {
    u32 b = 1;                     /* u32, not int: the depth is bounded by the
                                    * u32 input length, so a signed counter is
                                    * one 2 GB brace run away from overflow UB */
    while (p->i < p->n && b > 0) {
        u8 c = p->s[p->i++];
        if (c == '{') { if (b < 0xFFFFFFFFu) b++; }
        else if (c == '}') b--;
    }
}

/* ASCII alpha test (no <ctype.h> locale surprises). */
static inline int isalpha_ascii(u8 c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* True when p->i points at "\name" and no further letter follows it, so
 * \end matches \end{…} but not \endash. */
static bool at_cmd(const MP *p, const char *name) {
    usize len = strlen(name);
    if (p->i >= p->n || p->s[p->i] != '\\') return false;
    if ((usize)(p->n - p->i) < len + 1) return false;
    if (memcmp(p->s + p->i + 1, name, len) != 0) return false;
    usize j = (usize)p->i + 1 + len;
    return j >= (usize)p->n || !isalpha_ascii(p->s[j]);
}

/* Step past "\name" once at_cmd() has confirmed it is there. */
static void skip_cmd(MP *p, const char *name) {
    p->i += (u32)strlen(name) + 1u;
}

static u32 read_cmd_name(MP *p, char *out, u32 cap) {
    if (!p || !out || cap == 0) return 0;
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

/* Read the "{name}" argument of \begin / \end. Empty when it is missing. */
static u32 read_env_name(MP *p, char *out, u32 cap) {
    u32 k = 0;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t')) p->i++;
    if (p->i >= p->n || p->s[p->i] != '{') return 0;
    p->i++;
    while (p->i < p->n && p->s[p->i] != '}') {
        if (k + 1 < cap) out[k++] = (char)p->s[p->i];
        p->i++;
    }
    if (p->i < p->n) p->i++;   /* the '}' */
    out[k] = 0;
    return k;
}

/* Skip an optional "{…}" argument (the column spec of \begin{array}). */
static void skip_opt_group(MP *p) {
    u32 save = p->i;
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t')) p->i++;
    if (p->i < p->n && p->s[p->i] == '{') { p->i++; mm_skip_group(p); return; }
    p->i = save;
}

/* Skip the optional "[…]" spacing argument of a "\\" row break. */
static void skip_opt_bracket(MP *p) {
    u32 save = p->i;
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t')) p->i++;
    if (p->i < p->n && p->s[p->i] == '[') {
        u32 j = p->i + 1, guard = 0;
        while (j < p->n && p->s[j] != ']' && guard++ < 64) j++;
        if (j < p->n && p->s[j] == ']') { p->i = j + 1; return; }
    }
    p->i = save;
}

/* Delimiter after \left / \right / \middle. 0 means "draw nothing": the
 * invisible "." form, and anything not recognised, which still groups. */
static u32 read_delim(MP *p) {
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t')) p->i++;
    if (p->i >= p->n) return 0;
    if (p->s[p->i] == '\\') {
        p->i++;
        char nm[32];
        if (read_cmd_name(p, nm, sizeof nm) == 0) return 0;
        if (strcmp(nm, "{") == 0) return '{';
        if (strcmp(nm, "}") == 0) return '}';
        if (strcmp(nm, "|") == 0) return 0x2016;
        for (const MathSym *m = MATH_SYMS; m->name; m++)
            if (strcmp(nm, m->name) == 0) return m->cp;
        return 0;
    }
    u32 cp = 0;
    u32 adv = utf8_decode(p->s + p->i, p->n - p->i, &cp);
    p->i += adv ? adv : 1;
    if (cp == '.') return 0;
    if (cp == '<') return 0x27E8;
    if (cp == '>') return 0x27E9;
    return cp;
}

/* True when the token at p->i terminates the list being parsed. */
static bool list_stops(const MP *p, u32 stop) {
    if (p->i >= p->n) return true;
    u8 c = p->s[p->i];
    if ((stop & MM_STOP_CELL) && c == '&') return true;
    if (c != '\\') return false;
    if ((stop & MM_STOP_CELL) &&
        (at_cmd(p, "end") || (p->i + 1 < p->n && p->s[p->i + 1] == '\\')))
        return true;
    if ((stop & MM_STOP_RIGHT) && at_cmd(p, "right")) return true;
    return false;
}

/* Consume one cell's bytes without building any box, for the cells a full grid
 * drops. Braces hide the separators inside them, exactly as in parse_list. */
static void mm_skip_cell(MP *p) {
    u32 brace = 0;
    while (p->i < p->n) {
        u8 c = p->s[p->i];
        if (c == '{') { brace++; p->i++; continue; }
        if (c == '}') { if (brace) brace--; p->i++; continue; }
        if (brace == 0 && list_stops(p, MM_STOP_CELL)) return;
        p->i++;
    }
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

/* Row accumulator for parse_matrix: one row of cells at a time, copied into
 * the arena as it closes, so a grid costs ~1 KB of stack whatever its size. */
typedef struct {
    MBox **rowv[MM_MAX_ROWS];
    u16    rowlen[MM_MAX_ROWS];
    MBox  *cur[MM_MAX_COLS];
    u16    rows, ncur;
} MGrid;

static void grid_end_row(MGrid *g) {
    usize bytes;
    if (g->ncur > 0 && g->rows < MM_MAX_ROWS &&
        checked_mul_usize((usize)g->ncur, sizeof(MBox *), &bytes)) {
        MBox **row = mm_alloc(bytes);
        if (row) {
            memcpy(row, g->cur, bytes);
            g->rowv[g->rows] = row;
            g->rowlen[g->rows] = g->ncur;
            g->rows++;
        }
    }
    g->ncur = 0;
}

/* Arena bytes mb_matrix() will ask for, over its six blocks. mm_alloc rounds
 * each request up to 8, so charge that rounding to every one of them. */
static usize grid_bytes(u16 rows, u16 cols) {
    return sizeof(MBox) + sizeof(MMat)
         + (usize)rows * (usize)cols * sizeof(MBox *)
         + (usize)cols * sizeof(f32)
         + 2u * (usize)rows * sizeof(f32)
         + 6u * 8u;
}

/* How many of the leading `rows` the arena can still lay out. A grid whose
 * cells drank the reserve dry keeps the rows that fit rather than returning
 * NULL and taking the surrounding formula down with it. */
static u16 grid_rows_that_fit(u16 rows, u16 cols) {
    usize avail = mm_avail();
    while (rows > 0 && grid_bytes(rows, cols) > avail) rows--;
    return rows;
}

/* Lay the parsed rows out: columns get the width of their widest cell, rows
 * share one baseline, and the whole grid is centred on the math axis. */
static MBox *mb_matrix(const MGrid *g, u16 rows, u16 cols, f32 scale, u8 left_align) {
    usize cells, bytes;
    if (rows > g->rows) rows = g->rows;
    if (!checked_mul_usize((usize)rows, (usize)cols, &cells)) return NULL;
    if (!checked_mul_usize(cells, sizeof(MBox *), &bytes)) return NULL;
    MBox *b = mb_new(MB_MATRIX);
    MBox **kids = mm_alloc(bytes);
    MMat *m     = mm_alloc(sizeof *m);
    f32 *colw   = mm_alloc((usize)cols * sizeof(f32));
    f32 *rowa   = mm_alloc((usize)rows * sizeof(f32));
    f32 *rowd   = mm_alloc((usize)rows * sizeof(f32));
    if (!b || !kids || !m || !colw || !rowa || !rowd) return NULL;
    memset(kids, 0, bytes);
    memset(m, 0, sizeof *m);

    for (u16 j = 0; j < cols; j++) colw[j] = 0;
    for (u16 i = 0; i < rows; i++) {
        rowa[i] = GA(scale); rowd[i] = GD(scale);
        for (u16 j = 0; j < g->rowlen[i] && j < cols; j++) {
            MBox *cell = g->rowv[i][j];
            kids[(usize)i * cols + j] = cell;
            if (!cell) continue;
            if (cell->w > colw[j]) colw[j] = cell->w;
            if (cell->h > rowa[i]) rowa[i] = cell->h;
            if (cell->d > rowd[i]) rowd[i] = cell->d;
        }
    }
    /* Gaps come off the cell metrics so they track the reader's font size.
     * Left-aligned grids (cases, aligned) need the wider column gutter: the
     * second column is a condition, not a neighbouring entry. */
    m->hgap = (left_align ? 1.20f : 0.85f) * g_cw * scale;
    m->vgap = 0.30f * g_ch * scale;
    m->colw = colw; m->rowa = rowa; m->rowd = rowd;
    m->rows = rows; m->cols = cols; m->left_align = left_align;

    f32 w = 0, total = 0;
    for (u16 j = 0; j < cols; j++) w += colw[j];
    w += m->hgap * (f32)(cols - 1);
    for (u16 i = 0; i < rows; i++) total += rowa[i] + rowd[i];
    total += m->vgap * (f32)(rows - 1);

    f32 axis = AXIS(scale);
    b->kids = kids; b->nkids = (u32)cells; b->aux = m;
    b->w = w;
    b->h = total * 0.5f + axis;
    b->d = total * 0.5f - axis;
    if (b->d < 0) b->d = 0;
    b->a = axis; b->scale = scale;
    return b;
}

/* \begin{env} … \end{env}. Cells split on '&', rows on '\\'. Terminates at
 * \end whatever name it carries — a mismatched \end closing the wrong
 * environment still ends this one — and at end of input if it never arrives. */
static MBox *parse_matrix(MP *p, f32 scale, int depth, const MatrixEnv *env) {
    MGrid g;
    memset(&g, 0, sizeof g);
    f32 cs = env->small ? sub_scale(scale) : scale;
    u32 guard = 0;
    bool closed = false;

    while (p->i < p->n && guard++ < MM_MAX_CELL_SCAN) {
        u32 before = p->i;
        if (g.rows >= MM_MAX_ROWS || g.ncur >= MM_MAX_COLS ||
            mm_avail() < MM_GRID_RESERVE) {
            mm_skip_cell(p);          /* truncated: scanned, never built */
        } else {
            g.cur[g.ncur++] = parse_list(p, cs, MM_STOP_CELL, depth + 1);
        }

        bool done = false;
        if (p->i >= p->n) {
            done = true;
        } else if (p->s[p->i] == '&') {
            p->i++;
        } else if (p->s[p->i] == '\\' && p->i + 1 < p->n && p->s[p->i + 1] == '\\') {
            p->i += 2;
            skip_opt_bracket(p);
            grid_end_row(&g);
        } else if (at_cmd(p, "end")) {
            char tmp[32];
            skip_cmd(p, "end");
            read_env_name(p, tmp, sizeof tmp);
            closed = done = true;
        } else if (p->i == before) {
            p->i++;    /* parse_list made no progress (depth cap) — force it */
        }
        if (done) break;
    }
    /* The scan guard tripped before \end arrived: swallow the rest of the
     * environment so its raw cells cannot spill into the surrounding formula
     * as literal text. A missing \end simply lands on end of input. */
    if (!closed) {
        while (p->i < p->n && !at_cmd(p, "end")) p->i++;
        if (p->i < p->n) {
            char tmp[32];
            skip_cmd(p, "end");
            read_env_name(p, tmp, sizeof tmp);
        }
    }
    /* A trailing "\\" before \end opens a row TeX ignores; so does a grid that
     * never got any content at all. */
    grid_end_row(&g);
    if (g.rows > 0 && g.rowlen[g.rows - 1] == 1) {
        MBox *last = g.rowv[g.rows - 1][0];
        if (!last || last->w == 0) g.rows--;
    }
    u16 cols = 0;
    for (u16 i = 0; i < g.rows; i++)
        if (g.rowlen[i] > cols) cols = g.rowlen[i];
    if (g.rows == 0 || cols == 0) return mb_kern(0);
    /* Drop trailing rows until the layout fits what the cells left behind, so
     * a grid that outran MM_GRID_RESERVE renders short instead of returning
     * NULL and truncating the whole formula at the \begin. */
    u16 rows = grid_rows_that_fit(g.rows, cols);
    if (rows == 0) return mb_kern(0);
    if (rows < g.rows) {           /* fewer rows can mean fewer columns */
        cols = 0;
        for (u16 i = 0; i < rows; i++)
            if (g.rowlen[i] > cols) cols = g.rowlen[i];
        if (cols == 0) return mb_kern(0);
    }
    return mb_matrix(&g, rows, cols, cs, env->left_align);
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
        /* Braces hide alignment from the enclosing environment, as in TeX, so
         * the group starts from a clean stop set. */
        p->i++;
        return parse_list(p, scale, MM_STOP_BRACE, depth + 1);
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
        if (strcmp(name, "left") == 0) {
            u32 lcp = read_delim(p);
            MBox *body = parse_list(p, scale, MM_STOP_RIGHT, depth + 1);
            u32 rcp = 0;
            if (at_cmd(p, "right")) { skip_cmd(p, "right"); rcp = read_delim(p); }
            /* An unmatched \left just closes at end of input: the body is still
             * laid out and drawn, so nothing after it is dropped. */
            MBox *f = mb_fence(body, lcp, rcp, scale);
            return f ? f : body;
        }
        if (strcmp(name, "right") == 0 || strcmp(name, "middle") == 0) {
            /* Unpaired here (a matched \right is consumed by \left above);
             * keep the delimiter itself at base size. */
            u32 cp = read_delim(p);
            return cp ? mb_glyph(cp, scale) : mb_kern(0);
        }
        if (strcmp(name, "begin") == 0) {
            char env[32];
            read_env_name(p, env, sizeof env);
            const MatrixEnv *me = MATRIX_ENVS;
            for (const MatrixEnv *e = MATRIX_ENVS; e->name; e++)
                if (strcmp(env, e->name) == 0) { me = e; break; }
            if (strcmp(env, "array") == 0) skip_opt_group(p);   /* column spec */
            MBox *grid = parse_matrix(p, scale, depth + 1, me);
            if (!me->left && !me->right) return grid;
            MBox *f = mb_fence(grid, me->left, me->right, scale);
            return f ? f : grid;
        }
        if (strcmp(name, "end") == 0) {
            /* A stray \end with no \begin: swallow its argument so the
             * environment name does not print as literal text. */
            char env[32];
            read_env_name(p, env, sizeof env);
            return mb_kern(0);
        }
        for (const MathAccent *a = MATH_ACCENTS; a->name; a++) {
            if (strcmp(name, a->name) != 0) continue;
            MBox *nuc = parse_unit(p, scale, depth + 1);
            MBox *ac = mb_accent(nuc, a->cp, a->rel, scale);
            return ac ? ac : nuc;
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

        /* Big operators before the symbol table: display style draws them
         * larger and (except for the integrals) stacks their limits. */
        for (const MathBigOp *o = MATH_BIGOPS; o->name; o++) {
            if (strcmp(name, o->name) != 0) continue;
            MBox *g = mb_glyph(o->cp, p->display ? scale * 1.60f : scale);
            if (g) g->limits = o->limits;   /* attach_script gates on display */
            return g;
        }
        for (const MathSym *m = MATH_SYMS; m->name; m++)
            if (strcmp(name, m->name) == 0) return mb_glyph(m->cp, scale);
        for (const char **f = MATH_FUNCS; *f; f++) {
            if (strcmp(name, *f) != 0) continue;
            MBox *h = letters_to_hlist(name, nl, scale);
            if (h)
                for (const char **l = MATH_FUNC_LIMITS; *l; l++)
                    if (strcmp(name, *l) == 0) { h->limits = 1; break; }
            return h;
        }

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

/* Attach one script to `base`, re-opening an existing script/limits box so
 * x_i^2 keeps both parts on the same nucleus. A big operator in display style
 * takes its scripts as limits (above/below) instead of at the side. */
static MBox *attach_script(MP *p, MBox *base, u8 which, MBox *arg, f32 scale) {
    MBox *sup = NULL, *sub = NULL;
    if (base && (base->kind == MB_SCRIPT || base->kind == MB_LIMITS)) {
        sup = base->kids[1]; sub = base->kids[2]; base = base->kids[0];
    }
    if (which == '^') sup = arg; else sub = arg;
    if (base && base->limits && p->display) return mb_limits(base, sup, sub, scale);
    return mb_script(base, sup, sub);
}

static MBox *parse_list(MP *p, f32 scale, u32 stop, int depth) {
    /* Depth cap: consume the rest of this list without building so the caller
     * still makes progress and nothing recurses further. A cell/fence list is
     * left where it is — its owner forces progress and re-syncs on the next
     * separator, which keeps the surrounding environment intact. */
    if (depth > MM_MAX_NEST) {
        if (stop & MM_STOP_BRACE) mm_skip_group(p);
        else if (stop == 0) p->i = p->n;
        return mb_kern(0);
    }
    MBox *kids[256]; u32 nk = 0;
    while (p->i < p->n && nk < 255) {
        u8 c = p->s[p->i];
        if ((stop & MM_STOP_BRACE) && c == '}') { p->i++; break; }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { p->i++; continue; }
        if (stop && list_stops(p, stop)) break;

        if (c == '^' || c == '_') {
            /* stray script with no base — attach to an empty box */
            u8 which = c; p->i++;
            f32 cs = sub_scale(scale);
            MBox *arg = parse_unit(p, cs, depth + 1);
            MBox *base = (nk > 0) ? kids[nk - 1] : mb_kern(0);
            MBox *sc = attach_script(p, base, which, arg, scale);
            /* Guard against scratch exhaustion: never store a NULL box. When
             * sc is NULL keep the existing base in kids[] (don't clobber it
             * with NULL), symmetric with the unit-attach path below. */
            if (sc) { if (nk > 0) kids[nk - 1] = sc; else kids[nk++] = sc; }
            continue;
        }

        MBox *u = parse_unit(p, scale, depth + 1);
        if (!u) {
            if (stop & MM_STOP_BRACE) continue;
            /* parse_unit declines without consuming only on '}' (end of input
             * is the loop guard). A cell/fence list must leave that brace for
             * whoever owns the group, but the top level owns nothing: drop it
             * the way TeX's "Too many }'s" recovery does and keep going, since
             * breaking here silently discards the entire rest of the formula
             * ("}x" would measure empty). Any other NULL is arena exhaustion,
             * which still truncates. */
            if (stop == 0 && p->s[p->i] == '}') { p->i++; continue; }
            break;
        }
        /* attach immediately-following scripts to this unit */
        while (p->i < p->n && (p->s[p->i] == '^' || p->s[p->i] == '_')) {
            u8 which = p->s[p->i]; p->i++;
            f32 cs = sub_scale(scale);
            MBox *arg = parse_unit(p, cs, depth + 1);
            u = attach_script(p, u, which, arg, scale);
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

/* One delimiter of a fence, centred vertically on `center_y`. */
static void draw_delim(u32 cp, f32 x, f32 center_y, f32 ds) {
    if (cp == 0) return;                       /* the invisible "." form */
    if (delim_is_bar(cp)) {
        f32 h = g_ch * ds * 0.86f;
        f32 t = fmaxf(1.0f, g_ch * ds * 0.035f);
        f32 w = delim_width(cp, ds);
        f32 y = center_y - h * 0.5f;
        if (!g_r) return;
        if (cp == 0x2016) {
            renderer_draw_rect(g_r, x + w * 0.30f - t * 0.5f, y, t, h, g_fg);
            renderer_draw_rect(g_r, x + w * 0.70f - t * 0.5f, y, t, h, g_fg);
        } else {
            renderer_draw_rect(g_r, x + (w - t) * 0.5f, y, t, h, g_fg);
        }
        return;
    }
    /* A glyph box spans [baseline-GA, baseline+GD], so its centre sits
     * 0.25*ch*ds above the baseline it is given. */
    emit_glyph(cp, x, center_y + 0.25f * g_ch * ds, ds);
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
    case MB_LIMITS: {
        MBox *op = b->kids[0], *sup = b->kids[1], *sub = b->kids[2];
        f32 gap = b->a;
        f32 oh = op ? op->h : GA(b->scale), od = op ? op->d : GD(b->scale);
        if (op)  draw_box(op,  x + (b->w - op->w)  * 0.5f, baseline_y);
        if (sup) draw_box(sup, x + (b->w - sup->w) * 0.5f,
                          baseline_y - oh - gap - sup->d);
        if (sub) draw_box(sub, x + (b->w - sub->w) * 0.5f,
                          baseline_y + od + gap + sub->h);
        break;
    }
    case MB_FENCE: {
        MBox *body = b->kids[0];
        f32 ds = b->c;
        f32 cy = baseline_y - b->a;              /* fence centre == math axis */
        f32 lw = delim_width(b->cp, ds);
        draw_delim(b->cp, x, cy, ds);
        if (body) draw_box(body, x + lw, baseline_y);
        draw_delim(b->cp2, x + lw + (body ? body->w : 0), cy, ds);
        break;
    }
    case MB_ACCENT: {
        MBox *nuc = b->kids[0];
        f32 top = baseline_y - b->h;             /* top of the accent's ink */
        if (b->cp == 0) {
            if (g_r) renderer_draw_rect(g_r, x, top, b->w, b->b, g_fg);
        } else {
            f32 aw = g_cw * b->c;
            /* The mark sits at the top of a modifier letter's box, so line
             * that box top up with the reserved ink band. */
            emit_glyph(b->cp, x + (b->w - aw) * 0.5f, top + GA(b->c), b->c);
        }
        if (nuc) draw_box(nuc, x + (b->w - nuc->w) * 0.5f, baseline_y);
        break;
    }
    case MB_MATRIX: {
        const MMat *m = b->aux;
        if (!m) break;
        f32 cy = baseline_y - b->h;              /* top edge of the grid */
        for (u16 i = 0; i < m->rows; i++) {
            f32 bl = cy + m->rowa[i];            /* every cell in a row shares it */
            f32 cx = x;
            for (u16 j = 0; j < m->cols; j++) {
                MBox *cell = b->kids[(usize)i * m->cols + j];
                if (cell) {
                    f32 off = m->left_align ? 0.0f : (m->colw[j] - cell->w) * 0.5f;
                    draw_box(cell, cx + off, bl);
                }
                cx += m->colw[j] + m->hgap;
            }
            cy += m->rowa[i] + m->rowd[i] + m->vgap;
        }
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
    if (!tex || len == 0 || !isfinite(cw) || !isfinite(ch) ||
        cw <= 0.0f || ch <= 0.0f || cw > 16384.0f || ch > 16384.0f)
        return NULL;
    mm_reset();
    g_cw = cw; g_ch = ch;
    MP p = { tex, len, 0, display };
    MBox *root = parse_list(&p, 1.0f, 0, 0);
    return root;
}

void md_math_measure(const u8 *tex, u32 len, f32 cw, f32 ch, bool display,
                     f32 *out_w, f32 *out_ascent, f32 *out_descent) {
    f32 safe_ch = isfinite(ch) && ch > 0.0f && ch <= 16384.0f ? ch : 0.0f;
    f32 w = 0, a = ASCENT_F * safe_ch, d = DESCENT_F * safe_ch;
    if (tex && len) {
        MBox *root = build(tex, len, cw, ch, display);
        if (root) { w = root->w; a = root->h; d = root->d; }
    }
    /* The block layout turns these into scroll extents, so malformed input has
     * to come back finite and non-negative however odd the box tree got. */
    if (!isfinite(w) || w < 0.0f) w = 0.0f;
    if (!isfinite(a) || a < 0.0f) a = 0.0f;
    if (!isfinite(d) || d < 0.0f) d = 0.0f;
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
