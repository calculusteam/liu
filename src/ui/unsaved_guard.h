/*
 * Guard actions that would discard modified editor content.
 *
 * The modified flag is used by the editor indicator and Save, and checked
 * before opening another file, closing the viewer, quitting or overwriting a
 * file changed on disk.
 *
 * The first attempt is refused with an explanation. Repeating the same action
 * after UNSAVED_GUARD_MIN_SEC confirms it; an early repeat does not advance
 * the timer. A refusal expires so a later action requires a new confirmation.
 * A different action is checked independently, and an edit disarms the pending
 * confirmation.
 *
 * The guard is pure and keeps no window state. filebrowser.c checks it before
 * freeing editor text and displays FileBrowser.notice.
 */
#ifndef UI_UNSAVED_GUARD_H
#define UI_UNSAVED_GUARD_H

#include "core/types.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    UNSAVED_NONE = 0,
    UNSAVED_CLOSE,       /* close the viewer */
    UNSAVED_OPEN,        /* put another document where this one is */
    UNSAVED_QUIT,        /* quit, or close the tab or window that holds it */
    UNSAVED_OVERWRITE,   /* save over a file that changed on disk or is read-only */
} UnsavedAction;

typedef struct {
    u8  armed;           /* the UnsavedAction that was refused, or UNSAVED_NONE */
    f64 at;              /* when it was refused (the caller's clock, seconds) */
} UnsavedGuard;

/* A repeat sooner than this is the same press arriving twice. Longer than the
 * system's default double-click interval (0.5 s), so the second click of a
 * double click on another file does not discard the first file's edits. */
#define UNSAVED_GUARD_MIN_SEC    0.75
/* ...and one later than this is a new question. */
#define UNSAVED_GUARD_WINDOW_SEC 6.0

/* The key the messages name. */
#if defined(__APPLE__) || defined(PLATFORM_MACOS)
#define UNSAVED_SAVE_KEY "\xe2\x8c\x98" "S"          /* ⌘S */
#else
#define UNSAVED_SAVE_KEY "Ctrl+S"
#endif

static inline void unsaved_guard_disarm(UnsavedGuard *g) {
    if (g) { g->armed = UNSAVED_NONE; g->at = 0.0; }
}

/* Is this call the confirming repeat of `action`? True once: it disarms. */
static inline bool unsaved_guard_confirmed(UnsavedGuard *g, UnsavedAction action, f64 now) {
    if (!g || action == UNSAVED_NONE || g->armed != (u8)action) return false;
    f64 age = now - g->at;
    if (age < UNSAVED_GUARD_MIN_SEC || age > UNSAVED_GUARD_WINDOW_SEC) return false;
    unsaved_guard_disarm(g);
    return true;
}

/* Record that `action` was refused at `now`. The same action refused again
 * inside the minimum keeps its first stamp: the press that arrives twice must
 * neither confirm itself nor push the window along. */
static inline void unsaved_guard_refuse(UnsavedGuard *g, UnsavedAction action, f64 now) {
    if (!g) return;
    if (g->armed == (u8)action) {
        f64 age = now - g->at;
        if (age >= 0.0 && age < UNSAVED_GUARD_MIN_SEC) return;
    }
    g->armed = (u8)action;
    g->at = now;
}

/* May `action` go ahead? `at_stake` is "there is something unsaved to lose".
 * With nothing at stake: yes. Otherwise yes only for the confirming repeat;
 * any other call is refused and becomes the one a repeat can confirm. */
static inline bool unsaved_guard_allow(UnsavedGuard *g, UnsavedAction action,
                                       bool at_stake, f64 now) {
    if (!at_stake) { unsaved_guard_disarm(g); return true; }
    if (unsaved_guard_confirmed(g, action, now)) return true;
    unsaved_guard_refuse(g, action, now);
    return false;
}

/* `name` cut to a length a one-line notice can carry, on a character boundary,
 * with an ellipsis when something was left out. */
static inline void unsaved_guard_name(char *out, usize cap, const char *name) {
    enum { KEEP = 48 };
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!name) return;
    usize n = strlen(name);
    if (n <= KEEP) { snprintf(out, cap, "%s", name); return; }
    usize cut = KEEP;
    while (cut > 0 && ((unsigned char)name[cut] & 0xC0) == 0x80) cut--;
    snprintf(out, cap, "%.*s\xe2\x80\xa6", (int)cut, name);        /* … */
}

/* What a refusal says. `name` is the file's name, not its path. */
static inline void unsaved_guard_message(char *out, usize cap, UnsavedAction action,
                                         const char *name) {
    char shown[64];
    unsaved_guard_name(shown, sizeof shown, name && name[0] ? name : "This file");
    if (action == UNSAVED_OVERWRITE)
        snprintf(out, cap, "%s changed on disk \xe2\x80\x94 repeat " UNSAVED_SAVE_KEY
                 " to overwrite", shown);
    else
        snprintf(out, cap, "%s has unsaved changes \xe2\x80\x94 " UNSAVED_SAVE_KEY
                 " to save, repeat to discard", shown);
}

/* The same repeat rule guards a save over a file the user made read-only. */
static inline void unsaved_guard_message_readonly(char *out, usize cap, const char *name) {
    char shown[64];
    unsaved_guard_name(shown, sizeof shown, name && name[0] ? name : "This file");
    snprintf(out, cap, "%s is read-only \xe2\x80\x94 repeat " UNSAVED_SAVE_KEY
             " to overwrite", shown);
}

#endif /* UI_UNSAVED_GUARD_H */
