/*
 * notice_wrap — a notice's words on as many short lines as they need, up to a
 * few.
 *
 * The explorer's status card sits at the foot of the sidebar, about 22 columns
 * wide at its default width, and the reason a message carries ("Not pasted:
 * local.txt: something with that name is already here") is the one thing the
 * card is for, so it must not be cut off after two lines. The card takes up to
 * NOTICE_WRAP_MAX lines, its height follows the lines it needs, and only a
 * message longer than that ends in an ellipsis.
 *
 * A line breaks at the last space that fits (inside the word when there is
 * none), the spaces at the break are dropped, and the last line the card has
 * room for is clipped with an ellipsis when more is left (graphemes, never half
 * an emoji; columns are display columns, a wide character counts two).
 * notice_wrap_keep_end first shortens a word longer than a line in its middle,
 * so a long file name does not push the reason off the card.
 *
 * Pure: no display is needed.
 */
#ifndef UI_NOTICE_WRAP_H
#define UI_NOTICE_WRAP_H

#include "core/types.h"
#include "core/utf8.h"
#include "ui/sidebar_agent_row.h"   /* sba_clip_ellipsis */

#include <stdio.h>
#include <string.h>

#define NOTICE_WRAP_MAX      4      /* lines the card has room for */
#define NOTICE_WRAP_LINE_CAP 288    /* bytes a line holds: a 255-byte message and an ellipsis */

typedef struct {
    char line[NOTICE_WRAP_MAX][NOTICE_WRAP_LINE_CAP];
    i32  count;
} NoticeWrap;

/* `s` on at most `max_lines` (1..NOTICE_WRAP_MAX) lines of `cols` display
 * columns (4 at the least). Returns the number of lines, 0 for no text. */
static inline i32 notice_wrap(const char *s, i32 cols, i32 max_lines, NoticeWrap *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!s || !*s) return 0;
    if (cols < 4) cols = 4;
    if (max_lines < 1) max_lines = 1;
    if (max_lines > NOTICE_WRAP_MAX) max_lines = NOTICE_WRAP_MAX;

    const char *rest = s;
    while (out->count < max_lines) {
        char *line = out->line[out->count];
        usize len = strlen(rest);
        if ((i32)utf8_display_cols((const u8 *)rest, len) <= cols) {
            snprintf(line, NOTICE_WRAP_LINE_CAP, "%s", rest);
            out->count++;
            break;
        }
        if (out->count + 1 == max_lines) {            /* the last line there is room for */
            sba_clip_ellipsis(rest, cols, line, NOTICE_WRAP_LINE_CAP);
            out->count++;
            break;
        }
        usize off = 0, last_space = 0;
        i32 used = 0;
        while (off < len) {
            u32 cp = 0; i32 gc = 1;
            u32 gb = utf8_next_grapheme((const u8 *)rest + off, len - off, &cp, &gc);
            if (gb == 0) break;
            if (gc < 0) gc = 0;
            if (used + gc > cols) break;
            if (cp == ' ') last_space = off;
            used += gc; off += gb;
        }
        usize cut = last_space > 0 ? last_space : off;
        if (cut == 0) {                                /* always move on, by whole characters */
            cut = 1;
            while (((u8)rest[cut] & 0xC0) == 0x80) cut++;
        }
        usize n = cut < NOTICE_WRAP_LINE_CAP - 1 ? cut : NOTICE_WRAP_LINE_CAP - 1;
        memcpy(line, rest, n);
        line[n] = '\0';
        out->count++;
        rest += cut;
        while (*rest == ' ') rest++;
        if (!*rest) break;
    }
    return out->count;
}

/* Whether the lines hold every non-space byte of `s`, in order: nothing was
 * clipped. */
static inline bool notice_wrap_whole(const NoticeWrap *w, const char *s) {
    const char *p = s;
    for (i32 i = 0; i < w->count; i++) {
        for (const char *q = w->line[i]; *q; q++) {
            if (*q == ' ') continue;
            while (*p == ' ') p++;
            if (*p != *q) return false;
            p++;
        }
    }
    while (*p == ' ') p++;
    return *p == '\0';
}

/* `w[n]` in at most `room` display columns: as it is when it fits, else its
 * head, an ellipsis and its tail, in whole graphemes. Returns the bytes
 * written to `out` (NUL-terminated). */
static inline usize notice_wrap_elide(const char *w, usize n, i32 room, char *out, usize cap) {
    if (!out || cap == 0) return 0;
    u16 off[NOTICE_WRAP_LINE_CAP + 1];
    u8  wid[NOTICE_WRAP_LINE_CAP];
    i32 k = 0, total = 0;
    usize o = 0;
    while (o < n && k < NOTICE_WRAP_LINE_CAP) {
        u32 cp = 0; i32 gc = 1;
        u32 gb = utf8_next_grapheme((const u8 *)w + o, n - o, &cp, &gc);
        if (gb == 0) break;
        if (gc < 0) gc = 0;
        off[k] = (u16)o; wid[k] = (u8)gc;
        total += gc; o += gb; k++;
    }
    off[k] = (u16)o;
    usize m;
    if (total <= room || room < 3 || o < n || n > 0xFFFFu) {
        m = n < cap - 1 ? n : cap - 1;
        memcpy(out, w, m);
        out[m] = '\0';
        return m;
    }
    i32 head_room = (room - 1) / 2, tail_room = room - 1 - head_room;
    i32 h = 0, used = 0;
    while (h < k && used + wid[h] <= head_room) used += wid[h++];
    i32 t = k;
    used = 0;
    while (t > h && used + wid[t - 1] <= tail_room) used += wid[--t];
    usize hb = off[h], tb = (usize)off[k] - off[t];
    if (hb + 3 + tb >= cap) {                      /* no room for the shortened word: as it is */
        m = n < cap - 1 ? n : cap - 1;
        memcpy(out, w, m);
        out[m] = '\0';
        return m;
    }
    memcpy(out, w, hb);
    memcpy(out + hb, "\xE2\x80\xA6", 3);
    memcpy(out + hb + 3, w + off[t], tb);
    out[hb + 3 + tb] = '\0';
    return hb + 3 + tb;
}

/* notice_wrap for a message whose end carries its point (a refusal's reason
 * after a file's name). When the words do not fit on `max_lines`, a word longer
 * than a line first gives its middle to an ellipsis, so the end stays on the
 * card; only what is still too long is clipped as notice_wrap clips it. A
 * message that fits is wrapped exactly as notice_wrap wraps it. */
static inline i32 notice_wrap_keep_end(const char *s, i32 cols, i32 max_lines, NoticeWrap *out) {
    i32 n = notice_wrap(s, cols, max_lines, out);
    if (n == 0 || notice_wrap_whole(out, s)) return n;
    if (cols < 4) cols = 4;
    char buf[2 * NOTICE_WRAP_LINE_CAP];
    usize o = 0;
    bool changed = false;
    while (*s && o + 1 < sizeof buf) {
        if (*s == ' ') { buf[o++] = *s++; continue; }
        const char *e = s;
        while (*e && *e != ' ') e++;
        usize wn = (usize)(e - s);
        usize m = notice_wrap_elide(s, wn, cols, buf + o, sizeof buf - o);
        if (m != wn || memcmp(buf + o, s, m) != 0) changed = true;
        o += m;
        s = e;
    }
    buf[o] = '\0';
    return changed ? notice_wrap(buf, cols, max_lines, out) : n;
}

#endif /* UI_NOTICE_WRAP_H */
