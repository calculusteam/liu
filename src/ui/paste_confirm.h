/*
 * Liu — the "paste these lines?" prompt: when it is needed, what it shows, and
 * where its parts sit.
 *
 * A paste into a program that did NOT turn on bracketed paste is typed input
 * by definition: every line break in it is an Enter, so a clipboard that ends
 * in a newline — "copy this command" on a web page — runs as soon as it lands.
 * That is the oldest paste attack there is, and the guard against it is to
 * stop, show the text, ask. With bracketing ON the program owns the decision
 * and nothing is asked. core/paste_filter.h already guarantees that no control
 * byte rides along; this is the half that lets the user SEE the lines before
 * they run.
 *
 * Pure arithmetic and string handling, no AppState, for the reason
 * core/path_escape.h and ui/model_delete.h are: the renderer and the click
 * handler in main.c both need the panel's rectangles and must never derive
 * them separately, and all of it can be checked without a window.
 */
#ifndef UI_PASTE_CONFIRM_H
#define UI_PASTE_CONFIRM_H

#include "core/types.h"
#include "core/paste_filter.h"
#include "core/utf8.h"
#include <stdbool.h>
#include <string.h>

#define PC_PREVIEW_MAX_LINES 6     /* lines of the paste the panel shows */
#define PC_PREVIEW_COLS      60    /* columns of each; an ellipsis may add one */
#define PC_LINE_BYTES        256   /* one preview line, UTF-8 and NUL */

/* Does this paste need the user's say-so? True when it contains a line break
 * (LF, CR, or CRLF) AND something that is not whitespace — a lone "\n" only
 * presses Enter, which the user did by pasting it. `*breaks` receives the
 * number of line breaks (a CRLF counts once). */
static inline bool paste_confirm_needed(const char *s, i32 *breaks) {
    i32 n = 0;
    bool text = false;
    if (breaks) *breaks = 0;
    if (!s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\n') n++;
        else if (*p == '\r') { if (p[1] != '\n') n++; }
        else if (*p != ' ' && *p != '\t' && !paste_byte_unsafe(*p)) text = true;
    }
    if (breaks) *breaks = n;
    return n > 0 && text;
}

typedef struct {
    i32  lines;                    /* logical lines in the paste */
    i32  breaks;                   /* line breaks */
    i32  shown;                    /* preview lines filled */
    i32  hidden;                   /* lines not shown */
    char text[PC_PREVIEW_MAX_LINES][PC_LINE_BYTES];
} PasteSummary;

/* Rows the panel's preview box has: the lines shown plus a "… and N more
 * lines" row when some are hidden. At least one, so an odd paste still has a
 * box. */
static inline i32 paste_summary_rows(const PasteSummary *s) {
    i32 rows = s ? s->shown + (s->hidden > 0 ? 1 : 0) : 1;
    return rows < 1 ? 1 : rows;
}

/* Build what the panel shows from the RAW clipboard, as the filter will send
 * it: control bytes are skipped (so a hidden ESC sequence is not drawn as the
 * cursor movement it would have been — and is not sent either), TAB becomes a
 * space, each line is clipped to PC_PREVIEW_COLS display columns on a
 * GRAPHEME boundary and ends in an ellipsis (one column more) when it was cut.
 * A line is what lies between breaks; a trailing break does not start an empty
 * one. */
static inline void paste_confirm_summarize(const char *s, PasteSummary *out) {
    memset(out, 0, sizeof *out);
    if (!s) return;
    usize n = strlen(s);
    usize i = 0;
    i32 lines = 0;
    while (i < n) {
        usize e = i;
        while (e < n && s[e] != '\n' && s[e] != '\r') e++;
        lines++;
        if (out->shown < PC_PREVIEW_MAX_LINES) {
            char *dst = out->text[out->shown++];
            usize w = 0;
            i32 cols = 0;
            bool cut = false;
            for (usize k = i; k < e; ) {
                unsigned char c = (unsigned char)s[k];
                if (c == '\t') {
                    if (cols + 1 > PC_PREVIEW_COLS) { cut = true; break; }
                    dst[w++] = ' '; cols++; k++;
                    continue;
                }
                if (paste_byte_unsafe(c)) { k++; continue; }
                u32 cp = 0; i32 gc = 0;
                u32 used = utf8_next_grapheme((const u8 *)s + k, e - k, &cp, &gc);
                if (used == 0) { k++; continue; }                 /* malformed byte */
                if (cols + gc > PC_PREVIEW_COLS || w + used + 4 >= PC_LINE_BYTES) { cut = true; break; }
                memcpy(dst + w, s + k, used);
                w += used; cols += gc; k += used;
            }
            if (cut) { memcpy(dst + w, "\xe2\x80\xa6", 3); w += 3; }      /* … */
            dst[w] = '\0';
        }
        if (e >= n) break;
        i = (s[e] == '\r' && e + 1 < n && s[e + 1] == '\n') ? e + 2 : e + 1;
    }
    out->lines  = lines;
    out->hidden = lines > out->shown ? lines - out->shown : 0;
    paste_confirm_needed(s, &out->breaks);
}

/* ---- geometry: ONE author for the renderer (ui.c) and the click handler
 * (main.c). All in framebuffer pixels; `dpi` is the window's scale. -------- */
typedef struct { f32 x, y, w, h; } PcRect;

typedef struct {
    PcRect panel;
    PcRect preview;            /* the sunken box the lines sit in */
    PcRect cancel, paste;      /* buttons */
    f32    pad;
    f32    title_y, divider_y, body_y;
    f32    row_h;              /* one preview / body line */
} PasteConfirmGeom;

static inline PasteConfirmGeom paste_confirm_geom(f32 fb_w, f32 fb_h, f32 dpi, i32 preview_rows) {
    PasteConfirmGeom g;
    if (dpi <= 0.0f) dpi = 1.0f;
    if (preview_rows < 1) preview_rows = 1;
    const f32 ui_ch = 16.0f * dpi;
    g.pad   = 22.0f * dpi;
    g.row_h = ui_ch;

    f32 w = 560.0f * dpi;
    if (w > fb_w - 48.0f * dpi) w = fb_w - 48.0f * dpi;
    if (w < 200.0f * dpi) w = 200.0f * dpi;

    f32 y = 0.0f;                                  /* panel-relative first */
    g.title_y   = y + g.pad;
    g.divider_y = g.title_y + ui_ch + 12.0f * dpi;
    g.body_y    = g.divider_y + 18.0f * dpi;
    f32 prev_y  = g.body_y + 2.0f * ui_ch + 6.0f * dpi + 14.0f * dpi;
    f32 prev_h  = (f32)preview_rows * ui_ch + 20.0f * dpi;
    f32 btn_h   = 34.0f * dpi;
    f32 btn_y   = prev_y + prev_h + 18.0f * dpi;
    f32 h       = btn_y + btn_h + g.pad;

    f32 px = (fb_w - w) * 0.5f;
    f32 py = (fb_h - h) * 0.5f;
    if (py < 8.0f * dpi) py = 8.0f * dpi;

    g.panel   = (PcRect){ px, py, w, h };
    g.title_y += py; g.divider_y += py; g.body_y += py;
    g.preview = (PcRect){ px + g.pad, py + prev_y, w - 2.0f * g.pad, prev_h };
    f32 bw_paste = 120.0f * dpi, bw_cancel = 96.0f * dpi;
    f32 paste_x = px + w - g.pad - bw_paste;
    g.paste  = (PcRect){ paste_x, py + btn_y, bw_paste, btn_h };
    g.cancel = (PcRect){ paste_x - 12.0f * dpi - bw_cancel, py + btn_y, bw_cancel, btn_h };
    return g;
}

static inline bool pc_rect_hit(PcRect r, f32 x, f32 y) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

typedef enum { PC_HIT_NONE = 0, PC_HIT_PASTE, PC_HIT_CANCEL, PC_HIT_OUTSIDE } PcHit;

/* A press at (x, y): Paste, Cancel, outside the panel (which cancels, like
 * every other modal here) or inert chrome inside it. */
static inline PcHit paste_confirm_hit(const PasteConfirmGeom *g, f32 x, f32 y) {
    if (pc_rect_hit(g->paste, x, y))  return PC_HIT_PASTE;
    if (pc_rect_hit(g->cancel, x, y)) return PC_HIT_CANCEL;
    if (!pc_rect_hit(g->panel, x, y)) return PC_HIT_OUTSIDE;
    return PC_HIT_NONE;
}

#endif /* UI_PASTE_CONFIRM_H */
