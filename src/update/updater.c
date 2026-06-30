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
#include <sys/stat.h>

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
    if (ap && bp) {
        int c = strcmp(apre, bpre);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
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

static bool sha256_hex(const unsigned char *data, size_t len, char out_hex[65]) {
    unsigned char md[32];
    unsigned int mdlen = 0;
    if (EVP_Digest(data, len, md, &mdlen, EVP_sha256(), NULL) != 1 || mdlen != 32)
        return false;
    for (int i = 0; i < 32; i++) snprintf(out_hex + i * 2, 3, "%02x", md[i]);
    out_hex[64] = '\0';
    return true;
}

static bool ed25519_verify(const unsigned char *data, size_t len,
                           const unsigned char *sig, size_t sig_len) {
    if (sig_len != 64) return false;
    EVP_PKEY *pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL,
                       LIU_UPDATE_PUBKEY, LIU_UPDATE_PUBKEY_LEN);
    if (!pk) return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pk) == 1)
        ok = (EVP_DigestVerify(ctx, sig, sig_len, data, len) == 1);
    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pk);
    return ok;
}

static unsigned char *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return NULL; }
    *out_len = (size_t)sz;
    return buf;
}

/* ===================== state helpers ===================== */

static void upd_fail(UpdateState *st, const char *msg) {
    snprintf(st->err, sizeof st->err, "%s", msg);
    atomic_store(&st->phase, UPD_ERROR);
}

void updater_init(UpdateState *st) {
    if (!st) return;
    memset(st, 0, sizeof *st);
    atomic_store(&st->phase, UPD_IDLE);
    st->auto_install_allowed = updater_plat_can_autoinstall();
    st->initialized = true;
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

/* Returns true and fills avail_* when a compatible, newer, non-yanked version
 * exists. Sets st->status with a reason otherwise (e.g. min-os, malformed). */
static bool updater_parse_feed(UpdateState *st) {
    cJSON *root = cJSON_Parse(st->feed_body);
    if (!root) { snprintf(st->status, sizeof st->status, "Update feed is unreadable."); return false; }

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
            if (cJSON_IsString(aa) && strcmp(aa->valuestring, arch) == 0) { match = a; break; }
        }
        if (!match) { snprintf(st->status, sizeof st->status, "No %s build available.", arch); goto done; }
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
        /* The download URL must be https — the feed is TLS-transported but not
         * signature-pinned, so a tampered feed must not point us at http or a
         * custom scheme (the artifact is Ed25519-verified regardless, but we
         * never even fetch over an untrusted transport). */
        if (strncmp(url->valuestring, "https://", 8) != 0) {
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
        st->avail_size = cJSON_IsNumber(size) ? (long long)size->valuedouble : 0;
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
    size_t len = 0;
    unsigned char *buf = read_whole_file(st->downloaded_path, &len);
    if (!buf) { upd_fail(st, "Couldn't read the downloaded update."); goto cleanup; }

    char hex[65];
    if (!sha256_hex(buf, len, hex) || strcasecmp(hex, st->avail_sha256) != 0) {
        free(buf); upd_fail(st, "Update integrity check failed."); goto cleanup;
    }
    unsigned char sig[80];
    size_t siglen = b64_decode(st->avail_sig_b64, sig, sizeof sig);
    if (siglen != 64 || !ed25519_verify(buf, len, sig, siglen)) {
        free(buf); upd_fail(st, "Update signature verification failed \xe2\x80\x94 aborting."); goto cleanup;
    }
    free(buf);

    /* Verified. Extract → re-check version → strip quarantine → install. */
    atomic_store(&st->phase, UPD_INSTALLING);
    snprintf(st->status, sizeof st->status, "Installing\xe2\x80\xa6");

    /* Close the TOCTOU window: we verified bytes read into RAM above, but ditto
     * re-reads the file from disk. Re-hash the on-disk file immediately before
     * extraction so a concurrent writer can't swap in an unverified payload
     * between verification and install — the installed artifact is exactly the
     * one whose hash+signature we just checked. */
    {
        size_t dlen = 0;
        unsigned char *dbuf = read_whole_file(st->downloaded_path, &dlen);
        char dhex[65];
        bool same = dbuf && sha256_hex(dbuf, dlen, dhex) &&
                    strcasecmp(dhex, st->avail_sha256) == 0;
        if (dbuf) free(dbuf);
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

/* ~/.config/Liu/update_check — last silent-check epoch (plain text). */
static void throttle_path(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !*home) { out[0] = '\0'; return; }
    snprintf(out, cap, "%s/.config/Liu/update_check", home);
}

/* Stamp the throttle file with the current epoch, creating ~/.config/Liu first
 * so the 24h gate actually engages on a fresh profile. Called only AFTER a
 * check successfully reaches the server (see updater_tick) — stamping before
 * the check (the old behavior) let a failed launch-time check, e.g. offline or
 * a feed 404, suppress the next check for a full 24h. */
static void stamp_autocheck(void) {
    char path[1024];
    throttle_path(path, sizeof path);
    if (!path[0]) return;
    FILE *w = fopen(path, "w");
    if (!w) {
        const char *home = getenv("HOME");
        if (home && *home) {
            char dir[1024];
            snprintf(dir, sizeof dir, "%s/.config", home);       mkdir(dir, 0755);
            snprintf(dir, sizeof dir, "%s/.config/Liu", home);   mkdir(dir, 0755);
        }
        w = fopen(path, "w");
    }
    if (w) { fprintf(w, "%lld\n", (long long)time(NULL)); fclose(w); }
}

void updater_maybe_autocheck(UpdateState *st) {
    if (!st || !st->initialized) return;
    char path[1024];
    throttle_path(path, sizeof path);
    if (path[0]) {
        FILE *f = fopen(path, "r");
        if (f) {
            long long last = 0;
            if (fscanf(f, "%lld", &last) == 1) {
                if ((long long)time(NULL) - last < 86400) { fclose(f); return; }
            }
            fclose(f);
        }
    }
    updater_begin_check(st, true);
}

void updater_tick(UpdateState *st) {
    if (!st || !st->initialized) return;
    int phase = atomic_load(&st->phase);

    /* Reclaim buffers from a cancelled op once its async completion has landed
     * (io_done is set on the session queue AFTER the fields are written, so by
     * the time we observe it on the main thread the writes are visible). */
    if (phase == UPD_IDLE && atomic_load(&st->io_done)) {
        if (st->feed_body) { free(st->feed_body); st->feed_body = NULL; }
        if (st->downloaded_path[0]) {
            updater_plat_cleanup_temp(st->downloaded_path);
            st->downloaded_path[0] = '\0';
        }
        atomic_store(&st->io_done, false);
    }

    if (phase == UPD_CHECKING) {
        if (!atomic_load(&st->io_done)) return;
        if (!atomic_load(&st->io_ok)) {
            if (!st->silent) upd_fail(st, st->io_err[0] ? st->io_err : "Couldn't reach the update server.");
            else atomic_store(&st->phase, UPD_IDLE); /* silent: stay quiet on network error */
            if (st->feed_body) { free(st->feed_body); st->feed_body = NULL; }
            return;
        }
        /* Reached the server and got a body — reset the 24h autocheck gate now
         * (success), not before the check, so transient failures don't blind the
         * next launch for a day. Applies to manual checks too: a manual check is
         * just as good as an autocheck for "we looked recently". */
        stamp_autocheck();
        bool found = updater_parse_feed(st);
        if (st->feed_body) { free(st->feed_body); st->feed_body = NULL; }
        if (found) {
            atomic_store(&st->phase, UPD_AVAILABLE);
            snprintf(st->status, sizeof st->status, "Liu %s is available.", st->avail_version);
            if (st->silent) {
                snprintf(st->toast_msg, sizeof st->toast_msg,
                         "Liu %s available \xe2\x80\x94 Settings \xe2\x80\xba About", st->avail_version);
                st->toast_pending = true;
            }
        } else {
            if (st->silent) atomic_store(&st->phase, UPD_IDLE);
            else {
                atomic_store(&st->phase, UPD_UPTODATE);
                if (!st->status[0]) snprintf(st->status, sizeof st->status, "You're on the latest version.");
            }
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
/* Test hook: verify a file against an expected sha256 hex + base64 Ed25519 sig,
 * exercising the real static crypto path. Returns bit0=sha-ok, bit1=sig-ok. */
int updater_selftest_verify(const char *path, const char *sha_hex, const char *sig_b64);
int updater_selftest_verify(const char *path, const char *sha_hex, const char *sig_b64) {
    size_t len = 0;
    unsigned char *buf = read_whole_file(path, &len);
    if (!buf) return -1;
    int result = 0;
    char hex[65];
    if (sha256_hex(buf, len, hex) && strcasecmp(hex, sha_hex) == 0) result |= 1;
    unsigned char sig[80];
    size_t sl = b64_decode(sig_b64, sig, sizeof sig);
    if (sl == 64 && ed25519_verify(buf, len, sig, sl)) result |= 2;
    free(buf);
    return result;
}
#endif
