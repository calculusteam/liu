/* updater.c — C11 auto-updater logic (no Cocoa here).
 * State machine, semver, feed parsing, and the security-critical SHA-256 +
 * Ed25519 verification (OpenSSL EVP, already linked via libssh2's libcrypto).
 */
#include "update/updater.h"
#include "core/update_pubkey.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <limits.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "cJSON.h"
#ifndef PLATFORM_WIN32
#include <openssl/evp.h>
#endif

#ifndef LIU_VERSION
#define LIU_VERSION "0.0.0"
#endif
#ifndef LIU_UPDATE_FEED_URL
#define LIU_UPDATE_FEED_URL ""
#endif

/* Which platform's artifact this build may install. Compile-time, because the
 * answer cannot change at runtime, and it is the half of artifact identity
 * that "arch" alone does not carry. */
#if defined(__APPLE__) || defined(PLATFORM_MACOS)
#define LIU_UPDATE_OS "macos"
#elif defined(__linux__) || defined(PLATFORM_LINUX)
#define LIU_UPDATE_OS "linux"
#else
#define LIU_UPDATE_OS "unknown"
#endif

const char *updater_current_version(void) { return LIU_VERSION; }

/* ===================== semver ===================== */

static void parse_ver(const char *s, int out[3], const char **pre) {
    out[0] = out[1] = out[2] = 0;
    *pre = NULL;
    if (!s) return;
    if (*s == 'v' || *s == 'V') s++;
    for (int i = 0; i < 3; i++) {
        while (*s >= '0' && *s <= '9') {
            /* Saturate instead of overflowing on a hostile component like
             * "99999999999" — signed overflow is UB and would mis-compare. */
            if (out[i] <= (INT_MAX - 9) / 10) out[i] = out[i] * 10 + (*s - '0');
            else out[i] = INT_MAX;
            s++;
        }
        if (*s == '-') { *pre = s + 1; return; }
        if (*s == '+' || *s == '\0') return;
        if (*s == '.') { s++; continue; }
        return; /* unexpected char */
    }
    if (*s == '-') *pre = s + 1;
}

/* Compare two SemVer prerelease strings by dot-separated identifiers (SemVer
 * §11): numeric identifiers compare numerically, numeric ranks below
 * alphanumeric, alphanumeric compare lexically (ASCII), and a shorter prefix of
 * identifiers ranks below a longer one. Build metadata (from '+') is ignored.
 * apre/bpre point into the original version string and run to its end, so an
 * identifier ends at '.', '+' (build meta) or '\0'. Replaces a raw strcmp that
 * mis-ordered e.g. "alpha.2" vs "alpha.10" and let build metadata leak in. */
static int cmp_prerelease(const char *a, const char *b) {
    for (;;) {
        bool aend = (*a == '\0' || *a == '+');
        bool bend = (*b == '\0' || *b == '+');
        if (aend && bend) return 0;
        if (aend) return -1;
        if (bend) return 1;
        const char *ae = a, *be = b;
        while (*ae && *ae != '.' && *ae != '+') ae++;
        while (*be && *be != '.' && *be != '+') be++;
        size_t alen = (size_t)(ae - a), blen = (size_t)(be - b);
        bool anum = alen > 0, bnum = blen > 0;
        for (const char *p = a; p < ae; p++) if (*p < '0' || *p > '9') { anum = false; break; }
        for (const char *p = b; p < be; p++) if (*p < '0' || *p > '9') { bnum = false; break; }
        int c = 0;
        if (anum && bnum) {
            const char *as = a; while (as < ae - 1 && *as == '0') as++;   /* strip leading zeros */
            const char *bs = b; while (bs < be - 1 && *bs == '0') bs++;
            size_t an = (size_t)(ae - as), bn = (size_t)(be - bs);
            if (an != bn) c = an < bn ? -1 : 1;                            /* longer number = larger */
            else c = strncmp(as, bs, an);
        } else if (anum != bnum) {
            c = anum ? -1 : 1;                                            /* numeric < alphanumeric */
        } else {
            size_t m = alen < blen ? alen : blen;
            c = strncmp(a, b, m);
            if (c == 0 && alen != blen) c = alen < blen ? -1 : 1;         /* shorter prefix ranks lower */
        }
        if (c != 0) return c < 0 ? -1 : 1;
        a = (*ae == '.') ? ae + 1 : ae;
        b = (*be == '.') ? be + 1 : be;
    }
}

int liu_semver_cmp(const char *a, const char *b) {
    int an[3], bn[3];
    const char *apre, *bpre;
    parse_ver(a, an, &apre);
    parse_ver(b, bn, &bpre);
    for (int i = 0; i < 3; i++)
        if (an[i] != bn[i]) return an[i] < bn[i] ? -1 : 1;
    /* equal numeric: a prerelease ranks BELOW the same release (SemVer §11). */
    bool ap = apre != NULL, bp = bpre != NULL;
    if (ap && !bp) return -1;
    if (!ap && bp) return 1;
    if (ap && bp) return cmp_prerelease(apre, bpre);
    return 0;
}

bool liu_update_is_newer(const char *feed_ver, const char *cur_ver) {
    return liu_semver_cmp(feed_ver, cur_ver) > 0;
}

/* ===================== crypto ===================== */

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode standard base64 into out; returns decoded length, or 0 on bad input. */
static size_t b64_decode(const char *in, unsigned char *out, size_t cap) {
    size_t outn = 0;
    int quad[4], qi = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '=' || *p == '\n' || *p == '\r' || *p == ' ') {
            if (*p == '=') break;
            continue;
        }
        int v = b64_val((unsigned char)*p);
        if (v < 0) return 0;
        quad[qi++] = v;
        if (qi == 4) {
            if (outn + 3 > cap) return 0;
            out[outn++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
            out[outn++] = (unsigned char)((quad[1] << 4) | (quad[2] >> 2));
            out[outn++] = (unsigned char)((quad[2] << 6) | quad[3]);
            qi = 0;
        }
    }
    if (qi == 2) { if (outn + 1 > cap) return 0; out[outn++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4)); }
    else if (qi == 3) {
        if (outn + 2 > cap) return 0;
        out[outn++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
        out[outn++] = (unsigned char)((quad[1] << 4) | (quad[2] >> 2));
    }
    return outn;
}

/* The update file is capped at 1 GiB. Hashing streams chunks from an fd, but
 * pure Ed25519 verification is one-shot, so the signed bytes are
 * memory-mapped. File-backed clean pages are demand-paged and reclaimable; a
 * heap-buffer fallback is used if mmap fails.
 *
 * Signing a manifest instead would change the signed object and break
 * verification by existing clients. Passing one fd through hashing, signature
 * verification, and the pre-extraction check ensures each stage reads the same
 * file object and narrows the TOCTOU window. */

#define UPD_MAX_ARTIFACT_BYTES (1024ull * 1024 * 1024)

/* Streaming SHA-256 of the whole file behind `fd`. Constant memory. */
static bool sha256_hex_fd(int fd, char out_hex[65], u64 *out_len) {
    if (fd < 0) return false;
    if (lseek(fd, 0, SEEK_SET) != 0) return false;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;

    unsigned char chunk[64 * 1024];
    u64 total = 0;
    while (ok) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0) { if (errno == EINTR) continue; ok = false; break; }
        if (n == 0) break;
        total += (u64)n;
        if (total > UPD_MAX_ARTIFACT_BYTES) { ok = false; break; }
        if (EVP_DigestUpdate(ctx, chunk, (size_t)n) != 1) { ok = false; break; }
    }

    unsigned char md[32];
    unsigned int mdlen = 0;
    if (ok) ok = EVP_DigestFinal_ex(ctx, md, &mdlen) == 1 && mdlen == 32;
    EVP_MD_CTX_free(ctx);
    if (!ok) return false;

    for (int i = 0; i < 32; i++) snprintf(out_hex + i * 2, 3, "%02x", md[i]);
    out_hex[64] = '\0';
    if (out_len) *out_len = total;
    return true;
}

/* The public key used for Ed25519 verification in this file. A self-test build can substitute its
 * verification key. */
static const unsigned char *g_pubkey = LIU_UPDATE_PUBKEY;
static size_t g_pubkey_len = LIU_UPDATE_PUBKEY_LEN;
#ifdef UPDATER_SELFTEST
void updater_selftest_set_pubkey(const unsigned char *pk, size_t len) { g_pubkey = pk; g_pubkey_len = len; }
#endif

/* Ed25519 over a buffer. */
static bool ed25519_verify_buf(const unsigned char *data, size_t len,
                               const unsigned char *sig, size_t sig_len) {
    if (!data || sig_len != 64) return false;
    unsigned char empty = 0;
    bool ok = false;
    EVP_PKEY *pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, g_pubkey, g_pubkey_len);
    if (pk) {
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pk) == 1)
            ok = (EVP_DigestVerify(ctx, sig, sig_len, len ? data : &empty, len) == 1);
        if (ctx) EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pk);
    }
    return ok;
}

/* Ed25519 over the file behind `fd`, mapped rather than copied. */
static bool ed25519_verify_fd(int fd, u64 len,
                              const unsigned char *sig, size_t sig_len) {
    if (fd < 0 || sig_len != 64 || len > UPD_MAX_ARTIFACT_BYTES) return false;

    const unsigned char *data = NULL;
    void *map = MAP_FAILED;
    unsigned char *heap = NULL;
    unsigned char empty = 0;

    if (len == 0) {
        data = &empty;                     /* mmap of length 0 is invalid */
    } else {
        map = mmap(NULL, (size_t)len, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
            data = (const unsigned char *)map;
        } else {
            /* Fallback: a platform or filesystem that will not map. Costs the
             * allocation this function exists to avoid, but an update that
             * cannot be verified at all would be worse. */
            if (lseek(fd, 0, SEEK_SET) != 0) return false;
            heap = (unsigned char *)malloc((size_t)len);
            if (!heap) return false;
            u64 got = 0;
            while (got < len) {
                ssize_t n = read(fd, heap + got, (size_t)(len - got));
                if (n < 0) { if (errno == EINTR) continue; break; }
                if (n == 0) break;
                got += (u64)n;
            }
            if (got != len) { free(heap); return false; }
            data = heap;
        }
    }

    bool ok = ed25519_verify_buf(data, (size_t)len, sig, sig_len);

    if (map != MAP_FAILED) munmap(map, (size_t)len);
    free(heap);
    return ok;
}

/* Open the artifact once, for everything that follows. O_NOFOLLOW: the
 * download path is ours, and a symlink appearing at it is not something to
 * follow into. */
static int upd_open_artifact(const char *path, u64 *out_len) {
    if (out_len) *out_len = 0;
    if (!path || !*path) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat sb;
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) ||
        (u64)sb.st_size > UPD_MAX_ARTIFACT_BYTES) {
        close(fd);
        return -1;
    }
    if (out_len) *out_len = (u64)sb.st_size;
    return fd;
}

/* ===================== state helpers ===================== */

static void upd_fail(UpdateState *st, const char *msg) {
    snprintf(st->err, sizeof st->err, "%s", msg);
    atomic_store(&st->phase, UPD_ERROR);
}

void updater_init(UpdateState *st) {
    if (!st) return;
    memset(st, 0, sizeof *st);
    atomic_init(&st->phase, UPD_IDLE);
    atomic_init(&st->io_done, false);
    atomic_init(&st->io_ok, false);
    atomic_init(&st->feed_len, 0);
    atomic_init(&st->bytes_done, 0);
    atomic_init(&st->bytes_total, 0);
    atomic_init(&st->io_workers, 0);
    atomic_init(&st->io_cancel_epoch, 0);
    st->auto_install_allowed = updater_plat_can_autoinstall();
    st->initialized = true;

#ifndef NDEBUG
    /* Dev-only UI hook: LIU_UPDATE_FORCE_AVAILABLE=<ver> fakes an available
     * update so the toolbar chip / About flow can be exercised without
     * shipping a newer release. Display-only and fail-closed: "Install" on
     * the empty URL/signature errors out in the normal verify path. Compiled
     * out of Release builds via NDEBUG. */
    const char *force = getenv("LIU_UPDATE_FORCE_AVAILABLE");
    if (force && *force) {
        snprintf(st->avail_version, sizeof st->avail_version, "%s", force);
        snprintf(st->status, sizeof st->status, "Update %s available.", force);
        atomic_store(&st->phase, UPD_AVAILABLE);
    }
#endif
}

/* ===================== the signed feed ===================== */

#define FEED_SIG_MARKER ",\"feed_signature\":\""

/* Domain separation prevents a signature made for another release artifact or manifest from
 * verifying as a feed. The signed message is the context string followed by the feed bytes. */
#define FEED_SIG_CONTEXT "liu-update-feed-v1\n"

UpdFeedSig updater_feed_verify(const char *body, size_t len, size_t *signed_len) {
    if (signed_len) *signed_len = 0;
    if (!body || len == 0) return UPD_FEED_SIG_MISSING;
    const size_t ml = sizeof FEED_SIG_MARKER - 1;
    const char *found = NULL;
    /* The LAST marker: a note or URL inside the signed text may contain the same bytes, but
     * nothing after the real signature can, and the tail shape is checked below. */
    if (len >= ml)
        for (size_t i = len - ml + 1; i-- > 0; )
            if (memcmp(body + i, FEED_SIG_MARKER, ml) == 0) { found = body + i; break; }
    if (!found) return UPD_FEED_SIG_MISSING;

    const char *sig_start = found + ml;
    size_t room = (size_t)((body + len) - sig_start);
    const char *q = memchr(sig_start, '"', room < 128 ? room : 128);
    if (!q) return UPD_FEED_SIG_MALFORMED;
    /* After the closing quote: the object's closing brace and nothing but whitespace. */
    const char *t = q + 1;
    if (t >= body + len || *t != '}') return UPD_FEED_SIG_MALFORMED;
    for (t++; t < body + len; t++)
        if (*t != ' ' && *t != '\t' && *t != '\r' && *t != '\n') return UPD_FEED_SIG_MALFORMED;

    char b64[130];
    size_t bl = (size_t)(q - sig_start);
    if (bl == 0 || bl >= sizeof b64) return UPD_FEED_SIG_MALFORMED;
    memcpy(b64, sig_start, bl);
    b64[bl] = '\0';
    unsigned char sig[80];
    if (b64_decode(b64, sig, sizeof sig) != 64) return UPD_FEED_SIG_MALFORMED;

    size_t msg = (size_t)(found - body);
    if (signed_len) *signed_len = msg;

    const size_t cl = sizeof FEED_SIG_CONTEXT - 1;
    unsigned char *buf = (unsigned char *)malloc(cl + msg);
    if (!buf) return UPD_FEED_SIG_BAD;                    /* cannot check = not verified */
    memcpy(buf, FEED_SIG_CONTEXT, cl);
    memcpy(buf + cl, body, msg);
    bool ok = ed25519_verify_buf(buf, cl + msg, sig, 64);
    free(buf);
    return ok ? UPD_FEED_SIG_OK : UPD_FEED_SIG_BAD;
}

UpdFeedFresh updater_feed_freshness(long long generated, long long expires,
                                    long long now, long long seen) {
    if (generated <= 0) return UPD_FEED_FRESH_NO_TIME;
    if (expires > 0 && now > expires) return UPD_FEED_FRESH_EXPIRED;
    if (generated < seen) return UPD_FEED_FRESH_OLDER;
    return UPD_FEED_FRESH_OK;
}

/* The per-user state file stores the generated timestamp of the newest accepted feed as
 * `generated <epoch>\n`. Feeds with an earlier timestamp are rejected even if their signature is
 * valid. An absent or unreadable file means no accepted feed is recorded. */
static bool updater_state_path(char *out, size_t cap) {
#ifdef UPDATER_SELFTEST
    /* Release builds ignore `LIU_UPDATE_STATE` so the environment cannot redirect the replay
     * marker. */
    const char *e = getenv("LIU_UPDATE_STATE");
    if (e && e[0]) return snprintf(out, cap, "%s", e) < (int)cap;
#endif
    const char *home = getenv("HOME");
    if (!home || !home[0]) return false;
    return snprintf(out, cap, "%s/.config/Liu/update_state", home) < (int)cap;
}

static long long updater_state_load(void) {
    char path[1024];
    if (!updater_state_path(path, sizeof path)) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[64] = "";
    long long v = 0;
    if (fgets(line, sizeof line, f) && strncmp(line, "generated ", 10) == 0) {
        errno = 0;
        char *end = NULL;
        long long n = strtoll(line + 10, &end, 10);
        if (end != line + 10 && errno != ERANGE && n > 0) v = n;
    }
    fclose(f);
    return v;
}

static void updater_state_store(long long generated) {
    char path[1024], tmp[1100];
    if (!updater_state_path(path, sizeof path)) return;
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return;
    {
        /* Create the parent directories before writing the state file. */
        char dir[1024];
        const char *home = getenv("HOME");
        if (home && home[0]) {
            snprintf(dir, sizeof dir, "%s/.config", home); (void)mkdir(dir, 0700);
            snprintf(dir, sizeof dir, "%s/.config/Liu", home); (void)mkdir(dir, 0700);
        }
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    char buf[64];
    int n = snprintf(buf, sizeof buf, "generated %lld\n", generated);
    bool ok = n > 0 && write(fd, buf, (size_t)n) == n;
    ok = (close(fd) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) unlink(tmp);
}

/* ===================== feed parsing ===================== */

/* Pick the version entry to offer. Prefer feed.latest when it is a usable
 * (present, newer-than-current, non-yanked) entry — this honors a staged
 * rollout where "latest" may deliberately trail the highest build. If "latest"
 * is missing or yanked, fall back to the newest non-yanked entry that is still
 * newer than the running version, so yanking the latest build doesn't strand
 * users on their current version until the feed re-points "latest". */
static cJSON *updater_pick_version(cJSON *versions, const char *latest_ver) {
    const char *cur = updater_current_version();
    cJSON *best = NULL;
    const char *best_ver = NULL;
    cJSON *v = NULL;
    cJSON_ArrayForEach(v, versions) {
        cJSON *vv = cJSON_GetObjectItemCaseSensitive(v, "version");
        if (!cJSON_IsString(vv)) continue;
        const char *ver = vv->valuestring;
        if (!liu_update_is_newer(ver, cur)) continue;   /* not an upgrade */
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v, "yanked"))) continue;
        /* A usable "latest" wins verbatim — never promote an un-promoted build
         * above what the feed designates as latest. */
        if (latest_ver && strcmp(ver, latest_ver) == 0) return v;
        if (!best_ver || liu_semver_cmp(ver, best_ver) > 0) { best = v; best_ver = ver; }
    }
    return best;
}

/* Pin an artifact URL's host to the compiled-in feed's host. The feed's Ed25519
 * signature is checked before the feed is parsed and the artifact is verified
 * as well; this pin also keeps a download from ever connecting off the release
 * host (no forced request to another server, no tracking beacon). Host = the
 * text between "https://" and the first '/'. An empty or non-https feed (a
 * build with updates disabled) matches nothing, so no download is attempted. */
static bool url_host_matches_feed(const char *url) {
    const char *feed = LIU_UPDATE_FEED_URL;
    if (!url || strncmp(feed, "https://", 8) != 0 || strncmp(url, "https://", 8) != 0)
        return false;
    const char *fh = feed + 8, *uh = url + 8;
    size_t fn = strcspn(fh, "/");
    size_t un = strcspn(uh, "/");
    return fn > 0 && fn == un && strncasecmp(fh, uh, fn) == 0;
}

/* Returns true and fills avail_* when a compatible, newer, non-yanked version
 * exists. Sets st->status with a reason otherwise (e.g. min-os, malformed). */
static bool updater_feed_reject(UpdateState *st, const char *why) {
    st->feed_rejected = true;
    snprintf(st->status, sizeof st->status, "%s", why);
    return false;
}

static bool updater_parse_feed(UpdateState *st) {
    st->feed_rejected = false;
    const char *body = st->feed_body ? st->feed_body : "";

    /* Signature first, before the JSON is even parsed: what is parsed below is exactly what was
     * signed (the member after the marker is the signature, and the tail shape was checked). */
    switch (updater_feed_verify(body, strlen(body), NULL)) {
    case UPD_FEED_SIG_OK: break;
    case UPD_FEED_SIG_MISSING:
        return updater_feed_reject(st, "Update feed is not signed \xe2\x80\x94 ignoring it.");
    default:
        return updater_feed_reject(st, "Update feed signature is invalid \xe2\x80\x94 ignoring it.");
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) { snprintf(st->status, sizeof st->status, "Update feed is unreadable."); return false; }

    /* Authentic, so now: is it FRESH? A signature does not say when. */
    {
        cJSON *gen = cJSON_GetObjectItemCaseSensitive(root, "generated");
        cJSON *exp = cJSON_GetObjectItemCaseSensitive(root, "expires");
        long long g = 0, e = 0;
        if (cJSON_IsNumber(gen) && isfinite(gen->valuedouble) && gen->valuedouble > 0 && gen->valuedouble < 4e12)
            g = (long long)gen->valuedouble;
        if (cJSON_IsNumber(exp) && isfinite(exp->valuedouble) && exp->valuedouble > 0 && exp->valuedouble < 4e12)
            e = (long long)exp->valuedouble;
        long long seen = updater_state_load();
        switch (updater_feed_freshness(g, e, (long long)time(NULL), seen)) {
        case UPD_FEED_FRESH_OK: break;
        case UPD_FEED_FRESH_NO_TIME:
            cJSON_Delete(root);
            return updater_feed_reject(st, "Update feed has no generation time \xe2\x80\x94 ignoring it.");
        case UPD_FEED_FRESH_EXPIRED:
            cJSON_Delete(root);
            return updater_feed_reject(st, "Update feed has expired \xe2\x80\x94 ignoring it.");
        case UPD_FEED_FRESH_OLDER:
            cJSON_Delete(root);
            return updater_feed_reject(st, "Update feed is older than one already seen \xe2\x80\x94 ignoring it.");
        }
        if (g > seen) updater_state_store(g);
    }

    bool found = false;
    cJSON *latest = cJSON_GetObjectItemCaseSensitive(root, "latest");
    const char *lver = cJSON_IsString(latest) ? latest->valuestring : NULL;
    cJSON *versions = cJSON_GetObjectItemCaseSensitive(root, "versions");

    cJSON *v = cJSON_IsArray(versions) ? updater_pick_version(versions, lver) : NULL;
    if (v) {
        const char *sel_ver = cJSON_GetObjectItemCaseSensitive(v, "version")->valuestring;
        cJSON *minos = cJSON_GetObjectItemCaseSensitive(v, "minimumSystemVersion");
        const char *min_os = cJSON_IsString(minos) ? minos->valuestring : "11.0";
        cJSON *notes = cJSON_GetObjectItemCaseSensitive(v, "notesURL");
        cJSON *arts = cJSON_GetObjectItemCaseSensitive(v, "artifacts");

        const char *arch = updater_plat_arch();
        cJSON *a = NULL, *match = NULL;
        cJSON_ArrayForEach(a, arts) {
            cJSON *aa = cJSON_GetObjectItemCaseSensitive(a, "arch");
            if (!cJSON_IsString(aa) || strcmp(aa->valuestring, arch) != 0) continue;
            /* Architecture alone does not identify an artifact because the same
             * architecture may have separate macOS and Linux builds. If `os` is
             * absent, interpret the entry as macOS for feed compatibility. */
            cJSON *ao = cJSON_GetObjectItemCaseSensitive(a, "os");
            const char *aos = cJSON_IsString(ao) ? ao->valuestring : "macos";
            if (strcmp(aos, LIU_UPDATE_OS) != 0) continue;
            match = a;
            break;
        }
        if (!match) {
            snprintf(st->status, sizeof st->status, "No %s %s build available.",
                     LIU_UPDATE_OS, arch);
            goto done;
        }
        if (!updater_plat_os_at_least(min_os)) {
            snprintf(st->status, sizeof st->status, "Liu %s requires macOS %s.", sel_ver, min_os);
            goto done;
        }
        cJSON *url = cJSON_GetObjectItemCaseSensitive(match, "url");
        cJSON *sha = cJSON_GetObjectItemCaseSensitive(match, "sha256");
        cJSON *sig = cJSON_GetObjectItemCaseSensitive(match, "ed25519");
        cJSON *size = cJSON_GetObjectItemCaseSensitive(match, "size");
        if (!cJSON_IsString(url) || !cJSON_IsString(sha) || !cJSON_IsString(sig)) {
            snprintf(st->status, sizeof st->status, "Update feed entry is malformed."); goto done;
        }
        /* A valid Ed25519 signature is ~88 base64 chars and a sha256 hex is 64;
         * a feed field too long to hold is a *malformed feed*, not a key/hash
         * mismatch — say so distinctly so an operator debugging the feed isn't
         * sent chasing a phantom signing-key problem (the field would otherwise
         * be silently snprintf-truncated and surface as "verification failed"). */
        if (strlen(sig->valuestring) >= sizeof st->avail_sig_b64 ||
            strlen(sha->valuestring) >= sizeof st->avail_sha256 ||
            strlen(url->valuestring) >= sizeof st->avail_url) {
            snprintf(st->status, sizeof st->status, "Update feed entry is malformed."); goto done;
        }
        /* The download URL must be https on the feed's host: the artifact is
         * Ed25519-verified regardless, but nothing is fetched over another
         * transport or from another host. */
        if (strncmp(url->valuestring, "https://", 8) != 0 ||
            !url_host_matches_feed(url->valuestring)) {
            snprintf(st->status, sizeof st->status, "Update feed entry is malformed."); goto done;
        }

        snprintf(st->avail_version, sizeof st->avail_version, "%s", sel_ver);
        snprintf(st->avail_url, sizeof st->avail_url, "%s", url->valuestring);
        snprintf(st->avail_sha256, sizeof st->avail_sha256, "%s", sha->valuestring);
        snprintf(st->avail_sig_b64, sizeof st->avail_sig_b64, "%s", sig->valuestring);
        snprintf(st->avail_min_os, sizeof st->avail_min_os, "%s", min_os);
        /* notesURL is opened in the browser on a click — accept https only so a
         * tampered feed can't dispatch an arbitrary URL scheme via NSWorkspace. */
        st->avail_notes[0] = '\0';
        if (cJSON_IsString(notes) && strncmp(notes->valuestring, "https://", 8) == 0)
            snprintf(st->avail_notes, sizeof st->avail_notes, "%s", notes->valuestring);
        /* Clamp before the double->long long cast: a non-finite or out-of-range
         * hostile "size" makes the cast undefined. Non-positive / too-large /
         * NaN => 0 (unknown), so the 1 GiB hard cap applies downstream. */
        st->avail_size = 0;
        if (cJSON_IsNumber(size)) {
            double sz = size->valuedouble;
            if (isfinite(sz) && sz > 0 && sz < 1073741824.0)   /* < 1 GiB hard cap */
                st->avail_size = (long long)sz;
        }
        found = true;
    }
done:
    cJSON_Delete(root);
    return found;
}

/* ===================== verify + install (main thread) ===================== */

static void updater_verify_and_install(UpdateState *st) {
    /* Bound the on-disk artifact before slurping it into RAM — the streaming
     * download cap can be skipped if the whole body arrives in one delegate
     * callback, so re-check here (feed-declared size + slack, hard 1 GiB ceiling)
     * to avoid an OOM from a hostile feed/server. */
    {
        struct stat sb;
        /* Compute the cap in unsigned 64-bit math and clamp the feed-supplied
         * size to the hard ceiling *before* adding the slack — a hostile feed
         * could otherwise drive avail_size near LLONG_MAX and overflow the
         * signed add (UB / wrap-negative), defeating the bound entirely. */
        u64 hard = 1024ull * 1024 * 1024;
        u64 cap;
        if (st->avail_size > 0) {
            u64 sz = (u64)st->avail_size;
            if (sz > hard) sz = hard;            /* clamp first, no overflow */
            cap = sz + sz / 20 + 65536;          /* sz <= hard, can't overflow u64 */
            if (cap > hard) cap = hard;
        } else {
            cap = hard;
        }
        if (stat(st->downloaded_path, &sb) != 0 || (u64)sb.st_size > cap) {
            upd_fail(st, "Update is unexpectedly large \xe2\x80\x94 aborting."); goto cleanup;
        }
    }
    /* One fd for the whole of verification and the TOCTOU re-check, so every
     * read below is of the same file OBJECT rather than of whatever the path
     * happens to name at that moment. Nothing is buffered: the hash streams and
     * the signature is taken over a read-only mapping. */
    u64 art_len = 0;
    int art_fd = upd_open_artifact(st->downloaded_path, &art_len);
    if (art_fd < 0) { upd_fail(st, "Couldn't read the downloaded update."); goto cleanup; }

    char hex[65];
    u64 hashed = 0;
    if (!sha256_hex_fd(art_fd, hex, &hashed) || hashed != art_len ||
        strcasecmp(hex, st->avail_sha256) != 0) {
        close(art_fd); upd_fail(st, "Update integrity check failed."); goto cleanup;
    }
    unsigned char sig[80];
    size_t siglen = b64_decode(st->avail_sig_b64, sig, sizeof sig);
    if (siglen != 64 || !ed25519_verify_fd(art_fd, art_len, sig, siglen)) {
        close(art_fd);
        upd_fail(st, "Update signature verification failed \xe2\x80\x94 aborting."); goto cleanup;
    }

    /* Verified. Extract → re-check version → strip quarantine → install. */
    atomic_store(&st->phase, UPD_INSTALLING);
    snprintf(st->status, sizeof st->status, "Installing\xe2\x80\xa6");

    /* Close the TOCTOU window: everything above verified the file behind
     * `art_fd`, but the extractor re-opens the PATH. Two checks, because
     * neither alone is enough. Re-hash through the fd — proving that inode's
     * contents are still what was signed, which a re-open by path could not
     * (the path could name a different file by then). Then confirm the path
     * still resolves to that same inode, so what the extractor is about to
     * open IS the object just verified. */
    {
        char dhex[65];
        u64 dlen = 0;
        struct stat fdst, pathst;
        bool same = sha256_hex_fd(art_fd, dhex, &dlen) &&
                    dlen == art_len &&
                    strcasecmp(dhex, st->avail_sha256) == 0 &&
                    fstat(art_fd, &fdst) == 0 &&
                    stat(st->downloaded_path, &pathst) == 0 &&
                    fdst.st_dev == pathst.st_dev &&
                    fdst.st_ino == pathst.st_ino;
        close(art_fd);
        art_fd = -1;
        if (!same) {
            upd_fail(st, "Update integrity check failed \xe2\x80\x94 aborting."); goto cleanup;
        }
    }

    char app_path[1024];
    if (!updater_plat_extract(st->downloaded_path, app_path, sizeof app_path)) {
        upd_fail(st, "Couldn't extract the update."); goto cleanup;
    }
    /* Fail closed: require the extracted bundle to report a strictly-newer
     * version. An unreadable/missing version aborts rather than installing. */
    char bv[64];
    if (!updater_plat_bundle_version(app_path, bv, sizeof bv) ||
        !liu_update_is_newer(bv, updater_current_version())) {
        upd_fail(st, "The downloaded build couldn't be confirmed newer \xe2\x80\x94 aborting."); goto cleanup;
    }
    updater_plat_strip_quarantine(app_path);

    char installed[1024];
    if (!updater_plat_install(app_path, installed, sizeof installed)) {
        upd_fail(st, "Couldn't install the update (is Liu in a writable location?)."); goto cleanup;
    }
    /* Success — relaunch. The main loop sees relaunch_requested and quits. */
    atomic_store(&st->phase, UPD_RELAUNCHING);
    snprintf(st->status, sizeof st->status, "Restarting Liu\xe2\x80\xa6");
    st->relaunch_requested = true;
    updater_plat_relaunch_and_quit(installed);

cleanup:
    if (st->downloaded_path[0]) { updater_plat_cleanup_temp(st->downloaded_path); st->downloaded_path[0] = '\0'; }
}

/* ===================== control ===================== */

void updater_begin_check(UpdateState *st, bool silent) {
    if (!st || !st->initialized) return;
    int phase = atomic_load(&st->phase);
    if (phase == UPD_CHECKING || phase == UPD_DOWNLOADING ||
        phase == UPD_VERIFYING || phase == UPD_INSTALLING || phase == UPD_RELAUNCHING)
        return; /* already busy */
    st->silent = silent;
    st->err[0] = '\0';
    st->avail_version[0] = '\0';
    atomic_store(&st->io_done, false);
    atomic_store(&st->io_ok, false);
    st->io_err[0] = '\0';
    atomic_store(&st->phase, UPD_CHECKING);
    snprintf(st->status, sizeof st->status, "Checking for updates\xe2\x80\xa6");
    updater_plat_fetch_feed(st, LIU_UPDATE_FEED_URL);
}

void updater_begin_install(UpdateState *st) {
    if (!st || atomic_load(&st->phase) != UPD_AVAILABLE) return;
    if (!st->auto_install_allowed) return; /* UI should offer "Open Releases" instead */
    atomic_store(&st->io_done, false);
    atomic_store(&st->io_ok, false);
    st->io_err[0] = '\0';
    atomic_store(&st->bytes_done, 0);
    atomic_store(&st->bytes_total, st->avail_size);
    atomic_store(&st->phase, UPD_DOWNLOADING);
    snprintf(st->status, sizeof st->status, "Downloading Liu %s\xe2\x80\xa6", st->avail_version);
    updater_plat_download(st, st->avail_url, st->avail_size);
}

void updater_cancel(UpdateState *st) {
    if (!st) return;
    /* Cancel the in-flight task but DO NOT free feed_body / downloaded_path here:
     * a completion handler may still be executing on the session's background
     * queue and writing those fields. Reclamation is deferred to updater_tick,
     * which only touches them once io_done is set (i.e. after the completion has
     * finished). This avoids a data race / use-after-free on the non-atomic
     * pointer when a Cancel control is wired up. */
    updater_plat_cancel(st);
    st->silent = false;
    atomic_store(&st->phase, UPD_IDLE);
    st->status[0] = '\0';
}

void updater_shutdown(UpdateState *st) {
    if (!st || !st->initialized) return;

    updater_plat_cancel(st);

    /* The platform workers keep a raw pointer to this UpdateState. A normal
     * Cancel is deliberately asynchronous so the UI never stalls, but a
     * window destructor cannot return until the last worker has stopped
     * publishing through that pointer. curl is terminated above, so this is
     * normally only a few milliseconds. */
    while (atomic_load_explicit(&st->io_workers, memory_order_acquire) > 0) {
        struct timespec ts = { 0, 1000 * 1000 }; /* 1 ms */
        nanosleep(&ts, NULL);
    }

    free(st->feed_body);
    st->feed_body = NULL;
    atomic_store(&st->feed_len, 0);
    if (st->downloaded_path[0]) {
        updater_plat_cleanup_temp(st->downloaded_path);
        st->downloaded_path[0] = '\0';
    }
    st->initialized = false;
    atomic_store(&st->phase, UPD_IDLE);
}

/* Auto-check cadence. Every launch performs one silent check, and it is NOT
 * throttled by an on-disk timestamp: a once-a-day stamp would keep a release
 * cut minutes after the day's first launch invisible however often the app was
 * reopened. A feed fetch is one small HTTPS GET; per-launch is cheap.
 *
 * While the app stays open the check repeats on UPD_AUTOCHECK_INTERVAL_S so a
 * window left running for days still notices a release. A check that never
 * reached the server (offline at login, feed 404) retries on the shorter
 * UPD_AUTOCHECK_RETRY_S instead of waiting out the full interval. */
#define UPD_AUTOCHECK_INTERVAL_S  (6.0 * 3600.0)
#define UPD_AUTOCHECK_RETRY_S     (15.0 * 60.0)

/* Record the outcome of a completed check; drives the in-session re-check
 * timer above (process-local, so relaunching always re-checks immediately). */
static void note_check_done(UpdateState *st, bool ok) {
    st->last_check_epoch = (double)time(NULL);
    st->last_check_ok    = ok;
}

void updater_autocheck_on_launch(UpdateState *st) {
    if (!st || !st->initialized) return;
    updater_begin_check(st, true);
}

void updater_tick(UpdateState *st) {
    if (!st || !st->initialized) return;
    int phase = atomic_load(&st->phase);

    /* Reclaim buffers from a cancelled op once its async completion has landed
     * (io_done is set on the session queue AFTER the fields are written, so by
     * the time the main thread observes it the writes are visible). */
    if (phase == UPD_IDLE && atomic_load(&st->io_done)) {
        if (st->feed_body) { free(st->feed_body); st->feed_body = NULL; }
        if (st->downloaded_path[0]) {
            updater_plat_cleanup_temp(st->downloaded_path);
            st->downloaded_path[0] = '\0';
        }
        atomic_store(&st->io_done, false);
    }

    /* In-session re-check: the launch check only covers a freshly started app.
     * A window left open past the interval (or a launch check that failed
     * offline) checks again on its own. Skipped while a check/download is in
     * flight and while an update is already waiting to be installed, so it can
     * never clobber UPD_AVAILABLE out from under the toolbar chip. */
    if ((phase == UPD_IDLE || phase == UPD_UPTODATE) && st->last_check_epoch > 0.0) {
        double now = (double)time(NULL);
        double due = st->last_check_epoch +
                     (st->last_check_ok ? UPD_AUTOCHECK_INTERVAL_S : UPD_AUTOCHECK_RETRY_S);
        /* now < last_check_epoch: the wall clock stepped backwards — re-check
         * rather than sit out an arbitrarily long dead window. */
        if (now >= due || now < st->last_check_epoch) {
            updater_begin_check(st, true);
            return;
        }
    }

    if (phase == UPD_CHECKING) {
        if (!atomic_load(&st->io_done)) return;
        if (!atomic_load(&st->io_ok)) {
            note_check_done(st, false);
            if (!st->silent) upd_fail(st, st->io_err[0] ? st->io_err : "Couldn't reach the update server.");
            else {
                /* Silent: no toast, no error state — but the check is OVER, and
                 * the line About shows must stop saying it is under way, or a
                 * launch that could not reach the feed (offline, or the feed
                 * missing) would leave About reading "Checking for updates..."
                 * as a check that never finishes. */
                atomic_store(&st->phase, UPD_IDLE);
                snprintf(st->status, sizeof st->status, "Couldn't reach the update server.");
            }
            if (st->feed_body) { free(st->feed_body); st->feed_body = NULL; }
            return;
        }
        /* Reached the server and got a body — arm the in-session re-check timer
         * from here (success), not from the moment the check started, so a
         * transient failure retries on the short interval. Applies to manual
         * checks too: a manual check counts as a recent check just as an
         * autocheck does. */
        note_check_done(st, true);
        /* Clear any previous status before parsing the result. The check
         * then publishes its final status or reason, so a completed or
         * failed request does not leave the interface showing that it is
         * still checking. */
        st->status[0] = '\0';
        bool found = updater_parse_feed(st);
        if (st->feed_body) { free(st->feed_body); st->feed_body = NULL; }
        if (st->feed_rejected) {
            /* Treat a rejected feed as a failed check. Silent launch checks remain quiet, while
             * manual checks report the error. Retry on the short interval so a stale CDN
             * response does not suppress checks for the full interval. */
            note_check_done(st, false);
            if (st->silent) atomic_store(&st->phase, UPD_IDLE);
            else { char why[200]; snprintf(why, sizeof why, "%s", st->status); upd_fail(st, why); }
            return;
        }
        if (found) {
            atomic_store(&st->phase, UPD_AVAILABLE);
            snprintf(st->status, sizeof st->status, "Liu %s is available.", st->avail_version);
            if (st->silent) {
                snprintf(st->toast_msg, sizeof st->toast_msg,
                         "Liu %s available \xe2\x80\x94 Settings \xe2\x80\xba About", st->avail_version);
                st->toast_pending = true;
            }
        } else {
            atomic_store(&st->phase, st->silent ? UPD_IDLE : UPD_UPTODATE);
            if (!st->status[0]) snprintf(st->status, sizeof st->status, "You're on the latest version.");
        }
        return;
    }

    if (phase == UPD_DOWNLOADING) {
        if (!atomic_load(&st->io_done)) return;
        if (!atomic_load(&st->io_ok)) { upd_fail(st, st->io_err[0] ? st->io_err : "Download failed."); return; }
        atomic_store(&st->phase, UPD_VERIFYING);
        snprintf(st->status, sizeof st->status, "Verifying\xe2\x80\xa6");
        return; /* verify next frame so the UI paints "Verifying" */
    }

    if (phase == UPD_VERIFYING) {
        updater_verify_and_install(st);
        return;
    }
}

#ifdef UPDATER_SELFTEST
/* Verify a file's SHA-256 and base64 Ed25519 signature. Return bit 0 when the
 * hash matches and bit 1 when the signature matches. */
int updater_selftest_verify(const char *path, const char *sha_hex, const char *sig_b64);
int updater_selftest_verify(const char *path, const char *sha_hex, const char *sig_b64) {
    u64 len = 0;
    int fd = upd_open_artifact(path, &len);
    if (fd < 0) return -1;
    int result = 0;
    char hex[65];
    u64 hashed = 0;
    if (sha256_hex_fd(fd, hex, &hashed) && hashed == len &&
        strcasecmp(hex, sha_hex) == 0) result |= 1;
    unsigned char sig[80];
    size_t sl = b64_decode(sig_b64, sig, sizeof sig);
    if (sl == 64 && ed25519_verify_fd(fd, len, sig, sl)) result |= 2;
    close(fd);
    return result;
}
#endif
