/*
 * Liu — customizable keybinding system
 * Maps key+modifier combos to actions. Loaded from config, editable at runtime.
 */
#ifndef CORE_KEYBIND_H
#define CORE_KEYBIND_H

#include "core/types.h"
#include "platform/platform.h"

/* All bindable actions */
typedef enum {
    ACT_NONE = 0,

    /* Tab management */
    ACT_NEW_TAB,
    ACT_CLOSE_TAB,
    ACT_NEXT_TAB,
    ACT_PREV_TAB,
    ACT_TAB_1, ACT_TAB_2, ACT_TAB_3, ACT_TAB_4,
    ACT_TAB_5, ACT_TAB_6, ACT_TAB_7, ACT_TAB_8, ACT_TAB_9,

    /* View */
    ACT_TOGGLE_SIDEBAR,
    ACT_TOGGLE_FULLSCREEN,
    ACT_COMMAND_PALETTE,

    /* Font */
    ACT_FONT_BIGGER,
    ACT_FONT_SMALLER,
    ACT_FONT_RESET,

    /* Edit */
    ACT_COPY,
    ACT_PASTE,
    ACT_SELECT_ALL,
    ACT_FIND,
    ACT_CUT,                   /* cut the shell's command line (Cmd+X) */

    /* App */
    ACT_SETTINGS,
    ACT_SSH_CONNECT,
    ACT_IMPORT_SSH_CONFIG,

    /* Terminal */
    ACT_SCROLL_UP_PAGE,
    ACT_SCROLL_DOWN_PAGE,
    ACT_SCROLL_TO_TOP,
    ACT_SCROLL_TO_BOTTOM,
    ACT_CLEAR_SCREEN,
    ACT_BROADCAST_TOGGLE,

    /* Split */
    ACT_SPLIT_HORIZONTAL,
    ACT_SPLIT_VERTICAL,
    ACT_SAVE_FILE,

    /* Prompt navigation */
    ACT_PREV_PROMPT,
    ACT_NEXT_PROMPT,

    /* Tab rename */
    ACT_RENAME_TAB,

    /* Quake mode */
    ACT_QUAKE_TOGGLE,

    /* Tab groups */
    ACT_CREATE_TAB_GROUP,
    ACT_TOGGLE_TAB_GROUP,

    /* Undo close */
    ACT_UNDO_CLOSE_TAB,

    /* Smart Vault */
    ACT_VAULT_UNLOCK,          /* open unlock dialog (or lock if unlocked) */
    ACT_VAULT_BROWSER,         /* open vault browser overlay */
    ACT_VAULT_LOCK,            /* force lock (palette only) */
    ACT_VAULT_CHANGE_MASTER,   /* change master password (palette only) */

    /* Inline autosuggest */
    ACT_ACCEPT_SUGGESTION,     /* accept the ghost completion at the prompt */

    /* Sites / dev-server manager */
    ACT_SITE_MANAGER,          /* open the Sites overlay (Cmd+Shift+S) */

    /* Notes Vault → agent: pick a Vault doc and inject an @path source ref
     * into the active agent's prompt (Cmd+Shift+A). */
    ACT_VAULT_SOURCE,

    /* Voice input: toggle dictation (Cmd+Shift+M). Press once to start
     * recording, again to stop; the transcript is typed into whichever pane
     * is in front, agent or plain shell. */
    ACT_VOICE_INPUT,

    ACT_SKETCH,              /* command palette → sketch → image clipboard */

    /* A window of its own, and quitting every window. On macOS these are the
     * app menu's Cmd+N and Cmd+Q and have no table chord; Linux has no menu
     * bar to carry them. */
    ACT_NEW_WINDOW,
    ACT_QUIT,

    ACT_COUNT
} Action;

/* A single keybinding: key + modifiers → action */
typedef struct {
    KeyCode  key;
    u32      mods;     /* MOD_CTRL | MOD_SHIFT | MOD_ALT | MOD_SUPER */
    Action   action;
} KeyBinding;

#define MAX_KEYBINDINGS 64
/* keybindings.json's "keybindings_version". 2: the split moved off Cmd+T.
 * 3: written by a build with the Linux table — a Super chord in it is the
 * user's, never a leftover of the macOS table (keybind_load). */
#define KEYBIND_FILE_VERSION 3

typedef struct {
    KeyBinding bindings[MAX_KEYBINDINGS];
    i32        count;
} KeyBindTable;

/* Initialize with defaults */
void keybind_init_defaults(KeyBindTable *kb);

/* Lookup: returns Action for key+mods, or ACT_NONE */
Action keybind_lookup(const KeyBindTable *kb, KeyCode key, u32 mods);

/* Get binding for an action (for display in UI). Returns NULL if unbound. */
const KeyBinding *keybind_get(const KeyBindTable *kb, Action action);

/* Set/change a binding */
void keybind_set(KeyBindTable *kb, Action action, KeyCode key, u32 mods);

/* Remove binding for action */
void keybind_remove(KeyBindTable *kb, Action action);

/* The UI spells MOD_SUPER as Cmd on macOS and Super elsewhere so inline
 * shortcuts match keybind_format. */
#ifdef PLATFORM_MACOS
#define UI_MOD_PRIMARY "Cmd"
#else
#define UI_MOD_PRIMARY "Super"
#endif

/* The modifier of the app's own fixed chords outside a terminal pane — the
 * file browser, the editor and its Find bar, dialog fields, the image viewer —
 * and of a click on a link: Cmd on macOS; Ctrl on X11, where Super belongs to
 * the desktop shell (GNOME moves a window on Super+drag) and these keys are no
 * shell's. Inside a pane a bare Ctrl chord IS the shell's, which is why the
 * binding table uses Ctrl+Shift there. UI_MOD_APP is its word in a hint. */
#ifdef PLATFORM_MACOS
#define MOD_APP    MOD_SUPER
#define UI_MOD_APP "Cmd"
#else
#define MOD_APP    MOD_CTRL
#define UI_MOD_APP "Ctrl"
#endif

/* Fixed text that names a default chord (a seeded note, a Settings hint):
 * the platform's default, as the binding table has it. */
#ifdef PLATFORM_MACOS
#define UI_CHORD_PALETTE "Cmd+K"
#define UI_CHORD_COPY    "Cmd+C"
#else
#define UI_CHORD_PALETTE "Ctrl+Shift+P"
#define UI_CHORD_COPY    "Ctrl+Shift+C"
#endif

/* Format binding as human-readable string: "Cmd+Shift+T" (macOS) /
 * "Super+Shift+T" (elsewhere). */
const char *keybind_format(const KeyBinding *b);

/* Action name for display */
const char *action_name(Action a);

/* Save/load keybindings to/from JSON */
bool keybind_save(const KeyBindTable *kb, const char *path);
bool keybind_load(KeyBindTable *kb, const char *path);

#endif
