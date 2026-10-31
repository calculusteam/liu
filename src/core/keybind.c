/*
 * Liu — keybinding system implementation
 */
#include "core/keybind.h"
#include "core/atomic_file.h"
#include "core/json_lines.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* =========================================================================
 * Action names (for display + config serialization)
 * ========================================================================= */

static const char *g_action_names[] = {
    [ACT_NONE]              = "none",
    [ACT_SKETCH]            = "Sketch",
    [ACT_NEW_WINDOW]        = "New Window",
    [ACT_QUIT]              = "Quit",
    [ACT_NEW_TAB]           = "New Tab",
    [ACT_CLOSE_TAB]         = "Close Tab",
    [ACT_NEXT_TAB]          = "Next Tab",
    [ACT_PREV_TAB]          = "Previous Tab",
    [ACT_TAB_1]             = "Tab 1",
    [ACT_TAB_2]             = "Tab 2",
    [ACT_TAB_3]             = "Tab 3",
    [ACT_TAB_4]             = "Tab 4",
    [ACT_TAB_5]             = "Tab 5",
    [ACT_TAB_6]             = "Tab 6",
    [ACT_TAB_7]             = "Tab 7",
    [ACT_TAB_8]             = "Tab 8",
    [ACT_TAB_9]             = "Tab 9",
    [ACT_TOGGLE_SIDEBAR]    = "Toggle Sidebar",
    [ACT_TOGGLE_FULLSCREEN] = "Toggle Fullscreen",
    [ACT_COMMAND_PALETTE]   = "Command Palette",
    [ACT_FONT_BIGGER]       = "Increase Font",
    [ACT_FONT_SMALLER]      = "Decrease Font",
    [ACT_FONT_RESET]        = "Reset Font Size",
    [ACT_COPY]              = "Copy",
    [ACT_PASTE]             = "Paste",
    [ACT_SELECT_ALL]        = "Select All",
    [ACT_CUT]               = "Cut",
    [ACT_FIND]              = "Find",
    [ACT_SETTINGS]          = "Settings",
    [ACT_SSH_CONNECT]       = "SSH Connect",
    [ACT_IMPORT_SSH_CONFIG] = "Import SSH Config",
    [ACT_SCROLL_UP_PAGE]    = "Scroll Up Page",
    [ACT_SCROLL_DOWN_PAGE]  = "Scroll Down Page",
    [ACT_SCROLL_TO_TOP]     = "Scroll to Top",
    [ACT_SCROLL_TO_BOTTOM]  = "Scroll to Bottom",
    [ACT_CLEAR_SCREEN]      = "Clear Screen",
    [ACT_BROADCAST_TOGGLE]  = "Toggle Broadcast",
    [ACT_SPLIT_HORIZONTAL]  = "Split Horizontal",
    [ACT_SPLIT_VERTICAL]    = "Split Vertical",
    [ACT_SAVE_FILE]         = "Save File",
    [ACT_FOCUS_PANE_LEFT]   = "Focus Pane Left",
    [ACT_FOCUS_PANE_RIGHT]  = "Focus Pane Right",
    [ACT_FOCUS_PANE_UP]     = "Focus Pane Above",
    [ACT_FOCUS_PANE_DOWN]   = "Focus Pane Below",
    [ACT_NEXT_PANE]         = "Next Pane",
    [ACT_PREV_PANE]         = "Previous Pane",
    [ACT_TOGGLE_PANE_ZOOM]  = "Toggle Pane Zoom",
    [ACT_PREV_PROMPT]       = "Previous Prompt",
    [ACT_NEXT_PROMPT]       = "Next Prompt",
    [ACT_UNDO_CLOSE_TAB]   = "Undo Close Tab",
    [ACT_QUAKE_TOGGLE]      = "Toggle Quake Mode",
    [ACT_CREATE_TAB_GROUP]  = "Create Tab Group",
    [ACT_TOGGLE_TAB_GROUP]  = "Toggle Tab Group",
    [ACT_RENAME_TAB]        = "Rename Tab",
    [ACT_VAULT_UNLOCK]      = "Vault Unlock",
    [ACT_VAULT_BROWSER]     = "Vault Browser",
    [ACT_VAULT_LOCK]        = "Vault Lock",
    [ACT_VAULT_CHANGE_MASTER] = "Vault Change Master Password",
    [ACT_ACCEPT_SUGGESTION] = "Accept Suggestion",
    [ACT_SITE_MANAGER]      = "Site Manager",
    [ACT_VAULT_SOURCE]      = "Add Vault Source",
    [ACT_VOICE_INPUT]       = "Voice Input",
};

const char *action_name(Action a) {
    if (a >= 0 && a < ACT_COUNT && g_action_names[a])
        return g_action_names[a];
    return "Unknown";
}

/* =========================================================================
 * Defaults — macOS style (Cmd = Super), and the Linux / X11 table below it
 * ========================================================================= */

void keybind_init_defaults(KeyBindTable *kb) {
    memset(kb, 0, sizeof(*kb));

    #define BIND(act, k, m) do { \
        kb->bindings[kb->count++] = (KeyBinding){ .key = k, .mods = m, .action = act }; \
    } while(0)

#ifdef PLATFORM_MACOS
    /* Tab management */
    BIND(ACT_NEW_TAB,         KEY_T,  MOD_SUPER);
    BIND(ACT_CLOSE_TAB,       KEY_W,  MOD_SUPER);
    BIND(ACT_NEXT_TAB,        KEY_TAB, MOD_CTRL);
    BIND(ACT_PREV_TAB,        KEY_TAB, MOD_CTRL | MOD_SHIFT);
    BIND(ACT_TAB_1,           KEY_1,  MOD_SUPER);
    BIND(ACT_TAB_2,           KEY_2,  MOD_SUPER);
    BIND(ACT_TAB_3,           KEY_3,  MOD_SUPER);
    BIND(ACT_TAB_4,           KEY_4,  MOD_SUPER);
    BIND(ACT_TAB_5,           KEY_5,  MOD_SUPER);
    BIND(ACT_TAB_6,           KEY_6,  MOD_SUPER);
    BIND(ACT_TAB_7,           KEY_7,  MOD_SUPER);
    BIND(ACT_TAB_8,           KEY_8,  MOD_SUPER);
    BIND(ACT_TAB_9,           KEY_9,  MOD_SUPER);

    /* View */
    BIND(ACT_TOGGLE_SIDEBAR,  KEY_B,  MOD_SUPER);
    BIND(ACT_COMMAND_PALETTE, KEY_K,  MOD_SUPER);
    BIND(ACT_SITE_MANAGER,    KEY_S,  MOD_SUPER | MOD_SHIFT);
    BIND(ACT_VAULT_SOURCE,    KEY_A,  MOD_SUPER | MOD_SHIFT);  /* pick a Vault doc → agent */
    /* M for microphone. Cmd+Shift+V is already the Vault browser, and no
     * NSMenu item claims "m", so this reaches the app loop rather than being
     * swallowed as a menu key equivalent. */
    BIND(ACT_VOICE_INPUT,     KEY_M,  MOD_SUPER | MOD_SHIFT);

    /* Font */
    BIND(ACT_FONT_BIGGER,     KEY_UNKNOWN, MOD_SUPER); /* Cmd++ handled via char */
    BIND(ACT_FONT_SMALLER,    KEY_UNKNOWN, MOD_SUPER); /* Cmd+- handled via char */
    BIND(ACT_FONT_RESET,      KEY_0,  MOD_SUPER);

    /* Edit */
    BIND(ACT_COPY,            KEY_C,  MOD_SUPER);
    BIND(ACT_PASTE,           KEY_V,  MOD_SUPER);
    BIND(ACT_SELECT_ALL,      KEY_A,  MOD_SUPER);
    BIND(ACT_CUT,             KEY_X,  MOD_SUPER);
    BIND(ACT_FIND,            KEY_F,  MOD_SUPER);

    /* App */
    BIND(ACT_SETTINGS,        KEY_UNKNOWN, MOD_SUPER); /* Cmd+, via menu */
    BIND(ACT_SSH_CONNECT,     KEY_UNKNOWN, MOD_SUPER); /* via menu/toolbar */

    /* Scroll */
    BIND(ACT_SCROLL_UP_PAGE,   KEY_PAGE_UP,   MOD_SHIFT);
    BIND(ACT_SCROLL_DOWN_PAGE, KEY_PAGE_DOWN,  MOD_SHIFT);
    BIND(ACT_SCROLL_TO_TOP,    KEY_HOME,       MOD_SHIFT);
    BIND(ACT_SCROLL_TO_BOTTOM, KEY_END,        MOD_SHIFT);

    /* Jump between shell prompts in scrollback. Plain Cmd+Up / Cmd+Down are
     * the macOS "scroll to top / bottom of document" convention in TextEdit-
     * style fields — users coming from other apps expect that, so prompt
     * jumps use Cmd+Shift+Up/Down instead. The other Cmd+Arrow combos remain
     * reserved: window snap is Cmd+Ctrl+Arrows, pane focus is Cmd+Alt+Arrows
     * and the divider nudge Cmd+Ctrl+Alt+Arrows. */
    BIND(ACT_PREV_PROMPT,      KEY_UP,         MOD_SUPER | MOD_SHIFT);
    BIND(ACT_NEXT_PROMPT,      KEY_DOWN,       MOD_SUPER | MOD_SHIFT);

    /* Clear */
    BIND(ACT_CLEAR_SCREEN,    KEY_K,  MOD_SUPER | MOD_SHIFT);

    /* Split */
    BIND(ACT_SPLIT_HORIZONTAL, KEY_D,  MOD_SUPER);
    BIND(ACT_SPLIT_VERTICAL,   KEY_D,  MOD_SUPER | MOD_SHIFT);

    /* Panes — Cmd+Option and an arrow to the neighbour, Cmd+[ / Cmd+] through
     * them in reading order, Cmd+Shift+Enter to fill the tab with one. */
    BIND(ACT_FOCUS_PANE_LEFT,  KEY_LEFT,  MOD_SUPER | MOD_ALT);
    BIND(ACT_FOCUS_PANE_RIGHT, KEY_RIGHT, MOD_SUPER | MOD_ALT);
    BIND(ACT_FOCUS_PANE_UP,    KEY_UP,    MOD_SUPER | MOD_ALT);
    BIND(ACT_FOCUS_PANE_DOWN,  KEY_DOWN,  MOD_SUPER | MOD_ALT);
    BIND(ACT_PREV_PANE,        KEY_LEFT_BRACKET,  MOD_SUPER);
    BIND(ACT_NEXT_PANE,        KEY_RIGHT_BRACKET, MOD_SUPER);
    BIND(ACT_TOGGLE_PANE_ZOOM, KEY_ENTER, MOD_SUPER | MOD_SHIFT);

    /* Broadcast */
    BIND(ACT_BROADCAST_TOGGLE, KEY_I,  MOD_SUPER | MOD_SHIFT);

    /* File */
    BIND(ACT_SAVE_FILE,       KEY_S,  MOD_SUPER);

    /* Tab groups — palette only by default; Cmd+Shift+G is Changes (SCM). */
    /* BIND(ACT_CREATE_TAB_GROUP, KEY_G,  MOD_SUPER | MOD_SHIFT); */

    /* Undo close tab */
    BIND(ACT_UNDO_CLOSE_TAB,  KEY_T,  MOD_SUPER | MOD_SHIFT);

    /* Rename active tab (Cmd+Shift+E). Double-click on a tab also works. */
    BIND(ACT_RENAME_TAB,      KEY_E,  MOD_SUPER | MOD_SHIFT);

    /* Smart Vault — Cmd+Shift+V opens the browser (unlock prompt first
     * if the vault is still locked). Cmd+V is paste; don't collide. */
    BIND(ACT_VAULT_BROWSER,   KEY_V,  MOD_SUPER | MOD_SHIFT);
#else
    /* Super is commonly reserved by the desktop shell, so application
     * shortcuts use Ctrl and Alt combinations. Ctrl+C sends SIGINT and Ctrl+V
     * is the shell's literal-next command; terminal clipboard shortcuts use
     * Ctrl+Shift.
     *
     * When macOS shortcuts would collide after modifier translation, such as
     * Cmd+T and Cmd+Shift+T, the second action uses Ctrl+Alt+<key> because
     * keybind_lookup returns the first match. Tab-by-index uses Alt+<n>. */

    /* Tab management */
    BIND(ACT_NEW_TAB,         KEY_T,  MOD_CTRL | MOD_SHIFT);
    BIND(ACT_CLOSE_TAB,       KEY_W,  MOD_CTRL | MOD_SHIFT);
    BIND(ACT_NEXT_TAB,        KEY_TAB, MOD_CTRL);
    BIND(ACT_PREV_TAB,        KEY_TAB, MOD_CTRL | MOD_SHIFT);
    BIND(ACT_TAB_1,           KEY_1,  MOD_ALT);
    BIND(ACT_TAB_2,           KEY_2,  MOD_ALT);
    BIND(ACT_TAB_3,           KEY_3,  MOD_ALT);
    BIND(ACT_TAB_4,           KEY_4,  MOD_ALT);
    BIND(ACT_TAB_5,           KEY_5,  MOD_ALT);
    BIND(ACT_TAB_6,           KEY_6,  MOD_ALT);
    BIND(ACT_TAB_7,           KEY_7,  MOD_ALT);
    BIND(ACT_TAB_8,           KEY_8,  MOD_ALT);
    BIND(ACT_TAB_9,           KEY_9,  MOD_ALT);

    /* View */
    BIND(ACT_TOGGLE_SIDEBAR,  KEY_B,  MOD_CTRL | MOD_SHIFT);
    /* F11 is the fullscreen key on Linux desktops; macOS reaches fullscreen
     * through the native View menu (Ctrl+Cmd+F). */
    BIND(ACT_TOGGLE_FULLSCREEN, KEY_F11, MOD_NONE);
    /* Ctrl+Shift+P is the editor convention; Super+K stays as a second chord
     * because it is the macOS habit and neither GNOME nor KDE claims it. */
    BIND(ACT_COMMAND_PALETTE, KEY_P,  MOD_CTRL | MOD_SHIFT);
    BIND(ACT_COMMAND_PALETTE, KEY_K,  MOD_SUPER);
    BIND(ACT_SITE_MANAGER,    KEY_S,  MOD_CTRL | MOD_SHIFT);
    BIND(ACT_VAULT_SOURCE,    KEY_A,  MOD_CTRL | MOD_ALT);   /* Ctrl+Shift+A selects all */
    BIND(ACT_VOICE_INPUT,     KEY_M,  MOD_CTRL | MOD_SHIFT);

    /* Font — there is no View menu to carry Cmd+= / Cmd+- here. Ctrl+= and
     * Ctrl+Shift+= (Ctrl++ on a US layout) both grow the text. */
    BIND(ACT_FONT_BIGGER,     KEY_EQUAL, MOD_CTRL);
    BIND(ACT_FONT_BIGGER,     KEY_EQUAL, MOD_CTRL | MOD_SHIFT);
    BIND(ACT_FONT_SMALLER,    KEY_MINUS, MOD_CTRL);
    BIND(ACT_FONT_RESET,      KEY_0,  MOD_CTRL);

    /* Edit — Ctrl+Shift, since bare Ctrl+C / Ctrl+V belong to the shell. */
    BIND(ACT_COPY,            KEY_C,  MOD_CTRL | MOD_SHIFT);
    BIND(ACT_PASTE,           KEY_V,  MOD_CTRL | MOD_SHIFT);
    BIND(ACT_SELECT_ALL,      KEY_A,  MOD_CTRL | MOD_SHIFT);
    BIND(ACT_CUT,             KEY_X,  MOD_CTRL | MOD_SHIFT);
    BIND(ACT_FIND,            KEY_F,  MOD_CTRL | MOD_SHIFT);

    /* App — no menu bar here, so Settings needs a real key. */
    BIND(ACT_SETTINGS,        KEY_COMMA, MOD_CTRL);
    BIND(ACT_SSH_CONNECT,     KEY_UNKNOWN, MOD_CTRL); /* via the toolbar */

    /* Scroll */
    BIND(ACT_SCROLL_UP_PAGE,   KEY_PAGE_UP,   MOD_SHIFT);
    BIND(ACT_SCROLL_DOWN_PAGE, KEY_PAGE_DOWN,  MOD_SHIFT);
    BIND(ACT_SCROLL_TO_TOP,    KEY_HOME,       MOD_SHIFT);
    BIND(ACT_SCROLL_TO_BOTTOM, KEY_END,        MOD_SHIFT);

    /* Prompt jumps: <Super><Shift>Up/Down is the window manager's move to
     * another monitor. */
    BIND(ACT_PREV_PROMPT,      KEY_UP,         MOD_CTRL | MOD_SHIFT);
    BIND(ACT_NEXT_PROMPT,      KEY_DOWN,       MOD_CTRL | MOD_SHIFT);

    /* Clear — Ctrl+L is readline's own clear-screen, so the app's takes Shift. */
    BIND(ACT_CLEAR_SCREEN,    KEY_L,  MOD_CTRL | MOD_SHIFT);

    /* Split — Ctrl+Shift+D side by side; stacked takes Ctrl+Alt+D, Shift is
     * already spent. */
    BIND(ACT_SPLIT_HORIZONTAL, KEY_D,  MOD_CTRL | MOD_SHIFT);
    BIND(ACT_SPLIT_VERTICAL,   KEY_D,  MOD_CTRL | MOD_ALT);

    /* Panes — Alt+Shift and an arrow to the neighbour: Ctrl+Shift+Up/Down
     * are the prompt jumps, Ctrl+Alt+Left/Right belong to the desktop's
     * workspace switch and Super+Alt+Arrows nudge the divider. Ctrl+Shift+[ / ]
     * go through the panes in order and Ctrl+Shift+Z fills the tab with one. */
    BIND(ACT_FOCUS_PANE_LEFT,  KEY_LEFT,  MOD_ALT | MOD_SHIFT);
    BIND(ACT_FOCUS_PANE_RIGHT, KEY_RIGHT, MOD_ALT | MOD_SHIFT);
    BIND(ACT_FOCUS_PANE_UP,    KEY_UP,    MOD_ALT | MOD_SHIFT);
    BIND(ACT_FOCUS_PANE_DOWN,  KEY_DOWN,  MOD_ALT | MOD_SHIFT);
    BIND(ACT_PREV_PANE,        KEY_LEFT_BRACKET,  MOD_CTRL | MOD_SHIFT);
    BIND(ACT_NEXT_PANE,        KEY_RIGHT_BRACKET, MOD_CTRL | MOD_SHIFT);
    BIND(ACT_TOGGLE_PANE_ZOOM, KEY_Z, MOD_CTRL | MOD_SHIFT);

    /* Broadcast */
    BIND(ACT_BROADCAST_TOGGLE, KEY_I,  MOD_CTRL | MOD_SHIFT);

    /* File — not Ctrl+S, which is XOFF to the tty and freezes the pane for
     * anyone who presses it from editor habit; Ctrl+Shift+S is the site
     * manager, so saving takes Ctrl+Alt+S. */
    BIND(ACT_SAVE_FILE,       KEY_S,  MOD_CTRL | MOD_ALT);

    /* Ctrl+Shift+T is the new tab (the terminal convention wins over the
     * browser's reopen), so reopening moves one modifier over. */
    BIND(ACT_UNDO_CLOSE_TAB,  KEY_T,  MOD_CTRL | MOD_ALT);
    BIND(ACT_RENAME_TAB,      KEY_E,  MOD_CTRL | MOD_SHIFT);
    BIND(ACT_VAULT_BROWSER,   KEY_V,  MOD_CTRL | MOD_ALT);   /* Ctrl+Shift+V pastes */

    /* Windows */
    BIND(ACT_NEW_WINDOW,      KEY_N,  MOD_CTRL | MOD_SHIFT);
    BIND(ACT_QUIT,            KEY_Q,  MOD_CTRL | MOD_SHIFT);
#endif

    /* Inline autosuggest — Right arrow accepts the ghost completion (fish
     * style). Only consumed when a suggestion is showing; otherwise the key
     * keeps its normal cursor-move meaning. Rebindable. */
    BIND(ACT_ACCEPT_SUGGESTION, KEY_RIGHT, 0);

    #undef BIND
}

/* =========================================================================
 * Lookup
 * ========================================================================= */

Action keybind_lookup(const KeyBindTable *kb, KeyCode key, u32 mods) {
    if (key == KEY_UNKNOWN) return ACT_NONE;
    for (i32 i = 0; i < kb->count; i++) {
        if (kb->bindings[i].key == key && kb->bindings[i].mods == mods)
            return kb->bindings[i].action;
    }
    return ACT_NONE;
}

const KeyBinding *keybind_get(const KeyBindTable *kb, Action action) {
    for (i32 i = 0; i < kb->count; i++) {
        if (kb->bindings[i].action == action) return &kb->bindings[i];
    }
    return NULL;
}

void keybind_set(KeyBindTable *kb, Action action, KeyCode key, u32 mods) {
    if (!kb || action == ACT_NONE) return;

    /* A key chord can only dispatch one action. Remove any previous owner
     * before updating this action so loaded/custom bindings are not shadowed
     * by a default that happens to appear earlier in the table. */
    if (key != KEY_UNKNOWN) {
        for (i32 i = 0; i < kb->count; ) {
            if (kb->bindings[i].action != action &&
                kb->bindings[i].key == key &&
                kb->bindings[i].mods == mods) {
                memmove(&kb->bindings[i], &kb->bindings[i + 1],
                        (usize)(kb->count - i - 1) * sizeof(KeyBinding));
                kb->count--;
                continue;
            }
            i++;
        }
    }

    /* Update existing or append */
    for (i32 i = 0; i < kb->count; i++) {
        if (kb->bindings[i].action == action) {
            kb->bindings[i].key = key;
            kb->bindings[i].mods = mods;
            return;
        }
    }
    if (kb->count < MAX_KEYBINDINGS) {
        kb->bindings[kb->count++] = (KeyBinding){ .key = key, .mods = mods, .action = action };
    }
}

/* Every chord of the action goes: "Unbound" in keybindings.json and Delete in
 * Settings mean the action has none. An action can have two default chords
 * (the palette's Super+K on Linux); leaving the second would keep the cleared
 * action bound, and the next save would write that chord as the user's. */
void keybind_remove(KeyBindTable *kb, Action action) {
    for (i32 i = 0; i < kb->count; ) {
        if (kb->bindings[i].action == action) {
            memmove(&kb->bindings[i], &kb->bindings[i+1],
                    (usize)(kb->count - i - 1) * sizeof(KeyBinding));
            kb->count--;
            continue;
        }
        i++;
    }
}

/* =========================================================================
 * Format binding as string
 * ========================================================================= */

static const char *key_name(KeyCode k) {
    switch (k) {
        case KEY_A: return "A"; case KEY_B: return "B"; case KEY_C: return "C";
        case KEY_D: return "D"; case KEY_E: return "E"; case KEY_F: return "F";
        case KEY_G: return "G"; case KEY_H: return "H"; case KEY_I: return "I";
        case KEY_J: return "J"; case KEY_K: return "K"; case KEY_L: return "L";
        case KEY_M: return "M"; case KEY_N: return "N"; case KEY_O: return "O";
        case KEY_P: return "P"; case KEY_Q: return "Q"; case KEY_R: return "R";
        case KEY_S: return "S"; case KEY_T: return "T"; case KEY_U: return "U";
        case KEY_V: return "V"; case KEY_W: return "W"; case KEY_X: return "X";
        case KEY_Y: return "Y"; case KEY_Z: return "Z";
        case KEY_0: return "0"; case KEY_1: return "1"; case KEY_2: return "2";
        case KEY_3: return "3"; case KEY_4: return "4"; case KEY_5: return "5";
        case KEY_6: return "6"; case KEY_7: return "7"; case KEY_8: return "8";
        case KEY_9: return "9";
        case KEY_ENTER: return "Enter"; case KEY_TAB: return "Tab";
        case KEY_ESCAPE: return "Esc"; case KEY_SPACE: return "Space";
        case KEY_BACKSPACE: return "Backspace"; case KEY_DELETE: return "Delete";
        case KEY_UP: return "Up"; case KEY_DOWN: return "Down";
        case KEY_LEFT: return "Left"; case KEY_RIGHT: return "Right";
        case KEY_HOME: return "Home"; case KEY_END: return "End";
        case KEY_PAGE_UP: return "PageUp"; case KEY_PAGE_DOWN: return "PageDn";
        case KEY_F1: return "F1"; case KEY_F2: return "F2"; case KEY_F3: return "F3";
        case KEY_F4: return "F4"; case KEY_F5: return "F5"; case KEY_F6: return "F6";
        case KEY_F7: return "F7"; case KEY_F8: return "F8"; case KEY_F9: return "F9";
        case KEY_F10: return "F10"; case KEY_F11: return "F11"; case KEY_F12: return "F12";
        case KEY_COMMA: return ","; case KEY_PERIOD: return ".";
        case KEY_LEFT_BRACKET: return "["; case KEY_RIGHT_BRACKET: return "]";
        case KEY_MINUS: return "-"; case KEY_EQUAL: return "=";
        case KEY_SLASH: return "/";
        default: return "?";
    }
}

const char *keybind_format(const KeyBinding *b) {
    static char buf[64];
    usize pos = 0;
    buf[0] = '\0';

    /* Append s into buf, tracking remaining capacity; always NUL-terminate. */
#define KB_APPEND(s) do {                                                  \
        int n = snprintf(buf + pos, sizeof(buf) - pos, "%s", (s));         \
        if (n < 0) break;                                                  \
        if ((usize)n >= sizeof(buf) - pos) { pos = sizeof(buf) - 1; break; } \
        pos += (usize)n;                                                   \
    } while (0)

    if (b->mods & MOD_CTRL)  KB_APPEND("Ctrl+");
    if (b->mods & MOD_ALT)   KB_APPEND("Alt+");
    if (b->mods & MOD_SHIFT) KB_APPEND("Shift+");
    if (b->mods & MOD_SUPER) KB_APPEND(UI_MOD_PRIMARY "+");
    KB_APPEND(key_name(b->key));
#undef KB_APPEND
    return buf;
}

/* =========================================================================
 * Save / Load (JSON-like format)
 * ========================================================================= */

bool keybind_save(const KeyBindTable *kb, const char *path) {
    if (!kb || kb->count < 0 || kb->count > MAX_KEYBINDINGS) return false;
    /* Through a link to the file it names, as config_save() writes. */
    AtomicFileWriter writer;
    if (!atomic_file_begin_private_through(&writer, path)) return false;
    FILE *f = writer.stream;
    fprintf(f, "{\n  \"keybindings_version\": %d,\n", KEYBIND_FILE_VERSION);
    bool wrote_entry = false;
    KeyBindTable defaults;
    keybind_init_defaults(&defaults);
    for (i32 action = 1; action < ACT_COUNT; action++) {
        const KeyBinding *b = keybind_get(kb, (Action)action);
        if (b && b->key == KEY_UNKNOWN) continue;
        if (!b && !keybind_get(&defaults, (Action)action)) continue;
        if (wrote_entry) fprintf(f, ",\n");
        fprintf(f, "  \"%s\": \"%s\"", action_name((Action)action), b ? keybind_format(b) : "Unbound");
        wrote_entry = true;
    }
    if (wrote_entry) fprintf(f, "\n");
    fprintf(f, "}\n");
    return atomic_file_commit(&writer);
}

static Action action_from_name(const char *name) {
    for (i32 i = 1; i < ACT_COUNT; i++) {
        if (g_action_names[i] && strcmp(g_action_names[i], name) == 0)
            return (Action)i;
    }
    return ACT_NONE;
}

static KeyCode key_from_name(const char *name) {
    if (strlen(name) == 1) {
        char c = name[0];
        if (c >= 'A' && c <= 'Z') return KEY_A + (KeyCode)(c - 'A');
        if (c >= 'a' && c <= 'z') return KEY_A + (KeyCode)(c - 'a');
        if (c >= '0' && c <= '9') return KEY_0 + (KeyCode)(c - '0');
    }
    static const KeyCode named[] = {
        KEY_ENTER, KEY_TAB, KEY_ESCAPE, KEY_SPACE, KEY_BACKSPACE, KEY_DELETE,
        KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PAGE_UP, KEY_PAGE_DOWN,
        KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
        /* key_name writes these four too, so a shortcut on one must parse
         * back at the next launch. */
        KEY_COMMA, KEY_PERIOD, KEY_LEFT_BRACKET, KEY_RIGHT_BRACKET,
        KEY_MINUS, KEY_EQUAL, KEY_SLASH
    };
    for (usize i = 0; i < sizeof named / sizeof *named; i++)
        if (strcmp(name, key_name(named[i])) == 0) return named[i];
    return KEY_UNKNOWN;
}

bool keybind_load(KeyBindTable *kb, const char *path) {
    /* Laid out one member a line first (core/json_lines.h), so a minified
     * file is read rather than taken as empty and replaced with the
     * defaults by the quit's save. */
    usize raw_n = 0, text_n = 0;
    char *raw = json_read_file(path, (usize)1 << 20, &raw_n);
    if (!raw) return false;
    char *text = json_lay_out_members(raw, raw_n, &text_n);
    free(raw);
    FILE *f = text && text_n > 0 ? fmemopen(text, text_n, "r") : NULL;
    if (!f) { free(text); return text != NULL; }

    char line[256];
    i32 version = 0;
    while (fgets(line, sizeof(line), f)) {
        char vtext[16];
        if (sscanf(line, " \"keybindings_version\" : %15[0-9]", vtext) == 1) {
            version = atoi(vtext);
            continue;
        }
        char action_str[64], bind_str[64];
        if (sscanf(line, " \"%63[^\"]\" : \"%63[^\"]\"", action_str, bind_str) != 2)
            continue;

        Action act = action_from_name(action_str);
        if (act == ACT_NONE) continue;
        if (strcmp(bind_str, "Unbound") == 0) {
            keybind_remove(kb, act);
            continue;
        }

        /* Parse bind_str like "Cmd+Shift+T" */
        u32 mods = 0;
        char *token = bind_str;
        char *last_plus = strrchr(bind_str, '+');
        if (last_plus) {
            /* Parse modifiers before last + */
            char mod_part[64];
            i32 mod_len = (i32)(last_plus - bind_str);
            memcpy(mod_part, bind_str, (usize)mod_len);
            mod_part[mod_len] = '\0';

            if (strstr(mod_part, "Ctrl")) mods |= MOD_CTRL;
            if (strstr(mod_part, "Alt")) mods |= MOD_ALT;
            if (strstr(mod_part, "Shift")) mods |= MOD_SHIFT;
            if (strstr(mod_part, "Cmd") || strstr(mod_part, "Super")) mods |= MOD_SUPER;

            token = last_plus + 1;
        }

        KeyCode key = key_from_name(token);
        if (key != KEY_UNKNOWN) keybind_set(kb, act, key, mods);
    }

    fclose(f);
    free(text);
#ifndef PLATFORM_MACOS
    /* On non-macOS systems, reset legacy Super bindings that conflict with the
     * current defaults. Files at the current version retain explicit bindings;
     * only the first chord stored for each action is considered. */
    KeyBindTable def;
    keybind_init_defaults(&def);
    bool first_seen[ACT_COUNT] = { false };
    for (i32 i = 0; version < KEYBIND_FILE_VERSION && i < kb->count; i++) {
        Action a = kb->bindings[i].action;
        if (a <= ACT_NONE || a >= ACT_COUNT || first_seen[a]) continue;
        first_seen[a] = true;
        if (!(kb->bindings[i].mods & MOD_SUPER)) continue;
        const KeyBinding *d = keybind_get(&def, a);
        if (!d || (d->mods & MOD_SUPER)) continue;
        kb->bindings[i].key  = d->key;
        kb->bindings[i].mods = d->mods;
    }
#endif
    const KeyBinding *split_h = keybind_get(kb, ACT_SPLIT_HORIZONTAL);
    if (version < 2 && split_h && split_h->key == KEY_T && split_h->mods == MOD_SUPER) {
        keybind_set(kb, ACT_SPLIT_HORIZONTAL, KEY_D, MOD_SUPER);
        const KeyBinding *new_tab = keybind_get(kb, ACT_NEW_TAB);
        if (!new_tab || new_tab->key != KEY_T || new_tab->mods != MOD_SUPER) {
            keybind_set(kb, ACT_NEW_TAB, KEY_T, MOD_SUPER);
        }
    }
    return true;
}
