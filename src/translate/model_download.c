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
#include "translate/model_paths.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>       /* strcasecmp */
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifndef PLATFORM_WIN32
#include <openssl/evp.h>
#endif

/* Configure-time defaults (CMake passes the real values; these are the
 * fallbacks for an ad-hoc build). Default is AngelSlim's official Sherry
 * STQ1_0 GGUF, which the engine decodes since the PR #22836 stride-16 fix. */
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
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (md_cancel_requested()) {
            if (cancelled) *cancelled = true;
            ok = false;
            break;
        }
        ok = EVP_DigestUpdate(ctx, buf, n) == 1;
    }
    if (ferror(f)) ok = false;
    unsigned char md[32];
    unsigned int mdlen = 0;
    if (ok) ok = EVP_DigestFinal_ex(ctx, md, &mdlen) == 1 && mdlen == 32;
    if (ctx) EVP_MD_CTX_free(ctx);
    fclose(f);
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

/* fork+exec curl streaming `url` to `tmp`. Returns the pid, or -1. */
static pid_t md_spawn_curl(const char *url, const char *tmp, bool resume) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        const char *argv[24];
        int a = 0;
        argv[a++] = "curl";
        argv[a++] = "-fsSL";                 /* fail on HTTP error, silent, show error, follow redirects */
        argv[a++] = "--connect-timeout"; argv[a++] = "30";
        argv[a++] = "--retry";           argv[a++] = "3";
        argv[a++] = "--retry-delay";     argv[a++] = "2";
        argv[a++] = "--speed-limit";     argv[a++] = "1024";  /* abort if < 1 KB/s ... */
        argv[a++] = "--speed-time";      argv[a++] = "60";    /* ... sustained for 60 s */
        if (resume) { argv[a++] = "-C"; argv[a++] = "-"; }
        argv[a++] = "-o"; argv[a++] = tmp;
        argv[a++] = url;
        argv[a]   = NULL;
        execvp("curl", (char *const *)argv);
        _exit(127);   /* exec failed (curl not found) */
    }
    return pid;
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
    snprintf(tmp, sizeof tmp, "%s.part", dest);

    struct stat sb;
    bool resume = (stat(tmp, &sb) == 0 && sb.st_size > 0);

    pid_t pid = md_spawn_curl(url, tmp, resume);
    if (pid < 0) { md_finish_error("could not start curl"); return NULL; }

    pthread_mutex_lock(&g_md.mtx);
    g_md.curl_pid = pid;
    pthread_mutex_unlock(&g_md.mtx);

    f64 last_t = md_now();
    u64 last_b = 0;
    f64 speed  = 0.0;
    bool cancelled = false;
    int  curl_status = -1;

    for (;;) {
        pthread_mutex_lock(&g_md.mtx);
        bool want_cancel = g_md.cancel;
        pthread_mutex_unlock(&g_md.mtx);
        if (want_cancel && !cancelled) { kill(pid, SIGTERM); cancelled = true; }

        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        bool child_done = (r == pid);
        if (child_done) {
            curl_status = st;
            /* Child is reaped — clear the stored pid under the lock so a
             * concurrent cancel()/shutdown() can't kill() a recycled PID. */
            pthread_mutex_lock(&g_md.mtx);
            g_md.curl_pid = 0;
            pthread_mutex_unlock(&g_md.mtx);
        }

        u64 done = (stat(tmp, &sb) == 0) ? (u64)sb.st_size : 0;
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
        if (g_md.curl_pid > 0) kill(g_md.curl_pid, SIGTERM);
    }
    pthread_mutex_unlock(&g_md.mtx);
}

void model_download_shutdown(void) {
    pthread_mutex_lock(&g_md.mtx);
    bool started = g_md.thread_started;
    if (g_md.state == MODEL_DL_DOWNLOADING || g_md.state == MODEL_DL_VERIFYING) {
        g_md.cancel = true;
        if (g_md.curl_pid > 0) kill(g_md.curl_pid, SIGTERM);
    }
    pthread_mutex_unlock(&g_md.mtx);
    if (started) {
        pthread_join(g_md.thread, NULL);
        g_md.thread_started = false;
    }
}
