/*
 * Liu — command-palette row semantics. See palette_row.h.
 */
#include "ui/palette_row.h"

#include <stdio.h>
#include <string.h>

/* Verbs that start a command name. Order matters only for prefixes that share
 * a stem; none currently do. Each carries its trailing space so the split
 * lands after it. */
static const char *const FAMILY_VERBS[] = {
    "Toggle ", "Go to ", "Check for ", "Import ", "Create ", "Generate ",
    "Search ", "Show ", "Filter ", "Restart ", "Start ", "Stop ", "Close ",
    "Open ", "New ", "Add ", "Rename ", "Ungroup ", "Attach ", "Select ",
    "Clear ", "Wake ", "Sleep ",
};
static const usize FAMILY_VERB_COUNT =
    sizeof FAMILY_VERBS / sizeof FAMILY_VERBS[0];

i32 palette_family_split(const char *name) {
    if (!name || !name[0]) return 0;

    /* Explicit family separator wins: "Vault: Lock" -> "Vault: ". */
    const char *colon = strstr(name, ": ");
    if (colon) {
        i32 split = (i32)(colon - name) + 2;
        /* Nothing after the separator means there is no label left to
         * emphasise, so keep the row at full weight. */
        return name[split] ? split : 0;
    }

    for (usize i = 0; i < FAMILY_VERB_COUNT; i++) {
        usize vlen = strlen(FAMILY_VERBS[i]);
        if (strncmp(name, FAMILY_VERBS[i], vlen) != 0) continue;
        /* "Toggle " alone, or a name that is nothing but the verb, would dim
         * the entire row — leave those undimmed. */
        return name[vlen] ? (i32)vlen : 0;
    }
    return 0;
}

/* A command's icon names its CATEGORY, what the row is about, and every row
 * of a category wears the same mark: the one that subject wears elsewhere in
 * Liu where it has one (the sidebar's sections, the tab strip, the connect
 * form's tile, Settings' tabs). The verb in front says nothing a row's words
 * do not: "Toggle Translate" is Translate's, not a switch's. A Settings row is
 * Settings' gear whatever tab it opens. */
typedef struct { const char *name; IconKind icon; } IconRule;

/* Every command the palette lists (palette.c's g_commands_static) is named
 * here or by its category below, so no row falls through to the verbs or the
 * chevron. */
static const IconRule ICON_EXACT[] = {
    /* Tabs */
    { "New Tab",                ICON_PLUS },
    { "Close Tab",              ICON_CLOSE },
    { "Change Title",           ICON_PENCIL },
    { "Next Tab",               ICON_CHEVRON_RIGHT },
    { "Previous Tab",           ICON_CHEVRON_LEFT },
    /* Windows (and "Window: …" below) */
    { "New Window",             ICON_WINDOW },
    { "Toggle Quake Mode",      ICON_WINDOW },
    /* Splits (and "Split: …"): side by side, and the one stacked */
    { "Split Horizontal",       ICON_COLUMNS },
    { "Split Vertical",         ICON_ROWS },
    /* The sidebar, the explorer and what opens beside a terminal */
    { "Toggle Sidebar",         ICON_SIDEBAR },
    { "Customize Sidebar",      ICON_SLIDERS },
    { "Toggle File Browser",    ICON_FT_FOLDER },
    { "Folder History",         ICON_FT_FOLDER },
    { "Code Editor",            ICON_FT_CODE },
    { "Sketch",                 ICON_BRUSH },
    /* The window's chrome: each bar is drawn where it sits */
    { "Toggle Tab Bar",         ICON_PANEL_TOP },
    { "Toggle Status Bar",      ICON_PANEL_BOTTOM },
    { "Toggle Scrollbar",       ICON_PANEL_RIGHT },
    { "Toggle Toolbar Icons",   ICON_SHAPES },
    { "Toggle Git Changes",     ICON_FT_BRANCH },
    { "Toggle Render Benchmark", ICON_GAUGE },
    /* Text (and "Line Height: …") */
    { "Font Size",              ICON_TYPE },
    { "Toggle Ligatures",       ICON_TYPE },
    { "Toggle Bold is Bright",  ICON_TYPE },
    { "Opacity",                ICON_BLEND },
    /* The cursor (and "Cursor: …") */
    { "Toggle Cursor Blink",    ICON_TEXT_CURSOR },
    { "Toggle Cursor Animation", ICON_TEXT_CURSOR },
    /* The terminal */
    { "Find in Terminal",       ICON_SEARCH },
    { "Clear Terminal",         ICON_CLOSE },
    { "Toggle Copy on Select",  ICON_COPY },
    { "Cycle Option as Alt",    ICON_OPTION },
    { "Toggle Secure Keyboard Entry", ICON_KEYBOARD },  /* the Vault has the padlock */
    { "Toggle Visual Bell",     ICON_BELL },
    { "Toggle Audible Bell",    ICON_BELL },
    /* Themes (and "Theme: …") */
    { "Create Theme",           ICON_PALETTE },
    { "Import Theme",           ICON_PALETTE },
    /* The notes Vault: the sidebar's Notes section */
    { "Open Vault",             ICON_NOTEBOOK },
    { "Create Note",            ICON_NOTEBOOK },
    { "Open Note",              ICON_NOTEBOOK },
    { "Search Notes",           ICON_NOTEBOOK },
    { "Add Vault Source",       ICON_NOTEBOOK },
    { "Show Backlinks",         ICON_NOTEBOOK },
    { "Go to Heading",          ICON_HASH },
    /* The knowledge graph: the graph tab's own mark */
    { "Graph View",             ICON_GRAPH },
    { "Graph Current Folder",   ICON_GRAPH },
    { "Filter Graph by Folder", ICON_GRAPH },
    /* Agents: the sidebar's Agents section, the recall chip, the voice card */
    { "Agent History",          ICON_BOT },
    { "Toggle Prompt Recall",   ICON_PROMPT },
    { "Voice Input",            ICON_MIC },
    { "Toggle Translate",       ICON_LANGUAGES },
    /* The browser (and "Browser …"), and the element picked in it */
    { "Browser",                ICON_NETWORK },
    { "Select Element",         ICON_SCOPE },
    { "Attach Element",         ICON_SCOPE },
    /* Remote connections: the connect form's tiles */
    { "SSH Connect",            ICON_SERVER },
    { "Mosh Connect",           ICON_SERVER },
    { "Import SSH Config",      ICON_SERVER },
    { "SFTP Connect",           ICON_FOLDER_SYNC },
    { "Open SFTP Tab",          ICON_FOLDER_SYNC },
    { "Telnet Connect",         ICON_TERMINAL },
    { "Serial Connect",         ICON_TERMINAL },
    { "SSH Keys",               ICON_KEY },
    { "Generate SSH Key",       ICON_KEY },
    { "Known Hosts",            ICON_FINGERPRINT },
    { "Port Forwarding",        ICON_ARROW_LEFT_RIGHT },
    { "Clear History",          ICON_HISTORY },   /* the connections made before */
    /* Broadcast */
    { "Toggle Broadcast",       ICON_BROADCAST },
    { "Broadcast Targets",      ICON_BROADCAST },
    /* Sites, the dev-server launcher */
    { "Site Manager",           ICON_ROCKET },
    { "Start Site",             ICON_ROCKET },
    { "Restart Site",           ICON_ROCKET },
    { "Stop All Sites",         ICON_ROCKET },
    /* Tab groups */
    { "Create Tab Group",       ICON_GROUP },
    { "Rename Group",           ICON_GROUP },
    { "Close Group",            ICON_GROUP },
    { "Toggle Group Open/Close", ICON_GROUP },
    { "Ungroup Current Tab",    ICON_GROUP },
    /* Tab sleep (and "Tab Sleep: …"): the sleeping tab's moon */
    { "Toggle Tab Sleep (this tab)", ICON_MOON },
    { "Sleep This Tab Now",     ICON_MOON },
    { "Wake All Tabs",          ICON_MOON },
    /* The app */
    { "Settings",               ICON_GEAR },
    { "Check for Updates",      ICON_REFRESH },
    { "Reload",                 ICON_REFRESH },
};

/* Categories: a name that starts with one, at a word boundary ("Window: Snap
 * Left", "Browser Split Right"), wears its mark. Checked after exact names. */
static const IconRule ICON_CATEGORY[] = {
    { "Settings",       ICON_GEAR },
    { "Theme",          ICON_PALETTE },
    { "Set Profile",    ICON_USER },
    { "Vault",          ICON_FT_LOCK },   /* Settings' Vault tab */
    { "Window",         ICON_WINDOW },
    { "Split",          ICON_COLUMNS },
    { "Browser",        ICON_NETWORK },
    { "Translate",      ICON_LANGUAGES },
    { "Prompt Recall",  ICON_PROMPT },
    { "Tab Sleep",      ICON_MOON },
    { "Opacity",        ICON_BLEND },
    { "Line Height",    ICON_TYPE },
    { "Font",           ICON_TYPE },
    { "Cursor",         ICON_TEXT_CURSOR },
    { "Scrollback",     ICON_SCROLL },
    { "Notes",          ICON_NOTEBOOK },
    { "Note",           ICON_NOTEBOOK },
};

/* Verbs, last: only for a command added without a row above, so it still says
 * roughly what it does instead of wearing the chevron. */
static const IconRule ICON_VERB[] = {
    { "Search",     ICON_SEARCH },
    { "Go to",      ICON_ARROW_RIGHT },
    { "Open",       ICON_ARROW_UP_RIGHT },
    { "New",        ICON_PLUS },
    { "Add",        ICON_PLUS },
    { "Close",      ICON_CLOSE },
    /* A checkmark reads as "this is on" — wrong for a row that merely names a
     * command. A switch says "this flips something" without asserting state,
     * which the palette has no way to know. */
    { "Toggle",     ICON_TOGGLE },
    { "Restart",    ICON_REFRESH },
    { "Start",      ICON_REFRESH },
    { "Stop",       ICON_STOP },
    { "Import",     ICON_UP },
    { "Create",     ICON_PLUS },
    { "Generate",   ICON_PLUS },
    { "Filter",     ICON_FILTER },
    { "Show",       ICON_EYE },
    { "Rename",     ICON_PENCIL },
    { "Select",     ICON_CHECK },   /* confirmation is the right meaning here */
    { "Clear",      ICON_CLOSE },
    { "Wake",       ICON_BELL },
    { "Sleep",      ICON_MOON },
};

/* The rule of `rules` that `name` starts with at a word boundary, so "Newton"
 * doesn't inherit "New"'s icon. */
static IconKind icon_prefix_rule(const IconRule *rules, usize n, const char *name) {
    for (usize i = 0; i < n; i++) {
        usize flen = strlen(rules[i].name);
        if (strncmp(name, rules[i].name, flen) != 0) continue;
        char next = name[flen];
        if (next == ' ' || next == ':') return rules[i].icon;
    }
    return ICON_NONE;
}

static IconKind icon_for_type(i32 type) {
    switch ((PaletteRowType)type) {
    case PALETTE_ROW_HOST:         return ICON_SERVER;   /* the SFTP tab's host side */
    case PALETTE_ROW_RECENT:       return ICON_HISTORY;  /* = Clear History's rows */
    case PALETTE_ROW_SNIPPET:      return ICON_CLIPBOARD; /* text put into the pane */
    case PALETTE_ROW_HISTORY:      return ICON_BOT;      /* drawn with the agent's logo */
    case PALETTE_ROW_HEADING:      return ICON_HASH;
    case PALETTE_ROW_NOTE:
    case PALETTE_ROW_NOTE_MATCH:
    case PALETTE_ROW_NOTE_SOURCE:  return ICON_FT_DOC_TEXT;
    case PALETTE_ROW_SITE:         return ICON_ROCKET;
    case PALETTE_ROW_FOLDER:
    case PALETTE_ROW_GRAPH_FOLDER: return ICON_FT_FOLDER;
    case PALETTE_ROW_COMMAND:      break;
    }
    return ICON_CHEVRON_RIGHT;
}

PaletteIconRule palette_icon_rule(const char *name, i32 type, IconKind *out) {
    IconKind k = ICON_NONE;
    PaletteIconRule rule = PALETTE_ICON_BY_TYPE;
    /* Only a command is named by its words: a host, a snippet or a note is
     * named by whoever made it, and one called "Settings box" is still a
     * host. */
    if (type == PALETTE_ROW_COMMAND && name && name[0]) {
        for (usize i = 0; i < sizeof ICON_EXACT / sizeof ICON_EXACT[0]; i++) {
            if (strcmp(name, ICON_EXACT[i].name) == 0) {
                k = ICON_EXACT[i].icon;
                rule = PALETTE_ICON_BY_NAME;
                break;
            }
        }
        if (k == ICON_NONE) {
            k = icon_prefix_rule(ICON_CATEGORY,
                                 sizeof ICON_CATEGORY / sizeof ICON_CATEGORY[0], name);
            if (k != ICON_NONE) rule = PALETTE_ICON_BY_CATEGORY;
        }
        if (k == ICON_NONE) {
            k = icon_prefix_rule(ICON_VERB, sizeof ICON_VERB / sizeof ICON_VERB[0], name);
            if (k != ICON_NONE) rule = PALETTE_ICON_BY_VERB;
        }
    }
    if (k == ICON_NONE) k = icon_for_type(type);
    if (out) *out = k;
    return rule;
}

IconKind palette_icon_for(const char *name, i32 type) {
    IconKind k = ICON_NONE;
    palette_icon_rule(name, type, &k);
    return k;
}

bool palette_shortcut_for(const KeyBindTable *kb, const char *name,
                          char *out, usize cap) {
    if (out && cap) out[0] = '\0';
    if (!kb || !name || !name[0] || !out || cap == 0) return false;

    /* action_name() is the same human-readable spelling the palette lists, so
     * the label IS the lookup key — no second table to keep in step. */
    for (i32 a = ACT_NONE + 1; a < ACT_COUNT; a++) {
        const char *an = action_name((Action)a);
        if (!an || strcmp(an, name) != 0) continue;

        const KeyBinding *b = keybind_get(kb, (Action)a);
        if (!b || b->key == KEY_UNKNOWN) return false; /* native-menu placeholder has no usable table key */
        const char *text = keybind_format(b);
        if (!text || !text[0]) return false;
        snprintf(out, cap, "%s", text);    /* copy: keybind_format is static */
        return out[0] != '\0';
    }
    return false;
}
