/*
 * Liu — process-wide memory accounting for the optional, expensive subsystems.
 *
 * Every heavy subsystem in the tree has its own cap — scrollback bytes, the
 * two image caches, the editor and its undo mirrors, a bridge migration's
 * arena, the voice capture buffer, the graph. Each is bounded, and each is
 * chosen without reference to the others, so without a ledger nothing in the
 * process could answer "how much is all of this, together, right now". A user
 * with deep scrollback in sixteen panes, a lightbox open, a migration running
 * and a dictation buffered is inside every individual budget and outside
 * anything anyone reasoned about.
 *
 * This is deliberately NOT an allocator. It is a ledger: subsystems report
 * what they retain, the total is observable, and caches that can give memory
 * back register a way to do so. The per-subsystem caps stay exactly where they
 * are and remain the primary defence — this is the layer that makes their sum
 * visible, and gives the reclaimable ones a place to be asked first.
 *
 * Accounting counters support concurrent owners through atomic updates.
 * Reclaimer registration, pressure policy and diagnostics are UI-thread-only;
 * a worker must never invoke cache callbacks owned by the UI.
 */
#ifndef CORE_MEM_BUDGET_H
#define CORE_MEM_BUDGET_H

#include "core/types.h"

typedef enum {
    MEM_SCROLLBACK = 0,   /* compressed history + its ring, all panes       */
    MEM_TERMINAL,         /* live + alt grids and per-row tables, all panes  */
    MEM_IMAGE_CPU,        /* decoded pixels the viewers retain              */
    MEM_IMAGE_GPU,        /* renderer texture caches                        */
    MEM_EDITOR,           /* open document + its two undo mirrors + frames  */
    MEM_HISTORY,          /* history migration + bounded live-title mailbox */
    MEM_VOICE,            /* capture buffer + whatever the worker holds     */
    MEM_GRAPH,            /* Markdown knowledge-graph nodes/edges           */
    MEM_RENDER_GPU,       /* render targets + atlas upload staging          */
    MEM_FONT_CPU,         /* glyph + emoji atlas mirrors held on the CPU    */
    MEM_FONT_GPU,         /* the glyph and emoji atlas textures             */
    MEM_MODEL_CPU,        /* a loaded LLM: the GGUF mapping + host scratch  */
    MEM_MODEL_GPU,        /* its weights and scratch resident on the GPU    */
    MEM_MODEL_KV,         /* its KV cache, wherever that lives              */
    /* The PTY gather rings and the readers that own them, every session in
     * the process — panes and Sites alike. Retained CAPACITY, not the bytes
     * currently pending: the ring is allocated at its full size for the life
     * of the reader and that is what the process is holding. Reported through
     * the TRANSIENT (thread-safe) counter because a reader is stopped from
     * whichever thread owns the teardown, and the tab-trash worker is one of
     * them. Excludes the reader's 64 KiB thread stack — an OS mapping rather
     * than something this process allocates — which is the one part of a
     * reader's footprint this number does not name. */
    MEM_PTY_IO,
    /* CPU-side renderer state: the per-row glyph/background/decoration caches
     * and their index arrays, plus each renderer's instance and rect batches.
     * The GPU-side textures and buffers stay in the MEM_*_GPU categories; this
     * is what the host keeps so it does not have to rebuild a frame from the
     * grid. Inventoried from actual capacities, never estimated from live
     * element counts — a batch that grew to a peak and emptied is still
     * holding that memory. */
    MEM_RENDER_CPU,
    /* The Sites registry and its per-site runtime, MINUS everything that has
     * a category of its own: a site's log Terminal counts as MEM_TERMINAL,
     * its scrollback as MEM_SCROLLBACK, and its PTY reader ring as
     * MEM_PTY_IO. What is left is the Site records themselves — path,
     * command, the rolling ANSI-stripped tail, the port-detection carry —
     * about 3.8 KiB each. */
    MEM_SITES,
    /* The diff dock: the `git diff` text it shows, its row and file arrays,
     * and — through the TRANSIENT counter — the capture buffer the worker is
     * still filling. One owner at a time: the worker charges the transient
     * side while it grows the buffer, and the pickup in git_diff_poll moves
     * that exact capacity onto the retained side in the same step, so a
     * result is never counted twice and never counted by nobody. Capacities,
     * not lengths: what was asked of malloc is what the process is holding. */
    MEM_GIT,
    MEM_RESOURCE_COUNT
} MemResource;

/* Report the CURRENT retained bytes for a resource. Idempotent — callers set
 * the level rather than adding deltas, because a subsystem always knows its
 * own total and a missed delta would drift forever. */
void  mem_budget_set(MemResource r, usize bytes);
/* Main-thread aggregate owners (many terminals/viewers sharing one category)
 * report deltas. Saturates at 0/SIZE_MAX rather than wrapping. */
void  mem_budget_adjust(MemResource r, isize delta);
/* Worker-owned temporary allocations. Thread-safe; included in get()/total()
 * but kept separate from the main-thread retained level. */
void  mem_budget_transient_adjust(MemResource r, isize delta);
usize mem_budget_get(MemResource r);
/* Category high-water mark since process start. It persists after allocations
 * are released so diagnostics can distinguish a subsystem that allocated and
 * returned to zero from one that was never used. */
usize mem_budget_peak(MemResource r);
void  mem_budget_peak_reset(void);
usize mem_budget_total(void);
const char *mem_budget_name(MemResource r);

/* A soft ceiling on the sum. Crossing it is not an error and nothing is
 * refused: it is the point at which reclaimable caches are asked to shrink,
 * before the correctness-critical resources (the live terminal grid, the open
 * document) are ever considered — which they never are. */
#define MEM_BUDGET_SOFT_TOTAL ((usize)1536 * 1024 * 1024)

/* Register a way to give memory back. `fn` should release what it can and
 * return; the ledger re-reads the totals afterwards. Reclaimers are called in
 * the order below, coldest first, and only ever for caches — never for
 * anything whose loss would change what the user sees as data. */
typedef void (*MemReclaimFn)(void *ctx);
void mem_budget_register_reclaim(MemResource r, MemReclaimFn fn, void *ctx);

/* Ask the registered reclaimers to shrink, worst-offender first, while the
 * total exceeds the soft ceiling. Returns the bytes the ledger says were
 * released. Safe to call every frame — it does nothing while under budget. */
usize mem_budget_reclaim_if_over(void);
/* Explicit monotonic time for callers that already sampled the process turn,
 * and for deterministic policy tests. One cooldown is shared by all windows;
 * every pass backs off, even when partial reclamation leaves the total over.
 * >=16 MiB growth bypasses the cooldown. Returning under/registering resets. */
#define MEM_RECLAIM_COOLDOWN_SECONDS 2.0
#define MEM_RECLAIM_WAKE_DELTA ((usize)16 * 1024 * 1024)
usize mem_budget_reclaim_if_over_at(f64 now);

/* Diagnostics: how many times the soft ceiling has been crossed and how many
 * bytes reclamation has recovered, cumulative. */
void mem_budget_stats(u64 *out_pressure_events, u64 *out_reclaimed_bytes);

/* The OS said the MACHINE is short of memory (platform_memory_pressure_take;
 * 1 = warn, 2 = critical). Unlike mem_budget_reclaim_if_over this does not
 * consult the soft ceiling — a process under its own budget can still be
 * the one that should give back — and it asks EVERY registered reclaimer
 * once, coldest first, bounded to one call each. Graded: a WARN is acted on
 * at most once per MEM_PRESSURE_REWARM_SECONDS elapsed seconds (the caches would only be
 * re-filled and re-dropped), a CRITICAL always, and a level higher than the
 * last one handled always. Returns the bytes the ledger says were released.
 * Main thread, like everything else here. */
#define MEM_PRESSURE_REWARM_SECONDS 2.0
usize mem_budget_reclaim_pressure(i32 level);
usize mem_budget_reclaim_pressure_at(i32 level, f64 now);
/* Diagnostics for the OS path: signals handled and bytes they released. */
void  mem_budget_pressure_stats(u64 *out_os_events, u64 *out_os_reclaimed_bytes);

#endif /* CORE_MEM_BUDGET_H */
