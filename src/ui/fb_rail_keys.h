/*
 * Liu — which keys the explorer rail of a Code Editor tab takes.
 *
 * The docked explorer takes Delete, Cmd+Backspace and the other file keys
 * while it was the last thing clicked (main.c, "File-browser shortcuts when the
 * sidebar is focused"). The rail of a Code Editor tab is the same list in a tab
 * of its own: its Delete is the explorer's Delete, run with the rail as the
 * acting browser (AppState.fb_actor).
 *
 * A Code Editor tab has two typing surfaces, the rail and the document, and the
 * keyboard is one of them's. A click in the rail gives it to the rail (no caret
 * is drawn in the document: FileBrowser.kbd_away); a click in the document, or
 * any key the document takes, gives it back. While the rail has it, the keys
 * that mean "delete this file" are the rail's:
 *
 *   Delete (forward delete, the Del key; fn+Backspace on a Mac)   no modifier
 *   Cmd+Backspace (Ctrl+Backspace on X11: MOD_APP)                the Mac delete
 *
 * and a bare Backspace is not one: it is the key every text field expects.
 *
 * Pure: the decision from booleans, so every cell of the table can be checked.
 */
#ifndef UI_FB_RAIL_KEYS_H
#define UI_FB_RAIL_KEYS_H

#include "core/types.h"

typedef enum {
    FB_RAIL_KEY_NONE = 0,    /* not the rail's: the key goes on to the document */
    FB_RAIL_KEY_DELETE,      /* delete what is selected: the Trash, or ask first */
} FbRailKey;

typedef struct {
    bool rail_has_keys;      /* the last click was in the rail, no key the document took since */
    bool plain_list;         /* the rail shows files (not a graph's folder filter) */
    bool modal_open;         /* a menu, dialog, prompt or bar takes the keys before the list */
    bool forward_delete;     /* the key is Delete */
    bool backspace;          /* the key is Backspace */
    bool app_mod;            /* MOD_APP: Cmd on macOS, Ctrl on X11 */
    bool any_mod;            /* any modifier at all */
} FbRailKeyIn;

static inline FbRailKey fb_rail_key(const FbRailKeyIn *k) {
    if (!k || !k->rail_has_keys || !k->plain_list || k->modal_open) return FB_RAIL_KEY_NONE;
    if (k->forward_delete && !k->any_mod) return FB_RAIL_KEY_DELETE;
    if (k->backspace && k->app_mod) return FB_RAIL_KEY_DELETE;
    return FB_RAIL_KEY_NONE;
}

#endif /* UI_FB_RAIL_KEYS_H */
