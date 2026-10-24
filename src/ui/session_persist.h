/*
 * The saved active-tab index is converted from one window's tab array to the
 * flat array written to disk. Skipping tabs before it shifts the restored
 * selection, so the count and write loops must use the same predicate.
 */
#ifndef UI_SESSION_PERSIST_H
#define UI_SESSION_PERSIST_H

#include "core/types.h"

/* `kind` is a TabKind; taken as an i32 so this header stays free of ui.h.
 *
 * TAB_TERMINAL (0) is the only kind with a session behind it. A TAB_FILEBROWSER
 * holds a FileBrowser and a TAB_BROWSER a WKWebView — app_close_tab already
 * states it where it frees them: "File-browser and Browser tabs have no PTY/SSH
 * state to preserve". */
static inline bool session_tab_is_restorable(i32 kind) {
    return kind == 0;   /* TAB_TERMINAL */
}

/* Where `active` — an index into `kinds[0..count)` — lands in the array that
 * results from writing only the restorable tabs.
 *
 * When the active tab is itself not restorable it resolves to the slot its
 * position collapses into, i.e. the next restorable tab, clamped to the last
 * one. Returns 0 when nothing is restorable, which is also the index the
 * restorer clamps to for an empty array. */
static inline i32 session_active_flat_index(const i32 *kinds, i32 count,
                                            i32 active) {
    if (!kinds || count <= 0) return 0;
    if (active < 0) active = 0;
    if (active > count) active = count;

    i32 flat = 0;
    for (i32 i = 0; i < active && i < count; i++)
        if (session_tab_is_restorable(kinds[i])) flat++;

    i32 total = flat;
    for (i32 i = active; i < count; i++)
        if (session_tab_is_restorable(kinds[i])) total++;

    if (total == 0) return 0;
    if (flat >= total) flat = total - 1;
    return flat;
}

#endif /* UI_SESSION_PERSIST_H */
