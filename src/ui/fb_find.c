/*
 * Liu — in-document Find / Replace for the file-browser viewer.
 *
 * Operates over fb->view_content as a flat byte buffer, so the same match set
 * drives every viewer mode: matches are byte ranges that map to glyph src_off
 * in the rendered markdown view and to raw line offsets in the live-preview /
 * code editor. Plain substring (optionally case-folded) or POSIX extended
 * regex. Replace is gated on an editable buffer (editor_mode / raw mode) so we
 * never splice bytes out from under a parsed AST in rendered read mode.
 */
#include "ui/filebrowser.h"
#include "ui/markdown/md_render.h"   /* full MdGlyphRect + MD_GLYPH_NO_SRC */
#include "ui/chrome_palette.h"
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
        FbFindMatch *n = realloc(fb->find_matches, ncap * sizeof(*n));
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

void fb_find_recompute(FileBrowser *fb) {
    if (!fb) return;
    fb->find_match_count = 0;
    fb->find_regex_error = false;
    if (!fb->view_content || fb->find_query_len == 0) {
        fb->find_current = -1;
        return;
    }
    const char *content = fb->view_content;
    usize n = fb->view_size;

#ifndef PLATFORM_WIN32
    if (fb->find_regex) {
        regex_t re;
        int cflags = REG_EXTENDED | REG_NEWLINE | (fb->find_case ? 0 : REG_ICASE);
        if (regcomp(&re, fb->find_query, cflags) != 0) {
            fb->find_regex_error = true;
            fb->find_current = -1;
            return;
        }
        usize pos = 0;
        regmatch_t m;
        while (pos <= n) {
            int eflags = (pos > 0 && content[pos - 1] != '\n') ? REG_NOTBOL : 0;
            if (regexec(&re, content + pos, 1, &m, eflags) != 0) break;
            u32 ms = (u32)(pos + m.rm_so);
            u32 me = (u32)(pos + m.rm_eo);
            if (!ff_push_match(fb, ms, me)) break;
            /* Advance past the match; guard zero-length matches (e.g. "a*"). */
            pos = (m.rm_eo > m.rm_so) ? me : me + 1;
        }
        regfree(&re);
    } else
#endif
    {
        u32 qlen = fb->find_query_len;
        bool fold = !fb->find_case;
        for (usize i = 0; i + qlen <= n; ) {
            if (ff_eq(content + i, fb->find_query, qlen, fold)) {
                if (!ff_push_match(fb, (u32)i, (u32)(i + qlen))) break;
                i += qlen;
            } else {
                i++;
            }
        }
    }

    if (fb->find_match_count == 0) fb->find_current = -1;
    else if (fb->find_current < 0 || (u32)fb->find_current >= fb->find_match_count)
        fb->find_current = 0;
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
    } else {
        /* Line-based scroll (raw markdown / code / text editors). */
        fb->view_scroll = line > 3 ? line - 3 : 0;
    }
}

void fb_find_next(FileBrowser *fb) {
    if (!fb || fb->find_match_count == 0) return;
    fb->find_current = (fb->find_current + 1) % (i32)fb->find_match_count;
    ff_reveal_current(fb);
}

void fb_find_prev(FileBrowser *fb) {
    if (!fb || fb->find_match_count == 0) return;
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
    if (fb->find_current < 0 || (u32)fb->find_current >= fb->find_match_count) return false;
    FbFindMatch m = fb->find_matches[fb->find_current];
    fb_editor_push_undo(fb);
    if (!fb_editor_splice(fb, m.start, m.end - m.start,
                          fb->find_replace, fb->find_replace_len))
        return false;
    fb_editor_push_undo(fb);
    fb_find_recompute(fb);
    /* Keep the cursor near where the edit landed. */
    i32 line, col; ff_offset_to_linecol(fb, m.start, &line, &col);
    fb->cursor_line = line; fb->cursor_col = col + (i32)fb->find_replace_len;
    if (fb->find_match_count > 0) ff_reveal_current(fb);
    return true;
}

void fb_find_replace_all(FileBrowser *fb) {
    if (!ff_replace_ok(fb) || fb->find_match_count == 0) return;
    fb_editor_push_undo(fb);
    /* Splice from the LAST match to the first so earlier offsets stay valid. */
    for (i32 i = (i32)fb->find_match_count - 1; i >= 0; i--) {
        FbFindMatch m = fb->find_matches[i];
        fb_editor_splice(fb, m.start, m.end - m.start,
                         fb->find_replace, fb->find_replace_len);
    }
    fb_editor_push_undo(fb);
    fb->find_current = 0;
    fb_find_recompute(fb);
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
 * ------------------------------------------------------------------------- */

void fb_find_draw_matches(FileBrowser *fb, Renderer *r, const Theme *theme) {
    if (!fb || !fb->find_active || fb->find_match_count == 0 || !fb->md_glyph_rects)
        return;
    Color all = theme ? theme->ansi[3] : (Color){0.92f, 0.72f, 0.33f, 1.0f};
    Color cur = theme_ui_accent(theme);
    Color all_bg = { all.r, all.g, all.b, 0.28f };
    Color cur_bg = { cur.r, cur.g, cur.b, 0.45f };
    for (u32 i = 0; i < fb->md_glyph_count; i++) {
        MdGlyphRect *g = &fb->md_glyph_rects[i];
        if (g->src_off == MD_GLYPH_NO_SRC) continue;
        /* Which match (if any) contains this glyph? */
        for (u32 mi = 0; mi < fb->find_match_count; mi++) {
            FbFindMatch m = fb->find_matches[mi];
            if (g->src_off >= m.start && g->src_off < m.end) {
                bool is_cur = ((i32)mi == fb->find_current);
                renderer_draw_rect(r, g->x, g->y, g->w, g->h, is_cur ? cur_bg : all_bg);
                break;
            }
        }
    }
}

void fb_find_draw_line(const FileBrowser *fb, Renderer *r, usize line_off,
                       i32 line_len, f32 lx, f32 cy, f32 lcw, f32 lch,
                       const Theme *theme) {
    if (!fb || !fb->find_active || fb->find_match_count == 0) return;
    Color all = theme ? theme->ansi[3] : (Color){0.92f, 0.72f, 0.33f, 1.0f};
    Color cur = theme_ui_accent(theme);
    Color all_bg = { all.r, all.g, all.b, 0.28f };
    Color cur_bg = { cur.r, cur.g, cur.b, 0.45f };
    u32 ls = (u32)line_off, le = (u32)line_off + (u32)line_len;
    const char *lp = fb->view_content + line_off;
    for (u32 mi = 0; mi < fb->find_match_count; mi++) {
        FbFindMatch m = fb->find_matches[mi];
        if (m.end <= ls || m.start >= le) continue;        /* no overlap with line */
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
static void ff_field(Renderer *r, const char *label, const char *text, bool focused,
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
        u32 cps = 0; for (const u8 *q = (const u8 *)text; *q; q++) if ((*q & 0xC0) != 0x80) cps++;
        f32 cxp = tx + (f32)cps * cw;
        if (cxp < fx + inner_w) renderer_draw_rect(r, cxp, y, 2.0f, ch, caret);
    }
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
    else snprintf(status, sizeof status, "%d/%u", fb->find_current + 1, fb->find_match_count);
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
    ff_field(r, "Find", fb->find_query, !fb->find_focus_replace,
             fx, row_y, field_w, cw, ch, fg, dim, caret, &cp);

    if (fb->find_replace_mode) {
        f32 row2_y = row_y + rowh;
        ff_field(r, "Repl", fb->find_replace, fb->find_focus_replace,
                 fx, row2_y, barw - 2.0f * pad, cw, ch, fg, dim, caret, &cp);
    }

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r);
}
