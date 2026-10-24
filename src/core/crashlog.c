#include "core/crashlog.h"
#include "core/config.h"
#include "core/mkdir_p.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <dirent.h>
#include <stdint.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#else
#include <sys/utsname.h>
#endif

/* backtrace() is in libSystem and glibc; musl has no <execinfo.h>. */
#if defined(__has_include)
#  if __has_include(<execinfo.h>)
#    include <execinfo.h>
#    define CRASHLOG_HAVE_BACKTRACE 1
#  endif
#endif
#ifndef CRASHLOG_HAVE_BACKTRACE
#define CRASHLOG_HAVE_BACKTRACE 0
#endif

#ifndef LIU_VERSION
#define LIU_VERSION "unknown"
#endif
#ifndef LIU_BUILD
#define LIU_BUILD "unknown"
#endif

/* Resolved crasherrors/ directory and the pre-built path this run's signal
 * handler writes to (built at init so the handler itself touches no allocator). */
static char g_dir[1024]      = {0};
static char g_sig_path[1280] = {0};
static volatile sig_atomic_t g_exception_recorded = 0;
static volatile sig_atomic_t g_initialized        = 0;
static volatile sig_atomic_t g_handling_signal    = 0;

/* What a report needs besides the reason, as text, built ONCE at init: a
 * signal handler may not format anything. Without it a report could not say
 * which build, architecture and macOS crashed. */
static char g_header[512] = {0};
/* The handler's own stack, so a stack overflow on the main thread — where
 * there is no room left to run a handler — still leaves a report. */
#define CRASHLOG_ALTSTACK_BYTES (128u * 1024u)
#define CRASHLOG_MAX_FRAMES 64

/* Keep at most this many reports around so the folder never grows without
 * bound while still preserving a useful history. */
#define CRASHLOG_KEEP_MAX 50

const char *crashlog_dir(void) { return g_dir; }

/* ---- async-signal-safe write helpers (signal context) ------------------- */

static void safe_write(int fd, const char *s) {
    if (!s) return;
    usize n = 0;
    while (s[n]) n++;
    usize off = 0;
    while (off < n) {
        ssize_t w = write(fd, s + off, n - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        off += (usize)w;
    }
}

static const char *sig_reason(int sig) {
    switch (sig) {
        case SIGSEGV: return "Signal SIGSEGV (segmentation fault — invalid memory access)";
        case SIGABRT: return "Signal SIGABRT (abort — failed assertion / uncaught exception / abort())";
        case SIGBUS:  return "Signal SIGBUS (bus error — misaligned or invalid memory access)";
        case SIGILL:  return "Signal SIGILL (illegal instruction)";
        case SIGFPE:  return "Signal SIGFPE (arithmetic error — divide by zero / overflow)";
#ifdef SIGTRAP
        case SIGTRAP: return "Signal SIGTRAP (trace trap)";
#endif
        default:      return "Signal (fatal)";
    }
}

/* "0x" + sixteen hex digits, with nothing but write(). */
static void safe_write_hex(int fd, uintptr_t v) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        unsigned d = (unsigned)((v >> ((15 - i) * 4)) & 0xF);
        buf[2 + i] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
    }
    buf[18] = '\0';
    safe_write(fd, buf);
}

/* The instruction that faulted, out of the interrupted thread's saved
 * registers. backtrace() does not have it: it walks frame pointers from the
 * handler, and a function that faults before (or without) building its frame
 * is simply absent from that walk. */
static void *crash_pc(void *uctx) {
    if (!uctx) return NULL;
#if defined(__APPLE__) && defined(__aarch64__)
    return (void *)(uintptr_t)__darwin_arm_thread_state64_get_pc(
        ((ucontext_t *)uctx)->uc_mcontext->__ss);
#elif defined(__APPLE__) && defined(__x86_64__)
    return (void *)(uintptr_t)((ucontext_t *)uctx)->uc_mcontext->__ss.__rip;
#elif defined(__linux__) && defined(__aarch64__)
    return (void *)(uintptr_t)((ucontext_t *)uctx)->uc_mcontext.pc;
#elif defined(__linux__) && defined(__x86_64__) && defined(REG_RIP)
    return (void *)(uintptr_t)((ucontext_t *)uctx)->uc_mcontext.gregs[REG_RIP];
#else
    return NULL;
#endif
}

static void crash_sig_handler(int sig, siginfo_t *info, void *uctx) {
    /* A second fault while handling the first one must not recurse through
     * libc or overwrite the report indefinitely. */
    if (g_handling_signal) _exit(128 + sig);
    g_handling_signal = 1;

    /* An NSException already wrote a richer report and then abort()ed; don't
     * clobber it with the generic SIGABRT note — just let the process die. */
    if (g_exception_recorded && sig == SIGABRT) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }

    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(g_sig_path, flags, 0600);
    if (fd >= 0) {
        /* First line is the one-line reason the next launch surfaces. It is
         * on disk before anything below runs: the stack walk reads a stack
         * that may be the thing that is broken. */
        safe_write(fd, sig_reason(sig));
        safe_write(fd, "\n\n");
        safe_write(fd, g_header);
#if CRASHLOG_HAVE_BACKTRACE
        /* Symbolising takes the loader's lock, and the crash may have happened
         * under it. A process that hangs here is worse than one that died
         * without a stack, so the walk gets three seconds. */
        signal(SIGALRM, SIG_DFL);
        alarm(3);
        if (info && (sig == SIGSEGV || sig == SIGBUS)) {
            safe_write(fd, "\nFault address ");
            safe_write_hex(fd, (uintptr_t)info->si_addr);
            safe_write(fd, "\n");
        }
        void *pc = crash_pc(uctx);
        if (pc) {
            safe_write(fd, "\nCrashed at:\n");
            backtrace_symbols_fd(&pc, 1, fd);
        }
        safe_write(fd, "\nBacktrace of the crashing thread:\n");
        void *frames[CRASHLOG_MAX_FRAMES];
        int nframes = backtrace(frames, CRASHLOG_MAX_FRAMES);
        if (nframes > 0) backtrace_symbols_fd(frames, nframes, fd);
        alarm(0);
#else
        (void)info; (void)uctx;
#endif
        close(fd);
    }

    /* Re-raise so the OS still produces its own crash report and the process
     * terminates with the right status. SA_RESETHAND already restored the
     * default disposition. */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ---- the report's header (normal context, at init) ---------------------- */

static void header_append(usize *len, const char *fmt, const char *a, const char *b,
                          const char *c) {
    if (*len >= sizeof g_header) return;
    int n = snprintf(g_header + *len, sizeof g_header - *len, fmt, a, b, c);
    if (n > 0) *len += (usize)n < sizeof g_header - *len ? (usize)n
                                                         : sizeof g_header - *len - 1u;
}

static void build_header(void) {
    usize len = 0;
#if defined(__aarch64__)
    const char *arch = "arm64";
#elif defined(__x86_64__)
    const char *arch = "x86_64";
#else
    const char *arch = "unknown-arch";
#endif
    header_append(&len, "Liu %s (%s) %s", LIU_VERSION, LIU_BUILD, arch);

#if defined(__APPLE__)
    /* An Intel build running under Rosetta is its own situation. */
    int translated = 0;
    size_t tsz = sizeof translated;
    if (sysctlbyname("sysctl.proc_translated", &translated, &tsz, NULL, 0) == 0 &&
        translated == 1)
        header_append(&len, "%s%s%s", " (translated by Rosetta)", "", "");
    char os[64] = "?", osbuild[64] = "?";
    size_t sz = sizeof os;
    (void)sysctlbyname("kern.osproductversion", os, &sz, NULL, 0);
    sz = sizeof osbuild;
    (void)sysctlbyname("kern.osversion", osbuild, &sz, NULL, 0);
    os[sizeof os - 1] = osbuild[sizeof osbuild - 1] = '\0';
    header_append(&len, "\nmacOS %s (%s)%s", os, osbuild, "");
#else
    struct utsname un;
    if (uname(&un) == 0)
        header_append(&len, "\n%s %s %s", un.sysname, un.release, un.machine);
#endif

    char when[64] = "?";
    time_t now = time(NULL);
    struct tm tmv;
    if (localtime_r(&now, &tmv))
        (void)strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S %z", &tmv);
    char pid[32];
    snprintf(pid, sizeof pid, "%ld", (long)getpid());
    header_append(&len, "\nLaunched %s, pid %s%s", when, pid, "");

#if defined(__APPLE__)
    /* Where the executable was loaded: with it, the frame addresses below
     * resolve against the released binary (`atos -o Liu -l <base>`). */
    char base[32];
    snprintf(base, sizeof base, "%p", (const void *)_dyld_get_image_header(0));
    header_append(&len, "\nImage base %s%s%s", base, "", "");
#endif
    header_append(&len, "%s%s%s", "\n", "", "");
}

/* Give the calling thread a signal stack unless it already has one (a
 * sanitizer runtime installs its own, and frees what it finds there). */
static void install_altstack(void) {
    stack_t cur;
    if (sigaltstack(NULL, &cur) == 0 && !(cur.ss_flags & SS_DISABLE) &&
        cur.ss_size >= CRASHLOG_ALTSTACK_BYTES / 2u)
        return;
    void *mem = mmap(NULL, CRASHLOG_ALTSTACK_BYTES, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) return;
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = mem;
    ss.ss_size = CRASHLOG_ALTSTACK_BYTES;
    if (sigaltstack(&ss, NULL) != 0) (void)munmap(mem, CRASHLOG_ALTSTACK_BYTES);
}

/* ---- dir helpers -------------------------------------------------------- */

/* Delete oldest reports beyond CRASHLOG_KEEP_MAX (counts both .log and .seen). */
static void prune_old(void) {
    DIR *d = opendir(g_dir);
    if (!d) return;
    struct CrashEntry {
        char name[256];
        time_t mt;
    } keep[CRASHLOG_KEEP_MAX];
    i32 count = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (strncmp(de->d_name, "crash-", 6) != 0) continue;
        if (strlen(de->d_name) >= sizeof keep[0].name) continue;
        char full[1536];
        int n = snprintf(full, sizeof full, "%s/%s", g_dir, de->d_name);
        if (n < 0 || (usize)n >= sizeof full) continue;
        struct stat st;
        if (lstat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if (count < CRASHLOG_KEEP_MAX) {
            memcpy(keep[count].name, de->d_name, strlen(de->d_name) + 1u);
            keep[count].mt = st.st_mtime;
            count++;
            continue;
        }

        /* Keep only the newest N entries while scanning, which also bounds
         * a directory with thousands of stale reports. */
        i32 oldest = 0;
        for (i32 i = 1; i < count; i++) {
            if (keep[i].mt < keep[oldest].mt ||
                (keep[i].mt == keep[oldest].mt &&
                 strcmp(keep[i].name, keep[oldest].name) < 0)) oldest = i;
        }
        bool current_is_newer =
            st.st_mtime > keep[oldest].mt ||
            (st.st_mtime == keep[oldest].mt &&
             strcmp(de->d_name, keep[oldest].name) > 0);
        if (current_is_newer) {
            char old_full[1536];
            n = snprintf(old_full, sizeof old_full, "%s/%s",
                         g_dir, keep[oldest].name);
            if (n >= 0 && (usize)n < sizeof old_full) (void)unlink(old_full);
            memcpy(keep[oldest].name, de->d_name, strlen(de->d_name) + 1u);
            keep[oldest].mt = st.st_mtime;
        } else {
            (void)unlink(full);
        }
    }
    closedir(d);
}

void crashlog_init(void) {
    if (g_initialized) return;

    const char *base = config_user_dir();
    if (!base || !base[0]) return;
    /* ~/.config/Liu/crasherrors, with every parent that is missing: a new
     * account has no ~/.config, and one mkdir() cannot make two levels. */
    int n = snprintf(g_dir, sizeof g_dir, "%s/crasherrors", base);
    if (n < 0 || (usize)n >= sizeof g_dir || !liu_mkdir_p(g_dir, 0700)) {
        g_dir[0] = '\0';
        return;
    }

    /* Pre-build this run's signal-report path (unique per pid + launch time) so
     * the signal handler does zero formatting/allocation. */
    n = snprintf(g_sig_path, sizeof g_sig_path, "%s/crash-%ld-%ld.log",
                 g_dir, (long)getpid(), (long)time(NULL));
    if (n < 0 || (usize)n >= sizeof g_sig_path) {
        g_sig_path[0] = '\0';
        return;
    }

    prune_old();
    build_header();
    install_altstack();
#if CRASHLOG_HAVE_BACKTRACE
    /* The first backtrace() may load and bind the unwinder; do that here, not
     * inside the handler. */
    { void *warm[2]; (void)backtrace(warm, 2); }
#endif

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_sig_handler;
    sigemptyset(&sa.sa_mask);
    /* One-shot (the default disposition is restored once the handler runs),
     * on the signal stack, with the interrupted thread's registers. */
    sa.sa_flags = SA_RESETHAND | SA_ONSTACK | SA_SIGINFO;
    const int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGTRAP };
    for (usize i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        sigaction(sigs[i], &sa, NULL);
    }

    g_initialized = 1;
}

void crashlog_record_exception(const char *name, const char *reason,
                               const char *backtrace_text) {
    if (!g_initialized || !g_sig_path[0]) return;

    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(g_sig_path, flags, 0600);
    if (fd < 0) return;

    /* First line = one-line reason: "<Name>: <reason>". */
    if (name && name[0]) { safe_write(fd, name); safe_write(fd, ": "); }
    safe_write(fd, (reason && reason[0]) ? reason : "uncaught exception");
    safe_write(fd, "\n\n");
    safe_write(fd, g_header);
    safe_write(fd, "\nCall stack:\n");
    if (backtrace_text && backtrace_text[0]) safe_write(fd, backtrace_text);
    close(fd);

    /* Tell the SIGABRT that follows (from the default exception handler's
     * abort()) not to overwrite this richer report. */
    g_exception_recorded = 1;
}

/* ---- pending (shown on next launch) ------------------------------------- */

static bool read_first_line(const char *path, char *out, usize cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return false;
    int read_cap = cap > (usize)INT_MAX ? INT_MAX : (int)cap;
    bool got_line = fgets(out, read_cap, f) != NULL;
    if (got_line) {
        usize n = strlen(out);
        while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r')) out[--n] = '\0';
    }
    fclose(f);
    return got_line;
}

bool crashlog_take_pending(char *reason_out, usize reason_cap,
                           char *path_out, usize path_cap) {
    if (!g_dir[0]) return false;
    DIR *d = opendir(g_dir);
    if (!d) return false;

    char  newest_name[256] = {0};
    time_t newest_mt = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (strncmp(de->d_name, "crash-", 6) != 0) continue;
        usize ln = strlen(de->d_name);
        if (ln < 4 || strcmp(de->d_name + ln - 4, ".log") != 0) continue; /* skip .seen */

        if (strlen(de->d_name) >= sizeof newest_name) continue;

        char full[1536];
        int n = snprintf(full, sizeof full, "%s/%s", g_dir, de->d_name);
        if (n < 0 || (usize)n >= sizeof full) continue;
        struct stat st;
        if (lstat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (newest_name[0] == '\0' || st.st_mtime >= newest_mt) {
            newest_mt = st.st_mtime;
            memcpy(newest_name, de->d_name, strlen(de->d_name) + 1u);
        }
    }
    closedir(d);

    if (newest_name[0] == '\0') return false;

    char newest_full[1536];
    int n = snprintf(newest_full, sizeof newest_full, "%s/%s", g_dir, newest_name);
    if (n < 0 || (usize)n >= sizeof newest_full) return false;
    if (path_out && path_cap) {
        n = snprintf(path_out, path_cap, "%s", newest_full);
        if (n < 0 || (usize)n >= path_cap) {
            path_out[0] = '\0';
            return false;
        }
    }
    if (reason_out && reason_cap) read_first_line(newest_full, reason_out, reason_cap);

    /* Mark every unseen report shown (rename .log -> .log.seen) in a second
     * directory pass. This has no arbitrary 64-file ceiling. */
    d = opendir(g_dir);
    if (!d) return true;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' || strncmp(de->d_name, "crash-", 6) != 0)
            continue;
        usize ln = strlen(de->d_name);
        if (ln < 4 || ln >= 256 || strcmp(de->d_name + ln - 4, ".log") != 0)
            continue;
        char from[1536], to[1600];
        int nf = snprintf(from, sizeof from, "%s/%s", g_dir, de->d_name);
        int nt = nf >= 0 && (usize)nf < sizeof from
               ? snprintf(to, sizeof to, "%s.seen", from) : -1;
        if (nf >= 0 && (usize)nf < sizeof from &&
            nt >= 0 && (usize)nt < sizeof to) (void)rename(from, to);
    }
    closedir(d);
    return true;
}
