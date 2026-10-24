/*
 * Liu — command-palette row semantics.
 *
 * The three questions a palette row asks about a command that are pure
 * functions of its name, split out of the renderer so they can be tested
 * without a window:
 *
 *   - where does the family prefix end ("Settings: " | "Appearance")
 *   - which icon belongs to it
 *   - what key is bound to it right now
 *
 * Everything here is display logic only; nothing draws.
 */
#ifndef UI_PALETTE_ROW_H
#define UI_PALETTE_ROW_H

#include "core/types.h"
#include "core/keybind.h"
#include "ui/icon.h"

/* Palette item kinds as stored in PaletteItem.type. Named here because the row
 * renderer branches on them and bare 0/2/4/8 read as nothing at the call site. */
typedef enum {
    PALETTE_ROW_HOST    = 0,
    PALETTE_ROW_SNIPPET = 1,
    PALETTE_ROW_COMMAND = 2,
    PALETTE_ROW_RECENT  = 3,    /* "[Recent] user@host": an SSH connection made before */
    PALETTE_ROW_HISTORY = 4,
    PALETTE_ROW_HEADING = 5,    /* Go to Heading: a heading of the open document */
    PALETTE_ROW_NOTE    = 6,    /* Open Note, Show Backlinks: a note to open */
    PALETTE_ROW_NOTE_MATCH = 7, /* Search Notes: a line in a note */
    PALETTE_ROW_SITE    = 8,
    PALETTE_ROW_NOTE_SOURCE = 9, /* Add Vault Source: a note to hand the agent */
    PALETTE_ROW_FOLDER  = 10,   /* a folder-history row; its label is a path */
    PALETTE_ROW_GRAPH_FOLDER = 11, /* Filter Graph by Folder: a folder of the graph */
} PaletteRowType;

/* Length in bytes of the dimmed family prefix of `name`, or 0 when the whole
 * label should be drawn at full weight.
 *
 * Two rules, in order:
 *   1. An explicit ": " separator — "Settings: Appearance" dims "Settings: ".
 *   2. A recognised verb prefix — "Toggle Sidebar" dims "Toggle ".
 *
 * A name that is *only* a verb ("Settings") never splits: dimming the entire
 * row would make it the faintest thing in the list. Only command rows should
 * be passed here — a hostname or a note title has no family, and greying its
 * first word just makes it harder to read. */
i32 palette_family_split(const char *name);

/* Icon for a row. A command's icon names its CATEGORY — what it is about, not
 * the verb in front — so every row of a category wears the same mark and the
 * mark is the one that subject wears elsewhere in Liu (the sidebar's sections,
 * the tab strip, the connect form). Asked in order: the exact name, its
 * category ("Window: …", "Theme: …"), a verb for a command no table names yet;
 * every other kind of row is its type's. Never ICON_NONE. */
IconKind palette_icon_for(const char *name, i32 type);

/* Which of those steps chose the icon (stored in *out). A palette whose every
 * command resolves by NAME or CATEGORY has no row left to a guess. */
typedef enum {
    PALETTE_ICON_BY_NAME,
    PALETTE_ICON_BY_CATEGORY,
    PALETTE_ICON_BY_VERB,
    PALETTE_ICON_BY_TYPE,
} PaletteIconRule;
PaletteIconRule palette_icon_rule(const char *name, i32 type, IconKind *out);

/* Current shortcut for the command labelled `name`, e.g. "Cmd+T".
 *
 * Returns false (and empties `out`) when the label isn't a bound action or the
 * user removed its binding — the caller then draws no badge: the badge
 * reflects the live table, not a decoration.
 *
 * COPIES into `out`. keybind_format() hands back a static buffer, so a caller
 * that held its pointer across rows would render every badge as the last
 * formatted shortcut — and clip the text too, since the box is measured from
 * the correct string. */
bool palette_shortcut_for(const KeyBindTable *kb, const char *name,
                          char *out, usize cap);

#endif /* UI_PALETTE_ROW_H */
