/*
 * Liu - SSH key generation and management via system ssh-keygen
 *
 * We shell out to ssh-keygen rather than linking OpenSSL directly.
 * This keeps our binary minimal and leverages the system's trusted implementation.
 *
 * Key scanning reads ~/.ssh/ for existing key files, detects type and
 * passphrase status, and computes fingerprints via ssh-keygen -lf.
 */
#include "ssh/keygen.h"
#include "core/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
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
 * system()/popen() so we never interpolate user-controlled data (key paths,
 * comments, passphrases) into a shell command line. That closes the single-
 * quote and metacharacter injection vectors.
 *
 * Secret prompts (new key passphrase / change-passphrase) are driven through a
 * private PTY so passphrases never appear in argv. */

static int run_silent(const char *const argv[]) {
    posix_spawn_file_actions_t acts;
    if (posix_spawn_file_actions_init(&acts) != 0) return -1;
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        posix_spawn_file_actions_adddup2(&acts, devnull, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&acts, devnull, STDERR_FILENO);
        posix_spawn_file_actions_addclose(&acts, devnull);
    }
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &acts, NULL,
                           (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&acts);
    if (devnull >= 0) close(devnull);
    if (rc != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_capture(const char *const argv[], char *out, size_t max_out) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    posix_spawn_file_actions_t acts;
    if (posix_spawn_file_actions_init(&acts) != 0) {
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    posix_spawn_file_actions_addclose(&acts, pipefd[0]);
    posix_spawn_file_actions_adddup2(&acts, pipefd[1], STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        posix_spawn_file_actions_adddup2(&acts, devnull, STDERR_FILENO);
        posix_spawn_file_actions_addclose(&acts, devnull);
    }
    posix_spawn_file_actions_addclose(&acts, pipefd[1]);

    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &acts, NULL,
                           (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&acts);
    close(pipefd[1]);
    if (devnull >= 0) close(devnull);
    if (rc != 0) { close(pipefd[0]); return -1; }

    size_t total = 0;
    if (out && max_out > 0) {
        while (total + 1 < max_out) {
            ssize_t n = read(pipefd[0], out + total, max_out - 1 - total);
            if (n <= 0) break;
            total += (size_t)n;
        }
        out[total] = '\0';
    } else {
        /* Drain so child doesn't block on SIGPIPE */
        char scratch[256];
        while (read(pipefd[0], scratch, sizeof(scratch)) > 0) {}
    }
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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

static void pty_window_append(char *window, size_t *used,
                              const char *chunk, size_t chunk_len) {
    const size_t cap = 4096;
    if (!window || !used || !chunk) return;
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

static int run_pty_prompted(const char *const argv[],
                            const char *const prompts[],
                            const char *const replies[],
                            i32 prompt_count) {
    if (prompt_count <= 0) return run_silent(argv);

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
    snprintf(slave_path, sizeof(slave_path), "%s", slave_name);

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
        setenv("LC_ALL", "C", 1);
        setenv("LANG", "C", 1);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    char window[4096] = {0};
    size_t window_used = 0;
    i32 stage = 0;
    i64 deadline_ms = 30000;

    for (;;) {
        if (stage < prompt_count && strstr(window, prompts[stage]) != NULL) {
            const char *reply = replies[stage] ? replies[stage] : "";
            if (!write_all(master, reply, strlen(reply)) ||
                !write_all(master, "\n", 1)) {
                kill(pid, SIGKILL);
                (void)waitpid(pid, NULL, 0);
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

        struct pollfd pfd = { .fd = master, .events = POLLIN | POLLHUP | POLLERR };
        int pr = poll(&pfd, 1, 250);
        if (pr < 0) {
            if (errno == EINTR) continue;
            kill(pid, SIGKILL);
            (void)waitpid(pid, NULL, 0);
            secure_zero(window, sizeof(window));
            close(master);
            return -1;
        }
        if (pr == 0) {
            deadline_ms -= 250;
            if (deadline_ms > 0) continue;
            kill(pid, SIGKILL);
            (void)waitpid(pid, NULL, 0);
            secure_zero(window, sizeof(window));
            close(master);
            return -1;
        }

        if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[512];
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
                pty_window_append(window, &window_used, buf, (size_t)n);
                continue;
            }
            if (n == 0) {
                int st = 0;
                (void)waitpid(pid, &st, 0);
                secure_zero(window, sizeof(window));
                close(master);
                return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            }
            if (errno != EINTR && errno != EAGAIN) {
                kill(pid, SIGKILL);
                (void)waitpid(pid, NULL, 0);
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
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) home = "/tmp";
    snprintf(buf, (size_t)buf_size, "%s/.ssh", home);
    return buf;
}

static bool read_file(const char *path, char *buf, size_t buf_size) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, buf_size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return n > 0;
}

/* Build the ssh-keygen argv for a given algorithm, writing to key_path.
 * Returns the algorithm label for the output struct. */
static const char *build_keygen_argv(KeyAlgorithm algo, const char *key_path,
                                      const char *passphrase, bool include_passphrase_arg,
                                      const char *comment,
                                      char *bits_buf, size_t bits_buf_size,
                                      const char *argv_out[16]) {
    const char *type = "rsa";
    int bits = 0;
    const char *label = "rsa";
    switch (algo) {
    case KEYGEN_RSA_2048:   type = "rsa";     bits = 2048; label = "rsa";       break;
    case KEYGEN_RSA_4096:   type = "rsa";     bits = 4096; label = "rsa";       break;
    case KEYGEN_ED25519:    type = "ed25519"; bits = 0;    label = "ed25519";   break;
    case KEYGEN_ECDSA_P256: type = "ecdsa";   bits = 256;  label = "ecdsa-256"; break;
    case KEYGEN_ECDSA_P384: type = "ecdsa";   bits = 384;  label = "ecdsa-384"; break;
    case KEYGEN_ECDSA_P521: type = "ecdsa";   bits = 521;  label = "ecdsa-521"; break;
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
    if (algorithm_out && algorithm_out_size > 0)
        snprintf(algorithm_out, algorithm_out_size, "%s", label);
    if (!has_passphrase) return run_silent(argv);

    const char *prompts[] = {
        "Enter passphrase",
        "Enter same passphrase again",
    };
    const char *replies[] = {
        passphrase,
        passphrase,
    };
    return run_pty_prompted(argv, prompts, replies, 2);
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
    return run_pty_prompted(argv, prompts, replies, 3);
}

/* Create a 0700 private scratch directory in $TMPDIR (or /tmp) via mkdtemp.
 * This avoids the predictable /tmp/Liu_keygen_<pid> path which was exposed
 * to symlink / TOCTOU attacks: the PID is guessable, and any user on the
 * system could race us to create the file as a symlink to /etc/passwd before
 * ssh-keygen wrote the private key. mkdtemp gives us a unique, randomly
 * named directory owned by the current user with 0700 permissions.
 * Writes the directory path into out_dir (buf_size >= 24). Returns true
 * on success. */
static bool keygen_mkscratch(char *out_dir, size_t buf_size) {
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
    /* Strip trailing slash for clean path joins */
    size_t tl = strlen(tmpdir);
    while (tl > 1 && tmpdir[tl - 1] == '/') tl--;
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
    memset(out, 0, sizeof(*out));

    /* Create a unique 0700 scratch directory; do NOT use a predictable
     * /tmp/<pid> path (symlink / TOCTOU attack surface). */
    char scratch[256];
    if (!keygen_mkscratch(scratch, sizeof(scratch))) return false;

    char tmp_priv[288], tmp_pub[296];
    snprintf(tmp_priv, sizeof(tmp_priv), "%s/id", scratch);
    snprintf(tmp_pub,  sizeof(tmp_pub),  "%s.pub", tmp_priv);

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
        if (run_capture(fargv, line, sizeof(line)) == 0) {
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
    /* Ensure parent directory exists */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", filepath);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(dir, 0700);
    }

    /* Remove existing files */
    char pub_path[520];
    snprintf(pub_path, sizeof(pub_path), "%s.pub", filepath);
    unlink(filepath);
    unlink(pub_path);

    /* Build argv directly — no shell interpretation */
    char bits_buf[16];
    int rc = run_keygen_generate(algo, filepath, passphrase, comment,
                                 bits_buf, sizeof(bits_buf), NULL, 0);
    if (rc != 0) return false;

    /* Set permissions */
    chmod(filepath, 0600);
    chmod(pub_path, 0644);

    /* Get fingerprint */
    if (fingerprint_out && fp_size > 0) {
        fingerprint_out[0] = '\0';
        const char *fargv[] = { "ssh-keygen", "-lf", pub_path, NULL };
        char line[512] = {0};
        if (run_capture(fargv, line, sizeof(line)) == 0) {
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
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    /* OpenSSH new format: look for bcrypt KDF (encrypted) */
    if (strstr(buf, "ENCRYPTED")) return true;

    /* OpenSSH new format: check for aes256-ctr or similar cipher */
    /* "-----BEGIN OPENSSH PRIVATE KEY-----" with bcrypt/aes is encrypted */
    if (strstr(buf, "-----BEGIN OPENSSH PRIVATE KEY-----")) {
        /* For new-format keys, try ssh-keygen -y with empty passphrase.
         * If it fails, the key is encrypted. */
        const char *argv[] = { "ssh-keygen", "-y", "-P", "", "-f", path, NULL };
        int rc = run_silent(argv);
        return rc != 0;
    }

    return false;
}

/* Detect key type from public key line */
static void detect_key_type(const char *pub_line, char *type_out, i32 type_size, i32 *bits_out) {
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
    *count = 0;

    char dir_path[512];
    if (!ssh_dir || !ssh_dir[0]) {
        ssh_get_default_dir(dir_path, sizeof(dir_path));
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
        snprintf(priv_path, sizeof(priv_path), "%s/%s", ssh_dir, name);

        /* Must be a regular file */
        struct stat st;
        if (stat(priv_path, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;

        /* Check if it has SSH key header */
        FILE *f = fopen(priv_path, "r");
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
        snprintf(ki->pub_path, sizeof(ki->pub_path), "%s.pub", priv_path);

        /* Check for passphrase */
        ki->has_passphrase = key_is_encrypted(priv_path);

        /* Read public key for type detection */
        char pub_content[4096];
        if (read_file(ki->pub_path, pub_content, sizeof(pub_content))) {
            detect_key_type(pub_content, ki->type, sizeof(ki->type), &ki->bits);
        } else {
            snprintf(ki->type, sizeof(ki->type), "unknown");
        }

        /* Get fingerprint via argv-based ssh-keygen -lf */
        {
            const char *fargv[] = { "ssh-keygen", "-lf", ki->pub_path, NULL };
            char line[512] = {0};
            if (run_capture(fargv, line, sizeof(line)) == 0) {
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
                    ki->bits = atoi(line);
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
    snprintf(pub_path, sizeof(pub_path), "%s.pub", private_key_path);

    bool ok = true;
    if (unlink(private_key_path) != 0) ok = false;
    /* Public key may not exist; that's fine */
    unlink(pub_path);
    return ok;
}

char *ssh_read_public_key(const char *pub_key_path) {
    if (!pub_key_path || !pub_key_path[0]) return NULL;

    FILE *f = fopen(pub_key_path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 16384) { fclose(f); return NULL; }

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t n = fread(buf, 1, (size_t)size, f);
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
    return run_silent(argv) == 0;
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

    /* Read the first few lines of the key file and check for encryption markers */
    FILE *f = fopen(key_path, "r");
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
        if (run_silent(argv) != 0) encrypted = true;
    }

    return encrypted;
}
