/*
 * Liu - Command palette (Cmd+K)
 * Fuzzy search overlay for hosts, snippets, and commands.
 */
#include "core/types.h"
#include "core/memory.h"
#include "core/string_utils.h"
#include "core/theme_import.h"
#include "core/config.h"
#include "core/net.h"
#include "ui/palette.h"
#include "core/sites.h"   /* Site/SiteManager for the launcher mode */
#include "vault/vault.h"
#include "history/event.h"
#include "history/parser.h"
#include "history/session.h"
#include "history/session_title.h"
#include "agentctl/agentctl_delegated.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include "core/mem_budget.h"
#include "platform/platform.h"

/* =========================================================================
 * Fuzzy matcher (scoring-based matching)
 * ========================================================================= */

/*
 * Fuzzy match score. Higher = better match.
 * Returns -1 if no match.
 *
 * Scoring:
 *  - Consecutive matches: +4
 *  - Start of word: +3
 *  - Camel case boundary: +2
 *  - Any match: +1
 *  - Gap penalty: -1
 */
static i32 fuzzy_score(const char *pattern, const char *text) {
    i32 plen = (i32)strlen(pattern);
    i32 tlen = (i32)strlen(text);
    if (plen == 0) return 0;
    if (plen > tlen) return -1;

    i32 pi = 0;       /* pattern index */
    i32 score = 0;
    i32 consecutive = 0;
    bool prev_matched = false;
    bool prev_was_sep = true; /* treat start as separator */

    for (i32 ti = 0; ti < tlen && pi < plen; ti++) {
        char tc = (char)tolower((unsigned char)text[ti]);
        char pc = (char)tolower((unsigned char)pattern[pi]);

        bool is_sep = (text[ti] == ' ' || text[ti] == '-' || text[ti] == '_' ||
                       text[ti] == '.' || text[ti] == '/' || text[ti] == '@');

        if (tc == pc) {
            score += 1; /* base match */
            if (prev_matched) {
                consecutive++;
                score += consecutive * 2; /* consecutive bonus */
            } else {
                consecutive = 0;
            }
            if (prev_was_sep) score += 3; /* word start bonus */
            if (ti > 0 && islower((unsigned char)text[ti-1]) && isupper((unsigned char)text[ti]))
                score += 2; /* camelCase bonus */
            prev_matched = true;
            pi++;
        } else {
            if (prev_matched) score -= 1; /* gap penalty */
            prev_matched = false;
            consecutive = 0;
        }
        prev_was_sep = is_sep;
    }

    return (pi == plen) ? score : -1;
}

/* A row whose whole name was typed is the row that was meant. The fuzzy score
 * cannot say so: "Browser" scores the same against "Toggle File Browser", and
 * "Opacity" against "Opacity: Increase", and a tie would fall to table order,
 * so Enter after the name typed in full could run a different command. */
#define PALETTE_EXACT_BONUS 100000
static bool palette_text_is(const char *query, const char *text) {
    for (; *query && *text; query++, text++)
        if (tolower((unsigned char)*query) != tolower((unsigned char)*text)) return false;
    return !*query && !*text;
}

/* =========================================================================
 * MRU cache — most-recently-used ordering for palette items.
 *
 * Stores (text → unix-ms) pairs in a flat array, persisted to
 * `~/.config/Liu/palette_mru` as one line per entry: "<ts>|<text>". When
 * the user activates an item its timestamp is bumped; when the palette
 * opens these timestamps reorder items[] so the most-recently used row
 * floats to the top. Items the user has never picked keep their authored
 * relative order at the bottom.
 *
 * All text matching is by exact string compare against PaletteItem.text.
 * That works for static commands (stable strings) and dynamic rows whose
 * label is reconstructable across sessions (host labels, snippet labels,
 * "Theme: <name>", "Set Profile: <name>"). Agent-history rows have
 * per-session labels and won't accumulate useful MRU data; the sort just
 * leaves them where the scanner placed them.
 * ========================================================================= */

#define MAX_MRU_ENTRIES 256

typedef struct {
    char text[256];
    i64  ts_ms;
} MruEntry;

static MruEntry g_mru[MAX_MRU_ENTRIES];
static i32      g_mru_count  = 0;
static bool     g_mru_loaded = false;

static bool mru_path(char *out, usize cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    const char *home = net_home_dir();
    if (!home || !home[0]) return false;
    int n = snprintf(out, cap, "%s/.config/Liu/palette_mru", home);
    return n > 0 && (usize)n < cap;
}

static bool mru_ensure_dir(const char *path) {
    char dir[1024];
    int n = path ? snprintf(dir, sizeof dir, "%s", path) : -1;
    if (n <= 0 || (usize)n >= sizeof dir) return false;
    char *sl = strrchr(dir, '/');
    if (!sl) return false;
    *sl = '\0';
    if (mkdir(dir, 0700) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat st;
    return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

static void mru_load(void) {
    if (g_mru_loaded) return;
    g_mru_loaded = true;
    char path[1024];
    if (!mru_path(path, sizeof path)) return;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 0 || st.st_size > 256 * 1024) { close(fd); return; }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return; }
    char line[512];
    while (fgets(line, sizeof line, f) && g_mru_count < MAX_MRU_ENTRIES) {
        usize ll = strlen(line);
        if (ll > 0 && line[ll - 1] != '\n' && !feof(f)) {
            int c; while ((c = fgetc(f)) != '\n' && c != EOF) {}
            if (c == EOF) break;
            continue;
        }
        char *bar = strchr(line, '|');
        if (!bar) continue;
        *bar = '\0';
        errno = 0;
        char *end = NULL;
        long long raw = strtoll(line, &end, 10);
        if (errno == ERANGE || end == line || *end != '\0' || raw <= 0)
            continue;
        i64 ts = (i64)raw;
        char *txt = bar + 1;
        usize tl = strlen(txt);
        while (tl > 0 && (txt[tl-1] == '\n' || txt[tl-1] == '\r')) txt[--tl] = '\0';
        if (tl == 0 || tl >= sizeof g_mru[0].text || strchr(txt, '|')) continue;
        snprintf(g_mru[g_mru_count].text, sizeof g_mru[0].text, "%s", txt);
        g_mru[g_mru_count].ts_ms = ts;
        g_mru_count++;
    }
    fclose(f);
}

static void mru_save(void) {
    char path[1024];
    if (!mru_path(path, sizeof path) || !mru_ensure_dir(path)) return;
    char tmp[1060];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path);
    if (n <= 0 || (usize)n >= sizeof tmp) return;
    int fd = mkstemp(tmp);
    if (fd < 0) return;
    (void)fchmod(fd, 0600);
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return; }
    bool ok = true;
    for (i32 i = 0; i < g_mru_count; i++) {
        if (fprintf(f, "%lld|%s\n",
                    (long long)g_mru[i].ts_ms, g_mru[i].text) < 0) {
            ok = false; break;
        }
    }
    if (ok && fflush(f) != 0) ok = false;
    if (ok && fsync(fd) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp, path) != 0) unlink(tmp);
}

static i64 mru_get(const char *text) {
    if (!text || !*text) return 0;
    for (i32 i = 0; i < g_mru_count; i++) {
        if (strcmp(g_mru[i].text, text) == 0) return g_mru[i].ts_ms;
    }
    return 0;
}

static i64 mru_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (ts.tv_sec < 0 || (u64)ts.tv_sec > (u64)INT64_MAX / 1000u) return 0;
    return (i64)ts.tv_sec * 1000 + (i64)(ts.tv_nsec / 1000000);
}

void palette_mru_record(const char *text) {
    if (!text || !*text) return;
    usize text_len = strnlen(text, sizeof g_mru[0].text);
    if (text_len == 0 || text_len >= sizeof g_mru[0].text ||
        strpbrk(text, "|\r\n")) return;
    mru_load();
    i64 now = mru_now_ms();
    for (i32 i = 0; i < g_mru_count; i++) {
        if (strcmp(g_mru[i].text, text) == 0) {
            g_mru[i].ts_ms = now;
            mru_save();
            return;
        }
    }
    if (g_mru_count < MAX_MRU_ENTRIES) {
        snprintf(g_mru[g_mru_count].text, sizeof g_mru[0].text, "%s", text);
        g_mru[g_mru_count].ts_ms = now;
        g_mru_count++;
    } else {
        /* Evict the entry with the oldest timestamp — a true LRU pop. */
        i32 oldest = 0;
        for (i32 i = 1; i < g_mru_count; i++) {
            if (g_mru[i].ts_ms < g_mru[oldest].ts_ms) oldest = i;
        }
        snprintf(g_mru[oldest].text, sizeof g_mru[0].text, "%s", text);
        g_mru[oldest].ts_ms = now;
    }
    mru_save();
}

/* Stable insertion sort items[] by MRU timestamp descending. Items with
 * ts==0 (never used) keep their authored relative order at the tail.
 * Called once at the end of palette_open so subsequent filter passes
 * inherit the new order. Pre-computes timestamps into a parallel array
 * so the linear mru_get scan is not repeated inside the inner loop. */
static void palette_sort_by_mru(PaletteItem *items, i32 n) {
    if (n <= 1) return;
    i64 ts[256];          /* MAX_PALETTE_ITEMS — 2 KB on stack, fine */
    if (n > 256) n = 256;
    for (i32 i = 0; i < n; i++) ts[i] = mru_get(items[i].text);
    for (i32 i = 1; i < n; i++) {
        PaletteItem tmp_it = items[i];
        i64         tmp_ts = ts[i];
        i32 j = i - 1;
        while (j >= 0 && ts[j] < tmp_ts) {
            items[j + 1] = items[j];
            ts[j + 1]    = ts[j];
            j--;
        }
        items[j + 1] = tmp_it;
        ts[j + 1]    = tmp_ts;
    }
}

/* =========================================================================
 * Palette state
 * ========================================================================= */

#define MAX_PALETTE_ITEMS 256

typedef struct {
    bool         active;
    PaletteMode  mode;
    char         query[128];
    i32          query_len;
    char         cached_query[128];
    bool         filter_dirty;
    /* Lazily allocated on open, freed on close — the palette is usually closed,
     * so the two 256-entry arrays (~198 KB) are not resident while it is. NULL
     * while closed; accessors stay safe because the counts are zeroed on close.
     */
    PaletteItem *items;
    i32          item_count;
    PaletteItem *filtered;
    i32          filtered_count;
    i32          selected;
} PaletteState;

static PaletteState g_palette = {0};

/* ---- Owner scoping ------------------------------------------------------
 * The model above is ONE struct for the process, while `palette_active`, the
 * typed query and the selection are per AppState. With two windows a shared
 * model would go wrong: ui.c calls palette_set_query() from the RENDER path,
 * so each window would refilter it to its own text every frame, each
 * resetting `selected`; entering Agent History in one window would flip the
 * mode for the other; and with both in history mode the rows Enter resolves
 * against would belong to whichever window scanned last — resuming the WRONG
 * session.
 *
 * Until the model itself moves onto AppState, it is explicitly owned: the
 * window that populated it (palette_open / any palette_enter_*_mode) is the
 * only one that can read or mutate it. Everyone else sees an empty, inert
 * palette — a window showing no rows is recoverable, a window resuming
 * someone else's agent session is not.
 *
 * `scope` is who is asking, set by the loop before it services a window.
 * Nobody ever calling palette_set_scope() leaves `scoped` false, which is the
 * single-window behaviour. */
static const void *g_palette_scope  = NULL;
static const void *g_palette_owner  = NULL;
static bool        g_palette_scoped = false;

void palette_set_scope(const void *owner) {
    g_palette_scope  = owner;
    g_palette_scoped = true;
}

/* True when the current caller may touch the model. */
static bool palette_owns(void) {
    return !g_palette_scoped || g_palette_owner == g_palette_scope;
}

/* Take the model for `owner` (NULL = whoever the loop is currently serving).
 * Called by every entry point that repopulates it. */
static void palette_claim(const void *owner) {
    g_palette_owner = owner ? owner : g_palette_scope;
}

/* "Restart Site" enters the sites list with restart-on-Enter semantics.
 * Mode-scoped state, same pattern as the history filter; cleared on
 * palette_open and on every plain sites-mode entry. */
static bool g_sites_restart_mode = false;

/* Built-in commands (static portion) */
static const char *g_commands_static[] = {
    "New Tab",
    "Close Tab",
    "Split Horizontal",
    "Split Vertical",
    "Focus Pane Left",        /* the keyboard to the pane on that side */
    "Focus Pane Right",
    "Focus Pane Above",
    "Focus Pane Below",
    "Next Pane",              /* the panes in reading order, wrapping */
    "Previous Pane",
    "Toggle Pane Zoom",       /* the focused pane fills the tab, or the layout comes back */
    "Toggle Sidebar",
    "Customize Sidebar",     /* the sidebar's own settings popover */
    "Toggle File Browser",
    "Sketch",                /* drawing canvas; copies an image on close */
    "Code Editor",           /* full-tab editor: explorer rail + code editor */
    "Toggle Render Benchmark",   /* frame-time / FPS / CPU HUD, top-right */
    "Graph View",            /* knowledge graph of the notes Vault */
    "Graph Current Folder",  /* graph the active folder instead of the Vault */
    "Open Vault",            /* open the notes Vault folder in the file browser */
    "Toggle Broadcast",
    "Toggle Secure Keyboard Entry",   /* keep the keyboard from other programs while Liu is in front */
    "Broadcast Targets",
    "Port Forwarding",
    "SSH Connect",            /* opens the in-app SSH dialog (libssh2 path, SFTP-ready) */
    "SFTP Connect",           /* the same form, for a local | host file-transfer tab */
    "Open SFTP Tab",          /* the focused SSH pane's host as an SFTP tab (else the form) */
    "Import SSH Config",
    "Create Theme",        /* AI-driven theme generation via local CLI agent */
    "Create Note",         /* new Markdown note in the Vault, opened for editing */
    "Import Theme",
    "Generate SSH Key",
    "SSH Keys",
    "Known Hosts",
    "Clear History",
    "Settings",
    "Site Manager",           /* open the Sites / dev-server manager overlay */
    "Start Site",             /* push palette into the site-launcher list */
    "Restart Site",           /* same list, Enter restarts instead of toggling */
    "Stop All Sites",
    "Find in Terminal",
    "New Window",
    "Change Title",           /* rename the active tab inline */
    "Check for Updates",
    "Toggle Translate",
    "Translate: Cycle Target Language",
    "Translate: Cycle Backend",
    "Settings: Translate",
    "Voice Input",            /* dictate a prompt into the focused pane */
    "Settings: Voice",
    "Settings: Notifications",
    "Rename Group",
    "Close Group",
    "Toggle Group Open/Close",
    "Mosh Connect",
    "Telnet Connect",
    "Serial Connect",
    "Browser",                /* open in-app localhost browser tab */
    "Browser Split Right",    /* terminal | browser side-by-side */
    "Browser Split Left",
    "Browser Split Below",    /* terminal above, browser below */
    "Browser Split Above",
    "Select Element",         /* arm browser pick mode → paste to agent */
    "Attach Element",         /* re-send last picked element to agent */
    "Agent History",          /* push palette into history mode (Claude/Codex/Copilot) */
    "Delegate to Codex",      /* a job written here, run beside the focused pane (agentctl) */
    "Delegate to Claude Code",
    "Delegate to Gemini",
    "Delegate to OpenCode",
    "Agent Team",             /* every delegated agent of the window: state, files, spend */
    "Folder History",         /* folders a command was run in — cd the pane there */
    "Go to Heading",          /* push palette into markdown outline mode */
    "Open Note",              /* push palette into markdown quick switcher */
    "Search Notes",           /* push palette into markdown vault content search */
    "Add Vault Source",       /* inject an @path reference to a Vault note into the active agent */
    "Show Backlinks",         /* notes linking to the current note */
    "Filter Graph by Folder", /* scope the knowledge graph to a folder */
    "Toggle Quake Mode",
    "Create Tab Group",
    "Ungroup Current Tab",
    "Toggle Tab Sleep (this tab)",
    "Sleep This Tab Now",
    "Wake All Tabs",
    "Toggle Tab Bar",
    "Toggle Toolbar Icons",
    "Toggle Scrollbar",
    "Toggle Status Bar",
    "Toggle Git Changes",    /* repo · branch · +N −M badge, bottom-right */
    /* Font / appearance */
    "Font Size",
    "Line Height: Increase",
    "Line Height: Decrease",
    /* Terminal behavior toggles */
    "Cursor: Cycle Style",
    "Toggle Cursor Blink",
    "Toggle Cursor Animation",
    "Toggle Bold is Bright",
    "Toggle Copy on Select",
    "Cycle Option as Alt",
    /* The agent pane's "what did I ask?" card. Toggle is the on/off; the cycle
     * reaches Always, which Settings' 3-up control also offers. */
    "Toggle Prompt Recall",
    "Prompt Recall: Cycle Mode",
    "Toggle Visual Bell",
    "Toggle Audible Bell",
    "Toggle Ligatures",
    "Scrollback: Increase",
    "Scrollback: Decrease",
    /* Tab sleep presets */
    "Tab Sleep: Off",
    "Tab Sleep: 5 min",
    "Tab Sleep: 10 min",
    "Tab Sleep: 20 min",
    "Tab Sleep: 30 min",
    "Tab Sleep: 60 min",
    /* Opacity */
    "Opacity: Increase",
    "Opacity: Decrease",
    "Opacity",
    /* Jump to a settings tab */
    "Settings: Appearance",
    "Settings: Terminal",
    "Settings: Keys",
    "Settings: Vault",
    "Settings: About",
    /* Smart Vault */
    "Vault: Open Browser",
    "Vault: Unlock",
    "Vault: Lock Now",
    "Vault: Change Master Password",
    "Window: Snap Left",
    "Window: Snap Right",
    "Window: Snap Top",
    "Window: Snap Bottom",
    "Window: Maximize",
    "Window: Center",
    "Window: Snap Top-Left",
    "Window: Snap Top-Right",
    "Window: Snap Bottom-Left",
    "Window: Snap Bottom-Right",
    "Split: Divider Left",
    "Split: Divider Right",
    "Split: Reset (50/50)",
    NULL
};

/* Dynamic command buffer (themes + profiles) */
#define MAX_DYNAMIC_CMDS 128
static char g_dynamic_cmds[MAX_DYNAMIC_CMDS][128];
static const char *g_commands[MAX_DYNAMIC_CMDS + 64];

/* The config is passed in per open rather than parked in a file-scope pointer:
 * a pointer kept across opens would borrow from whichever AppState was focused
 * last, and a window merge frees that AppState. It is only ever read here,
 * during the open, so the caller hands it over. */
static void palette_rebuild_commands(const AppConfig *cfg) {
    i32 n = 0;

    /* Static commands */
    for (i32 i = 0; g_commands_static[i] && n < MAX_DYNAMIC_CMDS + 60; i++) {
        g_commands[n++] = g_commands_static[i];
    }

    /* Theme commands (built-in + user) */
    i32 theme_count = 0;
    const char **theme_names = theme_list_names(&theme_count);
    for (i32 i = 0; i < theme_count && n < MAX_DYNAMIC_CMDS + 60; i++) {
        snprintf(g_dynamic_cmds[i], sizeof(g_dynamic_cmds[i]), "Theme: %s", theme_names[i]);
        g_commands[n++] = g_dynamic_cmds[i];
    }

    /* Theme deletion is handled in Settings → Appearance. */
    i32 di = theme_count;

    /* Profile commands */
    if (cfg) {
        for (i32 i = 0; i < cfg->profile_count && n < MAX_DYNAMIC_CMDS + 60; i++) {
            i32 pdi = di + i;
            if (pdi >= MAX_DYNAMIC_CMDS) break;
            snprintf(g_dynamic_cmds[pdi], sizeof(g_dynamic_cmds[pdi]),
                     "Set Profile: %s", cfg->profiles[i].name);
            g_commands[n++] = g_dynamic_cmds[pdi];
        }
    }

    g_commands[n] = NULL;
}

/* Streaming snippet → palette item adapter. user points at an i32 next_id
 * counter, incremented once per row. */
static bool palette_snippet_cb(const VaultSnippet *snip, void *user) {
    if (g_palette.item_count >= MAX_PALETTE_ITEMS) return false;
    i32 *next_id = (i32 *)user;
    PaletteItem *it = &g_palette.items[g_palette.item_count++];
    snprintf(it->text, sizeof(it->text), "%s", snip->label);
    if (snip->description[0])
        snprintf(it->detail, sizeof(it->detail), "%s", snip->description);
    else if (snip->tags[0])
        snprintf(it->detail, sizeof(it->detail), "%s", snip->tags);
    else
        snprintf(it->detail, sizeof(it->detail), "%.120s", snip->command);
    it->id = (*next_id)++;
    it->type = 1;
    return true;
}

/* Ensure the items/filtered arrays exist (lazy alloc). Returns false on OOM. */
static bool palette_ensure_arrays(void) {
    if (!g_palette.items)
        g_palette.items = calloc(MAX_PALETTE_ITEMS, sizeof(PaletteItem));
    if (!g_palette.filtered)
        g_palette.filtered = calloc(MAX_PALETTE_ITEMS, sizeof(PaletteItem));
    return g_palette.items && g_palette.filtered;
}

void palette_open(Vault *v, const AppConfig *cfg, const void *owner) {
    /* Repopulating takes the model: `owner` is the window doing it, so every
     * later read/mutate can be checked against it. */
    palette_claim(owner);
    /* Free any prior allocation before the memset zeroes the pointers. */
    free(g_palette.items);
    free(g_palette.filtered);
    memset(&g_palette, 0, sizeof(g_palette));
    g_sites_restart_mode = false;   /* never carry restart intent across opens */
    if (!palette_ensure_arrays()) { g_palette.active = false; return; }
    g_palette.active = true;
    g_palette.mode = PALETTE_MODE_ROOT;
    g_palette.filter_dirty = true;

    /* Rebuild dynamic command list (themes + profiles) */
    palette_rebuild_commands(cfg);

    /* Load recent history entries first (type 3 = history) */
    if (v) {
        VaultHistoryEntry history[10];
        i32 nh = vault_get_history(v, history, 10);
        for (i32 i = 0; i < nh && g_palette.item_count < MAX_PALETTE_ITEMS; i++) {
            /* Deduplicate: skip if same host+port+user already added */
            bool dup = false;
            for (i32 j = 0; j < g_palette.item_count; j++) {
                if (g_palette.items[j].type == 3 &&
                    strcmp(g_palette.items[j].detail, "") != 0) {
                    /* Build a comparison key */
                    char key_existing[512], key_new[512];
                    snprintf(key_existing, sizeof(key_existing), "%s", g_palette.items[j].detail);
                    snprintf(key_new, sizeof(key_new), "%s@%s:%d",
                             history[i].username, history[i].hostname, history[i].port);
                    if (strcmp(key_existing, key_new) == 0) { dup = true; break; }
                }
            }
            if (dup) continue;

            PaletteItem *item = &g_palette.items[g_palette.item_count++];
            snprintf(item->text, sizeof(item->text), "[Recent] %s@%s",
                     history[i].username, history[i].hostname);
            snprintf(item->detail, sizeof(item->detail), "%s@%s:%d",
                     history[i].username, history[i].hostname, history[i].port);
            item->id = i;
            item->type = 3; /* history entry */
        }
    }

    /* Load hosts */
    if (v) {
        VaultHost hosts[128];
        i32 n = vault_host_list(v, hosts, 128);
        for (i32 i = 0; i < n && g_palette.item_count < MAX_PALETTE_ITEMS; i++) {
            PaletteItem *item = &g_palette.items[g_palette.item_count++];
            snprintf(item->text, sizeof(item->text), "%s", hosts[i].label);
            snprintf(item->detail, sizeof(item->detail), "%s@%s:%d",
                     hosts[i].username, hosts[i].hostname, hosts[i].port);
            item->id = i;
            item->type = 0;
        }
    }

    /* Load snippets — streaming, so no 600 KB VaultSnippet[128] array goes on
     * the stack just to copy each label/detail into a PaletteItem. */
    if (v) {
        i32 next_id = 0;
        vault_snippet_for_each(v, palette_snippet_cb, &next_id);
        (void)next_id;
    }

    /* Load commands */
    for (i32 i = 0; g_commands[i] && g_palette.item_count < MAX_PALETTE_ITEMS; i++) {
        PaletteItem *item = &g_palette.items[g_palette.item_count++];
        snprintf(item->text, sizeof(item->text), "%s", g_commands[i]);
        item->detail[0] = '\0';
        item->id = i;
        item->type = 2;
    }

    /* Reorder so the user's recent picks float to the top — first time
     * users see the original "Recent → Hosts → Snippets → Commands"
     * grouping; once they start activating things, those rows lead. */
    mru_load();
    palette_sort_by_mru(g_palette.items, g_palette.item_count);

    /* Initial filter: show all */
    g_palette.filtered_count = g_palette.item_count;
    memcpy(g_palette.filtered, g_palette.items,
           (size_t)g_palette.item_count * sizeof(PaletteItem));
}

/* Lazy side tables holding per-session strings for agent-history rows. Stay
 * NULL until the user first opens history mode, which keeps idle RAM lower —
 * the alternative is carrying 96+512 bytes in every PaletteItem regardless of
 * type (i.e., also for hosts/snippets/commands/recent SSH). Codex-style
 * resume commands need the full on-disk path, not just the bare session-id.
 * Declared up here (not next to history_scan_cb) because palette_close also
 * releases them. */
#define HISTORY_SID_CAP  96
#define HISTORY_PATH_CAP 512
static char (*g_history_sids)[HISTORY_SID_CAP]   = NULL;
static char (*g_history_paths)[HISTORY_PATH_CAP] = NULL;
/* The folder each session ran in, when its agent records one ("" otherwise):
 * a resume runs there (Claude finds a session by the folder it is asked in). */
static char (*g_history_cwds)[HISTORY_PATH_CAP]  = NULL;
/* The history scan worker (defined with the rest of it further down). */
static void hist_worker_cancel(void);
static bool g_hist_scanning;
/* The folder the open history list was asked for ("" = every session). The
 * main thread's own copy: the worker's is behind its lock. */
static char g_hist_scope[1024];

void palette_close(void) {
    /* A window that does not own the model must not free the arrays the owner
     * is still rendering from. Its own AppState.palette_active already took
     * its overlay off screen. */
    if (!palette_owns()) return;
    g_palette.active = false;
    /* Release the ~198 KB item arrays while closed; zero the counts so the
     * accessors (which guard on *_count) stay safe with NULL pointers. */
    free(g_palette.items);     g_palette.items = NULL;
    free(g_palette.filtered);  g_palette.filtered = NULL;
    g_palette.item_count = 0;
    g_palette.filtered_count = 0;
    g_palette.selected = 0;
    /* History-mode side tables (~156 KB) are repopulated from a fresh scan
     * on every palette_enter_history_mode, and the selection flow copies
     * sid/path out before the palette closes — safe to drop them here
     * instead of pinning them until palette_shutdown. The getters return ""
     * while these are NULL. */
    if (g_history_sids)  { free(g_history_sids);  g_history_sids  = NULL; }
    if (g_history_paths) { free(g_history_paths); g_history_paths = NULL; }
    if (g_history_cwds)  { free(g_history_cwds);  g_history_cwds  = NULL; }
    /* Stop work for the closed view; the process completion pump reaps it
     * without needing a render. The last adopted list stays for reopen. */
    hist_worker_cancel();
    g_hist_scanning = false;

    /* And the scan arena behind them. arena_reset only rewinds the offset, so
     * the 12 MiB mapping — and every page the scan actually touched — stayed
     * resident from the first Agent History open until process exit. The
     * lifetime is the same one the two tables above have: every borrowed
     * ChatSessionMeta points INTO this arena, and the only readers copy what
     * they need out before returning (the picker into the tables above; the
     * transcript loader and the handoff writer each inside a single call that
     * takes its own borrow). A later borrow simply re-mmaps, so the whole
     * cost of releasing here is one mmap on the next open. */
    chat_scan_shutdown();
}

bool palette_is_open(void) {
    return palette_owns() && g_palette.active;
}

void palette_release_owner(const void *owner) {
    if (!owner || g_palette_owner != owner) return;
    const void *saved_scope = g_palette_scope;
    bool saved_scoped = g_palette_scoped;
    palette_set_scope(owner);
    palette_close();
    g_palette_owner = NULL;
    g_palette_scope = saved_scope == owner ? NULL : saved_scope;
    g_palette_scoped = saved_scoped;
}

void palette_set_query(const char *query) {
    /* THE hot one: ui.c calls this from the render path, so a non-owning
     * window re-ran the whole fuzzy filter over someone else's items every
     * frame and reset their selection with it. */
    if (!palette_owns()) return;
    if (!query) query = "";
    /* Vault search owns its own result set (grepped by the app as the user
     * types); the fuzzy filter must not second-guess or reorder it. */
    if (g_palette.mode == PALETTE_MODE_SEARCH) {
        snprintf(g_palette.query, sizeof(g_palette.query), "%s", query);
        g_palette.query_len = (i32)strlen(query);
        snprintf(g_palette.cached_query, sizeof(g_palette.cached_query), "%s", query);
        return;
    }
    if (!g_palette.filter_dirty && strcmp(query, g_palette.cached_query) == 0) {
        return;
    }

    snprintf(g_palette.query, sizeof(g_palette.query), "%s", query);
    g_palette.query_len = (i32)strlen(query);
    snprintf(g_palette.cached_query, sizeof(g_palette.cached_query), "%s", query);
    g_palette.filter_dirty = false;

    if (g_palette.query_len == 0) {
        g_palette.filtered_count = g_palette.item_count;
        memcpy(g_palette.filtered, g_palette.items,
               (size_t)g_palette.item_count * sizeof(PaletteItem));
        g_palette.selected = 0;
        return;
    }

    /* Score and filter */
    g_palette.filtered_count = 0;
    for (i32 i = 0; i < g_palette.item_count; i++) {
        i32 s1 = fuzzy_score(query, g_palette.items[i].text);
        i32 s2 = fuzzy_score(query, g_palette.items[i].detail);
        i32 best = s1 > s2 ? s1 : s2;
        if (s1 >= 0 && palette_text_is(query, g_palette.items[i].text)) best += PALETTE_EXACT_BONUS;

        if (best >= 0) {
            PaletteItem *f = &g_palette.filtered[g_palette.filtered_count++];
            *f = g_palette.items[i];
            f->score = best;
        }
    }

    /* Sort by score descending (simple insertion sort) */
    for (i32 i = 1; i < g_palette.filtered_count; i++) {
        PaletteItem tmp = g_palette.filtered[i];
        i32 j = i - 1;
        while (j >= 0 && g_palette.filtered[j].score < tmp.score) {
            g_palette.filtered[j + 1] = g_palette.filtered[j];
            j--;
        }
        g_palette.filtered[j + 1] = tmp;
    }

    g_palette.selected = 0;
}

void palette_input_char(u32 codepoint) {
    if (!palette_owns()) return;
    if (g_palette.query_len < 127) {
        g_palette.query[g_palette.query_len++] = (char)codepoint;
        g_palette.query[g_palette.query_len] = '\0';
        palette_set_query(g_palette.query);
    }
}

void palette_backspace(void) {
    if (!palette_owns()) return;
    if (g_palette.query_len > 0) {
        g_palette.query[--g_palette.query_len] = '\0';
        palette_set_query(g_palette.query);
    }
}

void palette_move_up(void) {
    if (!palette_owns()) return;
    if (g_palette.selected > 0) g_palette.selected--;
}

void palette_move_down(void) {
    if (!palette_owns()) return;
    if (g_palette.selected < g_palette.filtered_count - 1) g_palette.selected++;
}

/* The one that decides what Enter acts on. A non-owner reading this would
 * resume a session from the OTHER window's history scan. */
PaletteItem *palette_get_selected(void) {
    if (!palette_owns()) return NULL;
    if (g_palette.selected < 0 || g_palette.selected >= g_palette.filtered_count)
        return NULL;
    return &g_palette.filtered[g_palette.selected];
}

i32 palette_filtered_count(void) {
    return palette_owns() ? g_palette.filtered_count : 0;
}

PaletteItem *palette_get_item(i32 index) {
    if (!palette_owns()) return NULL;
    if (index < 0 || index >= g_palette.filtered_count) return NULL;
    return &g_palette.filtered[index];
}

i32 palette_selected_index(void) {
    return palette_owns() ? g_palette.selected : 0;
}

const char *palette_query(void) {
    return palette_owns() ? g_palette.query : "";
}

/* =========================================================================
 * Agent history mode — populates the palette with sessions discovered via
 * `chat_scan()` (history/scanner.c) instead of the root command/host list.
 * ========================================================================= */

static void fmt_session_time(i64 ms, char *buf, usize cap) {
    if (ms <= 0) { snprintf(buf, cap, "—"); return; }
    time_t now = time(NULL);
    time_t t = (time_t)(ms / 1000);
    time_t delta = now > t ? now - t : 0;
    /* Relative time for recent entries, absolute date otherwise. */
    if (delta < 60)             snprintf(buf, cap, "just now");
    else if (delta < 3600)      snprintf(buf, cap, "%lldm ago", (long long)(delta / 60));
    else if (delta < 24*3600)   snprintf(buf, cap, "%lldh ago", (long long)(delta / 3600));
    else if (delta < 7*24*3600) snprintf(buf, cap, "%lldd ago", (long long)(delta / (24*3600)));
    else {
        struct tm tm; localtime_r(&t, &tm);
        strftime(buf, cap, "%b %d, %Y", &tm);
    }
}

static void fmt_session_bytes(i64 bytes, char *buf, usize cap) {
    if (bytes < 1024)           snprintf(buf, cap, "%lld B",  (long long)bytes);
    else if (bytes < 1024*1024) snprintf(buf, cap, "%.1f KB", (f64)bytes / 1024.0);
    else                        snprintf(buf, cap, "%.1f MB", (f64)bytes / (1024.0 * 1024.0));
}

/* Display label for an agent in the history list. chat_tool_name() returns
 * the machine-friendly lowercase id ("claude", "roo", ...), which is not
 * meant for the UI; this returns the human-friendly form for row subtitles.
 * Falls through to chat_tool_name() for tools without a display name here.
 */
static const char *tool_display(ChatTool t) {
    switch (t) {
        case CHAT_TOOL_CLAUDE:      return "Claude Code";
        case CHAT_TOOL_CODEX:       return "Codex";
        case CHAT_TOOL_COPILOT:     return "Copilot";
        case CHAT_TOOL_CURSOR:      return "Cursor Agent";
        case CHAT_TOOL_AMP:         return "Amp";
        case CHAT_TOOL_CLINE:       return "Cline";
        case CHAT_TOOL_ROO:         return "Roo Code";
        case CHAT_TOOL_KILO:        return "Kilo Code";
        case CHAT_TOOL_KIRO:        return "Kiro";
        case CHAT_TOOL_CRUSH:       return "Crush";
        case CHAT_TOOL_OPENCODE:    return "OpenCode";
        case CHAT_TOOL_DROID:       return "Droid";
        case CHAT_TOOL_ANTIGRAVITY: return "Antigravity";
        case CHAT_TOOL_KIMI:        return "Kimi";
        case CHAT_TOOL_QWEN:        return "Qwen";
        case CHAT_TOOL_COMMANDCODE: return "Command Code";
        case CHAT_TOOL_XAI:         return "Grok";
        case CHAT_TOOL_CLINE_CLI:   return "Cline CLI";
        case CHAT_TOOL_PI:          return "Pi";
        case CHAT_TOOL_OMP:         return "Oh My Pi";
        case CHAT_TOOL_OPENCLAW:    return "OpenClaw";
        case CHAT_TOOL_HERMES:      return "Hermes";
        case CHAT_TOOL_DEVIN:       return "Devin";
        case CHAT_TOOL_QODER:       return "Qoder";
        case CHAT_TOOL_LETTA:       return "Letta Code";
        case CHAT_TOOL_MASTRACODE:  return "Mastra Code";
        case CHAT_TOOL_MAKI:        return "Maki";
        case CHAT_TOOL_MUSE:        return "Muse Code";
        case CHAT_TOOL_PRIME:       return "Prime Agent";
        default:                    return chat_tool_name(t);
    }
}

/* If `path` points inside an editor-fork globalStorage tree (which is where
 * Cline/Roo/Kilo store their sessions), return the human-readable name of
 * that IDE. Returns NULL for CLI-agent paths so the caller can drop the
 * " • IDE" suffix entirely instead of printing an empty string. The check is
 * a substring match on the globalStorage segment because that segment is
 * stable across macOS / Linux layouts (the prefix differs but the
 * "<Product>/User/globalStorage" middle is fixed). */
static const char *ide_label_from_path(const char *path) {
    if (!path) return NULL;
    if (strstr(path, "/Code - Insiders/User/globalStorage/")) return "VS Code Insiders";
    if (strstr(path, "/Code/User/globalStorage/"))            return "Visual Studio Code";
    if (strstr(path, "/Cursor/User/globalStorage/"))          return "Cursor";
    if (strstr(path, "/VSCodium/User/globalStorage/"))        return "VSCodium";
    return NULL;
}

/* Claude encodes the project path as "-path-to-dir" (slashes → dashes).
 * Show just the last segment so the row stays readable.
 *
 * Some scanners hand over a literal absolute path instead (Kimi recovers the
 * real work dir from kimi.json rather than going through a lossy slug), so a
 * '/' takes precedence — otherwise a whole path with no dashes in it would be
 * printed verbatim and swamp the row. */
static void fmt_project_label(const char *slug, char *out, usize cap) {
    if (!slug || !*slug || strcmp(slug, "-") == 0) {
        snprintf(out, cap, "(no project)");
        return;
    }
    const char *sep = strrchr(slug, '/');
    if (!sep) sep = strrchr(slug, '-');
    const char *base = (sep && sep[1] != '\0') ? sep + 1 : slug;
    if (!*base) base = slug;
    snprintf(out, cap, "%s", base);
}

/* ===========================================================================
 * Agent History: the list is built by a worker and adopted as a snapshot.
 *
 * A scan walks every directory of every agent's store and sorts every entry,
 * which must not hold the palette's first row on the main thread. The
 * worker owns its own scan arena (chat_scan_arena_create, never the borrowed
 * main-thread one) and its own result arrays, formats the rows itself, and
 * publishes an immutable snapshot the main thread adopts with a copy.
 *
 * Three rules, each the answer to a race:
 *   - every request takes a generation; a snapshot for an older one (the
 *     palette was closed and reopened, or moved to another cwd) is dropped
 *     and, if the mode is still waiting, the newest request is started;
 *   - a close CANCELS the run and never joins it — the process pump reaps
 *     it whether or not its former owner paints another frame;
 *   - the LAST adopted snapshot is kept (≤ MAX_PALETTE_ITEMS rows, ~255 KB)
 *     and served the instant the mode reopens for the same filter, so the
 *     previous list shows at once and the fresh one replaces it.
 * =========================================================================== */

typedef struct {
    PaletteItem *items;                      /* MAX_PALETTE_ITEMS slots */
    char       (*sids)[HISTORY_SID_CAP];
    char       (*paths)[HISTORY_PATH_CAP];
    char       (*cwds)[HISTORY_PATH_CAP];
    i32          count;
    u32          gen;
    bool         complete;                   /* false when the scan was cancelled */
    char         cwd_filter[1024];
} HistorySnapshot;

typedef struct {
    pthread_t       tid;
    pthread_mutex_t mtx;
    bool            live;
    bool            ready;
    _Atomic bool    published;
    _Atomic bool    cancel;
    u32             gen;                     /* the newest request */
    char            want_cwd[1024];          /* its filter */
    HistorySnapshot res;                     /* published, for pickup */
    /* The job the running thread took: gen + filter, copied at spawn. */
    u32             job_gen;
    char            job_cwd[1024];
} HistoryWorker;

static HistoryWorker   g_hw = { .mtx = PTHREAD_MUTEX_INITIALIZER };
static HistorySnapshot g_hist_last;          /* last adopted list, served on reopen */
/* g_hist_scanning (declared above palette_close): a request is outstanding. */

#define HIST_SNAP_BYTES ((usize)MAX_PALETTE_ITEMS * (sizeof(PaletteItem) + HISTORY_SID_CAP + 2 * HISTORY_PATH_CAP))
#define HIST_TITLE_ARENA_BYTES MB(4)          /* what the transcript loader gives a line */

static void hist_snapshot_free(HistorySnapshot *hs) {
    if (!hs) return;
    if (hs->items || hs->sids || hs->paths)
        mem_budget_transient_adjust(MEM_HISTORY, -(isize)HIST_SNAP_BYTES);
    free(hs->items); free(hs->sids); free(hs->paths); free(hs->cwds);
    memset(hs, 0, sizeof *hs);
}

static bool hist_snapshot_alloc(HistorySnapshot *hs) {
    memset(hs, 0, sizeof *hs);
    hs->items = calloc(MAX_PALETTE_ITEMS, sizeof(PaletteItem));
    hs->sids  = calloc((usize)MAX_PALETTE_ITEMS, HISTORY_SID_CAP);
    hs->paths = calloc((usize)MAX_PALETTE_ITEMS, HISTORY_PATH_CAP);
    hs->cwds  = calloc((usize)MAX_PALETTE_ITEMS, HISTORY_PATH_CAP);
    if (!hs->items || !hs->sids || !hs->paths || !hs->cwds) {
        free(hs->items); free(hs->sids); free(hs->paths); free(hs->cwds);
        memset(hs, 0, sizeof *hs);
        return false;
    }
    /* Retained-shaped but on the TRANSIENT counter: MEM_HISTORY's level is
     * SET by the scanner for its borrowed arena, and an adjust there would be
     * overwritten by the next set. */
    mem_budget_transient_adjust(MEM_HISTORY, (isize)HIST_SNAP_BYTES);
    return true;
}

/* ---- what a row is called: the session's first prompt --------------------
 *
 * Named by its project folder alone, every row of one project would read
 * exactly alike, and a word from the prompt would find nothing. The worker
 * reads each listed session's first real prompt (chat_session_title: the
 * start of the file, a few kilobytes) and that is the row's first line; the
 * project moves to the second line and both stay searchable.
 *
 * At most MAX_PALETTE_ITEMS sessions are listed, newest first, so this is a
 * few hundred small reads the first time and none after: a title is
 * remembered under (path, size, mtime) — a session that grew is read again,
 * one that did not is not. The reads have a wall-clock budget per scan; rows
 * past it keep the project name this once and are named on the next open.
 *
 * The memo is probed, not direct-mapped: with one slot per key, two sessions
 * sharing a slot would put each other out on every scan and be read again on
 * every open. A key may sit anywhere in a window of HIST_TITLE_MEMO_PROBE
 * slots; when the window is full the entry no scan has asked for the longest
 * goes, so the rows on show never put each other out. */
#define HIST_TITLE_MEMO_SLOTS 1024
#define HIST_TITLE_MEMO_PROBE 16
#define HIST_TITLE_BUDGET_SEC 0.40
typedef struct {
    u64  key;                                 /* FNV-1a of the path */
    i64  size, mtime;
    u32  seen;                                /* the scan that last asked for it */
    bool used;
    char title[CHAT_SESSION_TITLE_CAP];       /* "" = looked, nothing to call it */
} HistTitleMemo;
static HistTitleMemo  *g_title_memo;
static u32             g_title_memo_scan;     /* under the mutex */
static pthread_mutex_t g_title_memo_mtx = PTHREAD_MUTEX_INITIALIZER;
static _Atomic u64     g_title_reads;         /* files opened for a title, ever */
#define HIST_TITLE_MEMO_BYTES ((usize)HIST_TITLE_MEMO_SLOTS * sizeof(HistTitleMemo))

u64 palette_history_title_reads(void) {
    return atomic_load_explicit(&g_title_reads, memory_order_relaxed);
}

static u64 hist_title_key(const char *path) {
    u64 h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) { h ^= *p; h *= 1099511628211ull; }
    return h ? h : 1;
}

/* A scan begins: what it asks for is younger than everything asked before. */
static void hist_title_memo_begin_scan(void) {
    pthread_mutex_lock(&g_title_memo_mtx);
    g_title_memo_scan++;
    pthread_mutex_unlock(&g_title_memo_mtx);
}

/* 1 = remembered (out filled, possibly ""), 0 = not known. */
static bool hist_title_memo_get(const ChatSessionMeta *m, char *out, usize cap) {
    bool hit = false;
    u64 key = hist_title_key(m->path);
    pthread_mutex_lock(&g_title_memo_mtx);
    for (u32 i = 0; g_title_memo && i < HIST_TITLE_MEMO_PROBE; i++) {
        HistTitleMemo *e = &g_title_memo[(key + i) % HIST_TITLE_MEMO_SLOTS];
        if (!e->used || e->key != key) continue;
        if (e->size == m->size_bytes && e->mtime == m->last_modified_ms) {
            snprintf(out, cap, "%s", e->title);
            e->seen = g_title_memo_scan;
            hit = true;
        }
        break;                                /* the key lives in one slot */
    }
    pthread_mutex_unlock(&g_title_memo_mtx);
    return hit;
}

static void hist_title_memo_put(const ChatSessionMeta *m, const char *title) {
    u64 key = hist_title_key(m->path);
    pthread_mutex_lock(&g_title_memo_mtx);
    if (!g_title_memo) {
        g_title_memo = calloc(HIST_TITLE_MEMO_SLOTS, sizeof(HistTitleMemo));
        if (g_title_memo) mem_budget_transient_adjust(MEM_HISTORY, (isize)HIST_TITLE_MEMO_BYTES);
    }
    if (g_title_memo) {
        /* Its own slot, else a free one, else the one asked for longest ago. */
        HistTitleMemo *own = NULL, *spare = NULL, *oldest = NULL;
        for (u32 i = 0; i < HIST_TITLE_MEMO_PROBE && !own; i++) {
            HistTitleMemo *e = &g_title_memo[(key + i) % HIST_TITLE_MEMO_SLOTS];
            if (e->used && e->key == key) own = e;
            else if (!e->used) { if (!spare) spare = e; }
            else if (!oldest || (u32)(g_title_memo_scan - e->seen) > (u32)(g_title_memo_scan - oldest->seen)) oldest = e;
        }
        HistTitleMemo *e = own ? own : spare ? spare : oldest;
        e->key = key; e->size = m->size_bytes; e->mtime = m->last_modified_ms; e->used = true;
        e->seen = g_title_memo_scan;
        snprintf(e->title, sizeof e->title, "%s", title ? title : "");
    }
    pthread_mutex_unlock(&g_title_memo_mtx);
}

static void hist_title_memo_free(void) {
    pthread_mutex_lock(&g_title_memo_mtx);
    if (g_title_memo) {
        free(g_title_memo);
        g_title_memo = NULL;
        mem_budget_transient_adjust(MEM_HISTORY, -(isize)HIST_TITLE_MEMO_BYTES);
    }
    pthread_mutex_unlock(&g_title_memo_mtx);
}

/* What the scan callback is handed: the snapshot it fills, and what the
 * worker lends it for reading titles. */
typedef struct {
    HistorySnapshot *hs;
    Arena           *title_arena;             /* NULL = no titles this scan */
    f64              title_spent;             /* seconds of reading so far */
    i32              title_read, title_known; /* files read / answers remembered */
    i32              title_named;             /* rows that got a title */
    const char      *delegated;               /* sessions delegated agents ran (agentctl_delegated.h) */
} HistScanCtx;

static f64 hist_now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
}

static bool hist_row_title(HistScanCtx *ctx, const ChatSessionMeta *m, char *out, usize cap) {
    out[0] = '\0';
    if (hist_title_memo_get(m, out, cap)) {
        ctx->title_known++;
        if (out[0]) ctx->title_named++;
        return out[0] != '\0';
    }
    if (!ctx->title_arena || ctx->title_spent >= HIST_TITLE_BUDGET_SEC ||
        atomic_load_explicit(&g_hw.cancel, memory_order_acquire)) return false;
    f64 t0 = hist_now_sec();
    bool got = chat_session_title(ctx->title_arena, m->tool, m->path, m->size_bytes, out, cap);
    ctx->title_spent += hist_now_sec() - t0;
    ctx->title_read++;
    atomic_fetch_add_explicit(&g_title_reads, 1, memory_order_relaxed);
    if (got) ctx->title_named++;
    hist_title_memo_put(m, got ? out : "");
    return got;
}

/* snprintf may have cut the tail in the middle of a code point. */
static void hist_utf8_trim_tail(char *s) {
    usize n = strlen(s);
    usize k = n;
    while (k > 0 && ((unsigned char)s[k - 1] & 0xc0) == 0x80) k--;      /* continuation bytes */
    if (k == 0) return;
    unsigned char lead = (unsigned char)s[k - 1];
    usize need = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xe ? 3 : (lead >> 3) == 0x1e ? 4 : 1;
    if (need > n - (k - 1)) s[k - 1] = '\0';
}

/* The row formatter, on the worker: everything it calls is pure over its
 * arguments (fmt_* write into the caller's buffers, tool_display and
 * ide_label_from_path return constants, localtime_r is reentrant). */
static bool history_scan_cb(const ChatSessionMeta *m, void *user) {
    HistScanCtx *ctx = (HistScanCtx *)user;
    HistorySnapshot *hs = ctx->hs;
    if (hs->count >= MAX_PALETTE_ITEMS) return false;

    /* cwd-scoped mode: only emit sessions belonging to the filter directory.
     * chat_session_matches_cwd() owns the per-tool rules — a recorded cwd
     * (Claude's decoded slug, Codex's session_meta header) when there is one,
     * a slug-direction comparison for CommandCode, and false for tools whose
     * working directory isn't cheaply recoverable. Unmatchable sessions are
     * skipped: the user has narrowed to a project and those rows are noise.
     * Use $HOME (= no filter) to see everything. */
    if (hs->cwd_filter[0] && !chat_session_matches_cwd(m, hs->cwd_filter)) {
        return true;   /* skip but keep scanning */
    }

    i32 idx = hs->count;
    PaletteItem *it = &hs->items[idx];
    memset(it, 0, sizeof *it);
    it->type = 4;                 /* agent-history */
    it->tool = (u8)m->tool;
    it->id   = idx;
    snprintf(hs->sids[idx],  HISTORY_SID_CAP,  "%s", m->session_id);
    snprintf(hs->paths[idx], HISTORY_PATH_CAP, "%s", m->path);
    snprintf(hs->cwds[idx],  HISTORY_PATH_CAP, "%s", m->cwd ? m->cwd : "");

    /* Primary label (text) — just the project basename. Bright, big. (The
     * session's first prompt takes this line below when there is one, and the
     * project then leads the second.) */
    fmt_project_label(m->project, it->text, sizeof it->text);
    /* No project slug — Codex keeps none — but a recorded folder: that folder
     * is the project. */
    if ((!m->project || !*m->project || strcmp(m->project, "-") == 0) && m->cwd && m->cwd[0])
        fmt_project_label(m->cwd, it->text, sizeof it->text);

    /* Secondary label (detail) — subtitle rendered dim on row 2.
     * Format: "<tool>  ·  <short-id>  ·  <time>  ·  <size>"
     * All parts stay fuzzy-searchable. */
    const char *sid = m->session_id;
    usize sidlen = strlen(sid);
    char short_sid[28];
    if (sidlen > 16) snprintf(short_sid, sizeof short_sid, "%.10s…%.4s", sid, sid + sidlen - 4);
    else             snprintf(short_sid, sizeof short_sid, "%s", sid);

    char ts[24]; fmt_session_time(m->last_modified_ms, ts, sizeof ts);
    /* OpenCode is the one tool whose size is not measured. Its sessions live
     * in a SQLite DB, and summing LENGTH(message.data) to get a byte total
     * forces SQLite to materialize the largest message value whole, a large
     * peak of memory for one row of a list. scan_opencode drops that subquery
     * and leaves size_bytes at 0, so show the message count here instead;
     * event_count carries the real COUNT(*) and costs nothing to compute. */
    char sz[16];
    if (chat_tool_is_db_store(m->tool)) snprintf(sz, sizeof sz, "%u msgs", m->event_count);
    else                               fmt_session_bytes(m->size_bytes, sz, sizeof sz);
    /* For Cline/Roo/Kilo (and any future extension-based agent) the IDE
     * the session lives in is part of the identity — "Roo Code in Cursor"
     * is a different transcript from "Roo Code in other editors". For CLI agents
     * the IDE field is omitted entirely. */
    const char *agent = tool_display(m->tool);
    const char *ide   = ide_label_from_path(m->path);
    if (ide) {
        snprintf(it->detail, sizeof it->detail,
                 "%s • %s  ·  %s  ·  %s  ·  %s",
                 agent, ide, short_sid, ts, sz);
    } else {
        snprintf(it->detail, sizeof it->detail, "%s  ·  %s  ·  %s  ·  %s",
                 agent, short_sid, ts, sz);
    }

    /* A session a delegated agent ran for another: said so, with what the
     * job was and who asked */
    char deleg[200];
    if (ctx->delegated && agentctl_delegated_lookup(ctx->delegated, m->session_id, deleg, sizeof deleg, NULL, 0)) {
        char rest[sizeof it->detail];
        snprintf(rest, sizeof rest, "%s", it->detail);
        snprintf(it->detail, sizeof it->detail, "delegated%s%s  \xc2\xb7  %s", deleg[0] ? ": " : "", deleg, rest);
        hist_utf8_trim_tail(it->detail);
    }

    /* Named by its first prompt when there is one: the prompt is the first
     * line and the project leads the second. Without one the project is
     * above and the rest below. */
    char title[CHAT_SESSION_TITLE_CAP];
    if (hist_row_title(ctx, m, title, sizeof title)) {
        char project[44], rest[sizeof it->detail];
        snprintf(project, sizeof project, "%s", it->text);
        hist_utf8_trim_tail(project);
        snprintf(rest, sizeof rest, "%s", it->detail);
        snprintf(it->detail, sizeof it->detail, "%s  ·  %s", project, rest);
        hist_utf8_trim_tail(it->detail);
        snprintf(it->text, sizeof it->text, "%s", title);
    }
    hs->count++;
    return true;
}

static bool hist_probe_on(void) {
    static i32 v = -1;
    if (v < 0) { const char *e = liu_dev_env("LIU_HISTORY_PROBE"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v == 1;
}

static void *hist_worker_main(void *arg) {
    (void)arg;
    HistorySnapshot hs;
    u32 gen;
    f64 title_sec = 0.0;
    i32 title_read = 0, title_known = 0, title_named = 0;
    f64 t0 = (f64)clock() / CLOCKS_PER_SEC;
    struct timespec w0; clock_gettime(CLOCK_MONOTONIC, &w0);
    pthread_mutex_lock(&g_hw.mtx);
    gen = g_hw.job_gen;
    bool ok = hist_snapshot_alloc(&hs);
    if (ok) snprintf(hs.cwd_filter, sizeof hs.cwd_filter, "%s", g_hw.job_cwd);
    pthread_mutex_unlock(&g_hw.mtx);
    hs.gen = gen;

    if (ok) {
        /* The worker's OWN arena — the borrowed one is the main thread's, and
         * a find or a transcript load may be using it right now. Charged
         * transient for as long as it is mapped. */
        Arena a = chat_scan_arena_create();
        if (a.base) {
            mem_budget_transient_adjust(MEM_HISTORY, (isize)CHAT_SCAN_ARENA_BYTES);
            /* Scratch for reading titles: one line at a time, reset per line. */
            Arena ta = arena_create(HIST_TITLE_ARENA_BYTES);
            if (ta.base) mem_budget_transient_adjust(MEM_HISTORY, (isize)HIST_TITLE_ARENA_BYTES);
            HistScanCtx ctx = { &hs, ta.base ? &ta : NULL, 0.0, 0, 0, 0, NULL };
            /* the config folder was worked out at launch, before any scan */
            char *delegated = agentctl_delegated_read(config_user_dir());
            ctx.delegated = delegated;
            hist_title_memo_begin_scan();
            hs.complete = chat_scan_cancellable(&a, 0, history_scan_cb, &ctx, &g_hw.cancel);
            free(delegated);
            title_sec = ctx.title_spent;
            title_read = ctx.title_read; title_known = ctx.title_known; title_named = ctx.title_named;
            if (ta.base) {
                arena_destroy(&ta);
                mem_budget_transient_adjust(MEM_HISTORY, -(isize)HIST_TITLE_ARENA_BYTES);
            }
            arena_destroy(&a);
            mem_budget_transient_adjust(MEM_HISTORY, -(isize)CHAT_SCAN_ARENA_BYTES);
        }
    }

    if (hist_probe_on()) {
        struct timespec w1; clock_gettime(CLOCK_MONOTONIC, &w1);
        fprintf(stderr, "[history] worker gen=%u rows=%d complete=%d wall=%.1f ms cpu=%.1f ms "
                        "titles=%.1f ms (read %d, remembered %d, named %d)\n",
                gen, hs.count, hs.complete,
                ((f64)w1.tv_sec - (f64)w0.tv_sec) * 1e3 + ((f64)w1.tv_nsec - (f64)w0.tv_nsec) / 1e6,
                ((f64)clock() / CLOCKS_PER_SEC - t0) * 1e3, title_sec * 1e3,
                title_read, title_known, title_named);
    }
    pthread_mutex_lock(&g_hw.mtx);
    hist_snapshot_free(&g_hw.res);            /* a result nobody collected */
    g_hw.res   = hs;
    g_hw.ready = true;
    atomic_store_explicit(&g_hw.published, true, memory_order_release);
    pthread_mutex_unlock(&g_hw.mtx);
    platform_wake_event_loop();
    return NULL;
}

/* Start the newest request if no run is live. Returns true when one is
 * running for it (started now or already) — the pickup restarts for a
 * newer generation when a stale one lands. */
static bool hist_worker_kick(void) {
    pthread_mutex_lock(&g_hw.mtx);
    if (g_hw.live) { pthread_mutex_unlock(&g_hw.mtx); return true; }
    g_hw.job_gen = g_hw.gen;
    snprintf(g_hw.job_cwd, sizeof g_hw.job_cwd, "%s", g_hw.want_cwd);
    atomic_store_explicit(&g_hw.cancel, false, memory_order_release);
    bool ok = pthread_create(&g_hw.tid, NULL, hist_worker_main, NULL) == 0;
    g_hw.live = ok;
    pthread_mutex_unlock(&g_hw.mtx);
    return ok;
}

/* Take the published snapshot, joining the thread that made it (which is one
 * instruction from returning). Never waits on a run in flight. */
static bool hist_worker_take(HistorySnapshot *out) {
    if (!atomic_load_explicit(&g_hw.published, memory_order_acquire)) return false;
    pthread_mutex_lock(&g_hw.mtx);
    bool ready = g_hw.ready, live = g_hw.live;
    if (ready) {
        *out = g_hw.res;
        memset(&g_hw.res, 0, sizeof g_hw.res);
        g_hw.ready = false;
        atomic_store_explicit(&g_hw.published, false, memory_order_release);
    }
    pthread_mutex_unlock(&g_hw.mtx);
    if (!ready) return false;
    if (live) {
        pthread_join(g_hw.tid, NULL);
        pthread_mutex_lock(&g_hw.mtx);
        g_hw.live = false;
        pthread_mutex_unlock(&g_hw.mtx);
    }
    return true;
}

/* Copy a snapshot into the palette's own arrays — the only thing that touches
 * g_palette from the history machinery, and it runs on the main thread. */
static bool hist_adopt(const HistorySnapshot *hs) {
    if (!palette_ensure_arrays()) return false;
    if (!g_history_sids)  g_history_sids  = calloc((usize)MAX_PALETTE_ITEMS, HISTORY_SID_CAP);
    if (!g_history_paths) g_history_paths = calloc((usize)MAX_PALETTE_ITEMS, HISTORY_PATH_CAP);
    if (!g_history_cwds)  g_history_cwds  = calloc((usize)MAX_PALETTE_ITEMS, HISTORY_PATH_CAP);
    if (!g_history_sids || !g_history_paths || !g_history_cwds) return false;
    i32 n = hs->count;
    if (n > MAX_PALETTE_ITEMS) n = MAX_PALETTE_ITEMS;
    memcpy(g_palette.items, hs->items, (usize)n * sizeof(PaletteItem));
    memcpy(g_history_sids,  hs->sids,  (usize)n * HISTORY_SID_CAP);
    memcpy(g_history_paths, hs->paths, (usize)n * HISTORY_PATH_CAP);
    memcpy(g_history_cwds,  hs->cwds,  (usize)n * HISTORY_PATH_CAP);
    g_palette.item_count = n;
    /* Rebuild the filtered view now, for whatever the query is: the render
     * path would do it on its next palette_set_query, but a caller that reads
     * the count right after a poll (a test, a keyboard handler) must see the
     * rows too. The selection is kept where it was when the query is empty
     * — a refresh must not yank the highlight back to the top. */
    i32 keep_sel = g_palette.selected;
    g_palette.filter_dirty = true;
    char q[sizeof g_palette.query];
    snprintf(q, sizeof q, "%s", g_palette.query);
    palette_set_query(q);
    if (g_palette.query_len == 0 && keep_sel < g_palette.filtered_count) g_palette.selected = keep_sel;
    return true;
}

bool palette_history_scanning(void) {
    return palette_owns() && g_palette.mode == PALETTE_MODE_HISTORY && g_hist_scanning;
}

const char *palette_history_scope(void) {
    if (!palette_owns() || g_palette.mode != PALETTE_MODE_HISTORY) return "";
    return g_hist_scope;
}

i32 palette_history_agent_count(void) {
    if (!palette_owns() || g_palette.mode != PALETTE_MODE_HISTORY || !g_palette.filtered) return 0;
    /* One bit per ChatTool; a walk of the rows on show. It is asked once a
     * frame while the list is open, and reads one byte of each row. */
    u64 seen = 0;
    for (i32 i = 0; i < g_palette.filtered_count; i++) {
        const u8 tool = g_palette.filtered[i].tool;
        if (tool < 64) seen |= (u64)1 << tool;
    }
    i32 n = 0;
    for (; seen; seen &= seen - 1) n++;
    return n;
}

bool palette_history_poll(void) {
    /* A non-owner must not take and discard another window's ready result. */
    if (!palette_owns()) return false;
    HistorySnapshot hs;
    if (!hist_worker_take(&hs)) {
        /* Nothing landed. `scanning` holds only while a run is live: a kick
         * that failed, or a result taken by a path that did not settle the
         * flag, must not leave the panel saying "scanning" forever. */
        if (g_hist_scanning) {
            pthread_mutex_lock(&g_hw.mtx);
            bool live = g_hw.live;
            pthread_mutex_unlock(&g_hw.mtx);
            if (!live && !atomic_load_explicit(&g_hw.published, memory_order_acquire))
                g_hist_scanning = false;
        }
        return false;
    }
    pthread_mutex_lock(&g_hw.mtx);
    u32 newest = g_hw.gen;
    pthread_mutex_unlock(&g_hw.mtx);
    bool waiting = palette_owns() && g_palette.active &&
                   g_palette.mode == PALETTE_MODE_HISTORY && g_hist_scanning;
    if (hist_probe_on())
        fprintf(stderr, "[history] poll gen=%u newest=%u complete=%d rows=%d waiting=%d\n",
                hs.gen, newest, hs.complete, hs.count, waiting);
    if (hs.gen != newest) {
        /* Stale: a newer request is pending (the kick was refused while this
         * one ran). Start it if the mode still wants it. */
        hist_snapshot_free(&hs);
        if (waiting) hist_worker_kick(); else g_hist_scanning = false;
        return false;
    }
    /* The newest generation landed, complete or cancelled: nothing is
     * outstanding either way. */
    g_hist_scanning = false;
    if (!waiting || !hs.complete) { hist_snapshot_free(&hs); return waiting; }
    bool adopted = hist_adopt(&hs);
    hist_snapshot_free(&g_hist_last);
    g_hist_last = hs;                               /* kept for the next open */
    return adopted;
}

bool palette_history_pump(const void **changed_owner) {
    if (changed_owner) *changed_owner = NULL;
    const void *saved_scope = g_palette_scope;
    bool saved_scoped = g_palette_scoped;
    palette_set_scope(g_palette_owner);
    bool changed = palette_history_poll();
    if (changed && changed_owner) *changed_owner = g_palette_owner;
    g_palette_scope = saved_scope;
    g_palette_scoped = saved_scoped;
    return changed;
}

static void hist_worker_cancel(void) {
    pthread_mutex_lock(&g_hw.mtx);
    bool live = g_hw.live;
    pthread_mutex_unlock(&g_hw.mtx);
    if (live) atomic_store_explicit(&g_hw.cancel, true, memory_order_release);
}

void palette_history_shutdown(void) {
    HistorySnapshot hs;
    hist_worker_cancel();
    pthread_mutex_lock(&g_hw.mtx);
    bool live = g_hw.live;
    pthread_mutex_unlock(&g_hw.mtx);
    if (live) {
        pthread_join(g_hw.tid, NULL);
        pthread_mutex_lock(&g_hw.mtx);
        g_hw.live = false;
        pthread_mutex_unlock(&g_hw.mtx);
    }
    if (hist_worker_take(&hs)) hist_snapshot_free(&hs);
    pthread_mutex_lock(&g_hw.mtx);
    hist_snapshot_free(&g_hw.res);
    g_hw.ready = false;
    atomic_store_explicit(&g_hw.published, false, memory_order_release);
    pthread_mutex_unlock(&g_hw.mtx);
    hist_snapshot_free(&g_hist_last);
    hist_title_memo_free();                   /* the worker is joined: nobody reads it */
    g_hist_scanning = false;
}

void palette_enter_history_mode(const char *cwd_filter) {
    palette_claim(NULL);   /* rescanning makes this window the owner */
    if (!palette_ensure_arrays()) return;
    /* Reset items but keep query so typed text can fuzzy-search sessions. */
    g_palette.mode = PALETTE_MODE_HISTORY;
    g_palette.item_count = 0;
    g_palette.filtered_count = 0;
    g_palette.selected = 0;
    g_palette.query[0]       = '\0';
    g_palette.query_len      = 0;
    g_palette.cached_query[0]= '\0';
    g_palette.filter_dirty   = true;

    /* The absolute cwd filter the worker compares each session's recorded cwd
     * against. Trailing slashes are trimmed so "/foo/" and "/foo" are treated
     * identically. */
    char want[1024] = {0};
    if (cwd_filter && *cwd_filter) {
        usize plen = strlen(cwd_filter);
        while (plen > 1 && cwd_filter[plen - 1] == '/') plen--;
        if (plen > 0 && plen + 1 < sizeof want) {
            memcpy(want, cwd_filter, plen);
            want[plen] = '\0';
        }
    }

    snprintf(g_hist_scope, sizeof g_hist_scope, "%s", want);

    /* Serve the last list for this filter at once — the fresh one replaces
     * it when the worker lands. A stale row is better than an empty panel
     * for the time a scan takes, and the rows it names still exist. */
    if (g_hist_last.items && g_hist_last.count > 0 &&
        strcmp(g_hist_last.cwd_filter, want) == 0)
        hist_adopt(&g_hist_last);

    /* A new generation, whether or not a run is live: an in-flight scan for
     * another filter (or an abandoned open) lands as a stale generation and
     * the pickup starts this one. */
    pthread_mutex_lock(&g_hw.mtx);
    g_hw.gen++;
    snprintf(g_hw.want_cwd, sizeof g_hw.want_cwd, "%s", want);
    bool live = g_hw.live;
    pthread_mutex_unlock(&g_hw.mtx);
    if (live) atomic_store_explicit(&g_hw.cancel, true, memory_order_release);
    g_hist_scanning = hist_worker_kick();

    /* Initial view: show all. */
    g_palette.filtered_count = g_palette.item_count;
    memcpy(g_palette.filtered, g_palette.items,
           (usize)g_palette.item_count * sizeof(PaletteItem));
}


bool palette_sites_restart_mode(void) {
    return palette_owns() && g_sites_restart_mode;
}

void palette_enter_sites_restart_mode(const struct SiteManager *m) {
    g_sites_restart_mode = true;
    palette_enter_sites_mode(m);
    g_sites_restart_mode = true;   /* enter_sites_mode clears it; re-arm */
}

void palette_enter_sites_mode(const struct SiteManager *m) {
    palette_claim(NULL);
    g_sites_restart_mode = false;
    if (!palette_ensure_arrays()) return;
    g_palette.mode           = PALETTE_MODE_SITES;
    g_palette.item_count     = 0;
    g_palette.filtered_count = 0;
    g_palette.selected       = 0;
    g_palette.query[0]       = '\0';
    g_palette.query_len      = 0;
    g_palette.cached_query[0]= '\0';
    g_palette.filter_dirty   = true;

    for (i32 i = 0; m && i < m->count && g_palette.item_count < MAX_PALETTE_ITEMS; i++) {
        const Site *st = m->sites[i];
        if (!st) continue;
        PaletteItem *it = &g_palette.items[g_palette.item_count];
        memset(it, 0, sizeof *it);
        it->type = 8;             /* site row (launcher) */
        it->id   = i;
        snprintf(it->text, sizeof it->text, "%s", st->name);

        const char *dot, *status;
        switch (st->status) {
            case SITE_RUNNING:  dot = "\xe2\x97\x8f"; status = "Running";  break; /* ● */
            case SITE_STARTING: dot = "\xe2\x97\x90"; status = "Starting"; break; /* ◐ */
            case SITE_FAILED:   dot = "\xe2\x9c\x95"; status = "Failed";   break; /* ✕ */
            default:            dot = "\xe2\x97\x8b"; status = "Stopped";  break; /* ○ */
        }
        i32 port = site_effective_port(st);
        /* Command capped short: detail renders right-aligned on a single
         * row, so an unbounded npm script line would crowd out the name. */
        if (port > 0) {
            snprintf(it->detail, sizeof it->detail, "%s %s  \xc2\xb7  :%d  \xc2\xb7  %.40s",
                     dot, status, port, st->command);
        } else {
            snprintf(it->detail, sizeof it->detail, "%s %s  \xc2\xb7  %.40s",
                     dot, status, st->command);
        }
        g_palette.item_count++;
    }

    g_palette.filtered_count = g_palette.item_count;
    memcpy(g_palette.filtered, g_palette.items,
           (usize)g_palette.item_count * sizeof(PaletteItem));
}

void palette_exit_history_mode(Vault *v, const AppConfig *cfg,
                               const void *owner) {
    g_palette.mode = PALETTE_MODE_ROOT;
    palette_open(v, cfg, owner); /* re-populate root items + reset query */
}

void palette_enter_outline_mode(const PaletteOutlineEntry *entries, u32 count) {
    palette_claim(NULL);
    if (!palette_ensure_arrays()) return;
    g_palette.mode           = PALETTE_MODE_OUTLINE;
    g_palette.active         = true;
    g_palette.item_count     = 0;
    g_palette.filtered_count = 0;
    g_palette.selected       = 0;
    g_palette.query[0]       = '\0';
    g_palette.query_len      = 0;
    g_palette.cached_query[0]= '\0';
    g_palette.filter_dirty   = true;

    for (u32 i = 0; entries && i < count && g_palette.item_count < MAX_PALETTE_ITEMS; i++) {
        const PaletteOutlineEntry *e = &entries[i];
        PaletteItem *it = &g_palette.items[g_palette.item_count];
        memset(it, 0, sizeof *it);
        it->type = 5;            /* markdown outline heading */
        it->id   = (i32)i;       /* index back into the caller's outline table */

        u8 lvl = e->level ? e->level : 1;
        u32 indent = (u32)(lvl - 1) * 2;
        if (indent > 12) indent = 12;
        usize w = 0;
        for (u32 s = 0; s < indent && w + 1 < sizeof it->text; s++) it->text[w++] = ' ';
        usize n = e->text_len;
        if (n > sizeof it->text - 1 - w) n = sizeof it->text - 1 - w;
        if (e->text && n) memcpy(it->text + w, e->text, n);
        it->text[w + n] = '\0';
        snprintf(it->detail, sizeof it->detail, "H%u", lvl);
        g_palette.item_count++;
    }

    /* Initial view: document order. */
    g_palette.filtered_count = g_palette.item_count;
    memcpy(g_palette.filtered, g_palette.items,
           (usize)g_palette.item_count * sizeof(PaletteItem));
}

void palette_enter_picker_mode(PaletteMode mode, u8 item_type,
                               const PaletteListEntry *entries, u32 count) {
    palette_claim(NULL);
    if (!palette_ensure_arrays()) return;
    g_palette.mode           = mode;
    g_palette.active         = true;
    g_palette.item_count     = 0;
    g_palette.filtered_count = 0;
    g_palette.selected       = 0;
    g_palette.query[0]       = '\0';
    g_palette.query_len      = 0;
    g_palette.cached_query[0]= '\0';
    g_palette.filter_dirty   = true;

    palette_set_picker_items(item_type, entries, count);
}

void palette_set_picker_items(u8 item_type,
                              const PaletteListEntry *entries, u32 count) {
    /* Re-grep results for a picker this window is driving — never for one it
     * does not own (that would swap the other window's rows mid-read). */
    if (!palette_owns()) return;
    if (!palette_ensure_arrays()) return;
    g_palette.item_count = 0;
    for (u32 i = 0; entries && i < count && g_palette.item_count < MAX_PALETTE_ITEMS; i++) {
        const PaletteListEntry *e = &entries[i];
        PaletteItem *it = &g_palette.items[g_palette.item_count];
        memset(it, 0, sizeof *it);
        it->type = item_type;
        it->id   = e->id;
        usize n = e->name_len;
        if (n > sizeof it->text - 1) n = sizeof it->text - 1;
        if (e->name && n) memcpy(it->text, e->name, n);
        it->text[n] = '\0';
        usize dn = e->detail_len;
        if (dn > sizeof it->detail - 1) dn = sizeof it->detail - 1;
        if (e->detail && dn) memcpy(it->detail, e->detail, dn);
        it->detail[dn] = '\0';
        g_palette.item_count++;
    }
    g_palette.filtered_count = g_palette.item_count;
    memcpy(g_palette.filtered, g_palette.items,
           (usize)g_palette.item_count * sizeof(PaletteItem));
    if (g_palette.selected >= g_palette.filtered_count)
        g_palette.selected = g_palette.filtered_count > 0 ? g_palette.filtered_count - 1 : 0;
}

PaletteMode palette_mode(void) {
    return palette_owns() ? g_palette.mode : PALETTE_MODE_ROOT;
}

const char *palette_history_session_id(i32 item_id) {
    if (!palette_owns() || !g_history_sids) return "";
    if (item_id < 0 || item_id >= MAX_PALETTE_ITEMS) return "";
    return g_history_sids[item_id];
}

const char *palette_history_session_path(i32 item_id) {
    if (!palette_owns() || !g_history_paths) return "";
    if (item_id < 0 || item_id >= MAX_PALETTE_ITEMS) return "";
    return g_history_paths[item_id];
}

const char *palette_history_session_cwd(i32 item_id) {
    if (!palette_owns() || !g_history_cwds) return "";
    if (item_id < 0 || item_id >= MAX_PALETTE_ITEMS) return "";
    return g_history_cwds[item_id];
}

void palette_shutdown(void) {
    /* Flush MRU one last time so any in-memory floats made this session
     * land on disk even if the user didn't trigger a save path. */
    mru_save();

    if (g_history_sids)  { free(g_history_sids);  g_history_sids  = NULL; }
    if (g_history_paths) { free(g_history_paths); g_history_paths = NULL; }
    if (g_history_cwds)  { free(g_history_cwds);  g_history_cwds  = NULL; }
    palette_history_shutdown();
    free(g_palette.items);    g_palette.items = NULL;
    free(g_palette.filtered); g_palette.filtered = NULL;
    /* Clear the counts with the freed arrays so accessors cannot index
     * released storage. Clear the owner as well so a later open can claim
     * its own `AppState`. */
    g_palette.item_count     = 0;
    g_palette.filtered_count = 0;
    g_palette.selected       = 0;
    g_palette.active         = false;
    g_palette_owner          = NULL;
}
