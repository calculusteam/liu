#ifndef LIU_X11_CLIPSAVE_H
#define LIU_X11_CLIPSAVE_H

/* Handing the clipboard to a clipboard manager when Liu quits.
 *
 * An X11 selection lives in its owner: when Liu exits, what it copied is gone
 * unless something else took it over. The freedesktop clipboard-manager
 * protocol is how an application asks: if the CLIPBOARD_MANAGER selection has
 * an owner, convert it to the target SAVE_TARGETS with a property naming the
 * targets to keep, then go on answering the manager's requests for the
 * CLIPBOARD (TARGETS, then each target) until its SelectionNotify says it has
 * taken the clipboard over; a property of None in that reply is a refusal. A
 * desktop with no manager (a bare window manager, KWin without Klipper) has no
 * owner of CLIPBOARD_MANAGER and the quit goes on at once.
 *
 * The manager can be slow or can never answer, and the quit must not hang on
 * it: the wait ends when nothing has happened for CLIP_SAVE_IDLE_SEC, and
 * never later than CLIP_SAVE_HARD_SEC after it began (a long transfer keeps
 * it alive; a silent manager costs one idle period). Pure: times are plain
 * numbers, so the decisions need no server. */

#include "core/types.h"

#define CLIP_SAVE_IDLE_SEC 1.0
#define CLIP_SAVE_HARD_SEC 2.5

typedef struct {
    bool owns_clipboard;     /* the server says our window owns CLIPBOARD */
    bool has_content;        /* text or an image to give */
    bool secret;             /* the copy is a password (platform_clipboard_set_secret) */
    bool manager_present;    /* CLIPBOARD_MANAGER has an owner */
    bool already_asked;      /* the handshake ran once in this process */
} ClipSaveFacts;

/* Whether to start the handshake. A secret is never handed on: it is for one
 * paste and is given up after its timer, and a manager keeps what it takes. */
static inline bool clip_save_wanted(const ClipSaveFacts *f) {
    return f && f->owns_clipboard && f->has_content && !f->secret &&
           f->manager_present && !f->already_asked;
}

/* The targets to ask the manager to keep, in the order Liu offers them. The
 * caller maps a kind to its atom. */
typedef enum { CLIP_SAVE_UTF8 = 0, CLIP_SAVE_TEXT, CLIP_SAVE_STRING, CLIP_SAVE_PNG } ClipSaveTarget;

static inline i32 clip_save_targets(bool text, bool png, ClipSaveTarget *out, i32 cap) {
    i32 n = 0;
    if (!out) return 0;
    if (png) {
        if (n < cap) out[n++] = CLIP_SAVE_PNG;
    } else if (text) {
        if (n < cap) out[n++] = CLIP_SAVE_UTF8;
        if (n < cap) out[n++] = CLIP_SAVE_TEXT;
        if (n < cap) out[n++] = CLIP_SAVE_STRING;
    }
    return n;
}

typedef enum {
    CLIP_SAVE_IDLE = 0,     /* not started */
    CLIP_SAVE_WAITING,      /* convert sent, no SelectionNotify yet */
    CLIP_SAVE_SAVED,        /* the manager took the clipboard over */
    CLIP_SAVE_REFUSED,      /* its SelectionNotify named no property */
    CLIP_SAVE_TIMED_OUT,    /* it did not finish in time */
} ClipSaveState;

typedef struct {
    ClipSaveState state;
    f64           began;      /* when the convert was sent */
    f64           deadline;   /* the wait ends here unless the manager is busy */
} ClipSave;

static inline void clip_save_begin(ClipSave *s, f64 now) {
    s->state = CLIP_SAVE_WAITING;
    s->began = now;
    s->deadline = now + CLIP_SAVE_IDLE_SEC;
}

/* The manager is working: it asked us for something. Gives it another idle
 * period, within the hard limit. */
static inline void clip_save_activity(ClipSave *s, f64 now) {
    if (s->state != CLIP_SAVE_WAITING) return;
    f64 d = now + CLIP_SAVE_IDLE_SEC, hard = s->began + CLIP_SAVE_HARD_SEC;
    s->deadline = d < hard ? d : hard;
}

/* The SelectionNotify for SAVE_TARGETS. Only the first counts. */
static inline void clip_save_notify(ClipSave *s, bool property_none) {
    if (s->state != CLIP_SAVE_WAITING) return;
    s->state = property_none ? CLIP_SAVE_REFUSED : CLIP_SAVE_SAVED;
}

/* Still waiting at `now`? Time running out ends it as TIMED_OUT. */
static inline bool clip_save_pending(ClipSave *s, f64 now) {
    if (s->state == CLIP_SAVE_WAITING && now >= s->deadline) s->state = CLIP_SAVE_TIMED_OUT;
    return s->state == CLIP_SAVE_WAITING;
}

/* How long the loop may sleep for the manager's next word, in milliseconds:
 * the time left, rounded up; 0 when it is not waiting. */
static inline i32 clip_save_wait_ms(const ClipSave *s, f64 now) {
    if (s->state != CLIP_SAVE_WAITING) return 0;
    f64 left = s->deadline - now;
    if (left <= 0.0) return 0;
    i32 ms = (i32)(left * 1000.0);
    return (f64)ms < left * 1000.0 ? ms + 1 : ms;
}

#endif
