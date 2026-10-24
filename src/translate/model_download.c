/*
 * Liu - on-demand local-model downloader (implementation).
 *
 * Threading mirrors translate_local.c: one worker thread, all shared state
 * guarded by a single mutex (progress samples land every ~150 ms, so lock
 * contention is irrelevant). The worker forks `curl` to stream the file to a
 * "<dest>.part" temp, polls the temp's size for progress, then verifies the
 * SHA-256 and renames it into place. curl handles HTTPS, redirects (-L, so a
 * Hugging Face resolve/main URL works), and resume (-C -).
 */
#include "translate/model_download.h"
#include "core/curl_path.h"
#include "translate/model_paths.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>       /* strcasecmp */
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

extern char **environ;

#ifndef PLATFORM_WIN32
#include <openssl/evp.h>
#endif

/* Configure-time defaults (CMake passes the real values; these are the
 * fallbacks for an ad-hoc build). Default is AngelSlim's official Sherry
 * STQ1_0 GGUF, which the engine decodes. */
#ifndef LIU_MODEL_URL
#define LIU_MODEL_URL \
    "https://huggingface.co/AngelSlim/Hy-MT1.5-1.8B-1.25bit-GGUF/resolve/main/" \
    LIU_MODEL_FILENAME
#endif
#ifndef LIU_MODEL_SHA256
#define LIU_MODEL_SHA256 \
    "93e025c93cc082e73a3f142b757623a8b9cf541c020a8013ca4ee669556860ab"
#endif
#ifndef LIU_MODEL_SIZE
#define LIU_MODEL_SIZE 461860704ULL
#endif

#define MD_PATH_CAP 1024
#define MD_HARD_CAP (16ull * 1024 * 1024 * 1024) /* custom models: max 16 GiB */

static struct {
    pthread_mutex_t mtx;
    pthread_t       thread;
    bool            thread_started;

    /* shared state (mtx) */
    ModelDownloadState state;
    u64   bytes_done;
    u64   bytes_total;
    f64   speed_bps;
    f64   eta_sec;
    char  error[160];
    char  done_path[MD_PATH_CAP];
    pid_t curl_pid;     /* >0 while curl runs; used by cancel */
    bool  cancel;

    /* worker inputs (set under mtx before pthread_create) */
    char  url[MD_PATH_CAP];
    char  dest[MD_PATH_CAP];
    char  sha[65];
    u64   expected_size;
} g_md = {
    .mtx   = PTHREAD_MUTEX_INITIALIZER,
    .state = MODEL_DL_IDLE,
};

static f64 md_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
}

const char *model_download_default_url(void)    { return LIU_MODEL_URL; }
const char *model_download_default_sha256(void) { return LIU_MODEL_SHA256; }
u64         model_download_default_size(void)   { return (u64)LIU_MODEL_SIZE; }

const char *model_download_default_dest(char *out, usize cap) {
    return liu_model_default_path(out, cap);
}

/* mkdir -p for every component of `dir` (errors other than EEXIST ignored —
 * a real permission problem surfaces later as a curl write failure). */
static void md_mkdir_p(const char *dir) {
    char tmp[MD_PATH_CAP];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

/* Snapshot the cancel flag (also used by the verify read loop). */
static bool md_cancel_requested(void) {
    pthread_mutex_lock(&g_md.mtx);
    bool c = g_md.cancel;
    pthread_mutex_unlock(&g_md.mtx);
    return c;
}

/* Streaming SHA-256 of a file (no whole-file malloc for the 440 MB model).
 * Checks the cancel flag each block so a cancel during verification aborts
 * promptly instead of hashing the whole 440 MB; `*cancelled` is set in that
 * case and the function returns false. */
static bool md_sha256_file(const char *path, char out_hex[65], bool *cancelled) {
    if (cancelled) *cancelled = false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
    unsigned char buf[1 << 16];
    while (ok) {
        size_t n = fread(buf, 1, sizeof buf, f);
        if (md_cancel_requested()) {
            if (cancelled) *cancelled = true;
            ok = false;
            break;
        }
        if (n > 0) ok = EVP_DigestUpdate(ctx, buf, n) == 1;
        if (!ok) break;
        /* fread may return a short final block together with EOF or an I/O
         * error. Never issue another read after either condition. */
        if (ferror(f)) { ok = false; break; }
        if (n < sizeof(buf)) break;
    }
    unsigned char md[32];
    unsigned int mdlen = 0;
    if (ok) ok = EVP_DigestFinal_ex(ctx, md, &mdlen) == 1 && mdlen == 32;
    if (ctx) EVP_MD_CTX_free(ctx);
    if (fclose(f) != 0) ok = false;
    if (!ok) return false;
    for (int i = 0; i < 32; i++) snprintf(out_hex + i * 2, 3, "%02x", md[i]);
    out_hex[64] = '\0';
    return true;
}

static void md_finish_error(const char *msg) {
    pthread_mutex_lock(&g_md.mtx);
    g_md.state = MODEL_DL_ERROR;
    snprintf(g_md.error, sizeof g_md.error, "%s",
             msg ? msg : "download failed");
    g_md.curl_pid = 0;
    pthread_mutex_unlock(&g_md.mtx);
}

/* Spawn curl streaming `url` to `tmp`. Returns the pid, or -1. */
static pid_t md_spawn_curl(const char *url, const char *tmp, bool resume, u64 byte_cap) {
    if (!url || !tmp || !url[0] || !tmp[0]) return -1;
    char cap_arg[32];
    int nw = snprintf(cap_arg, sizeof cap_arg, "%llu",
                      (unsigned long long)byte_cap);
    if (nw < 0 || (usize)nw >= sizeof cap_arg) return -1;
    char *argv[28];
    int a = 0;
    argv[a++] = (char *)"curl";
    argv[a++] = (char *)"-fsSL";
    argv[a++] = (char *)"--connect-timeout"; argv[a++] = (char *)"30";
    argv[a++] = (char *)"--retry";           argv[a++] = (char *)"3";
    argv[a++] = (char *)"--retry-delay";     argv[a++] = (char *)"2";
    argv[a++] = (char *)"--max-time";        argv[a++] = (char *)"3600";
    argv[a++] = (char *)"--max-filesize";    argv[a++] = cap_arg;
    argv[a++] = (char *)"--proto";           argv[a++] = (char *)"=https";
    argv[a++] = (char *)"--proto-redir";     argv[a++] = (char *)"=https";
    argv[a++] = (char *)"--speed-limit";     argv[a++] = (char *)"1024";
    argv[a++] = (char *)"--speed-time";      argv[a++] = (char *)"60";
    if (resume) { argv[a++] = (char *)"-C"; argv[a++] = (char *)"-"; }
    argv[a++] = (char *)"-o"; argv[a++] = (char *)tmp;
    argv[a++] = (char *)url;
    argv[a] = NULL;

    posix_spawn_file_actions_t fa;
    int rc = posix_spawn_file_actions_init(&fa);
    if (rc != 0) return -1;
    rc = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO,
                                           "/dev/null", O_RDONLY, 0);
    pid_t pid = 0;
    if (rc == 0) rc = posix_spawnp(&pid, liu_curl_exe(), &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    return rc == 0 && pid > 0 ? pid : -1;
}

static void *md_worker(void *arg) {
    (void)arg;

    char url[MD_PATH_CAP], dest[MD_PATH_CAP], sha[65];
    u64 esize;
    pthread_mutex_lock(&g_md.mtx);
    snprintf(url, sizeof url, "%s", g_md.url);
    snprintf(dest, sizeof dest, "%s", g_md.dest);
    snprintf(sha, sizeof sha, "%s", g_md.sha);
    esize = g_md.expected_size;
    pthread_mutex_unlock(&g_md.mtx);

    /* Ensure the destination directory exists. */
    char dir[MD_PATH_CAP];
    snprintf(dir, sizeof dir, "%s", dest);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; md_mkdir_p(dir); }

    char tmp[MD_PATH_CAP];
    int tmp_n = snprintf(tmp, sizeof tmp, "%s.part", dest);
    if (tmp_n < 0 || (usize)tmp_n >= sizeof tmp) {
        md_finish_error("destination path is too long");
        return NULL;
    }

    u64 byte_cap = esize ? esize : MD_HARD_CAP;
    if (byte_cap > MD_HARD_CAP) byte_cap = MD_HARD_CAP;

    struct stat sb;
    bool resume = (stat(tmp, &sb) == 0 && sb.st_size > 0 &&
                   (u64)sb.st_size <= byte_cap);
    if (stat(tmp, &sb) == 0 && (sb.st_size < 0 || (u64)sb.st_size > byte_cap))
        (void)unlink(tmp);

    pid_t pid = md_spawn_curl(url, tmp, resume, byte_cap);
    if (pid < 0) { md_finish_error("could not start curl"); return NULL; }

    pthread_mutex_lock(&g_md.mtx);
    g_md.curl_pid = pid;
    pthread_mutex_unlock(&g_md.mtx);

    f64 last_t = md_now();
    u64 last_b = 0;
    f64 speed  = 0.0;
    bool cancelled = false;
    bool oversized = false;
    f64  cancel_at = 0.0;
    int  curl_status = -1;

    for (;;) {
        pthread_mutex_lock(&g_md.mtx);
        bool want_cancel = g_md.cancel;
        pthread_mutex_unlock(&g_md.mtx);
        if (want_cancel && !cancelled) {
            kill(pid, SIGTERM);
            cancelled = true;
            cancel_at = md_now();
        } else if (cancelled && md_now() - cancel_at > 2.0) {
            /* A wedged curl must not make model_download_shutdown hang. */
            kill(pid, SIGKILL);
        }

        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r < 0 && errno == EINTR) continue;
        bool child_done = (r == pid || r < 0);
        if (child_done) {
            curl_status = (r == pid) ? st : -1;
            /* Child is reaped — clear the stored pid under the lock so a
             * concurrent cancel()/shutdown() can't kill() a recycled PID. */
            pthread_mutex_lock(&g_md.mtx);
            g_md.curl_pid = 0;
            pthread_mutex_unlock(&g_md.mtx);
        }

        u64 done = 0;
        if (stat(tmp, &sb) == 0 && sb.st_size >= 0) done = (u64)sb.st_size;
        if (done > byte_cap && !oversized) {
            oversized = true;
            if (!child_done) kill(pid, SIGTERM);
            if (cancel_at == 0.0) cancel_at = md_now();
        }
        if (oversized && !child_done && md_now() - cancel_at > 2.0) kill(pid, SIGKILL);
        f64 now = md_now();
        f64 dt  = now - last_t;
        if (dt >= 0.2) {
            u64 db   = done >= last_b ? done - last_b : 0;
            f64 inst = dt > 0 ? (f64)db / dt : 0.0;
            speed = (speed <= 0.0) ? inst : speed * 0.6 + inst * 0.4;
            last_t = now;
            last_b = done;
        }
        f64 eta = (speed > 1.0 && esize && (f64)esize > (f64)done)
                      ? ((f64)esize - (f64)done) / speed : -1.0;

        pthread_mutex_lock(&g_md.mtx);
        g_md.bytes_done  = done;
        g_md.bytes_total = esize;
        g_md.speed_bps   = speed;
        g_md.eta_sec     = eta;
        pthread_mutex_unlock(&g_md.mtx);

        if (child_done) break;
        usleep(150000);   /* 150 ms */
    }

    if (cancelled) {
        unlink(tmp);
        pthread_mutex_lock(&g_md.mtx);
        g_md.state   = MODEL_DL_CANCELLED;
        g_md.curl_pid = 0;
        g_md.cancel  = false;
        pthread_mutex_unlock(&g_md.mtx);
        return NULL;
    }

    if (oversized) {
        unlink(tmp);
        md_finish_error("download exceeded the size limit");
        return NULL;
    }

    if (!(WIFEXITED(curl_status) && WEXITSTATUS(curl_status) == 0)) {
        unlink(tmp);   /* don't trust a half file across a hard failure */
        md_finish_error("download failed (network or server error)");
        return NULL;
    }

    if (esize && (stat(tmp, &sb) != 0 || (u64)sb.st_size != esize)) {
        unlink(tmp);
        md_finish_error("downloaded size does not match expected");
        return NULL;
    }

    pthread_mutex_lock(&g_md.mtx);
    g_md.state    = MODEL_DL_VERIFYING;
    g_md.curl_pid = 0;
    pthread_mutex_unlock(&g_md.mtx);

    if (sha[0]) {
        char got[65];
        bool hash_cancelled = false;
        if (!md_sha256_file(tmp, got, &hash_cancelled)) {
            unlink(tmp);
            /* A cancel during verification aborts the hash; report it as a
             * cancellation rather than a hashing failure. */
            if (hash_cancelled) {
                pthread_mutex_lock(&g_md.mtx);
                g_md.state    = MODEL_DL_CANCELLED;
                g_md.curl_pid = 0;
                g_md.cancel   = false;
                pthread_mutex_unlock(&g_md.mtx);
                return NULL;
            }
            md_finish_error("could not hash downloaded file");
            return NULL;
        }
        if (strcasecmp(got, sha) != 0) {
            unlink(tmp);
            md_finish_error("checksum mismatch — file corrupt, deleted");
            return NULL;
        }
    }

    if (rename(tmp, dest) != 0) {
        unlink(tmp);
        md_finish_error("could not move file into place");
        return NULL;
    }

    pthread_mutex_lock(&g_md.mtx);
    g_md.state      = MODEL_DL_DONE;
    g_md.bytes_done = esize ? esize : g_md.bytes_done;
    g_md.eta_sec    = 0.0;
    snprintf(g_md.done_path, sizeof g_md.done_path, "%s", dest);
    pthread_mutex_unlock(&g_md.mtx);
    return NULL;
}

bool model_download_start(const char *url, const char *dest,
                          const char *expected_sha256, u64 expected_size) {
    char dbuf[MD_PATH_CAP];
    if (!dest || !dest[0]) {
        if (!model_download_default_dest(dbuf, sizeof dbuf)) return false;
        dest = dbuf;
    }
    if (!url || !url[0])                 url = LIU_MODEL_URL;
    if (!expected_sha256)               expected_sha256 = LIU_MODEL_SHA256;
    if (expected_size == 0)             expected_size = (u64)LIU_MODEL_SIZE;
    if (strncmp(url, "https://", 8) != 0 || strlen(url) >= sizeof g_md.url ||
        strlen(dest) >= sizeof g_md.dest - (sizeof ".part" - 1u) ||
        expected_size == 0 || expected_size > MD_HARD_CAP) return false;
    if (expected_sha256[0]) {
        if (strlen(expected_sha256) != 64) return false;
        for (i32 i = 0; i < 64; i++) {
            char c = expected_sha256[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return false;
        }
    }

    pthread_mutex_lock(&g_md.mtx);
    if (g_md.state == MODEL_DL_DOWNLOADING || g_md.state == MODEL_DL_VERIFYING) {
        pthread_mutex_unlock(&g_md.mtx);
        return false;                   /* one download at a time */
    }
    pthread_mutex_unlock(&g_md.mtx);

    /* A prior worker may have finished but not been joined — join it before
     * the handle is reused. Only the main thread calls start(), so this is
     * race-free. */
    if (g_md.thread_started) {
        pthread_join(g_md.thread, NULL);
        g_md.thread_started = false;
    }

    pthread_mutex_lock(&g_md.mtx);
    snprintf(g_md.url,  sizeof g_md.url,  "%s", url);
    snprintf(g_md.dest, sizeof g_md.dest, "%s", dest);
    snprintf(g_md.sha,  sizeof g_md.sha,  "%s", expected_sha256);
    g_md.expected_size = expected_size;
    g_md.cancel        = false;
    g_md.curl_pid      = 0;
    g_md.state         = MODEL_DL_DOWNLOADING;   /* visible to active() at once */
    g_md.bytes_done    = 0;
    g_md.bytes_total   = expected_size;
    g_md.speed_bps     = 0.0;
    g_md.eta_sec       = -1.0;
    g_md.error[0]      = '\0';
    g_md.done_path[0]  = '\0';
    pthread_mutex_unlock(&g_md.mtx);

    if (pthread_create(&g_md.thread, NULL, md_worker, NULL) != 0) {
        pthread_mutex_lock(&g_md.mtx);
        g_md.state = MODEL_DL_ERROR;
        snprintf(g_md.error, sizeof g_md.error, "could not start worker thread");
        pthread_mutex_unlock(&g_md.mtx);
        return false;
    }
    g_md.thread_started = true;
    return true;
}

void model_download_poll(ModelDownloadStatus *out) {
    if (!out) return;
    pthread_mutex_lock(&g_md.mtx);
    out->state       = g_md.state;
    out->bytes_done  = g_md.bytes_done;
    out->bytes_total = g_md.bytes_total;
    out->speed_bps   = g_md.speed_bps;
    out->eta_sec     = g_md.eta_sec;
    snprintf(out->dest_path, sizeof out->dest_path, "%s", g_md.done_path);
    snprintf(out->error,     sizeof out->error,     "%s", g_md.error);
    pthread_mutex_unlock(&g_md.mtx);
}

bool model_download_active(void) {
    pthread_mutex_lock(&g_md.mtx);
    bool a = g_md.state == MODEL_DL_DOWNLOADING || g_md.state == MODEL_DL_VERIFYING;
    pthread_mutex_unlock(&g_md.mtx);
    return a;
}

void model_download_cancel(void) {
    pthread_mutex_lock(&g_md.mtx);
    if (g_md.state == MODEL_DL_DOWNLOADING || g_md.state == MODEL_DL_VERIFYING) {
        g_md.cancel = true;
        /* The worker alone signals/reaps curl. Killing here can race the tiny
         * window after waitpid reaped the child but before curl_pid is cleared,
         * potentially signaling a recycled PID. The worker polls in <=150 ms. */
    }
    pthread_mutex_unlock(&g_md.mtx);
}

void model_download_shutdown(void) {
    pthread_mutex_lock(&g_md.mtx);
    bool started = g_md.thread_started;
    if (g_md.state == MODEL_DL_DOWNLOADING || g_md.state == MODEL_DL_VERIFYING) {
        g_md.cancel = true;
    }
    pthread_mutex_unlock(&g_md.mtx);
    if (started) {
        pthread_join(g_md.thread, NULL);
        g_md.thread_started = false;
    }
}
