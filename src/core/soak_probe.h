/*
 * Resource snapshots for long-running workloads. Record process footprint,
 * allocator usage, descriptors, threads, child processes, and memory-ledger
 * high-water marks.
 */
#ifndef CORE_SOAK_PROBE_H
#define CORE_SOAK_PROBE_H

#include "core/types.h"
#include "core/mem_budget.h"
#include "core/vm_regions.h"
#include <stdbool.h>
#include <stdio.h>

/* True when a sanitizer replaces the process allocator. Allocator statistics
 * are not comparable to live application allocations in that mode. */
static inline bool soak_allocator_is_sanitized(void) {
#if defined(__has_feature)
  #if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
      __has_feature(memory_sanitizer)
    return true;
  #endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#endif
    return false;
}

typedef struct {
    /* Where in the run this is. */
    i32   epoch;              /* 0 = warm-up; the gate excludes it            */
    f64   t_sec;              /* seconds since the run started                */
    u64   ops;                /* scenario operations completed so far         */

    /* Memory the OS reports. */
    u64   phys_footprint;     /* macOS TASK_VM_INFO; Linux VmRSS+swap         */
    u64   resident;           /* resident_size / VmRSS                        */

    /* Memory the ALLOCATOR reports, which is the discriminator between the two
     * things phys_footprint adds together.
     *
     * phys_footprint can swing across a wide band — even with a NEGATIVE
     * overall slope — while the ledger reads zero at every single sample. None
     * of that is ownership: it is libmalloc holding freed pages against the
     * peak working set and macOS reclaiming them on its own schedule, and a
     * gate that only sees phys_footprint cannot say so — it reports GROWING
     * and a leak hunt begins on a process that owns nothing.
     *
     * `malloc_in_use` is what the allocator says is HANDED OUT right now. That
     * is bounded by what the process owns, so it trends like the ledger and
     * unlike the footprint, and it also catches what the ledger cannot: a leak
     * in a subsystem that never learned to charge itself. `malloc_reserved` is
     * `size_allocated`, and it is NOT the resident half of the answer — it can
     * read far above phys_footprint, because it counts the address space the
     * zone carved out, most of it never touched. It is recorded as an
     * observation and nothing is judged on it. There is no libmalloc counter
     * for "freed but still-dirty pages", which is what the footprint's excess
     * actually is, and the one API that would settle it —
     * malloc_zone_pressure_relief — is off limits here: relieving pressure to
     * make a number look better is the definition of hiding growth. */
    /* WHERE the footprint lives, by VM tag — the residency half of the RAM
     * question, which neither the ledger nor malloc's in-use figure can
     * answer. `phys_footprint - ledger_total` is a RAW GAP; this is what turns
     * it into attributed buckets. See core/vm_regions.h. */
    VmrSnapshot vm;

    usize malloc_in_use;      /* macOS malloc_zone_statistics size_in_use     */
    usize malloc_reserved;    /* ... size_allocated; 0 where unavailable      */
    u64   malloc_blocks;      /* blocks_in_use — a count, not a size          */

    /* Memory the process claims. */
    usize ledger[MEM_RESOURCE_COUNT];
    /* The high-water each category reached since the measured window opened,
     * from mem_budget_peak(). A sample is taken SETTLED, so `ledger` alone
     * reads zero for every category a scenario returns cleanly — which is all
     * of them in most scenarios — and cannot say whether the category was ever
     * exercised. This can. */
    usize ledger_peak[MEM_RESOURCE_COUNT];
    usize ledger_total;

    /* Exact-return resources. Every one of these must come back to its
     * post-warm-up value; none of them is allowed a plateau. */
    i32   fds;                /* whole descriptor table, not the first 1024   */
    i32   threads;            /* live mach threads / /proc/self/task entries  */
    i32   pty_readers;        /* pty_reader_live_count()                      */
    i32   children;           /* processes whose parent is this process */
    i32   zombies;            /* of those, exited and unreaped                */
    i32   trash_items;        /* tab-trash queue depth                        */
    usize trash_bytes;
    f64   trash_oldest_sec;
    i32   sites;              /* registered / running, when a scenario has any */
    i32   sites_running;
    i32   snap_files;         /* TMPDIR/liu-sleep-<pid>-* left behind          */
    usize snap_bytes;

    /* Snapshot the process tree at the final traversal depth so
     * descendants beyond the configured limit are reported as
     * incomplete. */
    u64   errors;
    u64   timeouts;
    u64   bytes_dropped;
    u64   bytes_duplicated;
    u64   unexpected_wakes;
} SoakSample;

/* Fill everything this module can know on its own. The scenario overwrites
 * `epoch`, `ops`, the site counters and the error counters afterwards — they
 * are its own bookkeeping, not the OS's. Cheap enough to call between epochs;
 * NOT cheap enough to call per operation (the FD listing is a syscall over the
 * whole table). */
void soak_probe_take(SoakSample *out);

/* Has the process's owned-child population STOPPED MOVING?
 *
 * A baseline sample must describe a settled process or every later epoch is
 * compared against a moment of teardown, and the failure is loud and wrong: a
 * warm-up sample taken while a closed pane's shell is still exiting counts one
 * child and one descriptor more than every later epoch, and a count that FALLS
 * below its baseline is a hard failure by design — it normally means a double
 * close — although nothing leaked.
 *
 * Waiting for zombies to hit zero is not enough, which is the trap: a child that
 * has been signalled but has not yet exited is neither a zombie nor gone, and
 * `session_destroy` returns before the kernel is finished with it. So the test
 * is stability rather than any particular value — the scenario legitimately
 * keeps a shell alive — and it needs a real interval between the two
 * observations, since a caller polling once per loop iteration would otherwise
 * compare two reads microseconds apart and call anything stable.
 *
 * Returns true once `children` has read the same value twice at least
 * `min_gap_sec` apart with no zombies; the CALLER owns the overall deadline. */
/* Process-tree resources are tracked separately from this process's own
 * allocator and VM statistics because local sessions run in child processes.
 */
typedef struct {
    i32 procs;        /* descendants found (0 if the query is unavailable)   */
    i32 depth_max;    /* deepest level reached, for reading the shape        */
    u64 bytes;        /* summed phys_footprint of those descendants          */
    bool truncated;   /* the depth cap or the array cap was hit              */
} SoakTree;

void soak_probe_tree(SoakTree *out);

/* Wall-clock seconds spent inside soak_probe_take() since the previous call;
 * reading this value resets the accumulator. The probe walks VM regions,
 * queries libmalloc zone statistics, and enumerates child processes. Its
 * elapsed time is recorded so frame-latency reports can exclude
 * instrumentation cost. */
f64 soak_probe_overhead_take(void);

bool soak_children_settled(f64 min_gap_sec);
/* Forget the stability history — call before a phase whose child count is
 * expected to change, so the next settle waits for the NEW population. */
void soak_children_settled_reset(void);

/* Seconds since the first soak_probe_take() in this process, on
 * CLOCK_MONOTONIC. Reset with soak_probe_reset_clock(). */
f64  soak_probe_now(void);
void soak_probe_reset_clock(void);

/* One JSON object per line — the machine-readable half. `scenario` and `phase`
 * are copied into the row so a merged file stays sortable. Flushed on every
 * call, because a soak that is killed must leave a usable series behind. */
void soak_probe_write_jsonl(FILE *f, const char *scenario, const char *phase,
                            const SoakSample *s);

/* The human half: one aligned line per epoch. */
void soak_probe_print(const char *scenario, const SoakSample *s);
/* phys_footprint / mem_budget_total / raw_gap, then the VM-tag
 * decomposition and the unattributed residual. Reported, never gated. */
void soak_probe_print_ram(const char *label, const SoakSample *s);
void soak_probe_print_header(void);

/* Print a histogram of live thread names. Names help identify which subsystem
 * owns thread growth; unnamed threads are reported as (unnamed). This is best
 * effort because system frameworks may create threads without names. */
void soak_probe_print_threads(const char *label);

/* Print file descriptor types and, when available, the path or socket each
 * descriptor refers to. */
void soak_probe_print_fds(const char *label);

/* Print the PID, state, and executable name for each direct child process.
 */
void soak_probe_print_children(const char *label);

/* Sleep-snapshot leftovers in TMPDIR for THIS pid. Counted by soak_probe_take;
 * exposed because a scenario that deliberately fails a snapshot wants to check
 * the cleanup on its own schedule. */
void soak_probe_count_snapshots(i32 *out_files, usize *out_bytes);

#endif /* CORE_SOAK_PROBE_H */
