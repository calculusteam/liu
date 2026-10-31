/*
 * Liu — where a delegated agent's tab and its log sit, and what they say.
 *
 * Each job an agent started (agentctl) is a small tab in the strip, joined to
 * the right of the tab whose pane asked for it: a quarter of a tab's width,
 * the agent's logo and nothing else. Its state is the tab's BACKGROUND, edge
 * to edge: a dithered haze (agentctl_haze_field) tinted by the state,
 * drifting while a turn runs and still otherwise. The open tab's fill and
 * outline stop at its own edge; a job tab keeps the same look either way.
 * From AGC_FOLD_MIN jobs a count chip leads them, and a click on it folds
 * them into it; past AGC_TAB_JOBS_MAX the first one counts the rest. The owning tab's slot
 * is widened by its job tabs, so the strip's scroll, drag and hit test move
 * them with it. A pointer resting on one shows the inside of a small
 * terminal under the strip with the job's newest lines; a click on the job
 * tab or on that hover opens the job's chat, a panel over the window with
 * the whole log, a field for the next message and its buttons.
 *
 * Only a working job tab is drawn on a clock, at AGC_HAZE_FPS; every other
 * state is still. The chat shows time in whole minutes, so it owes a repaint
 * only at a minute edge (agentctl_card_next_edge_ms).
 *
 * Pure: geometry and text, no AppState. The strip publishes each tab's job
 * width and the jobs it shows; the painter and the hit test read those.
 */
#ifndef UI_AGENTCTL_CARD_H
#define UI_AGENTCTL_CARD_H

#include "core/types.h"
#include "core/utf8.h"
#include "agentctl/agentctl_adapter.h"   /* AGJ_*, AGO_*, AGE_* */
#include "ui/paste_confirm.h"            /* PcRect, pc_rect_hit */
#include "ui/layout.h"                   /* the window's strip, pill and status bar */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define AGC_TAB_JOBS_MAX     6      /* job tabs beside one tab; the first counts the rest */
#define AGC_TAB_FRACTION     0.25f  /* of the strip's tab width */
#define AGC_TAB_PAD_PT       6.0f   /* logo to the job tab's sides, at the narrowest */
#define AGC_TAB_ICON_PT      18.0f  /* the logo: larger than a tab's own agent icon,
                                     * since it is all the job tab carries */
/* The hover is the inside of a small terminal: the job's newest lines in the
 * theme's terminal colours, wrapped to its columns, the newest at the bottom,
 * with a cursor after them. The pointer can cross into it, and a click on it
 * opens the job's chat. */
#define AGC_TIP_GAP_PT         4.0f   /* strip's bottom to the hover */
#define AGC_TIP_COLS           42     /* the body's columns at the UI cell */
#define AGC_TIP_MIN_COLS       16     /* narrower than this, no hover */
#define AGC_TIP_ROWS           6      /* the body's rows, the cursor's among them */
#define AGC_TIP_PAD_X_PT       12.0f
#define AGC_TIP_PAD_Y_PT       10.0f
#define AGC_TIP_ROW_GAP_PT     2.0f
#define AGC_TIP_RADIUS_PT      9.0f
#define AGC_CARD_ICON_PT       14.0f
/* The hover comes and goes the way the strip's own tab panel does: it rises
 * AGC_TIP_RISE_PT into place as it fades in, sinks part of the way back as it
 * fades out, and glides from one job tab to the next without closing. */
#define AGC_TIP_OPEN_S       0.22
#define AGC_TIP_CLOSE_S      0.15
#define AGC_TIP_RISE_PT      6.0f
#define AGC_TIP_GLIDE_RATE   16.0f  /* of the distance left, per second */

static inline f32 agc_clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/* The hover's alpha and how far it has risen (0..1), `t` seconds into
 * opening or into closing. */
static inline void agentctl_tip_envelope(bool opening, f64 t, f32 *alpha, f32 *rise) {
    if (opening) {
        f32 u = agc_clamp01((f32)(t / AGC_TIP_OPEN_S));
        f32 v = 1.0f - u;
        f32 e = 1.0f - v * v * v;                       /* ease out */
        *alpha = e;
        *rise = e;
    } else {
        f32 u = agc_clamp01((f32)(t / AGC_TIP_CLOSE_S));
        f32 inv = 1.0f - u;
        f32 io = u < 0.5f ? 4.0f * u * u * u
                          : 1.0f - (2.0f - 2.0f * u) * (2.0f - 2.0f * u) * (2.0f - 2.0f * u) * 0.5f;
        *alpha = inv * inv;                             /* no late snap */
        *rise = 1.0f - io * 0.45f;
    }
}

/* How far into opening (or closing) the hover already is when it turns at
 * `alpha`, so turning back mid-way carries on from where it stands instead of
 * starting over. */
static inline f64 agentctl_tip_turn_at(bool opening, f32 alpha) {
    alpha = agc_clamp01(alpha);
    if (opening) return (1.0 - cbrt(1.0 - (f64)alpha)) * AGC_TIP_OPEN_S;
    return (1.0 - sqrt((f64)alpha)) * AGC_TIP_CLOSE_S;
}

/* One job as the strip shows it. */
typedef struct {
    u32 job;
    i32 agent;
    u8  state, outcome, end;
    i64 turn_start_ms, turn_end_ms;   /* wall clock; 0 = none yet */
    bool asking;                      /* waits for the user to allow a tool */
    u16 chip;                         /* > 0: this slot is the count chip that
                                       * leads the tab's job tabs, for this many
                                       * jobs; the fields above are its lead's */
    u32 look;                         /* the chip's haze: its own picture */
} AgcTabJob;

/* ---- the count chip that folds a tab's job tabs ---------------------------
 *
 * A tab with AGC_FOLD_MIN jobs or more has a chip in front of its job tabs:
 * its lead job's haze and logo and the number of jobs. A click on it folds
 * the job tabs into it, and another opens them again, as a tab group's chip
 * does. Folded, the chip is the tab's only job tab. Where nobody chose for
 * the tab, the setting decides: AUTO folds once the jobs would not all have
 * a job tab of their own (past AGC_TAB_JOBS_MAX), ALWAYS folds from the
 * first chip, NEVER leaves them open. */
#define AGC_FOLD_MIN          3      /* jobs beside one tab from which a chip leads them */
#define AGC_CHIP_LOOK_BIT     0x80000000u   /* job ids never reach it */

typedef enum { AGC_FOLD_AUTO = 0, AGC_FOLD_ALWAYS, AGC_FOLD_NEVER } AgcFoldPolicy;
typedef enum { AGC_TAB_FOLD_UNSET = 0, AGC_TAB_FOLDED, AGC_TAB_UNFOLDED } AgcTabFold;

static inline bool agentctl_jobs_chip(i32 total) { return total >= AGC_FOLD_MIN; }

static inline bool agentctl_jobs_folded(i32 total, u8 policy, u8 tab_choice) {
    if (!agentctl_jobs_chip(total)) return false;
    if (tab_choice == AGC_TAB_FOLDED) return true;
    if (tab_choice == AGC_TAB_UNFOLDED) return false;
    if (policy == AGC_FOLD_ALWAYS) return true;
    if (policy == AGC_FOLD_NEVER) return false;
    return total > AGC_TAB_JOBS_MAX;
}

/* How many of `total` jobs have a job tab of their own when the tab's slots
 * are AGC_TAB_JOBS_MAX, one of them taken by a chip when `chip`. */
static inline i32 agentctl_job_tabs_shown_in(i32 total, bool chip) {
    i32 cap = chip ? AGC_TAB_JOBS_MAX - 1 : AGC_TAB_JOBS_MAX;
    if (total <= 0) return 0;
    return total > cap ? cap : total;
}

/* The jobs the first shown job tab stands for beyond itself ("+N"), as
 * agentctl_job_tabs_more() with a chip taking a slot. */
static inline i32 agentctl_job_tabs_more_in(i32 total, bool chip) {
    i32 cap = chip ? AGC_TAB_JOBS_MAX - 1 : AGC_TAB_JOBS_MAX;
    return total > cap ? total - cap + 1 : 0;
}

/* How many of a tab's `n` job slots a docked row draws, `w` each, in the
 * `room` beside its pill that must keep `keep` for itself: all of them, else
 * the chip alone when the first slot is one, else none. */
static inline i32 agentctl_job_slots_fit(i32 n, bool first_is_chip, f32 room, f32 w, f32 keep) {
    if (n <= 0) return 0;
    if (room - (f32)n * w >= keep) return n;
    if (first_is_chip && room - w >= keep) return 1;
    return 0;
}

/* The chip's look: its own place in the noise, from the first of the tab's
 * jobs, so it stays the same picture while its lead changes. */
static inline u32 agentctl_job_chip_look(u32 first_job) { return first_job | AGC_CHIP_LOOK_BIT; }

/* Where a chip `w` wide puts its logo and its number (`digits` characters of
 * `cw` each): the logo and the number side by side, `gap` apart, centred;
 * the number alone, centred, when both do not fit. */
typedef struct {
    bool logo;
    f32  icon_x, text_x;
} AgcChipPlace;

static inline AgcChipPlace agentctl_job_chip_place(f32 x, f32 w, f32 icon, f32 cw, i32 digits,
                                                   f32 gap, f32 pad) {
    AgcChipPlace p = { false, 0.0f, 0.0f };
    if (digits < 1) digits = 1;
    f32 tw = (f32)digits * cw;
    f32 both = icon + gap + tw;
    if (both + 2.0f * pad <= w) {
        p.logo = true;
        p.icon_x = floorf(x + (w - both) * 0.5f + 0.5f);
        p.text_x = p.icon_x + icon + gap;
    } else {
        p.text_x = floorf(x + (w - tw) * 0.5f + 0.5f);
    }
    return p;
}

/* One job tab's width: a quarter of the strip's tab width, never narrower
 * than the logo with its padding, in whole pixels. */
static inline f32 agentctl_job_tab_w(f32 tab_w, f32 icon_px, f32 dpi) {
    if (dpi <= 0.0f) dpi = 1.0f;
    f32 w = floorf(tab_w * AGC_TAB_FRACTION + 0.5f);
    f32 lo = ceilf(icon_px + 2.0f * AGC_TAB_PAD_PT * dpi);
    return w < lo ? lo : w;
}

/* How many job tabs `total` jobs take: all of them up to AGC_TAB_JOBS_MAX,
 * past it AGC_TAB_JOBS_MAX with the last counting the rest. */
static inline i32 agentctl_job_tabs_shown(i32 total) {
    if (total <= 0) return 0;
    return total > AGC_TAB_JOBS_MAX ? AGC_TAB_JOBS_MAX : total;
}

/* The jobs the last shown tab stands for beyond itself ("+N" counts N+1
 * jobs: itself and the hidden ones); 0 when every job has its own tab. */
static inline i32 agentctl_job_tabs_more(i32 total) {
    return total > AGC_TAB_JOBS_MAX ? total - AGC_TAB_JOBS_MAX + 1 : 0;
}

/* Which of `n` job tabs, each `w` wide from `x0`, holds `x`; -1 for none. */
static inline i32 agentctl_job_tab_at(f32 x, f32 x0, f32 w, i32 n) {
    if (n <= 0 || w <= 0.0f || x < x0) return -1;
    i32 k = (i32)((x - x0) / w);
    return k < n ? k : -1;
}

/* What a job tab's background says. */
typedef enum {
    AGC_CUE_NONE = 0,     /* stopped, taken over: no haze */
    AGC_CUE_WORKING,      /* a turn is running: the accent, drifting */
    AGC_CUE_ASKING,       /* waiting for the user's answer: the accent, faint, still */
    AGC_CUE_WAITING,      /* queued or starting: the quiet colour, faint, still */
    AGC_CUE_DONE,         /* the last turn ended well: green, still */
    AGC_CUE_FAILED,       /* the last turn failed, or the job ended on its own: red, still */
} AgcCue;

/* `state`, `outcome`, `end` are AgentctlJobState / Outcome / End. */
static inline AgcCue agentctl_job_cue(u8 state, u8 outcome, u8 end) {
    switch (state) {
    case AGJ_RUNNING:          return AGC_CUE_WORKING;
    case AGJ_PENDING_APPROVAL: return AGC_CUE_ASKING;
    case AGJ_QUEUED:
    case AGJ_STARTING:         return AGC_CUE_WAITING;
    case AGJ_IDLE:
        if (outcome == AGO_DONE)    return AGC_CUE_DONE;
        if (outcome == AGO_STOPPED) return AGC_CUE_NONE;
        return AGC_CUE_FAILED;
    case AGJ_ENDED:
        if (end == AGE_MERGED) return AGC_CUE_DONE;
        return (end == AGE_TAKEN_OVER || end == AGE_CLOSED || end == AGE_DISCARDED) ? AGC_CUE_NONE : AGC_CUE_FAILED;
    default:                   return AGC_CUE_NONE;
    }
}

/* The same, for a job that may be waiting for the user to allow a tool:
 * then it reads as asking, whatever its turn is doing. */
static inline AgcCue agentctl_job_cue_ex(u8 state, u8 outcome, u8 end, bool asking) {
    if (asking && state == AGJ_RUNNING) return AGC_CUE_ASKING;
    return agentctl_job_cue(state, outcome, end);
}

/* The job a chip shows of `n` (oldest first): the newest at work, else the
 * newest asking, else the newest waiting to start, else the newest. */
static inline i32 agentctl_jobs_lead(const AgcTabJob *jobs, i32 n) {
    static const AgcCue order[] = { AGC_CUE_WORKING, AGC_CUE_ASKING, AGC_CUE_WAITING };
    if (n <= 0) return -1;
    for (i32 o = 0; o < (i32)(sizeof order / sizeof order[0]); o++)
        for (i32 i = n - 1; i >= 0; i--)
            if (agentctl_job_cue_ex(jobs[i].state, jobs[i].outcome, jobs[i].end, jobs[i].asking) == order[o])
                return i;
    return n - 1;
}

/* ---- the job tab's background --------------------------------------------
 *
 * The site's background shader, with the job tab as its viewport: an fbm
 * field, domain-warped, lit by two glows, quantised to six tones with an 8x8
 * ordered (Bayer) dither. Each job has a look of its own (AgcHazeLook, from
 * its id): its place in the noise, where its two lights rest and a small
 * turn of its hue, so job tabs side by side are separate pictures, not one
 * picture repeated. What moves is shared: every running job tab reads the
 * same clock (agentctl_haze_run_ms), drifts the site's way at one speed and
 * turns its lights on one circle at one pace (AGC_HAZE_ORBIT,
 * AGC_HAZE_TURN), so several move as one, in step. A frame's values are stretched between the field's own darkest
 * and brightest (AGC_HAZE_DARK, AGC_HAZE_BRIGHT) and curved to one mean
 * light (AGC_HAZE_MEAN): every job tab has the same depth, a dithered floor
 * in its dark part and the top tones in its light, whichever part of the
 * noise it shows. It runs AGC_HAZE_SPEED times as fast
 * as the site's, since at the site's pace a tab this size reads as still,
 * and its ramp climbs higher (AGC_HAZE_TOP); the ramp's colour is the agent's
 * brand colour while a turn runs (agentctl_haze_branded, agentctl_haze_brand)
 * and the outcome's once it ended, its height the state's
 * (agentctl_haze_strength). The field is worked out at AGC_HAZE_FPS key
 * frames, and between two keys each cell's colour moves from the one to the
 * next (agentctl_haze_frac), drawn at AGC_HAZE_DRAW_HZ: a cell that changes
 * tone flows into it instead of stepping. A change of state cross-fades over
 * AGC_HAZE_FADE_S, and a new job tab's haze fades in. One tone per square
 * cell of AGC_HAZE_CELL_PT, laid from the tab's top-left corner and cut at
 * its edges, so the field covers the whole tab. Tone 0 is the surface under
 * the tab. Pure and deterministic: the painter draws one square per
 * non-zero cell. */
#define AGC_HAZE_CELL_PT   2.0f
#define AGC_HAZE_T0        40.0f   /* a field time with good tone coverage */
#define AGC_HAZE_SPEED     4.0f    /* field seconds per second of a running turn (site: 0.5) */
#define AGC_HAZE_FPS       12      /* key frames a second while a turn runs */
#define AGC_HAZE_DRAW_HZ   60      /* frames a second while cells flow between two keys */
/* Tone 5: the surface moved AGC_HAZE_TOP of the way to the haze's colour
 * with each channel squared (a deeper shade of it); for the Liu theme's
 * accent over its background that is (62,45,147), well above the site's top
 * tone #211A41 (0.2213 would be that) so it reads at a tab's size. A brand
 * colour's is agentctl_haze_deep_rgb's: (154,70,28) for Claude Code's orange.
 * Tones are mixed in sRGB, as the site's shader mixes them. */
#define AGC_HAZE_TOP       0.56f
#define AGC_HAZE_FADE_S    0.6     /* a change of state, and a new job tab, cross-fade over this */
#define AGC_HAZE_DARK      0.25f   /* this share of a frame's cells sits on the floor */
#define AGC_HAZE_BRIGHT    0.98f   /* ...and the cells above this share reach the top */
#define AGC_HAZE_FLOOR     0.08f   /* the floor: a sparse dither of the first tone */
#define AGC_HAZE_MEAN      0.48f   /* a frame's mean light between floor and top */
#define AGC_HAZE_REACH     4.6f    /* tones a full-strength field climbs before the dither */
#define AGC_HAZE_HUE       0.12f   /* radians a job's hue may turn either way: a brand colour stays itself */

static inline f32 agc_fract(f32 x) { return x - floorf(x); }

static inline f32 agc_hash(f32 x, f32 y) {
    x = agc_fract(x * 123.34f);
    y = agc_fract(y * 456.21f);
    f32 d = x * (x + 45.32f) + y * (y + 45.32f);
    x += d;
    y += d;
    return agc_fract(x * y);
}

static inline f32 agc_vnoise(f32 x, f32 y) {
    f32 ix = floorf(x), iy = floorf(y);
    f32 fx = x - ix, fy = y - iy;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    f32 a = agc_hash(ix, iy), b = agc_hash(ix + 1.0f, iy);
    f32 c = agc_hash(ix, iy + 1.0f), d = agc_hash(ix + 1.0f, iy + 1.0f);
    f32 top = a + (b - a) * fx, bot = c + (d - c) * fx;
    return top + (bot - top) * fy;
}

static inline f32 agc_fbm(f32 x, f32 y) {
    f32 s = 0.0f, a = 0.5f;
    for (int i = 0; i < 4; i++) {
        s += a * agc_vnoise(x, y);
        x = x * 2.03f + 17.1f;
        y = y * 2.03f + 17.1f;
        a *= 0.5f;
    }
    return s;
}

static inline f32 agc_bayer2(f32 x, f32 y) {
    x = floorf(x);
    y = floorf(y);
    return agc_fract(x * 0.5f + y * y * 0.75f);
}
static inline f32 agc_bayer4(f32 x, f32 y) { return agc_bayer2(0.5f * x, 0.5f * y) * 0.25f + agc_bayer2(x, y); }
/* The 8x8 threshold of cell (x, y): 64 distinct values in [0, 1). */
static inline f32 agc_bayer8(f32 x, f32 y) { return agc_bayer4(0.5f * x, 0.5f * y) * 0.25f + agc_bayer2(x, y); }

static inline f32 agc_blob(f32 px, f32 py, f32 cx, f32 cy, f32 sx, f32 sy, f32 k) {
    f32 dx = (px - cx) * sx, dy = (py - cy) * sy;
    return expf(-(dx * dx + dy * dy) * k);
}

/* How far up the tone ramp a state's field may climb (0 = no haze): the
 * site's own two values for the quieter states. */
static inline f32 agentctl_haze_strength(AgcCue cue) {
    switch (cue) {
        case AGC_CUE_WORKING: return 1.0f;
        case AGC_CUE_DONE:    return 0.8f;
        case AGC_CUE_FAILED:  return 0.8f;
        case AGC_CUE_ASKING:  return 0.6f;
        case AGC_CUE_WAITING: return 0.35f;
        default:              return 0.0f;
    }
}

/* Whether a state's haze drifts (and so asks for frames). */
static inline bool agentctl_haze_moves(AgcCue cue) { return cue == AGC_CUE_WORKING; }

/* Whether a state's haze takes the agent's brand colour: while a turn runs,
 * waits for an answer or has yet to start. A turn that ended takes the
 * outcome's colour (green, red). */
static inline bool agentctl_haze_branded(AgcCue cue) {
    return cue == AGC_CUE_WORKING || cue == AGC_CUE_ASKING || cue == AGC_CUE_WAITING;
}

static inline f32 agc_luma(const f32 c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

/* Tone 5 of a haze over `under` toward `tint`: the surface moved
 * AGC_HAZE_TOP of the way to the tint with each channel squared (a deeper
 * shade of it). A brand colour is raised to AGC_HAZE_BRAND_POW instead and
 * moved AGC_HAZE_BRAND_TOP of the way: squaring turns an orange red, and a
 * dark orange then reads as rust rather than as the brand. */
#define AGC_HAZE_BRAND_POW 1.5f
#define AGC_HAZE_BRAND_TOP 0.64f
static inline void agentctl_haze_deep_rgb(const f32 under[3], const f32 tint[3], bool branded, f32 out[3]) {
    f32 top = branded ? AGC_HAZE_BRAND_TOP : AGC_HAZE_TOP;
    for (i32 i = 0; i < 3; i++) {
        f32 c = agc_clamp01(tint[i]);
        c = branded ? powf(c, AGC_HAZE_BRAND_POW) : c * c;
        out[i] = under[i] + (c - under[i]) * top;
    }
}

/* A brand colour as a job tab's haze takes it over a surface of luma
 * `under_luma`: kept as it is when its luma is at least AGC_HAZE_BRAND_GAP
 * from the surface's, else moved there with its hue kept: over a dark
 * surface its channels are scaled up together and, once the brightest
 * reaches 1, lifted toward white; over a light one scaled down together. A
 * brand of no hue (a black or a white mark) becomes a grey of that luma, so
 * every agent's haze reads as clearly as a coloured one's. In 0..1. */
#define AGC_HAZE_BRAND_GAP 0.50f
static inline void agentctl_haze_brand(f32 rgb[3], f32 under_luma) {
    for (i32 i = 0; i < 3; i++) rgb[i] = agc_clamp01(rgb[i]);
    f32 l = agc_luma(rgb);
    if (under_luma < 0.5f) {
        f32 want = fminf(1.0f, under_luma + AGC_HAZE_BRAND_GAP);
        if (l >= want) return;
        if (l < 1e-4f) {
            rgb[0] = rgb[1] = rgb[2] = want;
            return;
        }
        f32 k = want / l, mx = 0.0f;
        for (i32 i = 0; i < 3; i++) {
            rgb[i] *= k;
            if (rgb[i] > mx) mx = rgb[i];
        }
        if (mx > 1.0f) {
            for (i32 i = 0; i < 3; i++) rgb[i] /= mx;
            f32 have = agc_luma(rgb);
            f32 m = have < 1.0f ? agc_clamp01((want - have) / (1.0f - have)) : 0.0f;
            for (i32 i = 0; i < 3; i++) rgb[i] += (1.0f - rgb[i]) * m;
        }
    } else {
        f32 want = fmaxf(0.0f, under_luma - AGC_HAZE_BRAND_GAP);
        if (l <= want) return;
        f32 k = want / l;
        for (i32 i = 0; i < 3; i++) rgb[i] *= k;
    }
    for (i32 i = 0; i < 3; i++) rgb[i] = agc_clamp01(rgb[i]);
}

/* The clock a job's field is at, in ms, for a job whose turn started at
 * `start_ms` and ended at `end_ms` (wall clock, 0 = not yet): the wall clock
 * itself while the turn runs, so every running job tab of every window is at
 * the same field time and they change frame together; held at the moment
 * the turn ended once it did; 0 before a turn or for one that never ended. */
static inline i64 agentctl_haze_run_ms(AgcCue cue, i64 start_ms, i64 end_ms, i64 now_ms) {
    if (start_ms <= 0) return 0;
    if (agentctl_haze_moves(cue)) return now_ms > 0 ? now_ms : 0;
    return end_ms >= start_ms ? end_ms : 0;
}

/* The field time at clock `run_ms`, in whole frames, so every window draws
 * the same picture between two frame edges. It starts over every
 * AGC_HAZE_WRAP field seconds (about 68 minutes of clock): past that the
 * noise's lattice coordinates grow too large for its hash in f32. */
#define AGC_HAZE_WRAP 16384.0
static inline f32 agentctl_haze_time(i64 run_ms) {
    if (run_ms < 0) run_ms = 0;
    i64 frame = run_ms * AGC_HAZE_FPS / 1000;
    f64 t = fmod((f64)frame / (f64)AGC_HAZE_FPS * (f64)AGC_HAZE_SPEED, AGC_HAZE_WRAP);
    return AGC_HAZE_T0 + (f32)t;
}

/* The first run time (ms) of key frame `k`. */
static inline i64 agentctl_haze_key_ms(i64 k) {
    if (k <= 0) return 0;
    return (k * 1000 + AGC_HAZE_FPS - 1) / AGC_HAZE_FPS;
}

/* How far `run_ms` is from its key frame to the next: 0..1. */
static inline f32 agentctl_haze_frac(i64 run_ms) {
    if (run_ms < 0) run_ms = 0;
    i64 k = run_ms * AGC_HAZE_FPS / 1000;
    i64 a = agentctl_haze_key_ms(k), b = agentctl_haze_key_ms(k + 1);
    return b > a ? (f32)(run_ms - a) / (f32)(b - a) : 0.0f;
}

/* Milliseconds from `run_ms` to the next key frame: 1..1000/FPS+1. */
static inline i64 agentctl_haze_next_ms(i64 run_ms) {
    if (run_ms < 0) run_ms = 0;
    i64 frame = run_ms * AGC_HAZE_FPS / 1000;
    i64 next = ((frame + 1) * 1000 + AGC_HAZE_FPS - 1) / AGC_HAZE_FPS;
    return next > run_ms ? next - run_ms : 1;
}

/* A job's own haze (see above). A pure function of the job's id, so every
 * window draws the same picture for it. */
typedef struct {
    f32 ox, oy;          /* the field's place in the noise */
    f32 gx[2], gy[2];    /* where its two lights rest, in the tab's units */
    f32 gk[2];           /* how tight each light is */
    f32 hue;             /* radians the haze's colour is turned by */
} AgcHazeLook;

/* Every light wanders on a circle of AGC_HAZE_ORBIT about its rest, turning
 * AGC_HAZE_TURN radians a field second, the second light half a turn behind
 * the first: the same in every job tab, so their lights move together. */
#define AGC_HAZE_ORBIT 0.14f
#define AGC_HAZE_TURN  0.075f

/* Where light `i` of look `L` stands at field time `tt`, in the tab's units
 * (x already scaled by the aspect `asp`). */
static inline void agentctl_haze_light(const AgcHazeLook *L, i32 i, f32 tt, f32 asp, f32 *x, f32 *y) {
    f32 ang = AGC_HAZE_TURN * tt + (i ? 3.1415927f : 0.0f);
    *x = (L->gx[i] + AGC_HAZE_ORBIT * cosf(ang)) * asp;
    *y = L->gy[i] + AGC_HAZE_ORBIT * sinf(ang);
}

static inline u32 agc_mix(u32 x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* The next of a job's numbers, 0..1. */
static inline f32 agc_unit(u32 *s) {
    *s = agc_mix(*s + 0x9e3779b9u);
    return (f32)(*s >> 8) / 16777216.0f;
}

static inline AgcHazeLook agentctl_haze_look(u32 job) {
    AgcHazeLook L;
    u32 s = agc_mix(job * 0x27d4eb2fu + 0x165667b1u);
    L.ox = agc_unit(&s) * 48.0f;
    L.oy = agc_unit(&s) * 48.0f;
    /* one light in each half, one high and one low, either way round */
    f32 side = agc_unit(&s) < 0.5f ? -1.0f : 1.0f;
    L.gx[0] = side * (0.08f + 0.36f * agc_unit(&s));
    L.gy[0] = 0.06f + 0.34f * agc_unit(&s);
    L.gx[1] = -side * (0.08f + 0.36f * agc_unit(&s));
    L.gy[1] = -(0.06f + 0.34f * agc_unit(&s));
    if (agc_unit(&s) < 0.5f) { L.gy[0] = -L.gy[0]; L.gy[1] = -L.gy[1]; }
    L.gk[0] = 1.3f + 0.8f * agc_unit(&s);
    L.gk[1] = 1.5f + 0.8f * agc_unit(&s);
    L.hue = AGC_HAZE_HUE * (2.0f * agc_unit(&s) - 1.0f);
    return L;
}

/* `rgb` turned `a` radians about the grey axis, kept in 0..1. */
static inline void agentctl_hue_turn(f32 rgb[3], f32 a) {
    f32 c = cosf(a), o = (1.0f - c) / 3.0f, q = 0.57735027f * sinf(a);
    f32 r = rgb[0] * (c + o) + rgb[1] * (o - q) + rgb[2] * (o + q);
    f32 g = rgb[0] * (o + q) + rgb[1] * (c + o) + rgb[2] * (o - q);
    f32 b = rgb[0] * (o - q) + rgb[1] * (o + q) + rgb[2] * (c + o);
    rgb[0] = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
    rgb[1] = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
    rgb[2] = b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
}

/* The field drifts slowly: most frames repeat the one before, and a repeat
 * owes no repaint. A field of more cells than this is repainted every frame. */
#define AGC_HAZE_CELLS_MAX 2048
#define AGC_HAZE_LOOKAHEAD AGC_HAZE_FPS   /* frames looked at past the one drawn */
#define AGC_HAZE_BINS      256            /* the stretch's histogram */

/* Tones 0..5 of a `cols` x `rows` field, row by row from the top-left, at
 * field time `t` for look `L` and strength `k` (agentctl_haze_strength).
 * Rows run upward in the field, as the shader's fragment coordinates do.
 * All zero for no strength or a field of more than AGC_HAZE_CELLS_MAX. */
static inline void agentctl_haze_field(u8 *out, i32 cols, i32 rows, f32 t, const AgcHazeLook *L, f32 k) {
    if (!out || cols <= 0 || rows <= 0) return;
    i32 n = cols * rows;
    if (n > AGC_HAZE_CELLS_MAX || k <= 0.0f || !L) {
        if (n <= AGC_HAZE_CELLS_MAX) memset(out, 0, (usize)n);
        return;
    }
    f32 asp = (f32)cols / (f32)rows;
    f32 tt = t;
    /* the site's three drifts, the same for every job */
    const f32 ay = 0.05f, by = -0.04f, cx = 0.03f, cy = -0.02f;
    f32 lx[2], ly[2];
    for (i32 i = 0; i < 2; i++) agentctl_haze_light(L, i, tt, asp, &lx[i], &ly[i]);
    f32 v[AGC_HAZE_CELLS_MAX];
    u32 hist[AGC_HAZE_BINS];
    memset(hist, 0, sizeof hist);
    for (i32 i = 0; i < n; i++) {
        f32 fx = (f32)(i % cols) + 0.5f, fy = (f32)(rows - 1 - i / cols) + 0.5f;
        f32 px = (fx / (f32)cols - 0.5f) * asp, py = fy / (f32)rows - 0.5f;
        f32 nx = (px + L->ox) * 1.3f, ny = (py + L->oy) * 1.3f;
        f32 wx = agc_fbm(nx * 1.6f, ny * 1.6f + ay * tt);
        f32 wy = agc_fbm(nx * 1.6f + 5.2f, ny * 1.6f + by * tt);
        f32 f = agc_fbm(nx * 1.3f + 1.4f * (wx - 0.5f) + cx * tt, ny * 1.3f + 1.4f * (wy - 0.5f) + cy * tt);
        f32 g0 = agc_blob(px, py, lx[0], ly[0], 0.8f, 1.1f, L->gk[0]);
        f32 g1 = 0.85f * agc_blob(px, py, lx[1], ly[1], 0.8f, 1.1f, L->gk[1]);
        f32 glow = g0 > g1 ? g0 : g1;
        f32 e = (f - 0.30f) / (0.72f - 0.30f);
        e = e < 0.0f ? 0.0f : (e > 1.0f ? 1.0f : e);
        e = e * e * (3.0f - 2.0f * e);
        f32 val = (0.10f + 0.90f * e) * (0.10f + 0.90f * glow);   /* 0.01..1 */
        v[i] = val;
        i32 b = (i32)(val * (f32)AGC_HAZE_BINS);
        hist[b < 0 ? 0 : (b >= AGC_HAZE_BINS ? AGC_HAZE_BINS - 1 : b)]++;
    }
    /* the stretch: the bins holding the AGC_HAZE_DARK-th and
     * AGC_HAZE_BRIGHT-th share of the cells */
    u32 want_lo = (u32)(AGC_HAZE_DARK * (f32)(n - 1)), want_hi = (u32)(AGC_HAZE_BRIGHT * (f32)(n - 1));
    f32 lo = 0.0f, hi = 1.0f;
    u32 seen = 0;
    bool got_lo = false;
    for (i32 b = 0; b < AGC_HAZE_BINS; b++) {
        seen += hist[b];
        if (!got_lo && seen > want_lo) { lo = ((f32)b + 0.5f) / (f32)AGC_HAZE_BINS; got_lo = true; }
        if (seen > want_hi) { hi = ((f32)b + 0.5f) / (f32)AGC_HAZE_BINS; break; }
    }
    if (hi - lo < 1.0f / (f32)AGC_HAZE_BINS) hi = lo + 1.0f / (f32)AGC_HAZE_BINS;
    f32 usum = 0.0f;
    for (i32 i = 0; i < n; i++) {
        f32 u = (v[i] - lo) / (hi - lo);
        v[i] = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
        usum += v[i];
    }
    /* ...and a curve that brings the frame's mean light to AGC_HAZE_MEAN */
    f32 um = usum / (f32)n, gam = 1.0f;
    if (um > 0.01f && um < 0.99f) gam = logf(AGC_HAZE_MEAN) / logf(um);
    gam = gam < 0.35f ? 0.35f : (gam > 2.5f ? 2.5f : gam);
    for (i32 i = 0; i < n; i++) {
        f32 fx = (f32)(i % cols) + 0.5f, fy = (f32)(rows - 1 - i / cols) + 0.5f;
        f32 u = v[i] > 0.0f ? powf(v[i], gam) : 0.0f;
        u = u * u * (3.0f - 2.0f * u);
        u = AGC_HAZE_FLOOR + (1.0f - AGC_HAZE_FLOOR) * u;
        f32 q = floorf(u * k * AGC_HAZE_REACH + agc_bayer8(fx, fy));
        out[i] = (u8)(q < 0.0f ? 0 : (q > 5.0f ? 5 : (i32)q));
    }
}

/* The run time of the first frame after `run_ms` whose picture differs from
 * `now` (the tones drawn at `run_ms`, row by row), looking at most
 * `max_frames` frames ahead; the last frame looked at when none differs. */
static inline i64 agentctl_haze_next_change_ms(const u8 *now, i32 cols, i32 rows, const AgcHazeLook *L,
                                               f32 k, i64 run_ms, i32 max_frames) {
    if (run_ms < 0) run_ms = 0;
    i64 at = run_ms + agentctl_haze_next_ms(run_ms);
    i32 n = cols * rows;
    if (!now || !L || cols <= 0 || rows <= 0 || n > AGC_HAZE_CELLS_MAX) return at;
    u8 next[AGC_HAZE_CELLS_MAX];
    for (i32 f = 1; f <= max_frames; f++) {
        agentctl_haze_field(next, cols, rows, agentctl_haze_time(at), L, k);
        if (memcmp(next, now, (usize)n) != 0) return at;
        if (f < max_frames) at += agentctl_haze_next_ms(at);
    }
    return at;
}

/* How far a cross-fade begun `since` seconds ago has come: 0..1, eased out. */
static inline f32 agentctl_haze_fade(f64 since) {
    if (since <= 0.0) return 0.0f;
    if (since >= AGC_HAZE_FADE_S) return 1.0f;
    f32 u = 1.0f - (f32)(since / AGC_HAZE_FADE_S);
    return 1.0f - u * u * u;
}

/* The key frames' tones a window keeps per job, for a tab of up to this
 * many cells; a larger one works both keys out every frame. */
#define AGC_HAZE_MEMO_CELLS 640

/* What a window keeps for one job's haze: its look, the state it shows and
 * the one it is fading from; the tones of the key frame drawn and the next,
 * worked out once a key; and the look-ahead for that key, so a repaint
 * inside it does not look again. */
typedef struct {
    u32 job;
    AgcHazeLook look;    /* agentctl_haze_look(job) */
    u8  cue, cue_from;   /* AgcCue; cue_from = AGC_CUE_NONE for a new job tab */
    f64 fade_at;         /* platform clock: when `cue` began to show */
    f64 seen_at;         /* platform clock: last drawn (the oldest is reused) */
    i32 cols, rows;
    f32 k;
    i64 frame;           /* the key frame the look-ahead is for */
    i64 change_ms;       /* agentctl_haze_next_change_ms from the key after it */
    i64 tkey;            /* the key frame t0 holds (t1 the next); -1 = none */
    i32 tcols, trows;
    f32 tk;
    u8  t0[AGC_HAZE_MEMO_CELLS], t1[AGC_HAZE_MEMO_CELLS];
} AgcHazeMemo;
#define AGC_HAZE_MEMO 64   /* as many jobs as can be shown at once (AGENTCTL_MAX_SHOWN) */

/* Elapsed time in whole minutes: "<1m", "7m", "1h05m". */
static inline void agentctl_fmt_minutes(i64 ms, char *out, usize cap) {
    if (ms < 0) ms = 0;
    i64 m = ms / 60000;
    if (m < 1)        snprintf(out, cap, "<1m");
    else if (m < 60)  snprintf(out, cap, "%lldm", (long long)m);
    else              snprintf(out, cap, "%lldh%02lldm", (long long)(m / 60), (long long)(m % 60));
}

/* When the minutes shown for a turn that began at `start_ms` change next. */
static inline i64 agentctl_card_next_edge_ms(i64 start_ms, i64 now_ms) {
    if (start_ms <= 0 || now_ms < start_ms) return now_ms + 60000;
    i64 elapsed = now_ms - start_ms;
    return start_ms + (elapsed / 60000 + 1) * 60000;
}

/* `s` cut to `cols` display columns on a grapheme boundary, with an ellipsis
 * (one column) when it was cut. Control bytes are skipped. */
static inline void agentctl_fit(const char *s, i32 cols, char *out, usize cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!s || cols <= 0) return;
    usize n = strlen(s);
    /* the whole width first: only text that does not fit loses a column to
     * the ellipsis */
    i32 total = 0;
    for (usize k = 0; k < n; ) {
        unsigned char c = (unsigned char)s[k];
        if (c < 0x20 || c == 0x7f) { k++; continue; }
        u32 cp = 0; i32 gc = 0;
        u32 len = utf8_next_grapheme((const u8 *)s + k, n - k, &cp, &gc);
        if (len == 0) len = 1;
        total += gc > 0 ? gc : 0;
        k += len;
    }
    i32 room = total <= cols ? cols : cols - 1;
    usize w = 0;
    i32 used = 0;
    for (usize k = 0; k < n; ) {
        unsigned char c = (unsigned char)s[k];
        if (c < 0x20 || c == 0x7f) { k++; continue; }
        u32 cp = 0; i32 gc = 0;
        u32 len = utf8_next_grapheme((const u8 *)s + k, n - k, &cp, &gc);
        if (len == 0) { k++; continue; }
        if (gc < 0) gc = 0;
        if (used + gc > room || w + len + 4 >= cap) break;
        memcpy(out + w, s + k, len);
        w += len;
        used += gc;
        k += len;
    }
    if (total > cols && w + 3 < cap) { memcpy(out + w, "\xE2\x80\xA6", 3); w += 3; }
    out[w] = '\0';
}

/* Display columns of `s` (control bytes count for nothing). */
static inline i32 agentctl_cols(const char *s) {
    if (!s) return 0;
    usize n = strlen(s);
    i32 total = 0;
    for (usize k = 0; k < n; ) {
        unsigned char c = (unsigned char)s[k];
        if (c < 0x20 || c == 0x7f) { k++; continue; }
        u32 cp = 0; i32 gc = 0;
        u32 len = utf8_next_grapheme((const u8 *)s + k, n - k, &cp, &gc);
        if (len == 0) len = 1;
        total += gc > 0 ? gc : 0;
        k += len;
    }
    return total;
}

/* ---- the log panel: the job's chat --------------------------------------- */

/* What the chat's field does with Enter, by the job's state: sends between
 * turns (the agent has a session to resume); while a turn runs or before the
 * first one, holds the message and sends it once the job is idle; nothing
 * once the job has ended. */
typedef enum {
    AGC_MSG_SEND = 0,       /* idle: Enter sends the next message */
    AGC_MSG_BUSY,           /* a turn runs: Enter queues it for when it ends */
    AGC_MSG_NOT_YET,        /* asked, queued or starting: likewise */
    AGC_MSG_ENDED,          /* nothing can be sent */
} AgcMsgState;

static inline AgcMsgState agentctl_msg_state(u8 state) {
    switch (state) {
    case AGJ_IDLE:    return AGC_MSG_SEND;
    case AGJ_RUNNING: return AGC_MSG_BUSY;
    case AGJ_PENDING_APPROVAL:
    case AGJ_QUEUED:
    case AGJ_STARTING: return AGC_MSG_NOT_YET;
    default:           return AGC_MSG_ENDED;
    }
}

typedef enum {
    AGC_BTN_HIDE = 0,
    AGC_BTN_CLOSE,       /* end the job ("Dismiss" once it has ended) */
    AGC_BTN_STOP,        /* only while a turn runs */
    AGC_BTN_CONTINUE,    /* only between turns, with a session: open it in the agent */
    AGC_BTN_ALLOW,       /* the tool it asks for, this once */
    AGC_BTN_ALWAYS,      /* that kind of call, for the rest of the job */
    AGC_BTN_DENY,
    AGC_BTN_MERGE,       /* an isolated job, between turns: its work into the folder */
    AGC_BTN_DISCARD,     /* ... or thrown away */
    AGC_BTN_REVIEW,      /* a read-only review of its work by another agent */
    AGC_BTN_COUNT
} AgcButton;

/* Which buttons a job's state shows: `asking` while it waits for a tool to
 * be allowed, `isolated` with a worktree of its own, `reviewable` once a
 * turn has finished well. */
static inline void agentctl_panel_buttons(u8 state, u8 outcome, bool has_session, bool asking, bool isolated,
                                          bool reviewable, bool shown[AGC_BTN_COUNT]) {
    for (i32 b = 0; b < AGC_BTN_COUNT; b++) shown[b] = false;
    shown[AGC_BTN_HIDE] = shown[AGC_BTN_CLOSE] = true;
    bool idle = state == AGJ_IDLE;
    shown[AGC_BTN_STOP] = state == AGJ_RUNNING;
    shown[AGC_BTN_CONTINUE] = idle && has_session && !isolated;
    shown[AGC_BTN_ALLOW] = shown[AGC_BTN_ALWAYS] = shown[AGC_BTN_DENY] = asking && state == AGJ_RUNNING;
    shown[AGC_BTN_MERGE] = shown[AGC_BTN_DISCARD] = idle && isolated;
    shown[AGC_BTN_REVIEW] = idle && reviewable && outcome == AGO_DONE;
}

/* The chat is a small Liu window: the job as the open tab of a tab strip, a
 * terminal under it in the terminal's own font and cell that shows the log
 * and ends in a prompt row, and a status bar with what the job runs on and
 * the keys that work it as buttons. The strip and the pill are Liu's own
 * window's; the status bar is taller than Liu's, to hold the buttons. */
#define AGC_LOG_W_PT        780.0f
#define AGC_LOG_H_PT        600.0f
#define AGC_LOG_RADIUS_PT   10.0f
#define AGC_LOG_PILL_X_PT   8.0f      /* strip edge to the tab: a panel has no window lights */
#define AGC_LOG_PILL_MIN_PT 180.0f
#define AGC_LOG_PAD_X_PT    (2.0f * TERMINAL_PADDING_PT)
#define AGC_LOG_PAD_Y_PT    (TERMINAL_PADDING_PT + 2.0f)
#define AGC_LOG_HINT_GAP_PT 6.0f      /* between two key buttons */
#define AGC_LOG_HINT_PAD_PT 8.0f      /* a key button's text to its sides */
#define AGC_LOG_HINT_INSET_PT 4.0f    /* the status bar's top and bottom to a key button */
#define AGC_LOG_STATUS_PT   30.0f     /* taller than Liu's own status bar: it holds buttons */
#define AGC_LOG_SB_GAP_PT   12.0f     /* the status bar's edge and segment gap */

typedef struct {
    PcRect panel;                     /* the window */
    PcRect strip, pill;               /* the tab strip and the job's tab in it */
    PcRect term;                      /* the terminal */
    PcRect status;                    /* the status bar */
    PcRect log;                       /* the log's rows inside the terminal */
    PcRect input;                     /* the prompt row: the next message */
    PcRect btn[AGC_BTN_COUNT];        /* the key hints in the status bar; zero width = not shown */
    f32    cell_w, cell_h;            /* the terminal's cell */
    f32    banner_y;                  /* the dim line the terminal starts with */
    f32    info_right;                /* where the status bar's left part must end */
    i32    log_cols, log_rows;
} AgentctlPanelGeom;

/* `cell_w` / `cell_h` are the terminal's cell, `pill_content_w` what the tab
 * holds (logo, name, state), `shown[k]` which keys the job's state allows and
 * `hint_w[k]` how wide each one's hint is (its word and its keys). */
static inline AgentctlPanelGeom agentctl_panel_geom(f32 fb_w, f32 fb_h, f32 dpi, f32 cell_w, f32 cell_h,
                                                    f32 pill_content_w,
                                                    const bool shown[AGC_BTN_COUNT],
                                                    const f32 hint_w[AGC_BTN_COUNT]) {
    AgentctlPanelGeom g;
    memset(&g, 0, sizeof g);
    if (dpi <= 0.0f) dpi = 1.0f;
    if (cell_w <= 0.0f) cell_w = 8.0f * dpi;
    if (cell_h <= 0.0f) cell_h = 16.0f * dpi;
    f32 w = AGC_LOG_W_PT * dpi, h = AGC_LOG_H_PT * dpi;
    if (w > fb_w - 48.0f * dpi) w = fb_w - 48.0f * dpi;
    if (h > fb_h - 48.0f * dpi) h = fb_h - 48.0f * dpi;
    if (w < 300.0f * dpi) w = 300.0f * dpi;
    if (h < 240.0f * dpi) h = 240.0f * dpi;
    w = floorf(w);
    h = floorf(h);
    f32 px = floorf((fb_w - w) * 0.5f), py = floorf((fb_h - h) * 0.5f);
    if (py < 8.0f * dpi) py = floorf(8.0f * dpi);
    g.panel = (PcRect){ px, py, w, h };
    f32 strip_h = roundf(TOOLBAR_HEIGHT_PT * dpi), status_h = roundf(AGC_LOG_STATUS_PT * dpi);
    g.strip = (PcRect){ px, py, w, strip_h };
    f32 pad_y = roundf(PILL_PAD_Y_PT * dpi), pill_x = roundf(AGC_LOG_PILL_X_PT * dpi);
    f32 pill_w = ceilf(pill_content_w + 2.0f * PILL_INNER_PAD_PT * dpi);
    if (pill_w < AGC_LOG_PILL_MIN_PT * dpi) pill_w = roundf(AGC_LOG_PILL_MIN_PT * dpi);
    if (pill_w > w - 2.0f * pill_x) pill_w = w - 2.0f * pill_x;
    g.pill = (PcRect){ px + pill_x, py + pad_y, pill_w, strip_h - 2.0f * pad_y };
    g.status = (PcRect){ px, py + h - status_h, w, status_h };
    g.term = (PcRect){ px, py + strip_h, w, h - strip_h - status_h };
    f32 tpx = roundf(AGC_LOG_PAD_X_PT * dpi), tpy = roundf(AGC_LOG_PAD_Y_PT * dpi);
    f32 tx = g.term.x + tpx, tw = g.term.w - 2.0f * tpx;
    g.cell_w = cell_w;
    g.cell_h = cell_h;
    g.banner_y = g.term.y + tpy;
    g.input = (PcRect){ tx, g.term.y + g.term.h - tpy - cell_h, tw, cell_h };
    f32 log_y = g.banner_y + 2.0f * cell_h;                   /* the banner, then a blank row */
    f32 log_h = g.input.y - log_y;
    if (log_h < cell_h) log_h = cell_h;
    g.log = (PcRect){ tx, log_y, tw, log_h };
    g.log_cols = (i32)(tw / cell_w);
    g.log_rows = (i32)(log_h / cell_h);
    if (g.log_cols < 1) g.log_cols = 1;
    if (g.log_rows < 1) g.log_rows = 1;
    /* the keys as buttons, right to left: Hide, Close, Continue, then what
     * the job's state asks for (a tool to allow, read left to right as Allow,
     * Always, Deny), Stop, then merge or discard and a review. What does not
     * fit is left out, the last first: Stop keeps its key (Ctrl+C) when its
     * button goes. */
    static const AgcButton order[] = { AGC_BTN_HIDE, AGC_BTN_CLOSE, AGC_BTN_CONTINUE,
                                       AGC_BTN_DENY, AGC_BTN_ALWAYS, AGC_BTN_ALLOW, AGC_BTN_STOP,
                                       AGC_BTN_DISCARD, AGC_BTN_MERGE, AGC_BTN_REVIEW };
    f32 hpad = roundf(AGC_LOG_HINT_PAD_PT * dpi), gap = roundf(AGC_LOG_HINT_GAP_PT * dpi);
    f32 inset = roundf(AGC_LOG_HINT_INSET_PT * dpi);
    f32 x = px + w - roundf(8.0f * dpi);
    for (usize i = 0; i < sizeof order / sizeof order[0]; i++) {
        AgcButton b = order[i];
        if (!shown || !shown[b] || !hint_w || hint_w[b] <= 0.0f) continue;
        f32 bw = ceilf(hint_w[b]) + 2.0f * hpad;
        if (x - bw < px + w * 0.35f) break;                   /* the left part keeps its room */
        x -= bw;
        g.btn[b] = (PcRect){ x, g.status.y + inset, bw, status_h - 2.0f * inset };
        x -= gap;
    }
    g.info_right = x - roundf(AGC_LOG_SB_GAP_PT * dpi) + gap;
    return g;
}

/* The button under a point, or -1. */
static inline i32 agentctl_panel_hit(const AgentctlPanelGeom *g, f32 x, f32 y) {
    if (!g) return -1;
    for (i32 b = 0; b < AGC_BTN_COUNT; b++)
        if (g->btn[b].w > 0.0f && pc_rect_hit(g->btn[b], x, y)) return b;
    return -1;
}

/* ---- finding and copying in the log ------------------------------------ */

/* The first match of `q` (ASCII letters compared without case) in `text`
 * from byte `from`; false when there is none. */
static inline bool agentctl_text_find(const char *text, u32 len, const char *q, u32 from, u32 *at) {
    usize ql = q ? strlen(q) : 0;
    if (!ql || ql > len) return false;
    for (u32 i = from; i + ql <= len; i++) {
        usize k = 0;
        for (; k < ql; k++) {
            char a = text[i + k], b = q[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (k == ql) { *at = i; return true; }
    }
    return false;
}

/* ---- the team at a glance ---------------------------------------------- */

#define AGC_TEAM_W_PT     700.0f
#define AGC_TEAM_ROW_PT   46.0f
#define AGC_TEAM_HEAD_PT  52.0f
#define AGC_TEAM_ROWS_MAX 12

typedef struct {
    PcRect panel;
    PcRect row[AGC_TEAM_ROWS_MAX];
    i32    rows;
    f32    pad, head_y;
} AgcTeamGeom;

/* Every job of every tab, a row each, in a panel centred on the window. */
static inline AgcTeamGeom agentctl_team_geom(f32 fb_w, f32 fb_h, f32 dpi, i32 nrows) {
    AgcTeamGeom g;
    memset(&g, 0, sizeof g);
    if (dpi <= 0.0f) dpi = 1.0f;
    if (nrows > AGC_TEAM_ROWS_MAX) nrows = AGC_TEAM_ROWS_MAX;
    if (nrows < 0) nrows = 0;
    g.pad = roundf(18.0f * dpi);
    f32 w = AGC_TEAM_W_PT * dpi;
    if (w > fb_w - 48.0f * dpi) w = fb_w - 48.0f * dpi;
    if (w < 280.0f * dpi) w = 280.0f * dpi;
    f32 rh = roundf(AGC_TEAM_ROW_PT * dpi);
    f32 head = roundf(AGC_TEAM_HEAD_PT * dpi);
    f32 h = head + (f32)(nrows > 0 ? nrows : 1) * rh + g.pad;
    if (h > fb_h - 48.0f * dpi) {
        i32 fit = (i32)((fb_h - 48.0f * dpi - head - g.pad) / rh);
        if (fit < 1) fit = 1;
        if (nrows > fit) nrows = fit;
        h = head + (f32)(nrows > 0 ? nrows : 1) * rh + g.pad;
    }
    f32 px = floorf((fb_w - w) * 0.5f), py = floorf((fb_h - h) * 0.5f);
    if (py < 8.0f * dpi) py = floorf(8.0f * dpi);
    g.panel = (PcRect){ px, py, floorf(w), floorf(h) };
    g.head_y = py + g.pad;
    for (i32 i = 0; i < nrows; i++) g.row[i] = (PcRect){ px + g.pad * 0.5f, py + head + (f32)i * rh, floorf(w) - g.pad, rh };
    g.rows = nrows;
    return g;
}

static inline i32 agentctl_team_hit(const AgcTeamGeom *g, f32 x, f32 y) {
    for (i32 i = 0; g && i < g->rows; i++) if (pc_rect_hit(g->row[i], x, y)) return i;
    return -1;
}

/* ---- the log's rows ----------------------------------------------------- */

/* One wrapped row of the log: which line, which bytes of it. */
typedef struct {
    i32 line;
    u32 off, len;
} AgcRow;

/* agentctl_wrap, leaving out the first `skip` rows: row `skip` is written to
 * out[0]. */
static inline i32 agc_wrap_rows(const char *text, u32 len, i32 cols, i32 line, u32 base, i32 skip,
                                AgcRow *out, i32 cap) {
    if (cols < 1) cols = 1;
    if (skip < 0) skip = 0;
    i32 rows = 0;
    u32 start = 0, k = 0;
    i32 used = 0;
    while (k < len) {
        u32 cp = 0; i32 gc = 0;
        u32 g = utf8_next_grapheme((const u8 *)text + k, len - k, &cp, &gc);
        if (g == 0) g = 1;
        if (gc < 0) gc = 0;
        if (used + gc > cols && k > start) {
            if (out && rows >= skip && rows - skip < cap) out[rows - skip] = (AgcRow){ line, base + start, k - start };
            rows++;
            start = k;
            used = 0;
        }
        used += gc;
        k += g;
    }
    if (out && rows >= skip && rows - skip < cap) out[rows - skip] = (AgcRow){ line, base + start, len - start };
    rows++;
    return rows;
}

/* Wraps `text` (`len` bytes, no newline) into rows of at most `cols` display
 * columns, on grapheme boundaries; writes up to `cap` rows and returns how
 * many it needed (an empty line is one row). */
static inline i32 agentctl_wrap(const char *text, u32 len, i32 cols, i32 line, u32 base,
                                AgcRow *out, i32 cap) {
    return agc_wrap_rows(text, len, cols, line, base, 0, out, cap);
}

/* The row after (`dir` 1) or before (-1) `from` that holds `q`, going round;
 * -1 when no row does. `bytes` is the log the rows point into. */
static inline i32 agentctl_find_row(const char *bytes, const AgcRow *rows, i32 n, const char *q, i32 from, i32 dir) {
    if (!bytes || !rows || n <= 0 || !q || !q[0]) return -1;
    if (dir == 0) dir = 1;
    for (i32 step = 1; step <= n; step++) {
        i32 i = ((from + dir * step) % n + n) % n;
        const AgcRow *r = &rows[i];
        u32 at;
        if (agentctl_text_find(bytes + r->off, r->len, q, 0, &at)) return i;
    }
    return -1;
}

/* The text of rows `a`..`b` (in order either way): the wrapped rows of one
 * line run on, a new line starts a new line. Returns the bytes written. */
static inline usize agentctl_rows_text(const char *bytes, const AgcRow *rows, i32 a, i32 b, char *out, usize cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!bytes || !rows) return 0;
    if (a > b) { i32 t = a; a = b; b = t; }
    usize w = 0;
    i32 prev_line = -1;
    for (i32 i = a; i <= b; i++) {
        const AgcRow *r = &rows[i];
        if (prev_line >= 0 && r->line != prev_line && w + 1 < cap) out[w++] = '\n';
        prev_line = r->line;
        u32 n = r->len;
        if (w + n + 1 > cap) n = (u32)(cap - 1 - w);
        memcpy(out + w, bytes + r->off, n);
        w += n;
        if (w + 1 >= cap) break;
    }
    out[w] = '\0';
    return w;
}

/* ---- the job tab's hover: a small terminal ------------------------------ */

typedef struct {
    PcRect win;                 /* the hover */
    f32    text_x, text_y;      /* its first cell */
    f32    row_h;
    f32    radius;
    i32    cols, rows;
} AgcTipGeom;

/* The hover hung at `top_y`, its left edge at `anchor_x` and kept inside the
 * frame. False when the frame is too narrow for AGC_TIP_MIN_COLS columns. */
static inline bool agentctl_tip_geom(f32 anchor_x, f32 top_y, f32 fb_w, f32 dpi, f32 ui_cw, f32 ui_ch,
                                     AgcTipGeom *g) {
    if (!g) return false;
    memset(g, 0, sizeof *g);
    if (dpi <= 0.0f) dpi = 1.0f;
    if (ui_cw <= 0.0f || ui_ch <= 0.0f) return false;
    f32 marg = 8.0f * dpi;
    f32 pad_x = roundf(AGC_TIP_PAD_X_PT * dpi), pad_y = roundf(AGC_TIP_PAD_Y_PT * dpi);
    i32 cols = AGC_TIP_COLS;
    f32 room = fb_w - 2.0f * marg - 2.0f * pad_x;
    i32 fit = room > 0.0f ? (i32)(room / ui_cw) : 0;
    if (fit < cols) cols = fit;
    if (cols < AGC_TIP_MIN_COLS) return false;
    f32 w = ceilf((f32)cols * ui_cw + 2.0f * pad_x);
    f32 x = floorf(anchor_x);
    if (x + w > fb_w - marg) x = floorf(fb_w - marg - w);
    if (x < marg) x = floorf(marg);
    f32 y = roundf(top_y);
    f32 row_h = ui_ch + roundf(AGC_TIP_ROW_GAP_PT * dpi);
    g->win = (PcRect){ x, y, w, 2.0f * pad_y + (f32)AGC_TIP_ROWS * row_h };
    g->text_x = x + pad_x;
    g->text_y = y + pad_y;
    g->row_h = row_h;
    g->radius = roundf(AGC_TIP_RADIUS_PT * dpi);
    g->cols = cols;
    g->rows = AGC_TIP_ROWS;
    return true;
}

/* The strip of the frame between the job tab's bottom (`tab_bottom`) and the
 * hover, as wide as the hover: the pointer crosses it on its way in, and the
 * hover stays open while it does. Zero height when there is none. */
static inline PcRect agentctl_tip_gap(const AgcTipGeom *g, f32 tab_bottom) {
    if (!g || g->win.w <= 0.0f || g->win.y <= tab_bottom) return (PcRect){ 0.0f, 0.0f, 0.0f, 0.0f };
    return (PcRect){ g->win.x, tab_bottom, g->win.w, g->win.y - tab_bottom };
}

/* What the hover's cursor is: live while the job is at work or about to be
 * (a turn running, starting, queued, or waiting for the user's answer), at
 * rest after a prompt mark between turns, none once the job has ended. */
typedef enum {
    AGC_TIP_CURSOR_NONE = 0,
    AGC_TIP_CURSOR_LIVE,
    AGC_TIP_CURSOR_REST,
} AgcTipCursor;

static inline AgcTipCursor agentctl_tip_cursor(u8 state) {
    switch (state) {
    case AGJ_PENDING_APPROVAL:
    case AGJ_QUEUED:
    case AGJ_STARTING:
    case AGJ_RUNNING: return AGC_TIP_CURSOR_LIVE;
    case AGJ_IDLE:    return AGC_TIP_CURSOR_REST;
    default:          return AGC_TIP_CURSOR_NONE;
    }
}

/* The hover's screen: `lines` (oldest first, NUL-terminated) wrapped to
 * `cols`, scrolled so the newest row is at the bottom of `rows` less
 * `reserve` (the cursor's row). Writes those rows into `out` (row offsets
 * are into their own line) and returns how many there are. */
static inline i32 agentctl_tip_screen(const char *const *lines, i32 n, i32 cols, i32 rows, i32 reserve,
                                      AgcRow *out, i32 cap) {
    if (!lines || n <= 0 || !out || cap <= 0) return 0;
    i32 room = rows - (reserve > 0 ? reserve : 0);
    if (room > cap) room = cap;
    if (room <= 0) return 0;
    i32 total = 0;
    for (i32 i = 0; i < n; i++)
        total += agentctl_wrap(lines[i] ? lines[i] : "", lines[i] ? (u32)strlen(lines[i]) : 0, cols, i, 0, NULL, 0);
    i32 skip = total > room ? total - room : 0;
    i32 count = 0;
    for (i32 i = 0; i < n && count < room; i++) {
        const char *s = lines[i] ? lines[i] : "";
        u32 len = (u32)strlen(s);
        i32 c = agentctl_wrap(s, len, cols, i, 0, NULL, 0);
        if (skip >= c) { skip -= c; continue; }
        agc_wrap_rows(s, len, cols, i, 0, skip, out + count, room - count);
        i32 got = c - skip;
        count += got < room - count ? got : room - count;
        skip = 0;
    }
    return count;
}

#endif /* UI_AGENTCTL_CARD_H */
