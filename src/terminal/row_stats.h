/*
 * Liu — what resolving a visible row costs, and how often it is paid twice.
 *
 * terminal_visible_row() answers "what cells are on visible row N". For a row
 * still in the live grid that is a pointer into t->cells and free. For a row
 * scrolled in from history it is a termline_materialize() into the per-terminal
 * scratch — a real decompression, and the returned pointer is only valid until
 * the NEXT resolve, so nothing can hold onto it.
 *
 * A dirty row is resolved independently by three consumers in the same frame:
 * the glyph/background row build, the decoration pass, and the image-badge
 * scan. For a live-grid row that is three pointer computations. For a cold row
 * it is three decompressions of the same line, and every scroll marks the whole
 * screen dirty (see the scroll-dirty rule in buffer.c), so scrolling through
 * cold history pays it on every row of every frame.
 *
 * This counts those resolves and times the cold ones. Main-thread only, like
 * the render path it measures; plain integers for that reason.
 */
#ifndef TERMINAL_ROW_STATS_H
#define TERMINAL_ROW_STATS_H

#include "core/types.h"
#include "core/percentile.h"

typedef struct {
    u64 resolve_calls;        /* every terminal_visible_row() that answered   */
    u64 live_calls;           /* ...of those, served from the live grid       */
    u64 cold_calls;           /* ...of those, a decompression                 */
    u64 cold_cells;           /* cells materialized                           */
    u64 cold_bytes;           /* those cells in bytes                         */
    u64 cold_ns;              /* summed; see cold_hist for the distribution   */
    LiuHist cold_hist;        /* per-materialize duration                     */
} LiuRowStats;

/* Per-process row statistics. buffer.c updates this structure. */
extern LiuRowStats g_liu_row_stats;

/* Collection is compile-time so the hot path avoids runtime clocks, counters,
 * and flag checks. Without LIU_ROW_STATS, the instrumentation compiles away.
 */
#ifdef LIU_ROW_STATS
extern bool g_liu_row_timing_on;
#endif

#ifdef LIU_ROW_STATS
static inline void liu_row_stat_live(void) {
    if (!g_liu_row_timing_on) return;
    g_liu_row_stats.resolve_calls++;
    g_liu_row_stats.live_calls++;
}

static inline void liu_row_stat_cold(i32 cells, u64 ns) {
    if (!g_liu_row_timing_on) return;
    LiuRowStats *s = &g_liu_row_stats;
    s->resolve_calls++;
    s->cold_calls++;
    if (cells > 0) {
        s->cold_cells += (u64)cells;
        s->cold_bytes += (u64)cells * sizeof(Cell);
    }
    s->cold_ns += ns;
    liu_hist_add_ns(&s->cold_hist, ns);
}

static inline void liu_row_stats_reset(void) {
    LiuRowStats *s = &g_liu_row_stats;
    *s = (LiuRowStats){0};
    liu_hist_reset(&s->cold_hist);
}
#else
/* Nothing to compile away at the call sites: both are empty. */
static inline void liu_row_stat_live(void) {}
static inline void liu_row_stat_cold(i32 cells, u64 ns) { (void)cells; (void)ns; }
#endif

#endif /* TERMINAL_ROW_STATS_H */
