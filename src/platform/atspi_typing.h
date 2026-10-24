/*
 * Text changes can include command output as well as characters already
 * echoed by the screen reader. Track typed keystrokes and split a matching
 * insertion into one update per key; preserve multi-character input-method
 * commits as a single update. Post unmatched changes as received and clear
 * pending keys. A recent key lets the next frame post immediately; unrelated
 * output remains interval-limited. Main-thread-only helpers for the X11
 * AT-SPI bridge.
 */
#ifndef LIU_PLATFORM_ATSPI_TYPING_H
#define LIU_PLATFORM_ATSPI_TYPING_H

#include "core/types.h"
#include "platform/atspi_text.h"

#define ATSPI_TYPED_UNITS 32   /* keystrokes remembered between two posts */
#define ATSPI_TYPED_SEC   1.0  /* a keystroke whose echo has not come by then never will */

/* Keystrokes into one window since the bridge last posted its text. Zeroed
 * with the window. */
typedef struct {
    f64 key_at;                       /* the last keystroke; 0 = none */
    u8  unit[ATSPI_TYPED_UNITS];      /* characters each keystroke typed, oldest first */
    u32 head, count;
} AtspiTyping;

static inline void atspi_typing_clear(AtspiTyping *ty) {
    ty->head = ty->count = 0;
}

/* A keystroke that typed `chars` characters (0 for a key that types nothing:
 * Backspace, an arrow, Return). Only a key that typed something is remembered
 * as a unit; every key may post early. */
static inline void atspi_typing_key(AtspiTyping *ty, f64 now, u32 chars) {
    if (ty->key_at > 0.0 && now - ty->key_at > ATSPI_TYPED_SEC) atspi_typing_clear(ty);
    ty->key_at = now;
    if (chars == 0) return;
    if (ty->count == ATSPI_TYPED_UNITS) {          /* the oldest gives way */
        ty->head = (ty->head + 1u) % ATSPI_TYPED_UNITS;
        ty->count--;
    }
    ty->unit[(ty->head + ty->count) % ATSPI_TYPED_UNITS] = (u8)(chars > 255u ? 255u : chars);
    ty->count++;
}

/* May a frame presented now post at once, past the interval? Only when a key
 * came after the last post, and recently. */
static inline bool atspi_typing_due(const AtspiTyping *ty, f64 last_post, f64 now) {
    return ty->key_at > 0.0 && ty->key_at > last_post && now - ty->key_at <= ATSPI_TYPED_SEC;
}

/* The pieces the edits `ed` (old text `o` -> new text `n`) are sent as. When
 * they are one insertion matching the oldest pending keystrokes, writes each
 * keystroke's length to `lens` (at most `cap`), drops those keystrokes and
 * returns how many; otherwise returns 0 (send the edits as they are) and drops
 * the pending keystrokes when an insertion did not match them. */
static inline u32 atspi_typing_split(AtspiTyping *ty, f64 now,
                                     const AtspiTextEdit *ed, u32 ne,
                                     const AtspiText *o, const AtspiText *n,
                                     u32 *lens, u32 cap) {
    bool inserts = false;
    for (u32 i = 0; i < ne; i++) if (!ed[i].del) inserts = true;
    if (!inserts) return 0;               /* a deletion or a caret move: keep them */
    if (ty->count == 0) return 0;
    if (now - ty->key_at > ATSPI_TYPED_SEC) { atspi_typing_clear(ty); return 0; }
    const AtspiTextEdit *e = &ed[0];
    bool typed = ne == 1 && !e->del && e->len > 0 &&
                 o->s.has_cursor && n->s.has_cursor &&
                 o->s.cursor16 == e->at && n->s.cursor16 == e->at + e->len &&
                 e->src + e->len <= n->n;
    for (u32 i = 0; typed && i < e->len; i++)
        if (n->cp[e->src + i] == '\n') typed = false;
    u32 m = 0, sum = 0;
    while (typed && m < ty->count && m < cap && sum < e->len) {
        u32 u = ty->unit[(ty->head + m) % ATSPI_TYPED_UNITS];
        lens[m++] = u;
        sum += u;
    }
    if (!typed || sum != e->len) {
        atspi_typing_clear(ty);
        return 0;
    }
    ty->head = (ty->head + m) % ATSPI_TYPED_UNITS;
    ty->count -= m;
    return m;
}

#endif /* LIU_PLATFORM_ATSPI_TYPING_H */
