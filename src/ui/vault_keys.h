/*
 * Keyboard routing for the Smart Vault list and its filter field.
 *
 * The filter handles editing chords such as Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V,
 * and Ctrl+Z on X11. The list handles Ctrl/Cmd+C to copy a selected secret,
 * Ctrl/Cmd+N to create an entry, and Ctrl/Cmd+E to edit it. Resolve list
 * shortcuts before forwarding input to the filter; when the filter has a
 * selection, Ctrl/Cmd+C copies the filter text.
 *
 * This helper has no UI state.
 */
#ifndef UI_VAULT_KEYS_H
#define UI_VAULT_KEYS_H

#include "core/types.h"
#include "platform/platform.h"

typedef enum {
    VAULT_LIST_KEY_NONE = 0,   /* not the list's: the filter box and the arrows next */
    VAULT_LIST_KEY_COPY,       /* copy the selected secret (a secret copy) */
    VAULT_LIST_KEY_NEW,        /* a new entry */
    VAULT_LIST_KEY_EDIT,       /* edit the selected entry */
} VaultListKey;

static inline VaultListKey vault_list_key(u32 key, u32 mods, bool filter_has_selection) {
    if (!(mods & (MOD_CTRL | MOD_SUPER))) return VAULT_LIST_KEY_NONE;
    if (key == KEY_C) return filter_has_selection ? VAULT_LIST_KEY_NONE : VAULT_LIST_KEY_COPY;
    if (key == KEY_N) return VAULT_LIST_KEY_NEW;
    if (key == KEY_E) return VAULT_LIST_KEY_EDIT;
    return VAULT_LIST_KEY_NONE;
}

#endif /* UI_VAULT_KEYS_H */
