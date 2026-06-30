/*
 * Liu - Command palette (Cmd+K)
 * Fuzzy search overlay for hosts, snippets, and commands.
 */
#include "core/types.h"
#include "core/memory.h"
#include "core/string_utils.h"
#include "core/theme_import.h"
#include "ui/palette.h"
#include "core/sites.h"   /* Site/SiteManager for the launcher mode */
#include "vault/vault.h"
#include "history/event.h"
#include "history/session.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

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

/* =========================================================================
 * MRU cache — most-recently-used ordering for palette items.
 *
 * Stores (text → unix-ms) pairs in a flat array, persisted to
 * `~/.config/Liu/palette_mru` as one line per entry: "<ts>|<text>". When
 * the user activates an item we bump its timestamp; when the palette
 * opens we use these timestamps to reorder items[] so the most-recently
 * used row floats to the top. Items the user has never picked keep their
 * authored relative order at the bottom.
 *
 * All text matching is by exact string compare against PaletteItem.text.
 * That works for static commands (stable strings) and dynamic rows whose
 * label is reconstructable across sessions (host labels, snippet labels,
 * "Theme: <name>", "Set Profile: <name>"). Agent-history rows have
 * per-session labels and won't accumulate useful MRU data — that's fine,
 * the sort just leaves them where the scanner placed them.
 * ========================================================================= */

#define MAX_MRU_ENTRIES 256

typedef struct {
    char text[256];
    i64  ts_ms;
} MruEntry;

static MruEntry g_mru[MAX_MRU_ENTRIES];
static i32      g_mru_count  = 0;
static bool     g_mru_loaded = false;

static void mru_path(char *out, usize cap) {
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/tmp";
    snprintf(out, cap, "%s/.config/Liu/palette_mru", home);
}

static void mru_ensure_dir(const char *path) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if (!sl) return;
    *sl = '\0';
    /* Best effort — mkdir with EEXIST is fine; permission failures will be
     * surfaced when the subsequent fopen() returns NULL and we silently
     * skip persistence (MRU still works in-memory for the session). */
    mkdir(dir, 0755);
}

static void mru_load(void) {
    if (g_mru_loaded) return;
    g_mru_loaded = true;
    char path[1024]; mru_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f) && g_mru_count < MAX_MRU_ENTRIES) {
        char *bar = strchr(line, '|');
        if (!bar) continue;
        *bar = '\0';
        i64 ts = (i64)strtoll(line, NULL, 10);
        char *txt = bar + 1;
        usize tl = strlen(txt);
        while (tl > 0 && (txt[tl-1] == '\n' || txt[tl-1] == '\r')) txt[--tl] = '\0';
        if (tl == 0 || ts <= 0) continue;
        snprintf(g_mru[g_mru_count].text, sizeof g_mru[0].text, "%s", txt);
        g_mru[g_mru_count].ts_ms = ts;
        g_mru_count++;
    }
    fclose(f);
}

static void mru_save(void) {
    char path[1024]; mru_path(path, sizeof path);
    mru_ensure_dir(path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (i32 i = 0; i < g_mru_count; i++) {
        fprintf(f, "%lld|%s\n",
                (long long)g_mru[i].ts_ms, g_mru[i].text);
    }
    fclose(f);
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
    return (i64)ts.tv_sec * 1000 + (i64)(ts.tv_nsec / 1000000);
}

void palette_mru_record(const char *text) {
    if (!text || !*text) return;
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
 * so we don't repeat the linear mru_get scan inside the inner loop. */
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
     * so two inline 256-entry arrays (~198 KB) were permanently resident for
     * nothing. NULL while closed; accessors stay safe because the counts are
     * zeroed on close. */
    PaletteItem *items;
    i32          item_count;
    PaletteItem *filtered;
    i32          filtered_count;
    i32          selected;
} PaletteState;

static PaletteState g_palette = {0};

/* "Restart Site" enters the sites list with restart-on-Enter semantics.
 * Mode-scoped state, same pattern as g_history_filter_cwd; cleared on
 * palette_open and on every plain sites-mode entry. */
static bool g_sites_restart_mode = false;

/* Built-in commands (static portion) */
static const char *g_commands_static[] = {
    "New Tab",
    "Close Tab",
    "Split Horizontal",
    "Split Vertical",
    "Toggle Sidebar",
    "Toggle File Browser",
    "Graph View",            /* knowledge graph of the notes Vault */
    "Graph Current Folder",  /* graph the active folder instead of the Vault */
    "Open Vault",            /* open the notes Vault folder in the file browser */
    "Toggle Broadcast",
    "Broadcast Targets",
    "Port Forwarding",
    "SSH Connect",            /* opens the in-app SSH dialog (libssh2 path, SFTP-ready) */
    "Import SSH Config",
    "Create Theme",        /* AI-driven theme generation via local CLI agent */
    "Create Note",         /* AI-driven Markdown doc generation into the Vault */
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
    "Settings: Notifications",
    "Rename Group",
    "Close Group",
    "Toggle Group Open/Close",
    "Mosh Connect",
    "Telnet Connect",
    "Serial Connect",
    "Agent History",          /* push palette into history mode (Claude/Codex/Copilot) */
    "Go to Heading",          /* push palette into markdown outline mode */
    "Open Note",              /* push palette into markdown quick switcher */
    "Search Notes",           /* push palette into markdown vault content search */
    "Show Backlinks",         /* notes linking to the current note */
    "Filter Graph by Folder", /* scope the knowledge graph to a folder */
    "Toggle Quake Mode",
    "Create Tab Group",
    "Ungroup Current Tab",
    "Toggle Tab Sleep (this tab)",
    "Sleep This Tab Now",
    "Wake All Tabs",
    "Toggle Render Benchmark",
    "Toggle Tab Bar",
    "Toggle Toolbar Icons",
    "Toggle Scrollbar",
    "Toggle Status Bar",
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
    "Toggle Option as Alt",
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

/* Config pointer for profile access */
static const AppConfig *g_palette_config = NULL;

void palette_set_config(const AppConfig *cfg) {
    g_palette_config = cfg;
}

static void palette_rebuild_commands(void) {
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

    /* Theme deletion is intentionally NOT exposed in the palette — it lives
     * only in Settings → Appearance (×) where the user is in a deliberate
     * "I'm managing themes" mindset, not skimming a fuzzy command list. */
    i32 di = theme_count;

    /* Profile commands */
    if (g_palette_config) {
        for (i32 i = 0; i < g_palette_config->profile_count && n < MAX_DYNAMIC_CMDS + 60; i++) {
            i32 pdi = di + i;
            if (pdi >= MAX_DYNAMIC_CMDS) break;
            snprintf(g_dynamic_cmds[pdi], sizeof(g_dynamic_cmds[pdi]),
                     "Set Profile: %s", g_palette_config->profiles[i].name);
            g_commands[n++] = g_dynamic_cmds[pdi];
        }
    }

    g_commands[n] = NULL;
}

/* Streaming snippet → palette item adapter. user points at an i32 next_id
 * counter that we monotonically increment per row. */
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

void palette_open(Vault *v) {
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
    palette_rebuild_commands();

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

    /* Load snippets — streaming so we don't put a 600 KB VaultSnippet[128]
     * array on the stack just to copy each label/detail into a PaletteItem. */
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

void palette_close(void) {
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
}

bool palette_is_open(void) {
    return g_palette.active;
}

void palette_set_query(const char *query) {
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
    if (g_palette.query_len < 127) {
        g_palette.query[g_palette.query_len++] = (char)codepoint;
        g_palette.query[g_palette.query_len] = '\0';
        palette_set_query(g_palette.query);
    }
}

void palette_backspace(void) {
    if (g_palette.query_len > 0) {
        g_palette.query[--g_palette.query_len] = '\0';
        palette_set_query(g_palette.query);
    }
}

void palette_move_up(void) {
    if (g_palette.selected > 0) g_palette.selected--;
}

void palette_move_down(void) {
    if (g_palette.selected < g_palette.filtered_count - 1) g_palette.selected++;
}

PaletteItem *palette_get_selected(void) {
    if (g_palette.selected < 0 || g_palette.selected >= g_palette.filtered_count)
        return NULL;
    return &g_palette.filtered[g_palette.selected];
}

i32 palette_filtered_count(void) {
    return g_palette.filtered_count;
}

PaletteItem *palette_get_item(i32 index) {
    if (index < 0 || index >= g_palette.filtered_count) return NULL;
    return &g_palette.filtered[index];
}

i32 palette_selected_index(void) {
    return g_palette.selected;
}

const char *palette_query(void) {
    return g_palette.query;
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
 * the machine-friendly lowercase id ("claude", "roo", ...) which leaks into
 * the UI as-is; this returns the human-friendly form for row subtitles.
 * Falls through to chat_tool_name() for tools we haven't titled yet. */
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
 * Show just the last segment so the row stays readable. */
static void fmt_project_label(const char *slug, char *out, usize cap) {
    if (!slug || !*slug || strcmp(slug, "-") == 0) {
        snprintf(out, cap, "(no project)");
        return;
    }
    const char *last = strrchr(slug, '-');
    const char *base = (last && last[1] != '\0') ? last + 1 : slug;
    if (!*base) base = slug;
    snprintf(out, cap, "%s", base);
}

/* Absolute cwd to filter scan results against. Empty string means "no filter
 * — show everything". Populated by palette_enter_history_mode before
 * chat_scan starts; the callback consults it per row. */
static char g_history_filter_cwd[1024] = {0};

static bool history_scan_cb(const ChatSessionMeta *m, void *user) {
    (void)user;
    if (g_palette.item_count >= MAX_PALETTE_ITEMS) return false;

    /* cwd-scoped mode: only emit sessions whose recorded cwd matches the
     * filter. Scanner populates m->cwd best-effort:
     *   - Claude: from the project slug (instant).
     *   - Codex:  from the session_meta header on the first JSONL line.
     *   - Other tools: NULL (cwd not extractable cheaply).
     * Sessions without a known cwd are skipped under filter mode — the
     * user has narrowed to a specific project and unmatchable rows are
     * noise. Use $HOME (= no filter) to see everything. */
    if (g_history_filter_cwd[0]) {
        if (!m->cwd || strcmp(m->cwd, g_history_filter_cwd) != 0) {
            return true;   /* skip but keep scanning */
        }
    }

    if (!g_history_sids) {
        g_history_sids = calloc((usize)MAX_PALETTE_ITEMS, HISTORY_SID_CAP);
        if (!g_history_sids) return false; /* out of memory — bail scan */
    }
    if (!g_history_paths) {
        g_history_paths = calloc((usize)MAX_PALETTE_ITEMS, HISTORY_PATH_CAP);
        if (!g_history_paths) return false;
    }

    i32 idx = g_palette.item_count;
    PaletteItem *it = &g_palette.items[idx];
    memset(it, 0, sizeof *it);
    it->type = 4;                 /* agent-history */
    it->tool = (u8)m->tool;
    it->id   = idx;
    snprintf(g_history_sids[idx],  HISTORY_SID_CAP,  "%s", m->session_id);
    snprintf(g_history_paths[idx], HISTORY_PATH_CAP, "%s", m->path);

    /* Primary label (text) — just the project basename. Bright, big. */
    fmt_project_label(m->project, it->text, sizeof it->text);

    /* Secondary label (detail) — subtitle rendered dim on row 2.
     * Format: "<tool>  ·  <short-id>  ·  <time>  ·  <size>"
     * All parts stay fuzzy-searchable. */
    const char *sid = m->session_id;
    usize sidlen = strlen(sid);
    char short_sid[28];
    if (sidlen > 16) snprintf(short_sid, sizeof short_sid, "%.10s…%.4s", sid, sid + sidlen - 4);
    else             snprintf(short_sid, sizeof short_sid, "%s", sid);

    char ts[24]; fmt_session_time(m->last_modified_ms, ts, sizeof ts);
    char sz[16]; fmt_session_bytes(m->size_bytes,     sz, sizeof sz);
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
    g_palette.item_count++;
    return true;
}

void palette_enter_history_mode(const char *cwd_filter) {
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

    /* Stash the absolute cwd filter so history_scan_cb can compare each
     * session's recorded cwd against it. Trailing slashes are trimmed so
     * "/foo/" and "/foo" are treated identically. */
    g_history_filter_cwd[0] = '\0';
    if (cwd_filter && *cwd_filter) {
        usize plen = strlen(cwd_filter);
        while (plen > 1 && cwd_filter[plen - 1] == '/') plen--;
        if (plen > 0 && plen + 1 < sizeof(g_history_filter_cwd)) {
            memcpy(g_history_filter_cwd, cwd_filter, plen);
            g_history_filter_cwd[plen] = '\0';
        }
    }

    /* The arena holds string fields (path/project/session_id/cwd) for every
     * scanned session — even ones we ultimately skip due to the cwd filter.
     * A heavy user can easily hit 500+ sessions × ~600 B each, so the
     * historical KB(64) silently truncated every tool after Claude/Codex
     * and only Claude/Codex showed in the picker. */
    Arena a = arena_create(MB(16));
    chat_scan(&a, 0, history_scan_cb, NULL);
    arena_destroy(&a);

    /* Initial view: show all. */
    g_palette.filtered_count = g_palette.item_count;
    memcpy(g_palette.filtered, g_palette.items,
           (usize)g_palette.item_count * sizeof(PaletteItem));
}


bool palette_sites_restart_mode(void) { return g_sites_restart_mode; }

void palette_enter_sites_restart_mode(const struct SiteManager *m) {
    g_sites_restart_mode = true;
    palette_enter_sites_mode(m);
    g_sites_restart_mode = true;   /* enter_sites_mode clears it; re-arm */
}

void palette_enter_sites_mode(const struct SiteManager *m) {
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
        const Site *st = &m->sites[i];
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

void palette_exit_history_mode(Vault *v) {
    g_palette.mode = PALETTE_MODE_ROOT;
    palette_open(v); /* re-populate root items + reset query */
}

void palette_enter_outline_mode(const PaletteOutlineEntry *entries, u32 count) {
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
    return g_palette.mode;
}

const char *palette_history_session_id(i32 item_id) {
    if (!g_history_sids) return "";
    if (item_id < 0 || item_id >= MAX_PALETTE_ITEMS) return "";
    return g_history_sids[item_id];
}

const char *palette_history_session_path(i32 item_id) {
    if (!g_history_paths) return "";
    if (item_id < 0 || item_id >= MAX_PALETTE_ITEMS) return "";
    return g_history_paths[item_id];
}

void palette_shutdown(void) {
    /* Flush MRU one last time so any in-memory floats made this session
     * land on disk even if the user didn't trigger a save path. */
    mru_save();

    if (g_history_sids)  { free(g_history_sids);  g_history_sids  = NULL; }
    if (g_history_paths) { free(g_history_paths); g_history_paths = NULL; }
    free(g_palette.items);    g_palette.items = NULL;
    free(g_palette.filtered); g_palette.filtered = NULL;
}
