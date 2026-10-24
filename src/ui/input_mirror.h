/*
 * Liu — the typed mirror: liu's own copy of what a pane's prompt holds.
 *
 * Every byte typed into a pane goes into the mirror (AppState.
 * translate_input_shadow), and three readers take it at its word: the shell
 * history, the autosuggest ghost and the prompt-recall card. Translate does
 * not — it checks the mirror against the cells (terminal/cmdline.c) — but it
 * NEEDS one for a prompt on several rows, where cells cannot say what stands
 * between two of them. So a mirror that is thrown away is a prompt that
 * cannot be translated until it has been emptied and typed again, and
 * everyday keys — an arrow key, a key in another pane, a Shift+Enter — must
 * not throw it away.
 *
 * What is here is the part of keeping it that is arithmetic:
 *
 *   - the text with a CARET, so that a character typed after "left, left"
 *     goes in where the program puts it, and Backspace / Delete take the
 *     character the program takes;
 *   - where the line being typed starts (`line`): a Shift+Enter is a line
 *     break IN the mirror, and the prompt-recall card is still handed one
 *     line at a time;
 *   - a small stash of parked mirrors, by session, so that typing in another
 *     pane does not cost this one its own.
 *
 * What a key MEANS is the caller's business (main.c): which keys move the
 * caret by one, which leave it somewhere the mirror cannot follow, and for
 * which panes any of this is done at all. Characters are code points. A
 * model that is wrong about a program's editor shows as a cell that does not
 * read back, and costs a refused translation. What no cell shows — a blank
 * the caret went back over, the top of an input the program cuts — is taken
 * from the mirror only while it has followed every key (terminal/cmdline.c
 * says which that is).
 *
 * Pure: no AppState, no terminal.
 */
#ifndef UI_INPUT_MIRROR_H
#define UI_INPUT_MIRROR_H

#include "core/types.h"

#include <string.h>

#define INPUT_MIRROR_CAP   4096
/* Panes with a prompt half typed while the keyboard is somewhere else. */
#define INPUT_MIRROR_SLOTS 8

/* One mirror, wherever its bytes live: the window's own fields or a parked
 * slot. `text` holds INPUT_MIRROR_CAP bytes and stays NUL-terminated. */
typedef struct {
    char *text;
    i32  *len;
    i32  *caret;      /* byte offset the next character goes in at */
    i32  *line;       /* byte offset the line being typed starts at */
    bool *overflow;   /* bytes were lost: the mirror is not the prompt */
} InputMirror;

static inline void input_mirror_reset(const InputMirror *m) {
    m->text[0] = '\0';
    *m->len = 0;
    *m->caret = 0;
    *m->line = 0;
    *m->overflow = false;
}

/* Put the caret at the end: the state after liu has written the whole text. */
static inline void input_mirror_caret_end(const InputMirror *m) {
    *m->caret = *m->len;
}

/* `n` bytes in at the caret. What does not fit is dropped and the mirror says
 * so (`overflow`), as its append always has. */
static inline void input_mirror_insert(const InputMirror *m, const u8 *data, i32 n) {
    if (!data || n <= 0) return;
    i32 len = *m->len, at = *m->caret;
    if (at < 0) at = 0;
    if (at > len) at = len;
    i32 room = INPUT_MIRROR_CAP - 1 - len;
    if (n > room) *m->overflow = true;
    if (room <= 0) return;
    if (n > room) n = room;
    memmove(m->text + at + n, m->text + at, (usize)(len - at));
    memcpy(m->text + at, data, (usize)n);
    len += n;
    m->text[len] = '\0';
    *m->len = len;
    /* Typed AT the line's start it belongs to that line. */
    if (at < *m->line) *m->line += n;
    *m->caret = at + n;
}

/* The bytes [from, to) out. */
static inline void input_mirror_cut(const InputMirror *m, i32 from, i32 to) {
    i32 len = *m->len;
    if (from < 0) from = 0;
    if (to > len) to = len;
    if (to <= from) return;
    i32 n = to - from;
    memmove(m->text + from, m->text + to, (usize)(len - to));
    len -= n;
    m->text[len] = '\0';
    *m->len = len;
    if (*m->caret >= to)        *m->caret -= n;
    else if (*m->caret > from)  *m->caret = from;
    if (*m->line >= to)         *m->line -= n;
    else if (*m->line > from)   *m->line = from;
}

/* Start of the character before byte `at` / after it. */
static inline i32 input_mirror_prev(const InputMirror *m, i32 at) {
    if (at <= 0) return 0;
    at--;
    while (at > 0 && ((unsigned char)m->text[at] & 0xC0) == 0x80) at--;
    return at;
}

static inline i32 input_mirror_next(const InputMirror *m, i32 at) {
    i32 len = *m->len;
    if (at >= len) return len;
    at++;
    while (at < len && ((unsigned char)m->text[at] & 0xC0) == 0x80) at++;
    return at;
}

/* One character to the left of the caret is taken out. False when there was
 * none. */
static inline bool input_mirror_backspace(const InputMirror *m) {
    i32 at = *m->caret;
    if (at <= 0 || *m->len <= 0) return false;
    input_mirror_cut(m, input_mirror_prev(m, at), at);
    return true;
}

/* The character under the caret is taken out. False when the caret is at the
 * end. */
static inline bool input_mirror_delete(const InputMirror *m) {
    i32 at = *m->caret;
    if (at >= *m->len) return false;
    input_mirror_cut(m, at, input_mirror_next(m, at));
    return true;
}

/* False when the caret was already at that end: whether the program's own
 * caret moved is then something only the program knows. */
static inline bool input_mirror_left(const InputMirror *m) {
    if (*m->caret <= 0) return false;
    *m->caret = input_mirror_prev(m, *m->caret);
    return true;
}

static inline bool input_mirror_right(const InputMirror *m) {
    if (*m->caret >= *m->len) return false;
    *m->caret = input_mirror_next(m, *m->caret);
    return true;
}

/* Option+Backspace removes the preceding run of non-letter and non-digit
 * characters, then the preceding run of word characters. ASCII letters and
 * digits and every non-ASCII code point count as word characters. Returns
 * false when the caret is already at the beginning. */
static inline bool input_mirror_word_at(const InputMirror *m, i32 at) {
    unsigned char c = (unsigned char)m->text[at];
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static inline bool input_mirror_kill_word(const InputMirror *m) {
    i32 to = *m->caret, at = to;
    while (at > 0 && !input_mirror_word_at(m, input_mirror_prev(m, at))) at = input_mirror_prev(m, at);
    while (at > 0 && input_mirror_word_at(m, input_mirror_prev(m, at))) at = input_mirror_prev(m, at);
    if (at == to) return false;
    input_mirror_cut(m, at, to);
    return true;
}

/* A line break typed with the key (Shift+Enter): it is in the prompt, and the
 * line being typed starts after it. */
static inline void input_mirror_newline(const InputMirror *m) {
    const u8 nl = '\n';
    i32 before = *m->len;
    input_mirror_insert(m, &nl, 1);
    if (*m->len > before) *m->line = *m->caret;
}

/* ---- parked mirrors -------------------------------------------------------
 *
 * Keyed by the session's instance id (session_instance_id): a number that is
 * never handed out twice, so a slot can outlive its pane without ever being
 * taken for another one's. */
typedef struct {
    u64  owner;       /* 0 = free */
    u64  seq;         /* parked later = larger */
    i32  len, caret, line;
    bool overflow;
    bool unsure;      /* see AppState.translate_input_shadow_unsure */
    bool loose;       /* ...and _loose */
    bool stale;       /* AppState.cmdline_hint_stale as it was */
    char text[INPUT_MIRROR_CAP];
} InputMirrorSlot;

typedef struct {
    InputMirrorSlot slot[INPUT_MIRROR_SLOTS];
    u64 seq;
} InputMirrorStash;

static inline InputMirrorSlot *input_mirror_stash_find(InputMirrorStash *st, u64 owner) {
    if (!st || !owner) return NULL;
    for (i32 i = 0; i < INPUT_MIRROR_SLOTS; i++)
        if (st->slot[i].owner == owner) return &st->slot[i];
    return NULL;
}

/* The slot to park `owner`'s mirror in: its own if it has one, a free one, or
 * the one parked longest ago (that pane's prompt is then simply not mirrored
 * any more). The slot comes back empty. */
static inline InputMirrorSlot *input_mirror_stash_claim(InputMirrorStash *st, u64 owner) {
    if (!st || !owner) return NULL;
    InputMirrorSlot *sl = input_mirror_stash_find(st, owner);
    for (i32 i = 0; !sl && i < INPUT_MIRROR_SLOTS; i++)
        if (!st->slot[i].owner) sl = &st->slot[i];
    if (!sl) {
        sl = &st->slot[0];
        for (i32 i = 1; i < INPUT_MIRROR_SLOTS; i++)
            if (st->slot[i].seq < sl->seq) sl = &st->slot[i];
    }
    sl->owner = owner;
    sl->seq = ++st->seq;
    sl->len = sl->caret = sl->line = 0;
    sl->overflow = sl->unsure = sl->loose = sl->stale = false;
    sl->text[0] = '\0';
    return sl;
}

static inline void input_mirror_stash_drop(InputMirrorStash *st, u64 owner) {
    InputMirrorSlot *sl = input_mirror_stash_find(st, owner);
    if (sl) { sl->owner = 0; sl->len = 0; sl->text[0] = '\0'; }
}

static inline InputMirror input_mirror_of_slot(InputMirrorSlot *sl) {
    return (InputMirror){ sl->text, &sl->len, &sl->caret, &sl->line, &sl->overflow };
}

#endif /* UI_INPUT_MIRROR_H */
