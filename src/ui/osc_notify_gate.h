/*
 * Liu — how often a program in a pane may ask for the user's attention.
 *
 * An in-band notification (OSC 9 / 777 / 99, vt_parser.c) is one short string
 * any output can print: `cat` of the wrong file, a loop gone wrong, a remote
 * machine. Each one becomes a mark on a tab and, with Liu in the background, a
 * banner and a Dock bounce — so they are admitted one at a time: at most one
 * every OSC_NOTIFY_MIN_GAP_SEC from the whole process, and the same words not
 * again for OSC_NOTIFY_SAME_GAP_SEC. (The bell's banner has the same kind of
 * limit, so `yes $'\a'` is not a notification storm.)
 *
 * Pure, so it can be checked without a window.
 */
#ifndef UI_OSC_NOTIFY_GATE_H
#define UI_OSC_NOTIFY_GATE_H

#include "core/types.h"

#define OSC_NOTIFY_MIN_GAP_SEC   2.0
#define OSC_NOTIFY_SAME_GAP_SEC 10.0

typedef struct {
    f64 last_at;        /* when one was last admitted */
    u64 last_hash;      /* what it said */
    bool any;
} OscNotifyGate;

static inline u64 osc_notify_hash(const char *title, const char *body) {
    u64 h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)(title ? title : ""); *p; p++) { h ^= *p; h *= 1099511628211ull; }
    h ^= 0x1f; h *= 1099511628211ull;                    /* "ab" + "c" is not "a" + "bc" */
    for (const unsigned char *p = (const unsigned char *)(body ? body : ""); *p; p++) { h ^= *p; h *= 1099511628211ull; }
    return h;
}

/* May this one through? Updates the gate when it may. */
static inline bool osc_notify_admit(OscNotifyGate *g, const char *title, const char *body, f64 now) {
    u64 h = osc_notify_hash(title, body);
    if (g->any) {
        f64 since = now - g->last_at;
        if (since < 0.0) since = OSC_NOTIFY_SAME_GAP_SEC;      /* the clock went back: start over */
        if (since < OSC_NOTIFY_MIN_GAP_SEC) return false;
        if (h == g->last_hash && since < OSC_NOTIFY_SAME_GAP_SEC) return false;
    }
    g->any = true;
    g->last_at = now;
    g->last_hash = h;
    return true;
}

#endif /* UI_OSC_NOTIFY_GATE_H */
