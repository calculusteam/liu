/*
 * Liu — in-document Find / Replace for the file-browser viewer.
 *
 * Operates over fb->view_content as a flat byte buffer, so the same match set
 * drives every viewer mode: matches are byte ranges that map to glyph src_off
 * in the rendered markdown view and to raw line offsets in the live-preview /
 * code editor. Plain substring (optionally case-folded) or POSIX extended
 * regex. Replace is gated on an editable buffer (editor_mode / raw mode) so
 * bytes are never spliced out from under a parsed AST in rendered read mode.
 */
#include "ui/filebrowser.h"
#include "ui/markdown/md_render.h"   /* full MdGlyphRect + MD_GLYPH_NO_SRC */
#include "ui/chrome_palette.h"
#include "core/checked.h"
#include "core/config.h"
#include "core/utf8.h"
#include "renderer/renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifndef PLATFORM_WIN32
#include <regex.h>   /* no POSIX regex on MinGW: find degrades to substring */
#endif

/* -------------------------------------------------------------------------
 * Matching
 * ------------------------------------------------------------------------- */

static bool ff_push_match(FileBrowser *fb, u32 start, u32 end) {
    if (fb->find_match_count >= fb->find_match_cap) {
        u32 ncap = fb->find_match_cap ? fb->find_match_cap * 2 : 256;
        if (ncap > FB_FIND_MAX_MATCHES) ncap = FB_FIND_MAX_MATCHES;
        if (fb->find_match_count >= ncap) return false;   /* hit the hard cap */
        usize bytes;
        if (!checked_mul_usize((usize)ncap, sizeof(*fb->find_matches), &bytes))
            return false;
        FbFindMatch *n = realloc(fb->find_matches, bytes);
        if (!n) return false;
        fb->find_matches = n;
        fb->find_match_cap = ncap;
    }
    fb->find_matches[fb->find_match_count++] = (FbFindMatch){ start, end };
    return true;
}

/* Case-aware byte compare of `q[qlen]` against the haystack at `hay`. */
static bool ff_eq(const char *hay, const char *q, u32 qlen, bool fold) {
    for (u32 i = 0; i < qlen; i++) {
        char a = hay[i], b = q[i];
        if (fold) { a = (char)tolower((unsigned char)a); b = (char)tolower((unsigned char)b); }
        if (a != b) return false;
    }
    return true;
}

/* One forward scan of the text for the query: ff_scan_begin compiles the
 * pattern (false = it does not compile), ff_scan_next hands out the matches
 * in order from the start, never overlapping, ff_scan_end frees it.
 *
 * Both readers walk THIS — the match list, which is capped because two passes
 * draw from it every frame, and Replace All, which must not be capped at all.
 * One walk, so the two cannot disagree about what a match is.
 *
 * The text is a byte range with a length. A regex over `text + pos` as a C
 * string would stop at the first NUL in the file and pay a strlen of the rest
 * for every match; REG_STARTEND, where the platform has it, does neither. */
typedef struct {
    const char *text;
    usize       size;
    const char *query;
    u32         qlen;
    bool        fold;
    bool        regex;
#ifndef PLATFORM_WIN32
    regex_t     re;
#endif
    usize       pos;             /* where the next search starts */
} FfScan;

static bool ff_scan_begin(FfScan *s, const FileBrowser *fb) {
    memset(s, 0, sizeof *s);
    s->text  = fb->view_content;
    s->size  = fb->view_size;
    s->query = fb->find_query;
    s->qlen  = fb->find_query_len;
    s->fold  = !fb->find_case;
#ifndef PLATFORM_WIN32
    if (fb->find_regex) {
        int cflags = REG_EXTENDED | REG_NEWLINE | (fb->find_case ? 0 : REG_ICASE);
        if (regcomp(&s->re, fb->find_query, cflags) != 0) return false;
        s->regex = true;
    }
#endif
    return true;
}

static void ff_scan_end(FfScan *s) {
#ifndef PLATFORM_WIN32
    if (s->regex) regfree(&s->re);
#endif
    s->regex = false;
}

static bool ff_scan_next(FfScan *s, u32 *start, u32 *end) {
#ifndef PLATFORM_WIN32
    if (s->regex) {
        if (s->pos > s->size) return false;
        int eflags = (s->pos > 0 && s->text[s->pos - 1] != '\n') ? REG_NOTBOL : 0;
        regmatch_t m;
        usize ms, me;
#ifdef REG_STARTEND
        m.rm_so = (regoff_t)s->pos;
        m.rm_eo = (regoff_t)s->size;
        if (regexec(&s->re, s->text, 1, &m, eflags | REG_STARTEND) != 0) {
            s->pos = s->size + 1;
            return false;
        }
        ms = (usize)m.rm_so;
        me = (usize)m.rm_eo;
#else
        if (regexec(&s->re, s->text + s->pos, 1, &m, eflags) != 0) {
            s->pos = s->size + 1;
            return false;
        }
        ms = s->pos + (usize)m.rm_so;
        me = s->pos + (usize)m.rm_eo;
#endif
        /* Past the match. An EMPTY one ("^", "a*") is passed by a whole
         * character, not a byte: the next search would otherwise start — and
         * match, and let Replace insert — in the middle of a ş. */
        if (me > ms) {
            s->pos = me;
        } else {
            usize left = s->size - me;
            i32 step = editor_char_len(s->text + me, (i32)(left < 4 ? left : 4), 0);
            s->pos = me + (usize)(step > 0 ? step : 1);
        }
        *start = (u32)ms;
        *end = (u32)me;
        return true;
    }
#endif
    if (s->qlen == 0) return false;
    while (s->pos + s->qlen <= s->size) {
        if (ff_eq(s->text + s->pos, s->query, s->qlen, s->fold)) {
            *start = (u32)s->pos;
            *end = (u32)(s->pos + s->qlen);
            s->pos += s->qlen;
            return true;
        }
        s->pos++;
    }
    return false;
}

void fb_find_recompute(FileBrowser *fb) {
    if (!fb) return;
    fb->find_match_count = 0;
    fb->find_regex_error = false;
    fb->find_truncated = false;
    /* The list about to be built is for the text as it is now. */
    fb->find_gen = fb->view_edit_gen;
    if (!fb->view_content || fb->find_query_len == 0) {
        fb->find_current = -1;
        return;
    }
    FfScan scan;
    if (!ff_scan_begin(&scan, fb)) {
        fb->find_regex_error = true;
        fb->find_current = -1;
        return;
    }
    u32 ms, me;
    while (ff_scan_next(&scan, &ms, &me)) {
        if (!ff_push_match(fb, ms, me)) {
            fb->find_truncated = true;      /* there are more than the list holds */
            break;
        }
    }
    ff_scan_end(&scan);

    if (fb->find_match_count == 0) fb->find_current = -1;
    else if (fb->find_current < 0 || (u32)fb->find_current >= fb->find_match_count)
        fb->find_current = 0;
}

/* Index of the first match that starts at or after `off`, or -1. */
static i32 ff_first_at_or_after(const FileBrowser *fb, usize off) {
    u32 lo = 0, hi = fb->find_match_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (fb->find_matches[mid].start < off) lo = mid + 1;
        else                                   hi = mid;
    }
    return lo < fb->find_match_count ? (i32)lo : -1;
}

/* The match list is byte ranges into the text AS IT WAS when they were found.
 * After any edit they name other bytes: with the bar open, a paste, a cut, an
 * undo or a Delete moves everything behind it, and a Replace at the old offsets
 * would land in the middle of some other word.
 *
 * Every reader of the list comes through here first. When the text has moved on
 * (view_edit_gen), the list is rebuilt, and the current match becomes the first
 * one at or after the caret — which is where the edit just happened and where
 * the user is looking — wrapping to the first. True when it rescanned. */
static bool ff_sync(FileBrowser *fb) {
    if (fb->find_gen == fb->view_edit_gen) return false;
    fb_find_recompute(fb);
    if (fb->find_match_count > 0) {
        i32 at = ff_first_at_or_after(fb, fb_editor_cursor_offset(fb));
        fb->find_current = at >= 0 ? at : 0;
    }
    return true;
}

bool fb_find_sync(FileBrowser *fb) {
    if (!fb || !fb->find_active) return false;
    return ff_sync(fb);
}

/* -------------------------------------------------------------------------
 * Navigation — move the cursor to a match and scroll it into view
 * ------------------------------------------------------------------------- */

static void ff_offset_to_linecol(const FileBrowser *fb, u32 off, i32 *line, i32 *col) {
    i32 ln = 0; u32 ls = 0;
    u32 lim = off < fb->view_size ? off : (u32)fb->view_size;
    for (u32 i = 0; i < lim; i++)
        if (fb->view_content[i] == '\n') { ln++; ls = i + 1; }
    *line = ln;
    *col = (i32)(off - ls);
}

static void ff_reveal_current(FileBrowser *fb) {
    if (fb->find_current < 0 || (u32)fb->find_current >= fb->find_match_count) return;
    FbFindMatch m = fb->find_matches[fb->find_current];
    i32 line, col;
    ff_offset_to_linecol(fb, m.start, &line, &col);
    fb->cursor_line = line;
    fb->cursor_col  = col;

    bool rendered_md = (fb->view_mode == FVIEW_MARKDOWN && !fb->md_raw_mode);
    if (rendered_md) {
        /* Map the match's first glyph (via src_off recorded last frame) to a
         * content-space y and scroll it near the top. Self-corrects next frame
         * if rects were stale. */
        for (u32 i = 0; i < fb->md_glyph_count; i++) {
            MdGlyphRect *g = &fb->md_glyph_rects[i];
            if (g->src_off != MD_GLYPH_NO_SRC && g->src_off >= m.start && g->src_off < m.end) {
                f32 content_y = g->y - fb->md_origin_y + fb->md_glyph_scroll_px;
                f32 target = content_y - 80.0f;
                fb->view_scroll_px = target > 0.0f ? target : 0.0f;
                break;
            }
        }
    } else if (fb->view_mode == FVIEW_MARKDOWN && fb->md_raw_mode) {
        fb->live_follow = true;
    } else {
        /* Code and text editors scroll by source line. */
        fb->view_scroll = line > 3 ? line - 3 : 0;
    }
}

void fb_find_next(FileBrowser *fb) {
    if (!fb) return;
    /* After an edit the list is rebuilt and "current" is already the first
     * match at or after the caret: that IS the next one. */
    bool resynced = ff_sync(fb);
    if (fb->find_match_count == 0) return;
    if (!resynced)
        fb->find_current = (fb->find_current + 1) % (i32)fb->find_match_count;
    ff_reveal_current(fb);
}

void fb_find_prev(FileBrowser *fb) {
    if (!fb) return;
    ff_sync(fb);
    if (fb->find_match_count == 0) return;
    fb->find_current = (fb->find_current - 1 + (i32)fb->find_match_count) % (i32)fb->find_match_count;
    ff_reveal_current(fb);
}

/* -------------------------------------------------------------------------
 * Replace — only in an editable context (never under a live read-mode AST)
 * ------------------------------------------------------------------------- */

static bool ff_replace_ok(const FileBrowser *fb) {
    /* raw markdown render + code/text editors read view_content directly; the
     * rendered markdown view holds AST slices into view_content, so splicing
     * there would dangle them. */
    return fb && fb->view_content &&
           (fb->editor_mode || fb->md_raw_mode);
}

bool fb_find_replace_one(FileBrowser *fb) {
    if (!ff_replace_ok(fb)) return false;
    /* The offsets below are spliced at: they have to be this text's. */
    ff_sync(fb);
    if (fb->find_current < 0 || (u32)fb->find_current >= fb->find_match_count) return false;
    FbFindMatch m = fb->find_matches[fb->find_current];
    fb_editor_push_undo(fb);
    if (!fb_editor_splice(fb, m.start, m.end - m.start,
                          fb->find_replace, fb->find_replace_len))
        return false;
    fb_editor_push_undo(fb);
    fb_find_recompute(fb);
    /* The next match is the first one PAST what was just written. The rescan
     * keeps the INDEX, and when the replacement contains the query that index
     * is the replacement's own text: "foo" -> "foobar" pressed twice would
     * give "foobarbar". An empty match ("^") is passed by one more character,
     * or it is found again where it was. When nothing follows there is no
     * current match — wrapping round to the replacement's own output is a
     * step the user takes (Next), not one a repeated Replace takes for them.
     */
    usize past = (usize)m.start + fb->find_replace_len;
    if (m.end == m.start) {
        usize left = past < fb->view_size ? fb->view_size - past : 0;
        i32 step = editor_char_len(fb->view_content + past, (i32)(left < 4 ? left : 4), 0);
        past += (usize)(step > 0 ? step : 1);
    }
    fb->find_current = ff_first_at_or_after(fb, past);
    /* Keep the cursor where the edit landed: after the replacement. */
    i32 line, col;
    ff_offset_to_linecol(fb, (u32)((usize)m.start + fb->find_replace_len), &line, &col);
    fb->cursor_line = line; fb->cursor_col = col;
    if (fb->find_current >= 0) ff_reveal_current(fb);
    return true;
}

void fb_find_replace_all(FileBrowser *fb) {
    if (!ff_replace_ok(fb) || fb->find_query_len == 0) return;
    /* Straight from the scan, not from the match list: the list stops at
     * FB_FIND_MAX_MATCHES (it is what the highlight passes draw from), so a
     * Replace All built on it would stop at the cap without a word. The scan
     * runs over the text as it is, so there is no stale list to trust
     * either.
     *
     * The affected span is built once: repeated backwards splices would move
     * the document tail once per match and split one Replace All into many
     * undo units. The scan supplies sorted, non-overlapping ranges, and
     * never looks at what this writes — the output is not rescanned. */
    FfScan scan;
    if (!ff_scan_begin(&scan, fb)) { fb->find_regex_error = true; return; }
    char *span = NULL;
    usize len = 0, cap = 0, first = 0, previous = 0;
    u32 count = 0, ms, me;
    bool ok = true;
    while (ff_scan_next(&scan, &ms, &me)) {
        if (count == 0) first = previous = ms;
        usize gap = ms - previous, need;
        if (!checked_add_usize(len, gap, &need) ||
            !checked_add_usize(need, fb->find_replace_len, &need) ||
            !checked_add_usize(need, 1, &need)) { ok = false; break; }
        if (need > cap) {
            usize grown = cap ? cap : 4096;
            while (grown < need) {
                if (grown > ((usize)-1) / 2u) { grown = need; break; }
                grown *= 2u;
            }
            char *bigger = realloc(span, grown);
            if (!bigger) { ok = false; break; }
            span = bigger;
            cap = grown;
        }
        memcpy(span + len, fb->view_content + previous, gap); len += gap;
        memcpy(span + len, fb->find_replace, fb->find_replace_len); len += fb->find_replace_len;
        previous = me;
        count++;
    }
    ff_scan_end(&scan);
    if (ok && count > 0) {
        span[len] = '\0';
        fb_editor_push_undo(fb);
        ok = fb_editor_splice(fb, first, previous - first, span, len);
        if (ok) fb_editor_push_undo(fb);
    }
    free(span);
    /* All or nothing: a result the editor will not hold (its size limit) or
     * could not be built is not half-applied, and it is not silent. */
    if (!ok) fb_notice_set(fb, true, "Not replaced: the result is too large");
    fb_find_recompute(fb);
    /* Every match was dealt with; what the rescan finds now is the
     * replacements' own output (a replacement that contains the query), and
     * none of it is "current". */
    fb->find_current = -1;
}

/* -------------------------------------------------------------------------
 * Open / close / input
 * ------------------------------------------------------------------------- */

void fb_find_open(FileBrowser *fb, bool replace_mode) {
    if (!fb) return;
    fb->find_active = true;
    /* Replace only makes sense on an editable buffer. */
    fb->find_replace_mode = replace_mode && ff_replace_ok(fb);
    fb->find_focus_replace = false;
    fb_find_recompute(fb);
}

void fb_find_close(FileBrowser *fb) {
    if (!fb) return;
    fb->find_active = false;
    fb->find_replace_mode = false;
    fb->find_focus_replace = false;
    fb->find_match_count = 0;
    fb->find_current = -1;
    fb->find_regex_error = false;
}

void fb_find_input_char(FileBrowser *fb, u32 cp) {
    if (!fb || !fb->find_active) return;
    char  *buf = fb->find_focus_replace ? fb->find_replace : fb->find_query;
    u32   *len = fb->find_focus_replace ? &fb->find_replace_len : &fb->find_query_len;
    u32    cap = (u32)sizeof(fb->find_query);   /* both fields are the same size */
    u8 enc[4];
    u32 n = utf8_encode(cp, enc);
    if (n == 0 || *len + n >= cap) return;
    memcpy(buf + *len, enc, n);
    *len += n;
    buf[*len] = '\0';
    if (!fb->find_focus_replace) fb_find_recompute(fb);
}

void fb_find_input_backspace(FileBrowser *fb) {
    if (!fb || !fb->find_active) return;
    char  *buf = fb->find_focus_replace ? fb->find_replace : fb->find_query;
    u32   *len = fb->find_focus_replace ? &fb->find_replace_len : &fb->find_query_len;
    if (*len == 0) return;
    /* Step back one UTF-8 codepoint. */
    u32 i = *len - 1;
    while (i > 0 && ((unsigned char)buf[i] & 0xC0) == 0x80) i--;
    *len = i;
    buf[*len] = '\0';
    if (!fb->find_focus_replace) fb_find_recompute(fb);
}

/* -------------------------------------------------------------------------
 * Highlight passes
 *
 * Both passes below run PER FRAME for as long as the find bar is open, so
 * neither may be linear in the match count. `find_matches[]` is sorted by
 * `start` and non-overlapping — both producers in fb_find_recompute() scan
 * forward and advance past each hit — which is what lets them binary-search
 * instead.
 *
 * That property is load-bearing: asking "which match contains this glyph?" by
 * walking every match for every glyph, with both capped at 8192, would be
 * quadratic in the worst case and paid again on every keystroke in the find
 * box.
 * ------------------------------------------------------------------------- */

/* Index of the last match whose start <= off, or -1 when none is at or before
 * it. Callers still have to check `off < matches[i].end`: this only says which
 * match could possibly contain the offset. */
static i32 ff_match_at_or_before(const FileBrowser *fb, u32 off) {
    u32 lo = 0, hi = fb->find_match_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (fb->find_matches[mid].start <= off) lo = mid + 1;
        else                                    hi = mid;
    }
    return lo ? (i32)(lo - 1) : -1;
}

void fb_find_draw_matches(FileBrowser *fb, Renderer *r, const Theme *theme) {
    if (!fb || !fb->find_active) return;
    fb_find_sync(fb);                    /* a highlight is an offset too */
    if (fb->find_match_count == 0 || !fb->md_glyph_rects)
        return;
    Color all = theme ? theme->ansi[3] : (Color){0.92f, 0.72f, 0.33f, 1.0f};
    Color cur = theme_ui_accent(theme);
    Color all_bg = { all.r, all.g, all.b, 0.28f };
    Color cur_bg = { cur.r, cur.g, cur.b, 0.45f };
    for (u32 i = 0; i < fb->md_glyph_count; i++) {
        MdGlyphRect *g = &fb->md_glyph_rects[i];
        if (g->src_off == MD_GLYPH_NO_SRC) continue;
        /* Which match (if any) contains this glyph? */
        i32 mi = ff_match_at_or_before(fb, g->src_off);
        if (mi < 0 || g->src_off >= fb->find_matches[mi].end) continue;
        bool is_cur = (mi == fb->find_current);
        renderer_draw_rect(r, g->x, g->y, g->w, g->h, is_cur ? cur_bg : all_bg);
    }
}

void fb_find_draw_line(const FileBrowser *fb, Renderer *r, usize line_off,
                       i32 line_len, f32 lx, f32 cy, f32 lcw, f32 lch,
                       const Theme *theme) {
    if (!fb || !r || !fb->view_content || !fb->find_active ||
        fb->find_match_count == 0 || line_len <= 0 ||
        line_off > fb->view_size || (usize)line_len > fb->view_size - line_off)
        return;
    /* A list from before the last edit names other bytes: draw none of it.
     * (This pass cannot rescan — the browser is const here; fb_find_render,
     * which closes the same frame, does, and the next frame draws it right.) */
    if (fb->find_gen != fb->view_edit_gen) return;
    Color all = theme ? theme->ansi[3] : (Color){0.92f, 0.72f, 0.33f, 1.0f};
    Color cur = theme_ui_accent(theme);
    Color all_bg = { all.r, all.g, all.b, 0.28f };
    Color cur_bg = { cur.r, cur.g, cur.b, 0.45f };
    u32 ls = (u32)line_off, le = (u32)line_off + (u32)line_len;
    const char *lp = fb->view_content + line_off;
    /* Start at the last match beginning at or before this line — the only one
     * that can straddle its first byte — and stop as soon as starts run past
     * the line's end. Scanning all 8192 matches for each of ~50 visible lines
     * is cheap per compare but adds up to real per-frame work for nothing. */
    i32 first = ff_match_at_or_before(fb, ls);
    for (u32 mi = (first > 0 ? (u32)first : 0); mi < fb->find_match_count; mi++) {
        FbFindMatch m = fb->find_matches[mi];
        if (m.start >= le) break;                          /* sorted: no more overlap */
        if (m.end <= ls) continue;                         /* ends before the line */
        u32 s = m.start > ls ? m.start : ls;
        u32 e = m.end   < le ? m.end   : le;
        /* Codepoint columns for the byte sub-range within the line. */
        i32 col_s = 0;
        for (u32 b = ls; b < s; ) {
            u32 cp; u32 nb = utf8_decode((const u8 *)lp + (b - ls), le - b, &cp);
            b += nb ? nb : 1; col_s++;
        }
        i32 span = 0;
        for (u32 b = s; b < e; ) {
            u32 cp; u32 nb = utf8_decode((const u8 *)lp + (b - ls), e - b, &cp);
            b += nb ? nb : 1; span++;
        }
        bool is_cur = ((i32)mi == fb->find_current);
        renderer_draw_rect(r, lx + (f32)col_s * lcw, cy, (f32)span * lcw, lch,
                           is_cur ? cur_bg : all_bg);
    }
}

/* -------------------------------------------------------------------------
 * Overlay bar
 * ------------------------------------------------------------------------- */

static void ff_text(Renderer *r, const char *s, f32 x, f32 y, Color c, f32 cw, f32 maxw) {
    if (!s) return;
    font_warm_text_glyphs(&r->font, s);
    const u8 *p = (const u8 *)s;
    f32 cx = x;
    while (*p) {
        if (maxw > 0.0f && cx + cw > x + maxw) break;
        u32 cp; u32 n = utf8_decode(p, 4, &cp);
        if (!n) { p++; continue; }
        if (cp >= 32) { renderer_push_glyph(r, cx, y, cp, c); cx += cw; }
        p += n;
    }
}

/* A compact field: label, the text + caret if focused, clipped to `fw`. */
/* Returns where the caret was drawn, or -1 when the field drew none. */
static f32 ff_field(Renderer *r, const char *label, const char *text, bool focused,
                    f32 x, f32 y, f32 fw, f32 cw, f32 ch, Color fg, Color dim,
                    Color caret, const ChromePalette *cp) {
    f32 lblw = (f32)strlen(label) * cw;
    ff_text(r, label, x, y, dim, cw, lblw);
    f32 fx = x + lblw + cw * 0.5f;
    f32 inner_w = fw - lblw - cw * 0.5f;
    Color sunk = cp->surface_sunken;
    renderer_draw_rrect_simple(r, fx, y - 2.0f, inner_w, ch + 4.0f, sunk, 4.0f);
    f32 tx = fx + cw * 0.4f;
    ff_text(r, text, tx, y, fg, cw, inner_w - cw * 0.8f);
    if (focused) {
        /* Columns, not characters: a wide (CJK) character takes two. */
        f32 cxp = tx + (f32)utf8_display_cols((const u8 *)text, strlen(text)) * cw;
        if (cxp < fx + inner_w) {
            renderer_draw_rect(r, cxp, y, 2.0f, ch, caret);
            return cxp;
        }
    }
    return -1.0f;
}

/* The Find bar has the keyboard while it is open: its caret, not the editor's
 * under it, is where an input method composes (FileBrowser.caret_*). */
static void ff_claim_caret(FileBrowser *fb, f32 cx, f32 y, f32 cw, f32 ch) {
    if (cx < 0.0f) return;
    fb->caret_drawn = true;
    fb->caret_x = cx;
    fb->caret_y = y;
    fb->caret_cw = cw;
    fb->caret_ch = ch;
}

void fb_ac_render(FileBrowser *fb, Renderer *r, f32 dpi, const Theme *theme, f32 opacity) {
    if (!fb || !fb->ac_active || fb->ac_count == 0 || !fb->ac_items) return;
    f32 cw = 8.0f * dpi, ch = 16.0f * dpi;
    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    renderer_set_ui_scale(r, cw, ch);

    ChromePalette cp = chrome_palette_for(theme);
    Color fg     = theme ? theme->fg : (Color){0.86f,0.86f,0.90f,1.0f};
    Color accent = theme_ui_accent(theme);

    u32 show = fb->ac_count < 8 ? fb->ac_count : 8;   /* cap visible rows */
    f32 rowh = ch + 4.0f * dpi;
    f32 maxw = 6.0f * cw;
    for (u32 i = 0; i < show; i++) {
        u32 cells = 0;
        for (const u8 *q = (const u8 *)fb->ac_items[i]; *q; q++) if ((*q & 0xC0) != 0x80) cells++;
        f32 tw = (f32)cells * cw;
        if (tw > maxw) maxw = tw;
    }
    f32 prefix = fb->ac_kind == 0 ? 2.0f * cw : 1.0f * cw;   /* [[ or # glyph */
    f32 panelw = maxw + prefix + 2.0f * cw;
    f32 panelh = (f32)show * rowh + 4.0f * dpi;
    f32 px = fb->caret_px_x;
    f32 py = fb->caret_px_y + fb->caret_px_h + 2.0f * dpi;

    Color panel = cp.surface_raised; panel.a = 0.99f * opacity;
    renderer_draw_rrect_simple(r, px, py, panelw, panelh, panel, 6.0f * dpi);
    Color border = cp.divider_strong; border.a *= opacity;
    renderer_draw_rect(r, px, py + panelh, panelw, 1.0f, border);
    renderer_flush_rects(r);

    for (u32 i = 0; i < show; i++) {
        f32 ry = py + 2.0f * dpi + (f32)i * rowh;
        bool sel = ((i32)i == fb->ac_sel);
        if (sel) {
            Color hl = accent; hl.a = 0.22f * opacity;
            renderer_draw_rect(r, px + 2.0f * dpi, ry, panelw - 4.0f * dpi, rowh, hl);
        }
        Color rowfg = sel ? accent : fg;
        ff_text(r, fb->ac_kind == 0 ? "[[" : "#", px + cw * 0.5f, ry + 2.0f * dpi, rowfg, cw, prefix + cw);
        ff_text(r, fb->ac_items[i], px + cw * 0.5f + prefix, ry + 2.0f * dpi, rowfg, cw, maxw + cw);
    }
    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r);
}

void fb_find_render(FileBrowser *fb, Renderer *r, f32 x, f32 y, f32 w,
                    f32 dpi, const Theme *theme, f32 opacity) {
    if (!fb || !fb->find_active) return;
    fb_find_sync(fb);                    /* the count shown is this text's */
    f32 cw = 8.0f * dpi, ch = 16.0f * dpi;
    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    renderer_set_ui_scale(r, cw, ch);

    ChromePalette cp = chrome_palette_for(theme);
    Color fg     = theme ? theme->fg : (Color){0.86f, 0.86f, 0.90f, 1.0f};
    Color dim    = { fg.r * 0.55f, fg.g * 0.55f, fg.b * 0.55f, fg.a };
    Color accent = theme_ui_accent(theme);
    Color caret  = theme ? theme->cursor : (Color){0.8f, 0.8f, 0.85f, 0.9f};

    f32 pad   = 10.0f * dpi;
    f32 rowh  = ch + 8.0f * dpi;
    i32 rows  = fb->find_replace_mode ? 2 : 1;
    f32 barw  = w - 2.0f * pad;
    if (barw > 560.0f * dpi) barw = 560.0f * dpi;
    f32 barh  = (f32)rows * rowh + 6.0f * dpi;
    f32 bx    = x + w - pad - barw;          /* right-aligned, like editors */
    f32 by    = y + 6.0f * dpi;

    Color panel = cp.surface_raised; panel.a = 0.98f * opacity;
    renderer_draw_rrect_simple(r, bx, by, barw, barh, panel, 6.0f * dpi);
    Color border = cp.divider_strong; border.a *= opacity;
    renderer_draw_rect(r, bx, by + barh, barw, 1.0f, border);

    f32 row_y = by + 4.0f * dpi + (rowh - ch) * 0.5f;
    f32 fx    = bx + pad;

    /* Right-side status: "n/m", regex error, and [Aa]/[.*] toggle pills. */
    char status[48];
    if (fb->find_regex_error)            snprintf(status, sizeof status, "bad regex");
    else if (fb->find_query_len == 0)    status[0] = '\0';
    else if (fb->find_match_count == 0)  snprintf(status, sizeof status, "0/0");
    /* "+": the list is full and there are more matches than it holds. */
    else snprintf(status, sizeof status, "%d/%u%s", fb->find_current + 1, fb->find_match_count,
                  fb->find_truncated ? "+" : "");
    f32 status_w = (f32)strlen(status) * cw;

    /* toggle pills at the far right of row 1 */
    f32 pill_w = 3.0f * cw;
    f32 pill_gap = 4.0f * dpi;
    f32 rx = bx + barw - pad - pill_w;
    Color pill_on  = accent; pill_on.a = 0.85f * opacity;
    Color pill_off = cp.surface_sunken; pill_off.a = opacity;
    /* [.*] regex */
    renderer_draw_rrect_simple(r, rx, row_y - 2.0f, pill_w, ch + 4.0f,
                               fb->find_regex ? pill_on : pill_off, 4.0f);
    ff_text(r, ".*", rx + cw * 0.5f, row_y, fb->find_regex ? chrome_legible_on(pill_on) : dim, cw, pill_w);
    f32 rx2 = rx - pill_gap - pill_w;
    /* [Aa] case */
    renderer_draw_rrect_simple(r, rx2, row_y - 2.0f, pill_w, ch + 4.0f,
                               fb->find_case ? pill_on : pill_off, 4.0f);
    ff_text(r, "Aa", rx2 + cw * 0.5f, row_y, fb->find_case ? chrome_legible_on(pill_on) : dim, cw, pill_w);

    /* status left of the pills */
    Color stc = fb->find_regex_error ? (Color){0.92f, 0.45f, 0.40f, opacity} : dim;
    if (status[0]) ff_text(r, status, rx2 - pill_gap - status_w, row_y, stc, cw, status_w + cw);

    /* query field fills the remaining width on row 1 */
    f32 field_w = (rx2 - pill_gap - status_w - cw) - fx;
    ff_claim_caret(fb, ff_field(r, "Find", fb->find_query, !fb->find_focus_replace,
                                fx, row_y, field_w, cw, ch, fg, dim, caret, &cp),
                   row_y, cw, ch);

    if (fb->find_replace_mode) {
        f32 row2_y = row_y + rowh;
        ff_claim_caret(fb, ff_field(r, "Repl", fb->find_replace, fb->find_focus_replace,
                                    fx, row2_y, barw - 2.0f * pad, cw, ch, fg, dim, caret, &cp),
                       row2_y, cw, ch);
    }

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r);
}
