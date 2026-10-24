/*
 * fb_menu.h — the file browser's context menu and its prompt: what is on
 * them, where it is, and what may be done.
 *
 * One author: the renderer, the hover tracker and the click handler all ask
 * here, so the menu and the prompt have the same size when they are clicked
 * as when they are drawn.
 *
 * It also decides what the menu OFFERS. A row that has no meaning for the
 * target (Rename on "..", Delete on nothing) is not on the menu; one that has
 * a meaning but cannot act right now (Paste with nothing copied) is drawn dim
 * and is not a click target.
 *
 * And it names what is wrong with a name BEFORE the file system is asked, so
 * the prompt can say why a name is refused instead of a bare "Rename failed".
 *
 * Pure: no renderer, no AppState.
 */
#ifndef LIU_UI_FB_MENU_H
#define LIU_UI_FB_MENU_H

#include "core/types.h"
#include "ui/ctx_menu.h"
#include "ui/layout.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- The menu ----------------------------------------------------------- */

typedef enum {
    FB_ACT_OPEN = 0,
    FB_ACT_OPEN_EDITOR,
    FB_ACT_RENAME,
    FB_ACT_COPY,
    FB_ACT_CUT,
    FB_ACT_PASTE,
    FB_ACT_NEW_FILE,
    FB_ACT_NEW_FOLDER,
    FB_ACT_DELETE,
    FB_ACT_COUNT
} FbMenuAct;

/* What the menu was opened on. */
typedef struct {
    bool has_entry;     /* a row — false for the empty space under the list */
    bool is_parent;     /* the ".." row */
    bool is_dir;
    bool remote;        /* an SFTP listing */
    bool clip_has;      /* something is on the file clipboard */
    bool can_trash;     /* a delete here can be taken back (local + a Trash) */
    i32  selection;     /* rows the action covers; 0 or 1 = just this one */
} FbMenuTarget;

/* Does the row mean anything for this target? */
static inline bool fb_menu_visible(FbMenuAct a, const FbMenuTarget *t) {
    bool item = t->has_entry && !t->is_parent;      /* a real file or folder */
    switch (a) {
    case FB_ACT_OPEN:        return t->has_entry;
    case FB_ACT_OPEN_EDITOR: return item && !t->is_dir;
    case FB_ACT_RENAME:
    case FB_ACT_COPY:
    case FB_ACT_CUT:
    case FB_ACT_DELETE:      return item;
    case FB_ACT_PASTE:
    case FB_ACT_NEW_FILE:
    case FB_ACT_NEW_FOLDER:  return true;
    default:                 return false;
    }
}

/* Can it act now? Only asked of a visible row. */
static inline bool fb_menu_enabled(FbMenuAct a, const FbMenuTarget *t) {
    if (!fb_menu_visible(a, t)) return false;
    bool many = t->selection > 1;
    switch (a) {
    /* One document, one name: neither is a thing to do to five files. */
    case FB_ACT_OPEN:        return !many;
    case FB_ACT_OPEN_EDITOR: return !many && !t->remote;   /* reads a local path */
    case FB_ACT_RENAME:      return !many;
    case FB_ACT_PASTE:       return t->clip_has;
    default:                 return true;
    }
}

/* The one row that destroys something. Drawn in the danger colour, last. */
static inline bool fb_menu_destructive(FbMenuAct a) { return a == FB_ACT_DELETE; }

/* Groups: open | change | create | destroy. */
static inline i32 fb_menu_group(FbMenuAct a) {
    switch (a) {
    case FB_ACT_OPEN: case FB_ACT_OPEN_EDITOR:              return 0;
    case FB_ACT_RENAME: case FB_ACT_COPY: case FB_ACT_CUT:
    case FB_ACT_PASTE:                                      return 1;
    case FB_ACT_NEW_FILE: case FB_ACT_NEW_FOLDER:           return 2;
    default:                                                return 3;
    }
}

/* The row's label. A trailing ellipsis means "this asks before it acts". */
static inline const char *fb_menu_label(FbMenuAct a, const FbMenuTarget *t,
                                        char *buf, usize cap) {
    i32 n = t ? t->selection : 0;
    bool many = n > 1;
    switch (a) {
    case FB_ACT_OPEN:        return "Open";
    case FB_ACT_OPEN_EDITOR: return "Open in Editor";
    case FB_ACT_RENAME:      return "Rename\xe2\x80\xa6";
    case FB_ACT_COPY:
        if (!many) return "Copy";
        snprintf(buf, cap, "Copy %d Items", (int)n);
        return buf;
    case FB_ACT_CUT:
        if (!many) return "Cut";
        snprintf(buf, cap, "Cut %d Items", (int)n);
        return buf;
    case FB_ACT_PASTE:       return "Paste";
    case FB_ACT_NEW_FILE:    return "New File\xe2\x80\xa6";
    case FB_ACT_NEW_FOLDER:  return "New Folder\xe2\x80\xa6";
    case FB_ACT_DELETE:
        /* Recoverable: it just happens. Permanent: it asks first. */
        if (t && t->can_trash) {
            if (!many) return "Move to Trash";
            snprintf(buf, cap, "Move %d Items to Trash", (int)n);
        } else {
            if (!many) return "Delete\xe2\x80\xa6";
            snprintf(buf, cap, "Delete %d Items\xe2\x80\xa6", (int)n);
        }
        return buf;
    default:                 return "";
    }
}

/* The key that does the same thing from the list, in keybind_format's
 * spelling ("Cmd" is the platform's primary modifier word, passed in). */
static inline const char *fb_menu_shortcut(FbMenuAct a, const char *mod,
                                           char *buf, usize cap) {
    switch (a) {
    case FB_ACT_RENAME:     return "F2";
    case FB_ACT_COPY:       snprintf(buf, cap, "%s+C", mod); return buf;
    case FB_ACT_CUT:        snprintf(buf, cap, "%s+X", mod); return buf;
    case FB_ACT_PASTE:      snprintf(buf, cap, "%s+V", mod); return buf;
    case FB_ACT_NEW_FILE:   snprintf(buf, cap, "%s+N", mod); return buf;
    case FB_ACT_NEW_FOLDER: snprintf(buf, cap, "Shift+%s+N", mod); return buf;
    case FB_ACT_DELETE:     snprintf(buf, cap, "%s+Backspace", mod); return buf;
    default:                return "";
    }
}

typedef struct {
    f32  x, y, w, h;                    /* the panel, clamped into the window */
    f32  row_h;
    i32  count;
    FbMenuAct acts[FB_ACT_COUNT];
    f32  row_y[FB_ACT_COUNT];
    bool enabled[FB_ACT_COUNT];
    bool sep_after[FB_ACT_COUNT];       /* a hairline in the gap under this row */
} FbMenuLayout;

/* Lay the menu out with its top-left at (at_x, at_y), `w` wide (the caller
 * measures the labels), kept inside a win_w x win_h window. Which rows, and
 * where the groups change, is this file's; where that puts them is
 * ctx_menu_place()'s, the one placement every context menu shares. */
static inline void fb_menu_layout(const FbMenuTarget *t, f32 at_x, f32 at_y, f32 w,
                                  f32 win_w, f32 win_h, f32 dpi, FbMenuLayout *out) {
    memset(out, 0, sizeof *out);
    bool sep[FB_ACT_COUNT] = { false };
    for (i32 a = 0; a < FB_ACT_COUNT; a++) {
        if (!fb_menu_visible((FbMenuAct)a, t)) continue;
        if (out->count > 0 &&
            fb_menu_group(out->acts[out->count - 1]) != fb_menu_group((FbMenuAct)a))
            sep[out->count - 1] = true;
        i32 i = out->count++;
        out->acts[i]    = (FbMenuAct)a;
        out->enabled[i] = fb_menu_enabled((FbMenuAct)a, t);
    }
    CtxMenuBox b;
    ctx_menu_place(&b, out->count, sep, at_x, at_y, w, win_w, win_h, dpi);
    out->x = b.x; out->y = b.y; out->w = b.w; out->h = b.h;
    out->row_h = b.row_h;
    for (i32 i = 0; i < out->count; i++) {
        out->row_y[i]     = b.row_y[i];
        out->sep_after[i] = b.sep_after[i];
    }
}

/* The same layout as the shared box, for the shared painter. */
static inline CtxMenuBox fb_menu_box(const FbMenuLayout *l) {
    CtxMenuBox b;
    memset(&b, 0, sizeof b);
    b.x = l->x; b.y = l->y; b.w = l->w; b.h = l->h; b.row_h = l->row_h;
    b.count = l->count;
    for (i32 i = 0; i < l->count; i++) { b.row_y[i] = l->row_y[i]; b.sep_after[i] = l->sep_after[i]; }
    return b;
}

static inline bool fb_menu_contains(const FbMenuLayout *l, f32 px, f32 py) {
    return px >= l->x && px < l->x + l->w && py >= l->y && py < l->y + l->h;
}

/* The row under the pointer, or -1: outside, on the padding, in a separator's
 * gap. A disabled row IS returned; ask l->enabled[] before acting on it. */
static inline i32 fb_menu_row_at(const FbMenuLayout *l, f32 px, f32 py) {
    if (px < l->x || px >= l->x + l->w) return -1;
    for (i32 i = 0; i < l->count; i++)
        if (py >= l->row_y[i] && py < l->row_y[i] + l->row_h) return i;
    return -1;
}

/* The row to HIGHLIGHT for a pointer: the one under it when that row can act. */
static inline i32 fb_menu_hover_at(const FbMenuLayout *l, f32 px, f32 py) {
    i32 i = fb_menu_row_at(l, px, py);
    return (i >= 0 && l->enabled[i]) ? i : -1;
}

/* The next enabled row from `from` in direction `dir` (+1 / -1), wrapping —
 * the arrow keys. `from` < 0 starts at the first (or last) one. -1 when the
 * menu has nothing enabled. */
static inline i32 fb_menu_step(const FbMenuLayout *l, i32 from, i32 dir) {
    return ctx_menu_step(l->count, l->enabled, from, dir);
}

static inline i32 fb_menu_index_of(const FbMenuLayout *l, FbMenuAct a) {
    for (i32 i = 0; i < l->count; i++) if (l->acts[i] == a) return i;
    return -1;
}

/* ---- The prompt --------------------------------------------------------- */

typedef enum {
    FB_PROMPT_RENAME     = 0,
    FB_PROMPT_NEW_FOLDER = 1,
    FB_PROMPT_NEW_FILE   = 2,
    FB_PROMPT_DELETE     = 3,   /* no field: a permanent delete asking first */
} FbPromptMode;

static inline bool fb_prompt_has_field(i32 mode) { return mode != FB_PROMPT_DELETE; }

/* The button Enter presses. */
static inline const char *fb_prompt_ok_label(i32 mode) {
    switch (mode) {
    case FB_PROMPT_RENAME: return "Rename";
    case FB_PROMPT_DELETE: return "Delete";
    default:               return "Create";
    }
}

#define FB_PROMPT_CANCEL_LABEL "Cancel"
#define FB_PROMPT_OK_KEY       "Enter"
#define FB_PROMPT_CANCEL_KEY   "Esc"

typedef struct {
    f32 x, y, w, h;                         /* the panel */
    f32 pad;
    f32 title_y;                            /* title text top */
    f32 sub_y;                              /* the line under it (what / where) */
    bool has_field;
    f32 field_x, field_y, field_w, field_h; /* zero without a field */
    f32 msg_y;                              /* the validation / reason line */
    f32 btn_y, btn_h;
    f32 ok_x, ok_w;                         /* the primary button, rightmost */
    f32 cancel_x, cancel_w;
} FbPromptGeom;

/* A button holds its label, a cell of air, and the key that presses it. */
static inline f32 fb_prompt_btn_w(const char *label, const char *key, f32 ui_cw, f32 dpi) {
    return (f32)(strlen(label) + 1 + strlen(key)) * ui_cw + 2.0f * 12.0f * dpi;
}

static inline FbPromptGeom fb_prompt_geom(f32 win_w, f32 win_h, f32 dpi, f32 ui_cw,
                                          f32 ui_ch, i32 mode) {
    FbPromptGeom g;
    memset(&g, 0, sizeof g);
    g.pad = 20.0f * dpi;
    g.w   = FB_PROMPT_W_PT * dpi;
    if (g.w > win_w - 16.0f * dpi) g.w = win_w - 16.0f * dpi;
    g.has_field = fb_prompt_has_field(mode);
    g.btn_h = FB_PROMPT_BTN_H_PT * dpi;
    f32 y = g.pad;
    g.title_y = y;                 y += ui_ch + 6.0f * dpi;
    g.sub_y   = y;                 y += ui_ch + 14.0f * dpi;
    if (g.has_field) {
        g.field_h = 38.0f * dpi;
        g.field_y = y;             y += g.field_h + 8.0f * dpi;
        g.msg_y   = y;             y += ui_ch + 12.0f * dpi;
    } else {
        g.msg_y   = y;             y += 4.0f * dpi;     /* no line: the sub says it */
    }
    g.btn_y   = y;                 y += g.btn_h + g.pad;
    g.h = y;
    g.x = floorf((win_w - g.w) * 0.5f);
    g.y = floorf((win_h - g.h) * 0.38f);    /* above centre: nearer the eye */
    if (g.y < 8.0f * dpi) g.y = 8.0f * dpi;
    g.title_y += g.y; g.sub_y += g.y; g.msg_y += g.y; g.btn_y += g.y;
    if (g.has_field) {
        g.field_y += g.y;
        g.field_x  = g.x + g.pad;
        g.field_w  = g.w - 2.0f * g.pad;
    }
    g.ok_w     = fb_prompt_btn_w(fb_prompt_ok_label(mode), FB_PROMPT_OK_KEY, ui_cw, dpi);
    g.cancel_w = fb_prompt_btn_w(FB_PROMPT_CANCEL_LABEL, FB_PROMPT_CANCEL_KEY, ui_cw, dpi);
    g.ok_x     = g.x + g.w - g.pad - g.ok_w;
    g.cancel_x = g.ok_x - 8.0f * dpi - g.cancel_w;
    return g;
}

static inline bool fb_prompt_contains(const FbPromptGeom *g, f32 px, f32 py) {
    return px >= g->x && px < g->x + g->w && py >= g->y && py < g->y + g->h;
}
static inline bool fb_prompt_on_ok(const FbPromptGeom *g, f32 px, f32 py) {
    return px >= g->ok_x && px < g->ok_x + g->ok_w &&
           py >= g->btn_y && py < g->btn_y + g->btn_h;
}
static inline bool fb_prompt_on_cancel(const FbPromptGeom *g, f32 px, f32 py) {
    return px >= g->cancel_x && px < g->cancel_x + g->cancel_w &&
           py >= g->btn_y && py < g->btn_y + g->btn_h;
}

/* ---- A name ------------------------------------------------------------- */

typedef enum {
    FB_NAME_OK = 0,
    FB_NAME_EMPTY,        /* nothing typed yet: not an error, Enter does nothing */
    FB_NAME_UNCHANGED,    /* a rename to the same name: Enter just closes */
    FB_NAME_RESERVED,     /* "." or ".." */
    FB_NAME_SEPARATOR,    /* holds a path separator */
    FB_NAME_CONTROL,      /* holds a control byte */
    FB_NAME_TOO_LONG,
    FB_NAME_EXISTS,       /* something else already has it (the caller's look) */
} FbNameVerdict;

#define FB_NAME_MAX_BYTES 255   /* NAME_MAX on every file system Liu lists */

/* Everything that can be said about a name without touching the disk.
 * `original` is the entry being renamed, NULL for a new file or folder. */
static inline FbNameVerdict fb_name_syntax(const char *name, const char *original) {
    if (!name || !name[0]) return FB_NAME_EMPTY;
    bool blank = true;
    usize len = 0;
    for (const u8 *p = (const u8 *)name; *p; p++, len++) {
        if (*p == '/' || *p == '\\') return FB_NAME_SEPARATOR;
        if (*p < 0x20 || *p == 0x7F) return FB_NAME_CONTROL;
        if (*p != ' ') blank = false;
    }
    if (blank) return FB_NAME_EMPTY;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return FB_NAME_RESERVED;
    if (len > FB_NAME_MAX_BYTES) return FB_NAME_TOO_LONG;
    if (original && !strcmp(name, original)) return FB_NAME_UNCHANGED;
    return FB_NAME_OK;
}

/* Is the verdict something to SAY (in the danger colour), and a reason the
 * primary button is not live? */
static inline bool fb_name_is_error(FbNameVerdict v) {
    return v != FB_NAME_OK && v != FB_NAME_EMPTY && v != FB_NAME_UNCHANGED;
}

/* May Enter act on it? (UNCHANGED may: it closes the prompt.) */
static inline bool fb_name_can_apply(FbNameVerdict v) {
    return v == FB_NAME_OK || v == FB_NAME_UNCHANGED;
}

static inline const char *fb_name_message(FbNameVerdict v) {
    switch (v) {
    case FB_NAME_RESERVED:  return "That name is reserved";
    case FB_NAME_SEPARATOR: return "A name cannot contain a slash";
    case FB_NAME_CONTROL:   return "A name cannot contain control characters";
    case FB_NAME_TOO_LONG:  return "That name is too long";
    case FB_NAME_EXISTS:    return "Something with that name is already here";
    default:                return "";
    }
}

/* How much of a name a rename selects when it opens: the part before the last
 * extension, so typing replaces the name and keeps ".md", the usual
 * file-manager convention. A folder, a dotfile and a name with no extension
 * select whole. */
static inline i32 fb_name_stem_len(const char *name, bool is_dir) {
    i32 len = name ? (i32)strlen(name) : 0;
    if (is_dir || len == 0) return len;
    i32 dot = -1;
    for (i32 i = len - 1; i > 0; i--)       /* i > 0: a leading dot is the name */
        if (name[i] == '.') { dot = i; break; }
    return dot > 0 ? dot : len;
}

/* ---- What happened ------------------------------------------------------ */

typedef enum {
    FB_STATUS_INFO = 0,   /* in progress, neutral ("Copied: a.md") */
    FB_STATUS_OK,         /* something was changed on disk */
    FB_STATUS_ERROR,      /* something was NOT done; stays up longer */
} FbStatusKind;

static inline f64 fb_status_hold_sec(FbStatusKind k) {
    return k == FB_STATUS_ERROR ? 6.0 : 3.5;
}

/* The line's opacity `age` seconds into a life of `hold`: a quick arrival, a
 * slower leave. 0 outside its life. */
#define FB_STATUS_FADE_IN_SEC   0.14
#define FB_STATUS_FADE_OUT_SEC  0.30
/* Each fade is asked for a little past its end, so the frame that SETTLES it
 * is painted: the last frame inside a fade is by construction not the final
 * value (a line left at 5% opacity, or arrived at 90%). */
#define FB_STATUS_SETTLE_SEC    0.06
static inline f32 fb_status_alpha(f64 age, f64 hold) {
    if (age < 0.0 || age >= hold) return 0.0f;
    if (age < FB_STATUS_FADE_IN_SEC) return (f32)(age / FB_STATUS_FADE_IN_SEC);
    if (age > hold - FB_STATUS_FADE_OUT_SEC) return (f32)((hold - age) / FB_STATUS_FADE_OUT_SEC);
    return 1.0f;
}
/* Is the opacity MOVING (the loop owes frames)? Between the two fades the
 * line is a still picture and owes none. */
static inline bool fb_status_fading(f64 age, f64 hold) {
    if (age < 0.0 || age >= hold + FB_STATUS_SETTLE_SEC) return false;
    return age < FB_STATUS_FADE_IN_SEC + FB_STATUS_SETTLE_SEC ||
           age > hold - FB_STATUS_FADE_OUT_SEC;
}
/* Seconds until the leave begins, for the loop's wait; 0 = nothing to wait for. */
static inline f64 fb_status_wake_in(f64 age, f64 hold) {
    f64 leave = hold - FB_STATUS_FADE_OUT_SEC;
    if (age < 0.0 || age >= leave) return 0.0;
    return leave - age;
}

#endif /* LIU_UI_FB_MENU_H */
