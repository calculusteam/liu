/*
 * Liu — single-line text editing for chrome fields. See textedit.h.
 */
#include "ui/textedit.h"
#include "core/utf8.h"
#include "platform/platform.h"

#include <string.h>

/* ------------------------------------------------------------ boundaries */

static bool is_cont(const char *b, i32 i) { return ((u8)b[i] & 0xC0) == 0x80; }

static i32 clamp_i(i32 v, i32 lo, i32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Snap a byte offset back to the start of the character containing it. */
static i32 snap_left(const char *b, i32 off) {
    while (off > 0 && is_cont(b, off)) off--;
    return off;
}

static i32 next_char(const char *b, i32 len, i32 off) {
    if (off >= len) return len;
    off++;
    while (off < len && is_cont(b, off)) off++;
    return off;
}

static i32 prev_char(const char *b, i32 off) {
    if (off <= 0) return 0;
    off--;
    while (off > 0 && is_cont(b, off)) off--;
    return off;
}

/* Word class for motion. Everything non-alphanumeric is a separator; that
 * matches how macOS treats punctuation in a single-line field closely enough
 * for a command query or a path. Bytes ≥ 0x80 count as word characters so a
 * Turkish or CJK run is one word rather than one word per byte. */
static bool is_word_byte(char c) {
    u8 u = (u8)c;
    if (u >= 0x80) return true;
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') ||
           (u >= 'A' && u <= 'Z') || u == '_';
}

static i32 word_left(const char *b, i32 off) {
    while (off > 0 && !is_word_byte(b[prev_char(b, off)])) off = prev_char(b, off);
    while (off > 0 &&  is_word_byte(b[prev_char(b, off)])) off = prev_char(b, off);
    return off;
}

static i32 word_right(const char *b, i32 len, i32 off) {
    while (off < len && !is_word_byte(b[off])) off = next_char(b, len, off);
    while (off < len &&  is_word_byte(b[off])) off = next_char(b, len, off);
    return off;
}

/* ------------------------------------------------------------- selection */

bool textedit_has_selection(const TextEdit *e) {
    return e && e->caret && e->sel && *e->caret != *e->sel;
}

void textedit_selection(const TextEdit *e, i32 *from, i32 *to) {
    i32 a = *e->caret, b = *e->sel;
    if (a > b) { i32 t = a; a = b; b = t; }
    if (from) *from = a;
    if (to)   *to   = b;
}

void textedit_clamp(TextEdit *e) {
    if (!e || !e->buf || !e->len || !e->caret || !e->sel) return;
    i32 len = *e->len;
    *e->caret = snap_left(e->buf, clamp_i(*e->caret, 0, len));
    *e->sel   = snap_left(e->buf, clamp_i(*e->sel,   0, len));
}

/* A byte offset past the end of `buf` is not a caret position, so stop at the
 * NUL rather than counting the dead bytes after it as characters.
 *
 * Callers legitimately hold such an offset for a frame: the shared editor
 * re-anchors lazily (app_edit_ex's te_shadow check), so every site that wipes
 * a field behind its back — a palette close clearing the query is the common
 * one — leaves the old caret in place until the next keystroke. Rendering
 * happens in between, and without this clamp the caret would be drawn in the
 * blank space past the end of the text. */
i32 textedit_cell_index(const char *buf, i32 byte_off) {
    if (!buf || byte_off <= 0) return 0;
    return (i32)utf8_len((const u8 *)buf, strnlen(buf, (usize)byte_off));
}

i32 textedit_byte_offset(const char *buf, i32 len, i32 cell) {
    if (!buf || cell <= 0) return 0;
    i32 off = 0;
    while (off < len && cell > 0) { off = next_char(buf, len, off); cell--; }
    return off;
}

/* --------------------------------------------------------------- undo --- */

void textedit_undo_reset(TextEditUndo *u) {
    if (!u) return;
    u->count = 0;
    u->pos = -1;
    u->typing = false;
}

/* Record the CURRENT buffer as the state to come back to. Everything after
 * `pos` is dropped — the usual "editing after undo forks the history" rule. */
static void undo_push(TextEditUndo *u, const TextEdit *e) {
    if (!u || !e) return;
    i32 len = *e->len;
    if (len >= TEXTEDIT_UNDO_CAP) return;      /* too long to snapshot */

    if (u->pos < 0) { u->count = 0; u->pos = -1; }
    i32 next = u->pos + 1;
    if (next >= TEXTEDIT_UNDO_DEPTH) {
        /* Ring is full — drop the oldest snapshot and slide down. */
        memmove(u->text[0], u->text[1],
                (usize)(TEXTEDIT_UNDO_DEPTH - 1) * TEXTEDIT_UNDO_CAP);
        memmove(u->caret, u->caret + 1,
                (usize)(TEXTEDIT_UNDO_DEPTH - 1) * sizeof u->caret[0]);
        next = TEXTEDIT_UNDO_DEPTH - 1;
    }
    memcpy(u->text[next], e->buf, (usize)len);
    u->text[next][len] = '\0';
    u->caret[next] = *e->caret;
    u->pos = next;
    u->count = next + 1;
}

/* Snapshot before a mutation. `coalescing` is true for plain typing, which
 * folds a run of insertions into one undo step. */
static void undo_mark(TextEditUndo *u, const TextEdit *e, bool coalescing) {
    if (!u) return;
    if (coalescing && u->typing && u->pos >= 0) return;   /* run already open */
    /* Make the tip equal the pre-edit text, so undo has somewhere to go. */
    if (u->pos < 0 || strcmp(u->text[u->pos], e->buf) != 0) undo_push(u, e);
    u->typing = coalescing;
}

static void undo_restore(TextEditUndo *u, TextEdit *e, i32 slot) {
    i32 n = (i32)strlen(u->text[slot]);
    if (n >= e->cap) n = e->cap - 1;
    memcpy(e->buf, u->text[slot], (usize)n);
    e->buf[n] = '\0';
    *e->len = n;
    *e->caret = clamp_i(u->caret[slot], 0, n);
    *e->sel = *e->caret;
    textedit_clamp(e);
}

static bool do_undo(TextEditUndo *u, TextEdit *e) {
    if (!u || u->pos < 0) return false;
    u->typing = false;
    /* The tip mirrors the state BEFORE the newest edit only once the live
     * buffer has moved past it; push the live text first so redo can return. */
    if (strcmp(u->text[u->pos], e->buf) != 0) {
        undo_push(u, e);
        if (u->pos <= 0) return false;
    }
    if (u->pos <= 0) return false;
    u->pos--;
    undo_restore(u, e, u->pos);
    return true;
}

static bool do_redo(TextEditUndo *u, TextEdit *e) {
    if (!u || u->pos < 0 || u->pos + 1 >= u->count) return false;
    u->typing = false;
    u->pos++;
    undo_restore(u, e, u->pos);
    return true;
}

/* -------------------------------------------------------------- editing - */

/* Remove [from,to) and leave the caret at `from`. */
static void erase_range(TextEdit *e, i32 from, i32 to) {
    i32 len = *e->len;
    from = clamp_i(from, 0, len);
    to   = clamp_i(to,   0, len);
    if (to <= from) return;
    memmove(e->buf + from, e->buf + to, (usize)(len - to));
    len -= (to - from);
    e->buf[len] = '\0';
    *e->len = len;
    *e->caret = from;
    *e->sel = from;
}

static bool delete_selection(TextEdit *e) {
    if (!textedit_has_selection(e)) return false;
    i32 a, b;
    textedit_selection(e, &a, &b);
    erase_range(e, a, b);
    return true;
}

static void insert_bytes(TextEdit *e, const char *src, i32 n) {
    if (n <= 0) return;
    delete_selection(e);
    i32 len = *e->len;
    if (len + n >= e->cap) {
        n = e->cap - 1 - len;                  /* partial insert beats none */
        while (n > 0 && is_cont(src, n)) n--;  /* never split a character */
        if (n <= 0) return;
    }
    i32 at = clamp_i(*e->caret, 0, len);
    memmove(e->buf + at + n, e->buf + at, (usize)(len - at));
    memcpy(e->buf + at, src, (usize)n);
    len += n;
    e->buf[len] = '\0';
    *e->len = len;
    *e->caret = at + n;
    *e->sel = *e->caret;
}

void textedit_insert_cp(TextEdit *e, TextEditUndo *u, u32 cp) {
    if (!e || !e->buf || cp < 32 || cp == 127) return;
    if ((e->flags & TEXTEDIT_DIGITS) && (cp < '0' || cp > '9')) return;
    u8 enc[4];
    u32 n = utf8_encode(cp, enc);
    if (n == 0) return;
    undo_mark(u, e, /*coalescing=*/!textedit_has_selection(e));
    insert_bytes(e, (const char *)enc, (i32)n);
}

/* Move the caret, extending the selection when Shift is held. */
static void move_to(TextEdit *e, i32 off, bool extend) {
    *e->caret = off;
    if (!extend) *e->sel = off;
}

/* ---------------------------------------------------------- key dispatch */

bool textedit_key(TextEdit *e, TextEditUndo *u, i32 key, u32 mods) {
    if (!e || !e->buf || !e->len || !e->caret || !e->sel) return false;
    textedit_clamp(e);

    const char *b = e->buf;
    i32 len = *e->len;
    i32 c   = *e->caret;
    bool shift = (mods & MOD_SHIFT) != 0;
    bool super = (mods & MOD_SUPER) != 0;
    bool alt   = (mods & MOD_ALT)   != 0;
    bool ctrl  = (mods & MOD_CTRL)  != 0;

#ifdef PLATFORM_MACOS
    /* Cmd combined with Ctrl or Opt belongs to the host, not to a text field:
     * Liu binds those to window snapping and pane commands, and no macOS
     * editing shortcut uses them. Declining here keeps those working while a
     * field has focus, instead of the field eating them as "line start". */
    if (super && (ctrl || alt)) return false;
    bool edit_chord = super && !ctrl && !alt;   /* Cmd+A / C / X / V / Z */
    bool to_end     = super;                    /* Cmd+arrow, Cmd+Delete */
    bool by_word    = alt;                      /* Opt+arrow, Opt+Delete */
    bool emacs      = ctrl && !super && !alt;   /* Ctrl+A / E / K / U / W */
#else
    /* X11: the desktop's text-field conventions. Ctrl edits (Ctrl+A / C / X /
     * V / Z, with Shift too, so a terminal habit of Ctrl+Shift+V pastes here)
     * and moves or erases by word; Home / End go to the ends. Ctrl+Alt chords
     * are the app's (split, save, the vault), so they are declined like the
     * macOS Cmd+Ctrl ones. Of the Emacs set only the erasers stay — Ctrl+A is
     * select-all here. */
    if (ctrl && alt) return false;
    bool edit_chord = ctrl && !alt && !super;
    bool to_end     = false;
    bool by_word    = ctrl || alt;
    /* Not with Shift: Ctrl+Shift+W closes the tab while a field has focus. */
    bool emacs      = ctrl && !alt && !super && !shift;
#endif

    switch (key) {
    case KEY_LEFT:
        if (u) u->typing = false;
        if (to_end)        move_to(e, 0, shift);
        else if (by_word)  move_to(e, word_left(b, c), shift);
        /* Collapsing a selection to its near edge is the macOS behaviour;
         * a bare arrow must not also step a character. */
        else if (textedit_has_selection(e) && !shift) {
            i32 a, z; textedit_selection(e, &a, &z); move_to(e, a, false);
        }
        else move_to(e, prev_char(b, c), shift);
        return true;

    case KEY_RIGHT:
        if (u) u->typing = false;
        if (to_end)        move_to(e, len, shift);
        else if (by_word)  move_to(e, word_right(b, len, c), shift);
        else if (textedit_has_selection(e) && !shift) {
            i32 a, z; textedit_selection(e, &a, &z); move_to(e, z, false);
        }
        else move_to(e, next_char(b, len, c), shift);
        return true;

    /* Every branch below snapshots BEFORE mutating — taking it afterwards
     * would record the post-edit text and make Cmd+Z a no-op. */
    case KEY_BACKSPACE:
        if (textedit_has_selection(e)) {
            undo_mark(u, e, false);
            delete_selection(e);
            return true;
        }
        if (c == 0) return true;
        undo_mark(u, e, false);
#ifdef PLATFORM_MACOS
        bool to_start = super || (ctrl && !alt);    /* Cmd/Ctrl+Backspace: line start */
#else
        bool to_start = false;                      /* Ctrl+Backspace: a word, below */
#endif
        if (to_start)                 erase_range(e, 0, c);
        else if (by_word)             erase_range(e, word_left(b, c), c);
        else                          erase_range(e, prev_char(b, c), c);
        return true;

    case KEY_DELETE:
        if (textedit_has_selection(e)) {
            undo_mark(u, e, false);
            delete_selection(e);
            return true;
        }
        if (c >= len) return true;
        undo_mark(u, e, false);
        if (to_end)        erase_range(e, c, len);
        else if (by_word)  erase_range(e, c, word_right(b, len, c));
        else               erase_range(e, c, next_char(b, len, c));
        return true;

#ifndef PLATFORM_MACOS
    case KEY_HOME:
        if (u) u->typing = false;
        move_to(e, 0, shift);
        return true;
    case KEY_END:
        if (u) u->typing = false;
        move_to(e, len, shift);
        return true;
#endif

    default: break;
    }

    /* Emacs-style bindings macOS honours in every text field. */
    if (emacs) {
        switch (key) {
#ifdef PLATFORM_MACOS
        case KEY_A: move_to(e, 0, shift);   return true;   /* line start */
        case KEY_E: move_to(e, len, shift); return true;   /* line end   */
#endif
        case KEY_K: undo_mark(u, e, false); erase_range(e, c, len); return true;
        case KEY_U: undo_mark(u, e, false); erase_range(e, 0, c);   return true;
        case KEY_W: undo_mark(u, e, false);
                    erase_range(e, word_left(b, c), c);             return true;
        default: break;
        }
    }

    if (edit_chord) {
        switch (key) {
        case KEY_A:                       /* select all */
            *e->sel = 0;
            *e->caret = len;
            if (u) u->typing = false;
            return true;

        case KEY_C:
        case KEY_X: {
            /* Never lift a secret out of a masked field onto the clipboard. */
            if (e->flags & TEXTEDIT_SECRET) return true;
            if (!textedit_has_selection(e)) return true;
            i32 a, z;
            textedit_selection(e, &a, &z);
            char tmp[TEXTEDIT_UNDO_CAP];
            i32 n = z - a;
            if (n > (i32)sizeof tmp - 1) n = (i32)sizeof tmp - 1;
            while (n > 0 && is_cont(b, a + n)) n--;
            memcpy(tmp, b + a, (usize)n);
            tmp[n] = '\0';
            bool copied = platform_clipboard_set(tmp) == PLATFORM_CLIP_OK;
            /* Cut deletes only what actually reached the clipboard — a failed
             * write, or a selection longer than the copy buffer, keeps it. */
            if (key == KEY_X && copied && n == z - a) {
                undo_mark(u, e, false);
                erase_range(e, a, z);
            }
            return true;
        }

        case KEY_V: {
            const char *clip = platform_clipboard_get();
            if (!clip || !clip[0]) return true;
            undo_mark(u, e, false);
            /* Single-line field: a pasted newline becomes a space rather than
             * a control byte the renderer would have to special-case. */
            char tmp[512];
            i32 n = 0;
            bool digits = (e->flags & TEXTEDIT_DIGITS) != 0;
            for (const char *p = clip; *p && n < (i32)sizeof tmp - 1; p++) {
                char ch = *p;
                if (digits) {
                    if (ch < '0' || ch > '9') continue;   /* drop, don't stop */
                } else if ((u8)ch < 0x20 || ch == 0x7f) {
                    ch = ' ';
                }
                tmp[n++] = ch;
            }
            /* Only a paste that filled tmp can end mid-sequence; drop that
             * partial tail. `n` is one PAST the last byte written, so the
             * scan has to start at n-1 — reading tmp[n] tested a byte the
             * loop never wrote, and a stray continuation value there ate the
             * last character of every long paste. A paste that fit ends on a
             * whole character and is left alone. */
            if (n >= (i32)sizeof tmp - 1) {
                while (n > 0 && is_cont(tmp, n - 1)) n--;
                if (n > 0 && ((u8)tmp[n - 1] & 0xC0) == 0xC0) n--;
            }
            tmp[n] = '\0';
            insert_bytes(e, tmp, n);
            return true;
        }

        case KEY_Z:
            if (shift) do_redo(u, e);
            else       do_undo(u, e);
            return true;

        default: break;
        }
    }

    return false;
}
