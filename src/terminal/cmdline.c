/*
 * Liu — shell command-line geometry (see cmdline.h for the contract).
 *
 * Everything here walks linear cell positions (row * cols + col) through a
 * `Walk`, which knows the body columns and where each row's text really ends,
 * and converts to *character* offsets on the way out: wide glyphs own two cells
 * but count once, exactly like the shell's own cursor arithmetic.
 *
 * The Walk is what makes multi-row input work. A shell's soft-wrapped
 * line continues at the screen edge, so every cell between two rows is text.
 * An agent CLI's input block does not: the row ends where the app stopped
 * painting, and the next row resumes after its border and indent. Filtering
 * positions through the Walk lets both shapes share one flat traversal.
 */
#include "terminal/cmdline.h"
#include "platform/platform.h"
#include "core/utf8.h"
#include <stdlib.h>
#include <string.h>

/* How far into a line the scan looks for the prompt's end. Long enough
 * for a multi-segment powerline prompt, short enough that a pasted
 * paragraph can't make the scan wander. */
#define SIGIL_SCAN_CHARS 240

/* Rows a drawn input block may span. Well past any prompt a user types by
 * hand — agents fold a big paste into a "[Pasted text #1 +N lines]" chip
 * rather than painting it — and it bounds the scan on a screen where every
 * row happens to carry text. */
#define BODY_MAX_ROWS 64

/* =========================================================================
 * Cells
 * ========================================================================= */

static const Cell *pos_cell(Terminal *t, i32 pos) {
    if (!t || t->cols <= 0 || pos < 0) return NULL;
    return terminal_cell_at(t, pos % t->cols, pos / t->cols);
}

static bool pos_is_dummy(Terminal *t, i32 pos) {
    const Cell *c = pos_cell(t, pos);
    return c && (c->attr.flags & ATTR_WDUMMY) != 0;
}

static u32 pos_cp(Terminal *t, i32 pos) {
    const Cell *c = pos_cell(t, pos);
    if (!c) return 0;
    return c->codepoint ? c->codepoint : ' ';
}

/* Cells that read as empty. The non-breaking spaces matter: Claude Code
 * separates its `❯` from the input with U+00A0, so treating only ASCII space
 * as blank makes the sigil fail to register and swallows the prompt itself
 * into the selection. */
static bool is_blank_cp(u32 cp) {
    return cp <= ' ' || cp == 0x00A0 || cp == 0x2007 || cp == 0x202F;
}

/* Box-drawing / block characters an agent frames its input with. Treated as
 * padding, and stepped over when deciding whether a sigil opens a row — `│ > `
 * and `┃  ` have to read the same as a bare prompt.
 *
 * '|' is only a border inside a drawn UI: at a shell prompt it is a pipe. */
static bool is_border_cp(u32 cp, bool boxed) {
    if (cp >= 0x2500 && cp <= 0x259F) return true;   /* box drawing + blocks */
    return boxed && cp == '|';
}

static bool is_padding_cp(u32 cp, bool boxed) {
    return is_blank_cp(cp) || is_border_cp(cp, boxed);
}

/* =========================================================================
 * Row shape
 * ========================================================================= */

/* First column of `row` holding something that is neither blank nor border,
 * or `t->cols` when the row has none. */
static i32 row_text_start(Terminal *t, i32 row, bool boxed) {
    i32 base = row * t->cols;
    for (i32 c = 0; c < t->cols; c++) {
        if (!is_padding_cp(pos_cp(t, base + c), boxed)) return c;
    }
    return t->cols;
}

/* One past the last such column, or 0 when the row has none. */
static i32 row_text_end(Terminal *t, i32 row, bool boxed) {
    i32 base = row * t->cols;
    for (i32 c = t->cols - 1; c >= 0; c--) {
        if (!is_padding_cp(pos_cp(t, base + c), boxed)) return c + 1;
    }
    return 0;
}

static bool row_has_text(Terminal *t, i32 row, bool boxed) {
    return row >= 0 && row < t->rows && row_text_start(t, row, boxed) < t->cols;
}

/* Box-drawing cells in a row this long can only be a rule, never typing. */
#define FRAME_RUN_MIN 4

/* Is this row part of a drawn frame rather than input?
 *
 * "Has no text" does not answer it, because agents write *into* their frames:
 * Grok closes its box with `╰──── Grok 4.5 (high) · always-approve ─╯` and Kimi
 * labels its rule `── input ─────`. Both rows carry text and would otherwise be
 * swept into the input block — Grok's would add 70 characters of border and
 * status to a 93-character prompt. A run of box-drawing cells is what actually
 * separates the two. */
static bool row_is_frame(Terminal *t, i32 row) {
    if (row < 0 || row >= t->rows) return false;
    i32 base = row * t->cols, run = 0;
    for (i32 c = 0; c < t->cols; c++) {
        u32 cp = pos_cp(t, base + c);
        if (cp >= 0x2500 && cp <= 0x259F) {
            if (++run >= FRAME_RUN_MIN) return true;
        } else {
            run = 0;
        }
    }
    return false;
}

/* Rows that can continue the input block: text the app painted, not its frame. */
static bool row_is_body(Terminal *t, i32 row, bool boxed) {
    return row_has_text(t, row, boxed) && !row_is_frame(t, row);
}

/* Characters a prompt ends with. The fancy arrows never occur inside a
 * command, so they always count; the ASCII ones are qualified by the caller. */
static bool is_sigil(u32 cp) {
    switch (cp) {
    case '$': case '%': case '#': case '>':
    case 0x00BB:  /* » */ case 0x203A:  /* › */ case 0x25B6:  /* ▶ */
    case 0x2022:  /* • */ case 0x276F:  /* ❯ */ case 0x279C:  /* ➜ */
    case 0x03BB:  /* λ */
        return true;
    default:
        return false;
    }
}

/* '>' is the one sigil that is also everyday shell syntax (`cmd > out`), so it
 * only reads as a prompt when it opens the line. */
static bool sigil_qualifies(u32 cp, bool at_line_start) {
    return cp != '>' || at_line_start;
}

/* A band composer opens its input row with a corner and a rule, "╰─ " (Oh My
 * Pi): box-drawing cells, which row_text_start() steps over as padding — so the
 * row would read as unmarked text. Column of the first cell after the corner's
 * separator, or -1 when the row does not open that way. "╰──…" of a frame is a
 * longer run and has no blank at the third cell. It is that column and no
 * further: an empty input shows a hint far to the right, and skipping blanks
 * to the next text would put the input's start under the hint. */
static i32 row_corner_input_col(Terminal *t, i32 row, bool boxed) {
    i32 base = row * t->cols, c = 0;
    while (c < t->cols && is_blank_cp(pos_cp(t, base + c))) c++;
    if (c + 2 >= t->cols) return -1;
    if (pos_cp(t, base + c) != 0x2570 || pos_cp(t, base + c + 1) != 0x2500) return -1;
    if (!is_blank_cp(pos_cp(t, base + c + 2))) return -1;
    (void)boxed;
    return c + 3 <= t->cols ? c + 3 : t->cols;
}

/* Column where the input begins on a row that opens with a prompt sigil
 * ("❯ …", "› …", "│ > …"), or -1 when the row does not open with one. An
 * agent's input block starts at the first such row, which is how the rows
 * above it — its border, its status line, a completion popup — stay out. */
static i32 row_prompt_input_col(Terminal *t, i32 row, bool boxed) {
    i32 base = row * t->cols;
    i32 corner = row_corner_input_col(t, row, boxed);
    if (corner >= 0) return corner;
    i32 c = row_text_start(t, row, boxed);
    if (c >= t->cols) return -1;
    u32 cp = pos_cp(t, base + c);
    if (!is_sigil(cp) || !sigil_qualifies(cp, true)) return -1;
    /* A sigil must be followed by a separator, else `>>=` or `#include`
     * would read as a prompt. */
    i32 n = c + 1;
    while (n < t->cols && pos_is_dummy(t, base + n)) n++;
    if (n < t->cols && !is_blank_cp(pos_cp(t, base + n))) return -1;
    while (n < t->cols && is_padding_cp(pos_cp(t, base + n), boxed)) n++;
    return n;
}

/* =========================================================================
 * Walk — the filtered traversal every offset conversion runs through
 * ========================================================================= */

typedef struct {
    Terminal *t;
    i32  cols;
    i32  lo, hi;              /* body columns [lo, hi) */
    i32  first_row, last_row;
    bool boxed;
    /* Per-row cut-off. Inside a drawn block a row ends where the app stopped
     * painting plus ONE blank — the space it wrapped on. Without that blank
     * the joined text loses a character per row and the Backspace count comes
     * up short, leaving a fragment of the input behind. */
    i32  row_end[BODY_MAX_ROWS];
} Walk;

static void walk_init(Walk *w, Terminal *t, i32 first_row, i32 last_row,
                      i32 lo, i32 hi, bool boxed) {
    w->t = t;
    w->cols = t->cols;
    w->lo = lo;
    w->hi = hi;
    w->boxed = boxed;
    w->first_row = first_row;
    w->last_row = last_row < first_row ? first_row : last_row;
    if (!boxed) return;
    for (i32 r = first_row; r <= w->last_row; r++) {
        i32 idx = r - first_row;
        if (idx >= BODY_MAX_ROWS) break;
        i32 e = row_text_end(t, r, boxed);
        if (e < lo) e = lo;
        if (e < hi) e++;              /* the wrap blank */
        if (e > hi) e = hi;
        w->row_end[idx] = e;
    }
}

static void walk_from_span(Walk *w, Terminal *t, const CmdSpan *sp) {
    i32 last = sp->end_row;
    if (sp->end_col == 0 && last > sp->start_row) last--;
    walk_init(w, t, sp->start_row, last, sp->body_left, sp->body_right,
              sp->boxed);
}

/* Does this cell carry span text? */
static bool walk_ok(const Walk *w, i32 pos) {
    if (pos < 0) return false;
    i32 row = pos / w->cols, col = pos % w->cols;
    if (col < w->lo || col >= w->hi) return false;
    if (!w->boxed || row >= w->last_row) return true;
    i32 idx = row - w->first_row;
    if (idx < 0 || idx >= BODY_MAX_ROWS) return true;
    return col < w->row_end[idx];
}

/* Characters in [from, to). */
static i32 count_chars(const Walk *w, i32 from, i32 to) {
    i32 n = 0;
    for (i32 p = from; p < to; p++) {
        if (!pos_cell(w->t, p)) break;
        if (!walk_ok(w, p)) continue;
        if (pos_is_dummy(w->t, p)) continue;
        n++;
    }
    return n;
}

/* Position `n` characters before `pos`, or -1 if the body is shorter. */
static i32 back_chars(const Walk *w, i32 pos, i32 n, i32 floor_pos) {
    while (n > 0 && pos > floor_pos) {
        pos--;
        if (!walk_ok(w, pos)) continue;
        if (pos_is_dummy(w->t, pos)) continue;   /* tail half of a wide glyph */
        n--;
    }
    return n == 0 ? pos : -1;
}

/* Position of the `n`-th character at or after `from`, clamped to `limit`. */
static i32 forward_chars(const Walk *w, i32 from, i32 n, i32 limit) {
    i32 p = from;
    while (p < limit && !walk_ok(w, p)) p++;
    while (n > 0 && p < limit) {
        p++;
        while (p < limit && (!walk_ok(w, p) || pos_is_dummy(w->t, p))) p++;
        n--;
    }
    return p;
}

/* Does the typed mirror still describe the cells in [start, end)? */
static bool hint_matches(const Walk *w, i32 start, i32 end, const char *hint) {
    const u8 *p = (const u8 *)hint;
    usize len = strlen(hint);
    usize i = 0;
    i32 q = start;
    while (i < len) {
        u32 cp = 0;
        u32 adv = utf8_decode(p + i, len - i, &cp);
        if (adv == 0) return false;
        i += adv;
        while (q < end && (!walk_ok(w, q) || pos_is_dummy(w->t, q))) q++;
        if (q >= end || !pos_cell(w->t, q)) return false;
        u32 got = pos_cp(w->t, q);
        /* The blank that stands in for a wrap point is whatever the app left
         * on screen; the mirror has the real space the user typed. */
        if (got != cp && !(is_blank_cp(got) && is_blank_cp(cp))) return false;
        q++;
    }
    return true;
}

/* =========================================================================
 * Span
 * ========================================================================= */

/* Is this terminal being driven by a full-screen app rather than a line
 * editor? Each of these is something an agent CLI / TUI turns on and a shell
 * never does. It changes two things: the app paints every row itself (so the
 * soft-wrap chain is not its line structure), and its input sits inside a
 * drawn block whose borders must not be mistaken for typed text. */
static bool terminal_is_tui(Terminal *t) {
    return (t->mode & MODE_ALT_SCREEN) != 0 ||
           t->Liu_kbd_flags != 0 || t->modify_other_keys != 0 ||
           (t->mode & MODE_MOUSE_TRACKING) != 0;
}

/* Rows of the drawn input block containing the cursor. Walks out from the
 * cursor row while rows still carry text, stopping at the row that opens with
 * a prompt sigil (that one is the block's first) and at the blank or all-border
 * row that closes it. */
static void block_rows(Terminal *t, i32 cy, bool boxed,
                       i32 *out_first, i32 *out_last) {
    /* Not every agent leaves the cursor in its input. Command Code hides the
     * cursor and parks it at the foot of the screen, so anchoring on the
     * cursor row would find an empty row and report nothing to select. The
     * nearest input row above it is the block. */
    if (!row_is_body(t, cy, boxed)) {
        i32 anchor = -1;
        for (i32 r = cy - 1, steps = 0; r >= 0 && steps < BODY_MAX_ROWS;
             r--, steps++) {
            if (row_is_body(t, r, boxed)) { anchor = r; break; }
        }
        if (anchor < 0) { *out_first = cy; *out_last = cy; return; }
        cy = anchor;
    }

    i32 first = cy, last = cy;
    while (first > 0 && (last - first + 1) < BODY_MAX_ROWS) {
        if (row_prompt_input_col(t, first, boxed) >= 0) break;
        if (!row_is_body(t, first - 1, boxed)) break;
        first--;
    }
    while (last + 1 < t->rows && (last - first + 1) < BODY_MAX_ROWS) {
        if (!row_is_body(t, last + 1, boxed)) break;
        if (row_prompt_input_col(t, last + 1, boxed) >= 0) break;
        last++;
    }
    *out_first = first;
    *out_last  = last;
}

/* Body columns of the block: the widest span of rows that is neither border
 * nor the continuation indent. `lo` is the smallest text start (the first row
 * measured past its sigil), `hi` stops at a right-hand border when the app
 * draws one and at the screen edge when it does not. */
static void block_body_cols(Terminal *t, i32 first, i32 last, bool boxed,
                            i32 *out_lo, i32 *out_hi) {
    i32 lo = t->cols, hi = t->cols;
    for (i32 r = first; r <= last; r++) {
        i32 s = row_text_start(t, r, boxed);
        if (r == first) {
            i32 ps = row_prompt_input_col(t, r, boxed);
            if (ps >= 0) s = ps;
        }
        if (s < lo) lo = s;

        /* Right border, if any: the last non-blank cell of the row. */
        i32 base = r * t->cols;
        i32 c = t->cols - 1;
        while (c >= 0 && is_blank_cp(pos_cp(t, base + c))) c--;
        if (c >= 0 && is_border_cp(pos_cp(t, base + c), boxed) && c < hi) hi = c;
    }
    if (lo > hi) lo = hi;
    *out_lo = lo;
    *out_hi = hi;
}

/* Text an app paints on its input's row that was not typed (CmdInputApp.ghost):
 * where the typed text ends, given the caret.
 *
 *   - an EMPTY input (nothing typed before the caret) shows a hint, and a hint
 *     stands apart from the caret: the first text right of it is two or more
 *     cells away. Text that starts AT the caret, or one cell off, is text
 *     typed — the caret taken back to the start of the line;
 *   - after typed text, a completion starts exactly AT the caret and is not the
 *     colour of the text before it. Typed text right of a caret that is in the
 *     middle of it is that colour.
 *
 * Only on the input's last row: the caret is at the end of the prompt there,
 * which is the only place either appears. `end` is where the span ended; the
 * answer is the caret's cell when there is a ghost, `end` otherwise. */
static i32 span_trim_ghost(Terminal *t, const Walk *w, i32 line_start, i32 last_row,
                           i32 cy, i32 cursor_pos, i32 end) {
    if (cy != last_row || cursor_pos >= end) return end;
    bool typed_before = false;
    u32 ref_fg = 0;
    for (i32 p = cursor_pos - 1; p >= line_start; p--) {
        if (!walk_ok(w, p) || pos_is_dummy(t, p)) continue;
        if (is_padding_cp(pos_cp(t, p), true)) continue;
        const Cell *c = pos_cell(t, p);
        if (!c) break;
        typed_before = true;
        ref_fg = c->attr.fg;
        break;
    }
    i32 row_end = (cy + 1) * t->cols;
    i32 q = cursor_pos;
    while (q < row_end && is_blank_cp(pos_cp(t, q))) q++;
    if (q >= row_end) return end;
    if (!typed_before) return q - cursor_pos >= 2 ? cursor_pos : end;
    if (q != cursor_pos) return end;
    const Cell *c = pos_cell(t, q);
    return c && c->attr.fg != ref_fg ? cursor_pos : end;
}

static bool span_impl(Terminal *t, const char *typed_hint, bool agent_ui, bool ghost,
                      CmdSpan *out) {
    if (!t || !out || !t->cells) return false;
    if (t->cols <= 0 || t->rows <= 0) return false;
    if (t->scroll_offset != 0) return false;   /* span is in visible-row coords */

    bool boxed = agent_ui || terminal_is_tui(t);
    i32 cols = t->cols;
    i32 cy = t->cursor_y;
    if (cy < 0) cy = 0;
    if (cy >= t->rows) cy = t->rows - 1;
    i32 cx = t->cursor_x;
    if (cx < 0) cx = 0;
    if (cx > cols) cx = cols;                  /* cursor can park past the edge */

    /* Extent of the input. A shell's line soft-wraps, so follow the wrap chain
     * (line_wrapped[r] means row r continues onto r+1). An agent paints its
     * own rows and stale wrap flags from earlier output say nothing about its
     * input block, so there the block is walked out from the cursor row. */
    i32 first_row = cy, last_row = cy;
    i32 lo = 0, hi = cols;
    if (boxed) {
        block_rows(t, cy, boxed, &first_row, &last_row);
        block_body_cols(t, first_row, last_row, boxed, &lo, &hi);
    } else {
        while (first_row > 0 && t->line_wrapped && t->line_wrapped[first_row - 1])
            first_row--;
        while (last_row + 1 < t->rows && t->line_wrapped && t->line_wrapped[last_row])
            last_row++;
    }

    Walk w;
    walk_init(&w, t, first_row, last_row, lo, hi, boxed);

    i32 line_start = first_row * cols;
    i32 line_end   = (last_row + 1) * cols;
    i32 cursor_pos = cy * cols + cx;
    /* Is the terminal cursor actually sitting in this input? It is not when an
     * agent hides it elsewhere, and then nothing about it can be trusted. */
    bool cursor_here = cursor_pos >= line_start && cursor_pos <= line_end;
    if (cursor_pos > line_end)   cursor_pos = line_end;
    if (cursor_pos < line_start) cursor_pos = line_start;

    /* End of the span: past the last cell carrying text — borders and the
     * padding out to them do not count, so `│ > hi        │` ends after "hi".
     * Never before the cursor, since a trailing space the user just typed is
     * still part of the line. */
    i32 end = line_start;
    for (i32 p = line_end - 1; p >= line_start; p--) {
        if (!walk_ok(&w, p)) continue;
        if (!is_padding_cp(pos_cp(t, p), boxed)) { end = p + 1; break; }
    }
    if (ghost && boxed && cursor_here) end = span_trim_ghost(t, &w, line_start, last_row, cy, cursor_pos, end);
    if (end < cursor_pos && cursor_here) end = cursor_pos;
    if (cursor_pos > end) cursor_pos = end;
    if (end > line_end) end = line_end;

    i32 start = -1;

    /* 1. liu's live mirror of what was typed here. Self-validating: it is only
     *    taken when the cells ending at the cursor still read back as the
     *    mirrored text, so a Tab completion or a history recall that rewrote
     *    the line falls through to the weaker evidence below. */
    if (typed_hint && typed_hint[0]) {
        i32 hint_chars = (i32)utf8_len((const u8 *)typed_hint, strlen(typed_hint));
        if (hint_chars > 0) {
            i32 cand = back_chars(&w, cursor_pos, hint_chars, line_start);
            if (cand >= line_start && hint_matches(&w, cand, cursor_pos, typed_hint))
                start = cand;
        }
    }

    /* 2. OSC 133 shell integration: 'B' marks the prompt's end, which is
     *    exactly where the input begins. Only trusted while the marker still
     *    sits inside this logical line and at/before the cursor — the rows are
     *    absolute, so a scroll can leave them stale. Never inside a drawn
     *    block: an agent emits no OSC 133, so any marker there is a leftover
     *    from the shell it was launched from. */
    if (start < 0 && !boxed && t->input_start_row >= first_row &&
        t->input_start_row <= cy && t->input_start_col >= 0 &&
        t->input_start_col <= cols) {
        i32 cand = t->input_start_row * cols + t->input_start_col;
        if (cand >= line_start && cand <= cursor_pos) start = cand;
    }

    /* 3. Prompt-sigil scan: skip up to the FIRST "… % " / "❯ " that qualifies
     *    as a prompt ending. The prompt comes before the command, so the first
     *    match is the prompt's; a later one belongs to the command itself
     *    (`echo 50% done`) and taking it would cut the command short. Erring
     *    the other way — a multi-segment prompt whose tail gets swept into the
     *    span — only over-copies, and still deletes the right thing. */
    if (start < 0) {
        i32 scan_end = cursor_pos < end ? cursor_pos : end;
        i32 seen = 0;
        bool at_line_start = true;
        for (i32 p = line_start; p < scan_end && seen < SIGIL_SCAN_CHARS; p++) {
            if (!walk_ok(&w, p)) continue;
            if (pos_is_dummy(t, p)) continue;
            seen++;
            u32 cp = pos_cp(t, p);
            /* Blanks and the block's border are still "before the line
             * starts", so an agent's `│ > ` opens the line the same way a
             * bare `> ` does. */
            if (is_padding_cp(cp, boxed)) continue;
            if (is_sigil(cp) && sigil_qualifies(cp, at_line_start) &&
                (p + 1 >= scan_end || is_blank_cp(pos_cp(t, p + 1)))) {
                i32 s = p + 1;
                while (s < scan_end &&
                       (!walk_ok(&w, s) || is_padding_cp(pos_cp(t, s), boxed)))
                    s++;
                start = s;
                break;
            }
            at_line_start = false;
        }
    }

    /* 4. Nothing recognisable — take the line as typed from its first cell
     *    that carries text. */
    if (start < 0) {
        start = line_start;
        while (start < end &&
               (!walk_ok(&w, start) || is_padding_cp(pos_cp(t, start), boxed)))
            start++;
    }

    if (start > end) start = end;
    if (start > cursor_pos) start = cursor_pos;

    out->start_row  = start / cols;
    out->start_col  = start % cols;
    out->end_row    = end / cols;
    out->end_col    = end % cols;
    out->body_left  = lo;
    out->body_right = hi;
    out->boxed      = boxed;
    out->nchars     = count_chars(&w, start, end);
    /* With the cursor parked outside the input there is no telling where the
     * app's own caret sits, so report the pessimistic end of the range: 0
     * makes cmdline_erase_plan() walk → all the way to the far edge before
     * erasing, which lands on the end of the text wherever the caret really
     * was (→ past the end is a no-op in every one of these inputs). */
    out->cursor_off = cursor_here ? count_chars(&w, start, cursor_pos) : 0;
    if (out->cursor_off > out->nchars) out->cursor_off = out->nchars;
    return true;
}

bool cmdline_span(Terminal *t, const char *typed_hint, bool agent_ui, CmdSpan *out) {
    return span_impl(t, typed_hint, agent_ui, false, out);
}

bool cmdline_span_app(Terminal *t, const char *typed_hint, bool agent_ui,
                      const CmdInputApp *app, CmdSpan *out) {
    return span_impl(t, typed_hint, agent_ui, app && app->ghost, out);
}

/* =========================================================================
 * Offsets
 * ========================================================================= */

static i32 span_start_pos(Terminal *t, const CmdSpan *sp) {
    return sp->start_row * t->cols + sp->start_col;
}

static i32 span_end_pos(Terminal *t, const CmdSpan *sp) {
    return sp->end_row * t->cols + sp->end_col;
}

/* =========================================================================
 * Text
 * ========================================================================= */

CmdErasePlan cmdline_erase_plan(const CmdSpan *sp, i32 from, i32 to) {
    CmdErasePlan plan = {0, 0, 0};
    if (!sp) return plan;
    if (from > to) { i32 tmp = from; from = to; to = tmp; }
    if (from < 0) from = 0;
    if (to > sp->nchars) to = sp->nchars;
    if (to <= from) return plan;

    i32 cursor = sp->cursor_off;
    if (cursor < 0) cursor = 0;
    if (cursor > sp->nchars) cursor = sp->nchars;

    if (cursor < to)      plan.move_right = to - cursor;
    else if (cursor > to) plan.move_left  = cursor - to;
    plan.backspaces = to - from;
    return plan;
}

char *cmdline_text(Terminal *t, const CmdSpan *sp, i32 from, i32 to) {
    if (!t || !sp || t->cols <= 0) return NULL;
    if (from < 0) from = 0;
    if (to > sp->nchars) to = sp->nchars;
    if (to <= from) return NULL;

    usize cap = (usize)(to - from) * UTF8_CLUSTER_MAX * 4u + 1u;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    /* One walk for the whole range: resolving each offset on its own is a
     * fresh traversal per character, which turns a long pasted prompt into a
     * quadratic scan of the grid. */
    Walk w;
    walk_from_span(&w, t, sp);
    i32 end = span_end_pos(t, sp);
    i32 pos = forward_chars(&w, span_start_pos(t, sp), from, end);

    usize len = 0;
    for (i32 i = from; i < to && pos < end; i++) {
        u32 cp = pos_cp(t, pos);
        if (cp == 0) break;
        /* Whatever a wrap point left on screen is a plain space in the text. */
        if (is_blank_cp(cp)) cp = ' ';
        u8 enc[UTF8_CLUSTER_MAX * 4];
        u32 n = utf8_cluster_encode(cp, enc, sizeof enc);
        if (n == 0 || len + n >= cap) { free(buf); return NULL; }
        memcpy(buf + len, enc, n);
        len += n;
        pos = forward_chars(&w, pos, 1, end);
    }
    buf[len] = '\0';
    if (len == 0) { free(buf); return NULL; }
    return buf;
}

/* =========================================================================
 * Verified capture
 * ========================================================================= */

/* Start of the code point that ends at byte `i` of the mirror. */
static i32 mirror_back(const char *m, i32 i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((u8)m[i] & 0xC0) == 0x80) i--;
    return i;
}

static u32 mirror_cp(const char *m, i32 from, i32 to) {
    u32 cp = 0;
    return utf8_decode((const u8 *)m + from, (usize)(to - from), &cp) ? cp : 0;
}

/* A blank typed inside a line: never a line break, never a tab. */
static bool is_typed_blank(u32 cp) {
    return cp == ' ' || cp == 0x00A0 || cp == 0x2007 || cp == 0x202F;
}

/* UTF-8 of the cells [from, to) of ONE row, as cmdline_text() spells them.
 * Returns the byte count, or -1 when `out` (which may be NULL to count only)
 * cannot hold it. */
static i32 cells_text(Terminal *t, i32 from, i32 to, char *out, usize cap) {
    usize len = 0;
    for (i32 p = from; p < to; p++) {
        if (pos_is_dummy(t, p)) continue;
        u32 cp = pos_cp(t, p);
        if (is_blank_cp(cp)) cp = ' ';
        u8 enc[UTF8_CLUSTER_MAX * 4];
        u32 n = utf8_cluster_encode(cp, enc, sizeof enc);
        if (n == 0) return -1;
        if (out) {
            if (len + n >= cap) return -1;
            memcpy(out + len, enc, n);
        }
        len += n;
    }
    if (out) out[len] = '\0';
    return (i32)len;
}

/* Rows right under row `last` that hold nothing, up to a frame, the next text
 * or the screen's end. */
static i32 open_rows_under(Terminal *t, i32 last) {
    i32 n = 0;
    for (i32 r = last + 1; r < t->rows && n < BODY_MAX_ROWS; r++) {
        if (row_has_text(t, r, true) || row_is_frame(t, r)) break;
        n++;
    }
    return n;
}

/* Read the mirror BACKWARDS against the input's cells, from its end, and say
 * where the two stop agreeing.
 *
 * Cells cannot say what stands between two rows of a drawn input: the space
 * the app wrapped on, a line break, or nothing at all — a word longer than the
 * row is cut where the row ends (Claude Code cuts a long URL this way). The
 * mirror can. So inside a row every painted cell must be the mirror's
 * character, and at a row's start the mirror is believed: one blank or one
 * line break, or neither, and the blanks a row opens with are either typed
 * (after a line break) or the app's indent. A row that holds nothing is a line
 * break and nothing else. Two blanks at a wrap are refused: nothing on screen
 * could confirm the second.
 *
 * Every row after the first has to be covered. On the FIRST row the mirror may
 * run out or stop agreeing: what stands before that point sits on one row,
 * where cells are exact, so the caller takes it from them — an attachment
 * chip, a paste the app folded into a label, text whose mirror was lost.
 *
 * Some of a prompt is in NO cell. Blanks typed last are on screen only as the
 * gap between the text and the cursor, and a blank the caret has been taken
 * back over is not on screen at all; nor is a blank typed before a line break,
 * or a line break typed last (the row it opened is empty). The cells end at
 * the last word while the prompt does not, and an erase counted from them is
 * short at the far end and takes too much at the near one. The mirror has
 * them. It is believed about them — the one thing here no cell can be asked
 * about — only when nothing has happened to it that it could not follow
 * (`sure`), and when the rest of it stands: all of it read back, or every cell
 * of the prompt accounted for.
 *
 * Returns the mirror offset the agreed tail starts at and its first cell in
 * `*pos`, or -1 when the tail does not reach back to the first row (or what it
 * says of the unseen has nothing to stand on). `*breaks` is how many line
 * breaks it ends with, each on a row of its own under the text, and
 * `*end_blanks` how many blanks follow the last of them. */
static i32 mirror_align(Terminal *t, const CmdSpan *sp, i32 last, const char *m,
                        bool *sure_io, bool caret_at_end, i32 *pos, i32 *breaks, i32 *end_blanks,
                        bool *unseen_any) {
    bool sure = *sure_io;
    i32 cols = t->cols;
    i32 end = sp->end_row * cols + sp->end_col;
    i32 start = sp->start_row * cols + sp->start_col;
    i32 whole = (i32)strlen(m);
    i32 mi = whole;
    bool unseen = false;
    *unseen_any = false;

    /* The mirror's end: line breaks that opened the empty rows under the text
     * (with whatever blanks were typed on those), then the blanks typed after
     * the last word, as many of which as the gap up to the cursor are on
     * screen. */
    i32 open = open_rows_under(t, last), gap = 0;
    for (i32 p = end - 1; p >= start && p >= last * cols && is_blank_cp(pos_cp(t, p)); p--) gap++;
    *breaks = 0;
    *end_blanks = 0;
    for (;;) {
        i32 at = mi, blanks = 0;
        while (at > 0) {
            i32 b = mirror_back(m, at);
            if (!is_typed_blank(mirror_cp(m, b, at))) break;
            at = b;
            blanks++;
        }
        if (at > 0 && *breaks < open) {
            i32 b = mirror_back(m, at);
            u32 cp = mirror_cp(m, b, at);
            if (cp == '\n' || cp == '\r') {
                if (*breaks == 0) *end_blanks = blanks;
                mi = b; (*breaks)++; unseen = true;
                continue;
            }
        }
        if (blanks > gap) {
            unseen = true;
            /* More blanks than the gap up to the cursor shows is what a caret
             * taken back over them looks like. With the caret AT the end, as
             * the mirror itself has it, and on this row, it is a mirror that
             * holds blanks the program never took (Claude Code swallows those
             * of a held space bar): not the prompt, whatever it says. */
            if (caret_at_end && *breaks == 0 && t->cursor_y == last) sure = *sure_io = false;
        }
        for (i32 k = gap; k < blanks; k++) mi = mirror_back(m, mi);
        break;
    }

    for (i32 r = last; r >= sp->start_row; r--) {
        bool first = r == sp->start_row;
        bool empty = !first && !row_has_text(t, r, true);
        i32 text_lo = first ? sp->start_col : empty ? 0 : row_text_start(t, r, true);
        i32 text_hi = empty ? 0 : r == last ? end - r * cols : row_text_end(t, r, true);
        if (text_hi > sp->body_right) text_hi = sp->body_right;
        for (i32 c = text_hi - 1; c >= text_lo; c--) {
            i32 p = r * cols + c;
            if (pos_is_dummy(t, p)) continue;
            u32 cp = pos_cp(t, p);
            bool same = false;
            if (mi > 0 && is_blank_cp(cp)) {
                i32 b = mirror_back(m, mi);
                if (is_typed_blank(mirror_cp(m, b, mi))) { mi = b; same = true; }
            } else if (mi > 0) {
                u8 enc[UTF8_CLUSTER_MAX * 4];
                i32 n = (i32)utf8_cluster_encode(cp, enc, sizeof enc);
                if (n > 0 && n <= mi && memcmp(m + mi - n, enc, (usize)n) == 0) {
                    mi -= n; same = true;
                }
            }
            if (same) continue;
            if (!first) return -1;
            *pos = p + 1;
            if (unseen && (mi > 0 || !sure)) {
                /* An unmatched mirror prefix cannot be aligned when the input
                 * spans multiple visible rows, so reject that partial match.
                 * For a single-row input, accept the whole visible prompt
                 * instead of the partial match. */
                if (last > sp->start_row) return -1;
                *pos = end;
                *breaks = 0;
                return whole;
            }
            return mi;
        }
        if (first) break;

        /* Between this row and the one above it. */
        i32 lead = empty ? 0 : text_lo - sp->body_left;
        if (lead < 0) lead = 0;
        i32 run = 0, at = mi;
        bool line_break = false;
        while (at > 0) {
            i32 b = mirror_back(m, at);
            u32 cp = mirror_cp(m, b, at);
            if (is_typed_blank(cp)) { run++; at = b; continue; }
            if (cp == '\n' || cp == '\r') { run++; at = b; line_break = true; }
            break;
        }
        i32 blanks = line_break ? run - 1 : run;
        bool above_empty = r - 1 > sp->start_row && !row_has_text(t, r - 1, true);
        if (empty || above_empty) {
            /* A row that holds nothing is there because of a line break, and
             * so is the row after it. Blanks typed on a line of their own are
             * in no cell. */
            if (!line_break) return -1;
            if (empty && blanks) unseen = true;
        }
        if (!empty) {
            bool known = line_break ? (blanks == lead || blanks == 0)
                                    : (run == lead || run == lead + 1 || run <= 1);
            if (!known) return -1;
        }
        /* The mirror begins exactly where this row does: what joins the row
         * to the one above is not in it. */
        if (at == 0 && run == 0) return -1;
        /* Blanks typed before a line break end the row above unseen, like
         * the ones typed last. */
        while (line_break && at > 0) {
            i32 b = mirror_back(m, at);
            if (!is_typed_blank(mirror_cp(m, b, at))) break;
            at = b;
            unseen = true;
        }
        mi = at;
    }
    *pos = start;
    if (unseen && !sure) {
        if (last > sp->start_row) return -1;
        *pos = end;
        *breaks = 0;
        return whole;
    }
    *unseen_any = unseen;
    return mi;
}

/* Where typing begins on the row that carries the prompt sigil — also when
 * nothing has been typed on it yet (a prompt that opens with a line break):
 * one separator past the sigil, as "❯ ", "› " and "│ ❯ " all have it. -1 when
 * the row does not open with a sigil. */
static i32 row_input_col(Terminal *t, i32 row) {
    i32 col = row_prompt_input_col(t, row, true);
    if (col < 0 || col < t->cols) return col;
    i32 c = row_text_start(t, row, true) + 1;
    while (c < t->cols && pos_is_dummy(t, row * t->cols + c)) c++;
    return c + 1 < t->cols ? c + 1 : t->cols - 1;
}

/* A prompt with an empty line in it is several blocks of rows to
 * cmdline_span(), which walks out from the cursor's row while rows carry text.
 * For the capture the input is all of them: up to the row its prompt sigil is
 * on, and down to the last text above the frame that closes it — downwards
 * only where there IS such a frame: under an input that ends in blank rows the
 * next text is the app's own. The mirror then has to account for every row,
 * the empty ones as line breaks, so a row taken in by mistake is a refusal. */
static void capture_extend(Terminal *t, CmdSpan *sp, i32 *last_io) {
    i32 first = sp->start_row, last = *last_io;
    i32 new_last = last;
    for (i32 r = last + 1, seen = last; r < t->rows && r - first < BODY_MAX_ROWS; r++) {
        if (row_is_frame(t, r)) { new_last = seen; break; }
        if (!row_has_text(t, r, true)) continue;
        if (row_prompt_input_col(t, r, true) >= 0) break;     /* another prompt */
        seen = r;
    }
    i32 new_first = first;
    if (row_prompt_input_col(t, first, true) < 0) {
        for (i32 r = first - 1; r >= 0 && new_last - r < BODY_MAX_ROWS; r--) {
            if (row_is_frame(t, r)) break;
            if (!row_has_text(t, r, true)) continue;
            if (row_prompt_input_col(t, r, true) >= 0) { new_first = r; break; }
        }
    }
    if (new_first == first && new_last == last) return;

    i32 lo, hi;
    block_body_cols(t, new_first, new_last, true, &lo, &hi);
    /* A row that opens with typed blanks starts its TEXT further in than the
     * body does, and with the sigil's row empty there is no other row to
     * say where the body begins. */
    i32 in = row_input_col(t, new_first);
    if (in >= 0 && in < lo) lo = in;
    sp->body_left = lo;
    sp->body_right = hi;
    if (new_first != first) {
        sp->start_row = new_first;
        sp->start_col = in;
    }
    if (new_last != last) {
        i32 e = row_text_end(t, new_last, true);
        if (e > hi) e = hi;
        sp->end_row = new_last;
        sp->end_col = e;
        if (e >= t->cols) { sp->end_row = new_last + 1; sp->end_col = 0; }
        *last_io = new_last;
    }
}

void cmdline_span_whole(Terminal *t, CmdSpan *sp) {
    if (!t || !sp || t->cols <= 0) return;
    i32 last = sp->end_row - (sp->end_col == 0 && sp->end_row > sp->start_row ? 1 : 0);
    capture_extend(t, sp, &last);
}

static bool capture_exact(Terminal *t, const char *mirror, bool mirror_sure, i32 mirror_caret,
                          CmdInputApp app, char *out, usize cap, usize *typed_from) {
    i32 whole_rows = app.whole_rows > 1 ? app.whole_rows : 1;
    if (typed_from) *typed_from = 0;
    if (!out || !cap) return false;
    out[0] = '\0';
    CmdSpan sp;
    if (!span_impl(t, NULL, true, app.ghost, &sp)) return false;
    i32 last = sp.end_row - (sp.end_col == 0 && sp.end_row > sp.start_row ? 1 : 0);
    bool have = mirror && mirror[0];
    bool caret_at_end = have && mirror_caret >= 0 && (usize)mirror_caret == strlen(mirror);
    /* With a mirror the block the caret is in may be the least of the input:
     * a prompt that opens with an empty line has none of its text there, and
     * the span of such a block starts at the sigil's own cell. */
    if (!have && sp.nchars <= 0) return false;
    if (have) {
        bool none = sp.nchars <= 0;
        capture_extend(t, &sp, &last);
        i32 in = none ? row_input_col(t, sp.start_row) : -1;
        if (in >= 0) sp.start_col = in;
        /* The mirror has its caret at the end and the cursor stands INSIDE
         * the text: a caret the mirror was not told about (a click the
         * program took, a key liu did not send). Its text may still read
         * back; its word for what no cell shows is not taken. */
        if (caret_at_end && t->cursor_y <= last &&
            t->cursor_y * t->cols + t->cursor_x < sp.end_row * t->cols + sp.end_col)
            mirror_sure = false;
    }
    /* The caret on an empty row right under the text is still in the input: a
     * line break typed last has opened that row, or the row above is full and
     * the caret after its last character is shown where the next one would go
     * (Claude Code does both). Which of the two, only a mirror that has
     * followed every key can say — so that is asked below, and without such a
     * mirror this is a cursor outside the input. */
    i32 under = t->cursor_y - last;
    bool below = under >= 1 && under <= open_rows_under(t, last);
    if (below && !(have && mirror_sure)) return false;
    if (t->cursor_y < sp.start_row || (t->cursor_y > last && !below) ||
        t->cursor_x < sp.body_left || t->cursor_x > sp.body_right) return false;
    bool sigil = row_prompt_input_col(t, sp.start_row, true) >= 0;
    bool framed = row_is_frame(t, sp.start_row - 1) && row_is_frame(t, last + 1);
    /* OpenCode bounds its input with blank rows carrying only a vertical
     * rail. Both rails must align with the body, not just any nearby border. */
    if (!framed && sp.body_left > 0 && sp.start_row > 0 && last + 1 < t->rows &&
        !row_has_text(t, sp.start_row - 1, true) && !row_has_text(t, last + 1, true)) {
        for (i32 c = 0; c < sp.body_left; c++) {
            if (is_border_cp(pos_cp(t, (sp.start_row - 1) * t->cols + c), true) &&
                is_border_cp(pos_cp(t, (last + 1) * t->cols + c), true)) {
                framed = true; break;
            }
        }
    }
    if (!sigil && !framed) return false;
    /* A cap or clipped viewport is not the end of an input. */
    if (last + 1 < t->rows && row_is_body(t, last + 1, true)) return false;
    if (last - sp.start_row + 1 >= BODY_MAX_ROWS) return false;

    i32 start = sp.start_row * t->cols + sp.start_col;
    i32 pos = start, mi = -1, breaks = 0, end_blanks = 0;
    bool unseen = false;
    if (have) mi = mirror_align(t, &sp, last, mirror, &mirror_sure, caret_at_end, &pos, &breaks, &end_blanks, &unseen);

    /* Some prompt UIs clip the top of an oversized input without marking the
     * truncation. When visible input exceeds the app's known whole_rows limit,
     * the displayed rows may be only the tail of the prompt. Treat the mirror as
     * complete only if it tracked every key and all visible cells match;
     * otherwise the hidden prefix cannot be inferred safely.
     *
     * With fewer rows, the prompt is not clipped. If the mirror contains extra
     * text before the visible cells, that prefix is stale and the visible prompt
     * is authoritative. */
    /* How tall the input is on screen: its rows of text, and the empty rows
     * under them that are its own — all of them where the app is known to
     * close its input with a frame and nothing else (`tight_frame`), and
     * otherwise the ones the mirror's last line breaks opened. */
    bool several = last > sp.start_row;
    i32 open = open_rows_under(t, last);
    bool closed = app.tight_frame && last + 1 + open < t->rows && row_is_frame(t, last + 1 + open);
    i32 tall = last - sp.start_row + 1 + (closed ? open : breaks);
    if (mi >= 0 && tall > 1 && tall > whole_rows) {
        if (!mirror_sure || pos != start) return false;
        mi = 0;
    }
    /* What the mirror says of what is in no cell stands on ALL of it having
     * been read back. With more in front that is not on screen (and not the
     * top of a cut input), a blank at that seam is the stale part's as easily
     * as the prompt's. */
    if (mi > 0 && unseen) {
        if (several) return false;
        pos = sp.end_row * t->cols + sp.end_col;
        mi = (i32)strlen(mirror);
        breaks = end_blanks = 0;
    }
    /* If the caret is at the start of the empty row below the text and the
     * mirror has no line break for it, the preceding row may be full. Some
     * prompt layouts reserve trailing columns; account for that margin when
     * deciding whether the row reached the body's right edge. */
    bool overflow = breaks == 0 && under == 1 && t->cursor_x == sp.body_left &&
                    row_text_end(t, last, true) >= sp.body_right - 4;
    /* In such an app the empty rows between the text and the frame are the
     * input's own: line breaks typed last, or that one overflow row. A reading
     * that does not account for each of them is not the prompt, and cells
     * account for none, so a mirror one line break short is refused. */
    if (closed && open > 0 && (mi < 0 || !(breaks == open || (overflow && open == 1)))) return false;
    /* What the mirror covers has to begin INSIDE the first row. Beginning at
     * that row's edge, it leaves what joins it to the cells before it in
     * neither: a blank the row cannot show before a line break, a second
     * blank at a wrap. */
    if (several && mi >= 0 && pos != start) {
        i32 edge = row_text_end(t, sp.start_row, true);
        if (edge > sp.body_right) edge = sp.body_right;
        if (pos >= sp.start_row * t->cols + edge) return false;
    }
    if (below) {
        /* The mirror has to stand whole — read back to its first byte, or
         * covering every cell: one that knows only part of the prompt may be
         * missing the very line break the caret is sitting after. And the
         * caret is on a row one of ITS line breaks opened, or, with none, at
         * the start of the row under the text. */
        if (!(mi == 0 || (mi > 0 && pos == start))) return false;
        if (breaks ? under > breaks : !overflow) return false;
        /* On the last of those rows the caret stands after the blanks typed
         * there: the mirror has at least that many. */
        if (breaks && under == breaks && t->cursor_x - sp.body_left > end_blanks) return false;
    }

    /* One row on screen, and a mirror that has followed every key holds a
     * line break the cells did not give back: this row is not the whole
     * prompt (an input with no frame, the caret in the first of its
     * paragraphs). Its cells alone would be erased as if it were. */
    if (last == sp.start_row && mirror_sure && mi > 0 && memchr(mirror, '\n', (usize)mi)) return false;

    if (last == sp.start_row && mi < 0) {
        /* One row and no mirror: the cells are the text. */
        char *text = cmdline_text(t, &sp, 0, sp.nchars);
        if (!text) return false;
        usize len = strlen(text);
        bool ok = len < cap;
        if (ok) {
            memcpy(out, text, len + 1);
            if (typed_from) *typed_from = len;
        }
        free(text);
        return ok;
    }

    /* What the mirror agrees with is the mirror's: it knows where its line
     * breaks are, and what of the prompt is in no cell. Before that, the cells
     * of the first row. On several rows there is no capture without it. */
    if (mi < 0) return false;
    i32 head = cells_text(t, start, pos, out, cap);
    usize tail = strlen(mirror + mi);
    if (head < 0 || (usize)head + tail >= cap) { out[0] = '\0'; return false; }
    memcpy(out + head, mirror + mi, tail + 1);
    if (typed_from) *typed_from = (usize)head;
    /* Blanks and line breaks alone are no prompt. */
    for (const unsigned char *c = (const unsigned char *)out; ; c++) {
        if (!*c) { out[0] = '\0'; return false; }
        if (*c > ' ') break;
    }
    return true;
}

bool cmdline_capture_prompt_ex(Terminal *t, const char *mirror, bool mirror_sure,
                               CmdInputApp app, char *out, usize cap, usize *typed_from) {
    return capture_exact(t, mirror, mirror_sure, -1, app, out, cap, typed_from);
}

bool cmdline_capture_prompt(Terminal *t, const char *mirror,
                            char *out, usize cap) {
    return cmdline_capture_prompt_ex(t, mirror, true, (CmdInputApp){ t ? t->rows : 1, true, 0, 0, false }, out, cap, NULL);
}

/* =========================================================================
 * An input read from its cells
 * ========================================================================= */

/* The column an input's text begins at on the row its sigil is on: one
 * separator past the sigil, so that blanks typed first stay in the text. -1
 * when the row does not open with one. */
static i32 sigil_text_col(Terminal *t, i32 row) {
    {
        i32 corner = row_corner_input_col(t, row, true);
        if (corner >= 0) return corner;
    }
    i32 c = row_text_start(t, row, true);
    if (c >= t->cols) return -1;
    if (!is_sigil(pos_cp(t, row * t->cols + c))) return -1;
    c++;
    while (c < t->cols && pos_is_dummy(t, row * t->cols + c)) c++;
    return c + 1 <= t->cols ? c + 1 : t->cols;
}

/* The drawn input as a box of rows: where it starts and ends, what closes
 * it, and whether the cursor is in it. */
typedef struct {
    CmdSpan sp;
    i32  last;            /* the last row that carries text */
    i32  open;            /* empty rows right under it, up to a frame or text */
    i32  base;            /* the column its text begins at, -1 with no sigil */
    bool frame_above;
    bool closed;          /* a frame right under the open rows */
    bool cursor_in;
} InputBox;

static bool input_box(Terminal *t, InputBox *b, bool ghost) {
    memset(b, 0, sizeof *b);
    if (!span_impl(t, NULL, true, ghost, &b->sp)) return false;
    b->last = b->sp.end_row - (b->sp.end_col == 0 && b->sp.end_row > b->sp.start_row ? 1 : 0);
    capture_extend(t, &b->sp, &b->last);
    b->open = open_rows_under(t, b->last);
    b->base = sigil_text_col(t, b->sp.start_row);
    b->frame_above = row_is_frame(t, b->sp.start_row - 1);
    b->closed = b->last + 1 + b->open < t->rows && row_is_frame(t, b->last + 1 + b->open);
    b->cursor_in = t->cursor_y >= b->sp.start_row && t->cursor_y <= b->last + b->open &&
                   t->cursor_x >= (b->base >= 0 && b->base < b->sp.body_left ? b->base : b->sp.body_left) &&
                   t->cursor_x <= b->sp.body_right;
    return true;
}

/* Text cells of row `r` of the box, [*from, *to), from where the input's text
 * begins. False when the row has text left of that. */
static bool box_row(Terminal *t, const InputBox *b, i32 r, i32 *from, i32 *to) {
    *from = b->base;
    *to = row_text_end(t, r, true);
    /* A wide character that ends the row owns the cell after it too. */
    if (*to > 0 && *to < t->cols && pos_is_dummy(t, r * t->cols + *to)) (*to)++;
    if (*to > b->sp.body_right) *to = b->sp.body_right;
    if (r == b->sp.start_row) {
        if (*to < *from) *to = *from;
        return true;
    }
    if (*to <= 0) { *from = *to = b->base; return true; }      /* an empty row */
    return row_text_start(t, r, true) >= b->base;
}

/* A cell that separates words: blank, and not the second half of a wide
 * character (which reads as blank too). */
static bool pos_is_gap(Terminal *t, i32 pos) {
    return !pos_is_dummy(t, pos) && is_blank_cp(pos_cp(t, pos));
}

/* Is the input's one row a hint — every painted cell of it dim? */
static bool box_is_hint(Terminal *t, const InputBox *b) {
    if (b->base < 0 || b->last != b->sp.start_row) return false;
    i32 from, to, seen = 0;
    if (!box_row(t, b, b->last, &from, &to)) return false;
    for (i32 c = from; c < to; c++) {
        const Cell *cell = pos_cell(t, b->last * t->cols + c);
        if (!cell || (cell->attr.flags & ATTR_WDUMMY) || is_blank_cp(pos_cp(t, b->last * t->cols + c))) continue;
        if (!(cell->attr.flags & ATTR_DIM)) return false;
        seen++;
    }
    return seen > 0;
}

/* Read the box from its cells, by the app's own wrap rule.
 *
 * The app wraps a line at a word, `width` columns from where the text
 * begins, and drops the blank it wrapped on; a word longer than the row is
 * cut where the row ends; a line break starts a row (Claude Code's editor
 * behaves exactly so). So what joins row r to the one above can be told from
 * the cells:
 *
 *   - either row holds nothing, or r opens with blanks: a line break (a
 *     wrapped row never opens with one);
 *   - the row above is not full, and r's first word would have fitted on it
 *     behind a blank: a line break — had it been one line, the word would
 *     be up there;
 *   - not full, and the word would not have fitted: the blank it wrapped on;
 *   - full: a blank, unless the word that ends the row above and the one that
 *     opens r are together longer than a row — then it is ONE word, cut.
 *
 * What this cannot see is blanks that are in no cell: typed last, before a
 * line break, on a line of their own, a second one at a wrap. The text is
 * the prompt without those, which is why nothing may be counted from it.
 *
 * False when the rows are not laid out as that rule says (a row wider than
 * `width`, text left of the input): the rule is not this program's. */
static bool box_read(Terminal *t, const InputBox *b, i32 width, char *out, usize cap) {
    i32 cols = t->cols;
    usize len = 0;
    out[0] = '\0';
    if (b->base < 0 || width <= 0) return false;
    i32 p_from = 0, p_to = 0;
    for (i32 r = b->sp.start_row; r <= b->last; r++) {
        i32 from, to;
        if (!box_row(t, b, r, &from, &to) || to - b->base > width) { out[0] = '\0'; return false; }
        if (r > b->sp.start_row) {
            const char *sep = "\n";
            bool p_empty = p_to <= p_from, r_empty = to <= from;
            i32 r_start = r_empty ? from : row_text_start(t, r, true);
            if (!p_empty && !r_empty && r_start == b->base) {
                i32 word = r_start;                 /* r's first word: [r_start, word) */
                while (word < to && !pos_is_gap(t, r * cols + word)) word++;
                i32 w_next = word - r_start;
                i32 used = p_to - b->base;
                bool wide = pos_is_dummy(t, r * cols + r_start + 1);
                bool full = used >= width || (wide && used == width - 1);
                if (!full) {
                    sep = used + 1 + w_next <= width ? "\n" : " ";
                } else {
                    i32 tail = p_to;                /* the word that ends the row above: [tail, p_to) */
                    while (tail > p_from && !pos_is_gap(t, (r - 1) * cols + tail - 1)) tail--;
                    sep = (p_to - tail) + w_next > width ? "" : " ";
                }
            }
            usize sl = strlen(sep);
            if (len + sl >= cap) { out[0] = '\0'; return false; }
            memcpy(out + len, sep, sl);
            len += sl;
        }
        i32 n = cells_text(t, r * cols + from, r * cols + to, out + len, cap - len);
        if (n < 0) { out[0] = '\0'; return false; }
        len += (usize)n;
        p_from = from;
        p_to = to;
    }
    out[len] = '\0';
    for (const unsigned char *c = (const unsigned char *)out; ; c++) {
        if (!*c) { out[0] = '\0'; return false; }
        if (*c > ' ') break;
    }
    return true;
}

bool cmdline_row_in_input(Terminal *t, i32 row) {
    InputBox b;
    if (!t || !input_box(t, &b, false)) return false;
    return row >= b.sp.start_row && row <= b.last + b.open;
}

CmdCaptureKind cmdline_capture(Terminal *t, const char *mirror, bool mirror_sure, i32 mirror_caret,
                               CmdInputApp app, char *out, usize cap, usize *typed_from,
                               CmdRefusal *why) {
    CmdRefusal unused;
    if (!why) why = &unused;
    *why = CMD_REFUSED_NO_INPUT;
    if (typed_from) *typed_from = 0;
    if (!out || !cap) return CMD_CAPTURE_NONE;
    out[0] = '\0';
    InputBox b;
    if (!t || !input_box(t, &b, app.ghost)) return CMD_CAPTURE_NONE;

    /* The same box in another mode is not a prompt: Claude Code's "!" row is
     * a shell command, and a translation typed over it would not be one. */
    if (app.other_sigil) {
        i32 c = row_text_start(t, b.sp.start_row, true);
        i32 at = b.sp.start_row * t->cols + c;
        if (c + 1 < t->cols && pos_cp(t, at) == app.other_sigil && is_blank_cp(pos_cp(t, at + 1)))
            return CMD_CAPTURE_NONE;
    }
    /* The hint an empty input shows is the app's text, not the user's. */
    bool no_text = b.last == b.sp.start_row && b.sp.nchars <= 0;
    if (no_text || (app.dim_hint && box_is_hint(t, &b))) {
        *why = CMD_REFUSED_EMPTY;
        return CMD_CAPTURE_NONE;
    }

    if (capture_exact(t, mirror, mirror_sure, mirror_caret, app, out, cap, typed_from)) {
        *why = CMD_REFUSED_NOT;
        return CMD_CAPTURE_EXACT;
    }
    out[0] = '\0';
    if (typed_from) *typed_from = 0;

    /* No caret in it, or neither a sigil nor a frame: not a prompt at all. */
    if (!b.cursor_in || (b.base < 0 && !b.frame_above)) return CMD_CAPTURE_NONE;
    *why = CMD_REFUSED_UNREAD;
    bool measured = app.wrap_width > 0 && app.tight_frame;
    i32 tall = b.last - b.sp.start_row + 1 + (b.closed && app.tight_frame ? b.open : 0);
    i32 whole_rows = app.whole_rows > 1 ? app.whole_rows : 1;
    if (tall <= 1) return CMD_CAPTURE_NONE;
    if (tall > whole_rows) {
        /* More rows than the app is known to show whole: its top may be cut,
         * and a row of the middle of a prompt reads like any other. */
        if (measured) *why = CMD_REFUSED_CUT;
        return CMD_CAPTURE_NONE;
    }
    if (!measured || b.base < 0 || !b.frame_above || !b.closed) return CMD_CAPTURE_NONE;
    if (!box_read(t, &b, app.wrap_width, out, cap)) return CMD_CAPTURE_NONE;
    if (typed_from) *typed_from = strlen(out);
    *why = CMD_REFUSED_NOT;
    return CMD_CAPTURE_WHOLE;
}

/* A history selection replaces the complete input, preserving the user's
 * opportunity to edit before executing. Newlines require bracketed paste. */
usize cmdline_recall_bytes(Terminal *t, const CmdSpan *sp, const char *command,
                           u8 *out, usize cap) {
    if (!t || !sp || !command || !command[0] || !out) return 0;
    bool paste = (t->mode & MODE_BRACKETED_PASTE) != 0;
    for (const unsigned char *p = (const unsigned char *)command; *p; p++) {
        if (*p == 127 || (*p < 32 && (!paste || (*p != '\n' && *p != '\t'))))
            return 0;
    }
    CmdErasePlan plan = cmdline_erase_plan(sp, 0, sp->nchars);
    u32 codes[3] = { KEY_RIGHT, KEY_LEFT, KEY_BACKSPACE };
    i32 counts[3] = { plan.move_right, plan.move_left, plan.backspaces };
    usize used = 0, len = strlen(command);
    for (i32 i = 0; i < 3; i++) {
        u8 key[32];
        i32 n = terminal_key_input(t, codes[i], 0, key, sizeof key);
        if (counts[i] < 0 || n <= 0 || (usize)counts[i] > (cap-used)/(usize)n) return 0;
        for (i32 j = 0; j < counts[i]; j++) {
            memcpy(out+used, key, (usize)n); used += (usize)n;
        }
    }
    usize brackets = paste ? 12u : 0u;
    if (brackets > cap-used || len > cap-used-brackets) return 0;
    if (paste) { memcpy(out+used, "\033[200~", 6); used += 6; }
    memcpy(out+used, command, len); used += len;
    if (paste) { memcpy(out+used, "\033[201~", 6); used += 6; }
    return used;
}
