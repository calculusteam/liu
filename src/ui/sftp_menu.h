/*
 * sftp_menu.h — the SFTP tab's menu: what its rows are, which of them can act,
 * what they say and which key does the same.
 *
 * One menu serves both the pane's Actions button and a right click on a row;
 * it is placed and painted as every context menu is (ui/ctx_menu.h, the
 * painter in ui.c), so it cannot come to look different from them. The rows
 * are in groups — go there or send it across | the clipboard | name things |
 * the listing | destroy — and the one that destroys is last.
 *
 * Pure: no renderer, no AppState.
 */
#ifndef LIU_UI_SFTP_MENU_H
#define LIU_UI_SFTP_MENU_H

#include "core/types.h"
#include "ui/ctx_menu.h"
#include <stdio.h>
#include <string.h>

typedef enum {
    SFTP_ACT_OPEN = 0,      /* a folder: go into it */
    SFTP_ACT_SEND,          /* the selection to the other pane's folder */
    SFTP_ACT_COPY,
    SFTP_ACT_CUT,
    SFTP_ACT_PASTE,
    SFTP_ACT_NEW_FOLDER,
    SFTP_ACT_RENAME,
    SFTP_ACT_REFRESH,
    SFTP_ACT_HIDDEN,        /* show / hide dotfiles */
    SFTP_ACT_HOME,
    SFTP_ACT_DELETE,
    SFTP_ACT_COUNT
} SftpMenuAct;

typedef struct {
    bool remote;            /* the pane is the host's */
    bool connected;         /* the pane can list (always, for the local one) */
    bool other_ready;       /* the other pane can take a transfer */
    bool has_entry;         /* the menu is about a row (or the selection) */
    bool is_dir;            /* ...a folder, when it is one row */
    i32  selection;         /* rows it covers (0 = none) */
    bool clip_has;          /* something is on the file clipboard */
    bool show_hidden;       /* the pane shows dotfiles now */
    bool busy;              /* a transfer is running (one at a time) */
    bool can_trash;         /* a delete here goes to the Trash (local, with one) */
} SftpMenuTarget;

static inline bool sftp_menu_visible(SftpMenuAct a, const SftpMenuTarget *t) {
    switch (a) {
    case SFTP_ACT_OPEN:   return t->has_entry && t->is_dir && t->selection <= 1;
    case SFTP_ACT_SEND:
    case SFTP_ACT_COPY:
    case SFTP_ACT_CUT:
    case SFTP_ACT_RENAME:
    case SFTP_ACT_DELETE: return t->has_entry;
    default:              return a >= 0 && a < SFTP_ACT_COUNT;
    }
}

static inline bool sftp_menu_enabled(SftpMenuAct a, const SftpMenuTarget *t) {
    if (!sftp_menu_visible(a, t)) return false;
    if (!t->connected && a != SFTP_ACT_HIDDEN) return false;
    switch (a) {
    case SFTP_ACT_SEND:   return t->other_ready && !t->busy && t->selection > 0;
    case SFTP_ACT_PASTE:  return t->clip_has && !t->busy;
    case SFTP_ACT_RENAME: return t->selection == 1;
    default:              return true;
    }
}

static inline bool sftp_menu_destructive(SftpMenuAct a) { return a == SFTP_ACT_DELETE; }

static inline i32 sftp_menu_group(SftpMenuAct a) {
    switch (a) {
    case SFTP_ACT_OPEN: case SFTP_ACT_SEND:                          return 0;
    case SFTP_ACT_COPY: case SFTP_ACT_CUT: case SFTP_ACT_PASTE:      return 1;
    case SFTP_ACT_NEW_FOLDER: case SFTP_ACT_RENAME:                  return 2;
    case SFTP_ACT_REFRESH: case SFTP_ACT_HIDDEN: case SFTP_ACT_HOME: return 3;
    default:                                                         return 4;
    }
}

/* The row's words. `host` names the far side for an upload ("" = "Remote").
 * A trailing ellipsis: this asks before it acts. */
static inline const char *sftp_menu_label(SftpMenuAct a, const SftpMenuTarget *t,
                                          const char *host, char *buf, usize cap) {
    i32 n = t ? t->selection : 0;
    bool many = n > 1;
    switch (a) {
    case SFTP_ACT_OPEN: return "Open";
    case SFTP_ACT_SEND:
        if (t && t->remote) {
            if (many) snprintf(buf, cap, "Download %d Items", (int)n);
            else      snprintf(buf, cap, "Download");
        } else {
            const char *h = host && host[0] ? host : "Remote";
            if (many) snprintf(buf, cap, "Upload %d Items to %s", (int)n, h);
            else      snprintf(buf, cap, "Upload to %s", h);
        }
        return buf;
    case SFTP_ACT_COPY:
        if (!many) return "Copy";
        snprintf(buf, cap, "Copy %d Items", (int)n);
        return buf;
    case SFTP_ACT_CUT:
        if (!many) return "Cut";
        snprintf(buf, cap, "Cut %d Items", (int)n);
        return buf;
    case SFTP_ACT_PASTE:      return "Paste";
    case SFTP_ACT_NEW_FOLDER: return "New Folder\xe2\x80\xa6";
    case SFTP_ACT_RENAME:     return "Rename\xe2\x80\xa6";
    case SFTP_ACT_REFRESH:    return "Refresh";
    case SFTP_ACT_HIDDEN:     return t && t->show_hidden ? "Hide Hidden Files" : "Show Hidden Files";
    case SFTP_ACT_HOME:       return "Go to Home Folder";
    case SFTP_ACT_DELETE:
        /* Recoverable: it just happens. Permanent (the host): it asks first. */
        if (t && t->can_trash) {
            if (!many) return "Move to Trash";
            snprintf(buf, cap, "Move %d Items to Trash", (int)n);
            return buf;
        }
        if (!many) return "Delete\xe2\x80\xa6";
        snprintf(buf, cap, "Delete %d Items\xe2\x80\xa6", (int)n);
        return buf;
    default: return "";
    }
}

/* The key that does the same from the list, in keybind_format's spelling. */
static inline const char *sftp_menu_shortcut(SftpMenuAct a, const char *mod, char *buf, usize cap) {
    switch (a) {
    case SFTP_ACT_OPEN:       return "Enter";
    case SFTP_ACT_SEND:       snprintf(buf, cap, "%s+Enter", mod); return buf;
    case SFTP_ACT_COPY:       snprintf(buf, cap, "%s+C", mod); return buf;
    case SFTP_ACT_CUT:        snprintf(buf, cap, "%s+X", mod); return buf;
    case SFTP_ACT_PASTE:      snprintf(buf, cap, "%s+V", mod); return buf;
    case SFTP_ACT_NEW_FOLDER: snprintf(buf, cap, "Shift+%s+N", mod); return buf;
    case SFTP_ACT_RENAME:     return "F2";
    case SFTP_ACT_REFRESH:    snprintf(buf, cap, "%s+R", mod); return buf;
    case SFTP_ACT_HIDDEN:     snprintf(buf, cap, "Shift+%s+.", mod); return buf;
    case SFTP_ACT_HOME:       snprintf(buf, cap, "Shift+%s+H", mod); return buf;
    case SFTP_ACT_DELETE:     snprintf(buf, cap, "%s+Backspace", mod); return buf;
    default:                  return "";
    }
}

/* The visible rows in order, and which of them a hairline follows. */
typedef struct {
    i32         count;
    SftpMenuAct acts[SFTP_ACT_COUNT];
    bool        sep_after[SFTP_ACT_COUNT];
    bool        enabled[SFTP_ACT_COUNT];
} SftpMenuRows;

static inline void sftp_menu_rows(const SftpMenuTarget *t, SftpMenuRows *out) {
    memset(out, 0, sizeof *out);
    for (i32 a = 0; a < SFTP_ACT_COUNT; a++) {
        if (!sftp_menu_visible((SftpMenuAct)a, t)) continue;
        if (out->count > 0 && sftp_menu_group(out->acts[out->count - 1]) != sftp_menu_group((SftpMenuAct)a))
            out->sep_after[out->count - 1] = true;
        out->enabled[out->count] = sftp_menu_enabled((SftpMenuAct)a, t);
        out->acts[out->count++] = (SftpMenuAct)a;
    }
}

_Static_assert(SFTP_ACT_COUNT <= CTX_MENU_MAX_ROWS, "the SFTP menu must fit the shared box");

#endif
