/*
 * Liu — the split layout of a tab, as sessions.json keeps it.
 *
 * A tab can hold up to MAX_SPLIT_PANES (8) panes in an arbitrary binary split
 * tree. Pane 1, pane 2 and ONE split direction + ratio describe two panes and
 * a single split; a tab of four panes, or a 2x2 grid, needs the whole tree.
 *
 * The extension stays FLAT, because older builds read this file with a key
 * scanner that mis-parses a nested object or array: new per-tab keys are
 *
 *     "pane_count": 4, "active_pane": 2,
 *     "layout": "H0.50(0,V0.40(1,2))",
 *     "pane3_type": ..., "pane3_cwd": ..., "pane3_hostname": ... (panes 3-8)
 *
 * and pane 2 keeps its existing pane2_* keys, as does the split/split_ratio
 * pair. An older build reading a four-pane tab therefore still gets the two
 * panes and the root direction it always read.
 *
 * The layout string is
 *     node   := pane | dir ratio '(' node ',' node ')'
 *     pane   := '0'..'7'                       (one digit: panes number 0-7)
 *     dir    := 'H' (side by side) | 'V' (stacked)
 *     ratio  := d '.' d{1,3}                   (0.10 .. 0.90)
 * Every pane 0..pane_count-1 appears exactly once. A file is untrusted input:
 * anything that is not exactly that — a repeated pane, a missing one, a ratio
 * outside the divider's own range, trailing bytes, a tree deeper than the pane
 * count allows — is refused whole, and the caller falls back to the two-pane
 * keys rather than building half a tree.
 *
 * Pure: no ui.h, no allocation, nothing that needs a window.
 */
#ifndef UI_SESSION_LAYOUT_H
#define UI_SESSION_LAYOUT_H

#include "core/types.h"
#include <string.h>
#include <stdio.h>

#define SESSION_LAYOUT_MAX_PANES  8
#define SESSION_LAYOUT_MAX_NODES  (SESSION_LAYOUT_MAX_PANES * 2 - 1)
#define SESSION_LAYOUT_MAX_TEXT   160

typedef struct {
    bool leaf;
    bool horizontal;      /* internal node: side by side (H) vs stacked (V) */
    f32  ratio;           /* internal node: divider position */
    i32  pane;            /* leaf: pane number */
    i32  first, second;   /* internal node: child indices into nodes[] */
} SessionLayoutNode;

typedef struct {
    SessionLayoutNode nodes[SESSION_LAYOUT_MAX_NODES];
    i32 count;
    i32 root;             /* -1 when empty */
    i32 panes;            /* leaves */
} SessionLayout;

/* ---- parsing ------------------------------------------------------------ */

typedef struct {
    const char *s;
    i32 seen_mask;        /* panes already placed */
    i32 panes;            /* pane_count the tree must cover */
} SessionLayoutParse;

static inline i32 session_layout_parse_node(SessionLayoutParse *p, SessionLayout *out,
                                            i32 depth) {
    if (depth > SESSION_LAYOUT_MAX_PANES) return -1;
    if (out->count >= SESSION_LAYOUT_MAX_NODES) return -1;
    char c = *p->s;

    if (c >= '0' && c <= '9') {
        i32 pane = c - '0';
        if (pane >= p->panes || (p->seen_mask & (1 << pane))) return -1;
        p->seen_mask |= 1 << pane;
        p->s++;
        i32 idx = out->count++;
        out->nodes[idx] = (SessionLayoutNode){ .leaf = true, .pane = pane,
                                               .first = -1, .second = -1 };
        return idx;
    }

    if (c != 'H' && c != 'V') return -1;
    bool horizontal = c == 'H';
    p->s++;

    /* ratio: one digit, '.', one to three digits — exact, never strtof on a
     * file's bytes. */
    if (!(p->s[0] >= '0' && p->s[0] <= '9') || p->s[1] != '.') return -1;
    i32 whole = p->s[0] - '0';
    p->s += 2;
    i32 frac = 0, digits = 0;
    while (digits < 3 && *p->s >= '0' && *p->s <= '9') {
        frac = frac * 10 + (*p->s - '0');
        p->s++;
        digits++;
    }
    if (digits == 0) return -1;
    i32 thousandths = whole * 1000 + frac * (digits == 1 ? 100 : digits == 2 ? 10 : 1);
    if (thousandths < 100 || thousandths > 900) return -1;

    if (*p->s != '(') return -1;
    p->s++;
    i32 idx = out->count++;
    out->nodes[idx] = (SessionLayoutNode){ .leaf = false, .horizontal = horizontal,
                                           .ratio = (f32)thousandths / 1000.0f,
                                           .pane = -1, .first = -1, .second = -1 };
    i32 a = session_layout_parse_node(p, out, depth + 1);
    if (a < 0 || *p->s != ',') return -1;
    p->s++;
    i32 b = session_layout_parse_node(p, out, depth + 1);
    if (b < 0 || *p->s != ')') return -1;
    p->s++;
    out->nodes[idx].first = a;
    out->nodes[idx].second = b;
    return idx;
}

/* True when `text` is a well-formed layout covering exactly panes
 * 0..pane_count-1, each once. `out` is only meaningful on true. */
static inline bool session_layout_parse(const char *text, i32 pane_count,
                                        SessionLayout *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    out->root = -1;
    if (!text || pane_count < 2 || pane_count > SESSION_LAYOUT_MAX_PANES) return false;
    if (strlen(text) >= SESSION_LAYOUT_MAX_TEXT) return false;

    SessionLayoutParse p = { .s = text, .seen_mask = 0, .panes = pane_count };
    i32 root = session_layout_parse_node(&p, out, 0);
    if (root < 0 || *p.s != '\0') { memset(out, 0, sizeof *out); out->root = -1; return false; }
    if (p.seen_mask != (1 << pane_count) - 1) {
        memset(out, 0, sizeof *out);
        out->root = -1;
        return false;
    }
    out->root = root;
    out->panes = pane_count;
    return true;
}

/* ---- formatting --------------------------------------------------------- */

static inline bool session_layout_format_node(const SessionLayout *l, i32 node,
                                              char *out, usize cap, usize *len,
                                              i32 depth) {
    if (node < 0 || node >= l->count || depth > SESSION_LAYOUT_MAX_PANES) return false;
    const SessionLayoutNode *n = &l->nodes[node];
    char buf[24];
    i32 w;
    if (n->leaf) {
        if (n->pane < 0 || n->pane >= SESSION_LAYOUT_MAX_PANES) return false;
        w = snprintf(buf, sizeof buf, "%d", n->pane);
    } else {
        i32 thousandths = (i32)(n->ratio * 100.0f + 0.5f) * 10;   /* two places */
        if (thousandths < 100) thousandths = 100;
        if (thousandths > 900) thousandths = 900;
        w = snprintf(buf, sizeof buf, "%c%d.%02d(", n->horizontal ? 'H' : 'V',
                     thousandths / 1000, (thousandths % 1000) / 10);
    }
    if (w <= 0 || *len + (usize)w >= cap) return false;
    memcpy(out + *len, buf, (usize)w);
    *len += (usize)w;
    if (n->leaf) return true;
    if (!session_layout_format_node(l, n->first, out, cap, len, depth + 1)) return false;
    if (*len + 1 >= cap) return false;
    out[(*len)++] = ',';
    if (!session_layout_format_node(l, n->second, out, cap, len, depth + 1)) return false;
    if (*len + 1 >= cap) return false;
    out[(*len)++] = ')';
    return true;
}

/* Writes the layout text into `out` (NUL-terminated); false when it does not
 * fit or the tree is malformed. */
static inline bool session_layout_format(const SessionLayout *l, char *out, usize cap) {
    if (!l || !out || cap == 0 || l->root < 0) return false;
    usize len = 0;
    out[0] = '\0';
    if (!session_layout_format_node(l, l->root, out, cap, &len, 0)) { out[0] = '\0'; return false; }
    out[len] = '\0';
    return true;
}

/* ---- per-pane keys ------------------------------------------------------ */

typedef enum {
    SPK_NONE = 0,
    SPK_TYPE, SPK_CWD, SPK_HOSTNAME, SPK_PORT, SPK_USERNAME, SPK_AUTH, SPK_KEYPATH,
} SessionPaneField;

/* "pane3_hostname" -> pane index 2, field SPK_HOSTNAME. Panes are numbered
 * from 1 in the file and pane 1 is the tab's top-level keys, so only
 * pane2..pane8 exist. Anything else is not a pane key. */
static inline bool session_pane_key(const char *key, i32 *pane, SessionPaneField *field) {
    if (!key || strncmp(key, "pane", 4) != 0) return false;
    if (key[4] < '2' || key[4] > '8' || key[5] != '_') return false;
    i32 idx = key[4] - '1';                      /* '2' -> 1 */
    const char *f = key + 6;
    SessionPaneField fld = SPK_NONE;
    if      (strcmp(f, "type") == 0)        fld = SPK_TYPE;
    else if (strcmp(f, "cwd") == 0)         fld = SPK_CWD;
    else if (strcmp(f, "hostname") == 0)    fld = SPK_HOSTNAME;
    else if (strcmp(f, "port") == 0)        fld = SPK_PORT;
    else if (strcmp(f, "username") == 0)    fld = SPK_USERNAME;
    else if (strcmp(f, "auth_method") == 0) fld = SPK_AUTH;
    else if (strcmp(f, "key_path") == 0)    fld = SPK_KEYPATH;
    else return false;
    if (pane) *pane = idx;
    if (field) *field = fld;
    return true;
}

/* ---- how much a restore may start --------------------------------------- */

/* Panes beyond each tab's first, across the whole file. Every tab keeps its
 * first pane (the tab count is capped separately); this bounds the extra
 * shells and connections a single file can make a launch start. 96 leaves
 * room for a real working set and still keeps a hand-written file from
 * asking for 32 tabs x 7 panes at once. */
#define SESSION_RESTORE_MAX_EXTRA_PANES 96

/* How many panes of a tab that WANTS `wanted` (1-8) to start, given how many
 * extras are left: all of them, else the two-pane form the pane2_* keys
 * express, else just the first. */
static inline i32 session_restore_pane_plan(i32 wanted, i32 extras_left) {
    if (wanted < 1) return 1;
    if (wanted > SESSION_LAYOUT_MAX_PANES) wanted = SESSION_LAYOUT_MAX_PANES;
    if (extras_left < 0) extras_left = 0;
    if (wanted - 1 <= extras_left) return wanted;
    if (wanted > 2 && extras_left >= 1) return 2;
    return 1;
}

#endif /* UI_SESSION_LAYOUT_H */
