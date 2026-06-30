/*
 * Liu - broadcast input + workspace management
 */
#include "core/types.h"
#include "ssh/ssh_session.h"
#include "terminal/terminal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Broadcast input
 * ========================================================================= */

typedef struct {
    bool    enabled;
    bool    targets[32]; /* which tabs receive broadcast */
    i32     target_count;
} BroadcastState;

static BroadcastState g_broadcast = {0};

void broadcast_enable(bool all_tabs, i32 tab_count) {
    g_broadcast.enabled = true;
    g_broadcast.target_count = tab_count;
    for (i32 i = 0; i < tab_count && i < 32; i++) {
        g_broadcast.targets[i] = all_tabs;
    }
}

void broadcast_disable(void) {
    g_broadcast.enabled = false;
}

bool broadcast_is_active(void) {
    return g_broadcast.enabled;
}

void broadcast_toggle_target(i32 tab_index) {
    if (tab_index >= 0 && tab_index < 32) {
        g_broadcast.targets[tab_index] = !g_broadcast.targets[tab_index];
    }
}

/* Send data to all broadcast targets */
void broadcast_write(Session **sessions, i32 count, const u8 *data, i32 len) {
    if (!g_broadcast.enabled) return;
    for (i32 i = 0; i < count && i < 32; i++) {
        if (g_broadcast.targets[i] && sessions[i]) {
            session_write(sessions[i], data, len);
        }
    }
}

/* =========================================================================
 * Workspace
 * ========================================================================= */

#define MAX_WORKSPACE_TABS 32

typedef struct {
    char  name[128];
    i32   tab_count;
    struct {
        char  title[128];
        char  host_id[64];  /* empty = local terminal */
        bool  is_ssh;
    } tabs[MAX_WORKSPACE_TABS];
} Workspace;

#define MAX_WORKSPACES 16
static Workspace *g_workspaces = NULL;
static i32 g_workspace_count = 0;
static i32 g_workspace_cap = 0;

i32 workspace_save(const char *name, const char (*titles)[128], const char (*host_ids)[64],
                    const bool *is_ssh, i32 tab_count) {
    if (g_workspace_count >= MAX_WORKSPACES) return -1;

    /* Lazy allocate */
    if (!g_workspaces) {
        g_workspace_cap = 4;
        g_workspaces = calloc((usize)g_workspace_cap, sizeof(Workspace));
        if (!g_workspaces) return -1;
    }
    if (g_workspace_count >= g_workspace_cap) {
        i32 new_cap = g_workspace_cap * 2;
        if (new_cap > MAX_WORKSPACES) new_cap = MAX_WORKSPACES;
        Workspace *grown = realloc(g_workspaces, (usize)new_cap * sizeof(Workspace));
        if (!grown) return -1;
        g_workspaces = grown;
        g_workspace_cap = new_cap;
    }

    Workspace *ws = &g_workspaces[g_workspace_count];
    snprintf(ws->name, sizeof(ws->name), "%s", name);
    ws->tab_count = tab_count < MAX_WORKSPACE_TABS ? tab_count : MAX_WORKSPACE_TABS;

    for (i32 i = 0; i < ws->tab_count; i++) {
        snprintf(ws->tabs[i].title, sizeof(ws->tabs[i].title), "%s", titles[i]);
        snprintf(ws->tabs[i].host_id, sizeof(ws->tabs[i].host_id), "%s", host_ids[i]);
        ws->tabs[i].is_ssh = is_ssh[i];
    }

    return g_workspace_count++;
}

i32 workspace_count(void) { return g_workspace_count; }

const char *workspace_name(i32 index) {
    if (!g_workspaces || index < 0 || index >= g_workspace_count) return NULL;
    return g_workspaces[index].name;
}

Workspace *workspace_get(i32 index) {
    if (!g_workspaces || index < 0 || index >= g_workspace_count) return NULL;
    return &g_workspaces[index];
}
