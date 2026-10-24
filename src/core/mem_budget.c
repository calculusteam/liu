/*
 * Liu — process-wide memory ledger. See mem_budget.h.
 */
#include "core/mem_budget.h"

#include <string.h>
#include <stdatomic.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {
    /* RELAXED ATOMIC, AND EVERY UPDATE IS A CAS.
     *
     * The level has more than one writer: `tab_trash.c`'s worker thread calls
     * `terminal_destroy`, which releases image bytes through
     * `mem_budget_adjust`, and parser buffer teardown charges the same
     * categories.
     *
     * An atomic field alone makes the concurrent READ safe but not the WRITE: a
     * load, an arithmetic step and a store are three operations, and a second
     * writer between the load and the store is simply lost. A lost update is
     * worse than a torn read here, because the ledger drives reclamation: a
     * category that reads low lets the process past a ceiling it has already
     * crossed.
     *
     * Relaxed ordering is enough — no caller needs this counter ordered against
     * anything else — and on arm64 the CAS is a bare ldxr/stxr pair. */
    _Atomic usize bytes;
    MemReclaimFn  reclaim;
    void         *ctx;
} MemSlot;

/* Read/write the main-thread level. Spelled out so no site can silently go
 * back to a plain access. */
#define MEM_LVL(r)         atomic_load_explicit(&g_mem[r].bytes, memory_order_relaxed)
#define MEM_LVL_SET(r, v)  atomic_store_explicit(&g_mem[r].bytes, (v), memory_order_relaxed)

static MemSlot g_mem[MEM_RESOURCE_COUNT];
static _Atomic usize g_transient[MEM_RESOURCE_COUNT];

/* The HIGH-WATER each category has reached, monotone until
 * mem_budget_peak_reset().
 *
 * Not a debugging nicety: a soak samples between operations, when the process
 * has settled, so `mem_budget_get` at sample time answers what is held right
 * then — which for a scenario that finishes each epoch empty is zero for every
 * category. Without the peak, the soak's coverage line could not tell "this
 * scenario exercised the category and gave it all back" from "this scenario
 * never touched it, so its exact-return check asserted nothing".
 *
 * Relaxed atomics because it is written from whichever thread charges the
 * category and read once per epoch; a lost update under a race costs one
 * sample of precision on a diagnostic, and paying for ordering on the ledger's
 * hot path would not. */
static _Atomic usize g_peak[MEM_RESOURCE_COUNT];

static void peak_note(MemResource r, usize now) {
    _Atomic usize *p = &g_peak[r];
    usize old = atomic_load_explicit(p, memory_order_relaxed);
    while (now > old &&
           !atomic_compare_exchange_weak_explicit(p, &old, now,
                   memory_order_relaxed, memory_order_relaxed)) { }
}

static u64     g_pressure_events = 0;
static u64     g_reclaimed_bytes = 0;

static const char *const kNames[MEM_RESOURCE_COUNT] = {
    "scrollback", "terminal", "image-cpu", "image-gpu", "editor", "history", "voice", "graph",
    "render-gpu", "font-cpu", "font-gpu",
    "model-cpu", "model-gpu", "model-kv",
    "pty-io", "render-cpu", "sites", "git",
};
_Static_assert(sizeof kNames / sizeof kNames[0] == MEM_RESOURCE_COUNT,
               "every MemResource needs a name — a missing one reads as \"?\" "
               "in the probe and in the pressure log");

usize mem_budget_peak(MemResource r) {
    if (r < 0 || r >= MEM_RESOURCE_COUNT) return 0;
    return atomic_load_explicit(&g_peak[r], memory_order_relaxed);
}

void mem_budget_peak_reset(void) {
    for (i32 i = 0; i < MEM_RESOURCE_COUNT; i++) {
        atomic_store_explicit(&g_peak[i], mem_budget_get((MemResource)i),
                              memory_order_relaxed);
    }
}

const char *mem_budget_name(MemResource r) {
    return (r >= 0 && r < MEM_RESOURCE_COUNT) ? kNames[r] : "?";
}

void mem_budget_set(MemResource r, usize bytes) {
    if (r < 0 || r >= MEM_RESOURCE_COUNT) return;
    MEM_LVL_SET(r, bytes);
    peak_note(r, mem_budget_get(r));
}

void mem_budget_adjust(MemResource r, isize delta) {
    if (r < 0 || r >= MEM_RESOURCE_COUNT || delta == 0) return;
    _Atomic usize *p = &g_mem[r].bytes;
    usize old = atomic_load_explicit(p, memory_order_relaxed);
    for (;;) {
        usize next;
        if (delta > 0) {
            usize add = (usize)delta;
            next = old > (usize)-1 - add ? (usize)-1 : old + add;
        } else {
            /* Avoid negating ISIZE_MIN. */
            usize sub = (usize)(-(delta + 1)) + 1u;
            next = sub >= old ? 0 : old - sub;   /* saturate, never wrap */
        }
        if (atomic_compare_exchange_weak_explicit(p, &old, next,
                memory_order_relaxed, memory_order_relaxed)) break;
        /* old now holds what the other writer left; recompute against it, so
         * the saturation is applied to the value that actually won. */
    }
    peak_note(r, mem_budget_get(r));
}

usize mem_budget_get(MemResource r) {
    if (r < 0 || r >= MEM_RESOURCE_COUNT) return 0;
    usize a = MEM_LVL(r);
    usize b = atomic_load_explicit(&g_transient[r], memory_order_relaxed);
    return a > (usize)-1 - b ? (usize)-1 : a + b;
}

void mem_budget_transient_adjust(MemResource r, isize delta) {
    if (r < 0 || r >= MEM_RESOURCE_COUNT || delta == 0) return;
    _Atomic usize *p = &g_transient[r];
    usize old = atomic_load_explicit(p, memory_order_relaxed);
    for (;;) {
        usize next;
        if (delta > 0) {
            usize add = (usize)delta;
            next = old > (usize)-1 - add ? (usize)-1 : old + add;
        } else {
            usize sub = (usize)(-(delta + 1)) + 1u;
            next = sub >= old ? 0 : old - sub;
        }
        if (atomic_compare_exchange_weak_explicit(p, &old, next,
                memory_order_relaxed, memory_order_relaxed)) break;
    }
    peak_note(r, mem_budget_get(r));
}

usize mem_budget_total(void) {
    usize total = 0;
    for (i32 i = 0; i < MEM_RESOURCE_COUNT; i++) {
        usize b = mem_budget_get((MemResource)i);
        /* Saturate rather than wrap: a bogus report from one subsystem must
         * not make the total look small. */
        if (total > (usize)-1 - b) return (usize)-1;
        total += b;
    }
    return total;
}

static f64   g_reclaim_retry_at = 0;
static usize g_reclaim_after_total = 0;

static f64 mem_monotonic_now(void) {
#ifdef _WIN32
    return (f64)GetTickCount64() * 0.001;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
#endif
}

void mem_budget_register_reclaim(MemResource r, MemReclaimFn fn, void *ctx) {
    if (r < 0 || r >= MEM_RESOURCE_COUNT) return;
    g_mem[r].reclaim = fn;
    g_mem[r].ctx     = ctx;
    /* Every pass puts the reclaimer on a cooldown keyed to the
     * total. A newly registered reclaimer changes what a pass could achieve
     * WITHOUT changing the total, so the cooldown has to be cancelled here or
     * the new one is not asked for up to two seconds. */
    g_reclaim_retry_at = 0;
}

void mem_budget_stats(u64 *out_pressure_events, u64 *out_reclaimed_bytes) {
    if (out_pressure_events) *out_pressure_events = g_pressure_events;
    if (out_reclaimed_bytes) *out_reclaimed_bytes = g_reclaimed_bytes;
}

static u64 g_os_events = 0, g_os_reclaimed = 0;
static i32 g_os_last_level = 0;
static f64 g_os_handled_at = 0;
static bool g_os_handled = false;

void mem_budget_pressure_stats(u64 *out_os_events, u64 *out_os_reclaimed_bytes) {
    if (out_os_events) *out_os_events = g_os_events;
    if (out_os_reclaimed_bytes) *out_os_reclaimed_bytes = g_os_reclaimed;
}

usize mem_budget_reclaim_pressure_at(i32 level, f64 now) {
    /* Zero means no queued notification, not that pressure recovered. Normal
     * event-loop turns must not turn the next WARN into a fresh escalation. */
    if (level <= 0) return 0;
    /* Re-warm gate: the same or a lower level inside the window is the OS
     * repeating itself while the caches are still cold. A step UP, or a
     * critical, is never deferred. */
    bool escalated = level > g_os_last_level;
    bool rewarmed  = !g_os_handled || now - g_os_handled_at >= MEM_PRESSURE_REWARM_SECONDS;
    if (level < 2 && !escalated && !rewarmed) return 0;
    g_os_last_level = level;
    g_os_handled_at = now;
    g_os_handled = true;
    g_os_events++;

    usize before = mem_budget_total();
    /* Coldest first is the registration order's promise (see
     * mem_budget_register_reclaim); every reclaimer is asked exactly once. */
    for (i32 i = 0; i < MEM_RESOURCE_COUNT; i++) {
        if (!g_mem[i].reclaim) continue;
        g_mem[i].reclaim(g_mem[i].ctx);
    }
    usize after = mem_budget_total();
    usize freed = before > after ? before - after : 0;
    g_os_reclaimed += freed;
    g_reclaimed_bytes += freed;
    /* Do not immediately repeat this cache pass through the soft path in the
     * same process turn. A new CRITICAL notification still bypasses its own
     * WARN gate and the soft deadline. */
    g_reclaim_after_total = after;
    g_reclaim_retry_at = now + MEM_RECLAIM_COOLDOWN_SECONDS;
    return freed;
}

usize mem_budget_reclaim_pressure(i32 level) {
    return mem_budget_reclaim_pressure_at(level, mem_monotonic_now());
}

usize mem_budget_reclaim_if_over_at(f64 now) {
    usize before = mem_budget_total();
    if (before <= MEM_BUDGET_SOFT_TOTAL) {
        g_reclaim_retry_at = 0;      /* back under; the next crossing is fresh */
        return 0;
    }

    if (now < g_reclaim_retry_at) {
        /* Shrinking is progress, not a reason to throw away freshly rewarmed
         * caches again. Only meaningful growth bypasses the elapsed gate. */
        usize growth = before > g_reclaim_after_total ? before - g_reclaim_after_total : 0;
        if (growth < MEM_RECLAIM_WAKE_DELTA) return 0;
    }

    g_pressure_events++;

    /* Worst offender first, and only among resources that registered a way to
     * give memory back. A resource with no reclaimer is one whose loss would
     * change what the user sees — the live grid, the open document — and is
     * never a candidate, however large it is.
     *
     * A reclaimer that frees nothing exhausts ITSELF, not the pass, so a big
     * unreclaimable image cache in front of a GPU cache full of cold textures
     * does not keep the second from being asked. Marks are sticky within a
     * pass: a reclaimer with nothing to give a microsecond ago still has
     * nothing, and that bounds a pass at one call per resource. */
    bool exhausted[MEM_RESOURCE_COUNT] = {0};
    for (;;) {
        i32 worst = -1;
        usize worst_bytes = 0;
        for (i32 i = 0; i < MEM_RESOURCE_COUNT; i++) {
            if (!g_mem[i].reclaim || exhausted[i]) continue;
            usize lvl = MEM_LVL(i);
            if (lvl > worst_bytes) { worst_bytes = lvl; worst = i; }
        }
        if (worst < 0 || worst_bytes == 0) break;

        exhausted[worst] = true;
        g_mem[worst].reclaim(g_mem[worst].ctx);
        /* The reclaimer re-reports through mem_budget_set/adjust. */
        if (mem_budget_total() <= MEM_BUDGET_SOFT_TOTAL) break;
    }

    usize after = mem_budget_total();
    usize freed = before > after ? before - after : 0;
    g_reclaimed_bytes += freed;
    g_reclaim_after_total = after;
    g_reclaim_retry_at = now + MEM_RECLAIM_COOLDOWN_SECONDS;
    return freed;
}

usize mem_budget_reclaim_if_over(void) {
    return mem_budget_reclaim_if_over_at(mem_monotonic_now());
}
