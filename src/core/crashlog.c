#include "core/crashlog.h"
#include "core/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>

#include <execinfo.h>   /* backtrace / backtrace_symbols_fd */

/* Resolved crasherrors/ directory and the pre-built path this run's signal
 * handler writes to (built at init so the handler itself touches no allocator). */
static char g_dir[1024]      = {0};
static char g_sig_path[1280] = {0};
static volatile sig_atomic_t g_exception_recorded = 0;
static volatile sig_atomic_t g_initialized        = 0;

/* Keep at most this many reports around so the folder never grows without
 * bound while still preserving a useful history. */
#define CRASHLOG_KEEP_MAX 50

const char *crashlog_dir(void) { return g_dir; }

/* ---- async-signal-safe write helpers (signal context) ------------------- */

static void safe_write(int fd, const char *s) {
    if (!s) return;
    usize n = 0;
    while (s[n]) n++;
    ssize_t off = 0;
    while ((usize)off < n) {
        ssize_t w = write(fd, s + off, n - (usize)off);
        if (w <= 0) break;
        off += w;
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

static void crash_sig_handler(int sig) {
    /* An NSException already wrote a richer report and then abort()ed; don't
     * clobber it with the generic SIGABRT note — just let the process die. */
    if (g_exception_recorded && sig == SIGABRT) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }

    int fd = open(g_sig_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        /* First line is the one-line reason the next launch surfaces. */
        safe_write(fd, sig_reason(sig));
        safe_write(fd, "\n\nBacktrace:\n");
        void *bt[64];
        int n = backtrace(bt, 64);
        backtrace_symbols_fd(bt, n, fd);   /* writes straight to fd, no malloc */
        close(fd);
    }

    /* Re-raise so the OS still produces its own crash report and the process
     * terminates with the right status. SA_RESETHAND already restored the
     * default disposition. */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ---- dir helpers -------------------------------------------------------- */

static void ensure_dir(const char *path) {
    if (!path || !path[0]) return;
    mkdir(path, 0755);   /* ignore EEXIST */
}

/* Delete oldest reports beyond CRASHLOG_KEEP_MAX (counts both .log and .seen). */
static void prune_old(void) {
    DIR *d = opendir(g_dir);
    if (!d) return;
    struct {
        char name[256];
        time_t mt;
    } ents[256];
    i32 count = 0;
    struct dirent *de;
    while ((de = readdir(d)) && count < 256) {
        if (de->d_name[0] == '.') continue;
        if (strncmp(de->d_name, "crash-", 6) != 0) continue;
        char full[1536];
        snprintf(full, sizeof(full), "%s/%s", g_dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        snprintf(ents[count].name, sizeof(ents[count].name), "%s", de->d_name);
        ents[count].mt = st.st_mtime;
        count++;
    }
    closedir(d);
    if (count <= CRASHLOG_KEEP_MAX) return;
    /* selection-sort the oldest to the front, unlink until within the cap */
    for (i32 removed = 0; count - removed > CRASHLOG_KEEP_MAX; removed++) {
        i32 oldest = -1;
        for (i32 i = 0; i < count; i++) {
            if (ents[i].name[0] == '\0') continue;
            if (oldest < 0 || ents[i].mt < ents[oldest].mt) oldest = i;
        }
        if (oldest < 0) break;
        char full[1536];
        snprintf(full, sizeof(full), "%s/%s", g_dir, ents[oldest].name);
        unlink(full);
        ents[oldest].name[0] = '\0';
    }
}

void crashlog_init(void) {
    if (g_initialized) return;

    const char *base = config_user_dir();
    if (!base || !base[0]) return;
    ensure_dir(base);                                  /* ~/.config/Liu */
    snprintf(g_dir, sizeof(g_dir), "%s/crasherrors", base);
    ensure_dir(g_dir);

    /* Pre-build this run's signal-report path (unique per pid + launch time) so
     * the signal handler does zero formatting/allocation. */
    snprintf(g_sig_path, sizeof(g_sig_path), "%s/crash-%ld-%ld.log",
             g_dir, (long)getpid(), (long)time(NULL));

    prune_old();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;   /* one-shot: restore default after we run */
    const int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGTRAP };
    for (usize i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        sigaction(sigs[i], &sa, NULL);
    }

    g_initialized = 1;
}

void crashlog_record_exception(const char *name, const char *reason,
                               const char *backtrace_text) {
    if (!g_initialized || !g_sig_path[0]) return;

    int fd = open(g_sig_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;

    /* First line = one-line reason: "<Name>: <reason>". */
    if (name && name[0]) { safe_write(fd, name); safe_write(fd, ": "); }
    safe_write(fd, (reason && reason[0]) ? reason : "uncaught exception");
    safe_write(fd, "\n\nCall stack:\n");
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
    if (fgets(out, (int)cap, f)) {
        usize n = strlen(out);
        while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r')) out[--n] = '\0';
    }
    fclose(f);
    return true;
}

bool crashlog_take_pending(char *reason_out, usize reason_cap,
                           char *path_out, usize path_cap) {
    if (!g_dir[0]) return false;
    DIR *d = opendir(g_dir);
    if (!d) return false;

    char  newest_name[256] = {0};
    time_t newest_mt = 0;
    /* Track all unseen reports so we can mark them shown in one pass. */
    char  unseen[64][256];
    i32   unseen_n = 0;

    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (strncmp(de->d_name, "crash-", 6) != 0) continue;
        usize ln = strlen(de->d_name);
        if (ln < 4 || strcmp(de->d_name + ln - 4, ".log") != 0) continue; /* skip .seen */

        if (unseen_n < 64)
            snprintf(unseen[unseen_n++], 256, "%s", de->d_name);

        char full[1536];
        snprintf(full, sizeof(full), "%s/%s", g_dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (newest_name[0] == '\0' || st.st_mtime >= newest_mt) {
            newest_mt = st.st_mtime;
            snprintf(newest_name, sizeof(newest_name), "%s", de->d_name);
        }
    }
    closedir(d);

    if (newest_name[0] == '\0') return false;

    char newest_full[1536];
    snprintf(newest_full, sizeof(newest_full), "%s/%s", g_dir, newest_name);
    if (reason_out && reason_cap) read_first_line(newest_full, reason_out, reason_cap);
    if (path_out && path_cap)     snprintf(path_out, path_cap, "%s", newest_full);

    /* Mark every unseen report shown (rename .log -> .log.seen) so none nag
     * again, while the files stay on disk for inspection. */
    for (i32 i = 0; i < unseen_n; i++) {
        char from[1536], to[1600];
        snprintf(from, sizeof(from), "%s/%s", g_dir, unseen[i]);
        snprintf(to,   sizeof(to),   "%s.seen", from);
        rename(from, to);
    }
    return true;
}
