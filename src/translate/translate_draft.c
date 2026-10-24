#include "translate/translate_draft.h"
#include "core/utf8.h"
#include <string.h>

static i32 units(const char *s, i32 off, i32 len) {
    TranslateSeg sg = {TRANSLATE_SEG_TEXT, off, len, 0};
    return translate_segment_units(s, &sg);
}

static bool safe_text(const char *s, i32 len, bool source) {
    for (i32 i = 0; i < len;) {
        u32 cp;
        u32 n = utf8_decode((const u8 *)s + i, (usize)(len - i), &cp);
        if (!n || cp == 0 || cp == 127 || (cp >= 0x80 && cp <= 0x9f) ||
            (cp < 32 && !(source && (cp == '\n' || cp == '\r' || cp == '\t'))))
            return false;
        i += (i32)n;
    }
    return true;
}

bool translate_draft_init(TranslateDraft *d, const char *source,
                          const TranslateSeg *segs, i32 count) {
    if (!d) return false;
    memset(d, 0, sizeof *d);
    d->failed = true;
    d->current = -1;
    if (!source || !segs || count <= 0 || count > TRANSLATE_MAX_SEGS) return false;
    usize n = strlen(source);
    if (!n || n >= sizeof d->source || !safe_text(source, (i32)n, true)) return false;
    i32 end = 0;
    for (i32 k = 0; k < count; k++) {
        const TranslateSeg *sg = &segs[k];
        if (sg->off < end || sg->len <= 0 || sg->off > (i32)n - sg->len ||
            sg->units <= 0 || sg->units > sg->len || sg->kind < TRANSLATE_SEG_TEXT ||
            sg->kind > TRANSLATE_SEG_BREAK) return false;
        /* Gaps are only allowed at the outer whitespace of the source. */
        if (k && sg->off != end) return false;
        if (!k) for (i32 i = 0; i < sg->off; i++)
            if ((unsigned char)source[i] > ' ') return false;
        end = sg->off + sg->len;
        d->off[k] = -1;
    }
    for (i32 i = end; i < (i32)n; i++)
        if ((unsigned char)source[i] > ' ') return false;
    memcpy(d->source, source, n + 1);
    memcpy(d->segs, segs, (usize)count * sizeof *segs);
    d->count = count;
    d->failed = false;
    return true;
}

bool translate_draft_append(TranslateDraft *d, i32 seg, const char *s, i32 len) {
    if (!d || d->failed) return false;
    if (!s || len < 0 || seg < 0 || seg >= d->count || seg < d->current ||
        d->segs[seg].kind != TRANSLATE_SEG_TEXT) { d->failed = true; return false; }
    if (d->current != seg) {
        if (d->pending_len) { d->failed = true; return false; }
        d->current = seg;
        d->off[seg] = d->used;
        d->pending_space = false;
    }
    for (i32 i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        d->pending_utf8[d->pending_len++] = c;
        u8 first = d->pending_utf8[0];
        i32 width = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
                    first >= 0xe0 && first <= 0xef ? 3 :
                    first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (!width || (d->pending_len > 1 && !utf8_is_continuation(c))) {
            d->failed = true; return false;
        }
        if (d->pending_len < width) continue;
        u32 cp;
        if (!utf8_decode(d->pending_utf8, (usize)width, &cp) || cp == 0 || cp == 127 ||
            (cp >= 0x80 && cp <= 0x9f) ||
            (cp < 32 && cp != '\n' && cp != '\r' && cp != '\t')) {
            d->failed = true; return false;
        }
        d->pending_len = 0;
        if (cp <= ' ') { if (d->len[seg]) d->pending_space = true; continue; }
        if (!d->len[seg] && utf8_is_joiner(cp)) { d->failed = true; return false; }
        i32 need = width + (d->pending_space ? 1 : 0);
        if (d->used + need >= (i32)sizeof d->output) { d->failed = true; return false; }
        if (d->pending_space) {
            d->output[d->used++] = ' ';
            d->len[seg]++;
            d->pending_space = false;
        }
        memcpy(d->output + d->used, d->pending_utf8, (usize)width);
        d->used += width;
        d->len[seg] += width;
    }
    d->output[d->used] = '\0';
    return true;
}

static i32 tail_units(const TranslateDraft *d) {
    i32 last = d->segs[d->count - 1].off + d->segs[d->count - 1].len;
    return units(d->source, last, (i32)strlen(d->source) - last);
}

/* The first edit of a plan: the caret to the end of the input, wherever it
 * stands. By the line where the editor has a key for it, by the character
 * where it has not. */
static TranslateEdit to_end(const TranslateDraft *d, i32 total) {
    if (!d->line_end_key) return (TranslateEdit){TRANSLATE_EDIT_RIGHT, total, 0};
    i32 lines = 1;
    for (const char *c = d->source; *c; c++) lines += *c == '\n';
    return (TranslateEdit){TRANSLATE_EDIT_TO_END, lines + TRANSLATE_END_SLACK, 0};
}

static bool valid(const TranslateDraft *d) {
    return d && d->count > 0 && d->count <= TRANSLATE_MAX_SEGS;
}

/* A dangling ZWJ could join the next protected character. Keep it buffered
 * until its following character arrives; an EOS here fails finalization. */
static i32 available(const TranslateDraft *d, i32 seg) {
    i32 n = d->len[seg];
    if (n >= 3 && memcmp(d->output + d->off[seg] + n - 3, "\xe2\x80\x8d", 3) == 0) n -= 3;
    return n;
}

i32 translate_draft_begin_plan(const TranslateDraft *d, const char *current_source,
                                TranslateEdit *edits, i32 cap) {
    if (!valid(d) || d->failed || d->begun || !current_source || !edits ||
        strcmp(d->source, current_source) != 0 || cap < d->count + 2) return 0;
    for (i32 k = 0; k < d->count; k++) {
        /* An attachment is never retyped, and a line break is never stepped
         * over: each has the one kind of draft it can be in. */
        if (d->segs[k].kind == (d->whole ? TRANSLATE_SEG_CHIP : TRANSLATE_SEG_BREAK)) return 0;
    }
    if (d->whole) {
        i32 all = units(d->source, 0, (i32)strlen(d->source));
        if (all <= 0) return 0;
        i32 keys = all + all / 8 + TRANSLATE_WHOLE_SLACK;
        if (d->line_end_key) {
            /* To the end, a few forward deletes for what may still stand
             * after it, and then Backspace — one byte a key — for all of it. */
            edits[0] = to_end(d, 0);
            edits[1] = (TranslateEdit){TRANSLATE_EDIT_DELETE, TRANSLATE_END_DELETES, 0};
            edits[2] = (TranslateEdit){TRANSLATE_EDIT_ERASE, keys, 0};
            return 3;
        }
        edits[0] = (TranslateEdit){TRANSLATE_EDIT_ERASE, keys, 0};
        edits[1] = (TranslateEdit){TRANSLATE_EDIT_DELETE, keys, 0};
        return 2;
    }
    i32 tail = tail_units(d);
    i32 total = units(d->source, 0, d->segs[0].off) + tail;
    for (i32 k = 0; k < d->count; k++) total += d->segs[k].units;
    i32 n = 0;
    edits[n++] = to_end(d, total);
    if (tail) edits[n++] = (TranslateEdit){TRANSLATE_EDIT_LEFT, tail, 0};
    for (i32 k = d->count - 1; k >= 0; k--)
        edits[n++] = (TranslateEdit){d->segs[k].kind == TRANSLATE_SEG_TEXT ?
            TRANSLATE_EDIT_ERASE : TRANSLATE_EDIT_LEFT, d->segs[k].units, 0};
    return n;
}

bool translate_draft_finish_segment(const TranslateDraft *d, i32 seg) {
    return valid(d) && !d->failed && !(d->current == seg && d->pending_len) &&
           seg >= 0 && seg < d->count &&
           d->segs[seg].kind == TRANSLATE_SEG_TEXT && d->len[seg] > 0 &&
           d->sent[seg] == d->len[seg];
}

i32 translate_draft_delta_plan(const TranslateDraft *d, i32 seg,
                                TranslateEdit *edits, i32 cap) {
    if (!valid(d) || d->failed || !d->begun || d->complete || !edits ||
        seg < d->cursor_seg || seg > d->count || cap < d->count + 2) return -1;
    if (seg == d->count) {
        for (i32 k = 0; k < d->count; k++)
            if (d->segs[k].kind == TRANSLATE_SEG_TEXT &&
                !translate_draft_finish_segment(d, k)) return -1;
    } else if (d->segs[seg].kind != TRANSLATE_SEG_TEXT) return -1;
    i32 n = 0;
    for (i32 k = d->cursor_seg; k < seg; k++) {
        if (d->segs[k].kind == TRANSLATE_SEG_TEXT) {
            if (!translate_draft_finish_segment(d, k)) return -1;
        } else if (!d->whole) {
            edits[n++] = (TranslateEdit){TRANSLATE_EDIT_RIGHT, d->segs[k].units, 0};
        } else if (d->segs[k].kind == TRANSLATE_SEG_BREAK) {
            /* The input was emptied: what stood between two lines is typed
             * back, its line breaks as the editor's own key. */
            edits[n++] = (TranslateEdit){TRANSLATE_EDIT_NEWLINE, d->segs[k].units, 0};
        } else {
            edits[n++] = (TranslateEdit){TRANSLATE_EDIT_SOURCE, d->segs[k].len, d->segs[k].off};
        }
    }
    if (seg == d->count) {
        /* Nothing stands after the text of an input that was emptied. */
        i32 tail = d->whole ? 0 : tail_units(d);
        if (tail) edits[n++] = (TranslateEdit){TRANSLATE_EDIT_RIGHT, tail, 0};
    } else if (available(d, seg) > d->sent[seg]) {
        edits[n++] = (TranslateEdit){TRANSLATE_EDIT_TEXT,
            available(d, seg) - d->sent[seg], d->off[seg] + d->sent[seg]};
    }
    return n;
}

void translate_draft_delta_applied(TranslateDraft *d, i32 seg) {
    d->cursor_seg = seg;
    if (seg == d->count) d->complete = true;
    else d->sent[seg] = available(d, seg);
}

i32 translate_draft_restore_plan(const TranslateDraft *d, TranslateEdit *edits, i32 cap) {
    if (!valid(d) || !d->begun || !edits || cap < 2 * d->count + 3) return 0;
    if (d->whole) {
        /* The input holds what was typed into it and nothing else: that
         * goes — counted generously, every key past the start does nothing —
         * and the prompt comes back as it was captured. */
        i32 typed = 0;
        for (i32 k = 0; k < d->count; k++) {
            i32 one = d->segs[k].kind != TRANSLATE_SEG_TEXT ? d->segs[k].units :
                      d->sent[k] ? units(d->output, d->off[k], d->sent[k]) : 0;
            if (one < 0) return 0;
            typed += one;
        }
        i32 n = 0;
        edits[n++] = (TranslateEdit){TRANSLATE_EDIT_ERASE, typed + TRANSLATE_WHOLE_SLACK, 0};
        edits[n++] = (TranslateEdit){TRANSLATE_EDIT_DELETE, TRANSLATE_END_DELETES, 0};
        if (d->count == 1) {
            edits[n++] = (TranslateEdit){TRANSLATE_EDIT_SOURCE, (i32)strlen(d->source), 0};
            return n;
        }
        /* Taken apart at its line breaks: put back a line at a time, with
         * the editor's own key between them. All of it pasted at once would
         * be folded into a label by an editor that folds a paste of several
         * lines (Claude Code: four and more). */
        i32 from = d->segs[0].off, end = d->segs[d->count - 1].off + d->segs[d->count - 1].len;
        for (i32 i = from; i <= end;) {
            i32 eol = i;
            while (eol < end && d->source[eol] != '\n') eol++;
            if (eol > i) {
                if (n + 2 > cap) return 0;
                edits[n++] = (TranslateEdit){TRANSLATE_EDIT_SOURCE, eol - i, i};
            }
            if (eol < end) {
                if (edits[n - 1].kind == TRANSLATE_EDIT_NEWLINE) edits[n - 1].count++;
                else {
                    if (n + 1 > cap) return 0;
                    edits[n++] = (TranslateEdit){TRANSLATE_EDIT_NEWLINE, 1, 0};
                }
            }
            i = eol + 1;
        }
        return n;
    }
    i32 tail = tail_units(d);
    i32 total = units(d->source, 0, d->segs[0].off) + tail;
    i32 sizes[TRANSLATE_MAX_SEGS];
    for (i32 k = 0; k < d->count; k++) {
        sizes[k] = d->segs[k].kind != TRANSLATE_SEG_TEXT ? d->segs[k].units :
                   d->sent[k] ? units(d->output, d->off[k], d->sent[k]) : 0;
        if (sizes[k] < 0) return 0;
        total += sizes[k];
    }
    i32 n = 0;
    edits[n++] = to_end(d, total);
    if (tail) edits[n++] = (TranslateEdit){TRANSLATE_EDIT_LEFT, tail, 0};
    for (i32 k = d->count - 1; k >= 0; k--)
        edits[n++] = (TranslateEdit){d->segs[k].kind == TRANSLATE_SEG_TEXT ?
            TRANSLATE_EDIT_ERASE : TRANSLATE_EDIT_LEFT, sizes[k], 0};
    for (i32 k = 0; k < d->count; k++)
        edits[n++] = d->segs[k].kind == TRANSLATE_SEG_TEXT ?
            (TranslateEdit){TRANSLATE_EDIT_SOURCE, d->segs[k].len, d->segs[k].off} :
            (TranslateEdit){TRANSLATE_EDIT_RIGHT, d->segs[k].units, 0};
    if (tail) edits[n++] = (TranslateEdit){TRANSLATE_EDIT_RIGHT, tail, 0};
    return n;
}

bool translate_draft_visible(const TranslateDraft *d, char *out, usize cap) {
    if (!valid(d) || !out || !cap) return false;
    if (d->whole) {
        /* The translations, with what was typed back between them. */
        usize used = 0;
        for (i32 k = 0; k < d->count; k++) {
            const TranslateSeg *sg = &d->segs[k];
            if (sg->kind == TRANSLATE_SEG_BREAK) {
                if ((usize)sg->units >= cap - used) return false;
                memset(out + used, '\n', (usize)sg->units);
                used += (usize)sg->units;
                continue;
            }
            bool text = sg->kind == TRANSLATE_SEG_TEXT;
            usize n = (usize)(text ? d->sent[k] : sg->len);
            const char *s = text ? d->output + (d->off[k] >= 0 ? d->off[k] : 0) : d->source + sg->off;
            if (n >= cap - used) return false;
            memcpy(out + used, s, n);
            used += n;
        }
        out[used] = '\0';
        return true;
    }
    usize used = 0;
    i32 end = 0;
    for (i32 k = 0; k <= d->count; k++) {
        i32 next = k < d->count ? d->segs[k].off : (i32)strlen(d->source);
        usize gap = (usize)(next - end);
        if (gap >= cap - used) return false;
        memcpy(out + used, d->source + end, gap); used += gap;
        if (k == d->count) break;
        bool text = d->segs[k].kind == TRANSLATE_SEG_TEXT;
        i32 n = text ? d->sent[k] : d->segs[k].len;
        const char *s = text ? d->output + (d->off[k] >= 0 ? d->off[k] : 0) :
                              d->source + d->segs[k].off;
        if ((usize)n >= cap - used) return false;
        memcpy(out + used, s, (usize)n); used += (usize)n;
        end = d->segs[k].off + d->segs[k].len;
    }
    out[used] = '\0';
    return true;
}
