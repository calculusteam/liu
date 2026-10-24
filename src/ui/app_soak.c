/*
 * Liu — the self-driving soak. See app_soak.h.
 */
#include "ui/app_soak.h"
#include "renderer/image_stats.h"
#include "ui/markdown/md_parse.h"
#include "ui/ui.h"
#include "ui/tab_trash.h"
#include "core/percentile.h"
#include "core/soak_probe.h"
#include "core/soak_gate.h"
#include "core/soak_epoch.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
  #include <libproc.h>
#endif

/* ---- configuration ------------------------------------------------------ */

static char  g_scenario[32];
static i32   g_resolved = -1;
static u64   g_seed = 0, g_rand = 0;
static i32   g_epochs = 5, g_warmup = 1;
/* Required number of epochs for a footprint series. Zero uses
 * SOAK_MIN_EPOCHS. */
static i32   g_mem_epochs_min = 0;
static u64   g_ops_per_epoch = 40;
static f64   g_min_epoch_seconds = 0.0;
static const AppSoakHost *g_host = NULL;

static u64 srand64(void) {
    u64 z = (g_rand += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static u32 srand_below(u32 n) { return n ? (u32)(srand64() % n) : 0; }

bool app_soak_active(void) {
    if (g_resolved < 0) {
        const char *e = liu_dev_env("LIU_SOAK");
        if (e && e[0]) {
            snprintf(g_scenario, sizeof g_scenario, "%s", e);
            g_resolved = 1;
        } else {
            g_scenario[0] = '\0';
            g_resolved = 0;
        }
    }
    return g_resolved == 1;
}

const char *app_soak_scenario(void) { (void)app_soak_active(); return g_scenario; }

/* ---- run state ---------------------------------------------------------- */

typedef struct {
    u64 windows_opened, windows_closed;
    u64 tabs_opened, tabs_closed;
    u64 splits_made, panes_closed;
    u64 prompts_attached;     /* attachment blocks handed to pane agents */
    u64 sleeps, wakes, sleep_blocked;
    u64 font_invalidations, dpi_changes, resizes;
    /* OSC 4 / OSC 104 sequences fed by `caches` — R11's live coverage. */
    u64 palette_writes;
    /* Counters used to confirm that each workload reached the operation
     * being measured. */
    u64 keys_sent;
    u64 stream_bytes_seen;
    u64 scroll_fills, scroll_scrolls, scroll_searches, scroll_selects, scroll_reflows;
    u64 sites_added, sites_started, sites_stopped, sites_removed;
    /* LOG BYTES PARSED, which is what a running Site actually does. Distinct
     * from sites_added, which is SETUP: a stable population of Sites should
     * not have to add another record every epoch to prove it is still alive,
     * and asking it to would measure lifecycle churn instead of the steady
     * state. */
    u64 site_log_writes;
    u64 agent_panes, agent_detected, agent_activity, agent_busy_seen;
    u64 agent_scan_sweeps;
    u64 md_opens, md_blocks, md_closes, md_navigations;
    u64 win_steady_frames, win_steady_peak;
    /* winminN: minimize/restore cycles of windows 1..N-1, each CONFIRMED
     * through the platform (AppKit's isMiniaturized), and the frames those
     * windows painted while minimized. */
    u64 min_requested, min_confirmed, restore_confirmed, min_unconfirmed;
    u64 min_frames_while_min, min_reported_visible;
    u64 tab_steady_frames, tab_steady_peak;
    u64 grid_steady_frames, grid_steady_peak;
    /* winflood: bytes fed into window 0, and how many frames EACH window
     * painted while that was the only thing happening. */
    u64 win_flood_bytes;
    /* winanim: how many UI animations window 0 ran while the others sat idle. */
    u64 win_anim_starts;
    u64 win_frames[8];
    i32 win_frames_n;
    u64 img_opens, img_sixels, img_closes;
    u64 br_opens, br_closes, br_peak, br_refused, br_webkit_peak;
    u64 ed_opens, ed_edits, ed_undos, ed_finds;
    u64 errors;
} SoakCounters;

static SoakCounters g_c;
static SoakSample   g_warm, g_final, g_epoch_samples[64];
/* Prompt attachment blocks alive at the final sample — after the tabs are
 * gone. Not a SoakSample field: that struct is core's, and the counter is the
 * terminal layer's (prompt_capture.h). */
static i32          g_final_imgs_live = 0;
static i32          g_epoch_n = 0;
static u64          g_ops = 0;
static bool         g_probe_children = false;
/* Scenario parameters, set once from the scenario NAME so a run is described
 * by its name alone: `stream8` is eight panes, `sites32` is thirty-two. */
static i32          g_stream_panes = 1;
static bool         g_stream_armed = false;
static i32          g_sites_want   = 0;
static i32          g_agents_want  = 0;   /* agents32 / agents64            */
/* Counter value at the start of the final sampling epoch. Comparing it at the
 * end confirms that the measured path remained active during that epoch. */
static u64          g_reach_last_epoch = 0;
static bool         g_reach_snapped    = false;
/* How many windows the settle must KEEP between epochs. 0 for every scenario
 * but `winsN`, whose axis IS the window count: a settle that closed the
 * population would make each epoch measure the rebuild rather than the steady
 * state. Cleared by scenario_teardown so the final sample is taken in the same
 * one-window shape the baseline was. */
static i32          g_windows_hold = 0;
static i32          g_windows_want = 0;   /* what winsN asked for */
static i32          g_tabs_want = 0;      /* steady tabsN in one window */
static Terminal    *g_tab_seeded_terminal[MAX_TABS];
static i32          g_grid_windows = 0, g_grid_tabs = 0;
static Terminal    *g_grid_seeded_terminal[8][MAX_TABS];
/* Explicit fixture ownership: a shell prompt increments content_writes before
 * the soak can fill the pane, so that counter cannot mean "fixture present".
 * Window indices stay stable while wins/winflood/winanim hold their population;
 * window 0 is reset when settle replaces its terminal. */
static Terminal    *g_win_seeded_terminal[8];
static char         g_img_dir[512];
static i32          g_img_ready    = 0;
static LiuImageStats g_img_base;      /* the stats are MONOTONIC; snapshot */
static i32          g_browser_want = 0;   /* browser1..browser4 */
static i32          g_webkit_base  = 0;   /* helper processes before the soak started */
/* SEPARATE FROM THE VALUE, because 0 is a legal reading: with no other WebKit
 * client running (those XPC services are per-client rather than persistent
 * daemons), 0 is exactly what the first probe returns. Taking 0 as "not
 * sampled yet" would re-take the baseline until it read non-zero, and the
 * first non-zero reading is by construction the soak's own helpers, created by
 * the operations before it. The delta `browser_webkit_procs() - g_webkit_base`
 * would then be short by those helpers — exactly zero for `browser1`, one view
 * and one set of helpers. */
static bool         g_webkit_base_taken = false;
/* Wall seconds spent on INSTRUMENTS inside do_one_op. See
 * app_soak_workload_seconds_take(). */
static f64          g_instr_in_op  = 0.0;
static char         g_md_dir[512];        /* the markdown/editor fixture root */
static i32          g_md_files     = 0;
static i32          g_agents_armed = 0;   /* panes already given a producer */
static u64          g_scan_sweeps0 = 0;   /* the process-scan counter is
                                           * MONOTONIC and never reset, so a
                                           * baseline is taken and subtracted */
static i32          g_scroll_lines = 10000;
static i32          g_scroll_cols  = 200;

/* Record latency from input dispatch until the typed character is rendered;
 * frame timing alone does not include this interval. */
static LiuHist      g_type_hist;
static f64          g_type_t0      = 0.0;
static u64          g_type_writes0 = 0;
static bool         g_type_pending = false;
static u64          g_type_returns = 0;
static u64          g_type_lost    = 0;
static i32          g_phase = 0;      /* 0 warm-up, 1 measured, 2 settle, 3 done */
static i32          g_epoch_i = 0;
static u64          g_epoch_ops_left = 0;
static f64          g_epoch_started_at = 0.0;
static i32          g_warm_left = 0;
static f64          g_settle_until = 0.0;
/* When app_soak_begin ran — see SOAK_STARTUP_SETTLE_SEC. */
static f64          g_started_at   = 0.0;
static i32          g_exit = 0;
static FILE        *g_jsonl = NULL;

/* ---- the operations, all production entry points ------------------------ */

/* ---- IDLE. The absence of work, which is a measurement too. -------------
 *
 * Liu paints nothing when idle, and this scenario exists to keep that true.
 * It performs no operation at all, and app_soak_tick deliberately does NOT
 * set `needs_redraw` for it — forcing a frame and then calling the result
 * "idle" would falsify the measurement. What the frame probe reports over it
 * is the wait distribution and a present count that should be zero. */
static void op_idle(u64 op) { (void)op; }

/* ---- TYPING. One keystroke, through the path a key event takes. ---------
 *
 * g_host->send_input reaches app_send_input — the first call main.c's
 * EVENT_KEY handler makes. That covers the scroll-to-bottom, the activity
 * mark, the keystroke-time stamp the agent busy gate reads, the pty write and
 * the broadcast fan-out; session_write alone would skip everything but the
 * write. It does NOT cover the translate shadow or prompt capture, which the
 * key handler calls separately afterwards — see app_soak.h. */
static void op_typing(u64 op) {
    AppState *app = g_host->window_state(0);
    if (!app || app->tab_count == 0 || !g_host->send_input) return;
    Terminal *t = app->tabs[app->active_tab].terminal;

    /* Close an outstanding measurement first: the echo may have landed on any
     * iteration since the dispatch, and this is the earliest the driver can
     * see it. */
    if (g_type_pending && t) {
        if ((u64)t->content_writes > g_type_writes0) {
            f64 dt = platform_time_sec() - g_type_t0;
            if (dt > 0.0) liu_hist_add_sec(&g_type_hist, dt);
            g_type_pending = false;
        } else if (platform_time_sec() - g_type_t0 > 2.0) {
            /* Two seconds without an echo is not a latency sample, it is a
             * lost one — counted rather than folded into the tail, because a
             * distribution that quietly absorbs its own failures is worse than
             * one with a gap in it. */
            g_type_lost++;
            g_type_pending = false;
        }
        return;                       /* one keystroke in flight at a time */
    }

    /* A carriage return runs a command and redraws a prompt: a different cost,
     * counted here and deliberately outside the echo distribution. */
    if (op % 40 == 39) {
        u8 cr = (u8)'\r';
        g_host->send_input(app, &cr, 1);
        g_c.keys_sent++;
        g_type_returns++;
        return;
    }

    u8 ch = (u8)('a' + (op % 26));
    if (t) { g_type_writes0 = (u64)t->content_writes; g_type_t0 = platform_time_sec(); g_type_pending = true; }
    g_host->send_input(app, &ch, 1);
    g_c.keys_sent++;
}

/* Create panes through the normal split path and write input through each
 * session so the terminal pipeline processes output. */
/* Repeatedly splitting the newly focused leaf eventually hits the minimum
 * pane size while larger leaves still have room. Exercise the requested
 * population instead of spending the entire scenario retrying that leaf. */
static void soak_split_largest(AppState *app) {
    Tab *tab = app_active_tab(app);
    if (!tab) return;
    i32 count = tab_split_pane_count(tab), best = 0;
    i64 largest = -1;
    for (i32 p = 0; p < count; p++) {
        Terminal *term = app_tab_pane_terminal(tab, p);
        i64 cells = term ? (i64)term->cols * term->rows : 0;
        if (cells > largest) { largest = cells; best = p; }
    }
    tab->active_pane = best;
    app_split_tab(app, (count % 2) ? SPLIT_H : SPLIT_V);
}

static void op_stream(u64 op) {
    AppState *app = g_host->window_state(0);
    if (!app || app->tab_count == 0) return;
    Tab *t = &app->tabs[app->active_tab];

    i32 have = tab_split_pane_count(t);
    if (have < g_stream_panes) {
        soak_split_largest(app);
        if (tab_split_pane_count(t) > have) g_c.splits_made++;
        g_stream_armed = false;
        return;                       /* one split per operation */
    }

    if (!g_stream_armed) {
        for (i32 p = 0; p < tab_split_pane_count(t); p++) {
            Session *ps = (p == 0) ? t->session
                        : (p == 1) ? t->session2 : t->extra_sessions[p - 2];
            if (!ps) continue;
            char cmd[256];
            i32 n = (p == 0)
                ? snprintf(cmd, sizeof cmd,
                           "i=0; while :; do printf 'S%%02d-%%06d %s\\n' %d $i; "
                           "i=$((i+1)); done\r", "abcdefghijklmnopqrstuvwxyz", p)
                : snprintf(cmd, sizeof cmd,
                           "i=0; while :; do printf 'S%%02d-%%06d\\n' %d $i; "
                           "i=$((i+1)); sleep 0.05; done\r", p);
            if (n > 0) session_write(ps, (const u8 *)cmd, n);
        }
        g_stream_armed = true;
        return;
    }
    /* Proof that the path was reached. `content_writes` counts cells whose
     * codepoint actually changed, so it moves only if bytes came off the pty,
     * through the poll budget, into the parser. Without this check a run in
     * which every producer failed to start would still report an idle
     * distribution. */
    {
        static u64 last_writes = 0;
        u64 now = 0;
        for (i32 p = 0; p < tab_split_pane_count(t); p++) {
            Terminal *pt = (p == 0) ? t->terminal
                         : (p == 1) ? t->terminal2 : t->extra_terminals[p - 2];
            if (pt) now += (u64)pt->content_writes;
        }
        if (now > last_writes) g_c.stream_bytes_seen += now - last_writes;
        last_writes = now;
    }
    (void)op;   /* and then nothing: draining them IS the measurement */
}

/* Populate scrollback, then exercise the six scrolling operations with the
 * configured row and column sizes. */
static void op_scroll(u64 op) {
    AppState *app = g_host->window_state(0);
    if (!app || app->tab_count == 0) return;
    Terminal *t = app->tabs[app->active_tab].terminal;
    if (!t) return;

    switch (op % 6) {
    case 0: {                                   /* fill hot + compressed */
        char row[512];
        for (i32 i = 0; i < 64; i++) {
            i32 n = snprintf(row, sizeof row, "row %06llu ",
                             (unsigned long long)(op * 64 + (u64)i));
            while (n < (i32)sizeof row - 4 && n < g_scroll_cols) {
                char c = (char)('a' + ((i + n) % 26));
                row[n++] = c;
            }
            n += snprintf(row + n, sizeof row - (usize)n, "\r\n");
            terminal_feed(t, (const u8 *)row, (usize)n);
        }
        g_c.scroll_fills++;
        break;
    }
    case 1: terminal_scroll_up(t, 3);   g_c.scroll_scrolls++; break;
    case 2: terminal_scroll_down(t, 3); g_c.scroll_scrolls++; break;
    case 3:                                     /* dense matches */
        terminal_search_start(t, "row", false, false);
        terminal_search_next(t);
        terminal_search_stop(t);
        g_c.scroll_searches++;
        break;
    case 4:                                     /* a block selection */
        selection_start(t, 0, 0, 1, false);
        selection_update(t, t->cols - 1, t->rows - 1);
        selection_clear(t);
        g_c.scroll_selects++;
        break;
    default: {                                  /* reflow */
        i32 w = (t->cols == g_scroll_cols) ? g_scroll_cols / 2 : g_scroll_cols;
        if (w < 20) w = 20;
        terminal_resize(t, w, t->rows);
        g_c.scroll_reflows++;
        break;
    }
    }
}

/* ---- BROWSER. One to four WKWebViews, created and destroyed for real. ---
 *
 * Four constraints, three of which fail silently:
 *
 *   Only loopback URLs. browser_nav_is_allowed accepts http/https with a
 *   local/tailnet host (names are resolved later); anything it rejects falls
 *   through to platform_open_url, which would launch the default browser from
 *   inside a soak. `file://` is rejected too, so it cannot be the fixture.
 *   Loopback ports that refuse at once are used instead: the page fails to
 *   load, and a WKWebView with a failed load is still a real WKWebView with a
 *   real WebContent process, which is what the lifecycle measurement needs.
 *
 *   A NULL url creates ONE browser, not N. app_new_browser_tab focuses any
 *   existing TAB_BROWSER and returns its index, so calling it four times
 *   yields one view.
 *
 *   SAME-URL DEDUP: a tab whose url matches byte for byte is reused. Distinct
 *   ports are required for distinct views.
 *
 *   THE CAP IS 4, PROCESS-WIDE, AND A REFUSAL IS NOT AN ERROR. The fifth view
 *   is refused with a toast and -1; counting that as an error would fail every
 *   run that reached the cap.
 *
 * WebKit memory is not Liu's. The helper processes are counted separately and
 * never enter the ledger or the raw-gap equation; they are not children of
 * this process, so the count is by name and by delta against a baseline taken
 * before the first view. */
/* Timed, and charged to the harness. This walks every pid on the machine and
 * calls proc_name() on each, and op_browser calls it once per operation. It
 * runs inside do_one_op, so its time lands in g_op_seconds, which main.c
 * subtracts from the harness accumulator; left there, a whole-process-table
 * scan would be charged to Liu's own `tick` and `work` on nearly every
 * operation of the browser scenario. Adding its time to g_instr_in_op takes
 * it back out. */
static i32 browser_webkit_procs(void) {
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
    f64 liu__t0 = platform_time_sec();
    i32 n = 0;
    i32 cap = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (cap <= 0) { g_instr_in_op += platform_time_sec() - liu__t0; return 0; }
    pid_t *pids = calloc((usize)cap / sizeof(pid_t) + 8, sizeof(pid_t));
    if (!pids) { g_instr_in_op += platform_time_sec() - liu__t0; return 0; }
    i32 got = proc_listpids(PROC_ALL_PIDS, 0, pids, cap);
    i32 count = got > 0 ? got / (i32)sizeof(pid_t) : 0;
    for (i32 i = 0; i < count; i++) {
        if (pids[i] <= 0) continue;
        char name[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_name(pids[i], name, sizeof name) <= 0) continue;
        if (strstr(name, "WebContent") || strstr(name, "WebKit")) n++;
    }
    free(pids);
    g_instr_in_op += platform_time_sec() - liu__t0;
    return n;
#else
    return 0;
#endif
}

static void op_browser(u64 op) {
    AppState *app = g_host->window_state(0);
    if (!app || !g_host->window_handle) return;
    struct PlatformWindow *pw = g_host->window_handle(0);
    if (!pw) return;

    if (!g_webkit_base_taken) {
        g_webkit_base = browser_webkit_procs();
        g_webkit_base_taken = true;
    }

    i32 views = app_browser_view_count();
    if ((u64)views > g_c.br_peak) g_c.br_peak = (u64)views;

    if (views < g_browser_want) {
        /* A distinct loopback port per view: distinct URL, instant refusal,
         * no network wait and nothing outside this machine is contacted. */
        char url[64];
        snprintf(url, sizeof url, "http://127.0.0.1:%d/", 9 + views);
        i32 slot = app_new_browser_tab(app, pw, url, NULL, NULL);
        if (slot >= 0) { g_c.br_opens++; g_c.tabs_opened++; }
        else            g_c.br_refused++;   /* the cap; not an error */
        return;
    }

    i32 wk = browser_webkit_procs() - g_webkit_base;
    if (wk > 0 && (u64)wk > g_c.br_webkit_peak) g_c.br_webkit_peak = (u64)wk;

    /* Create and destroy one view per cycle to exercise both lifecycle
     * paths. */
    if ((op % 16) == 15) {
        for (i32 i = app->tab_count - 1; i >= 0; i--) {
            if (app->tabs[i].kind != TAB_BROWSER) continue;
            app_close_tab(app, i);
            g_c.br_closes++;
            g_c.tabs_closed++;
            break;
        }
    }
}

/* Generate TGA fixtures here so image decoding, cache storage, and texture
 * upload use the normal image path without an external encoder. */
static bool img_write_tga(const char *path, i32 w, i32 h, u8 r, u8 g, u8 b) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    u8 hdr[18] = {0};
    hdr[2]  = 2;                                  /* uncompressed true-colour */
    hdr[12] = (u8)(w & 0xFF);  hdr[13] = (u8)((w >> 8) & 0xFF);
    hdr[14] = (u8)(h & 0xFF);  hdr[15] = (u8)((h >> 8) & 0xFF);
    hdr[16] = 32;                                 /* bits per pixel */
    hdr[17] = 0x28;                               /* alpha + top-down */
    bool ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    u8 *row = malloc((usize)w * 4u);
    if (row && ok) {
        for (i32 x = 0; x < w; x++) {             /* TGA stores BGRA */
            row[x * 4 + 0] = b; row[x * 4 + 1] = g;
            row[x * 4 + 2] = r; row[x * 4 + 3] = 255;
        }
        for (i32 y = 0; y < h && ok; y++)
            ok = fwrite(row, 1, (usize)w * 4u, f) == (usize)w * 4u;
    } else ok = false;
    free(row);
    fclose(f);
    return ok;
}

static void img_write_fixture(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(g_img_dir, sizeof g_img_dir, "%s/liu-soak-img", tmp);
    mkdir(g_img_dir, 0700);
    char path[640];
    /* Small: pool hits and misses. Large: the pool's byte budget and eviction.
     * Oversize: past METAL_IMAGE_OVERSIZE_BYTES so the dedicated tier runs —
     * 2100x2100x4 is 17.6 MiB, over the 16 MiB line a Sixel can never cross. */
    struct { const char *name; i32 w, h; u8 r, g, b; } kFix[] = {
        { "small00.tga",  64,   64, 200,  30,  30 },
        { "small01.tga", 128,  128,  30, 200,  30 },
        { "small02.tga", 256,  256,  30,  30, 200 },
        { "large00.tga", 1600, 1200, 180, 180,  40 },
        { "over00.tga",  2100, 2100,  40, 180, 180 },
    };
    for (usize i = 0; i < sizeof kFix / sizeof kFix[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", g_img_dir, kFix[i].name);
        img_write_tga(path, kFix[i].w, kFix[i].h, kFix[i].r, kFix[i].g, kFix[i].b);
    }
    g_img_ready = 1;
}

/* A tiny Sixel: DCS q, one colour, a band of set pixels, ST. Reaches
 * vt_parser.c's decoder, terminal_add_image and the renderer's upload. */
static void img_feed_sixel(Terminal *t, i32 idx) {
    char buf[2048];
    i32 n = snprintf(buf, sizeof buf,
                     "\033Pq#0;2;%d;%d;%d", 20 + (idx * 7) % 80, 40, 60);
    n += snprintf(buf + n, sizeof buf - (usize)n, "#0");
    for (i32 i = 0; i < 64 && n < (i32)sizeof buf - 8; i++)
        buf[n++] = (char)('?' + 63);          /* all six rows set */
    n += snprintf(buf + n, sizeof buf - (usize)n, "\033\\");
    terminal_feed(t, (const u8 *)buf, (usize)n);
}

/* The first terminal-bearing tab, or NULL. */
static Terminal *img_any_terminal(AppState *app) {
    for (i32 i = 0; i < app->tab_count; i++)
        if (app->tabs[i].kind != TAB_FILEBROWSER && app->tabs[i].terminal)
            return app->tabs[i].terminal;
    return NULL;
}

static void op_images(u64 op) {
    AppState *app = g_host->window_state(0);
    if (!app) return;
    if (!g_img_ready) { img_write_fixture(); g_img_base = g_liu_image_stats; return; }

    switch (op % 6) {
    case 0: case 1: {                  /* file-browser preview: cold then warm */
        Tab *t = &app->tabs[app->active_tab];
        FileBrowser *fb = (t->kind == TAB_FILEBROWSER) ? t->fb : NULL;
        if (!fb) {
            i32 slot = app_new_filebrowser_tab(app, g_img_dir);
            if (slot >= 0) { g_c.tabs_opened++; app_switch_tab(app, slot); }
            else g_c.errors++;
            return;
        }
        /* Same file twice in a row is the cache HIT; a different one is the
         * MISS. Both are wanted and the stats separate them. */
        i32 which = (i32)((op / 2) % 5);
        for (i32 i = 0; i < fb->entry_count; i++) {
            if (fb->entries[i].is_dir) continue;
            if ((which--) > 0) continue;
            if (fb_open_file(fb, i)) g_c.img_opens++;
            break;
        }
        break;
    }
    case 2:                            /* close: teardown with work possibly in flight */
        { Tab *t = &app->tabs[app->active_tab];
          FileBrowser *fb = (t->kind == TAB_FILEBROWSER) ? t->fb : NULL;
          if (fb) { fb_close_viewer(fb); g_c.img_closes++; } }
        break;
    case 3: case 4: {                  /* Sixel through the real parser */
        /* Not the active tab: this scenario spends most of its operations in a
         * FileBrowser tab, which has no Terminal. Find a terminal tab and keep
         * feeding it. */
        Terminal *term = img_any_terminal(app);
        if (term) { img_feed_sixel(term, (i32)op); g_c.img_sixels++; }
        break;
    }
    default: {                         /* drop them: eviction and re-upload */
        Terminal *term = img_any_terminal(app);
        if (term && (op % 30) == 5) terminal_clear_images(term);
        break;
    }
    }
}

/* ---- MULTI-WINDOW STEADY STATE. ----------------------------------------
 *
 * The `windows` scenario measures CHURN — open and close, over and over. This
 * measures the other half: what N windows COST while they just sit there,
 * each with a populated pane and live renderer, font atlas and caches. The two
 * answer different questions, and the churn number is dominated by AppKit's
 * own retention.
 *
 * THREE HAZARDS, each of which silently eats the population:
 *
 *   The last window may never be closed — liu_window_close treats it as a QUIT
 *   and ends the loop, so every teardown loop is `while (count > 1)`.
 *
 *   A held window may never have ZERO tabs. main.c reaps any window with no
 *   tabs at the top of every iteration, before the soak tick, whenever more
 *   than one is open. Content therefore goes in before anything is closed.
 *
 *   Window indices are UNSTABLE: liu_window_close moves the tail into the
 *   hole. Nothing here caches an index across an operation. */
static void op_windows_steady(u64 op) {
    i32 have = g_host->window_count();
    if (have < g_windows_want) {
        if (g_host->window_open() >= 0) {
            g_c.windows_opened++;
            g_win_seeded_terminal[have] = NULL;
        }
        else g_c.errors++;
        return;
    }

    /* Initialize each window's first pane before sampling so steady-state
     * frame costs include populated grids. */
    for (i32 w = 0; w < have; w++) {
        AppState *a = g_host->window_state(w);
        if (!a || a->tab_count == 0) continue;
        Terminal *t = a->tabs[a->active_tab].terminal;
        if (!t || g_win_seeded_terminal[w] == t) continue;
        char row[400];
        for (i32 i = 0; i < t->rows; i++) {
            i32 n = snprintf(row, sizeof row,
                             "\033[3%dmwin %d row %03d \033[4m\346\227\245\346\234\254\033[0m ",
                             1 + (i % 6), w, i);
            while (n < (i32)sizeof row - 8 && n < t->cols) {
                char c = (char)('a' + ((i + n) % 26));
                row[n++] = c;
            }
            n += snprintf(row + n, sizeof row - (usize)n, "\r\n");
            terminal_feed(t, (const u8 *)row, (usize)n);
        }
        g_win_seeded_terminal[w] = t;
        return;                       /* one window filled per operation */
    }

    /* And then: nothing. Steady state is the measurement — the loop keeps
     * painting every window because app_render runs per window and the frame
     * probe's forced-visible flag is already set for a soak. */
    if ((i32)op % 64 == 0) {
        u64 frames = 0;
        for (i32 w = 0; w < have; w++) {
            AppState *a = g_host->window_state(w);
            if (a) frames += (u64)a->frame_seq;
        }
        g_c.win_steady_frames = frames;
        if ((u64)have > g_c.win_steady_peak) g_c.win_steady_peak = (u64)have;
    }
}

/* Keep local PTY sessions alive in one window while rendering only the
 * active tab. Seed terminals by identity because a shell prompt is not a
 * reliable signal that the workload started. */
static void op_tabs_steady(u64 op) {
    AppState *app = g_host->window_state(0);
    if (!app) return;
    if (app->tab_count < g_tabs_want) {
        Session *s = session_create_local(app->grid_cols > 0 ? app->grid_cols : 80,
                                          app->grid_rows > 0 ? app->grid_rows : 24);
        if (s && session_status(s) == SESSION_CONNECTED) {
            i32 slot = app_new_tab(app, s, "soak-tab");
            if (slot >= 0) { g_c.tabs_opened++; g_tab_seeded_terminal[slot] = NULL; }
            else { session_destroy(s); g_c.errors++; }
        } else { if (s) session_destroy(s); g_c.errors++; }
        return;
    }
    for (i32 i = 0; i < app->tab_count; i++) {
        Terminal *t = app->tabs[i].terminal;
        if (!t || g_tab_seeded_terminal[i] == t) continue;
        char line[256];
        for (i32 row = 0; row < t->rows; row++) {
            i32 n = snprintf(line, sizeof line, "\033[3%dmsoak tab %02d row %03d \033[0m\r\n",
                             1 + (row % 6), i, row);
            if (n > 0) terminal_feed(t, (const u8 *)line, (usize)n);
        }
        g_tab_seeded_terminal[i] = t;
        return;
    }
    if ((op % 64) == 0) {
        g_c.tab_steady_frames = (u64)app->frame_seq;
        if ((u64)app->tab_count > g_c.tab_steady_peak)
            g_c.tab_steady_peak = (u64)app->tab_count;
    }
}

/* ---- MINIMIZE AND RESTORE, THROUGH THE REAL WINDOW SYSTEM. ---------------
 *
 * `winminN` holds N populated windows like `winsN` and then, on the frame
 * clock, minimizes windows 1..N-1 with the platform's own call (AppKit's
 * -miniaturize:, i.e. exactly what the yellow button does), confirms after
 * WINMIN_SETTLE_S that the window system says they are minimized, restores
 * them, and confirms again. Window 0 stays up so the loop is never left with
 * nothing to service. No Accessibility permission is needed: the process
 * minimizes its own windows. Minimizing is asynchronous (the Dock animation),
 * hence confirming on the clock rather than on the next operation. */
#define WINMIN_SETTLE_S 2.0
static i32 g_winmin_state = 0;          /* 0 up, 1 asked, 2 held minimized, 3 restore asked */
static f64 g_winmin_at = 0.0;
static u64 g_winmin_frames[8];
static bool g_winmin_confirmed[8];

static bool winmin_populated(void) {
    i32 have = g_host->window_count();
    if (have < g_windows_want) return false;
    for (i32 w = 0; w < have; w++) {
        AppState *a = g_host->window_state(w);
        if (!a || a->tab_count == 0) return false;
        if (g_win_seeded_terminal[w] != a->tabs[a->active_tab].terminal) return false;
    }
    return true;
}

static void winmin_restore_all(void) {
    i32 have = g_host->window_count();
    for (i32 w = 1; w < have; w++) {
        AppState *a = g_host->window_state(w);
        if (a && a->host_window) platform_window_set_minimized(a->host_window, false);
    }
    g_winmin_state = 0;
    g_winmin_at = soak_probe_now();
}

static void op_windows_minimize(u64 op) {
    if (!winmin_populated()) { op_windows_steady(op); return; }
    f64 now = soak_probe_now();
    i32 have = g_host->window_count();
    if (g_winmin_state == 0) {                        /* ask for minimize */
        /* Warm-up epochs stay in phase 0 and do not visit quiescing(). The
         * restore at their boundary must finish before a new minimize request. */
        if (now-g_winmin_at<WINMIN_SETTLE_S) return;
        for (i32 w = 1; w < have && w < 8; w++) {
            AppState *a = g_host->window_state(w);
            if (!a || !a->host_window) { g_c.errors++; continue; }
            platform_window_set_minimized(a->host_window, true);
            g_c.min_requested++;
        }
        g_winmin_state = 1; g_winmin_at = now;
        return;
    }
    if (now - g_winmin_at < WINMIN_SETTLE_S) return;
    if (g_winmin_state == 1) {                        /* confirm; start counting */
        for (i32 w = 1; w < have && w < 8; w++) {
            AppState *a = g_host->window_state(w);
            if (!a || !a->host_window) continue;
            g_winmin_confirmed[w]=platform_window_is_minimized(a->host_window);
            if (g_winmin_confirmed[w]) g_c.min_confirmed++;
            else {
                g_c.min_unconfirmed++;
                fprintf(stderr,"soak-winmin: minimize not confirmed window=%d epoch=%d time=%.3f visible=%d\n",
                        w,g_epoch_n,now,(i32)platform_window_visible(a->host_window));
            }
            /* From here on the Dock animation is over: whatever this window
             * paints until the restore is steady-state minimized painting. */
            g_winmin_frames[w] = a->frame_seq;
        }
        g_winmin_state = 2; g_winmin_at = now;
        return;
    }
    if (g_winmin_state == 2) {                        /* count; restore */
        for (i32 w = 1; w < have && w < 8; w++) {
            AppState *a = g_host->window_state(w);
            if (!a || !a->host_window) continue;
            if(g_winmin_confirmed[w]) g_c.min_frames_while_min += a->frame_seq - g_winmin_frames[w];
            /* What the paint gate is told about a minimized window. */
            if (g_winmin_confirmed[w] && platform_window_visible(a->host_window)) g_c.min_reported_visible++;
            platform_window_set_minimized(a->host_window, false);
        }
        g_winmin_state = 3; g_winmin_at = now;
        return;
    }
    for (i32 w = 1; w < have && w < 8; w++) {         /* confirm restored */
        AppState *a = g_host->window_state(w);
        if (!a || !a->host_window) continue;
        if (!platform_window_is_minimized(a->host_window)) g_c.restore_confirmed++;
        else {
            g_c.min_unconfirmed++;
            fprintf(stderr,"soak-winmin: restore not confirmed window=%d epoch=%d time=%.3f\n",w,g_epoch_n,now);
        }
    }
    g_winmin_state = 0;
}

/* The cross-product: multiple real AppStates, each holding several real PTYs.
 * Keep every owner alive across epochs so the memory series measures a steady
 * population. One operation creates or seeds at most one tab. */
static void op_window_tab_grid(u64 op) {
    i32 have = g_host->window_count();
    if (have < g_grid_windows) {
        if (g_host->window_open() >= 0) g_c.windows_opened++;
        else g_c.errors++;
        return;
    }
    u64 population = 0, frames = 0;
    for (i32 w = 0; w < have && w < g_grid_windows; w++) {
        AppState *app = g_host->window_state(w);
        if (!app) { g_c.errors++; return; }
        if (app->tab_count < g_grid_tabs) {
            Session *s = session_create_local(app->grid_cols > 0 ? app->grid_cols : 80,
                                              app->grid_rows > 0 ? app->grid_rows : 24);
            if (s && session_status(s) == SESSION_CONNECTED) {
                i32 slot = app_new_tab(app, s, "soak-grid");
                if (slot >= 0) { g_c.tabs_opened++; g_grid_seeded_terminal[w][slot] = NULL; }
                else { session_destroy(s); g_c.errors++; }
            } else { if (s) session_destroy(s); g_c.errors++; }
            return;
        }
        for (i32 ti = 0; ti < app->tab_count; ti++) {
            Terminal *t = app->tabs[ti].terminal;
            if (!t || g_grid_seeded_terminal[w][ti] == t) continue;
            char line[256];
            i32 n = snprintf(line, sizeof line,
                             "\033[3%dmgrid window %d tab %02d \033[0m\r\n",
                             1 + (ti % 6), w, ti);
            if (n > 0) terminal_feed(t, (const u8 *)line, (usize)n);
            g_grid_seeded_terminal[w][ti] = t;
            return;
        }
        population += (u64)app->tab_count;
        frames += (u64)app->frame_seq;
    }
    if ((op % 64) == 0) {
        g_c.grid_steady_frames = frames;
        if (population > g_c.grid_steady_peak) g_c.grid_steady_peak = population;
    }
}

/* ---- ONE WINDOW STREAMING, THE REST VISIBLE AND IDLE. ---------------------
 *
 * What does a window that is not changing cost while a SIBLING streams?
 * `wins` fills every window once and then holds, so every window is equally
 * idle and the loop's shared `needs_redraw` never has anything to over-apply.
 * Here window 0 is fed continuously and the others are left exactly as they
 * were filled.
 *
 * main.c's redraw flag is loop-wide on purpose, and the caret/toast/voice gate
 * is ORed across windows for the same reason, so this scenario does not assume
 * a defect. It measures per-window frame counts, and whether an idle window's
 * count tracks the streaming one.
 * ------------------------------------------------------------------------- */
static void op_windows_flood(u64 op) {
    i32 have = g_host->window_count();
    if (have < g_windows_want) {
        if (g_host->window_open() >= 0) {
            g_c.windows_opened++;
            g_win_seeded_terminal[have] = NULL;
        }
        else g_c.errors++;
        return;
    }

    /* Same populated steady state `wins` measures, so the two are comparable. */
    for (i32 w = 0; w < have; w++) {
        AppState *a = g_host->window_state(w);
        if (!a || a->tab_count == 0) continue;
        Terminal *t = a->tabs[a->active_tab].terminal;
        if (!t || g_win_seeded_terminal[w] == t) continue;
        char row[400];
        for (i32 i = 0; i < t->rows; i++) {
            i32 n = snprintf(row, sizeof row,
                             "\033[3%dmwin %d row %03d \033[4m\346\227\245\346\234\254\033[0m ",
                             1 + (i % 6), w, i);
            while (n < (i32)sizeof row - 8 && n < t->cols) {
                char c = (char)('a' + ((i + n) % 26));
                row[n++] = c;
            }
            n += snprintf(row + n, sizeof row - (usize)n, "\r\n");
            terminal_feed(t, (const u8 *)row, (usize)n);
        }
        g_win_seeded_terminal[w] = t;
        return;                       /* one window filled per operation */
    }

    /* Baseline the frame counters ONCE, after the fill — counting from window
     * creation would price the fill itself, which is not the question. */
    if (g_c.win_frames_n == 0) {
        for (i32 w = 0; w < have && w < 8; w++) {
            AppState *a = g_host->window_state(w);
            g_c.win_frames[w] = a ? (u64)a->frame_seq : 0;
        }
        g_c.win_frames_n = have < 8 ? have : 8;
        return;
    }

    /* From here on ONLY window 0 changes. */
    AppState *a0 = g_host->window_state(0);
    if (!a0 || a0->tab_count == 0) return;
    Terminal *t0 = a0->tabs[a0->active_tab].terminal;
    if (!t0) return;
    char row[256];
    for (i32 i = 0; i < 8; i++) {
        i32 n = snprintf(row, sizeof row,
                         "\033[3%dmflood %06llu.%d \033[0m", 1 + (i % 6),
                         (unsigned long long)op, i);
        while (n < (i32)sizeof row - 4 && n < t0->cols) {
            char c = (char)('a' + ((i + n) % 26));
            row[n++] = c;
        }
        n += snprintf(row + n, sizeof row - (usize)n, "\r\n");
        terminal_feed(t0, (const u8 *)row, (usize)n);
        g_c.win_flood_bytes += (u64)n;
    }

    /* Report as it goes, not only in the summary: this scenario exists to be
     * READ, and a run that has to be stopped early would otherwise produce
     * nothing at all. */
    if (op % 1024 == 0) {
        u64 f0 = 0;
        printf("  [winflood op %llu] frames since fill:", (unsigned long long)op);
        for (i32 w = 0; w < g_c.win_frames_n; w++) {
            AppState *a = g_host->window_state(w);
            u64 now = a ? (u64)a->frame_seq : 0;
            u64 got = now > g_c.win_frames[w] ? now - g_c.win_frames[w] : 0;
            if (w == 0) f0 = got;
            printf("  w%d=%llu%s", w, (unsigned long long)got,
                   (w && f0) ? "" : "");
            if (w && f0) printf("(%.0f%%)", 100.0 * (double)got / (double)f0);
        }
        printf("\n");
        fflush(stdout);
    }
}

/* Animate the first window while other visible windows remain idle. Restart
 * the animation when it drains so it stays active throughout the sampling
 * interval. */
static void op_windows_anim(u64 op) {
    i32 have = g_host->window_count();
    if (have < g_windows_want) {
        if (g_host->window_open() >= 0) {
            g_c.windows_opened++;
            g_win_seeded_terminal[have] = NULL;
        }
        else g_c.errors++;
        return;
    }

    /* The same populated steady state winflood measures. */
    for (i32 w = 0; w < have; w++) {
        AppState *a = g_host->window_state(w);
        if (!a || a->tab_count == 0) continue;
        Terminal *t = a->tabs[a->active_tab].terminal;
        if (!t || g_win_seeded_terminal[w] == t) continue;
        char row[400];
        for (i32 i = 0; i < t->rows; i++) {
            i32 n = snprintf(row, sizeof row,
                             "\033[3%dmwin %d row %03d \033[4m\346\227\245\346\234\254\033[0m ",
                             1 + (i % 6), w, i);
            while (n < (i32)sizeof row - 8 && n < t->cols) {
                char c = (char)('a' + ((i + n) % 26));
                row[n++] = c;
            }
            n += snprintf(row + n, sizeof row - (usize)n, "\r\n");
            terminal_feed(t, (const u8 *)row, (usize)n);
        }
        g_win_seeded_terminal[w] = t;
        return;                       /* one window filled per operation */
    }

    if (g_c.win_frames_n == 0) {
        for (i32 w = 0; w < have && w < 8; w++) {
            AppState *a = g_host->window_state(w);
            g_c.win_frames[w] = a ? (u64)a->frame_seq : 0;
        }
        g_c.win_frames_n = have < 8 ? have : 8;
        return;
    }

    /* Keep the animation deadline active throughout the sampling interval
     * by restarting it after each completed cycle. */
    AppState *a0 = g_host->window_state(0);
    if (!a0) return;
    static Anim s_win_anim;
    if (anim_done(&s_win_anim)) {
        /* Scope first: this is the window the animation belongs to, and it is
         * what tells a per-window deadline whose frame is owed. The
         * process-wide deadline ignores it, so the same scenario exercises
         * both. */
        anim_set_hover_scope(a0);
        anim_start(&s_win_anim, 0.25);
        g_c.win_anim_starts++;
    }

    if (op % 1024 == 0) {
        u64 f0 = 0;
        printf("  [winanim op %llu] frames since fill:", (unsigned long long)op);
        for (i32 w = 0; w < g_c.win_frames_n; w++) {
            AppState *a = g_host->window_state(w);
            u64 now = a ? (u64)a->frame_seq : 0;
            u64 got = now > g_c.win_frames[w] ? now - g_c.win_frames[w] : 0;
            if (w == 0) f0 = got;
            printf("  w%d=%llu", w, (unsigned long long)got);
            if (w && f0) printf("(%.0f%%)", 100.0 * (double)got / (double)f0);
        }
        printf("\n");
        fflush(stdout);
    }
}

/* ---- MARKDOWN AND THE EDITOR. --------------------------------------------
 *
 * Both drive a FILEBROWSER TAB rather than the docked sidebar, and that choice
 * is the whole hazard. `app_sidebar_sync()` runs every frame and MOVES
 * `app->filebrowser` between AppState and whichever tab owns it, zeroing the
 * side that is not holding it — so a driver that opens a document into the
 * docked browser while ownership sits elsewhere has its work stowed on the
 * very next frame, with `fb_open_file` having provably succeeded and the
 * document simply not there. `app_new_filebrowser_tab` gives the tab its own
 * `tab->fb`, which app_sidebar_sync never touches.
 *
 * The fixture is written once into the isolated TMPDIR: a large document with
 * tables, fenced code and image references, plus smaller ones to navigate
 * between. Real parser, real renderer, real editor, real undo — synthetic
 * bytes. */
static void md_write_fixture(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(g_md_dir, sizeof g_md_dir, "%s/liu-soak-md", tmp);
    mkdir(g_md_dir, 0700);

    for (i32 f = 0; f < 6; f++) {
        char path[640];
        snprintf(path, sizeof path, "%s/doc%02d.md", g_md_dir, f);
        FILE *fp = fopen(path, "w");
        if (!fp) continue;
        /* Large enough that layout is real work, and containing every block
         * kind the renderer has a separate path for. */
        fprintf(fp, "# Document %d\n\nA paragraph with [a link](doc%02d.md) and "
                    "`inline code`.\n\n", f, (f + 1) % 6);
        fprintf(fp, "| column | value | note |\n|---|---|---|\n");
        for (i32 i = 0; i < 40; i++)
            fprintf(fp, "| row %03d | %d | \346\227\245\346\234\254\350\252\236 |\n", i, i * f);
        fprintf(fp, "\n```c\n");
        for (i32 i = 0; i < 120; i++)
            fprintf(fp, "static int fn_%03d(int a) { return a * %d; }\n", i, i + 1);
        fprintf(fp, "```\n\n![an image](img%02d.png)\n\n![[embedded]]\n\n", f);
        for (i32 i = 0; i < 400; i++)
            fprintf(fp, "Paragraph %03d with enough words to make wrapping and "
                        "inline layout do real work on every reflow.\n\n", i);
        fclose(fp);
        g_md_files++;
    }
    char embed_path[640];
    snprintf(embed_path, sizeof embed_path, "%s/embedded.md", g_md_dir);
    FILE *embed = fopen(embed_path, "w");
    if (embed) { fputs("# Embedded note\n\nShared async fixture content.\n", embed); fclose(embed); }
}

static FileBrowser *md_tab_fb(AppState *app) {
    if (!app || app->tab_count == 0) return NULL;
    Tab *t = &app->tabs[app->active_tab];
    return (t->kind == TAB_FILEBROWSER) ? t->fb : NULL;
}

static void op_markdown(u64 op) {
    AppState *app = g_host->window_state(0);
    if (!app) return;
    if (!g_md_files) md_write_fixture();

    FileBrowser *fb = md_tab_fb(app);
    if (!fb) {
        i32 slot = app_new_filebrowser_tab(app, g_md_dir);
        if (slot >= 0) { g_c.tabs_opened++; app_switch_tab(app, slot); }
        else g_c.errors++;
        return;
    }

    switch (op % 5) {
    case 0: case 1: {                          /* open a document */
        char path[640];
        snprintf(path, sizeof path, "%s/doc%02llu.md", g_md_dir,
                 (unsigned long long)(op % 6));
        if (fb_open_md_path(fb, path)) {
            g_c.md_opens++;
            g_c.md_blocks += md_block_count(fb->md_doc);
        }
        break;
    }
    case 2:                                    /* close and re-open: teardown */
        fb_close_viewer(fb);
        g_c.md_closes++;
        break;
    case 3:                                    /* navigate the listing */
        if (fb_navigate(fb, g_md_dir)) g_c.md_navigations++;
        break;
    default:
        break;                                 /* a frame with the doc up */
    }
}

static void op_editor(u64 op) {
    AppState *app = g_host->window_state(0);
    if (!app) return;
    if (!g_md_files) md_write_fixture();

    FileBrowser *fb = md_tab_fb(app);
    if (!fb) {
        i32 slot = app_new_filebrowser_tab(app, g_md_dir);
        if (slot >= 0) { g_c.tabs_opened++; app_switch_tab(app, slot); }
        else g_c.errors++;
        return;
    }
    if (!fb->md_doc && !fb->view_content) {
        char path[640];
        snprintf(path, sizeof path, "%s/doc%02llu.md", g_md_dir,
                 (unsigned long long)(op % 6));
        if (fb_open_md_path(fb, path)) g_c.ed_opens++;
        return;
    }

    switch (op % 8) {
    case 0:
        fb_md_toggle_edit(fb);                 /* viewer <-> editor */
        break;
    case 1: case 2: case 3:                    /* real edits, real undo frames */
        fb_editor_insert_char(fb, (u32)('a' + (op % 26)));
        g_c.ed_edits++;
        break;
    case 4:
        fb_editor_newline(fb);
        g_c.ed_edits++;
        break;
    case 5:
        fb_editor_backspace(fb);
        g_c.ed_edits++;
        break;
    case 6:
        fb_editor_undo(fb);                    /* undo history growth */
        g_c.ed_undos++;
        break;
    default: {                                 /* find over the document */
        fb_find_open(fb, false);
        snprintf(fb->find_query, sizeof fb->find_query, "Paragraph %03d",
                 (i32)(op % 400));
        fb->find_query_len = (u32)strlen(fb->find_query);
        fb_find_recompute(fb);
        g_c.ed_finds++;
        break;
    }
    }
}

/* Exercise session, parser, agent-detection, activity, and tab aggregation
 * across 32 or 64 panes. Input is synthetic terminal data, so no agent CLI
 * needs to be installed or launched. */
static const char *const kAgentNames[] = {
    "claude", "codex", "opencode", "grok", "cline",
    "agy", "cursor-agent", "amp", "kimi", "qwen", "commandcode",
};

static void agent_producer_cmd(char *out, usize cap, i32 idx) {
    /* Four phases per cycle, offset per pane: a token burst, a tool-output
     * line, a spinner carrying its interrupt hint (what agent_activity.c uses
     * to tell a status line from an action line), then an idle prompt. The
     * markers are the ones the real CLIs draw.
     *
     * The shape is `sh -c '<script>' <name>`. Detection walks KERN_PROCARGS2
     * and matches the BASENAME of any argv entry, so the agent's name has to
     * be an ARGUMENT — typing the loop straight into the interactive shell
     * leaves argv as just the shell and detection matches nothing. The script
     * is single-quoted so its own printf strings can use double quotes
     * without escaping. */
    const char *name = kAgentNames[(usize)idx % (sizeof kAgentNames / sizeof kAgentNames[0])];
    snprintf(out, cap,
        "exec sh -c 'i=%d; while :; do "
          "j=0; while [ $j -lt 12 ]; do printf \"tok %%03d-%%06d\\n\" %d $i; "
            "i=$((i+1)); j=$((j+1)); done; "
          "printf \"\\342\\217\\272 Read src/pane%d/file_%%06d.c\\n\" $i; "
          "printf \"\\342\\234\\273 Thinking... (esc to interrupt)\\n\"; "
          "sleep 0.%d; printf \"> \\n\"; sleep 0.%d; "
        "done' %s\r",
        idx * 7, idx, idx, 5 + (idx % 7), 3 + (idx % 5), name);
}

static void op_agents(u64 op) {
    AppState *app = g_host->window_state(0);
    if (!app) return;

    /* BUILD: one tab or one split per operation. 64 panes is 8 tabs of 8. */
    i32 live = 0;
    for (i32 t = 0; t < app->tab_count; t++) live += tab_split_pane_count(&app->tabs[t]);
    if (live < g_agents_want) {
        i32 ti = app->active_tab;
        if (ti >= 0 && ti < app->tab_count &&
            tab_split_pane_count(&app->tabs[ti]) < MAX_SPLIT_PANES) {
            i32 before = tab_split_pane_count(&app->tabs[ti]);
            soak_split_largest(app);
            if (tab_split_pane_count(&app->tabs[ti]) > before) g_c.splits_made++;
        } else if (app->tab_count < MAX_TABS) {
            Session *ns = session_create_local(app->grid_cols > 0 ? app->grid_cols : 80,
                                               app->grid_rows > 0 ? app->grid_rows : 24);
            if (ns && session_status(ns) == SESSION_CONNECTED) {
                i32 slot = app_new_tab(app, ns, "agents");
                if (slot >= 0) { g_c.tabs_opened++; app_switch_tab(app, slot); }
                else { session_destroy(ns); g_c.errors++; }
            } else if (ns) session_destroy(ns);
        }
        return;
    }

    /* ARM: type one producer per pane, one pane per operation so the forkpty
     * storm is spread over the loop rather than blocking a single frame. */
    if (g_agents_armed < live) {
        i32 want = g_agents_armed, seen = 0;
        for (i32 t = 0; t < app->tab_count; t++) {
            Tab *tab = &app->tabs[t];
            i32 np = tab_split_pane_count(tab);
            for (i32 p = 0; p < np; p++, seen++) {
                if (seen != want) continue;
                Session *ps = (p == 0) ? tab->session
                            : (p == 1) ? tab->session2 : tab->extra_sessions[p - 2];
                if (ps) {
                    char cmd[512];
                    agent_producer_cmd(cmd, sizeof cmd, seen);
                    session_write(ps, (const u8 *)cmd, (i32)strlen(cmd));
                    g_c.agent_panes++;
                }
                g_agents_armed++;
                return;
            }
        }
        g_agents_armed = live;         /* nothing left to arm */
        return;
    }

    /* MEASURE. The app's own tick already runs app_poll_sessions, which is
     * what drives tab_update_pane_agents — calling it here would double-drain
     * an already-spent poll budget and corrupt the very policy under test. So
     * this only READS what the production pass produced. */
    for (i32 t = 0; t < app->tab_count; t++) {
        Tab *tab = &app->tabs[t];
        i32 np = tab_split_pane_count(tab);
        for (i32 p = 0; p < np; p++) {
            const PaneAgent *pa = &tab->pane_agents[p];
            if (pa->tool != 0) g_c.agent_detected++;   /* ChatTool as i32; 0 = none */
            if (pa->activity[0])            g_c.agent_activity++;
        }
    }
    /* Focus moves so foreground and background probe cadences both run (a
     * background pane probes at 2.0 s, a foreground one at 0.5 s). */
    if ((op % 23) == 0 && app->tab_count > 1)
        app_switch_tab(app, (i32)srand_below((u32)app->tab_count));

    u64 sweeps = 0; f64 secs = 0.0;
    session_proc_scan_stats(&sweeps, &secs);
    if (sweeps > g_scan_sweeps0) g_c.agent_scan_sweeps = sweeps - g_scan_sweeps0;
}

/* ---- SITES. Real child processes, through the production manager. -------
 *
 * app->site_mgr is the window's own SiteManager; sites_add / site_start /
 * site_stop / sites_remove are what the Sites panel calls. The app's own tick
 * already drives sites_poll, so the drain is the product's, not the
 * scenario's. */
static void op_sites(u64 op) {
    AppState *app = g_host->window_state(0);
    if (!app) return;
    SiteManager *m = &app->site_mgr;

    i32 have = m->count;
    if (have < g_sites_want) {
        char name[64], cmd[256];
        snprintf(name, sizeof name, "soak-site-%d", have);
        /* A mix by index: idle, continuous, and bursty — the three shapes the
         * Sites poll budget has to hold apart. */
        switch (have % 3) {
        case 0: snprintf(cmd, sizeof cmd, "exec sleep 3600"); break;
        case 1: snprintf(cmd, sizeof cmd,
                         "i=0; while :; do echo site-%d-$i; i=$((i+1)); done", have); break;
        default: snprintf(cmd, sizeof cmd,
                          "i=0; while :; do j=0; while [ $j -lt 40 ]; do "
                          "echo site-%d-$i; i=$((i+1)); j=$((j+1)); done; "
                          "sleep 0.2; done", have); break;
        }
        if (sites_add(m, name, "/tmp", cmd)) g_c.sites_added++;
        else g_c.errors++;
        return;
    }

    /* LIVENESS: what the running Sites have actually parsed since last time.
     * Generation-aware, because site_start replaces a Site's log Terminal and
     * the replacement's counter restarts at zero — comparing against the
     * destroyed Terminal's lifetime total would freeze this at its old
     * high-water mark. A drop means a new generation, not negative work. */
    {
        static u64 last_site_writes = 0;
        u64 now = 0;
        for (i32 i = 0; i < m->count; i++) {
            Site *st = sites_get(m, i);
            if (st && st->term) now += (u64)st->term->content_writes;
        }
        if (now < last_site_writes) last_site_writes = now;
        if (now > last_site_writes) g_c.site_log_writes += now - last_site_writes;
        last_site_writes = now;
    }

    /* Restart waves, stops and removals, so the retained log Terminal and the
     * exit path are both exercised rather than only the happy start. */
    if (have <= 0) return;
    i32 idx = (i32)(op % (u64)have);
    Site *s = sites_get(m, idx);
    if (!s) return;
    switch (op % 7) {
    case 3:
        if (s->status == SITE_RUNNING) { site_stop(s); g_c.sites_stopped++; }
        break;
    case 5:
        if (s->status != SITE_RUNNING && site_start(m, s)) g_c.sites_started++;
        break;
    default:
        if (s->status != SITE_RUNNING && site_start(m, s)) g_c.sites_started++;
        break;
    }
}


static AppState *pick_window(i32 *out_idx) {
    i32 n = g_host->window_count();
    if (n <= 0) { if (out_idx) *out_idx = -1; return NULL; }
    i32 i = (i32)srand_below((u32)n);
    if (out_idx) *out_idx = i;
    return g_host->window_state(i);
}

/* WINDOW WAVES: 1 -> 8 -> 1. Every window brings its own AppState, Renderer,
 * font atlas and Metal layer, so this is the largest ownership transition the
 * process has. The last window is never closed here — closing it is a QUIT,
 * and the process shutdown path is a different scenario (it is what atexit
 * covers, and it can only be exercised once). */
static void op_windows(u64 op) {
    i32 n = g_host->window_count();
    /* A triangle wave over the op index, so an epoch that is a whole number of
     * cycles starts and ends at the same window count. */
    i32 want = 1 + (i32)((op / 3) % 8);
    if (want > 8) want = 8;
    if (n < want) {
        if (g_host->window_open() >= 0) g_c.windows_opened++;
        else g_c.errors++;
    } else if (n > want && n > 1) {
        g_host->window_close(n - 1);
        g_c.windows_closed++;
    }
}

/* TABS AND SPLIT PANES inside whichever window comes up. Both are the real
 * functions: app_new_tab is what Cmd+T reaches and app_split_tab is Cmd+D. */
static void op_tabs_and_panes(void) {
    AppState *app = pick_window(NULL);
    if (!app) return;

    switch (srand_below(7)) {
    case 6: {                    /* give a pane's prompt an attachment block */
        /* The production attach, onto the production PaneAgent: this is what
         * puts a heap block into a slot the close/split/detach code then
         * MOVES or DROPS (prompt_capture.h). The final verdict asserts every
         * block is gone once the tabs are. */
        if (app->tab_count > 0) {
            Tab *t = &app->tabs[app->active_tab];
            i32 panes = tab_split_pane_count(t);
            PaneAgent *pa = &t->pane_agents[srand_below((u32)(panes > 0 ? panes : 1))];
            char path[64];
            snprintf(path, sizeof path, "/tmp/soak-%llu.png", (unsigned long long)g_ops);
            if (prompt_recall_attach(&pa->prompt, path)) g_c.prompts_attached++;
            pa->prompt.open = false;
        }
        break;
    }
    case 0: case 1: {            /* open a tab */
        if (app->tab_count < 8) {
            Session *s = session_create_local(app->grid_cols > 0 ? app->grid_cols : 80,
                                              app->grid_rows > 0 ? app->grid_rows : 24);
            if (s && session_status(s) == SESSION_CONNECTED) {
                if (app_new_tab(app, s, "soak") >= 0) g_c.tabs_opened++;
                else { session_destroy(s); g_c.errors++; }
            } else if (s) { session_destroy(s); }
        }
        break;
    }
    case 2:                      /* split the active tab */
        if (app->tab_count > 0) {
            i32 before = tab_split_pane_count(&app->tabs[app->active_tab]);
            app_split_tab(app, srand_below(2) ? SPLIT_H : SPLIT_V);
            if (tab_split_pane_count(&app->tabs[app->active_tab]) > before)
                g_c.splits_made++;
        }
        break;
    case 3:                      /* close ONE pane of a split */
        if (app->tab_count > 0) {
            Tab *t = &app->tabs[app->active_tab];
            i32 panes = tab_split_pane_count(t);
            if (panes > 1) {
                app_close_split_pane(app, app->active_tab, (i32)srand_below((u32)panes));
                g_c.panes_closed++;
            }
        }
        break;
    case 4:                      /* close the whole tab, panes and all */
        if (app->tab_count > 1) {
            app_close_tab(app, (i32)srand_below((u32)app->tab_count));
            g_c.tabs_closed++;
        }
        break;
    default:                     /* switch focus, which moves render caches */
        if (app->tab_count > 0) app->active_tab = (i32)srand_below((u32)app->tab_count);
        break;
    }
}

/* SLEEP AND WAKE, through the production policy. app_sleep_inactive_tabs is
 * the idle scan; it refuses a tab with a job running, which is the point — the
 * scenario should exercise the refusal as often as the success. */
static void op_sleep_wake(void) {
    AppState *app = pick_window(NULL);
    if (!app) return;

    /* Sleeping needs tabs that are not the active one, so the scenario makes
     * them. app_soak_tick refuses to pass a scenario that performed no
     * production operation. */
    while (app->tab_count < 4) {
        Session *s = session_create_local(app->grid_cols > 0 ? app->grid_cols : 80,
                                          app->grid_rows > 0 ? app->grid_rows : 24);
        if (!s || session_status(s) != SESSION_CONNECTED) {
            if (s) session_destroy(s);
            break;
        }
        if (app_new_tab(app, s, "soak-sleep") < 0) { session_destroy(s); break; }
        g_c.tabs_opened++;
    }
    if (app->tab_count == 0) return;

    if (srand_below(2)) {
        i32 idx = (i32)srand_below((u32)app->tab_count);
        if (idx == app->active_tab) return;
        if (app->tabs[idx].sleeping) {
            if (app_wake_tab(app, idx)) g_c.wakes++; else g_c.errors++;
        } else {
            SleepBlock b = app_tab_sleep_block(app, idx, true);
            if (b != SLEEP_OK) { g_c.sleep_blocked++; return; }
            if (app_sleep_tab(app, idx)) g_c.sleeps++;
        }
    } else {
        /* The scan itself: every tab, with a backdated activity time so the
         * idle window has "elapsed". */
        for (i32 i = 0; i < app->tab_count; i++)
            app->tabs[i].last_activity_time = platform_time_sec() - 3600.0;
        f64 saved = app->config.tab_sleep_idle_minutes;
        app->config.tab_sleep_idle_minutes = 0.05f;
        g_host->sleep_scan(app, platform_time_sec());
        app->config.tab_sleep_idle_minutes = (f32)saved;
        /* Wake everything back up so the next epoch starts level. */
        for (i32 i = 0; i < app->tab_count; i++)
            if (app->tabs[i].sleeping && app_wake_tab(app, i)) g_c.wakes++;
    }
}

/* RENDERER, FONT AND IMAGE CACHES. Everything here is a real invalidation:
 * a font size change drops and rebuilds the atlas, a window resize re-derives
 * every render cache, and a DPI change does both. */
static void op_caches(void) {
    AppState *app = pick_window(NULL);
    if (!app) return;
    switch (srand_below(5)) {
    case 0: {                    /* font size: rebuilds the glyph atlas */
        f32 was = app->config.font_size;
        f32 now = 11.0f + (f32)srand_below(9);
        if (now != was) {
            app->config.font_size = now;
            if (g_host->font_reload) g_host->font_reload(app);
            g_c.font_invalidations++;
        }
        break;
    }
    case 1: {                    /* window resize: every render cache re-derives */
        i32 w = 700 + (i32)srand_below(900), h = 500 + (i32)srand_below(600);
        app_update_layout(app, w, h, (i32)(w * app->dpi_scale), (i32)(h * app->dpi_scale));
        g_c.resizes++;
        break;
    }
    case 2:                      /* DPI: both, and the sized-glyph path too */
        app->dpi_scale = (app->dpi_scale > 1.5f) ? 1.0f : 2.0f;
        if (g_host->font_reload) g_host->font_reload(app);
        g_c.dpi_changes++;
        break;
    case 3: {                    /* OSC 4: the per-terminal palette override */
        /* This drives the whole product: the parser writes the override
         * table, render_terminal_pane rebuilds THAT pane's LUT from the theme
         * plus the overrides, and the table is a lazy allocation that the
         * soak's own leak and exact-return checks then have to see released
         * with its terminal.
         *
         * In `caches` because a palette change is a render-cache
         * invalidation, which is what this scenario is about, and the same
         * operation list already moves the font size, the DPI and the window
         * size underneath it. Half the operations RESET, so the allocation is
         * exercised in both directions rather than saturating at 256 set
         * entries after the first epoch. */
        if (app->tab_count > 0 && app->tabs[app->active_tab].terminal) {
            Terminal *t = app->tabs[app->active_tab].terminal;
            char seq[64];
            u32 idx = srand_below(256);
            i32 n;
            if (srand_below(2)) {
                n = snprintf(seq, sizeof seq, "\033]4;%u;rgb:%02x/%02x/%02x\007",
                             idx, (u32)srand_below(256), (u32)srand_below(256),
                             (u32)srand_below(256));
            } else {
                n = snprintf(seq, sizeof seq, "\033]104;%u\007", idx);
            }
            terminal_feed(t, (const u8 *)seq, (usize)n);
            g_c.palette_writes++;
        }
        break;
    }
    default:                     /* feed the visible terminal, so the caches fill */
        if (app->tab_count > 0 && app->tabs[app->active_tab].terminal) {
            const char *msg = "\033[33msoak\033[0m 日本語 🇹🇷 ❤️ ▁▂▃▄▅ ⣿⣶ done\n";
            terminal_feed(app->tabs[app->active_tab].terminal,
                          (const u8 *)msg, strlen(msg));
        }
        break;
    }
}

/* STATIC REPLAY. What do N unchanged panes cost while an animation forces
 * frames? The answer needs a window whose panes are built once and then
 * touched by nothing, with frames still being produced — which is what this
 * scenario is: split to N panes, fill each once, then do NOTHING per operation
 * while main.c's soak branch keeps asking for a redraw.
 *
 * N is 1..8: only the active tab's panes are drawn, and MAX_SPLIT_PANES is 8,
 * so the sweep covers the range that exists.
 *
 * The scenario prints its own per-epoch summary; LIU_FRAME_PROBE's `rows` and
 * `inst` fields carry the same numbers per second.
 *
 * On a static screen nearly every row is replayed from the decoration cache
 * rather than rebuilt, and the replay is a small fraction of a frame, so a
 * frame-level damage gate would save little in exchange for a second
 * invalidation predicate that has to account for every non-grid surface in the
 * app. Liu already paints nothing when idle, so the frames such a gate would
 * skip are frames something else asked for. */
static i32 g_replay_panes = 1;

/* Accumulated across the epoch from AppState's per-frame render counters. Read
 * one iteration late — app_soak_tick runs before app_render — which costs one
 * frame out of thousands and keeps the counters where they belong. */
static struct {
    u64 frames, rows_rebuilt, rows_replayed;
    u64 inst_bg, inst_glyph, inst_deco;
    u64 rebuild_ns, replay_ns;
    i32 panes_seen;        /* the count while frames were being counted */
} g_replay;

static void replay_accumulate(AppState *app) {
    if (!app) return;
    /* Only when a frame was actually PAINTED. app_soak_tick runs every loop
     * iteration, and the paint gate skips an occluded or unfocused window — so
     * without this the accumulator would add the last painted frame's numbers
     * over and over and report a steady state that is not there.
     * (LIU_FRAME_PROBE_VIS=1 forces every window visible, which is how a probe
     * driven from another terminal gets frames at all.) */
    static u64 seen = 0;
    if (app->frame_seq == seen) return;
    seen = app->frame_seq;
    g_replay.frames++;
    if (app->tab_count > 0) {
        i32 p = tab_split_pane_count(&app->tabs[app->active_tab]);
        if (p > g_replay.panes_seen) g_replay.panes_seen = p;
    }
    g_replay.rows_rebuilt  += (u64)app->frame_rows_rebuilt;
    g_replay.rows_replayed += (u64)app->frame_rows_replayed;
    g_replay.inst_bg       += (u64)app->frame_bg_instances;
    g_replay.inst_glyph    += (u64)app->frame_glyph_instances;
    g_replay.inst_deco     += (u64)app->frame_deco_instances;
    g_replay.rebuild_ns    += app->frame_rebuild_ns;
    g_replay.replay_ns     += app->frame_replay_ns;
}

static void replay_report(void) {
    if (!g_replay.frames) return;
    f64 f = (f64)g_replay.frames;
    /* The count while the frames were being counted, not the count now: the
     * epoch's settle has already reduced the tab back to one pane by the time
     * this runs. */
    printf("  replay: %d pane(s) | %llu frames | rows %llu rebuilt / %llu replayed"
           " | per frame: %.1f rebuilt, %.1f replayed, %.0f glyph inst"
           " | CPU per frame: rebuild %.4f ms, replay %.4f ms\n",
           g_replay.panes_seen, (unsigned long long)g_replay.frames,
           (unsigned long long)g_replay.rows_rebuilt,
           (unsigned long long)g_replay.rows_replayed,
           (f64)g_replay.rows_rebuilt / f, (f64)g_replay.rows_replayed / f,
           (f64)g_replay.inst_glyph / f,
           (f64)g_replay.rebuild_ns / f / 1.0e6,
           (f64)g_replay.replay_ns  / f / 1.0e6);
    memset(&g_replay, 0, sizeof g_replay);
}

static i32 g_replay_filled = -1;   /* the pane count the content was written for */

static void op_static_replay(u64 op) {
    (void)op;
    AppState *app = g_host->window_state(0);
    if (!app || app->tab_count == 0) return;
    Tab *t = &app->tabs[app->active_tab];

    /* A blinking caret dirties a row every half second, which is a real
     * clock-driven repaint and NOT what this scenario is measuring. Turned off
     * for the duration so "unchanged pane" means unchanged. */
    app->config.cursor_blink = false;

    i32 have = tab_split_pane_count(t);
    if (have < g_replay_panes) {
        app_split_tab(app, (have % 2) ? SPLIT_H : SPLIT_V);
        if (tab_split_pane_count(t) > have) g_c.splits_made++;
        g_replay_filled = -1;          /* the new pane needs content */
        return;                        /* one split per operation */
    }

    if (g_replay_filled != g_replay_panes) {
        /* Fill every pane ONCE with a screenful of mixed content, so the
         * caches hold real instances rather than blanks — an empty grid
         * measures nothing. */
        for (i32 p = 0; p < tab_split_pane_count(t); p++) {
            Terminal *pt = (p == 0) ? t->terminal
                         : (p == 1) ? t->terminal2 : t->extra_terminals[p - 2];
            if (!pt) continue;
            for (i32 i = 0; i < pt->rows; i++) {
                char row[600];
                i32 n = snprintf(row, sizeof row,
                                 "\033[3%dmrow %03d \033[4m\346\227\245\346\234\254\350\252\236\033[0m ",
                                 1 + (i % 6), i);
                /* The index is read before it is bumped, in two statements:
                 * `row[n++] = f(n)` is unsequenced and the compiler says so. */
                while (n < (i32)sizeof row - 8 && n < pt->cols) {
                    char c = (char)('a' + ((i + n) % 26));
                    row[n++] = c;
                }
                n += snprintf(row + n, sizeof row - (usize)n, "\r\n");
                terminal_feed(pt, (const u8 *)row, (usize)n);
            }
        }
        g_replay_filled = g_replay_panes;
        memset(&g_replay, 0, sizeof g_replay);   /* the fill is not the measurement */
        return;
    }
    /* And then: nothing. That is the measurement. */
}

/* GLYPH STAGING. What does the path from a rasterised glyph to the GPU cost,
 * and can the staging boundary go?
 *
 * THE PATH, read from font.c: the rasteriser writes each glyph into a
 * per-glyph malloc'd `tmp`; renderer_metal_upload_texture copies `tmp` into
 * the atlas texture; the caller then copies `tmp` a THIRD time into
 * atlas->atlas_bitmap, the CPU mirror that the atlas-grow path reads back
 * (the alpha bitmap is the atlas's backing store, so realloc plus a row copy
 * preserves every packed glyph). Three touches of every glyph's pixels.
 *
 * WHAT THIS SCENARIO DRIVES, one phase per epoch, each feeding codepoints that
 * are NOT already in the atlas so every one is a miss:
 *   ascii   the ordinary case, one cell per glyph
 *   cjk     a two-cell box per glyph, so twice the bytes
 *   emoji   the COLOUR atlas, four bytes per pixel instead of one
 *   reload  a font size change: the whole atlas is dropped and rebuilt
 *   dpi     a scale change: the same, plus the sized-glyph path
 *
 * The counters come from renderer_metal_upload_stats — uploads, bytes and the
 * CPU nanoseconds inside replaceRegion — so what is reported is what the
 * process actually did, not a model of it.
 *
 * ASCII produces no uploads in a warm process, because the atlas pre-warms
 * that range. The per-upload sizes follow the box arithmetic: an emoji is a
 * two-cell box in the four-byte colour atlas, and a DPI change uploads the
 * whole single-byte alpha atlas.
 *
 * The staging boundary stays. Removing the CPU mirror would save one small
 * memcpy per miss, and the mirror is what the atlas-grow path reads: without
 * it, growing the atlas needs a GPU->CPU readback of the whole texture, the
 * cost the atlas's large starting size exists to avoid. A texture Metal may
 * still be reading from also cannot be the authoritative copy. */
/* The codepoint ranges each phase walks. Small enough that an epoch covers a
 * whole cycle several times over, because every epoch must do IDENTICAL work:
 * a range wider than one epoch's feed means each epoch touches a different set
 * of codepoints, the atlas fills at a different rate, and the memory series is
 * measuring the fixture. Wide enough that the atlas genuinely fills and
 * reclaims, which is the state the uploads are being counted in.
 *
 * NOTE what this scenario's memory series is and is not. It is a STRESS shape:
 * it feeds glyphs nobody has asked for as fast as it can, so the atlas is
 * always near its reclamation point. The numbers worth reading here are the
 * UPLOAD counters; the plateau verdict is a check that the stress shape is
 * itself bounded, not a claim about ordinary use. */
#define GLYPH_ASCII_RANGE 0x200
#define GLYPH_CJK_RANGE   0x600
#define GLYPH_EMOJI_RANGE 0x200

static u64 g_glyph_u0, g_glyph_b0, g_glyph_n0;
static const char *g_glyph_phase = "ascii";
static i32 g_glyph_cp = 0x0100;    /* walks forward so nothing is a cache hit */

static void glyph_phase_begin(const char *name) {
    g_glyph_phase = name;
    renderer_metal_upload_stats(&g_glyph_u0, &g_glyph_b0, &g_glyph_n0);
}

static void glyph_phase_report(u64 glyphs_fed) {
    u64 u = 0, b = 0, n = 0;
    renderer_metal_upload_stats(&u, &b, &n);
    u64 du = u - g_glyph_u0, db = b - g_glyph_b0, dn = n - g_glyph_n0;
    printf("  glyphs/%-6s fed %6llu | uploads %6llu | %8llu B (%.1f B/upload) "
           "| %.3f ms total, %.1f us/upload\n",
           g_glyph_phase, (unsigned long long)glyphs_fed,
           (unsigned long long)du, (unsigned long long)db,
           du ? (f64)db / (f64)du : 0.0,
           (f64)dn / 1.0e6, du ? (f64)dn / (f64)du / 1000.0 : 0.0);
}

static u64 g_glyph_fed = 0;

static void op_glyph_staging(u64 op) {
    AppState *app = g_host->window_state(0);
    if (!app || app->tab_count == 0) return;
    Terminal *t = app->tabs[app->active_tab].terminal;
    if (!t) return;

    char buf[512];
    i32 n = 0;
    if (strcmp(g_glyph_phase, "ascii") == 0) {
        /* Latin-1 and Latin Extended: one cell each, and far more of them than
         * an atlas holds after a reset, so every one is a real miss. */
        for (i32 i = 0; i < 24 && n < (i32)sizeof buf - 8; i++) {
            u32 cp = 0x0021 + (u32)((g_glyph_cp++) % GLYPH_ASCII_RANGE);
            if (cp < 0x80) buf[n++] = (char)cp;
            else { buf[n++] = (char)(0xC0 | (cp >> 6)); buf[n++] = (char)(0x80 | (cp & 0x3F)); }
            g_glyph_fed++;
        }
    } else if (strcmp(g_glyph_phase, "cjk") == 0) {
        for (i32 i = 0; i < 16 && n < (i32)sizeof buf - 8; i++) {
            u32 cp = 0x4E00 + (u32)((g_glyph_cp++) % GLYPH_CJK_RANGE);
            buf[n++] = (char)(0xE0 | (cp >> 12));
            buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[n++] = (char)(0x80 | (cp & 0x3F));
            g_glyph_fed++;
        }
    } else if (strcmp(g_glyph_phase, "emoji") == 0) {
        for (i32 i = 0; i < 8 && n < (i32)sizeof buf - 8; i++) {
            u32 cp = 0x1F300 + (u32)((g_glyph_cp++) % GLYPH_EMOJI_RANGE);
            buf[n++] = (char)(0xF0 | (cp >> 18));
            buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[n++] = (char)(0x80 | (cp & 0x3F));
            g_glyph_fed++;
        }
    } else if (strcmp(g_glyph_phase, "reload") == 0) {
        if ((op % 40) == 0) {
            app->config.font_size = 11.0f + (f32)((op / 40) % 9);
            if (g_host->font_reload) g_host->font_reload(app);
            g_c.font_invalidations++;
        }
        n = snprintf(buf, sizeof buf, "reload %llu ", (unsigned long long)op);
        g_glyph_fed += (u64)n;
    } else {                                   /* dpi */
        if ((op % 40) == 0) {
            app->dpi_scale = (app->dpi_scale > 1.5f) ? 1.0f : 2.0f;
            if (g_host->font_reload) g_host->font_reload(app);
            g_c.dpi_changes++;
        }
        n = snprintf(buf, sizeof buf, "dpi %llu ", (unsigned long long)op);
        g_glyph_fed += (u64)n;
    }
    if (n > 0 && n < (i32)sizeof buf - 2) { buf[n++] = '\r'; buf[n++] = '\n'; }
    if (n > 0) terminal_feed(t, (const u8 *)buf, (usize)n);
}

/* ---- the schedule ------------------------------------------------------- */

static void do_one_op_inner(u64 op) {
    const char *s = g_scenario;
    if (strcmp(s, "windows") == 0)      op_windows(op);
    else if (strcmp(s, "panes") == 0)   op_tabs_and_panes();
    else if (strcmp(s, "sleep") == 0)   op_sleep_wake();
    else if (strcmp(s, "caches") == 0)  op_caches();
    else if (strcmp(s, "replay") == 0)  op_static_replay(op);
    else if (strcmp(s, "glyphs") == 0)  op_glyph_staging(op);
    else if (strcmp(s, "idle") == 0)    op_idle(op);
    else if (strcmp(s, "typing") == 0)  op_typing(op);
    else if (strncmp(s, "stream", 6) == 0) op_stream(op);
    else if (strcmp(s, "scroll") == 0)  op_scroll(op);
    else if (strncmp(s, "sites", 5) == 0)  op_sites(op);
    else if (strncmp(s, "agents", 6) == 0) op_agents(op);
    else if (strcmp(s, "markdown") == 0) op_markdown(op);
    else if (strcmp(s, "editor") == 0)   op_editor(op);
    else if (strncmp(s, "winflood", 8) == 0) op_windows_flood(op);
    else if (strncmp(s, "winanim", 7) == 0)  op_windows_anim(op);
    else if (strncmp(s, "winmin", 6) == 0) op_windows_minimize(op);
    else if (strncmp(s, "wins", 4) == 0)  op_windows_steady(op);
    else if (strncmp(s, "tabs", 4) == 0)  op_tabs_steady(op);
    else if (strncmp(s, "grid", 4) == 0)  op_window_tab_grid(op);
    else if (strcmp(s, "images") == 0)    op_images(op);
    else if (strncmp(s, "browser", 7) == 0) op_browser(op);
    else {                              /* "all": every kind, interleaved */
        switch (op % 4) {
        case 0: op_windows(op / 4);  break;
        case 1: op_tabs_and_panes(); break;
        case 2: op_sleep_wake();     break;
        default: op_caches();        break;
        }
    }
    g_ops++;
}

/* Between epochs: back to one window with one tab, so every epoch sample
 * describes the same shape. */
/* The shape every epoch sample is taken in. It has to be a shape the SCENARIO
 * cannot have changed, or the exact-return check compares two different
 * processes: the caches scenario moves the font size, the DPI and the window
 * size, and each of those changes the grid. Every knob the operations touch
 * is restored here, and the surviving terminal is given a clean grid. */
enum { SOAK_BASE_W = 1200, SOAK_BASE_H = 800 };
static f32 g_base_font = 0.0f, g_base_dpi = 0.0f;

static void settle_to_baseline(void) {
    /* Every sample is taken with every window restored. */
    if (strncmp(g_scenario, "winmin", 6) == 0) winmin_restore_all();
    /* `winsN` keeps its windows; every other scenario comes back to one. The
     * floor is 1 because liu_window_close treats the last window as a QUIT —
     * it saves the session file and ends the loop. */
    i32 keep = g_windows_hold > 1 ? g_windows_hold : 1;
    while (g_host->window_count() > keep) {
        g_host->window_close(g_host->window_count() - 1);
        g_c.windows_closed++;   /* the settle closes most of them; counting only
                                 * op_windows' closes reported "70 open / 0
                                 * close" for a run that closed 69 */
    }
    AppState *app = g_host->window_state(0);
    if (g_tabs_want > 0 || g_grid_windows > 0) {
        /* Keep the tab population intact between samples so each interval
         * reflects steady-state cost. */
        if (app && app->tab_count > 0) app_switch_tab(app, 0);
        return;
    }
    if (app) {
        for (i32 i = 0; i < app->tab_count; i++)
            if (app->tabs[i].sleeping) app_wake_tab(app, i);
        while (app->tab_count > 1) app_close_tab(app, app->tab_count - 1);
        if (app->tab_count > 0) {
            Tab *t = &app->tabs[0];
            while (tab_split_pane_count(t) > 1)
                app_close_split_pane(app, 0, tab_split_pane_count(t) - 1);
        }
        if (g_base_font > 0.0f && app->config.font_size != g_base_font) {
            app->config.font_size = g_base_font;
            if (g_host->font_reload) g_host->font_reload(app);
        }
        if (g_base_dpi > 0.0f && app->dpi_scale != g_base_dpi) {
            app->dpi_scale = g_base_dpi;
            if (g_host->font_reload) g_host->font_reload(app);
        }
        app_update_layout(app, SOAK_BASE_W, SOAK_BASE_H,
                          (i32)(SOAK_BASE_W * app->dpi_scale),
                          (i32)(SOAK_BASE_H * app->dpi_scale));
        /* A FRESH tab, not a cleared one. ED 3 empties the scrollback's LINES
         * but terminal_scrollback_bytes counts the RING CAPACITY too, and that
         * ring is grow-only on a live Terminal, so a surviving terminal's ring
         * grows toward its limit across a run that leaks nothing. A brand-new
         * Terminal has a brand-new ring, so every epoch sample describes the
         * same object.
         *
         * Opened BEFORE the old one is closed: closing the last tab of the
         * last window is a quit, and this must never trigger one. */
        /* THE FRESH TAB, in EVERY scenario including windows: a live
         * Terminal's scrollback ring is grow-only, so a cleared tab is not the
         * same object twice. In the windows scenario many AppStates are opened
         * and closed while one shell has to survive, which also checks that
         * app_destroy_window never closes fd 0. */
        if (app->tab_count > 0) {
            Session *fresh = session_create_local(app->grid_cols > 0 ? app->grid_cols : 80,
                                                  app->grid_rows > 0 ? app->grid_rows : 24);
            /* A failure here must not be silent: this is the step that gives
             * every epoch the same object to measure, so a settle that cannot
             * make its shell leaves the scenario comparing a terminal against
             * nothing, and the only other symptom is an exact-return mismatch
             * (`owned child processes 1 -> 0`) with the reason nowhere in the
             * output. */
            i32 slot = -1;
            bool connected = fresh && session_status(fresh) == SESSION_CONNECTED;
            if (connected) slot = app_new_tab(app, fresh, "soak-base");
            if (connected && slot >= 0) {
                g_win_seeded_terminal[0] = NULL;
                /* Close the OTHERS, identified by position relative to the new
                 * tab — not `app_close_tab(app, 0)` in a loop. app_new_tab
                 * returns the slot it used and that slot is not always the last
                 * one, so closing index 0 repeatedly can close the tab this
                 * function just made and leave one of the old ones. Indices
                 * shift as tabs close, so this walks DOWNWARD and skips the one
                 * slot to keep. */
                for (i32 ti = app->tab_count - 1; ti >= 0; ti--) {
                    if (ti == slot) continue;
                    app_close_tab(app, ti);
                    if (ti < slot) slot--;   /* the survivor shifted left */
                }
            } else {
                g_c.errors++;
                fprintf(stderr, "soak: settle could not create its baseline "
                        "shell (session=%s, status=%d, tab slot=%d, tabs=%d)\n",
                        fresh ? "made" : "NULL",
                        fresh ? (int)session_status(fresh) : -1,
                        (int)slot, (int)app->tab_count);
                if (fresh) session_destroy(fresh);
            }
        }
    }
    /* LIU_SOAK_PROBE=1 prints the owned children on the way out of every
     * settle. Sampling happens later and only reports COUNTS, so when the
     * baseline and the epochs disagree there is otherwise no way to see which
     * pid appeared or went — the same reason LIU_AGENT_PROBE exists. */
    if (g_probe_children) soak_probe_print_children("settle");

    /* The trash worker owns the heavy half of a tab close; a sample taken
     * while it still holds a Terminal is measuring the queue, not a leak. */
    f64 t0 = soak_probe_now();
    for (;;) {
        i32 depth = 0;
        tab_trash_stats(&depth, NULL, NULL);
        if (depth == 0 || soak_probe_now() - t0 > 20.0) break;
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    /* The agents scenario has to re-arm. `g_agents_armed` is a COUNT of panes
     * given a producer and the arm gate is `g_agents_armed < live`. This
     * function destroys the population, so without the reset the next epoch
     * rebuilds its 32 or 64 panes, finds the counter still reading the old
     * total, and arms NONE: every epoch after the first would measure idle
     * shells.
     *
     * The reachability check cannot catch that, because `g_c.agent_panes` is
     * cumulative and a counter that was true once stays true. `g_stream_armed`
     * follows the same rule for the streaming scenario. */
    g_agents_armed = 0;
    /* And so does `stream`, for a reason that only matters at ONE pane.
     *
     * op_stream resets g_stream_armed in its SPLIT branch — the branch that
     * runs while it is still building the population. At `stream8` that branch
     * runs after every settle (8 panes to rebuild), so the flag is cleared and
     * the producers are re-typed. At `stream1` the requested population is ONE
     * pane and the surviving tab already has one, so the split branch never
     * runs; the flag would stay true from the first epoch and the producer
     * would never be typed into the REPLACED Terminal.
     *
     * The flag is therefore reset HERE, next to the population it belongs to,
     * rather than in a branch that runs only for some sizes. */
    g_stream_armed = false;
}

/* WALL TIME SPENT MEASURING, kept apart from wall time spent running.
 *
 * take_sample is the instrument: a full VM-map walk, malloc_zone_statistics, a
 * process-tree walk, a multi-line reconciliation printed to a redirected
 * stdout, and a JSONL append. None of it is Liu doing its job, and all of it
 * runs INSIDE the frame — app_soak_tick is called from the main loop between
 * two of the frame probe's phase marks, so every millisecond of it would land
 * in the `tick` phase and therefore in `work` and `total`. Left there, its
 * per-epoch spike would own the p99 and the max of an idle loop, and the
 * measuring apparatus would be reported as Liu's latency tail.
 *
 * The accumulator is drained once per iteration by the loop, which subtracts
 * it from `tick` and reports it as its own phase. The same reasoning applies
 * to -nextDrawable and the in-flight fence, which block inside app_render and
 * are pulled out of `render` so that "render" means building the frame. */
static void take_sample(SoakSample *out, i32 epoch, const char *phase) {
    soak_probe_take(out);
    out->epoch = epoch;
    out->ops   = g_ops;
    out->errors = g_c.errors;
    soak_probe_print(g_scenario, out);
    if (g_jsonl) soak_probe_write_jsonl(g_jsonl, g_scenario, phase, out);
    if (g_probe_children && g_windows_want > 0) {
        for (i32 w = 0; w < g_host->window_count(); w++) {
            AppState *app = g_host->window_state(w);
            Terminal *t = app && app->tab_count ? app->tabs[app->active_tab].terminal : NULL;
            if (!t) continue;
            fprintf(stderr, "soak-grid phase=%s epoch=%d window=%d visible=%d frames=%llu cols=%d rows=%d "
                    "bytes=%zu palette=%d underline-cap=%d sync=%zu scratch=%d "
                    "grid-slack=%d alt-slack=%d\n",
                    phase, epoch, w,
                    (i32)platform_window_visible(g_host->window_handle(w)),
                    (unsigned long long)app->frame_seq,
                    t->cols, t->rows, terminal_grid_bytes(t),
                    t->palette != NULL, t->ul_map_cap, terminal_sync_arena_bytes(t),
                    t->sb_scratch_cap, t->grid_slack, t->alt_slack);
        }
    }
}

void app_soak_begin(const AppSoakHost *host) {
    g_host = host;
    (void)app_soak_active();
    g_started_at = soak_probe_now();

    const char *e = liu_dev_env("SOAK_SEED");
    g_seed = (e && *e) ? strtoull(e, NULL, 0) : 0x5EED5EED5EED5EEDull;
    g_rand = g_seed;

    bool full = false;
    { const char *m = liu_dev_env("SOAK_MODE"); full = m && (m[0] == 'f' || m[0] == 'F'); }
    g_epochs        = full ? 8 : 6;
    /* Use longer warm-up epochs to allow object pools to reach a steady
     * allocation state before sampling. */
    g_ops_per_epoch = full ? 300 : 96;
    /* FOUR by default. The window wave has to reach its peak of 8 concurrent
     * windows inside the warm-up, because two process-wide pools only reach
     * THEIR peak then:
     *
     *   - libdispatch's worker pool, which grows with window churn and then
     *     stays flat. The added threads are unnamed; Liu's named threads
     *     (liu-pty-reader, liu-tab-trash, the raster worker) never grow.
     *
     *   - SQLite's pending-close fd pool. Each window's AppState opens its own
     *     vault connection. On POSIX, closing ANY descriptor on a file drops
     *     every advisory lock the PROCESS holds on it, so SQLite refuses to
     *     close a connection's descriptor while another connection in the
     *     same process still has that file open — it parks it on the inode's
     *     unused list and reuses it. The soak's fd count therefore rises with
     *     PEAK window concurrency and stays there, as bare `vault.db` handles
     *     with no -wal/-shm beside them. That reads like a leak and is not
     *     one: closing the last connection returns every descriptor.
     *
     * Both are one-time costs that a longer warm-up contains, and the thread
     * pool's stacks (512 KiB each by default) are also what the window run's
     * apparent memory growth during warm-up is made of. */
    g_warmup        = 4;

    /* PER-SCENARIO, because the pools above are not what every scenario has to
     * get past. `panes` is the one that needs its own numbers.
     *
     * Its phys_footprint approaches an asymptote over thousands of operations
     * while malloc in-use and the reserved column stay flat. Nothing is
     * retained; the transient is pages of already-reserved arena becoming
     * resident as the tab and split populations reach their working set.
     *
     * A 6-epoch quick window cannot contain that, and a warm-up long enough to
     * would make quick mode far too long for one scenario. So the footprint is
     * declared to need 18 epochs: quick prints it INCONCLUSIVE and gates
     * in-use, the ledger and every exact-return resource, which is what a quick
     * run is for; full runs the length where it can be judged. */
    /* PER-SCENARIO PARAMETERS, read from the NAME so a run is fully described
     * by it: `stream8` is eight panes, `sites32` is thirty-two. */
    if (strncmp(g_scenario, "stream", 6) == 0) {
        g_stream_panes = atoi(g_scenario + 6);
        if (g_stream_panes < 1) g_stream_panes = 1;
        if (g_stream_panes > 8) g_stream_panes = 8;   /* MAX_SPLIT_PANES */
        /* Long epochs: the measurement is the loop draining producers, and
         * an epoch has to contain thousands of iterations for a tail to mean
         * anything. Operations are cheap here (the split is done after the
         * first few), so the count is high on purpose. */
        g_ops_per_epoch = full ? 4000 : 1500;
        g_warmup        = 2;
    } else if (strncmp(g_scenario, "sites", 5) == 0) {
        g_sites_want = atoi(g_scenario + 5);
        if (g_sites_want < 1)  g_sites_want = 1;
        if (g_sites_want > 32) g_sites_want = 32;
        g_ops_per_epoch = full ? 600 : 200;
        g_warmup        = 2;
    } else if (strcmp(g_scenario, "scroll") == 0) {
        { const char *v = liu_dev_env("SOAK_SCROLL_LINES"); if (v && *v) g_scroll_lines = atoi(v); }
        { const char *v = liu_dev_env("SOAK_SCROLL_COLS");  if (v && *v) g_scroll_cols  = atoi(v); }
        if (g_scroll_lines < 500)  g_scroll_lines = 500;
        if (g_scroll_cols  < 40)   g_scroll_cols  = 40;
        g_ops_per_epoch = full ? 900 : 300;
        g_warmup        = 3;
        /* Same declaration lifecycle, pty-scale and panes needed, for the same
         * reason: a reflow leaves bounded arena high-water, so a short window
         * watches its own maximum arrive and calls it a trend. */
        g_mem_epochs_min = 18;
    } else if (strcmp(g_scenario, "idle") == 0) {
        /* Few operations, many of them: an idle scenario that busy-loops is
         * not idle. The wait dominates and that is the point. */
        g_ops_per_epoch = full ? 400 : 200;
        g_warmup        = 1;
    } else if (strncmp(g_scenario, "agents", 6) == 0) {
        g_agents_want = atoi(g_scenario + 6);
        if (g_agents_want < 1)  g_agents_want = 1;
        if (g_agents_want > MAX_TABS * MAX_SPLIT_PANES) g_agents_want = MAX_TABS * MAX_SPLIT_PANES;
        /* Keep the sampling interval longer than pane construction so setup
         * work does not dominate the reported rate. */
        g_ops_per_epoch = full ? 4000 : 1500;
        g_warmup        = 2;
        g_mem_epochs_min = 18;
    } else if (strncmp(g_scenario, "winflood", 8) == 0) {
        g_windows_want = atoi(g_scenario + 8);
        if (g_windows_want < 1) g_windows_want = 1;
        if (g_windows_want > 8) g_windows_want = 8;   /* LIU_MAX_WINDOWS */
        g_windows_hold = g_windows_want;
        g_ops_per_epoch = full ? 1200 : 400;
        g_warmup        = 2;
        g_mem_epochs_min = 18;
    } else if (strncmp(g_scenario, "winanim", 7) == 0) {
        g_windows_want = atoi(g_scenario + 7);
        if (g_windows_want < 1) g_windows_want = 1;
        if (g_windows_want > 8) g_windows_want = 8;   /* LIU_MAX_WINDOWS */
        g_windows_hold = g_windows_want;
        g_ops_per_epoch = full ? 1200 : 400;
        g_warmup        = 2;
        g_mem_epochs_min = 18;
    } else if (strncmp(g_scenario, "winmin", 6) == 0) {
        g_windows_want = atoi(g_scenario + 6);
        if (g_windows_want < 2) g_windows_want = 2;   /* window 0 stays up */
        if (g_windows_want > 8) g_windows_want = 8;   /* LIU_MAX_WINDOWS */
        g_windows_hold = g_windows_want;
        g_ops_per_epoch = full ? 1200 : 400;
        g_warmup        = 2;
        g_mem_epochs_min = 18;
    } else if (strncmp(g_scenario, "wins", 4) == 0) {
        g_windows_want = atoi(g_scenario + 4);
        if (g_windows_want < 1) g_windows_want = 1;
        if (g_windows_want > 8) g_windows_want = 8;   /* LIU_MAX_WINDOWS */
        g_windows_hold = g_windows_want;
        g_ops_per_epoch = full ? 1200 : 400;
        g_warmup        = 2;
        g_mem_epochs_min = 18;
    } else if (strncmp(g_scenario, "tabs", 4) == 0) {
        g_tabs_want = atoi(g_scenario + 4);
        if (g_tabs_want < 1) g_tabs_want = 1;
        if (g_tabs_want > MAX_TABS) g_tabs_want = MAX_TABS;
        g_ops_per_epoch = full ? 1200 : 400;
        g_warmup = 2;
        g_mem_epochs_min = 18;
    } else if (strncmp(g_scenario, "grid", 4) == 0) {
        i32 w = 0, t = 0;
        if (sscanf(g_scenario, "grid%dx%d", &w, &t) == 2 &&
            w >= 1 && w <= 8 && t >= 1 && t <= MAX_TABS) {
            g_grid_windows = w;
            g_grid_tabs = t;
            g_windows_hold = w;
        } else g_c.errors++;
        g_ops_per_epoch = full ? 1200 : 400;
        g_warmup = 2;
        g_mem_epochs_min = 18;
    } else if (strncmp(g_scenario, "browser", 7) == 0) {
        g_browser_want = atoi(g_scenario + 7);
        if (g_browser_want < 1) g_browser_want = 1;
        if (g_browser_want > 4) g_browser_want = 4;   /* BROWSER_MAX_TABS */
        g_ops_per_epoch = full ? 600 : 200;
        g_warmup        = 2;
        g_mem_epochs_min = 18;
    } else if (strcmp(g_scenario, "images") == 0) {
        g_ops_per_epoch = full ? 900 : 300;
        g_warmup        = 2;
        g_mem_epochs_min = 18;
    } else if (strcmp(g_scenario, "markdown") == 0 ||
               strcmp(g_scenario, "editor") == 0) {
        g_ops_per_epoch = full ? 900 : 300;
        g_warmup        = 2;
        g_mem_epochs_min = 18;
    } else if (strcmp(g_scenario, "typing") == 0) {
        g_ops_per_epoch = full ? 3000 : 1000;
        g_warmup        = 2;
    }

    if (strcmp(g_scenario, "panes") == 0) {
        g_mem_epochs_min = 18;
        if (full) { g_epochs = 18; g_ops_per_epoch = 300; g_warmup = 6; }
    }

    { const char *v = liu_dev_env("SOAK_EPOCHS"); if (v && *v) { i32 n = atoi(v); if (n >= 1 && n <= 60) g_epochs = n; } }
    { const char *v = liu_dev_env("SOAK_OPS");    if (v && *v) { u64 n = strtoull(v, NULL, 0); if (n >= 1) g_ops_per_epoch = n; } }
    { const char *v = liu_dev_env("LIU_SOAK_PROBE"); g_probe_children = v && *v && *v != '0'; }
    { const char *v = liu_dev_env("SOAK_WARMUP"); if (v && *v) { i32 n = atoi(v); if (n >= 1 && n <= 60) g_warmup = n; } }
    g_min_epoch_seconds = 0.0;
    { const char *v = liu_dev_env("SOAK_EPOCH_SECONDS"); if (v && *v) {
        char *end = NULL;
        f64 n = strtod(v, &end);
        if (end && !*end && n > 0.0 && n <= 3600.0) g_min_epoch_seconds = n;
    } }

    { const char *p = liu_dev_env("SOAK_JSONL"); if (p && *p) g_jsonl = fopen(p, "a"); }

    soak_probe_reset_clock();
    printf("\n=== soak: app/%s (%s, %d warm-up + %d measured epochs x %llu ops, "
           "seed 0x%016llx) ===\n",
           g_scenario, full ? "full" : "quick", g_warmup, g_epochs,
           (unsigned long long)g_ops_per_epoch, (unsigned long long)g_seed);
    if (g_min_epoch_seconds > 0.0)
        printf("  minimum workload duration per epoch: %.3f s; operations continue until both gates pass\n",
               g_min_epoch_seconds);
    soak_probe_print_header();

    /* The knobs settle_to_baseline restores, sampled before anything moves
     * them. Taken here rather than hard-coded: the user's own config decides
     * what "unchanged" means, and a soak that reset to somebody else's font
     * size would rebuild the atlas on every settle. */
    {
        AppState *a0 = g_host->window_state(0);
        if (a0) { g_base_font = a0->config.font_size; g_base_dpi = a0->dpi_scale; }
    }

    g_warm_left = g_warmup;
    g_epoch_ops_left = g_ops_per_epoch;
    g_epoch_started_at = g_min_epoch_seconds > 0.0 ? soak_probe_now() : 0.0;
    g_phase = 0;
}

/* One operation per main-loop iteration, so the app keeps painting, polling
 * and animating between them. A soak that ran its whole epoch inside one
 * iteration would never let the renderer draw, and the renderer's caches are
 * half of what this is measuring.
 *
 * A SAMPLE IS NEVER TAKEN ON THE ITERATION THAT SETTLED. MEM_TERMINAL,
 * MEM_SCROLLBACK, MEM_EDITOR and MEM_RENDER_CPU are LEVELS re-derived by
 * app_poll_sessions once per iteration, and this tick runs after that poll —
 * so a sample taken immediately after closing three tabs still reports their
 * grids. The same applies to a destroyed session's child, which is reaped by
 * a later poll. QUIESCE_SEC gives both a bounded number of iterations to
 * catch up. */
#define SOAK_QUIESCE_SEC 0.5
/* How much longer than SOAK_QUIESCE_SEC the zombie/queue wait may run before
 * the sample is taken anyway. Past this a leftover is real. */
#define SOAK_QUIESCE_MAX_SEC 5.0

static f64 g_quiesce_until = 0.0;
/* Phase 4 arms its own quiesce window once, after the fixed settle wait. */
static bool g_final_quiesced = false;

/* The trash-queue depth can reach zero before the worker finishes stopping a
 * child process. Wait for child cleanup before checking the settled baseline.
 */
/* How long the owned-child count must hold still before a sample may be taken.
 *
 * Waiting for zombies is not enough: a child that has been signalled but has
 * not yet exited is neither a zombie nor gone, so a baseline taken while a
 * closed pane's shell is on its way out records one child (and its descriptor)
 * more than every later epoch. A count that falls below its baseline is a hard
 * failure by design, so the baseline has to be the right number. */
#define SOAK_CHILD_STABLE_SEC 0.25

/* THE BASELINE MAY NOT BE TAKEN WHILE THE PROCESS IS STILL STARTING UP.
 *
 * Liu's launch forks short-lived helpers — the PATH probe
 * (`sh -c '/bin/zsh' -lic ...`, which sources the user's rc), the notify-hook
 * install, the config probes — and they live a fraction of a second.
 * `soak_children_settled` asks only that the COUNT hold still for
 * SOAK_CHILD_STABLE_SEC, and a helper alive for half a second holds it
 * perfectly still while it runs. The baseline then records a population that
 * is about to vanish, and every exact-return check compares it against a
 * smaller one: resources going DOWN, the signature of two different
 * populations rather than a leak. `idle` is the most exposed, because op_idle
 * does nothing, so its warm-up epochs cost no wall clock and the baseline
 * would land shortly after launch.
 *
 * A bigger SOAK_CHILD_STABLE_SEC does not fix this — any constant can be
 * spanned by a helper that runs a little longer. What fixes it is refusing to
 * call the process quiet until it has been ALIVE long enough for launch to be
 * over. Two seconds is far longer than any of those helpers and far shorter
 * than the shortest scenario. The cap above still bounds the wait, so this can
 * never hang a run. */
#define SOAK_STARTUP_SETTLE_SEC 2.0

/* HOW MANY CHILDREN THE APP BELIEVES IT HAS.
 *
 * `soak_children_settled` asks only that the owned-child COUNT hold still for
 * SOAK_CHILD_STABLE_SEC, and a child that is DYING holds it perfectly still
 * while it dies — the shell of a tab settle_to_baseline has already
 * destroyed, for example. A baseline taken then holds one child more than
 * every later sample, and every exact-return check fails a run in which
 * nothing leaked. The startup guard (SOAK_STARTUP_SETTLE_SEC) does not cover
 * this; it is a different mechanism.
 *
 * A longer stability window would not fix it — the count is stable, just at
 * the wrong number. What settles it is that the expected count is DERIVABLE
 * rather than guessed: every child of this process belongs to a pane's
 * session or to a running Site, and both are enumerable. An orphan still
 * exiting belongs to neither, so `children > expected` is precisely "the
 * population has not finished draining".
 *
 * `session_child_pid` is used rather than `session_is_alive` deliberately:
 * the latter reaps, which would mutate session state from inside a wait. */
static i32 soak_expected_children(void) {
    i32 n = 0;
    if (!g_host || !g_host->window_count || !g_host->window_state) return -1;
    for (i32 w = 0; w < g_host->window_count(); w++) {
        AppState *a = g_host->window_state(w);
        if (!a) continue;
        for (i32 t = 0; t < a->tab_count; t++) {
            Tab *tab = &a->tabs[t];
            i32 np = tab_split_pane_count(tab);
            for (i32 p = 0; p < np; p++) {
                Session *ps = (p == 0) ? tab->session
                            : (p == 1) ? tab->session2 : tab->extra_sessions[p - 2];
                if (ps && session_child_pid(ps) > 0) n++;
            }
        }
        for (i32 si = 0; si < a->site_mgr.count; si++) {
            Site *st = sites_get(&a->site_mgr, si);
            if (st && st->session && session_child_pid(st->session) > 0) n++;
        }
    }
    return n;
}

static bool quiescing(void) {
    if (soak_probe_now() >= g_quiesce_until + SOAK_QUIESCE_MAX_SEC) return false;
    if (g_started_at > 0.0 &&
        soak_probe_now() - g_started_at < SOAK_STARTUP_SETTLE_SEC) return true;
    if (soak_probe_now() < g_quiesce_until) return true;
    if (strncmp(g_scenario,"winmin",6)==0) {
        if (soak_probe_now()-g_winmin_at<WINMIN_SETTLE_S) return true;
        for(i32 w=1;w<g_host->window_count();w++) {
            AppState *a=g_host->window_state(w);
            if(a && a->host_window && platform_window_is_minimized(a->host_window)) return true;
        }
    }
    SoakSample probe;
    soak_probe_take(&probe);
    if (probe.zombies > 0) return true;
    i32 depth = 0;
    tab_trash_stats(&depth, NULL, NULL);
    if (depth > 0) return true;
    /* AND THE COUNT MUST MATCH WHAT THE APP OWNS. See soak_expected_children:
     * a child that is dying holds the count still, so "stable" is not
     * "settled". A negative answer means the host cannot be asked, and the
     * stability test alone decides. */
    i32 expect = soak_expected_children();
    if (expect >= 0 && probe.children > expect) return true;
    return !soak_children_settled(SOAK_CHILD_STABLE_SEC);
}
/* THE SCENARIO'S OWN POPULATION, released once before the final sample.
 *
 * `settle` runs BETWEEN epochs and must leave the scenario's population
 * intact, or the trend measures the wave rather than the leak; `teardown` runs
 * ONCE and must leave the process in the same shape the baseline was taken in,
 * or every exact-return check compares two different worlds — Sites running at
 * the baseline and partly exited by the end are two different populations, not
 * a leak. */
static void scenario_teardown(bool final) {
    AppState *app = g_host->window_state(0);
    if (!app) return;
    if (strncmp(g_scenario, "sites", 5) == 0) {
        SiteManager *m = &app->site_mgr;
        while (m->count > 0) {
            if (!sites_remove(m, m->count - 1)) break;
            g_c.sites_removed++;
        }
    }
    /* Stop agent producers before destroying their panes so child-process
     * shutdown does not dominate the baseline wait. */
    if (strncmp(g_scenario, "agents", 6) == 0) {
        for (i32 t = 0; t < app->tab_count; t++) {
            Tab *tab = &app->tabs[t];
            i32 np = tab_split_pane_count(tab);
            for (i32 p = 0; p < np; p++) {
                Session *ps = (p == 0) ? tab->session
                            : (p == 1) ? tab->session2 : tab->extra_sessions[p - 2];
                if (ps) { const u8 etx = 0x03; session_write(ps, &etx, 1); }
            }
        }
        g_agents_armed = 0;
    }

    /* `winsN` hands its population back ONLY at the two sample points, and
     * `final` is what tells them apart.
     *
     * scenario_teardown runs twice: once before the BASELINE sample and once
     * before the FINAL one, so both describe the same empty shape. Clearing
     * the hold unconditionally would clear it for good: every settle would
     * then close the windows opened inside the epoch before the sample, and
     * every scale would measure ONE window. The hold is restored after the
     * baseline. */
    /* `gridWxT` holds its windows exactly like `winsN`: g_windows_want is 0
     * for it, so restoring that here would let every settle close the
     * grid's windows, and each epoch would REBUILD the population instead
     * of measuring it — the largest grids take more than one epoch to
     * build. */
    if (final || strncmp(g_scenario,"winmin",6)==0) g_windows_hold = 0;
    else       g_windows_hold = g_windows_want > 0 ? g_windows_want : g_grid_windows;

    /* `stream` needs nothing here: settle_to_baseline already closes every
     * split and replaces the surviving tab, which destroys the producers with
     * their sessions. The flag is reset so a later epoch re-arms rather than
     * assuming the typed command survived.
     *
     * `agents` needs the same re-arm, which settle_to_baseline does by
     * resetting `g_agents_armed`. That is a COUNT of panes given a producer,
     * and the arm gate is `g_agents_armed < live`: left at its old total, the
     * next epoch would rebuild its panes and arm NONE. The reachability check
     * cannot catch that, because g_c.agent_panes is cumulative and a counter
     * that was true once stays true. */
    g_stream_armed = false;
    /* Dock restore events can change the held windows' grid capacities. The
     * exact-return endpoints for winmin therefore use one fresh canonical
     * window; the measured epochs still hold the full minimized population. */
    if (final && strncmp(g_scenario,"winmin",6)==0) settle_to_baseline();
}

/* Separate time spent in app_soak_tick() from other frame work in the probe.
 * The tick phase includes workload setup and cleanup, so interpret it
 * alongside the operation counts and reachability data. */
static f64 g_op_seconds = 0.0;

static void do_one_op(u64 op) {
    f64 t0 = platform_time_sec();
    do_one_op_inner(op);
    g_op_seconds += platform_time_sec() - t0;
}

f64 app_soak_workload_seconds_take(void) {
    /* MINUS whatever ran inside do_one_op that was an instrument rather than
     * the workload. The subtraction is the right shape here: the caller adds
     * (all of app_soak_tick - this), so anything removed from "workload"
     * lands in "harness" by construction, which is where a process-table walk
     * belongs. Clamped because both terms are wall clocks. */
    f64 v = g_op_seconds - g_instr_in_op;
    if (v < 0.0) v = 0.0;
    g_op_seconds = 0.0;
    g_instr_in_op = 0.0;
    return v;
}

static void begin_quiesce(void) {
    settle_to_baseline();
    /* settle_to_baseline() is what CHANGES the child population — it closes
     * panes and windows — so the stability history from before it describes
     * the old one. Forgetting it here is what makes the wait below a wait for
     * the new steady state rather than a wait that was already satisfied. */
    soak_children_settled_reset();
    g_quiesce_until = soak_probe_now() + SOAK_QUIESCE_SEC;
}

/* The one metric that says this scenario is doing its job, by name. Kept
 * beside the reachability table below, which reads the same values — a second
 * list would drift from the first. 0 for a scenario the table does not cover;
 * those are caught by the generic "drove nothing" test instead. */
static u64 mixed_scenario_progress(void) {
    return g_c.windows_opened + g_c.windows_closed + g_c.tabs_opened +
           g_c.tabs_closed + g_c.splits_made + g_c.panes_closed +
           g_c.sleeps + g_c.wakes + g_c.font_invalidations +
           g_c.dpi_changes + g_c.resizes + g_c.palette_writes;
}

static bool scenario_reach_count(u64 *out) {
    if (strcmp(g_scenario, "all") == 0) { *out = mixed_scenario_progress(); return true; }
    if (strncmp(g_scenario, "typing", 6) == 0)   { *out = g_c.keys_sent;     return true; }
    if (strncmp(g_scenario, "scroll", 6) == 0)   { *out = g_c.scroll_fills;  return true; }
    /* NOT sites_added: that is SETUP. A stable population of running Sites
     * proves it is alive by PARSING LOG and by CHURNING, and it needs both
     * terms because the population deliberately mixes shapes. op_sites assigns
     * a command by index — index 0 is `exec sleep 3600`, an IDLE site on
     * purpose — so `sites1` is one site that never writes a byte and whose
     * log-write count is legitimately zero forever. Its continuing work is the
     * restart wave. A chatty population moves the first term, an idle one
     * moves the second, and every population moves at least one of them. */
    if (strncmp(g_scenario, "sites", 5) == 0) {
        *out = g_c.site_log_writes + g_c.sites_started + g_c.sites_stopped;
        return true;
    }
    if (strncmp(g_scenario, "stream", 6) == 0)   { *out = g_c.stream_bytes_seen; return true; }
    if (strncmp(g_scenario, "agents", 6) == 0)   { *out = g_c.agent_panes;   return true; }
    if (strncmp(g_scenario, "markdown", 8) == 0) { *out = g_c.md_opens;      return true; }
    if (strncmp(g_scenario, "editor", 6) == 0)   { *out = g_c.ed_edits;      return true; }
    if (strncmp(g_scenario, "images", 6) == 0)   { *out = g_c.img_opens + g_c.img_sixels; return true; }
    if (strncmp(g_scenario, "browser", 7) == 0)  { *out = g_c.br_opens;      return true; }
    /* `wins` HOLDS a population rather than accumulating one, so its "ever"
     * metric is a PEAK — and a peak cannot grow again once reached, so a
     * liveness check on it would fail `wins1` for doing exactly what the
     * scenario is for. What continues while the windows are alive is that they
     * PAINT: win_steady_frames sums every held window's frame_seq. */
    if (strncmp(g_scenario, "winanim", 7) == 0)  { *out = g_c.win_anim_starts; return true; }
    if (strncmp(g_scenario, "winmin", 6) == 0)   { *out = g_c.restore_confirmed; return true; }
    if (strncmp(g_scenario, "wins", 4) == 0)     { *out = g_c.win_steady_frames; return true; }
    if (strncmp(g_scenario, "tabs", 4) == 0)     { *out = g_c.tab_steady_frames; return true; }
    if (strncmp(g_scenario, "grid", 4) == 0)     { *out = g_c.grid_steady_frames; return true; }
    *out = 0;
    return false;   /* no liveness metric defined; the check does not apply */
}

/* The unattributable-drift floor for the PHYS series, per scenario where its
 * own null distribution is known. See SOAK_PHYS_FLOOR_SITES32_BYTES: the
 * platform-wide 4 MiB sits inside sites32's own spread, so it would fail runs
 * with nothing wrong. Only the phys series takes this — the in-use series is
 * the ownership question and keeps the platform floor. */
static f64 soak_phys_floor_bytes(void) {
    if (strncmp(g_scenario, "sites32", 7) == 0)
        return SOAK_PHYS_FLOOR_SITES32_BYTES;
    return SOAK_GROWTH_FLOOR_BYTES;
}

bool app_soak_tick(void) {
    if (!g_host) return false;
    /* Startup settings are recorded after the first frame. A cache/font/DPI
     * workload must not change them before that frame has been produced. */
    AppState *initial=g_host->window_state(0);
    if (g_ops==0 && initial && initial->frames_produced==0) return true;
    if (strcmp(g_scenario, "replay") == 0) replay_accumulate(g_host->window_state(0));

    switch (g_phase) {
    case 0:                                   /* warm-up operations */
        do_one_op(g_ops);
        if (soak_epoch_after_operation(&g_epoch_ops_left, g_epoch_started_at,
                g_min_epoch_seconds > 0.0 ? soak_probe_now() : 0.0, g_min_epoch_seconds)) {
            g_epoch_ops_left = g_ops_per_epoch;
            if (--g_warm_left <= 0) {
                /* The baseline must describe the SAME shape the final sample
                 * will, so a scenario that holds a population of its own
                 * (`sites8`'s eight running Sites) releases it before both.
                 * Both ends are empty. */
                scenario_teardown(false);
                begin_quiesce();
                g_phase = 1;
            }
            else {
                settle_to_baseline();
                g_epoch_started_at = g_min_epoch_seconds > 0.0 ? soak_probe_now() : 0.0;
            }
        }
        return true;

    case 1:                                   /* warm-up sample */
        if (quiescing()) return true;
        take_sample(&g_warm, 0, "warmup");
        if (strncmp(g_scenario,"winmin",6)==0) g_windows_hold=g_windows_want;
        /* Reset high-water marks after sampling the settled baseline so
         * reported peaks include only the subsequent workload. */
        mem_budget_peak_reset();
        /* Snapshot cumulative process-scan counters at the start of the
         * sampling interval so the report can calculate scans performed
         * during that interval. */
        { f64 _s = 0.0; session_proc_scan_stats(&g_scan_sweeps0, &_s); }
        if (strcmp(g_scenario, "replay") == 0) g_replay_panes = 1;
        if (strcmp(g_scenario, "glyphs") == 0) { glyph_phase_begin("ascii"); g_glyph_fed = 0; }
        if (strcmp(g_scenario, "scroll") == 0) {
            AppState *a0 = g_host->window_state(0);
            if (a0 && a0->tab_count > 0 && a0->tabs[a0->active_tab].terminal)
                terminal_set_scrollback_limit(a0->tabs[a0->active_tab].terminal,
                                              g_scroll_lines);
        }
        g_phase = 2;
        g_epoch_i = 0;
        g_epoch_ops_left = g_ops_per_epoch;
        g_epoch_started_at = g_min_epoch_seconds > 0.0 ? soak_probe_now() : 0.0;
        return true;

    case 2:                                   /* measured epoch operations */
        do_one_op(g_ops);
        if (soak_epoch_after_operation(&g_epoch_ops_left, g_epoch_started_at,
                g_min_epoch_seconds > 0.0 ? soak_probe_now() : 0.0, g_min_epoch_seconds)) {
            begin_quiesce(); g_phase = 3;
        }
        return true;

    case 3:                                   /* measured epoch sample */
        if (quiescing()) return true;
        if (g_epoch_n < (i32)(sizeof g_epoch_samples / sizeof g_epoch_samples[0]))
            take_sample(&g_epoch_samples[g_epoch_n++], ++g_epoch_i, "epoch");
        else g_epoch_i++;
        if (strcmp(g_scenario, "replay") == 0) replay_report();
        if (strcmp(g_scenario, "glyphs") == 0) {
            glyph_phase_report(g_glyph_fed);
            static const char *kPhases[] = { "ascii", "cjk", "emoji", "reload", "dpi" };
            i32 pi = g_epoch_i < 5 ? g_epoch_i : 4;
            glyph_phase_begin(kPhases[pi]);
            g_glyph_fed = 0;
        }
        if (g_epoch_i >= g_epochs) {
            begin_quiesce();
            g_settle_until = soak_probe_now() + 3.0;
            g_phase = 4;
        } else {
            /* Capture the reach counter when the final epoch starts; compare
             * it after the epoch to confirm the workload remained active
             * throughout the interval.
             */
            if (g_epoch_i == g_epochs - 1)
                g_reach_snapped = scenario_reach_count(&g_reach_last_epoch);
            g_epoch_ops_left = g_ops_per_epoch;
            g_epoch_started_at = g_min_epoch_seconds > 0.0 ? soak_probe_now() : 0.0;
            g_phase = 2;
            /* The replay sweep steps its pane count per epoch: 1, 2, 4, 8, and
             * then it stops — see op_static_replay for why 64 is not a state
             * this product has. */
            if (strcmp(g_scenario, "replay") == 0) {
                static const i32 kSweep[] = { 1, 2, 4, 8, 8, 8, 8, 8 };
                i32 idx = g_epoch_i < 8 ? g_epoch_i : 7;
                g_replay_panes = kSweep[idx];
                memset(&g_replay, 0, sizeof g_replay);   /* the split itself is
                                                          * a rebuild, and it
                                                          * belongs to neither
                                                          * epoch */
            }
        }
        return true;

    case 4:                                   /* bounded final settle */
        if (soak_probe_now() < g_settle_until) return true;
        /* THE FINAL SAMPLE GETS THE BASELINE'S TREATMENT.
         *
         * Every exact-return check compares these two samples, so they have to
         * be taken in the same state or the comparison measures the difference
         * between two sampling methods. The baseline goes through quiescing() —
         * no zombies, an empty trash queue, an owned-child count that has held
         * still — and so does this one; a fixed wall-clock wait alone samples
         * whatever it finds, such as a child in the gap between
         * local_child_stop's SIGKILL and its waitpid.
         *
         * The fixed wait stays in front: it is what lets the deferred work
         * START. The quiesce after it is what waits for it to FINISH, and it is
         * armed fresh here because begin_quiesce's window closed while the
         * three seconds ran. */
        if (!g_final_quiesced) {
            scenario_teardown(true);
            soak_children_settled_reset();
            g_quiesce_until  = soak_probe_now() + SOAK_QUIESCE_SEC;
            g_final_quiesced = true;
            return true;
        }
        if (quiescing()) return true;
        take_sample(&g_final, g_epoch_i + 1, "final");
        g_final_imgs_live = prompt_recall_imgs_live();
        g_phase = 5;
        return true;

    default:
        break;
    }

    /* ---- verdict --------------------------------------------------------- */
    bool ok = true;
    printf("\n  --- exact-return resources (baseline = after warm-up) ---\n");
    struct { const char *n; i64 a, b; } exact[] = {
        { "open file descriptors", g_warm.fds,         g_final.fds },
        { "live PTY readers",      g_warm.pty_readers, g_final.pty_readers },
        { "owned child processes", g_warm.children,    g_final.children },
        { "zombies",               0,                  g_final.zombies },
        { "tab-trash queue depth", 0,                  g_final.trash_items },
        { "sleep snapshot files",  g_warm.snap_files,  g_final.snap_files },
        { "prompt attachment blocks", 0,                g_final_imgs_live },
    };
    for (u32 i = 0; i < sizeof exact / sizeof exact[0]; i++) {
        bool e_ok = exact[i].a == exact[i].b;
        printf("  %-4s %-34s %10lld -> %-10lld\n", e_ok ? "ok" : "FAIL",
               exact[i].n, (long long)exact[i].a, (long long)exact[i].b);
        if (!e_ok) ok = false;
    }
    if (g_final.fds != g_warm.fds) soak_probe_print_fds("final");
    /* Threads are NOT an exact-return resource in the app the way they are in
     * a headless binary: AppKit, Metal and the display link each keep a pool
     * whose size depends on what the window server did, and none of it is
     * ours. Reported so a change is visible, never failed on. */
    printf("  note live threads                       %10d -> %-10d "
           "(framework pools; reported, not gated)\n", g_warm.threads, g_final.threads);
    /* A number nobody can act on becomes one they can: which threads. Printed
     * only when the count actually grew, so an ordinary run stays quiet. */
    if (g_final.threads > g_warm.threads) soak_probe_print_threads("final");
    if (g_final.children != g_warm.children || g_final.zombies > 0)
        soak_probe_print_children("final");
    for (i32 i = 0; i < MEM_RESOURCE_COUNT; i++) {
        if (g_warm.ledger[i] == 0 && g_final.ledger[i] == 0) continue;
        bool e_ok = g_warm.ledger[i] == g_final.ledger[i];
        /* The three GPU/CPU cache categories are RETAINED caches by contract —
         * font atlases and texture caches survive on purpose — so they are
         * reported and not gated. Everything else must return. */
        bool gated = !(i == MEM_FONT_CPU || i == MEM_FONT_GPU ||
                       i == MEM_IMAGE_GPU || i == MEM_RENDER_GPU ||
                       i == MEM_RENDER_CPU);
        printf("  %-4s ledger: %-26s %10zu -> %-10zu\n",
               gated ? (e_ok ? "ok" : "FAIL") : "note",
               mem_budget_name((MemResource)i), g_warm.ledger[i], g_final.ledger[i]);
        if (gated && !e_ok) ok = false;
    }

    /* What each category REACHED, from the ledger's own monotone high-water.
     * The loop above skips any category that is zero at both ends, and a
     * settled app is most of them — so without this line a category the
     * scenario exercised and returned cleanly is indistinguishable from one it
     * never touched, and the second means its check asserted nothing. */
    printf("\n  --- ledger categories: peak in the measured window -> at the end ---\n");
    for (i32 i = 0; i < MEM_RESOURCE_COUNT; i++) {
        usize peak = g_final.ledger_peak[i];
        if (peak == 0 && g_final.ledger[i] == 0) {
            printf("  %-14s        -\n", mem_budget_name((MemResource)i));
            continue;
        }
        printf("  %-14s %9.2f MiB -> %9.2f MiB\n",
               mem_budget_name((MemResource)i), peak / 1048576.0,
               g_final.ledger[i] / 1048576.0);
    }

    /* TWO SCENARIOS DELIBERATELY DO DIFFERENT WORK IN EACH EPOCH, and for them
     * a memory trend is not a gate — it is a statistic computed over a sweep.
     * `replay` steps its pane count 1, 2, 4, 8, and `glyphs` steps its phase
     * ascii -> cjk -> emoji -> reload -> dpi; those epochs are not comparable
     * with each other by construction, so a verdict on their trend would mean
     * nothing in either direction. The exact-return checks above still gate
     * them — those compare a state with itself and are valid whatever the
     * epochs did — and what these two scenarios exist to produce is their own
     * counters. */
    bool plateau_gates = strcmp(g_scenario, "replay") != 0 &&
                         strcmp(g_scenario, "glyphs") != 0;
    printf("\n  --- plateau (warm-up excluded, %d measured epochs) ---\n", g_epoch_n);
    if (!plateau_gates)
        printf("  note this scenario sweeps a parameter, so its epochs are not\n"
               "       comparable and the trend below is reported, not gated\n");
    {
        f64 b[64], o[64], scratch[64 * 64];
        for (i32 i = 0; i < g_epoch_n; i++) {
            b[i] = (f64)g_epoch_samples[i].phys_footprint;
            o[i] = (f64)g_epoch_samples[i].ops;
        }
        const char *cs = liu_dev_env("SOAK_CONTROL_SLOPE");
        f64 control = (cs && *cs) ? strtod(cs, NULL) : SOAK_NO_CONTROL;
        SoakTrend t = soak_trend(b, o, g_epoch_n, control,
                                 soak_phys_floor_bytes(), scratch, 64 * 64);
        i32 mem_need = g_mem_epochs_min > 0 ? g_mem_epochs_min : SOAK_MIN_EPOCHS;
        bool phys_judged = g_epoch_n >= mem_need;
        bool phys_grew   = t.verdict == SOAK_TREND_GROWING;
        /* The prefix has to agree with what the line can actually decide. A
         * sweep scenario's trends are reported and not gated, so they are never
         * printed as FAIL: `glyphs` feeds a DIFFERENT glyph class each epoch
         * (ascii, then CJK, then emoji, then a font reload) and the atlas is
         * supposed to grow across them. */
        /* A growth barely over the line is the same noise as a pass barely
         * under it — see soak_trend_marginal(). Reported, never failed. */
        bool phys_marginal = phys_grew && soak_trend_marginal(t.growth, t.threshold);
        bool phys_fails    = phys_grew && phys_judged && !phys_marginal;
        printf("  %-4s phys_footprint  %-12s slope %10.2f B/op   growth %8.2f MiB "
               "vs threshold %6.2f MiB\n",
               !plateau_gates ? "note" : (phys_fails ? "FAIL" : "ok"),
               (!phys_judged && phys_grew) ? "INCONCLUSIVE"
                   : (phys_marginal ? "MARGINAL" : soak_trend_name(t.verdict)),
               t.slope_net, t.growth / 1048576.0, t.threshold / 1048576.0);
        if (phys_marginal)
            printf("       (MARGINAL: %.0f%% of its own threshold, inside this "
                   "metric's measured run-to-run scatter — reported, not "
                   "failed; the in-use series below is the ownership half)\n",
                   100.0 * t.growth / t.threshold);
        if (!phys_grew && soak_trend_thin(t.growth, t.threshold))
            printf("       (THIN: the growth is %.0f%% of its own threshold — a "
                   "pass, but not evidence; read the in-use series instead)\n",
                   100.0 * t.growth / t.threshold);
        if (!phys_judged)
            printf("       (this scenario needs %d epochs before its footprint "
                   "means anything and this run had %d — the series decides "
                   "nothing here; in-use, the ledger and every exact-return "
                   "check still do)\n", mem_need, g_epoch_n);
        if (phys_fails && plateau_gates) ok = false;

        /* What libmalloc has handed out and not got back. The footprint adds
         * that together with pages the allocator freed and has not returned to
         * the OS, and the second term is large and scheduler-dependent — see
         * the note in core/soak_probe.h. This series is the ownership half on
         * its own, and it is gated for the same scenarios the footprint is. */
        if (g_epoch_samples[0].malloc_reserved > 0) {
            for (i32 i = 0; i < g_epoch_n; i++)
                b[i] = (f64)g_epoch_samples[i].malloc_in_use;
            /* The in-use series needs its own control for the same reason the
             * footprint does, and on the windows scenario it is the whole
             * answer: a bare NSWindow opened and closed with no Liu code in it
             * retains handed-out malloc bytes on every cycle, and this
             * scenario's growth stays within that, so it is AppKit's root
             * cycle. SOAK_CONTROL_INUSE_SLOPE carries the control's slope;
             * without the subtraction the run fails on the framework's
             * behaviour. */
            const char *us = liu_dev_env("SOAK_CONTROL_INUSE_SLOPE");
            f64 ucontrol = (us && *us) ? strtod(us, NULL) : SOAK_NO_CONTROL;
            SoakTrend u = soak_trend(b, o, g_epoch_n, ucontrol,
                                     SOAK_GROWTH_FLOOR_BYTES, scratch, 64 * 64);
            /* A sanitizer's allocator quarantines frees, so this series climbs
             * whatever the program does — see soak_probe.h. Printed, not
             * gated. */
            bool sanitized = soak_allocator_is_sanitized();
            printf("  %-4s malloc in-use   %-12s slope %10.2f B/op   growth %8.2f MiB "
                   "vs threshold %6.2f MiB\n",
                   (sanitized || !plateau_gates)
                       ? "note" : (u.verdict == SOAK_TREND_GROWING ? "FAIL" : "ok"),
                   soak_trend_name(u.verdict), u.slope_net,
                   u.growth / 1048576.0, u.threshold / 1048576.0);
            if (ucontrol == SOAK_NO_CONTROL)
                printf("       (no in-use control subtracted — "
                       "SOAK_CONTROL_INUSE_SLOPE unset)\n");
            else
                printf("       (raw slope %.2f B/op, control %.2f B/op)\n",
                       u.slope, ucontrol);
            if (sanitized)
                printf("       (a sanitizer owns the allocator and quarantines "
                       "frees: reported, NOT gated)\n");
            if (u.verdict == SOAK_TREND_GROWING && plateau_gates && !sanitized)
                ok = false;
        }
    }

    soak_probe_print_ram("baseline, settled", &g_warm);
    soak_probe_print_ram("final, settled",    &g_final);

    printf("\n  --- what was driven ---\n");
    printf("  windows %llu open / %llu close, tabs %llu / %llu, splits %llu, "
           "panes closed %llu, prompt attachments %llu\n",
           (unsigned long long)g_c.windows_opened, (unsigned long long)g_c.windows_closed,
           (unsigned long long)g_c.tabs_opened, (unsigned long long)g_c.tabs_closed,
           (unsigned long long)g_c.splits_made, (unsigned long long)g_c.panes_closed,
           (unsigned long long)g_c.prompts_attached);
    if (g_c.min_requested)
        printf("  minimize: %llu requested, %llu confirmed minimized, %llu confirmed "
               "restored, %llu unconfirmed; %llu frames painted while minimized "
               "(%llu minimized windows reported visible to the paint gate)\n",
               (unsigned long long)g_c.min_requested, (unsigned long long)g_c.min_confirmed,
               (unsigned long long)g_c.restore_confirmed, (unsigned long long)g_c.min_unconfirmed,
               (unsigned long long)g_c.min_frames_while_min,
               (unsigned long long)g_c.min_reported_visible);
    if (g_c.min_unconfirmed) {
        printf("  FAIL: %llu minimize/restore requests the window system did not carry out\n",
               (unsigned long long)g_c.min_unconfirmed);
        ok = false;
    }
    printf("  sleeps %llu, wakes %llu, sleep refused %llu; font %llu, dpi %llu, "
           "resize %llu, palette %llu; errors %llu\n",
           (unsigned long long)g_c.sleeps, (unsigned long long)g_c.wakes,
           (unsigned long long)g_c.sleep_blocked,
           (unsigned long long)g_c.font_invalidations,
           (unsigned long long)g_c.dpi_changes, (unsigned long long)g_c.resizes,
           (unsigned long long)g_c.palette_writes,
           (unsigned long long)g_c.errors);
    if (g_c.errors) ok = false;

    /* TYPING LATENCY, its own distribution — see the note at g_type_hist for
     * what the interval spans and why the frame series cannot express it.
     *
     * p99.9 is SUPPRESSED below a few thousand samples rather than printed
     * over a handful: a label that needs 10 000 samples and has 300 is not a
     * conservative estimate, it is the maximum wearing a percentile's name. */
    if (g_type_hist.count > 0) {
        const LiuHist *h = &g_type_hist;
        printf("\n  --- typing latency: dispatch -> echo visible in the grid ---\n");
        printf("  %8s %9s %9s %9s %9s %9s %9s\n",
               "count", "mean", "p50", "p95", "p99", "p99.9", "max");
        if (h->count >= 3000) {
            printf("  %8llu %8.3f %8.3f %8.3f %8.3f %8.3f %8.3f  ms\n",
                   (unsigned long long)h->count, liu_hist_mean_ns(h) / 1e6,
                   liu_hist_quantile_ms(h, 0.50), liu_hist_quantile_ms(h, 0.95),
                   liu_hist_quantile_ms(h, 0.99), liu_hist_quantile_ms(h, 0.999),
                   (f64)h->max_ns / 1e6);
        } else {
            printf("  %8llu %8.3f %8.3f %8.3f %8.3f %8s %8.3f  ms\n",
                   (unsigned long long)h->count, liu_hist_mean_ns(h) / 1e6,
                   liu_hist_quantile_ms(h, 0.50), liu_hist_quantile_ms(h, 0.95),
                   liu_hist_quantile_ms(h, 0.99), "n/a",
                   (f64)h->max_ns / 1e6);
            printf("       (p99.9 suppressed: %llu samples, needs 3000+)\n",
                   (unsigned long long)h->count);
        }
        printf("       (%llu carriage returns excluded — a command is not an "
               "echo; %llu keystrokes saw no echo within 2 s and are counted, "
               "not folded into the tail)\n",
               (unsigned long long)g_type_returns, (unsigned long long)g_type_lost);
    }

    /* WHAT EACH SCENARIO DROVE: every line is its scenario's proof that the
     * path was reached, since a percentile table over an operation that never
     * ran would mean nothing. */
    if (g_c.keys_sent || g_c.scroll_fills || g_c.sites_added ||
        g_c.stream_bytes_seen || g_c.agent_panes ||
        g_c.md_opens || g_c.ed_edits || g_c.win_steady_peak ||
        g_c.img_opens || g_c.img_sixels || g_c.br_opens) {
        if (g_c.agent_panes) {
            printf("  agents: %llu panes armed, %llu detection hits, %llu "
                   "activity hits, %llu process sweeps in the measured window\n",
                   (unsigned long long)g_c.agent_panes,
                   (unsigned long long)g_c.agent_detected,
                   (unsigned long long)g_c.agent_activity,
                   (unsigned long long)g_c.agent_scan_sweeps);
        }
        if (g_c.br_opens) {
            printf("  browser: %llu views created, %llu closed, peak %llu live, "
                   "%llu refused at the cap | WebKit helper processes: peak +%llu "
                   "(counted SEPARATELY from Liu's own memory)\n",
                   (unsigned long long)g_c.br_opens,
                   (unsigned long long)g_c.br_closes,
                   (unsigned long long)g_c.br_peak,
                   (unsigned long long)g_c.br_refused,
                   (unsigned long long)g_c.br_webkit_peak);
        }
        if (g_c.img_opens || g_c.img_sixels) {
            /* The stats are monotonic: report the DELTA over the run, not the
             * process total, or a second scenario in the same process would
             * inherit the first one's numbers. */
            const LiuImageStats *n = &g_liu_image_stats;
            printf("  images: %llu previews, %llu sixels, %llu closes | "
                   "cache hit/miss %llu/%llu, uploads %llu (%.2f MiB), "
                   "alloc/update %llu/%llu, oversize %llu, restage %llu, "
                   "evict %llu\n",
                   (unsigned long long)g_c.img_opens,
                   (unsigned long long)g_c.img_sixels,
                   (unsigned long long)g_c.img_closes,
                   (unsigned long long)(n->hits - g_img_base.hits),
                   (unsigned long long)(n->misses - g_img_base.misses),
                   (unsigned long long)(n->upload_count - g_img_base.upload_count),
                   (f64)(n->upload_bytes - g_img_base.upload_bytes) / 1048576.0,
                   (unsigned long long)(n->alloc_count - g_img_base.alloc_count),
                   (unsigned long long)(n->update_count - g_img_base.update_count),
                   (unsigned long long)(n->oversize_upload_count - g_img_base.oversize_upload_count),
                   (unsigned long long)(n->restage_count - g_img_base.restage_count),
                   (unsigned long long)(n->evict_count - g_img_base.evict_count));
        }
        if (g_c.md_opens || g_c.ed_edits) {
            printf("  markdown: %llu opens, %llu blocks parsed, %llu closes, "
                   "%llu navigations | editor: %llu opens, %llu edits, "
                   "%llu undos, %llu finds\n",
                   (unsigned long long)g_c.md_opens,
                   (unsigned long long)g_c.md_blocks,
                   (unsigned long long)g_c.md_closes,
                   (unsigned long long)g_c.md_navigations,
                   (unsigned long long)g_c.ed_opens,
                   (unsigned long long)g_c.ed_edits,
                   (unsigned long long)g_c.ed_undos,
                   (unsigned long long)g_c.ed_finds);
        }
        printf("  keys %llu; stream bytes %llu; scroll fill/scroll/search/"
               "select/reflow %llu/%llu/%llu/%llu/%llu; sites add/start/stop "
               "%llu/%llu/%llu\n",
               (unsigned long long)g_c.keys_sent,
               (unsigned long long)g_c.stream_bytes_seen,
               (unsigned long long)g_c.scroll_fills,
               (unsigned long long)g_c.scroll_scrolls,
               (unsigned long long)g_c.scroll_searches,
               (unsigned long long)g_c.scroll_selects,
               (unsigned long long)g_c.scroll_reflows,
               (unsigned long long)g_c.sites_added,
               (unsigned long long)g_c.sites_started,
               (unsigned long long)g_c.sites_stopped);
    }

    /* PER-SCENARIO REACHABILITY. The generic "drove nothing" test below cannot
     * see these, because none of them increments a window/tab/split counter. */
    /* TWO QUESTIONS, AND FOR SOME SCENARIOS TWO DIFFERENT METRICS.
     *
     *   `got` / `what`            did it EVER reach its target path. A setup
     *                             counter is the right answer here.
     *   scenario_reach_count()    is it STILL working in the final epoch. A
     *   / `live_what`             setup counter is the WRONG answer here: a
     *                             stable population of Sites does not add
     *                             another record every epoch, and demanding
     *                             that it did would measure lifecycle churn
     *                             instead of the steady state it exists to
     *                             hold. `sites` is the case that needs the
     *                             split; for the rest the two coincide. */
    struct { const char *name; u64 got; const char *what; const char *live_what; } reach[] = {
        { "all", mixed_scenario_progress(), "mixed state changes", "mixed state changes" },
        { "typing", g_c.keys_sent,         "keystrokes sent", "keystrokes sent" },
        { "scroll", g_c.scroll_fills,      "scrollback fills", "scrollback fills" },
        { "sites",  g_c.sites_added,       "Sites added",
                    "log bytes parsed plus start/stop transitions" },
        { "stream", g_c.stream_bytes_seen, "bytes seen from the producers",
                    "bytes seen from the producers" },
        { "agents", g_c.agent_panes,       "agent-shaped panes armed",
                    "agent-shaped panes armed" },
        { "markdown", g_c.md_opens,        "markdown documents parsed",
                    "markdown documents parsed" },
        { "editor", g_c.ed_edits,          "editor edits", "editor edits" },
        { "winflood", g_c.win_flood_bytes, "bytes streamed into window 0",
                    "bytes streamed into window 0" },
        { "winanim", g_c.win_anim_starts,  "animations run in window 0",
                    "animations run in window 0" },
        { "winmin", g_c.min_confirmed,     "confirmed minimizes", "confirmed minimizes" },
        { "wins",   g_c.win_steady_peak,   "windows held", "windows held" },
        { "tabs",   g_c.tab_steady_peak,   "tabs held", "tabs held" },
        { "grid",   g_c.grid_steady_peak,   "window-tab sessions held", "window-tab sessions held" },
        { "images", g_c.img_opens + g_c.img_sixels, "image opens or Sixels",
                    "image opens or Sixels" },
        { "browser", g_c.br_opens,         "browser views created",
                    "browser views created" },
    };
    /* While window 0 was the only thing changing, how many frames did each of
     * the others paint? Printed rather than asserted: the loop-wide redraw
     * flag is a design point, not a defect, and this is the number that would
     * justify changing it. */
    if (g_c.win_frames_n > 0 && (strncmp(g_scenario, "winflood", 8) == 0 ||
                                 strncmp(g_scenario, "winanim", 7) == 0)) {
        printf("\n  frames painted while ONLY window 0 was changing:\n");
        u64 base = 0;
        for (i32 w = 0; w < g_c.win_frames_n; w++) {
            AppState *a = g_host->window_state(w);
            u64 now = a ? (u64)a->frame_seq : 0;
            u64 got = now > g_c.win_frames[w] ? now - g_c.win_frames[w] : 0;
            if (w == 0) base = got;
            printf("    window %d: %8llu frames%s", w, (unsigned long long)got,
                   w == 0 ? "  (the one being fed)\n" : "");
            if (w > 0)
                printf("  = %.1f%% of window 0\n",
                       base ? 100.0 * (double)got / (double)base : 0.0);
        }
    }

    bool target_ever = true, target_final = true, target_checked = false;
    for (usize i = 0; i < sizeof reach / sizeof reach[0]; i++) {
        bool mine = (strncmp(g_scenario, reach[i].name, strlen(reach[i].name)) == 0);
        if (mine) target_checked = true;
        if (mine && reach[i].got == 0) {
            target_ever = false;
            printf("  FAIL: scenario '%s' produced zero %s — its target path "
                   "was never reached and its distribution describes nothing\n",
                   g_scenario, reach[i].what);
            ok = false;
        }
        /* AND WAS IT STILL DRIVING AT THE END. The counter above is cumulative,
         * so a scenario that worked once and then stopped keeps passing it. A
         * workload that stops mid-run makes the memory trend and the latency
         * distribution describe two different programs, and neither of them the
         * one named at the top. */
        u64 live_now = 0;
        bool live_known = scenario_reach_count(&live_now);
        if (mine && reach[i].got > 0 && g_reach_snapped && live_known &&
            live_now <= g_reach_last_epoch) {
            target_final = false;
            printf("  FAIL: scenario '%s' drove %s early and then STOPPED — the "
                   "count did not move during the final epoch (%llu at its "
                   "start, %llu at the end), so the trend and the distribution "
                   "describe a workload that was no longer running\n",
                   g_scenario, reach[i].live_what,
                   (unsigned long long)g_reach_last_epoch,
                   (unsigned long long)live_now);
            ok = false;
        }
    }

    /* A scenario that drove nothing is a failure, not a pass: every one of
     * these has a way of silently doing nothing (a window cap, a tab cap, a
     * sleep policy that refuses everything), and a plateau over zero
     * operations proves nothing. */
    u64 driven = g_c.windows_opened + g_c.tabs_opened + g_c.splits_made +
                 g_c.sleeps + g_c.font_invalidations + g_c.resizes +
                 g_c.palette_writes +
                 g_c.keys_sent + g_c.scroll_fills + g_c.sites_added +
                 g_c.stream_bytes_seen + g_c.agent_panes +
                 g_c.md_opens + g_c.ed_edits + g_c.win_steady_peak +
                 g_c.min_confirmed +
                 g_c.tab_steady_peak +
                 g_c.grid_steady_peak +
                 g_c.win_flood_bytes +
                 g_c.img_opens + g_c.img_sixels + g_c.br_opens;
    if (driven == 0 && strcmp(g_scenario, "idle") != 0) {
        printf("  FAIL: the scenario performed no production operation at all\n");
        ok = false;
    }
    /* An idle run is expected to perform zero operations; use the
     * frame-present count to confirm that no work was submitted. */
    if (strcmp(g_scenario, "idle") == 0)
        printf("  note idle drove nothing, which is the measurement — read the "
               "frame probe's present count, not this line\n");

    /* ONE MACHINE-READABLE LINE, so the runner never has to grep prose.
     *
     * A scenario can fail its target in more than one way, each worded
     * differently, so a search for one sentence misses the others. Every field
     * below is a fixed key with a 0/1 or an enumerated value, and the runner
     * reads only these. */
    printf("\nSOAK_MARKER scenario=%s complete=1 target_checked=%d "
           "target_ever=%d target_final_epoch=%d resource=%s seed=0x%016llx "
           "epochs=%d ops=%llu errors=%llu min_epoch_seconds=%.3f\n",
           g_scenario, target_checked ? 1 : 0, target_ever ? 1 : 0,
           target_final ? 1 : 0, ok ? "PASS" : "FAIL",
           (unsigned long long)g_seed, g_epochs,
           (unsigned long long)g_ops, (unsigned long long)g_c.errors,
           g_min_epoch_seconds);

    printf("\n  app/%s: %s (seed 0x%016llx)\n", g_scenario, ok ? "PASS" : "FAIL",
           (unsigned long long)g_seed);
    if (g_jsonl) { fclose(g_jsonl); g_jsonl = NULL; }
    g_exit = ok ? 0 : 1;
    return false;
}

bool app_soak_wants_redraw(void) {
    return strcmp(g_scenario, "idle") != 0;
}

/* WHICH window the forced redraw belongs to; -1 means "all", the behaviour
 * every scenario but one wants.
 *
 * winflood is the exception and the reason this exists. It asks what an idle
 * window costs while a sibling streams, and a harness that raises the LOOP-WIDE
 * redraw flag every frame would make every window paint, always, so the
 * scenario would measure its own scaffolding. Once the fill is done, winflood
 * marks only window 0, the same way a real streaming pane marks its own window
 * through visible_session_data_this_frame. */
i32 app_soak_redraw_window(void) {
    if ((strncmp(g_scenario, "winflood", 8) == 0 ||
         strncmp(g_scenario, "winanim", 7) == 0) && g_c.win_frames_n > 0)
        return 0;
    return -1;
}

i32 app_soak_exit_code(void) { return g_exit; }
