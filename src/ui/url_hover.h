/* The Cmd-hover link underline persists between pointer events. Scrolling
 * output, a shell repaint, or a keyboard resize can change the text beneath a
 * stationary pointer, making a cached underline disagree with the link activated
 * on click.
 *
 * Store the terminal's content-write count, scroll offset, and grid size with
 * the detected link. If any changes, re-run detection at the same cell and
 * update the underline when the link changes.
 */
#ifndef LIU_UI_URL_HOVER_H
#define LIU_UI_URL_HOVER_H

#include "core/types.h"
#include "terminal/terminal.h"

typedef struct {
    u64 writes;     /* cells whose codepoint changed */
    i64 pushed;     /* lines that left the top of the primary screen */
    i32 scroll;     /* the viewport's offset into history */
    i32 cols, rows;
    bool alt;       /* which screen */
} UrlHoverSnap;

/* content_writes alone misses one move: a scroll that writes nothing — a run of
 * empty lines pushes a link up the screen with every codepoint on it unchanged.
 * sb_abs_base + sb_count is the count of lines ever pushed into history, which
 * such a scroll does bump; the alt screen pushes nothing, but every program on
 * it repaints, and the swap itself is recorded. */
static inline UrlHoverSnap url_hover_snap(const Terminal *t) {
    UrlHoverSnap s = {0};
    if (t) {
        s.writes = t->content_writes;
        s.pushed = t->sb_abs_base + (i64)t->sb_count;
        s.scroll = t->scroll_offset;
        s.cols   = t->cols;
        s.rows   = t->rows;
        s.alt    = (t->mode & MODE_ALT_SCREEN) != 0;
    }
    return s;
}

/* Has the text the hover was detected against moved under it? */
static inline bool url_hover_stale(const UrlHoverSnap *s, const Terminal *t) {
    if (!s || !t) return true;
    UrlHoverSnap now = url_hover_snap(t);
    return s->writes != now.writes || s->pushed != now.pushed ||
           s->scroll != now.scroll || s->cols != now.cols || s->rows != now.rows ||
           s->alt != now.alt;
}

#endif /* LIU_UI_URL_HOVER_H */
