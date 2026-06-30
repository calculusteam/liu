#ifndef UI_PALETTE_H
#define UI_PALETTE_H

#include "core/types.h"
#include "core/config.h"
#include "vault/vault.h"

typedef struct {
    i32  score;
    char text[256];
    char detail[128];
    i32  id;        /* index into source array */
    u8   type;      /* 0=host, 1=snippet, 2=command, 3=recent-ssh, 4=agent-history */
    /* Extra field used only by agent-history items (type == 4). */
    u8   tool;      /* ChatTool: 1=claude, 2=codex, 3=copilot */
    /* `session_id` for history rows lives in a separate lazy-allocated
     * side table — see palette_history_session_id(). Kept out of this struct
     * so hosts/snippets/commands don't each carry an unused 96-byte buffer. */
} PaletteItem;

/* Palette modes — root (default, all commands/hosts/snippets), agent history,
 * or markdown document outline ("Go to Heading"). */
typedef enum {
    PALETTE_MODE_ROOT     = 0,
    PALETTE_MODE_HISTORY  = 1,
    PALETTE_MODE_OUTLINE  = 2,
    PALETTE_MODE_SWITCHER = 3,   /* markdown quick switcher (Open Note) */
    PALETTE_MODE_SEARCH   = 4,   /* markdown vault content search */
    PALETTE_MODE_BACKLINKS= 5,   /* notes that link to the current note */
    PALETTE_MODE_FOLDER   = 6,   /* scope the knowledge graph to a folder */
    PALETTE_MODE_SITES    = 7,   /* dev-site launcher (start/stop saved sites) */
} PaletteMode;

/* One heading handed to palette_enter_outline_mode. `text` need not be
 * nul-terminated; `text_len` bytes are copied. `level` is 1..6. */
typedef struct {
    const char *text;
    u32         text_len;
    u8          level;
} PaletteOutlineEntry;

/* A generic two-field list row for the switcher / vault-search pickers. Neither
 * string need be nul-terminated; the given lengths are copied. `id` is stored
 * on the resulting PaletteItem so the caller can map a (possibly filtered)
 * selection back to its source — e.g. a file index when rows are a subset. */
typedef struct {
    const char *name;       u32 name_len;     /* primary label */
    const char *detail;     u32 detail_len;   /* dim secondary text */
    i32         id;
} PaletteListEntry;

void palette_open(Vault *v);
void palette_close(void);
bool palette_is_open(void);
void palette_set_query(const char *query);
void palette_input_char(u32 codepoint);
void palette_backspace(void);
void palette_move_up(void);
void palette_move_down(void);
PaletteItem *palette_get_selected(void);
PaletteItem *palette_get_item(i32 index);
i32 palette_filtered_count(void);
i32 palette_selected_index(void);
const char *palette_query(void);

/* Mode control — lets the app push the palette into "Agent History" or pop back.
 *
 * `cwd_filter`: when NULL or empty, all discovered sessions are listed
 * (the historic behaviour). When non-empty, results are restricted to
 * sessions whose project slug matches that of `cwd_filter` — typical use
 * is "show only sessions for the project I'm currently inside, except
 * when the user is at $HOME in which case unfilter". The encoding is the
 * Claude Code convention: replace each '/' with '-'. Tools that do not
 * carry the project path in their session metadata (Codex, Cline, …) are
 * filtered out under this mode. */
void       palette_enter_history_mode(const char *cwd_filter);

/* Push the palette into the site-launcher list (Cmd+K -> "Start Site").
 * Items mirror the Site Manager's rows: name + live status/port/command.
 * Enter toggles the selected site (start when stopped, stop when running). */
struct SiteManager;
void       palette_enter_sites_mode(const struct SiteManager *m);
/* Same list, but Enter restarts the selected site (used by "Restart Site"). */
void       palette_enter_sites_restart_mode(const struct SiteManager *m);
bool       palette_sites_restart_mode(void);
void       palette_exit_history_mode(Vault *v);
PaletteMode palette_mode(void);

/* Push the palette into markdown-outline mode: each entry becomes a row
 * (indented by heading level), and PaletteItem.id is the entry index so the
 * caller can map a selection back to its heading. Items have type == 5. */
void       palette_enter_outline_mode(const PaletteOutlineEntry *entries, u32 count);

/* Push the palette into a generic picker `mode` (SWITCHER or SEARCH). Each
 * entry becomes a row; PaletteItem.id is the entry index and PaletteItem.type
 * is set to `item_type` so the caller can map a selection back. */
void       palette_enter_picker_mode(PaletteMode mode, u8 item_type,
                                     const PaletteListEntry *entries, u32 count);

/* Replace the picker's rows in place, preserving the current mode and the
 * typed query (used by vault search to re-grep as the user types). Resets the
 * filtered view to the new rows and clamps the selection. */
void       palette_set_picker_items(u8 item_type,
                                    const PaletteListEntry *entries, u32 count);

/* Look up the agent-history session-id for a palette item by its `id`
 * field (valid only when PaletteItem.type == 4). Returns "" if the side
 * table isn't allocated or the index is out of range. */
const char *palette_history_session_id(i32 item_id);
/* Absolute on-disk path of the session file — needed by resume commands that
 * require a full path (e.g. codex `-c experimental_resume=<path>`). */
const char *palette_history_session_path(i32 item_id);

/* Set config pointer for profile/theme enumeration */
void palette_set_config(const AppConfig *cfg);

/* Record `text` as just-used in the MRU cache. Floats this item to the top
 * the next time the palette is opened. Persists to disk on each call.
 * Called after every successful palette activation (Enter / click). */
void palette_mru_record(const char *text);

/* Release lazy side-tables (history sids/paths) and flush MRU to disk.
 * Call at app teardown. Safe to call when nothing was allocated. */
void palette_shutdown(void);

#endif
