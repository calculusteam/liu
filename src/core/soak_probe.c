/*
 * Liu — soak observation. See soak_probe.h.
 */
#include "core/soak_probe.h"
#include "core/private_tmp.h"
#include "core/pty_reader.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/param.h>

#if defined(PLATFORM_MACOS) || defined(__APPLE__)
  #include <libproc.h>
  #include <mach/mach.h>
  #include <malloc/malloc.h>
  #include <sys/sysctl.h>
#else
  /* mallinfo2 lives here, and __GLIBC_PREREQ comes with <features.h> which
   * this pulls in. Guarded because musl and the BSDs have neither, and
   * read_malloc's else-branch answers zero there by design. */
  #if defined(__GLIBC__)
    #include <malloc.h>
  #endif
#endif

/* tab_trash is a UI-layer module and not every soak binary links it. The
 * scenario supplies the numbers when it has them; the weak default keeps this
 * file linkable on its own. */
__attribute__((weak)) void tab_trash_stats(i32 *depth, usize *bytes, f64 *oldest_sec) {
    if (depth) *depth = 0;
    if (bytes) *bytes = 0;
    if (oldest_sec) *oldest_sec = 0.0;
}

static f64 mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
}

static f64 g_t0 = 0.0;

void soak_probe_reset_clock(void) { g_t0 = mono_now(); }

f64 soak_probe_now(void) {
    if (g_t0 == 0.0) g_t0 = mono_now();
    return mono_now() - g_t0;
}

/* ---- memory ------------------------------------------------------------- */

static void read_memory(u64 *phys, u64 *res) {
    *phys = 0; *res = 0;
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
    task_vm_info_data_t vm;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &n) == KERN_SUCCESS) {
        /* phys_footprint is what the OS itself charges against a memory limit,
         * and the only one of the two that tracks ownership rather than page
         * residency. TASK_VM_INFO_REV1_COUNT is where the field starts, so an
         * older kernel that answers with a shorter struct leaves it zero
         * rather than reporting garbage. */
        if (n >= TASK_VM_INFO_REV1_COUNT) *phys = (u64)vm.phys_footprint;
        *res = (u64)vm.resident_size;
    }
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        unsigned long total = 0, resident = 0;
        if (fscanf(f, "%lu %lu", &total, &resident) == 2)
            *res = (u64)resident * (u64)sysconf(_SC_PAGESIZE);
        fclose(f);
    }
    /* Linux has no phys_footprint; VmRSS + swapped is the closest
     * equivalent and is what the gate trends there. */
    f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        u64 rss = 0, swap = 0;
        while (fgets(line, sizeof line, f)) {
            unsigned long kb = 0;
            if (sscanf(line, "VmRSS: %lu kB", &kb) == 1)      rss  = (u64)kb * 1024u;
            else if (sscanf(line, "VmSwap: %lu kB", &kb) == 1) swap = (u64)kb * 1024u;
        }
        fclose(f);
        *phys = rss + swap;
        if (*res == 0) *res = rss;
    }
#endif
}

/* What the ALLOCATOR holds, as opposed to what the OS charges the process.
 *
 * Every zone, not just the default one: libmalloc runs several (the default
 * nano zone for small blocks, the scalable zone, and whatever a library
 * created), and asking only the default one under-reports exactly the large
 * blocks a scrollback churn produces. malloc_zone_statistics() takes a lock
 * per zone, so this is a per-EPOCH cost, never a per-operation one.
 *
 * The whole-process form (`malloc_zone_statistics(NULL, ...)`) exists but is
 * documented as approximate; enumerating is exact and costs one extra call. */
static void read_malloc(usize *in_use, usize *reserved, u64 *blocks) {
    *in_use = 0; *reserved = 0; *blocks = 0;
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
    vm_address_t *zones = NULL;
    unsigned count = 0;
    if (malloc_get_all_zones(mach_task_self(), NULL, &zones, &count) != KERN_SUCCESS)
        return;
    for (unsigned i = 0; i < count; i++) {
        malloc_zone_t *z = (malloc_zone_t *)zones[i];
        if (!z) continue;
        malloc_statistics_t st;
        memset(&st, 0, sizeof st);
        malloc_zone_statistics(z, &st);
        *in_use   += (usize)st.size_in_use;
        *reserved += (usize)st.size_allocated;
        *blocks   += (u64)st.blocks_in_use;
    }
#else
    /* glibc's mallinfo2 is the equivalent and is 64-bit clean; mallinfo(3) is
     * not, and silently truncates past 4 GiB — which a soak reaches. Anything
     * else answers zero and the gate then skips the series rather than
     * trending a number it cannot produce. */
  #if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
    #if __GLIBC_PREREQ(2, 33)
    struct mallinfo2 mi = mallinfo2();
    *in_use   = (usize)mi.uordblks;
    *reserved = (usize)(mi.arena + mi.hblkhd);
    *blocks   = 0;
    #endif
  #endif
#endif
}

/* ---- descriptors -------------------------------------------------------- */

static i32 count_fds(void) {
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
    /* The WHOLE table, not the first 1024. A soak running 128 readers holds
     * two control-pipe ends each on top of the pty masters; a 0..1023 loop
     * stops counting exactly where the interesting numbers start. */
    pid_t pid = getpid();
    int bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, NULL, 0);
    if (bytes <= 0) return -1;
    /* Room for growth between the sizing call and the listing call. */
    int cap = bytes + 64 * (int)sizeof(struct proc_fdinfo);
    struct proc_fdinfo *buf = (struct proc_fdinfo *)malloc((usize)cap);
    if (!buf) return -1;
    int got = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, buf, cap);
    free(buf);
    if (got <= 0) return -1;
    return got / (int)sizeof(struct proc_fdinfo);
#else
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    i32 n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        n++;
    }
    closedir(d);
    /* The opendir handle itself is one of them and is about to go. */
    return n > 0 ? n - 1 : n;
#endif
}

/* ---- threads ------------------------------------------------------------ */

static i32 count_threads(void) {
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
    thread_act_array_t list;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) return -1;
    for (mach_msg_type_number_t i = 0; i < n; i++)
        mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(thread_act_t));
    return (i32)n;
#else
    DIR *d = opendir("/proc/self/task");
    if (!d) return -1;
    i32 n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
#endif
}

/* ---- the process tree ---------------------------------------------------- */

#if defined(PLATFORM_MACOS) || defined(__APPLE__)
/* One process's phys_footprint. proc_pid_rusage is the same accounting the
 * kernel bills the process with, so a child measured here is directly
 * comparable to this process's own TASK_VM_INFO.phys_footprint. */
static u64 proc_footprint(pid_t pid) {
    struct rusage_info_v4 ri;
    if (proc_pid_rusage((int)pid, RUSAGE_INFO_V4, (rusage_info_t *)&ri) != 0)
        return 0;
    return (u64)ri.ri_phys_footprint;
}

/* Direct children of `pid` appended to `out`. Returns how many were added. */
static i32 kids_of(pid_t pid, pid_t *out, i32 cap) {
    if (cap <= 0) return 0;
    int bytes = proc_listpids(PROC_PPID_ONLY, (uint32_t)pid, NULL, 0);
    if (bytes <= 0) return 0;
    int alloc = bytes + 32 * (int)sizeof(pid_t);
    pid_t *tmp = (pid_t *)malloc((usize)alloc);
    if (!tmp) return 0;
    int got = proc_listpids(PROC_PPID_ONLY, (uint32_t)pid, tmp, alloc);
    int n = got > 0 ? got / (int)sizeof(pid_t) : 0;
    i32 added = 0;
    for (int i = 0; i < n && added < cap; i++) {
        if (tmp[i] <= 0 || tmp[i] == pid) continue;   /* the query can echo the parent */
        out[added++] = tmp[i];
    }
    free(tmp);
    return added;
}
#endif

#define SOAK_TREE_MAX_PROCS 512
#define SOAK_TREE_MAX_DEPTH 8

void soak_probe_tree(SoakTree *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
    pid_t frontier[SOAK_TREE_MAX_PROCS], next[SOAK_TREE_MAX_PROCS];
    i32 nf = kids_of(getpid(), frontier, SOAK_TREE_MAX_PROCS);
    if (nf >= SOAK_TREE_MAX_PROCS) out->truncated = true;
    for (i32 depth = 1; depth <= SOAK_TREE_MAX_DEPTH && nf > 0; depth++) {
        out->depth_max = depth;
        i32 nn = 0;
        for (i32 i = 0; i < nf; i++) {
            if (out->procs >= SOAK_TREE_MAX_PROCS) { out->truncated = true; break; }
            out->procs++;
            out->bytes += proc_footprint(frontier[i]);
            if (depth < SOAK_TREE_MAX_DEPTH && nn < SOAK_TREE_MAX_PROCS)
                nn += kids_of(frontier[i], next + nn, SOAK_TREE_MAX_PROCS - nn);
            else if (depth == SOAK_TREE_MAX_DEPTH && !out->truncated) {
                /* At the depth limit, probe whether the last-level process has
                 * children. The collection call above stops at MAX, so nn
                 * remains zero at this depth and cannot indicate truncation.
                 * Probe one child and discard the result; a match sets the
                 * truncation flag. */
                pid_t probe[1];
                if (kids_of(frontier[i], probe, 1) > 0) out->truncated = true;
            }
        }
        if (depth == SOAK_TREE_MAX_DEPTH && nn > 0) out->truncated = true;
        memcpy(frontier, next, (usize)nn * sizeof(pid_t));
        nf = nn;
    }
#else
    /* Linux would walk /proc's ppid field the way count_children does; the
     * audit this serves is macOS-only, so it reports "unavailable" rather
     * than a wrong zero that reads like "no children". */
    out->procs = -1;
#endif
}

/* ---- children and zombies ----------------------------------------------- */

static void count_children(i32 *out_children, i32 *out_zombies) {
    *out_children = 0; *out_zombies = 0;
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
    pid_t me = getpid();
    int bytes = proc_listpids(PROC_PPID_ONLY, (uint32_t)me, NULL, 0);
    if (bytes <= 0) return;
    int cap = bytes + 32 * (int)sizeof(pid_t);
    pid_t *pids = (pid_t *)malloc((usize)cap);
    if (!pids) return;
    int got = proc_listpids(PROC_PPID_ONLY, (uint32_t)me, pids, cap);
    int n = got > 0 ? got / (int)sizeof(pid_t) : 0;
    for (int i = 0; i < n; i++) {
        if (pids[i] <= 0) continue;
        (*out_children)++;
        struct kinfo_proc kp;
        usize len = sizeof kp;
        int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pids[i] };
        if (sysctl(mib, 4, &kp, &len, NULL, 0) == 0 && len > 0 &&
            kp.kp_proc.p_stat == SZOMB) (*out_zombies)++;
    }
    free(pids);
#else
    DIR *d = opendir("/proc");
    if (!d) return;
    pid_t me = getpid();
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char path[64];
        snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[512];
        char *got = fgets(line, sizeof line, f);
        fclose(f);
        if (!got) continue;
        char *rp = strrchr(line, ')');
        if (!rp || !rp[1]) continue;
        char st = 0; int ppid = 0;
        if (sscanf(rp + 1, " %c %d", &st, &ppid) != 2) continue;
        if ((pid_t)ppid != me) continue;
        (*out_children)++;
        if (st == 'Z') (*out_zombies)++;
    }
    closedir(d);
#endif
}

/* ---- thread census ------------------------------------------------------ */

void soak_probe_print_threads(const char *label) {
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
    enum { NAMES = 64, NAMELEN = 72 };
    char names[NAMES][NAMELEN];
    i32  counts[NAMES];
    i32  n_names = 0;

    /* mach's THREAD_EXTENDED_INFO, not proc_pidinfo's PROC_PIDTHREADID64INFO:
     * the latter's pth_name is not populated for threads named through
     * pthread_setname_np, which is how every thread in this tree and most
     * framework threads are named, so it reports them all as "(unnamed)". */
    thread_act_array_t list;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) {
        printf("  thread census: unavailable\n");
        return;
    }
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        thread_extended_info_data_t xi;
        mach_msg_type_number_t cnt = THREAD_EXTENDED_INFO_COUNT;
        const char *nm = "(unnamed)";
        if (thread_info(list[i], THREAD_EXTENDED_INFO, (thread_info_t)&xi, &cnt)
            == KERN_SUCCESS && xi.pth_name[0]) nm = xi.pth_name;
        i32 slot = -1;
        for (i32 k = 0; k < n_names; k++)
            if (strncmp(names[k], nm, NAMELEN - 1) == 0) { slot = k; break; }
        if (slot < 0 && n_names < NAMES) {
            slot = n_names++;
            snprintf(names[slot], NAMELEN, "%s", nm);
            counts[slot] = 0;
        }
        if (slot >= 0) counts[slot]++;
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(thread_act_t));

    printf("  thread census (%s): %u live\n", label ? label : "", (unsigned)n);
    for (i32 k = 0; k < n_names; k++)
        printf("      %4d  %s\n", counts[k], names[k]);
#else
    (void)label;
#endif
}

void soak_probe_print_fds(const char *label) {
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
    pid_t pid = getpid();
    int bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, NULL, 0);
    if (bytes <= 0) return;
    int cap = bytes + 64 * (int)sizeof(struct proc_fdinfo);
    struct proc_fdinfo *buf = (struct proc_fdinfo *)malloc((usize)cap);
    if (!buf) return;
    int got = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, buf, cap);
    int n = got > 0 ? got / (int)sizeof(struct proc_fdinfo) : 0;
    printf("  descriptors (%s): %d\n", label ? label : "", n);
    for (int i = 0; i < n; i++) {
        const char *kind = "?";
        char detail[MAXPATHLEN] = {0};
        switch (buf[i].proc_fdtype) {
        case PROX_FDTYPE_VNODE: {
            kind = "vnode";
            struct vnode_fdinfowithpath vi;
            if (proc_pidfdinfo(pid, buf[i].proc_fd, PROC_PIDFDVNODEPATHINFO,
                               &vi, sizeof vi) == (int)sizeof vi)
                snprintf(detail, sizeof detail, "%s", vi.pvip.vip_path);
            break;
        }
        case PROX_FDTYPE_SOCKET:  kind = "socket"; break;
        case PROX_FDTYPE_PIPE:    kind = "pipe";   break;
        case PROX_FDTYPE_KQUEUE:  kind = "kqueue"; break;
        case PROX_FDTYPE_PSHM:    kind = "shm";    break;
        case PROX_FDTYPE_PSEM:    kind = "sem";    break;
        default: break;
        }
        printf("      fd %-5d %-7s %s\n", buf[i].proc_fd, kind, detail);
    }
    free(buf);
#else
    (void)label;
#endif
}

void soak_probe_print_children(const char *label) {
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
    pid_t me = getpid();
    int bytes = proc_listpids(PROC_PPID_ONLY, (uint32_t)me, NULL, 0);
    if (bytes <= 0) { printf("  children (%s): none\n", label ? label : ""); return; }
    int cap = bytes + 32 * (int)sizeof(pid_t);
    pid_t *pids = (pid_t *)malloc((usize)cap);
    if (!pids) return;
    int got = proc_listpids(PROC_PPID_ONLY, (uint32_t)me, pids, cap);
    int n = got > 0 ? got / (int)sizeof(pid_t) : 0;
    printf("  children (%s): %d\n", label ? label : "", n);
    for (int i = 0; i < n; i++) {
        if (pids[i] <= 0) continue;
        struct kinfo_proc kp;
        usize len = sizeof kp;
        int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pids[i] };
        const char *st = "?";
        const char *nm = "?";
        if (sysctl(mib, 4, &kp, &len, NULL, 0) == 0 && len > 0) {
            switch (kp.kp_proc.p_stat) {
                case SIDL:   st = "idle";    break;
                case SRUN:   st = "run";     break;
                case SSLEEP: st = "sleep";   break;
                case SSTOP:  st = "stopped"; break;
                case SZOMB:  st = "ZOMBIE";  break;
                default: break;
            }
            nm = kp.kp_proc.p_comm;
        }
        printf("      pid %-7d %-8s %s\n", (int)pids[i], st, nm);
    }
    free(pids);
#else
    (void)label;
#endif
}

/* ---- sleep-snapshot leftovers ------------------------------------------- */

void soak_probe_count_snapshots(i32 *out_files, usize *out_bytes) {
    i32 files = 0; usize bytes = 0;
    char dir[1024];                          /* where ui/sleep_snap.c keeps them */
    if (!liu_private_tmp_dir(dir, sizeof dir, "liu-sleep")) snprintf(dir, sizeof dir, "/tmp");
    char prefix[64];
    snprintf(prefix, sizeof prefix, "liu-sleep-%d-", (int)getpid());
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, prefix, strlen(prefix)) != 0) continue;
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            struct stat st;
            if (stat(path, &st) == 0) { files++; bytes += (usize)st.st_size; }
        }
        closedir(d);
    }
    if (out_files) *out_files = files;
    if (out_bytes) *out_bytes = bytes;
}

/* ---- the sample --------------------------------------------------------- */

/* ---- what the instrument itself costs ------------------------------------
 *
 * soak_probe_take walks the whole task VM map, asks libmalloc for zone
 * statistics, enumerates the child processes and reads two task_info blocks.
 * That is tens of milliseconds, and the soak calls it from inside the frame —
 * not only at an epoch boundary through take_sample, but on EVERY tick while
 * quiescing(), which polls it to decide whether the process has settled.
 *
 * Next to an idle loop's per-iteration work that would own the p99 and the max
 * of the frame probe and be read as Liu's latency tail, so the probe's own
 * time is accumulated here (soak_probe_overhead_take). It is timed HERE, so
 * every caller is covered by construction and a new call site cannot
 * reintroduce the contamination.
 *
 * The accumulator is process-global and drained by whoever is reporting. A
 * benchmark that never drains it simply never reads it. */
static f64 g_probe_overhead_sec = 0.0;

f64 soak_probe_overhead_take(void) {
    f64 v = g_probe_overhead_sec;
    g_probe_overhead_sec = 0.0;
    return v;
}

void soak_probe_take(SoakSample *out) {
    f64 liu__t0 = soak_probe_now();
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->t_sec = soak_probe_now();
    read_memory(&out->phys_footprint, &out->resident);
    read_malloc(&out->malloc_in_use, &out->malloc_reserved, &out->malloc_blocks);
    vmr_snapshot(&out->vm);
    for (i32 r = 0; r < MEM_RESOURCE_COUNT; r++) {
        out->ledger[r]      = mem_budget_get((MemResource)r);
        out->ledger_peak[r] = mem_budget_peak((MemResource)r);
        out->ledger_total += out->ledger[r];
    }
    out->fds         = count_fds();
    out->threads     = count_threads();
    out->pty_readers = pty_reader_live_count();
    count_children(&out->children, &out->zombies);
    tab_trash_stats(&out->trash_items, &out->trash_bytes, &out->trash_oldest_sec);
    soak_probe_count_snapshots(&out->snap_files, &out->snap_bytes);
    g_probe_overhead_sec += soak_probe_now() - liu__t0;
}

/* ---- quiescence ---------------------------------------------------------- */

static i32 g_settle_last  = -1;
static f64 g_settle_since = 0.0;

void soak_children_settled_reset(void) {
    g_settle_last  = -1;
    g_settle_since = 0.0;
}

bool soak_children_settled(f64 min_gap_sec) {
    i32 children = 0, zombies = 0;
    count_children(&children, &zombies);
    f64 now = soak_probe_now();

    if (zombies > 0) {
        /* An unreaped child is still ours; the count cannot be called stable
         * while one is waiting to be collected. Restart the clock rather than
         * merely returning false, so a run of reaps does not accumulate
         * credit toward the gap. */
        g_settle_last  = -1;
        g_settle_since = now;
        return false;
    }
    if (children != g_settle_last) {
        g_settle_last  = children;
        g_settle_since = now;
        return false;
    }
    return (now - g_settle_since) >= min_gap_sec;
}

/* Ownership and footprint are different measurements. GPU surfaces, mappings
 * and malloc allocations all appear in the ownership ledger; their residency
 * cannot be recovered by subtracting that ledger from a VM bucket. The kernel
 * counters below can also overlap, so report them without inventing a sum.
 * Reported, never gated. */
void soak_probe_print_ram(const char *label, const SoakSample *s) {
    if (!s) return;
    f64 phys = (f64)s->phys_footprint / 1048576.0;
    f64 ledg = (f64)s->ledger_total   / 1048576.0;
    printf("\n  --- RAM reconciliation (%s) ---\n", label);
    printf("  %-22s %9.2f MiB\n", "phys_footprint", phys);
    printf("  %-22s %9.2f MiB\n", "mem_budget_total", ledg);
    printf("  %-22s %9.2f MiB   arithmetic only; ownership is not residency\n",
           "raw_gap", phys - ledg);
    if (!s->vm.available) {
        printf("  (no VM decomposition on this platform)\n");
        return;
    }
    printf("  %-22s %9.2f MiB   dirty + swapped over %u walked regions\n",
           "walked private dirty", s->vm.total_dirty / 1048576.0,
           s->vm.region_count);
    for (i32 i = 0; i < VMR_COUNT; i++) {
        u64 b = s->vm.dirty[i] + s->vm.swapped[i];
        if (b == 0) continue;
        printf("      %-18s %9.2f MiB  (%u regions)\n",
               vmr_category_name((VmrCategory)i), b / 1048576.0, s->vm.regions[i]);
    }
    printf("  Kernel counters (may overlap; not a footprint decomposition):\n");
    printf("  %-22s %9.2f MiB   anonymous charged to the task\n",
           "internal", s->vm.internal / 1048576.0);
    printf("  %-22s %9.2f MiB   held by the compressor, still charged\n",
           "compressed", s->vm.compressed / 1048576.0);
    printf("  %-22s %9.2f MiB   device (GPU) memory\n",
           "device", s->vm.device / 1048576.0);
    if (s->vm.gfx_footprint || s->vm.media_footprint)
        printf("  %-22s %9.2f MiB / %.2f MiB   graphics / media ledger tags\n",
               "gfx + media", s->vm.gfx_footprint / 1048576.0,
               s->vm.media_footprint / 1048576.0);
    printf("  Surface count and sizes require presenter telemetry; these VM "
           "counters do not identify individual drawables.\n");

    /* The process tree reports memory used by descendants such as shells,
     * Site commands and browser processes. Report it separately from Liu's
     * footprint because the ledger tracks memory Liu owns and can reclaim.
     * System monitors may show the combined total, so both values are
     * printed. */
    SoakTree tree;
    soak_probe_tree(&tree);
    if (tree.procs > 0)
        printf("  %-22s %9.2f MiB   over %d descendant processes, %d levels "
               "deep%s (NOT ours to free — reported, never charged)\n",
               "process tree", tree.bytes / 1048576.0, tree.procs,
               tree.depth_max, tree.truncated ? ", TRUNCATED" : "");
    else if (tree.procs == 0)
        printf("  %-22s %9s        no descendants\n", "process tree", "-");
}

/* ---- output ------------------------------------------------------------- */

void soak_probe_write_jsonl(FILE *f, const char *scenario, const char *phase,
                            const SoakSample *s) {
    if (!f || !s) return;
    fprintf(f,
        "{\"scenario\":\"%s\",\"phase\":\"%s\",\"epoch\":%d,\"t\":%.3f,\"ops\":%llu,"
        "\"phys_footprint\":%llu,\"resident\":%llu,"
        "\"malloc_in_use\":%zu,\"malloc_reserved\":%zu,\"malloc_blocks\":%llu,"
        "\"ledger_total\":%zu,"
        "\"fds\":%d,\"threads\":%d,\"pty_readers\":%d,\"children\":%d,\"zombies\":%d,"
        "\"trash_items\":%d,\"trash_bytes\":%zu,\"trash_oldest\":%.3f,"
        "\"sites\":%d,\"sites_running\":%d,\"snap_files\":%d,\"snap_bytes\":%zu,"
        "\"errors\":%llu,\"timeouts\":%llu,\"dropped\":%llu,\"duplicated\":%llu,"
        "\"unexpected_wakes\":%llu,\"ledger\":{",
        scenario ? scenario : "?", phase ? phase : "?", s->epoch, s->t_sec,
        (unsigned long long)s->ops,
        (unsigned long long)s->phys_footprint, (unsigned long long)s->resident,
        s->malloc_in_use, s->malloc_reserved, (unsigned long long)s->malloc_blocks,
        s->ledger_total, s->fds, s->threads, s->pty_readers, s->children,
        s->zombies, s->trash_items, s->trash_bytes, s->trash_oldest_sec,
        s->sites, s->sites_running, s->snap_files, s->snap_bytes,
        (unsigned long long)s->errors, (unsigned long long)s->timeouts,
        (unsigned long long)s->bytes_dropped, (unsigned long long)s->bytes_duplicated,
        (unsigned long long)s->unexpected_wakes);
    for (i32 r = 0; r < MEM_RESOURCE_COUNT; r++)
        fprintf(f, "%s\"%s\":%zu", r ? "," : "",
                mem_budget_name((MemResource)r), s->ledger[r]);
    fprintf(f, "}}\n");
    /* Flushed every line: a soak that is killed — by a watchdog, by the user,
     * by the thing it was hunting — must leave a usable series behind. */
    fflush(f);
}

void soak_probe_print_header(void) {
    printf("  %-5s %8s %10s %11s %11s %10s %11s %6s %8s %7s %6s %6s\n",
           "epoch", "t(s)", "ops", "phys(MiB)", "inuse(MiB)", "resv(MiB)",
           "ledger(KiB)", "fds", "threads", "readers", "child", "zomb");
}

void soak_probe_print(const char *scenario, const SoakSample *s) {
    if (!s) return;
    (void)scenario;
    /* The zone's size_allocated reports reserved address space, most of which
     * may be untouched. Print it alongside physical footprint and in-use
     * memory so the values can be compared; do not treat reserved size as
     * resident memory. */
    printf("  %-5d %8.1f %10llu %11.2f %11.2f %10.2f %11zu %6d %8d %7d %6d %6d\n",
           s->epoch, s->t_sec, (unsigned long long)s->ops,
           (f64)s->phys_footprint / (1024.0 * 1024.0),
           (f64)s->malloc_in_use / (1024.0 * 1024.0),
           (f64)s->malloc_reserved / (1024.0 * 1024.0),
           s->ledger_total / 1024,
           s->fds, s->threads, s->pty_readers, s->children, s->zombies);
}
