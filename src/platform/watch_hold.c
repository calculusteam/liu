#include "platform/watch_hold.h"
#include "platform/platform.h"

#include <stdlib.h>
#include <string.h>

static u8   g_hold[WATCH_HOLD_MAX_FD];
static i32  g_hold_count  = 0;
static bool g_owed_resume = false;

/* OPT-IN, not opt-out.
 *
 * Suppressing a pane's read watch also suppresses the wake that DRIVES its
 * drain, and the loop then falls back to whatever the timeout ladder and the
 * present grid allow, which can quietly become a throughput ceiling on a
 * single flooding pane (the pacing sleep in main.c stands aside for it). The
 * gain is on a rare multi-pane workload, so holds are off by default and
 * LIU_POLL_HOLD=1 turns them on. Everything else (the saturation counter and
 * the probe's sat/s) stays live either way. */
bool watch_hold_enabled(void) {
    static int en = -1;
    if (en < 0) {
        const char *e = getenv("LIU_POLL_HOLD");
        en = (e && e[0] == '1') ? 1 : 0;
    }
    return en != 0;
}

void watch_hold_set(int fd) {
    if (!watch_hold_enabled()) return;
    if (fd < 0 || fd >= WATCH_HOLD_MAX_FD) return;
    if (g_hold[fd]) return;              /* idempotent: the count must not drift */
    g_hold[fd] = 1;
    g_hold_count++;
}

bool watch_hold_is_set(int fd) {
    if (fd < 0 || fd >= WATCH_HOLD_MAX_FD) return false;
    return g_hold[fd] != 0;
}

i32 watch_hold_pending(void) { return g_hold_count; }

void watch_hold_clear(int fd) {
    if (fd < 0 || fd >= WATCH_HOLD_MAX_FD) return;
    if (!g_hold[fd]) return;
    g_hold[fd] = 0;
    g_hold_count--;
}

bool watch_hold_pass_begin(bool fired) {
    return fired || g_hold_count > 0 || g_owed_resume;
}

void watch_hold_pass_end(bool skipped_any) {
    if (g_hold_count > 0) {
        memset(g_hold, 0, sizeof g_hold);
        g_hold_count = 0;
    }
    g_owed_resume = skipped_any;
}

void platform_hold_watch(int fd) { watch_hold_set(fd); }
