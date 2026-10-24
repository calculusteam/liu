/*
 * The row combines an agent tile, name and topic, and a status pill. At the
 * default 240 pt sidebar width, the full pill uses 11 display cells at the 8 pt
 * UI cell size, plus padding and the dot, leaving 7 cells for text.
 *
 * The compact layout follows these rules:
 * - The pill shrinks from its full label to a short label, then to a dot, then
 *   disappears for an idle agent.
 * - The name is shown in full or as its first word; it is not clipped to a
 *   partial word.
 * - The topic is shown in full, ellipsized when at least `SBA_TOPIC_MIN_COLS`
 *   fit, or hidden below that width. Without a topic, the name is vertically
 *   centered.
 * - If the first word cannot fit beside the dot, show only the tile and dot.
 *
 * Geometry is in framebuffer pixels; the caller measures text in display
 * columns. This header does not depend on `AppState`, theme, or agent type.
 */
#ifndef UI_SIDEBAR_AGENT_ROW_H
#define UI_SIDEBAR_AGENT_ROW_H

#include "core/types.h"
#include "core/utf8.h"
#include <string.h>

/* Points. The painter reads them from here; nothing else may restate them. */
#define SBA_TILE_X_PT          16.0f   /* tile's left edge */
#define SBA_TILE_PT            22.0f   /* tile is square */
#define SBA_TEXT_GAP_PT         8.0f   /* tile → text */
#define SBA_PILL_MARGIN_PT     10.0f   /* pill → the bar's right edge */
#define SBA_PILL_PAD_PT         7.0f   /* pill's own padding, each side */
#define SBA_PILL_H_PT          18.0f
#define SBA_DOT_PT              5.0f   /* the working dot */
#define SBA_DOT_GAP_PT          5.0f   /* dot → label */
#define SBA_TEXT_PILL_GAP_PT    8.0f   /* text → pill: the two may never touch */
#define SBA_TOPIC_MIN_COLS      8      /* below this an ellipsized topic says nothing */

typedef enum {
    SBA_PILL_NONE = 0,   /* no pill (idle, at the last rung) */
    SBA_PILL_DOT,        /* the dot alone, in its own small pill */
    SBA_PILL_SHORT,      /* [dot] short label: the duration */
    SBA_PILL_FULL        /* [dot] full label: "working 5m" / "idle 5m" / "idle" */
} SbaPill;

typedef enum {
    SBA_NAME_NONE = 0,
    SBA_NAME_SHORT,      /* the first word */
    SBA_NAME_FULL
} SbaName;

typedef struct {
    f32  w;               /* the bar's width, px */
    f32  dpi;
    f32  cw;              /* the UI cell width, px (8pt at this dpi) */
    i32  name_cols;       /* the full name, display columns */
    i32  short_cols;      /* its first word (== name_cols for a one-word name) */
    i32  topic_cols;      /* the topic line; 0 = there is none */
    i32  pill_full_cols;  /* the full label's columns ("working now" = 11) */
    i32  pill_short_cols; /* the short label's ("now" = 3, "5m" = 2); 0 = none */
    bool working;         /* the dot precedes the label, and stands alone last */
} SbaIn;

typedef struct {
    f32     tile_x, tile;      /* the logo tile: x and side */
    f32     text_x;            /* where the name and topic start */
    SbaPill pill;
    f32     pill_x, pill_w;    /* pill_w == 0 when SBA_PILL_NONE */
    SbaName name;
    bool    topic;             /* draw the topic line at all */
    bool    topic_clip;        /* it did not fit whole: ellipsize to topic_cols */
    i32     topic_cols;        /* columns the topic may use (incl. the ellipsis) */
    f32     room;              /* px from text_x to the pill's gap: the text's width budget */
} SbaOut;

/* Width of a pill drawn with `label_cols` cells of text, or of the dot alone
 * when label_cols is 0 and `working`; 0 when there is nothing to show. */
static inline f32 sba_pill_width(i32 label_cols, bool working, f32 cw, f32 dpi) {
    if (label_cols <= 0 && !working) return 0.0f;
    f32 w = 2.0f * SBA_PILL_PAD_PT * dpi;
    if (working) w += SBA_DOT_PT * dpi + (label_cols > 0 ? SBA_DOT_GAP_PT * dpi : 0.0f);
    w += (f32)label_cols * cw;
    return w;
}

static inline f32 sba_pill_width_of(const SbaIn *in, SbaPill p) {
    switch (p) {
    case SBA_PILL_FULL:  return sba_pill_width(in->pill_full_cols, in->working, in->cw, in->dpi);
    case SBA_PILL_SHORT: return sba_pill_width(in->pill_short_cols, in->working, in->cw, in->dpi);
    case SBA_PILL_DOT:   return sba_pill_width(0, in->working, in->cw, in->dpi);
    default:             return 0.0f;
    }
}

/* Room left for the text with pill `p` on the right edge. */
static inline f32 sba_room_for(const SbaIn *in, SbaPill p, f32 text_x, f32 *pill_x, f32 *pill_w) {
    f32 pw = sba_pill_width_of(in, p);
    f32 px = in->w - SBA_PILL_MARGIN_PT * in->dpi - pw;
    if (pill_x) *pill_x = px;
    if (pill_w) *pill_w = pw;
    f32 room = px - text_x - (pw > 0.0f ? SBA_TEXT_PILL_GAP_PT * in->dpi : 0.0f);
    return room > 0.0f ? room : 0.0f;
}

static inline void sba_layout(const SbaIn *in, SbaOut *o) {
    memset(o, 0, sizeof *o);
    f32 dpi = in->dpi;
    o->tile_x = SBA_TILE_X_PT * dpi;
    o->tile   = SBA_TILE_PT * dpi;
    o->text_x = o->tile_x + o->tile + SBA_TEXT_GAP_PT * dpi;

    /* A rung is a (name, pill) pair; the outer walk is the name because the
     * pill's word is redundant with its colour and its dot, the name is not. */
    const SbaName names[2] = { SBA_NAME_FULL, SBA_NAME_SHORT };
    const i32     ncols[2] = { in->name_cols, in->short_cols };
    const SbaPill pills[3] = { SBA_PILL_FULL, SBA_PILL_SHORT, SBA_PILL_DOT };
    bool found = false;
    for (i32 n = 0; n < 2 && !found; n++) {
        if (ncols[n] <= 0) continue;
        /* A one-word name has no shorter form to fall back to. */
        if (n == 1 && in->short_cols >= in->name_cols) continue;
        for (i32 p = 0; p < 3; p++) {
            /* The short pill with no short label IS the dot rung; skip it
             * rather than land on it twice. */
            if (pills[p] == SBA_PILL_SHORT && in->pill_short_cols <= 0) continue;
            f32 px, pw;
            f32 room = sba_room_for(in, pills[p], o->text_x, &px, &pw);
            if ((f32)ncols[n] * in->cw <= room + 0.5f) {
                o->name = names[n];
                o->pill = pw > 0.0f ? pills[p] : SBA_PILL_NONE;
                o->pill_x = px; o->pill_w = pw; o->room = room;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        /* Nothing fits beside the dot: the tile and the dot are the row. */
        f32 px, pw;
        o->room = sba_room_for(in, SBA_PILL_DOT, o->text_x, &px, &pw);
        o->name = SBA_NAME_NONE;
        o->pill = pw > 0.0f ? SBA_PILL_DOT : SBA_PILL_NONE;
        o->pill_x = px; o->pill_w = pw;
        return;
    }
    if (in->topic_cols > 0 && in->cw > 0.0f) {
        i32 avail = (i32)((o->room + 0.5f) / in->cw);
        if (in->topic_cols <= avail) {
            o->topic = true; o->topic_clip = false; o->topic_cols = in->topic_cols;
        } else if (avail >= SBA_TOPIC_MIN_COLS) {
            o->topic = true; o->topic_clip = true; o->topic_cols = avail;
        }
    }
}

/* Copy `s` into `out` clipped to `cols` display columns, the last of them the
 * ellipsis. Graphemes, not bytes or codepoints (utf8_next_grapheme — the rule
 * every UI surface walks its text with): an emoji or a CJK character in a chat
 * title is two columns and must not be cut in half. `cols` < 2 yields the
 * ellipsis alone. */
static inline void sba_clip_ellipsis(const char *s, i32 cols, char *out, usize cap) {
    const u8 *p = (const u8 *)s;
    usize len = strlen(s), o = 0;
    i32 used = 0, budget = cols - 1;   /* the ellipsis takes the last one */
    while (*p && cap > 4) {
        u32 cp; i32 gc = 1;
        u32 gb = utf8_next_grapheme(p, len - (usize)(p - (const u8 *)s), &cp, &gc);
        if (gb == 0) break;
        if (gc < 0) gc = 0;
        if (used + gc > budget) break;
        if (o + gb + 4 > cap) break;
        memcpy(out + o, p, gb);
        o += gb; used += gc; p += gb;
    }
    if (o + 4 <= cap) { memcpy(out + o, "\xE2\x80\xA6", 3); o += 3; }
    if (cap) out[o < cap ? o : cap - 1] = '\0';
}

#endif /* UI_SIDEBAR_AGENT_ROW_H */
