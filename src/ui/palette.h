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
    PALETTE_MODE_FOLDER_HISTORY = 8, /* folders a command was run in — cd there */
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

/* The palette model is process-global, while the active flag, query, and
 * selection belong to each `AppState`. `owner` scopes reads and mutations so
 * another window cannot act on this palette. `cfg` supplies profile rows
 * during this call and is not retained because its `AppState` may be freed
 * by a window merge. */
void palette_open(Vault *v, const AppConfig *cfg, const void *owner);
void palette_close(void);

/* Set the current window before dispatching its events or rendering it. The
 * owner pointer is compared but never dereferenced; leaving it unset
 * preserves unscoped single-window behavior. */
void palette_set_scope(const void *owner);
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

/* With a `NULL` or empty `cwd_filter`, list all discovered sessions; otherwise
 * include only sessions matched to the supplied working directory.
 * `chat_session_matches_cwd()` uses the recorded working directory when present,
 * or the project slug for the relevant agent format (Claude Code replaces `/`
 * with `-`). Sessions without either value are excluded.
 */
void       palette_enter_history_mode(const char *cwd_filter);
/* Owner-scoped pickup; other windows cannot consume this owner's result. */
bool       palette_history_poll(void);
/* Process-wide main-thread pickup, once per event-loop turn including while
 * closed/hidden. Temporarily scopes adoption to the owner and restores the
 * caller's scope. Returns which owner needs repainting; never dereferences it. */
bool       palette_history_pump(const void **changed_owner);
/* Cancel/release a closing window's palette without affecting other owners. */
void       palette_release_owner(const void *owner);
/* A scan is outstanding for the open history mode (the rows on screen, if
 * any, are the previous list). */
bool       palette_history_scanning(void);
/* The folder the open history list is scoped to, or "" when it lists every
 * session there is. For the palette's own header line, not a path to act on. */
const char *palette_history_scope(void);
/* How many different agents the rows on show come from. */
i32        palette_history_agent_count(void);
/* Session files opened to name a row, since the process started. A reopen
 * with nothing changed on disk leaves it where it was. */
u64        palette_history_title_reads(void);
/* Cancel, join and release the worker and its cached list. palette_shutdown
 * calls it; it is idempotent. */
void       palette_history_shutdown(void);

/* Push the palette into the site-launcher list (Cmd+K -> "Start Site").
 * Items mirror the Site Manager's rows: name + live status/port/command.
 * Enter toggles the selected site (start when stopped, stop when running). */
struct SiteManager;
void       palette_enter_sites_mode(const struct SiteManager *m);
/* Same list, but Enter restarts the selected site (used by "Restart Site"). */
void       palette_enter_sites_restart_mode(const struct SiteManager *m);
bool       palette_sites_restart_mode(void);
/* Pop back to the root list. Same `cfg`/`owner` contract as palette_open —
 * it re-populates through it. */
void       palette_exit_history_mode(Vault *v, const AppConfig *cfg,
                                     const void *owner);
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
/* The folder the session ran in, when its agent records one; "" otherwise. */
const char *palette_history_session_cwd(i32 item_id);

/* Record `text` as just-used in the MRU cache. Floats this item to the top
 * the next time the palette is opened. Persists to disk on each call.
 * Called after every successful palette activation (Enter / click). */
void palette_mru_record(const char *text);

/* Release lazy side-tables (history sids/paths) and flush MRU to disk.
 * Call at app teardown. Safe to call when nothing was allocated. */
void palette_shutdown(void);

#endif
