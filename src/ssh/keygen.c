/*
 * Liu - SSH key generation and management via system ssh-keygen
 *
 * Keys are made by shelling out to ssh-keygen rather than linking OpenSSL
 * directly, which keeps the binary minimal and uses the system's trusted
 * implementation.
 *
 * Key scanning reads ~/.ssh/ for existing key files, detects type and passphrase
 * status, and computes fingerprints via ssh-keygen -lf.
 */
#include "ssh/keygen.h"
#include "ssh/key_protection.h"
#include "core/checked.h"
#include "core/memory.h"
#include "core/net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>   /* INT_MAX — not transitively included on glibc */
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#ifndef PLATFORM_WIN32
#include <pwd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#endif

#ifndef PLATFORM_WIN32
extern char **environ;
#endif

/* =========================================================================
 * Subprocess helpers (argv-based, no shell interpretation)
 * =========================================================================
 * All ssh-keygen / ssh-add invocations go through these helpers instead of
 * system()/popen() so user-controlled data (key paths, comments,
 * passphrases) is never interpolated into a shell command line. That closes
 * the single-quote and metacharacter injection vectors.
 *
 * Secret prompts (new key passphrase / change-passphrase) are driven through a
 * private PTY so passphrases never appear in argv. */

/* Resolve "ssh-keygen"/"ssh-add" to the system binary when one is installed
 * there, so a PATH inherited from the desktop or a wrapper cannot
 * substitute the tool that receives the private-key path. Falls back to
 * PATH lookup for a system with the tools somewhere else. */
static const char *helper_path(const char *name) {
    static char buf[2][64];
    static const char *const dirs[] = { "/usr/bin", "/usr/local/bin", "/opt/homebrew/bin" };
    i32 slot = strcmp(name, "ssh-add") == 0 ? 1 : 0;
    for (usize i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        int n = snprintf(buf[slot], sizeof buf[slot], "%s/%s", dirs[i], name);
        if (n > 0 && (usize)n < sizeof buf[slot] && access(buf[slot], X_OK) == 0)
            return buf[slot];
    }
    return name;
}

static int spawn_helper(pid_t *pid, const char *const argv[],
                        posix_spawn_file_actions_t *acts) {
    const char *exe = argv[0];
    if (strcmp(exe, "ssh-keygen") == 0 || strcmp(exe, "ssh-add") == 0)
        exe = helper_path(exe);
    return exe[0] == '/'
        ? posix_spawn(pid, exe, acts, NULL, (char *const *)argv, environ)
        : posix_spawnp(pid, exe, acts, NULL, (char *const *)argv, environ);
}

static bool chmod_regular_nofollow(const char *path, mode_t mode) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && fchmod(fd, mode) == 0;
    close(fd);
    return ok;
}

static i64 keygen_monotonic_ms(void);
static void keygen_kill_reap(pid_t pid);
static int keygen_reap_until(pid_t pid, i64 deadline_ms);

/* How long a helper that only READS a key (its fingerprint, whether it is
 * encrypted) may run. The key list is refreshed on the main loop, so a helper
 * stuck on its input must not hold it; one past the limit is killed and
 * counts as failed. Generating a key and changing a passphrase are not
 * limited (-1). */
#define KEYGEN_PROBE_LIMIT_MS 5000

/* A regular file at `path`, opened for reading (a link is followed: keys are
 * often linked in from elsewhere). O_NONBLOCK refuses a FIFO in its place at
 * once instead of waiting for a writer. NULL for anything else. */
static FILE *keygen_fopen_regular(const char *path) {
    if (!path || !path[0]) return NULL;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    FILE *f = fdopen(fd, "r");
    if (!f) close(fd);
    return f;
}

/* Wait for `pid`: for ever when `limit_ms` < 0, otherwise until the limit,
 * when it is killed. */
static int keygen_wait(pid_t pid, i64 limit_ms) {
    if (limit_ms < 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) return -1;
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    i64 now = keygen_monotonic_ms();
    if (now < 0) { keygen_kill_reap(pid); return -1; }
    return keygen_reap_until(pid, now + limit_ms);
}

static int run_silent(const char *const argv[], i64 limit_ms) {
    if (!argv || !argv[0]) return -1;
    posix_spawn_file_actions_t acts;
    if (posix_spawn_file_actions_init(&acts) != 0) return -1;
    int rc = posix_spawn_file_actions_addopen(&acts, STDIN_FILENO,
                                               "/dev/null", O_RDONLY, 0);
    if (rc == 0) rc = posix_spawn_file_actions_addopen(
        &acts, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (rc == 0) rc = posix_spawn_file_actions_addopen(
        &acts, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    if (rc == 0) rc = spawn_helper(&pid, argv, &acts);
    posix_spawn_file_actions_destroy(&acts);
    if (rc != 0) return -1;
    return keygen_wait(pid, limit_ms);
}

static int keygen_fd_above_stdio(int fd) {
    if (fd < 0) return fd;
    if (fd < 3) {
        int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        close(fd);
        return moved;
    }
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int run_capture(const char *const argv[], char *out, size_t max_out, i64 limit_ms) {
    if (!argv || !argv[0] || (out && max_out == 0)) return -1;
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pipefd[0] = keygen_fd_above_stdio(pipefd[0]);
    pipefd[1] = keygen_fd_above_stdio(pipefd[1]);
    if (pipefd[0] < 0 || pipefd[1] < 0) {
        if (pipefd[0] >= 0) close(pipefd[0]);
        if (pipefd[1] >= 0) close(pipefd[1]);
        return -1;
    }

    posix_spawn_file_actions_t acts;
    if (posix_spawn_file_actions_init(&acts) != 0) {
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    int arc = posix_spawn_file_actions_addclose(&acts, pipefd[0]);
    if (arc == 0) arc = posix_spawn_file_actions_adddup2(
        &acts, pipefd[1], STDOUT_FILENO);
    if (arc == 0) arc = posix_spawn_file_actions_addopen(
        &acts, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (arc == 0) arc = posix_spawn_file_actions_addclose(&acts, pipefd[1]);

    pid_t pid;
    int rc = arc == 0 ? spawn_helper(&pid, argv, &acts) : arc;
    posix_spawn_file_actions_destroy(&acts);
    close(pipefd[1]);
    if (rc != 0) { close(pipefd[0]); return -1; }

    i64 deadline = -1;
    if (limit_ms >= 0) {
        i64 now = keygen_monotonic_ms();
        deadline = now < 0 ? 0 : now + limit_ms;
    }
    size_t total = 0;
    char scratch[512];
    for (;;) {
        if (deadline >= 0) {
            i64 now = keygen_monotonic_ms();
            i64 left = now < 0 ? 0 : deadline - now;
            struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
            /* In slices, so the limit is read off the clock again. */
            int pr = left > 0 ? poll(&pfd, 1, left > 250 ? 250 : (int)left) : 0;
            if (pr < 0 && errno == EINTR) continue;
            if (pr == 0 && left > 250) continue;
            if (pr <= 0) {
                close(pipefd[0]);
                keygen_kill_reap(pid);
                if (out) out[0] = '\0';
                return -1;
            }
        }
        ssize_t n = read(pipefd[0], scratch, sizeof scratch);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (out && total + 1u < max_out) {
            size_t avail = max_out - 1u - total;
            size_t copy = (size_t)n < avail ? (size_t)n : avail;
            memcpy(out + total, scratch, copy);
            total += copy;
        }
    }
    if (out) out[total] = '\0';
    close(pipefd[0]);

    if (deadline < 0) return keygen_wait(pid, -1);
    return keygen_reap_until(pid, deadline);
}

static bool write_all(int fd, const void *buf, size_t len) {
    const u8 *p = (const u8 *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static i64 keygen_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    if (ts.tv_sec > INT64_MAX / 1000) return INT64_MAX;
    return (i64)ts.tv_sec * 1000 + (i64)ts.tv_nsec / 1000000;
}

static void keygen_kill_reap(pid_t pid) {
    if (pid <= 0) return;
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

static void pty_window_append(char *window, size_t *used,
                              const char *chunk, size_t chunk_len) {
    const size_t cap = 4096;
    if (!window || !used || !chunk) return;
    if (*used >= cap) *used = cap - 1u;
    if (chunk_len >= cap - 1) {
        memcpy(window, chunk + chunk_len - (cap - 1), cap - 1);
        *used = cap - 1;
        window[*used] = '\0';
        return;
    }
    if (*used + chunk_len >= cap - 1) {
        size_t drop = (*used + chunk_len) - (cap - 1);
        memmove(window, window + drop, *used - drop);
        *used -= drop;
    }
    memcpy(window + *used, chunk, chunk_len);
    *used += chunk_len;
    window[*used] = '\0';
}

/* The helper has closed its end of the pty: collect its exit status.
 *
 * End-of-file on the master (read() returning 0, or EIO, which is how both
 * macOS and Linux report a slave that was closed) arrives a moment BEFORE the
 * process can be reaped, so a single waitpid(WNOHANG) at that instant usually
 * finds nothing. Treating that as a failure would kill a helper that had just
 * succeeded and report it as failed — keygen_generate_to_file would delete
 * the key it made, and a passphrase change already written to disk would be
 * reported as not done. So: wait for the exit, up to the same deadline. */
static int keygen_reap_until(pid_t pid, i64 deadline_ms) {
    for (;;) {
        int st = 0;
        pid_t ended = waitpid(pid, &st, WNOHANG);
        if (ended == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (ended < 0 && errno != EINTR) return -1;
        i64 now = keygen_monotonic_ms();
        if (now < 0 || now >= deadline_ms) { keygen_kill_reap(pid); return -1; }
        struct timespec nap = { 0, 5 * 1000 * 1000 };
        nanosleep(&nap, NULL);
    }
}

/* `first_optional`: the helper may skip prompts[0] and open with prompts[1]
 * (ssh-keygen -p asks for the OLD passphrase only when the key has one). */
static int run_pty_prompted(const char *const argv[],
                            const char *const prompts[],
                            const char *const replies[],
                            i32 prompt_count, bool first_optional) {
    if (!argv || !argv[0] || prompt_count < 0 || prompt_count > 8 ||
        (prompt_count > 0 && (!prompts || !replies))) return -1;
    if (prompt_count == 0) return run_silent(argv, -1);
    for (i32 i = 0; i < prompt_count; i++)
        if (!prompts[i]) return -1;

    /* Precompute an /usr/bin/env argv so the post-fork child need not call
     * setenv (which can deadlock on an inherited libc allocator lock). */
    char *exec_argv[32] = {0};
    i32 argc = 0;
    exec_argv[argc++] = (char *)"/usr/bin/env";
    exec_argv[argc++] = (char *)"LC_ALL=C";
    exec_argv[argc++] = (char *)"LANG=C";
    for (i32 i = 0; argv[i]; i++) {
        if (argc >= (i32)(sizeof exec_argv / sizeof exec_argv[0]) - 1)
            return -1;
        /* env looks argv[0] up on PATH; hand it the resolved helper. */
        exec_argv[argc++] = (char *)(i == 0 ? helper_path(argv[0]) : argv[i]);
    }
    exec_argv[argc] = NULL;

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) return -1;
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);
        return -1;
    }

    const char *slave_name = ptsname(master);
    if (!slave_name || !slave_name[0]) {
        close(master);
        return -1;
    }

    char slave_path[256];
    int pn = snprintf(slave_path, sizeof slave_path, "%s", slave_name);
    if (pn < 0 || (usize)pn >= sizeof slave_path) {
        close(master);
        return -1;
    }
    int mflags = fcntl(master, F_GETFD, 0);
    if (mflags < 0 || fcntl(master, F_SETFD, mflags | FD_CLOEXEC) != 0) {
        close(master);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        return -1;
    }

    if (pid == 0) {
        close(master);
        if (setsid() < 0) _exit(127);
        int slave = open(slave_path, O_RDWR);
        if (slave < 0) _exit(127);
        if (dup2(slave, STDIN_FILENO) < 0 ||
            dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0) {
            close(slave);
            _exit(127);
        }
        if (slave > STDERR_FILENO) close(slave);
        execv("/usr/bin/env", exec_argv);
        _exit(127);
    }

    char window[4096] = {0};
    size_t window_used = 0;
    i32 stage = 0;
    i64 now_ms = keygen_monotonic_ms();
    if (now_ms < 0 || now_ms > INT64_MAX - 30000) {
        keygen_kill_reap(pid);
        close(master);
        return -1;
    }
    i64 deadline_ms = now_ms + 30000;

    for (;;) {
        now_ms = keygen_monotonic_ms();
        if (now_ms < 0 || now_ms >= deadline_ms) {
            keygen_kill_reap(pid);
            secure_zero(window, sizeof(window));
            close(master);
            return -1;
        }
        if (stage == 0 && first_optional && prompt_count > 1 &&
            strstr(window, prompts[0]) == NULL && strstr(window, prompts[1]) != NULL)
            stage = 1;   /* the helper did not ask the first question */
        if (stage < prompt_count && strstr(window, prompts[stage]) != NULL) {
            const char *reply = replies[stage] ? replies[stage] : "";
            if (!write_all(master, reply, strlen(reply)) ||
                !write_all(master, "\n", 1)) {
                keygen_kill_reap(pid);
                secure_zero(window, sizeof(window));
                close(master);
                return -1;
            }
            secure_zero(window, sizeof(window));
            window[0] = '\0';
            window_used = 0;
            stage++;
            continue;
        }

        int status = 0;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            secure_zero(window, sizeof(window));
            close(master);
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (done < 0 && errno != EINTR) {
            secure_zero(window, sizeof(window));
            close(master);
            return -1;
        }

        struct pollfd pfd = { .fd = master, .events = POLLIN | POLLHUP | POLLERR };
        i64 remain = deadline_ms - now_ms;
        int timeout = remain < 250 ? (int)remain : 250;
        int pr = poll(&pfd, 1, timeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            keygen_kill_reap(pid);
            secure_zero(window, sizeof(window));
            close(master);
            return -1;
        }
        if (pr == 0) continue;

        if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[512];
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
                pty_window_append(window, &window_used, buf, (size_t)n);
                continue;
            }
            if (n == 0 || errno == EIO) {
                /* The helper closed the pty: it is finishing, not failing. */
                int rc = keygen_reap_until(pid, deadline_ms);
                secure_zero(window, sizeof(window));
                close(master);
                return rc;
            }
            if (errno != EINTR && errno != EAGAIN) {
                keygen_kill_reap(pid);
                secure_zero(window, sizeof(window));
                close(master);
                return -1;
            }
        }
    }
}

/* Build "-b NNNN" style numeric arg into a stack buffer */
static const char *num_arg(char *buf, size_t n, int value) {
    snprintf(buf, n, "%d", value);
    return buf;
}

const char *keygen_algo_name(KeyAlgorithm algo) {
    switch (algo) {
        case KEYGEN_RSA_2048:   return "rsa-2048";
        case KEYGEN_RSA_4096:   return "rsa-4096";
        case KEYGEN_ED25519:    return "ed25519";
        case KEYGEN_ECDSA_P256: return "ecdsa-256";
        case KEYGEN_ECDSA_P384: return "ecdsa-384";
        case KEYGEN_ECDSA_P521: return "ecdsa-521";
    }
    return "unknown";
}

const char *ssh_get_default_dir(char *buf, i32 buf_size) {
    if (!buf || buf_size <= 0) return NULL;
    buf[0] = '\0';
    const char *home = net_home_dir();
    if (!home || !home[0]) return NULL;
    int n = snprintf(buf, (usize)buf_size, "%s/.ssh", home);
    if (n <= 0 || n >= buf_size) { buf[0] = '\0'; return NULL; }
    return buf;
}

static bool read_file(const char *path, char *buf, size_t buf_size) {
    if (!path || !path[0] || !buf || buf_size == 0) return false;
    buf[0] = '\0';
    FILE *f = keygen_fopen_regular(path);
    if (!f) return false;
    size_t n = fread(buf, 1, buf_size - 1, f);
    bool complete = !ferror(f);
    if (complete && n == buf_size - 1) {
        int extra = fgetc(f);
        if (extra != EOF || ferror(f)) complete = false;
    }
    buf[n] = '\0';
    fclose(f);
    return complete && n > 0;
}

static bool keygen_algo_valid(KeyAlgorithm algo) {
    return algo >= KEYGEN_RSA_2048 && algo <= KEYGEN_ECDSA_P521;
}

/* Build the ssh-keygen argv for a given algorithm, writing to key_path.
 * Returns the algorithm label for the output struct. */
static const char *build_keygen_argv(KeyAlgorithm algo, const char *key_path,
                                      const char *passphrase, bool include_passphrase_arg,
                                      const char *comment,
                                      char *bits_buf, size_t bits_buf_size,
                                      const char *argv_out[16]) {
    const char *type = NULL;
    int bits = 0;
    const char *label = NULL;
    switch (algo) {
    case KEYGEN_RSA_2048:   type = "rsa";     bits = 2048; label = "rsa";       break;
    case KEYGEN_RSA_4096:   type = "rsa";     bits = 4096; label = "rsa";       break;
    case KEYGEN_ED25519:    type = "ed25519"; bits = 0;    label = "ed25519";   break;
    case KEYGEN_ECDSA_P256: type = "ecdsa";   bits = 256;  label = "ecdsa-256"; break;
    case KEYGEN_ECDSA_P384: type = "ecdsa";   bits = 384;  label = "ecdsa-384"; break;
    case KEYGEN_ECDSA_P521: type = "ecdsa";   bits = 521;  label = "ecdsa-521"; break;
    default: return NULL;
    }
    i32 i = 0;
    argv_out[i++] = "ssh-keygen";
    argv_out[i++] = "-t"; argv_out[i++] = type;
    if (bits > 0) {
        argv_out[i++] = "-b";
        argv_out[i++] = num_arg(bits_buf, bits_buf_size, bits);
    }
    argv_out[i++] = "-f"; argv_out[i++] = key_path;
    if (include_passphrase_arg) {
        argv_out[i++] = "-N";
        argv_out[i++] = passphrase ? passphrase : "";
    }
    argv_out[i++] = "-C"; argv_out[i++] = (comment && comment[0]) ? comment : "Liu";
    argv_out[i++] = "-q";
    argv_out[i++] = NULL;
    return label;
}

static int run_keygen_generate(KeyAlgorithm algo, const char *key_path,
                               const char *passphrase, const char *comment,
                               char *bits_buf, size_t bits_buf_size,
                               char *algorithm_out, size_t algorithm_out_size) {
    const char *argv[16];
    bool has_passphrase = passphrase && passphrase[0];
    const char *label = build_keygen_argv(algo, key_path, passphrase,
                                          !has_passphrase, comment,
                                          bits_buf, bits_buf_size, argv);
    if (!label) return -1;
    if (algorithm_out && algorithm_out_size > 0)
        snprintf(algorithm_out, algorithm_out_size, "%s", label);
    if (!has_passphrase) return run_silent(argv, -1);

    const char *prompts[] = {
        "Enter passphrase",
        "Enter same passphrase again",
    };
    const char *replies[] = {
        passphrase,
        passphrase,
    };
    return run_pty_prompted(argv, prompts, replies, 2, false);
}

static int run_keygen_change(const char *key_path,
                             const char *old_passphrase,
                             const char *new_passphrase) {
    const char *argv[] = {
        "ssh-keygen", "-p", "-f", key_path, "-q", NULL
    };
    const char *prompts[] = {
        "Enter old passphrase",
        "Enter new passphrase",
        "Enter same passphrase again",
    };
    const char *replies[] = {
        old_passphrase ? old_passphrase : "",
        new_passphrase ? new_passphrase : "",
        new_passphrase ? new_passphrase : "",
    };
    /* An unencrypted key is not asked for its old passphrase: the first
     * question is optional, or a passphrase could never be ADDED to a key
     * (the driver would wait for a prompt that never comes). */
    return run_pty_prompted(argv, prompts, replies, 3, true);
}

/* Create a 0700 private scratch directory in $TMPDIR (or /tmp) via mkdtemp.
 * A predictable path such as /tmp/Liu_keygen_<pid> is open to symlink /
 * TOCTOU attacks: the PID is guessable, and another user on the system could
 * create the file first as a symlink to /etc/passwd before ssh-keygen writes
 * the private key. mkdtemp gives a unique, randomly named directory owned by
 * the current user with 0700 permissions. Writes the directory path into
 * out_dir (buf_size >= 24). Returns true on success. */
static bool keygen_mkscratch(char *out_dir, size_t buf_size) {
    if (!out_dir || buf_size == 0) return false;
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
    /* Strip trailing slash for clean path joins */
    size_t tl = strlen(tmpdir);
    while (tl > 1 && tmpdir[tl - 1] == '/') tl--;
    if (tl > (size_t)INT_MAX) return false;
    int n = snprintf(out_dir, buf_size, "%.*s/liu_kg_XXXXXX", (int)tl, tmpdir);
    if (n < 0 || (size_t)n >= buf_size) return false;
    return mkdtemp(out_dir) != NULL;
}

/* Best-effort cleanup of the scratch directory (private/public + dir). */
static void keygen_rmscratch(const char *dir, const char *priv, const char *pub) {
    if (priv && priv[0]) unlink(priv);
    if (pub  && pub[0])  unlink(pub);
    if (dir  && dir[0])  rmdir(dir);
}

bool keygen_generate(KeyAlgorithm algo, const char *comment,
                     const char *passphrase, GeneratedKey *out) {
    if (!out || !keygen_algo_valid(algo)) return false;
    memset(out, 0, sizeof(*out));

    /* Create a unique 0700 scratch directory; do NOT use a predictable
     * /tmp/<pid> path (symlink / TOCTOU attack surface). */
    char scratch[256];
    if (!keygen_mkscratch(scratch, sizeof(scratch))) return false;

    char tmp_priv[288], tmp_pub[296];
    int n = snprintf(tmp_priv, sizeof tmp_priv, "%s/id", scratch);
    int np = n >= 0 && (usize)n < sizeof tmp_priv
           ? snprintf(tmp_pub, sizeof tmp_pub, "%s.pub", tmp_priv) : -1;
    if (n < 0 || (usize)n >= sizeof tmp_priv ||
        np < 0 || (usize)np >= sizeof tmp_pub) {
        keygen_rmscratch(scratch, NULL, NULL);
        return false;
    }

    char bits_buf[16];
    int rc = run_keygen_generate(algo, tmp_priv, passphrase, comment,
                                 bits_buf, sizeof(bits_buf),
                                 out->algorithm, sizeof(out->algorithm));
    if (rc != 0) { keygen_rmscratch(scratch, tmp_priv, tmp_pub); return false; }

    /* Read generated files */
    if (!read_file(tmp_pub, out->public_key, sizeof(out->public_key))) {
        keygen_rmscratch(scratch, tmp_priv, tmp_pub);
        return false;
    }
    if (!read_file(tmp_priv, out->private_key, sizeof(out->private_key))) {
        keygen_rmscratch(scratch, tmp_priv, tmp_pub);
        return false;
    }

    /* Get fingerprint via argv-based ssh-keygen -lf */
    {
        const char *fargv[] = { "ssh-keygen", "-lf", tmp_pub, NULL };
        char line[512] = {0};
        if (run_capture(fargv, line, sizeof(line), KEYGEN_PROBE_LIMIT_MS) == 0) {
            char *sha = strstr(line, "SHA256:");
            if (sha) {
                char *end = strchr(sha, ' ');
                if (end) *end = '\0';
                char *nl = strchr(sha, '\n');
                if (nl) *nl = '\0';
                snprintf(out->fingerprint, sizeof(out->fingerprint), "%s", sha);
            }
        }
    }

    keygen_rmscratch(scratch, tmp_priv, tmp_pub);
    return true;
}

bool keygen_generate_to_file(KeyAlgorithm algo, const char *comment,
                             const char *passphrase, const char *filepath,
                             char *fingerprint_out, i32 fp_size) {
    if (!keygen_algo_valid(algo) || !filepath || !filepath[0] ||
        (fingerprint_out && fp_size <= 0)) return false;
    if (fingerprint_out) fingerprint_out[0] = '\0';

    /* Ensure parent directory exists */
    char dir[512];
    int dn = snprintf(dir, sizeof dir, "%s", filepath);
    if (dn <= 0 || (usize)dn >= sizeof dir) return false;
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        if (!dir[0]) return false;
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) return false;
        struct stat dst;
        if (stat(dir, &dst) != 0 || !S_ISDIR(dst.st_mode)) return false;
    }

    char pub_path[520];
    int pn = snprintf(pub_path, sizeof pub_path, "%s.pub", filepath);
    if (pn <= 0 || (usize)pn >= sizeof pub_path) return false;
    /* Never destroy an existing key as a side effect of this API. */
    struct stat existing;
    if (lstat(filepath, &existing) == 0 || lstat(pub_path, &existing) == 0)
        return false;

    /* Build argv directly — no shell interpretation */
    char bits_buf[16];
    int rc = run_keygen_generate(algo, filepath, passphrase, comment,
                                 bits_buf, sizeof(bits_buf), NULL, 0);
    if (rc != 0) {
        (void)unlink(filepath); (void)unlink(pub_path);
        return false;
    }

    /* Set permissions — on the opened fd, so the file checked is the file
     * changed (a chmod by path would follow a symlink swapped in between
     * lstat and chmod). */
    if (!chmod_regular_nofollow(filepath, 0600) ||
        !chmod_regular_nofollow(pub_path, 0644)) {
        (void)unlink(filepath); (void)unlink(pub_path);
        return false;
    }

    /* Get fingerprint */
    if (fingerprint_out && fp_size > 0) {
        const char *fargv[] = { "ssh-keygen", "-lf", pub_path, NULL };
        char line[512] = {0};
        if (run_capture(fargv, line, sizeof(line), KEYGEN_PROBE_LIMIT_MS) == 0) {
            char *sha = strstr(line, "SHA256:");
            if (sha) {
                char *end = strchr(sha, ' ');
                if (end) *end = '\0';
                char *nl = strchr(sha, '\n');
                if (nl) *nl = '\0';
                snprintf(fingerprint_out, (size_t)fp_size, "%s", sha);
            }
        }
    }

    return true;
}

/* Check if a private key file is encrypted (passphrase-protected) */
static bool key_is_encrypted(const char *path) {
    FILE *f = keygen_fopen_regular(path);
    if (!f) return false;

    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    /* The key's own header answers for every format ssh-keygen writes; only
     * text it does not recognise falls through to the checks below. */
    switch (ssh_key_protection(buf, n)) {
    case SSH_KEY_PLAIN:     return false;
    case SSH_KEY_ENCRYPTED: return true;
    case SSH_KEY_UNKNOWN:   break;
    }

    /* OpenSSH new format: look for bcrypt KDF (encrypted) */
    if (strstr(buf, "ENCRYPTED")) return true;

    /* OpenSSH new format: check for aes256-ctr or similar cipher */
    /* "-----BEGIN OPENSSH PRIVATE KEY-----" with bcrypt/aes is encrypted */
    if (strstr(buf, "-----BEGIN OPENSSH PRIVATE KEY-----")) {
        /* For new-format keys, try ssh-keygen -y with empty passphrase.
         * If it fails, the key is encrypted. */
        const char *argv[] = { "ssh-keygen", "-y", "-P", "", "-f", path, NULL };
        int rc = run_silent(argv, KEYGEN_PROBE_LIMIT_MS);
        return rc != 0;
    }

    return false;
}

/* Detect key type from public key line */
static void detect_key_type(const char *pub_line, char *type_out, i32 type_size, i32 *bits_out) {
    if (!pub_line || !type_out || type_size <= 0 || !bits_out) return;
    type_out[0] = '\0';
    *bits_out = 0;

    if (strstr(pub_line, "ssh-ed25519")) {
        snprintf(type_out, (size_t)type_size, "ed25519");
        *bits_out = 256;
    } else if (strstr(pub_line, "ssh-rsa")) {
        snprintf(type_out, (size_t)type_size, "rsa");
    } else if (strstr(pub_line, "ecdsa-sha2-nistp256")) {
        snprintf(type_out, (size_t)type_size, "ecdsa-256");
        *bits_out = 256;
    } else if (strstr(pub_line, "ecdsa-sha2-nistp384")) {
        snprintf(type_out, (size_t)type_size, "ecdsa-384");
        *bits_out = 384;
    } else if (strstr(pub_line, "ecdsa-sha2-nistp521")) {
        snprintf(type_out, (size_t)type_size, "ecdsa-521");
        *bits_out = 521;
    } else if (strstr(pub_line, "ssh-dss")) {
        snprintf(type_out, (size_t)type_size, "dsa");
        *bits_out = 1024;
    }
}

i32 ssh_scan_keys(const char *ssh_dir, KeyInfo *keys, i32 *count, i32 max) {
    if (!count) return 0;
    *count = 0;
    if (!keys || max <= 0) return 0;

    char dir_path[512];
    if (!ssh_dir || !ssh_dir[0]) {
        if (!ssh_get_default_dir(dir_path, sizeof(dir_path))) return 0;
        ssh_dir = dir_path;
    }

    DIR *d = opendir(ssh_dir);
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) && *count < max) {
        const char *name = ent->d_name;

        /* Skip dotfiles, .pub files, known_hosts, config, authorized_keys */
        if (name[0] == '.') continue;
        if (strstr(name, ".pub")) continue;
        if (strcmp(name, "known_hosts") == 0) continue;
        if (strcmp(name, "known_hosts.old") == 0) continue;
        if (strcmp(name, "config") == 0) continue;
        if (strcmp(name, "authorized_keys") == 0) continue;
        if (strcmp(name, "environment") == 0) continue;
        if (strcmp(name, "agent.sock") == 0) continue;

        /* Check if this looks like a private key file */
        char priv_path[1024];
        int pp = snprintf(priv_path, sizeof priv_path, "%s/%s", ssh_dir, name);
        if (pp <= 0 || (usize)pp >= sizeof priv_path) continue;
        if ((usize)pp >= sizeof keys[0].path) continue;

        /* A regular file with an SSH key header. */
        FILE *f = keygen_fopen_regular(priv_path);
        if (!f) continue;
        char header[128];
        if (!fgets(header, sizeof(header), f)) { fclose(f); continue; }
        fclose(f);

        if (!strstr(header, "PRIVATE KEY")) continue;

        /* This is a private key file */
        KeyInfo *ki = &keys[*count];
        memset(ki, 0, sizeof(*ki));
        snprintf(ki->name, sizeof(ki->name), "%s", name);
        snprintf(ki->path, sizeof(ki->path), "%s", priv_path);
        int kp = snprintf(ki->pub_path, sizeof ki->pub_path, "%s.pub", priv_path);
        if (kp <= 0 || (usize)kp >= sizeof ki->pub_path) continue;

        /* Check for passphrase */
        ki->has_passphrase = key_is_encrypted(priv_path);

        /* Read public key for type detection. The fingerprint is asked of
         * ssh-keygen only for a public key that was read as a regular file:
         * it would wait on a FIFO in its place. */
        char pub_content[4096];
        bool have_pub = read_file(ki->pub_path, pub_content, sizeof(pub_content));
        if (have_pub) {
            detect_key_type(pub_content, ki->type, sizeof(ki->type), &ki->bits);
        } else {
            snprintf(ki->type, sizeof(ki->type), "unknown");
        }

        /* Get fingerprint via argv-based ssh-keygen -lf */
        if (have_pub) {
            const char *fargv[] = { "ssh-keygen", "-lf", ki->pub_path, NULL };
            char line[512] = {0};
            if (run_capture(fargv, line, sizeof(line), KEYGEN_PROBE_LIMIT_MS) == 0) {
                /* Format: "4096 SHA256:xxxx comment (RSA)" */
                char *sha = strstr(line, "SHA256:");
                if (sha) {
                    char *end = strchr(sha, ' ');
                    if (end) *end = '\0';
                    char *nl = strchr(sha, '\n');
                    if (nl) *nl = '\0';
                    snprintf(ki->fingerprint, sizeof(ki->fingerprint), "%s", sha);
                }
                /* Extract bits from the first number if RSA */
                if (strcmp(ki->type, "rsa") == 0 && ki->bits == 0) {
                    i32 bits;
                    if (checked_parse_i32_prefix(line, &bits, NULL) && bits > 0)
                        ki->bits = bits;
                }
            }
        }

        (*count)++;
    }
    closedir(d);
    return *count;
}

bool ssh_delete_key(const char *private_key_path) {
    if (!private_key_path || !private_key_path[0]) return false;

    char pub_path[520];
    int n = snprintf(pub_path, sizeof pub_path, "%s.pub", private_key_path);
    if (n <= 0 || (usize)n >= sizeof pub_path) return false;

    bool ok = true;
    if (unlink(private_key_path) != 0) ok = false;
    /* Public key may not exist; that's fine */
    unlink(pub_path);
    return ok;
}

char *ssh_read_public_key(const char *pub_key_path) {
    if (!pub_key_path || !pub_key_path[0]) return NULL;

    FILE *f = keygen_fopen_regular(pub_key_path);
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size <= 0 || size > 16384 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); return NULL;
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t n = fread(buf, 1, (size_t)size, f);
    if (n != (size_t)size || ferror(f)) {
        free(buf); fclose(f); return NULL;
    }
    buf[n] = '\0';
    fclose(f);

    /* Trim trailing newline */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) {
        buf[--n] = '\0';
    }

    return buf;
}

bool keygen_add_to_agent(const char *key_path) {
    if (!key_path || !key_path[0]) return false;

    /* Verify file exists */
    struct stat st;
    if (stat(key_path, &st) != 0) return false;

    const char *argv[] = { "ssh-add", key_path, NULL };
    return run_silent(argv, -1) == 0;
}

bool keygen_change_passphrase(const char *key_path,
                              const char *old_passphrase,
                              const char *new_passphrase) {
    if (!key_path || !key_path[0]) return false;

    struct stat st;
    if (stat(key_path, &st) != 0) return false;

    return run_keygen_change(key_path, old_passphrase, new_passphrase) == 0;
}

bool keygen_is_encrypted(const char *key_path) {
    if (!key_path || !key_path[0]) return false;
    {
        FILE *hf = keygen_fopen_regular(key_path);
        if (!hf) return false;
        char head[2048];
        size_t hn = fread(head, 1, sizeof head - 1, hf);
        fclose(hf);
        switch (ssh_key_protection(head, hn)) {
        case SSH_KEY_PLAIN:     return false;
        case SSH_KEY_ENCRYPTED: return true;
        case SSH_KEY_UNKNOWN:   break;
        }
    }

    /* Read the first few lines of the key file and check for encryption markers */
    FILE *f = keygen_fopen_regular(key_path);
    if (!f) return false;

    char line[512];
    bool encrypted = false;
    int lines_read = 0;
    while (fgets(line, sizeof(line), f) && lines_read < 10) {
        lines_read++;
        if (strstr(line, "ENCRYPTED")) {
            encrypted = true;
            break;
        }
        if (strstr(line, "DEK-Info:")) {
            encrypted = true;
            break;
        }
    }
    fclose(f);

    /* For OpenSSH new-format keys without the ENCRYPTED marker,
     * try a no-passphrase parse as a fallback check */
    if (!encrypted) {
        const char *argv[] = { "ssh-keygen", "-y", "-P", "", "-f", key_path, NULL };
        if (run_silent(argv, KEYGEN_PROBE_LIMIT_MS) != 0) encrypted = true;
    }

    return encrypted;
}
