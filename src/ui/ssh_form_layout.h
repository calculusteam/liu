/*
 * ssh_form_layout.h — the connect form (SSH, SFTP, Telnet, Serial): where
 * every part of it is.
 *
 * ONE geometry for the painter (ui.c), the click and the pointer (main.c), so
 * the hover, the click and the drawing agree in every mode, the Telnet /
 * Serial layout and an empty Recent column included.
 *
 * The form, top to bottom: a header (the mode's tile, a title, one line on
 * what the connection opens), the fields in a column with their labels above
 * them, the SSH options, one line kept for what went wrong, and a footer with
 * the keys and the two buttons — each holding its label and the key that
 * presses it, as the explorer's prompt does. Recent connections are a column
 * to the right of the fields, for SSH and SFTP only (they are SSH history),
 * and only when the window has room for both.
 *
 * Pure: no renderer, no AppState.
 */
#ifndef LIU_UI_SSH_FORM_LAYOUT_H
#define LIU_UI_SSH_FORM_LAYOUT_H

#include "core/types.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SSHF_PAD_PT          24.0f   /* the panel's sides */
#define SSHF_HDR_PT          72.0f   /* tile, title, subtitle */
#define SSHF_TILE_PT         36.0f
#define SSHF_TILE_GAP_PT     14.0f   /* the tile -> the title */
#define SSHF_SUB_GAP_PT       4.0f   /* the title -> its subtitle */
#define SSHF_BODY_TOP_PT     20.0f
#define SSHF_BODY_BOTTOM_PT  16.0f
#define SSHF_FORM_W_PT      408.0f   /* the fields' column */
#define SSHF_RECENT_W_PT    280.0f   /* room for "user · key · just now" beside a row's x */
#define SSHF_FIELD_H_PT      36.0f
#define SSHF_FIELD_RADIUS_PT  9.0f
#define SSHF_FIELD_TEXT_PT   12.0f   /* the text's inset in a field */
#define SSHF_LABEL_GAP_PT     6.0f   /* a label -> its field */
#define SSHF_ROW_GAP_PT      14.0f   /* one row of fields -> the next */
#define SSHF_COL_GAP_PT      12.0f   /* User -> Port */
#define SSHF_PORT_W_PT       96.0f
#define SSHF_OPT_H_PT        20.0f   /* the SSH options row */
#define SSHF_CHECK_PT        16.0f
#define SSHF_CHECK_GAP_PT     8.0f   /* a box -> its words */
#define SSHF_MSG_GAP_PT      12.0f   /* the last row -> the message line */
#define SSHF_FOOTER_PT       60.0f
#define SSHF_BTN_H_PT        32.0f
#define SSHF_BTN_PAD_PT      12.0f   /* a button's label inset */
#define SSHF_BTN_GAP_PT      10.0f
#define SSHF_RECENT_INSET_PT 12.0f   /* the rows' inset in their column */
#define SSHF_RECENT_HEAD_GAP_PT 10.0f
#define SSHF_RECENT_ROW_PT   50.0f
#define SSHF_RECENT_STEP_GAP_PT 6.0f   /* between two bordered rows */
#define SSHF_REMOVE_PT       22.0f   /* a Recent row's x: the box a click takes */
#define SSHF_REMOVE_INSET_PT  8.0f   /* ...from the row's right edge */
#define SSHF_CLEAR_PAD_PT     6.0f   /* the header's Clear: its words' inset */
#define SSHF_RADIUS_PT       12.0f
#define SSHF_MARGIN_PT       16.0f   /* the least room left around the panel */

#define SSHF_OK_LABEL     "Connect"
#define SSHF_OK_KEY       "Enter"
#define SSHF_CANCEL_LABEL "Cancel"
#define SSHF_CANCEL_KEY   "Esc"
#define SSHF_MOSH_LABEL   "Use Mosh"
#define SSHF_X11_LABEL    "X11 Forwarding"
#define SSHF_OPT_KEY_CELLS 3          /* room for "⌥ M" after the words */
#define SSHF_CLEAR_LABEL       "Clear"
#define SSHF_CLEAR_ARMED_LABEL "Clear all?"   /* the second click clears */

typedef enum {
    SSHF_MODE_SSH = 0,
    SSHF_MODE_TELNET,
    SSHF_MODE_SERIAL,
    SSHF_MODE_SFTP,
} SshFormMode;

/* AppState.ssh_dialog_proto (0 SSH, 1 Telnet, 2 Serial) and ssh_dialog_sftp. */
static inline SshFormMode ssh_form_mode(i32 proto, bool sftp) {
    if (proto == 1) return SSHF_MODE_TELNET;
    if (proto == 2) return SSHF_MODE_SERIAL;
    return sftp ? SSHF_MODE_SFTP : SSHF_MODE_SSH;
}

static inline bool ssh_form_mode_logs_in(SshFormMode m) { return m == SSHF_MODE_SSH || m == SSHF_MODE_SFTP; }

/* The fields, in AppState.ssh_field's numbering. */
enum { SSHF_HOST = 0, SSHF_USER = 1, SSHF_PORT = 2, SSHF_PASS = 3, SSHF_FIELDS = 4 };

static inline bool ssh_form_field_shown(SshFormMode m, i32 f) {
    if (f == SSHF_HOST || f == SSHF_PORT) return true;
    return (f == SSHF_USER || f == SSHF_PASS) && ssh_form_mode_logs_in(m);
}

/* Tab's order: Host, User, Port, Password — Host and Port where nothing logs in.
 * `back` is Shift+Tab. */
static inline i32 ssh_form_next_field(SshFormMode m, i32 cur, bool back) {
    i32 f = cur;
    for (i32 i = 0; i < SSHF_FIELDS; i++) {
        f = (f + (back ? SSHF_FIELDS - 1 : 1)) % SSHF_FIELDS;
        if (ssh_form_field_shown(m, f)) return f;
    }
    return SSHF_HOST;
}

typedef struct { f32 x, y, w, h; } SshfRect;

static inline bool sshf_has(SshfRect r, f32 px, f32 py) {
    return r.w > 0.0f && r.h > 0.0f && px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

typedef struct {
    SshFormMode mode;
    f32      dpi;
    SshfRect panel, header, tile, body, footer;
    f32      text_x, title_y, sub_y;      /* the header's two lines */
    SshfRect label[SSHF_FIELDS];          /* a field's caption line */
    SshfRect field[SSHF_FIELDS];          /* zero when the mode has no such field */
    SshfRect opt_mosh, opt_x11;           /* box and words (SSH only) */
    SshfRect msg;                         /* the line for what went wrong */
    SshfRect cancel, connect;
    f32      btn_text_y;
    f32      hint_x, hint_y;              /* the footer's keys, left */
    bool     recent_shown;
    SshfRect rule;                        /* the hairline between the columns */
    SshfRect recent;                      /* the column the rows are inset in */
    f32      recent_head_y;
    SshfRect recent_clear;                /* "Clear" in the column's header line */
    f32      recent_list_y, recent_step, recent_row_h;
    i32      recent_drawn;                /* rows laid out */
    i32      recent_more;                 /* rows that did not fit */
    f32      more_y;                      /* "+N more" text top */
} SshFormGeom;

static inline f32 ssh_form_btn_w(const char *label, const char *key, f32 cw, f32 dpi) {
    return (f32)(strlen(label) + 1 + strlen(key)) * cw + 2.0f * SSHF_BTN_PAD_PT * dpi;
}

static inline SshfRect ssh_form_recent_row(const SshFormGeom *g, i32 i) {
    if (!g->recent_shown || i < 0 || i >= g->recent_drawn) return (SshfRect){ 0, 0, 0, 0 };
    return (SshfRect){ g->recent.x, g->recent_list_y + (f32)i * g->recent_step,
                       g->recent.w, g->recent_row_h };
}

/* The remove button sits at the row's right edge and is vertically centered.
 * The row label ends before the button. */
static inline SshfRect ssh_form_recent_remove(const SshFormGeom *g, i32 i) {
    SshfRect rr = ssh_form_recent_row(g, i);
    if (rr.w <= 0.0f) return rr;
    f32 b = SSHF_REMOVE_PT * g->dpi;
    return (SshfRect){ rr.x + rr.w - SSHF_REMOVE_INSET_PT * g->dpi - b, rr.y + floorf((rr.h - b) * 0.5f), b, b };
}

/* The whole form in a `win_w` x `win_h` window. `history` is the number of
 * recent connections; `lift` raises everything by that many pixels (the open
 * animation — the click is always tested at 0). */
static inline void ssh_form_geom(SshFormGeom *g, f32 win_w, f32 win_h, f32 dpi, f32 cw, f32 ch,
                                 i32 proto, bool sftp, i32 history, f32 lift) {
    memset(g, 0, sizeof *g);
    g->mode = ssh_form_mode(proto, sftp);
    g->dpi = dpi;
    bool logs_in = ssh_form_mode_logs_in(g->mode);
    f32 pad = SSHF_PAD_PT * dpi;
    f32 margin = SSHF_MARGIN_PT * dpi;
    f32 form_w = SSHF_FORM_W_PT * dpi;
    f32 rec_col = SSHF_RECENT_W_PT * dpi;
    f32 rule_w = fmaxf(1.0f, dpi);

    f32 ok_w = ssh_form_btn_w(SSHF_OK_LABEL, SSHF_OK_KEY, cw, dpi);
    f32 cn_w = ssh_form_btn_w(SSHF_CANCEL_LABEL, SSHF_CANCEL_KEY, cw, dpi);
    f32 one = pad + form_w + pad;
    f32 two = one + rule_w + rec_col;
    g->recent_shown = logs_in && history > 0 && two <= win_w - 2.0f * margin;
    f32 w = g->recent_shown ? two : one;
    if (w > win_w - 2.0f * margin) {
        /* A narrow window: the fields' column gives way, down to what the
         * footer's two buttons need beside the keys. */
        f32 least = 2.0f * pad + cn_w + SSHF_BTN_GAP_PT * dpi + ok_w + 16.0f * dpi;
        w = fmaxf(least, win_w - 2.0f * margin);
        form_w = w - 2.0f * pad;
    }

    /* The body's height: its rows, the options, the message line. */
    f32 row = ch + SSHF_LABEL_GAP_PT * dpi + SSHF_FIELD_H_PT * dpi;
    f32 gap = SSHF_ROW_GAP_PT * dpi;
    i32 rows = logs_in ? 3 : 2;
    f32 body_h = SSHF_BODY_TOP_PT * dpi + (f32)rows * row + (f32)(rows - 1) * gap;
    if (g->mode == SSHF_MODE_SSH) body_h += gap + SSHF_OPT_H_PT * dpi;
    body_h += SSHF_MSG_GAP_PT * dpi + ch + SSHF_BODY_BOTTOM_PT * dpi;

    f32 hdr = SSHF_HDR_PT * dpi, foot = SSHF_FOOTER_PT * dpi;
    f32 h = hdr + body_h + foot;
    f32 x = floorf((win_w - w) * 0.5f);
    f32 y = floorf((win_h - h) * 0.5f);           /* the window's middle, both ways */
    if (y < margin) y = margin;
    y -= lift;
    g->panel  = (SshfRect){ x, y, w, h };
    g->header = (SshfRect){ x, y, w, hdr };
    g->body   = (SshfRect){ x, y + hdr, w, body_h };
    g->footer = (SshfRect){ x, y + hdr + body_h, w, foot };

    f32 tile = SSHF_TILE_PT * dpi;
    g->tile = (SshfRect){ x + pad, y + floorf((hdr - tile) * 0.5f), tile, tile };
    g->text_x = g->tile.x + tile + SSHF_TILE_GAP_PT * dpi;
    f32 lines = 2.0f * ch + SSHF_SUB_GAP_PT * dpi;
    g->title_y = y + floorf((hdr - lines) * 0.5f);
    g->sub_y = g->title_y + ch + SSHF_SUB_GAP_PT * dpi;

    /* The fields. */
    f32 fx = x + pad, fy = g->body.y + SSHF_BODY_TOP_PT * dpi;
    f32 fh = SSHF_FIELD_H_PT * dpi, lg = SSHF_LABEL_GAP_PT * dpi;
    f32 port_w = fminf(SSHF_PORT_W_PT * dpi, form_w * 0.4f);
    #define SSHF_PUT(i, fx_, fw_) do {                                            \
        g->label[i] = (SshfRect){ (fx_), fy, (fw_), ch };                         \
        g->field[i] = (SshfRect){ (fx_), fy + ch + lg, (fw_), fh };               \
    } while (0)
    SSHF_PUT(SSHF_HOST, fx, form_w);
    fy += row + gap;
    if (logs_in) {
        f32 uw = form_w - port_w - SSHF_COL_GAP_PT * dpi;
        SSHF_PUT(SSHF_USER, fx, uw);
        SSHF_PUT(SSHF_PORT, fx + form_w - port_w, port_w);
        fy += row + gap;
        SSHF_PUT(SSHF_PASS, fx, form_w);
        fy += row;
    } else {
        SSHF_PUT(SSHF_PORT, fx, port_w);
        fy += row;
    }
    #undef SSHF_PUT

    if (g->mode == SSHF_MODE_SSH) {
        fy += gap;
        f32 oh = SSHF_OPT_H_PT * dpi;
        f32 box = SSHF_CHECK_PT * dpi + SSHF_CHECK_GAP_PT * dpi;
        f32 mw = box + (f32)(strlen(SSHF_MOSH_LABEL) + 1 + SSHF_OPT_KEY_CELLS) * cw;
        f32 xw = box + (f32)(strlen(SSHF_X11_LABEL) + 1 + SSHF_OPT_KEY_CELLS) * cw;
        g->opt_mosh = (SshfRect){ fx, fy, fminf(mw, form_w * 0.5f - 6.0f * dpi), oh };
        f32 x11_x = fx + fmaxf(form_w * 0.5f, mw + 12.0f * dpi);
        g->opt_x11 = (SshfRect){ x11_x, fy, fmaxf(0.0f, fminf(xw, fx + form_w - x11_x)), oh };
        fy += oh;
    }
    fy += SSHF_MSG_GAP_PT * dpi;
    g->msg = (SshfRect){ fx, fy, form_w, ch };

    /* The footer: the keys on the left, Cancel and Connect on the right. */
    f32 bh = SSHF_BTN_H_PT * dpi;
    f32 by = g->footer.y + floorf((foot - bh) * 0.5f);
    g->connect = (SshfRect){ x + w - pad - ok_w, by, ok_w, bh };
    g->cancel  = (SshfRect){ g->connect.x - SSHF_BTN_GAP_PT * dpi - cn_w, by, cn_w, bh };
    g->btn_text_y = by + floorf((bh - ch) * 0.5f);
    g->hint_x = x + pad;
    g->hint_y = g->btn_text_y;

    /* Recent connections: a column of two-line rows beside the fields. */
    if (g->recent_shown) {
        f32 rx = x + pad + form_w + pad;
        g->rule = (SshfRect){ rx, g->body.y, rule_w, body_h };
        f32 inset = SSHF_RECENT_INSET_PT * dpi;
        g->recent = (SshfRect){ rx + rule_w + inset, g->body.y,
                                x + w - inset - (rx + rule_w + inset), body_h };
        g->recent_head_y = g->body.y + SSHF_BODY_TOP_PT * dpi;
        /* Clear, right-aligned on the header line, wide enough for its armed
         * words so the second click lands where the first did. */
        f32 cpad = SSHF_CLEAR_PAD_PT * dpi;
        f32 clw = (f32)strlen(SSHF_CLEAR_ARMED_LABEL) * cw + 2.0f * cpad;
        f32 clh = ch + 8.0f * dpi;
        g->recent_clear = (SshfRect){ g->recent.x + g->recent.w - clw, g->recent_head_y - 4.0f * dpi, clw, clh };
        g->recent_list_y = g->recent_head_y + ch + SSHF_RECENT_HEAD_GAP_PT * dpi;
        g->recent_row_h = SSHF_RECENT_ROW_PT * dpi;
        g->recent_step = (SSHF_RECENT_ROW_PT + SSHF_RECENT_STEP_GAP_PT) * dpi;
        f32 bottom = g->msg.y + g->msg.h;                 /* the rows end with the fields */
        f32 room = bottom - g->recent_list_y + SSHF_RECENT_STEP_GAP_PT * dpi;
        i32 fit = room > 0.0f ? (i32)(room / g->recent_step) : 0;
        if (history > fit) {
            /* Keep a line for "+N more". */
            f32 room2 = room - ch - 4.0f * dpi;
            fit = room2 > 0.0f ? (i32)(room2 / g->recent_step) : 0;
        }
        g->recent_drawn = history < fit ? history : fit;
        g->recent_more = history - g->recent_drawn;
        g->more_y = g->recent_list_y + (f32)g->recent_drawn * g->recent_step + 2.0f * dpi;
    }
}

typedef enum {
    SSHF_HIT_NONE = 0,      /* inside the panel, on nothing that acts */
    SSHF_HIT_OUTSIDE,       /* outside the panel: a click dismisses */
    SSHF_HIT_FIELD,         /* index: the field (its label counts) */
    SSHF_HIT_MOSH,
    SSHF_HIT_X11,
    SSHF_HIT_CANCEL,
    SSHF_HIT_CONNECT,
    SSHF_HIT_RECENT,        /* index: the row */
    SSHF_HIT_RECENT_REMOVE, /* index: the row whose x it is */
    SSHF_HIT_RECENT_CLEAR,  /* the header's Clear */
} SshfHitKind;

typedef struct { SshfHitKind kind; i32 index; } SshfHit;

static inline SshfHit ssh_form_hit(const SshFormGeom *g, f32 px, f32 py) {
    SshfHit h = { SSHF_HIT_NONE, -1 };
    if (!sshf_has(g->panel, px, py)) { h.kind = SSHF_HIT_OUTSIDE; return h; }
    for (i32 f = 0; f < SSHF_FIELDS; f++) {
        if (!ssh_form_field_shown(g->mode, f)) continue;
        if (sshf_has(g->field[f], px, py) || sshf_has(g->label[f], px, py)) {
            h.kind = SSHF_HIT_FIELD; h.index = f; return h;
        }
    }
    if (g->mode == SSHF_MODE_SSH) {
        if (sshf_has(g->opt_mosh, px, py)) { h.kind = SSHF_HIT_MOSH; return h; }
        if (sshf_has(g->opt_x11, px, py))  { h.kind = SSHF_HIT_X11; return h; }
    }
    if (sshf_has(g->connect, px, py)) { h.kind = SSHF_HIT_CONNECT; return h; }
    if (sshf_has(g->cancel, px, py))  { h.kind = SSHF_HIT_CANCEL; return h; }
    if (g->recent_shown && sshf_has(g->recent_clear, px, py)) { h.kind = SSHF_HIT_RECENT_CLEAR; return h; }
    for (i32 i = 0; i < g->recent_drawn; i++) {
        if (!sshf_has(ssh_form_recent_row(g, i), px, py)) continue;
        h.kind = sshf_has(ssh_form_recent_remove(g, i), px, py) ? SSHF_HIT_RECENT_REMOVE : SSHF_HIT_RECENT;
        h.index = i;
        return h;
    }
    return h;
}

/* "just now", "4m ago", "3h ago", "2d ago", "1w ago", "5mo ago": how long ago
 * a recent connection was made. */
static inline void ssh_form_ago(f64 dt, char *out, usize cap) {
    static const struct { f64 below, unit; const char *name; } steps[] = {
        { 3600.0,          60.0,           "m"  },
        { 86400.0,         3600.0,         "h"  },
        { 86400.0 * 7.0,   86400.0,        "d"  },
        { 86400.0 * 30.0,  86400.0 * 7.0,  "w"  },
        { 1e300,           86400.0 * 30.0, "mo" },
    };
    if (!(dt >= 60.0)) { snprintf(out, cap, "just now"); return; }   /* a clock behind: now */
    for (usize i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        if (dt < steps[i].below) {
            i64 v = (i64)(dt / steps[i].unit);
            snprintf(out, cap, "%lld%s ago", (long long)(v < 1 ? 1 : v), steps[i].name);
            return;
        }
    }
}

#endif
