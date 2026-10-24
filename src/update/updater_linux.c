/* updater_linux.c — platform shim for the in-app updater on Linux.
 *
 * updater.c owns the state machine, the feed parsing and the crypto; this file
 * only does the two things that are OS-shaped: moving bytes over the network,
 * and putting a downloaded tree on disk. Same split as updater_macos.m.
 *
 * THE ARTIFACT CONTRACT is the one scripts/install.sh implements, so that a
 * tarball installed by hand and a tarball installed by the updater land
 * identically:
 *
 *   - a gzipped tar with a top-level tree holding the `Liu` ELF, the
 *     `liu-notify` / `liu-history` helpers beside it, and the `assets/` tree
 *     (the default font is resolved as <exe_dir>/../assets/fonts and is
 *     mandatory at runtime);
 *   - a `VERSION` file in that tree. updater.c requires an independent
 *     confirmation that the downloaded build is strictly newer and refuses to
 *     install without one — trusting the feed's own version number for that
 *     would defeat the check. install.sh writes VERSION into the prefix; the
 *     tarball must carry it too, because this consumer reads it from there.
 *   - installed as $PREFIX/bin/Liu + $PREFIX/assets + $PREFIX/VERSION, where
 *     $PREFIX is two levels up from the running binary.
 *
 * Network I/O shells out to curl on a detached thread. That is what the macOS
 * shim does (NSTask + curl), it keeps this file free of a TLS dependency, and
 * the artifact is Ed25519-verified by updater.c regardless of who fetched it.
 */
#include "update/updater.h"
#include "update/relaunch_wait.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

/* ===================== small helpers ===================== */

/* snprintf that reports truncation rather than quietly handing back a shorter
 * string. Every use here builds a filesystem path, and a clipped path does not
 * name a shorter file — it names a different one, or nothing. */
static bool path_fmt(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n > 0 && (size_t)n < cap;
}


static bool path_is_dir(const char *p) {
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool path_is_file(const char *p) {
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* Directory holding the running binary. */
static bool exe_dir(char *out, size_t cap) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0 || (size_t)n >= sizeof exe) return false;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash || slash == exe) return false;
    *slash = '\0';
    int w = snprintf(out, cap, "%s", exe);
    return w > 0 && (size_t)w < cap;
}

/* $PREFIX for a tree laid out as $PREFIX/bin/Liu. Returns false when the
 * binary is not in a .../bin/ directory — a build tree, in other words. */
static bool install_prefix(char *out, size_t cap) {
    char dir[PATH_MAX];
    if (!exe_dir(dir, sizeof dir)) return false;
    const char *base = strrchr(dir, '/');
    if (!base || strcmp(base, "/bin") != 0) return false;
    size_t keep = (size_t)(base - dir);
    if (keep == 0 || keep >= cap) return false;
    memcpy(out, dir, keep);
    out[keep] = '\0';
    return true;
}

/* Run argv to completion. Returns the exit status, or -1 if it never ran.
 * `pid_out` (optional) is published before the wait so a cancel can signal. */
static int run_sync(char *const argv[], _Atomic long *pid_out) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid_out) atomic_store(pid_out, (long)pid);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (pid_out) atomic_store(pid_out, 0);
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

/* mkdtemp under $XDG_RUNTIME_DIR (falls back to /tmp). */
static bool make_temp_dir(char *out, size_t cap) {
    const char *base = getenv("XDG_RUNTIME_DIR");
    if (!base || !base[0] || !path_is_dir(base)) base = "/tmp";
    int n = snprintf(out, cap, "%s/liu-update-XXXXXX", base);
    if (n <= 0 || (size_t)n >= cap) return false;
    return mkdtemp(out) != NULL;
}

/* ===================== identity / capability ===================== */

const char *updater_plat_arch(void) {
    static char arch[80];   /* utsname.machine is 65 bytes on glibc */
    if (arch[0]) return arch;
    struct utsname u;
    if (uname(&u) != 0) { snprintf(arch, sizeof arch, "unknown"); return arch; }
    /* The feed spells the 64-bit ARM slice "arm64", as macOS does; Linux says
     * aarch64 for the same machine. */
    if (strcmp(u.machine, "aarch64") == 0) snprintf(arch, sizeof arch, "arm64");
    else                                   snprintf(arch, sizeof arch, "%s", u.machine);
    return arch;
}

bool updater_plat_os_at_least(const char *min_os) {
    /* minimumSystemVersion is a macOS notion — an OS release number every
     * install shares. Linux has no single such number (a kernel version says
     * nothing about the glibc or the desktop), so there is nothing here to
     * compare against and no gate to enforce. A Linux artifact that needs a
     * floor has to express it some other way. */
    (void)min_os;
    return true;
}

bool updater_plat_can_autoinstall(void) {
    char prefix[PATH_MAX];
    if (!install_prefix(prefix, sizeof prefix)) return false;  /* build tree */
    /* The prefix is replaced by swapping a sibling directory into place, so
     * the PARENT is what has to be writable, not the prefix itself. */
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", prefix);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return false;
    *slash = '\0';
    return access(parent, W_OK) == 0;
}

/* ===================== network ===================== */

/* Both fetch and download run one curl on a detached thread and hand the
 * result back through the atomics updater_tick() polls. */
typedef struct {
    UpdateState *st;
    char         url[1024];
    char         out_path[PATH_MAX];  /* empty => feed fetch (read into memory) */
    long long    expect_size;
    u32          cancel_epoch;
} NetJob;

static void net_job_done(NetJob *job) {
    if (!job) return;
    UpdateState *st = job->st;
    free(job);
    /* Last access through st; updater_shutdown may free its containing
     * AppState immediately after this decrement. */
    atomic_fetch_sub_explicit(&st->io_workers, 1, memory_order_release);
}

static void net_fail(UpdateState *st, const char *msg) {
    snprintf(st->io_err, sizeof st->io_err, "%s", msg);
    atomic_store(&st->io_ok, false);
    atomic_store(&st->io_done, true);
}

/* Network-child sibling of run_sync. A detached updater worker cannot rely on
 * one process-global PID: several windows may check concurrently, and a later
 * request would overwrite the PID of an older one. Each job instead snapshots
 * its owning state's cancellation epoch and reaps its own child. */
static int net_run_sync(char *const argv[], UpdateState *st, u32 epoch) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    bool term_sent = false;
    i32 term_polls = 0;
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (atomic_load_explicit(&st->io_cancel_epoch, memory_order_acquire) != epoch) {
            if (!term_sent) {
                (void)kill(pid, SIGTERM);
                term_sent = true;
            } else if (++term_polls >= 200) { /* ~2 s grace */
                (void)kill(pid, SIGKILL);
            }
        }
        usleep(10000);
    }
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

/* The largest artifact a transfer may write: what the feed said it is plus 5%
 * and 64 KiB, never more than the 1 GiB ceiling updater.c enforces on the file
 * (the macOS shim derives the same number). Computed in unsigned 64-bit after
 * clamping, so a hostile "size" cannot overflow it. */
static unsigned long long upd_download_cap(long long expect_size) {
    const unsigned long long hard = 1024ull * 1024 * 1024;
    if (expect_size <= 0) return hard;
    unsigned long long sz = (unsigned long long)expect_size;
    if (sz > hard) sz = hard;
    unsigned long long cap = sz + sz / 20 + 65536;
    return cap > hard ? hard : cap;
}

static void *net_thread(void *arg) {
    NetJob *job = arg;
    UpdateState *st = job->st;

    /* A feed body goes to a file only because curl writes one; it must be a file
     * nobody else can have prepared. A predictable name such as
     * /tmp/liu-feed-<pid> would let any local user plant a symlink there and make
     * curl overwrite whatever it pointed at (curl -o follows it). A mkdtemp
     * directory is 0700 and new. */
    char feed_dir[PATH_MAX] = "";
    char tmp_body[PATH_MAX];
    const char *dest = job->out_path[0] ? job->out_path : tmp_body;
    bool to_memory = (job->out_path[0] == '\0');
    if (to_memory) {
        if (!make_temp_dir(feed_dir, sizeof feed_dir) ||
            !path_fmt(tmp_body, sizeof tmp_body, "%s/feed.json", feed_dir)) {
            net_fail(st, "Couldn't create a temp dir.");
            net_job_done(job);
            return NULL;
        }
    }

    /* The flags that make the URL mean one thing and the transfer stay bounded:
     *   --proto / --proto-redir  https only, on the first request AND on every
     *                            redirect (--proto alone leaves redirects free to
     *                            go to http or ftp);
     *   --globoff                a feed-supplied `[1-9]` or `{a,b}` is a name, not
     *                            a request to fetch nine files;
     *   --max-redirs             a redirect loop ends;
     *   --max-filesize           a hostile host cannot fill the disk — the 4 MiB
     *                            feed check below runs after the bytes are on it. */
    char cap_str[32];
    snprintf(cap_str, sizeof cap_str, "%llu",
             (unsigned long long)(to_memory ? 5ull * 1024 * 1024
                                            : upd_download_cap(job->expect_size)));
    char *const argv[] = {
        (char *)"curl", (char *)"-fsSL",
        (char *)"--proto", (char *)"=https",
        (char *)"--proto-redir", (char *)"=https",
        (char *)"--globoff",
        (char *)"--max-redirs", (char *)"5",
        (char *)"--max-filesize", cap_str,
        (char *)"--max-time", (char *)"120",
        (char *)"-o", (char *)dest,
        (char *)job->url, NULL
    };
    int rc = net_run_sync(argv, st, job->cancel_epoch);

    if (rc != 0) {
        if (to_memory) { unlink(dest); rmdir(feed_dir); }
        /* Worded as the macOS shim words them, curl's code included: a feed
         * check that fails says it could not reach the update server, and
         * only a failed download says "Download failed." */
        char why[96];
        if (rc == 127)      snprintf(why, sizeof why, "curl is not installed.");
        else if (rc < 0)    snprintf(why, sizeof why, "%s", to_memory ? "Couldn't reach the update server."
                                                                       : "Download failed.");
        else if (to_memory) snprintf(why, sizeof why, "Couldn't reach the update server (curl %d).", rc);
        else                snprintf(why, sizeof why, "Download failed (curl %d).", rc);
        net_fail(st, why);
        net_job_done(job);
        return NULL;
    }

    if (to_memory) {
        FILE *f = fopen(dest, "rb");
        if (!f) { unlink(dest); rmdir(feed_dir); net_fail(st, "Couldn't read the feed."); net_job_done(job); return NULL; }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        /* A feed is a few KB; anything enormous is not one. */
        if (len <= 0 || len > 4 * 1024 * 1024) {
            fclose(f); unlink(dest); rmdir(feed_dir);
            net_fail(st, "Update feed is not readable."); net_job_done(job); return NULL;
        }
        char *buf = malloc((size_t)len + 1);
        if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
            free(buf); fclose(f); unlink(dest); rmdir(feed_dir);
            net_fail(st, "Couldn't read the feed."); net_job_done(job); return NULL;
        }
        buf[len] = '\0';
        fclose(f);
        unlink(dest);
        rmdir(feed_dir);
        st->feed_body = buf;               /* main thread frees */
        atomic_store(&st->feed_len, len);
    } else {
        struct stat sb;
        if (stat(dest, &sb) == 0) atomic_store(&st->bytes_done, (long long)sb.st_size);
        if (!path_fmt(st->downloaded_path, sizeof st->downloaded_path, "%s", dest)) {
            net_fail(st, "Download path is too long."); net_job_done(job); return NULL;
        }
    }

    atomic_store(&st->io_ok, true);
    atomic_store(&st->io_done, true);
    net_job_done(job);
    return NULL;
}

static void net_start(UpdateState *st, const char *url, const char *out_path,
                     long long expect_size) {
    NetJob *job = calloc(1, sizeof *job);
    if (!job) { net_fail(st, "Out of memory."); return; }
    job->st = st;
    snprintf(job->url, sizeof job->url, "%s", url ? url : "");
    if (out_path) snprintf(job->out_path, sizeof job->out_path, "%s", out_path);
    job->expect_size = expect_size;
    job->cancel_epoch = atomic_load_explicit(&st->io_cancel_epoch,
                                             memory_order_acquire);

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    atomic_fetch_add_explicit(&st->io_workers, 1, memory_order_release);
    int rc = pthread_create(&th, &attr, net_thread, job);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        atomic_fetch_sub_explicit(&st->io_workers, 1, memory_order_release);
        free(job);
        net_fail(st, "Couldn't start the download.");
    }
}

void updater_plat_fetch_feed(UpdateState *st, const char *url) {
    if (!st) return;
    if (!url || !url[0]) { net_fail(st, "No update feed is configured."); return; }
    net_start(st, url, NULL, 0);
}

void updater_plat_download(UpdateState *st, const char *url, long long sz) {
    if (!st) return;
    char dir[PATH_MAX];
    if (!make_temp_dir(dir, sizeof dir)) { net_fail(st, "Couldn't create a temp dir."); return; }
    char out[PATH_MAX];
    if (!path_fmt(out, sizeof out, "%s/liu-update.tar.gz", dir)) {
        net_fail(st, "Temp path is too long."); return;
    }
    /* Published up front so the progress bar has a denominator; the feed's
     * size is the only source available before the transfer starts. */
    atomic_store(&st->bytes_total, sz);
    atomic_store(&st->bytes_done, 0);
    net_start(st, url, out, sz);
}

void updater_plat_cancel(UpdateState *st) {
    if (!st) return;
    /* Every detached worker for this state observes the epoch change and
     * terminates/reaps its own PID. No cross-window global PID aliasing. */
    atomic_fetch_add_explicit(&st->io_cancel_epoch, 1, memory_order_release);
}

/* ===================== install ===================== */

/* The directory under `dir` (at most `depth` levels down) that holds a regular
 * file named Liu — what `find <dir> -maxdepth 3 -type f -name Liu` would answer,
 * walked without a shell so that no character in the path is ever read as shell
 * syntax. Symlinks are neither followed nor accepted: a tarball is allowed to
 * contain one, and a `Liu` that points elsewhere is not the downloaded binary. */
static bool find_liu_root(const char *dir, int depth, char *out, size_t cap) {
    DIR *d = opendir(dir);
    if (!d) return false;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISREG(st.st_mode) && strcmp(e->d_name, "Liu") == 0) {
            found = path_fmt(out, cap, "%s", dir);
        }
    }
    if (!found && depth > 1) {
        rewinddir(d);
        while (!found && (e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            struct stat st;
            if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode))
                continue;
            char sub[PATH_MAX];
            if (!path_fmt(sub, sizeof sub, "%s/%s", dir, e->d_name)) continue;
            found = find_liu_root(sub, depth - 1, out, cap);
        }
    }
    closedir(d);
    return found;
}

bool updater_plat_extract(const char *tarball, char *out_root, size_t cap) {
    if (!tarball || !tarball[0] || !out_root || cap == 0) return false;
    char dir[PATH_MAX];
    if (!make_temp_dir(dir, sizeof dir)) return false;

    char *const argv[] = {
        (char *)"tar", (char *)"-xzf", (char *)tarball, (char *)"-C", dir, NULL
    };
    if (run_sync(argv, NULL) != 0) return false;

    /* Be liberal about the top-level directory name the producer chose, the
     * same way install.sh is: find the tree that holds the Liu binary. */
    char cand[PATH_MAX];
    if (path_fmt(cand, sizeof cand, "%s/Liu", dir) && path_is_file(cand))
        return path_fmt(out_root, cap, "%s", dir);
    return find_liu_root(dir, 3, out_root, cap);
}

bool updater_plat_bundle_version(const char *root, char *out, size_t cap) {
    if (!root || !out || cap == 0) return false;
    /* Fail closed: updater.c installs only when this confirms the tree is
     * strictly newer, and the feed's own number must not be what confirms it.
     * A tarball without VERSION is therefore not installable — see the
     * artifact contract at the top of this file. */
    const char *names[] = { "VERSION", "assets/VERSION" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char p[PATH_MAX];
        if (!path_fmt(p, sizeof p, "%s/%s", root, names[i])) continue;
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        char buf[64] = {0};
        size_t got = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        if (got == 0) continue;
        buf[got] = '\0';
        size_t l = strlen(buf);
        while (l && (buf[l-1] == '\n' || buf[l-1] == '\r' || buf[l-1] == ' ' || buf[l-1] == '\t'))
            buf[--l] = '\0';
        if (!l) continue;
        snprintf(out, cap, "%s", buf);
        return true;
    }
    return false;
}

void updater_plat_strip_quarantine(const char *path) {
    /* No quarantine attribute on Linux — nothing marks a downloaded file as
     * needing user consent before it runs. */
    (void)path;
}

/* Copy `src` tree to `dst` preserving modes. cp -a, because doing it by hand
 * buys nothing over a tool that is on every system that has tar. */
static bool copy_tree(const char *src, const char *dst) {
    char *const argv[] = { (char *)"cp", (char *)"-a", (char *)src, (char *)dst, NULL };
    return run_sync(argv, NULL) == 0;
}

static void rm_rf(const char *path) {
    if (!path || !path[0] || strcmp(path, "/") == 0) return;
    char *const argv[] = { (char *)"rm", (char *)"-rf", (char *)path, NULL };
    (void)run_sync(argv, NULL);
}

bool updater_plat_install(const char *root, char *out_installed, size_t cap) {
    if (!root || !out_installed || cap == 0) return false;
    char prefix[PATH_MAX];
    if (!install_prefix(prefix, sizeof prefix)) return false;

    /* Stage a sibling tree and swap, so a failure half-way leaves the running
     * install untouched — install.sh does the same dance for the same reason. */
    char stage[PATH_MAX], old[PATH_MAX], bin_dir[PATH_MAX];
    if (!path_fmt(stage, sizeof stage, "%s.new-%ld", prefix, (long)getpid()) ||
        !path_fmt(old,   sizeof old,   "%s.old-%ld", prefix, (long)getpid()))
        return false;
    rm_rf(stage);
    if (!path_fmt(bin_dir, sizeof bin_dir, "%s/bin", stage)) { rm_rf(stage); return false; }
    char *const mkdir_argv[] = { (char *)"mkdir", (char *)"-p", bin_dir, NULL };
    if (run_sync(mkdir_argv, NULL) != 0) { rm_rf(stage); return false; }

    /* Binary + the helpers that must sit beside it (the notify hooks resolve
     * <dir-of-Liu>/liu-notify). */
    const char *bins[] = { "Liu", "liu-notify", "liu-history" };
    for (size_t i = 0; i < sizeof bins / sizeof bins[0]; i++) {
        char s[PATH_MAX], d[PATH_MAX];
        if (!path_fmt(s, sizeof s, "%s/%s", root, bins[i]) ||
            !path_fmt(d, sizeof d, "%s/%s", bin_dir, bins[i])) { rm_rf(stage); return false; }
        if (!path_is_file(s)) {
            if (i == 0) { rm_rf(stage); return false; }   /* Liu is mandatory */
            continue;
        }
        if (!copy_tree(s, d)) { rm_rf(stage); return false; }
        chmod(d, 0755);
    }

    /* assets/ — the default font lives here and is mandatory at runtime. */
    {
        char s[PATH_MAX], d[PATH_MAX];
        if (!path_fmt(s, sizeof s, "%s/assets", root) ||
            !path_fmt(d, sizeof d, "%s/assets", stage)) { rm_rf(stage); return false; }
        if (path_is_dir(s) && !copy_tree(s, d)) { rm_rf(stage); return false; }
    }
    /* VERSION marker, so the next update (and install.sh) can read it back. */
    {
        char s[PATH_MAX], d[PATH_MAX];
        if (!path_fmt(s, sizeof s, "%s/VERSION", root) ||
            !path_fmt(d, sizeof d, "%s/VERSION", stage)) { rm_rf(stage); return false; }
        if (path_is_file(s)) (void)copy_tree(s, d);
    }
    /* Anything else the old prefix carried that the tarball did not ship
     * (terminfo, icon) is preserved by the swap only if it is brought
     * across. The licence and the notices travel with the binary. */
    {
        const char *keep[] = { "liu.terminfo", "share",
                               "LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md" };
        for (size_t i = 0; i < sizeof keep / sizeof keep[0]; i++) {
            char s[PATH_MAX], d[PATH_MAX];
            if (!path_fmt(s, sizeof s, "%s/%s", root, keep[i]) ||
                !path_fmt(d, sizeof d, "%s/%s", stage, keep[i])) continue;
            if (path_is_file(s) || path_is_dir(s)) { (void)copy_tree(s, d); continue; }
            if (!path_fmt(s, sizeof s, "%s/%s", prefix, keep[i])) continue;
            if (path_is_file(s) || path_is_dir(s)) (void)copy_tree(s, d);
        }
    }

    /* Swap. rename() is atomic within a filesystem, and stage/old/prefix are
     * all siblings, so they always are. */
    if (rename(prefix, old) != 0) { rm_rf(stage); return false; }
    if (rename(stage, prefix) != 0) {
        (void)rename(old, prefix);   /* put it back */
        rm_rf(stage);
        return false;
    }
    rm_rf(old);

    return path_fmt(out_installed, cap, "%s/bin/Liu", prefix);
}

void updater_plat_relaunch_and_quit(const char *installed) {
    if (!installed || !installed[0]) return;
    pid_t old = getpid();
    /* Double-fork so the new instance is reparented to init and does not die
     * with this process; the intermediate child is reaped here so no zombie
     * is left. */
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild < 0) _exit(127);
        if (grandchild > 0) _exit(0);
        setsid();
        /* Before relaunch, release resources owned by the current process,
         * including its X connection and pane terminals, so the window closes
         * and child shells receive the expected hangup. */
        struct rlimit rl;
        int fd_max = (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
                      rl.rlim_cur < 65536) ? (int)rl.rlim_cur : 65536;
        for (int fd = STDERR_FILENO + 1; fd < fd_max; fd++) close(fd);
        /* The new copy starts once this one has GONE (update/relaunch_wait.h),
         * not after a guess at how long a quit takes. Bounded, then started
         * anyway. */
        (void)relaunch_wait_for_exit(old, RELAUNCH_WAIT_MAX_MS);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execl(installed, installed, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

void updater_plat_cleanup_temp(const char *path) {
    if (!path || !path[0]) return;
    /* The download lives in a temp dir of ours; take the directory with it. */
    char dir[PATH_MAX];
    if (!path_fmt(dir, sizeof dir, "%s", path)) { rm_rf(path); return; }
    char *slash = strrchr(dir, '/');
    if (slash && strstr(dir, "liu-update-")) {
        *slash = '\0';
        rm_rf(dir);
        return;
    }
    rm_rf(path);
}
