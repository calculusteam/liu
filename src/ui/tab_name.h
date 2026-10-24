/*
 * Liu — what a file-browser tab is called.
 *
 * Kept apart from app_refresh_fb_tab_title as a pure rule, because the name
 * is the only thing telling the user which of several file-browser tabs is the
 * Vault graph, which is a folder graph, and which has a note open. Getting it
 * wrong is silent — the pill just says something that is no longer true.
 *
 * The rule, in order:
 *   1. a document open in the viewer  -> the DOCUMENT's name
 *   2. a graph rooted at the Vault    -> "Graph View"
 *   3. a graph rooted anywhere else   -> that folder's name
 *   4. otherwise                      -> the browser's cwd, as "~" / "/" / basename
 *
 * Every input is a plain value, so the caller does the field plumbing and this
 * stays a pure function of what is on screen — which is the point: the name has
 * to follow the live state, not the state the tab was opened in.
 */
#ifndef UI_TAB_NAME_H
#define UI_TAB_NAME_H

#include "core/types.h"

#include <stdio.h>
#include <string.h>

/* What the viewer is showing, reduced to the two questions the name asks. */
typedef struct {
    bool        doc_open;      /* a document (not a graph) is open in the viewer */
    const char *doc_path;      /* its path; may be NULL/empty                    */
    bool        graph_open;    /* the viewer is the knowledge graph              */
    const char *graph_root;    /* the folder the LIVE graph is rooted at         */
    const char *vault_root;    /* the Vault's path, or NULL/empty if unknown     */
    const char *cwd;           /* the browser's folder                           */
    const char *home;          /* $HOME, or NULL to skip the "~" rule            */
} TabNameInputs;

#define TAB_NAME_GRAPH_VIEW "Graph View"

static inline const char *tab_name_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return (slash && slash[1]) ? slash + 1 : path;
}

/* Write the tab's name into `out`. Never fails; `out` is always NUL-terminated
 * and never empty for a non-empty input set. */
static inline void tab_name_for(const TabNameInputs *in, char *out, usize cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!in) return;

    /* 1. A document is named after itself, the way every editor names its tabs
     *    — the containing folder is already visible in the explorer rail. */
    if (in->doc_open && in->doc_path && in->doc_path[0]) {
        const char *base = tab_name_basename(in->doc_path);
        if (base[0]) { snprintf(out, cap, "%s", base); return; }
    }

    /* 2. The Vault graph is ONE known thing and names itself. The test is on
     *    the LIVE root, not on how the tab was opened: narrowing the graph to a
     *    subfolder, or closing it and graphing something else, must rename the
     *    tab to match what it is now showing. A flag recording the opening act
     *    goes on lying the moment the graph moves. */
    if (in->graph_open && in->vault_root && in->vault_root[0] &&
        in->graph_root && in->graph_root[0] &&
        strcmp(in->graph_root, in->vault_root) == 0) {
        snprintf(out, cap, TAB_NAME_GRAPH_VIEW);
        return;
    }

    /* 3./4. The graphed folder if there is one — the browser's cwd can still be
     *       "/" or wherever the tab was created — otherwise the cwd. */
    const char *dir = (in->graph_open && in->graph_root && in->graph_root[0])
                        ? in->graph_root : in->cwd;
    if (!dir || !dir[0]) { snprintf(out, cap, "Files"); return; }

    char trimmed[1024];
    snprintf(trimmed, sizeof trimmed, "%s", dir);
    usize tlen = strlen(trimmed);
    while (tlen > 1 && trimmed[tlen - 1] == '/') trimmed[--tlen] = '\0';

    if (tlen == 1 && trimmed[0] == '/') { snprintf(out, cap, "/"); return; }

    /* The home directory ITSELF is "~"; anything under it keeps its basename,
     * which is shorter and just as identifying. */
    if (in->home && in->home[0]) {
        usize hlen = strlen(in->home);
        while (hlen > 1 && in->home[hlen - 1] == '/') hlen--;
        if (tlen == hlen && strncmp(trimmed, in->home, hlen) == 0) {
            snprintf(out, cap, "~");
            return;
        }
    }

    const char *base = tab_name_basename(trimmed);
    if (!base[0]) base = "/";
    snprintf(out, cap, "%s", base);
}

#endif /* UI_TAB_NAME_H */
