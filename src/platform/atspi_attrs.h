/*
 * Liu - the focused pane's text attributes in AT-SPI's terms (Linux screen readers)
 *
 * AT-SPI's Text interface answers GetAttributeRun / GetAttributes /
 * GetDefaultAttributes / GetAttributeValue with ATK's text attributes: a
 * string map and the character range the map holds over. A terminal's cells
 * carry a colour pair and a few flags, so this header is the one place a
 * cell's style (platform_a11y.h, the provider's cell styles) becomes those
 * maps, and the one place their runs are found.
 *
 * What a cell shows is decided the way the row builder draws it
 * (ui/terminal_row.h): inverse video swaps the pair and a bold cell is
 * brightened only for the eight base colours, never under inverse; a dim cell's
 * text is the colour times 0.797 (the builder's sRGB-space factor). So the
 * colours here are the colours ON SCREEN. The names and values are ATK's:
 *
 *   fg-color, bg-color   "R,G,B", each channel 0-255. ATK's documentation
 *                        says "%u,%u,%u" and gives no range; GTK writes
 *                        0-65535. Orca, the reader that names colours,
 *                        hands the numbers as they are to a 0-255 table
 *                        (colornames.rgbToName), so 16-bit values name the
 *                        wrong colour. ATSPI_ATTR_CHANNEL_MAX is that one
 *                        choice.
 *   weight               "700" for bold, "400" otherwise
 *   style                "italic" / "normal"
 *   underline            "single" / "double" / "error" (a curly one) / "none"
 *   strikethrough        "true" / "false"
 *   family-name, size    the face and its size in points (defaults only)
 *
 * A RUN is the characters around an offset whose map is the same; GetAttributes
 * and GetAttributeRun(false) answer only what differs from the pane's defaults,
 * GetAttributeRun(true) the full map. Offsets are the CHARACTERS atspi_text.h
 * counts: it fills the per-character style index while it converts the
 * provider's UTF-16 tables, and nothing here touches a unit or a column.
 *
 * Pure C: it needs no bus.
 */
#ifndef LIU_PLATFORM_ATSPI_ATTRS_H
#define LIU_PLATFORM_ATSPI_ATTRS_H

#include "core/types.h"
#include "platform/platform_a11y.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* The largest value of a colour channel in an fg-color / bg-color string. */
#define ATSPI_ATTR_CHANNEL_MAX 255u

enum { ATSPI_UL_NONE = 0, ATSPI_UL_SINGLE = 1, ATSPI_UL_DOUBLE = 2, ATSPI_UL_ERROR = 3 };

/* What one cell shows, as the attributes ATK has words for. */
typedef struct {
    u32 fg, bg;          /* 0x00RRGGBB as drawn */
    u8  bold, italic;
    u8  underline;       /* ATSPI_UL_* */
    u8  strike;
} AtspiAttrSet;

static inline bool atspi_attr_set_eq(const AtspiAttrSet *a, const AtspiAttrSet *b) {
    return a->fg == b->fg && a->bg == b->bg && a->bold == b->bold && a->italic == b->italic &&
           a->underline == b->underline && a->strike == b->strike;
}

/* ---- from a cell's style to what is drawn ------------------------------------ */

typedef struct {
    const u32 *palette;    /* 256 entries, 0x00RRGGBB */
    u32        def_fg, def_bg;
    bool       bold_bright;
} AtspiAttrCtx;

static inline u32 atspi_attr_color(const AtspiAttrCtx *c, u32 val, u32 fallback) {
    if (val & A11Y_COLOR_TRUE) return val & 0xFFFFFFu;
    if (val < 256u && c->palette) return c->palette[val];
    return fallback;
}

/* The row builder's dim: the text colour times 0.797, channel by channel. */
static inline u32 atspi_attr_dim(u32 rgb) {
    u32 r = (((rgb >> 16) & 0xFFu) * 797u + 500u) / 1000u;
    u32 g = (((rgb >> 8) & 0xFFu) * 797u + 500u) / 1000u;
    u32 b = ((rgb & 0xFFu) * 797u + 500u) / 1000u;
    return (r << 16) | (g << 8) | b;
}

static inline AtspiAttrSet atspi_attr_derive(const AtspiAttrCtx *c, const PlatformA11yStyle *s) {
    AtspiAttrSet a;
    memset(&a, 0, sizeof(a));
    bool inv = (s->flags & A11Y_STYLE_INVERSE) != 0;
    if (inv) {
        a.fg = atspi_attr_color(c, s->bg, c->def_bg);
        a.bg = atspi_attr_color(c, s->fg, c->def_fg);
    } else {
        a.fg = atspi_attr_color(c, s->fg, c->def_fg);
        a.bg = atspi_attr_color(c, s->bg, c->def_bg);
        if ((s->flags & A11Y_STYLE_BOLD) && c->bold_bright && c->palette &&
            !(s->fg & A11Y_COLOR_TRUE) && s->fg < 8u)
            a.fg = c->palette[s->fg + 8u];
    }
    if (s->flags & A11Y_STYLE_DIM) a.fg = atspi_attr_dim(a.fg);
    a.bold = (s->flags & A11Y_STYLE_BOLD) ? 1 : 0;
    a.italic = (s->flags & A11Y_STYLE_ITALIC) ? 1 : 0;
    a.strike = (s->flags & A11Y_STYLE_STRIKE) ? 1 : 0;
    switch (s->underline) {
    case 0:  a.underline = ATSPI_UL_NONE; break;
    case 2:  a.underline = ATSPI_UL_DOUBLE; break;
    case 3:  a.underline = ATSPI_UL_ERROR; break;        /* curly */
    default: a.underline = ATSPI_UL_SINGLE; break;       /* single, dotted, dashed */
    }
    return a;
}

/* ---- the key / value strings -------------------------------------------------- */

#define ATSPI_ATTR_MAX 10

typedef struct {
    char key[20];
    char val[96];
} AtspiAttrKV;

static inline void atspi_attr_kv(AtspiAttrKV *kv, const char *key, const char *val) {
    snprintf(kv->key, sizeof(kv->key), "%s", key);
    snprintf(kv->val, sizeof(kv->val), "%s", val);
}

static inline void atspi_attr_kv_color(AtspiAttrKV *kv, const char *key, u32 rgb) {
    char v[32];
    u32 max = ATSPI_ATTR_CHANNEL_MAX;
    snprintf(v, sizeof(v), "%u,%u,%u", ((rgb >> 16) & 0xFFu) * max / 255u,
             ((rgb >> 8) & 0xFFu) * max / 255u, (rgb & 0xFFu) * max / 255u);
    atspi_attr_kv(kv, key, v);
}

static inline const char *atspi_attr_underline_name(u8 u) {
    return u == ATSPI_UL_DOUBLE ? "double" : u == ATSPI_UL_ERROR ? "error"
         : u == ATSPI_UL_SINGLE ? "single" : "none";
}

/* The map of `s`. With `def` given, only what differs from it (a run's own
 * attributes); without, all six. Returns the entries written. */
static inline u32 atspi_attr_format(const AtspiAttrSet *s, const AtspiAttrSet *def,
                                    AtspiAttrKV *out) {
    u32 n = 0;
    if (!def || s->fg != def->fg) atspi_attr_kv_color(&out[n++], "fg-color", s->fg);
    if (!def || s->bg != def->bg) atspi_attr_kv_color(&out[n++], "bg-color", s->bg);
    if (!def || s->bold != def->bold) atspi_attr_kv(&out[n++], "weight", s->bold ? "700" : "400");
    if (!def || s->italic != def->italic) atspi_attr_kv(&out[n++], "style", s->italic ? "italic" : "normal");
    if (!def || s->underline != def->underline)
        atspi_attr_kv(&out[n++], "underline", atspi_attr_underline_name(s->underline));
    if (!def || s->strike != def->strike)
        atspi_attr_kv(&out[n++], "strikethrough", s->strike ? "true" : "false");
    return n;
}

/* A point size as text, one decimal at most, in no locale ("16", "13.5"). */
static inline void atspi_attr_size_text(f32 pt, char *out, usize cap) {
    if (!(pt > 0.0f) || pt > 1000.0f) { snprintf(out, cap, "%d", 0); return; }
    u32 tenths = (u32)(pt * 10.0f + 0.5f);
    if (tenths % 10u == 0) snprintf(out, cap, "%u", tenths / 10u);
    else snprintf(out, cap, "%u.%u", tenths / 10u, tenths % 10u);
}

/* The family a face file is named for, for a face whose own family string is
 * not known: its name without folders, extension and style words
 * ("Hack-Regular.ttf" -> "Hack"). */
static inline void atspi_attr_family(const char *file, char *out, usize cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!file || !file[0]) { snprintf(out, cap, "monospace"); return; }
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    usize len = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot && dot > base) len = (usize)(dot - base);
    static const char *const styles[] = {
        "Regular", "Bold", "Italic", "BoldItalic", "Oblique", "Light", "Medium", "Thin",
        "SemiBold", "ExtraLight", "ExtraBold", "Black", "Book", "Retina", NULL };
    for (bool again = true; again; ) {
        again = false;
        for (usize i = 0; styles[i]; i++) {
            usize sl = strlen(styles[i]);
            if (len > sl + 1 && base[len - sl - 1] == '-' && strncmp(base + len - sl, styles[i], sl) == 0) {
                len -= sl + 1;
                again = true;
                break;
            }
        }
    }
    if (len == 0) { snprintf(out, cap, "monospace"); return; }
    if (len >= cap) len = cap - 1;
    memcpy(out, base, len);
    out[len] = '\0';
}

/* ---- one snapshot's attributes ----------------------------------------------------- */

typedef struct {
    bool          valid;        /* the provider gave styles for this snapshot */
    AtspiAttrSet  defaults;     /* the pane's: its theme foreground and background, nothing else */
    char          family[96];
    f32           pt;
    AtspiAttrSet *sets;         /* nsets: what each of the provider's styles shows */
    u32           nsets, cap_sets;
    u16          *of_char;      /* n: the style of each character (index in sets) */
    u32           n, cap_chars;
} AtspiAttrs;

static inline void atspi_attrs_free(AtspiAttrs *a) {
    if (!a) return;
    free(a->sets);
    free(a->of_char);
    memset(a, 0, sizeof(*a));
}

/* Take the provider's style table for a new snapshot. Not valid (and the
 * snapshot answers with no attributes) when the provider gave none. */
static inline bool atspi_attrs_begin(AtspiAttrs *a, const PlatformA11yText *src) {
    a->valid = false;
    a->n = 0;
    if (!src || !src->style16 || !src->styles || src->style_count == 0 || !src->palette_rgb)
        return false;
    if (src->style_count > a->cap_sets) {
        AtspiAttrSet *p = realloc(a->sets, (usize)src->style_count * sizeof(*p));
        if (!p) return false;
        a->sets = p;
        a->cap_sets = src->style_count;
    }
    AtspiAttrCtx ctx = { src->palette_rgb, src->default_fg_rgb & 0xFFFFFFu,
                         src->default_bg_rgb & 0xFFFFFFu, src->bold_bright };
    for (u32 i = 0; i < src->style_count; i++) a->sets[i] = atspi_attr_derive(&ctx, &src->styles[i]);
    a->nsets = src->style_count;
    memset(&a->defaults, 0, sizeof(a->defaults));
    a->defaults.fg = ctx.def_fg;
    a->defaults.bg = ctx.def_bg;
    if (src->font_family && src->font_family[0]) {
        snprintf(a->family, sizeof(a->family), "%s", src->font_family);
    } else {
        atspi_attr_family(src->font_file, a->family, sizeof(a->family));
    }
    a->pt = src->font_pt;
    a->valid = true;
    return true;
}

/* Room for `nchars` characters' styles; false (and not valid) when memory ran out. */
static inline bool atspi_attrs_reserve(AtspiAttrs *a, u32 nchars) {
    if (!a->valid) return true;
    if (nchars <= a->cap_chars) return true;
    u32 c = a->cap_chars ? a->cap_chars : 256u;
    while (c < nchars) {
        if (c > 0x3FFFFFFFu) { a->valid = false; return false; }
        c *= 2u;
    }
    u16 *p = realloc(a->of_char, (usize)c * sizeof(u16));
    if (!p) { a->valid = false; return false; }
    a->of_char = p;
    a->cap_chars = c;
    return true;
}

/* Character `ci` shows the provider's style `id` (out of range = the plain cell). */
static inline void atspi_attrs_put(AtspiAttrs *a, u32 ci, u32 id) {
    if (!a->valid || ci >= a->cap_chars) return;
    a->of_char[ci] = (u16)(id < a->nsets ? id : 0u);
}

static inline void atspi_attrs_end(AtspiAttrs *a, u32 nchars) {
    a->n = a->valid ? nchars : 0;
}

static inline void atspi_attrs_clear(AtspiAttrs *a) {
    a->valid = false;
    a->n = 0;
}

/* ---- questions ------------------------------------------------------------------------ */

static inline bool atspi_attrs_same(const AtspiAttrs *a, u16 x, u16 y) {
    return x == y || atspi_attr_set_eq(&a->sets[x], &a->sets[y]);
}

/* The characters [*start, *end) around `off` that show the same attributes.
 * False when there are none to give (no styles, or `off` is not a character). */
static inline bool atspi_attrs_run(const AtspiAttrs *a, u32 off, u32 *start, u32 *end) {
    if (!a->valid || off >= a->n) return false;
    u16 id = a->of_char[off];
    u32 lo = off, hi = off + 1;
    while (lo > 0 && atspi_attrs_same(a, a->of_char[lo - 1], id)) lo--;
    while (hi < a->n && atspi_attrs_same(a, a->of_char[hi], id)) hi++;
    *start = lo;
    *end = hi;
    return true;
}

/* The pane's defaults: face and size, and the plain cell's six. */
static inline u32 atspi_attrs_defaults(const AtspiAttrs *a, AtspiAttrKV *out) {
    if (!a->valid) return 0;
    u32 n = 0;
    atspi_attr_kv(&out[n++], "family-name", a->family);
    char sz[16];
    atspi_attr_size_text(a->pt, sz, sizeof(sz));
    if (a->pt > 0.0f) atspi_attr_kv(&out[n++], "size", sz);
    n += atspi_attr_format(&a->defaults, NULL, out + n);
    return n;
}

/* The attributes of the run around `off`: only what differs from the defaults,
 * or, with `include_defaults`, every key (the run's value over the default). */
static inline u32 atspi_attrs_at(const AtspiAttrs *a, u32 off, bool include_defaults,
                                 AtspiAttrKV *out) {
    if (!a->valid || off >= a->n) return 0;
    const AtspiAttrSet *s = &a->sets[a->of_char[off]];
    if (!include_defaults) return atspi_attr_format(s, &a->defaults, out);
    u32 n = 0;
    atspi_attr_kv(&out[n++], "family-name", a->family);
    if (a->pt > 0.0f) {
        char sz[16];
        atspi_attr_size_text(a->pt, sz, sizeof(sz));
        atspi_attr_kv(&out[n++], "size", sz);
    }
    n += atspi_attr_format(s, NULL, out + n);
    return n;
}

/* One attribute's value at `off` (the full map's); false when the name is
 * not one of ours or `off` is not a character. */
static inline bool atspi_attrs_value(const AtspiAttrs *a, u32 off, const char *name,
                                     char *out, usize cap) {
    AtspiAttrKV kv[ATSPI_ATTR_MAX];
    u32 n = atspi_attrs_at(a, off, true, kv);
    for (u32 i = 0; i < n; i++) {
        if (strcmp(kv[i].key, name) == 0) {
            snprintf(out, cap, "%s", kv[i].val);
            return true;
        }
    }
    return false;
}

/* Do two snapshots of the same text show different attributes somewhere? */
static inline bool atspi_attrs_differ(const AtspiAttrs *a, const AtspiAttrs *b) {
    if (!a->valid || !b->valid || a->n != b->n) return false;
    for (u32 i = 0; i < a->n; i++)
        if (!atspi_attr_set_eq(&a->sets[a->of_char[i]], &b->sets[b->of_char[i]])) return true;
    return false;
}

#endif /* LIU_PLATFORM_ATSPI_ATTRS_H */
