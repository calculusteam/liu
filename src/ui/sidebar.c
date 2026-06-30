/*
 * Liu - sidebar (host list, SFTP browser, snippet library)
 * Rendered via the main OpenGL renderer, state managed here.
 *
 * Host list is organized into sections:
 *   1. Favorites (starred hosts)
 *   2. Recent   (last 10 connections from history)
 *   3. Hosts    (all vault hosts)
 */
#include "core/types.h"
#include "vault/vault.h"
#include <stdlib.h>
#include <string.h>

/* Maximum items per section */
#define SIDEBAR_MAX_FAVORITES 16
#define SIDEBAR_MAX_RECENT    10

/* =========================================================================
 * Sidebar item (unified across sections)
 * ========================================================================= */

typedef enum {
    SIDEBAR_SECTION_FAVORITES,
    SIDEBAR_SECTION_RECENT,
    SIDEBAR_SECTION_HOSTS,
} SidebarSection;

typedef struct {
    SidebarSection section;
    union {
        VaultHost         host;
        VaultHistoryEntry history;
    };
} SidebarItem;

/* =========================================================================
 * Host list state
 * ========================================================================= */

typedef struct {
    VaultHost *hosts;
    i32        host_count;
    i32        host_cap;

    VaultHost         favorites[SIDEBAR_MAX_FAVORITES];
    i32               fav_count;

    VaultHistoryEntry recent[SIDEBAR_MAX_RECENT];
    i32               recent_count;

    i32        selected;
    i32        scroll_offset;
    bool       loaded;
} HostListState;

static HostListState g_host_list = {0};

void sidebar_load_hosts(Vault *v) {
    if (!v) return;
    if (!g_host_list.hosts) {
        g_host_list.host_cap = 32;
        g_host_list.hosts = malloc((usize)g_host_list.host_cap * sizeof(VaultHost));
        if (!g_host_list.hosts) return;
    }
    g_host_list.host_count = vault_host_list(v, g_host_list.hosts, g_host_list.host_cap);
    g_host_list.loaded = true;
    g_host_list.selected = -1;
    g_host_list.scroll_offset = 0;

    /* Load favorites */
    g_host_list.fav_count = vault_get_favorites(v, g_host_list.favorites, SIDEBAR_MAX_FAVORITES);
    if (g_host_list.fav_count < 0) g_host_list.fav_count = 0;

    /* Load recent history */
    g_host_list.recent_count = vault_get_history(v, g_host_list.recent, SIDEBAR_MAX_RECENT);
    if (g_host_list.recent_count < 0) g_host_list.recent_count = 0;
}

/* Release the module-global host-list buffer at shutdown (the OS would reclaim
 * it anyway; this keeps leak-checkers quiet). Idempotent. */
void sidebar_free_hosts(void) {
    free(g_host_list.hosts);
    g_host_list.hosts = NULL;
    g_host_list.host_count = 0;
    g_host_list.host_cap = 0;
    g_host_list.loaded = false;
}

/* =========================================================================
 * Accessors for vault hosts (existing API, unchanged)
 * ========================================================================= */

i32 sidebar_host_count(void) {
    return g_host_list.host_count;
}

VaultHost *sidebar_get_host(i32 index) {
    if (index < 0 || index >= g_host_list.host_count) return NULL;
    return &g_host_list.hosts[index];
}

void sidebar_select_host(i32 index) {
    if (index >= -1 && index < g_host_list.host_count) {
        g_host_list.selected = index;
    }
}

i32 sidebar_selected_host(void) {
    return g_host_list.selected;
}

void sidebar_scroll(i32 delta) {
    g_host_list.scroll_offset += delta;
    if (g_host_list.scroll_offset < 0) g_host_list.scroll_offset = 0;
}

/* =========================================================================
 * Favorites section accessors
 * ========================================================================= */

i32 sidebar_favorites_count(void) {
    return g_host_list.fav_count;
}

VaultHost *sidebar_get_favorite(i32 index) {
    if (index < 0 || index >= g_host_list.fav_count) return NULL;
    return &g_host_list.favorites[index];
}

/* =========================================================================
 * Recent history section accessors
 * ========================================================================= */

i32 sidebar_recent_count(void) {
    return g_host_list.recent_count;
}

VaultHistoryEntry *sidebar_get_recent(i32 index) {
    if (index < 0 || index >= g_host_list.recent_count) return NULL;
    return &g_host_list.recent[index];
}

/* =========================================================================
 * Composite item count for rendering
 *
 * Total visible items = section headers + items per section.
 * Section headers are rendered as non-clickable labels.
 * Layout (from top):
 *   [Favorites header]  (if fav_count > 0)
 *   fav_count items
 *   [Recent header]     (if recent_count > 0)
 *   recent_count items
 *   [Hosts header]      (always, if host_count > 0)
 *   host_count items
 * ========================================================================= */

i32 sidebar_total_rows(void) {
    i32 total = 0;
    if (g_host_list.fav_count > 0)
        total += 1 + g_host_list.fav_count;     /* header + items */
    if (g_host_list.recent_count > 0)
        total += 1 + g_host_list.recent_count;   /* header + items */
    if (g_host_list.host_count > 0)
        total += 1 + g_host_list.host_count;     /* header + items */
    return total;
}

/*
 * Resolve a visual row index into a SidebarItem.
 * Returns true on success (item is populated), false if the row is a section
 * header (in that case *section_name is set if non-NULL).
 */
bool sidebar_resolve_row(i32 row, SidebarItem *item, const char **section_name) {
    i32 offset = 0;

    /* Favorites section */
    if (g_host_list.fav_count > 0) {
        if (row == offset) {
            if (section_name) *section_name = "Favorites";
            return false; /* header row */
        }
        offset++;
        if (row < offset + g_host_list.fav_count) {
            if (item) {
                item->section = SIDEBAR_SECTION_FAVORITES;
                item->host = g_host_list.favorites[row - offset];
            }
            return true;
        }
        offset += g_host_list.fav_count;
    }

    /* Recent section */
    if (g_host_list.recent_count > 0) {
        if (row == offset) {
            if (section_name) *section_name = "Recent";
            return false;
        }
        offset++;
        if (row < offset + g_host_list.recent_count) {
            if (item) {
                item->section = SIDEBAR_SECTION_RECENT;
                item->history = g_host_list.recent[row - offset];
            }
            return true;
        }
        offset += g_host_list.recent_count;
    }

    /* Hosts section */
    if (g_host_list.host_count > 0) {
        if (row == offset) {
            if (section_name) *section_name = "Hosts";
            return false;
        }
        offset++;
        if (row < offset + g_host_list.host_count) {
            if (item) {
                item->section = SIDEBAR_SECTION_HOSTS;
                item->host = g_host_list.hosts[row - offset];
            }
            return true;
        }
    }

    return false;
}
