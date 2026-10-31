/*
 * Liu — the "start another agent?" prompt: what it shows and where its parts
 * sit.
 *
 * An agent in a pane asked Liu (agentctl) to start another agent in a tab of
 * its own. That is a request from a program, raised under whatever the user is
 * doing, so it follows the trust-dialog rule of ui/hostkey_prompt.h: nothing
 * accepts in its first second, Enter only once the keyboard has been quiet for
 * a second, Escape declines at once. A click outside the panel does nothing —
 * a stray click is not an answer either way.
 *
 * The task is shown as the agent wrote it, through paste_confirm_summarize():
 * control bytes dropped, lines clipped on a grapheme boundary, the rest
 * counted. Pure, no AppState: the renderer (ui.c) and the click handler
 * (main.c) ask the same geometry.
 */
#ifndef UI_AGENTCTL_PROMPT_H
#define UI_AGENTCTL_PROMPT_H

#include "core/types.h"
#include "ui/paste_confirm.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define AGENTCTL_PROMPT_QUEUE 8

/* What the panel says, filled once when it comes up. Every string is clean
 * for drawing: the caller builds them from Liu's own words and from values
 * agentctl has already checked (agent, model, effort), and the folder and
 * the task go through agentctl_prompt_clean() / paste_confirm_summarize(). */
typedef struct {
    u32          job;
    i32          waiting;        /* requests of this pane queued behind this one */
    char         title[64];      /* "Start Codex?" */
    char         who[192];       /* "Claude Code (tab 2) wants to start Codex in a new tab." */
    char         how[160];       /* "gpt-6.1-sol · xhigh · Can write in the folder" */
    char         where[256];     /* "~/project" */
    PasteSummary task;
} AgentctlPromptView;

/* Copies `in` for drawing on one line: control bytes and DEL dropped. */
static inline void agentctl_prompt_clean(const char *in, char *out, usize cap) {
    if (!out || cap == 0) return;
    usize o = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && o + 1 < cap; p++) {
        if (*p < 0x20 || *p == 0x7f) continue;
        out[o++] = (char)*p;
    }
    /* never end inside a UTF-8 sequence */
    while (o > 0 && ((unsigned char)out[o - 1] & 0xC0) == 0x80) {
        usize k = o - 1;
        while (k > 0 && ((unsigned char)out[k] & 0xC0) == 0x80) k--;
        unsigned char lead = (unsigned char)out[k];
        usize need = (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
        if (o - k == need) break;
        o = k;
    }
    out[o] = '\0';
}

/* A folder for the panel: the home directory shown as "~". */
static inline void agentctl_prompt_folder(const char *path, const char *home, char *out, usize cap) {
    char clean[1024];
    agentctl_prompt_clean(path, clean, sizeof clean);
    usize hl = home ? strlen(home) : 0;
    if (hl > 1 && !strncmp(clean, home, hl) && (clean[hl] == '/' || clean[hl] == '\0'))
        snprintf(out, cap, "~%s", clean + hl);
    else
        snprintf(out, cap, "%s", clean);
}

/* ---- geometry: ONE author for the renderer and the click handler ------- */

typedef struct {
    PcRect panel;
    PcRect preview;            /* the sunken box the task sits in */
    PcRect decline, start;     /* buttons */
    PcRect trust;              /* "Allow for this tab": this one, the ones behind it and later ones */
    f32    pad;
    f32    title_y, divider_y, body_y;
    f32    row_h;
} AgentctlPromptGeom;

static inline AgentctlPromptGeom agentctl_prompt_geom(f32 fb_w, f32 fb_h, f32 dpi, i32 preview_rows) {
    AgentctlPromptGeom g;
    if (dpi <= 0.0f) dpi = 1.0f;
    if (preview_rows < 1) preview_rows = 1;
    const f32 ui_ch = 16.0f * dpi;
    g.pad   = 22.0f * dpi;
    g.row_h = ui_ch;
    f32 w = 580.0f * dpi;
    if (w > fb_w - 48.0f * dpi) w = fb_w - 48.0f * dpi;
    if (w < 220.0f * dpi) w = 220.0f * dpi;
    f32 y = 0.0f;                                  /* panel-relative first */
    g.title_y   = y + g.pad;
    g.divider_y = g.title_y + ui_ch + 12.0f * dpi;
    g.body_y    = g.divider_y + 18.0f * dpi;
    /* three body lines: who, how, where */
    f32 prev_y  = g.body_y + 3.0f * ui_ch + 2.0f * 6.0f * dpi + 14.0f * dpi;
    f32 prev_h  = (f32)preview_rows * ui_ch + 20.0f * dpi;
    f32 btn_h   = 34.0f * dpi;
    f32 btn_y   = prev_y + prev_h + 18.0f * dpi;
    f32 h       = btn_y + btn_h + g.pad;
    f32 px = (fb_w - w) * 0.5f;
    f32 py = (fb_h - h) * 0.5f;
    if (py < 8.0f * dpi) py = 8.0f * dpi;
    g.panel   = (PcRect){ px, py, w, h };
    g.title_y += py; g.divider_y += py; g.body_y += py;
    g.preview = (PcRect){ px + g.pad, py + prev_y, w - 2.0f * g.pad, prev_h };
    f32 bw_start = 120.0f * dpi, bw_decline = 104.0f * dpi, bw_trust = 208.0f * dpi;
    f32 start_x = px + w - g.pad - bw_start;
    g.start   = (PcRect){ start_x, py + btn_y, bw_start, btn_h };
    g.decline = (PcRect){ start_x - 12.0f * dpi - bw_decline, py + btn_y, bw_decline, btn_h };
    /* the trust button on the left, away from Start, so a hurried click is
     * not a standing permission */
    g.trust   = (PcRect){ px + g.pad, py + btn_y, bw_trust, btn_h };
    if (g.trust.x + g.trust.w + 12.0f * dpi > g.decline.x) g.trust.w = 0.0f;
    return g;
}

typedef enum {
    AGENTCTL_PROMPT_HIT_NONE = 0,
    AGENTCTL_PROMPT_HIT_START,
    AGENTCTL_PROMPT_HIT_DECLINE,
    AGENTCTL_PROMPT_HIT_TRUST,
} AgentctlPromptHit;

static inline AgentctlPromptHit agentctl_prompt_hit(const AgentctlPromptGeom *g, f32 x, f32 y) {
    if (!g) return AGENTCTL_PROMPT_HIT_NONE;
    if (pc_rect_hit(g->start, x, y))   return AGENTCTL_PROMPT_HIT_START;
    if (pc_rect_hit(g->decline, x, y)) return AGENTCTL_PROMPT_HIT_DECLINE;
    if (g->trust.w > 0.0f && pc_rect_hit(g->trust, x, y)) return AGENTCTL_PROMPT_HIT_TRUST;
    return AGENTCTL_PROMPT_HIT_NONE;
}

/* The trust button's word: how many requests it lets through now. */
static inline void agentctl_prompt_trust_label(i32 waiting, char *out, usize cap) {
    if (waiting > 0) snprintf(out, cap, "Allow all %d for this tab", waiting + 1);
    else             snprintf(out, cap, "Allow for this tab");
}

#endif /* UI_AGENTCTL_PROMPT_H */
