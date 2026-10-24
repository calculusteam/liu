/*
 * Liu — per-folder command history (see cmd_history.h).
 */
#include "core/cmd_history.h"
#include "core/project_dir.h"   /* liu_project_data_dir — Liu's central per-project dir */
#include "core/cmd_suggest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>

/* Entry storage is a byte arena of NUL-terminated strings plus an offset
 * table, not a fixed-width row matrix: real command lines run a few dozen
 * bytes, so a MAX_ENTRIES x ENTRY_CAP matrix would be almost all padding.
 * The arena starts at 4 KiB and grows only to what the folder's history
 * actually needs; its ceiling is MAX_ENTRIES x ENTRY_CAP. */
#define CMD_HISTORY_POOL_MIN 4096u
#define CMD_HISTORY_POOL_MAX ((u32)CMD_HISTORY_MAX_ENTRIES * \
                              (u32)CMD_HISTORY_ENTRY_CAP)

struct CmdHistory {
    char    dir[1024];                          /* folder this snapshot is for   */
    CmdHistoryKey key;                          /* identity of the file read     */
    bool    valid;                              /* dir/key reflect a COMPLETE read */
    i32     count;
    u32     off[CMD_HISTORY_MAX_ENTRIES];       /* entry i starts at pool + off[i] */
    char   *pool;                               /* heap byte arena, NUL-separated  */
    u32     pool_cap;
    u32     pool_used;                          /* bump cursor into pool           */
    /* A load that failed part-way (open, read, allocation) publishes nothing.
     * While the file keeps the identity it failed on, the next attempt waits
     * out a doubling backoff, so a file that cannot be read costs one lstat
     * per query rather than a re-read of its tail per keystroke. A changed
     * identity or folder retries at once. */
    char    fail_dir[1024];
    CmdHistoryKey fail_key;
    f64     fail_until;                         /* monotonic seconds             */
    f64     fail_backoff;
};

#define CMD_HISTORY_RETRY_MIN 0.5
#define CMD_HISTORY_RETRY_MAX 8.0

/* Single-slot cache: autosuggest and the popup always query the focused
 * terminal's cwd, so one folder is in play at a time. */
static _Thread_local struct CmdHistory g_cache = {0};

static f64 cmd_history_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
}

static CmdHistoryKey cmd_history_key_of(const struct stat *st) {
    CmdHistoryKey k = {0};
    k.dev       = (u64)st->st_dev;
    k.ino       = (u64)st->st_ino;
    k.size      = (i64)st->st_size;
    k.mtime_sec = (i64)st->st_mtime;
    k.ctime_sec = (i64)st->st_ctime;
#if defined(__APPLE__)
    k.mtime_ns  = (i64)st->st_mtimespec.tv_nsec;
    k.ctime_ns  = (i64)st->st_ctimespec.tv_nsec;
#else
    k.mtime_ns  = (i64)st->st_mtim.tv_nsec;
    k.ctime_ns  = (i64)st->st_ctim.tv_nsec;
#endif
    return k;
}

bool cmd_history_key_equal(const CmdHistoryKey *a, const CmdHistoryKey *b) {
    return a->dev == b->dev && a->ino == b->ino && a->size == b->size &&
           a->mtime_sec == b->mtime_sec && a->mtime_ns == b->mtime_ns &&
           a->ctime_sec == b->ctime_sec && a->ctime_ns == b->ctime_ns;
}

static const char *cmd_history_at(const struct CmdHistory *h, i32 i) {
    return h->pool + h->off[i];
}

/* Slide the live entries to the front of the arena, reclaiming the bytes of
 * entries that have since been evicted. Entries are appended in bump order and
 * only ever dropped from the front, so off[] is ascending and the copy always
 * moves left. Like a pool grow, this invalidates every entry pointer handed out
 * earlier — both only run inside load/record, which the header already
 * documents as the invalidation points. */
static void cmd_history_compact(void) {
    u32 w = 0;
    for (i32 i = 0; i < g_cache.count; i++) {
        u32 o = g_cache.off[i];
        u32 n = (u32)strlen(g_cache.pool + o) + 1;
        if (w != o) memmove(g_cache.pool + w, g_cache.pool + o, (usize)n);
        g_cache.off[i] = w;
        w += n;
    }
    g_cache.pool_used = w;
}

/* Make room for one more `need`-byte entry: reclaim first, grow only if the
 * live entries genuinely need it. MAX_ENTRIES entries of at most ENTRY_CAP
 * bytes each always fit in POOL_MAX, so this can only fail on OOM. */
static bool cmd_history_reserve(u32 need) {
    if (g_cache.pool && g_cache.pool_used + need <= g_cache.pool_cap) return true;
    if (g_cache.pool) {
        cmd_history_compact();
        if (g_cache.pool_used + need <= g_cache.pool_cap) return true;
    }
    u32 want = g_cache.pool_used + need;
    u32 cap  = g_cache.pool_cap ? g_cache.pool_cap : CMD_HISTORY_POOL_MIN;
    while (cap < want && cap < CMD_HISTORY_POOL_MAX) {
        u32 next = cap * 2;
        cap = (next > CMD_HISTORY_POOL_MAX || next < cap) ? CMD_HISTORY_POOL_MAX
                                                          : next;
    }
    if (cap < want) return false;
    char *p = realloc(g_cache.pool, cap);
    if (!p) return false;
    g_cache.pool     = p;
    g_cache.pool_cap = cap;
    return true;
}

/* Append an already bounded entry, evicting the oldest at capacity. False
 * only on OOM, and then the snapshot no longer matches its file. */
static bool cmd_history_push(const char *line) {
    usize len = strlen(line);
    if (len >= CMD_HISTORY_ENTRY_CAP) len = CMD_HISTORY_ENTRY_CAP - 1;
    if (g_cache.count >= CMD_HISTORY_MAX_ENTRIES) {
        memmove(g_cache.off, g_cache.off + 1,
                (usize)(CMD_HISTORY_MAX_ENTRIES - 1) * sizeof g_cache.off[0]);
        g_cache.count = CMD_HISTORY_MAX_ENTRIES - 1;
    }
    if (!cmd_history_reserve((u32)len + 1)) return false;
    char *dst = g_cache.pool + g_cache.pool_used;
    memcpy(dst, line, len);
    dst[len] = '\0';
    g_cache.off[g_cache.count++] = g_cache.pool_used;
    g_cache.pool_used += (u32)len + 1;
    return true;
}

/* Central per-folder history file: ~/.config/Liu/projects/<slug>/cmdhistory.
 *
 * NOTHING is read from the folder itself. A folder is not the user's word: a
 * cloned repository that shipped a "<dir>/.liu/cmdhistory" would otherwise
 * choose the commands Liu offers at the prompt as the user's own history. */
static bool cmd_history_file_path(const char *dir, char *out, usize cap) {
    char data_dir[1024];
    if (!liu_project_data_dir(dir, data_dir, sizeof data_dir)) return false;
    int n = snprintf(out, cap, "%s/cmdhistory", data_dir);
    return n > 0 && (usize)n < cap;
}

/* The log is appended to for as long as a folder is worked in. Past this size
 * it is cut back to its newest half (whole records) — see history_trim. */
#define CMD_HISTORY_MAX_FILE_BYTES (4u * 1024u * 1024u)

/* Plain lines remain readable. Records containing control bytes use
 * RS + hex, so embedded newlines are never confused with record boundaries. */
static bool cmd_history_normalize(const char *command, char *out, usize cap) {
    if (!command || !command[0] || strlen(command) >= cap) return false;
    bool content = false;
    for (const unsigned char *p = (const unsigned char *)command; *p; p++)
        if (!isspace(*p)) content = true;
    if (!content) return false;
    memcpy(out, command, strlen(command) + 1);
    return true;
}

static bool history_decode(char *line) {
    /* US metadata header: version status duration timestamp TAB RS hex. */
    if ((u8)line[0] == 0x1f) {
        char *payload = strchr(line, '\t');
        if (!payload || (u8)payload[1] != 0x1e) return false;
        memmove(line, payload + 1, strlen(payload + 1) + 1);
    }
    usize n = strlen(line);
    while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
    if ((u8)line[0] != 0x1e) return n > 0 && n < CMD_HISTORY_ENTRY_CAP;
    if ((n - 1) % 2 || (n - 1) / 2 >= CMD_HISTORY_ENTRY_CAP) return false;
    usize w = 0;
    for (usize i = 1; i < n; i += 2) {
        unsigned value = 0;
        for (usize j = 0; j < 2; j++) {
            char c = line[i+j];
            if (c >= '0' && c <= '9') value = value * 16 + (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') value = value * 16 + (unsigned)(c - 'a' + 10);
            else return false;
        }
        if (!value) return false;
        line[w++] = (char)value;
    }
    line[w] = 0;
    return w > 0;
}

/* Cut a log that outgrew CMD_HISTORY_MAX_FILE_BYTES back to its newest half,
 * starting at a record boundary. In place and under the writer's lock (`fd`
 * is the O_APPEND descriptor that holds it): another writer waiting on the
 * lock then appends to the same file, not to one that was renamed away. True
 * when the file was rewritten. Without this bound the log would grow by one
 * record per command for as long as a folder is worked in. */
static bool history_trim(int fd, const char *path, const struct stat *st) {
    off_t from = st->st_size / 2;
    usize want = (usize)(st->st_size - from);
    int rd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (rd < 0) return false;
    struct stat rs;
    char *tail = NULL;
    bool ok = fstat(rd, &rs) == 0 && rs.st_dev == st->st_dev && rs.st_ino == st->st_ino &&
              (tail = malloc(want)) != NULL;
    usize got = 0;
    while (ok && got < want) {
        ssize_t r = pread(rd, tail + got, want - got, from + (off_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (usize)r;
    }
    close(rd);
    char *nl = ok ? memchr(tail, '\n', got) : NULL;      /* the first whole record */
    if (!nl) { free(tail); return false; }
    usize skip = (usize)(nl - tail) + 1, done = skip;
    if (ftruncate(fd, 0) != 0) { free(tail); return false; }
    while (done < got) {
        ssize_t w = write(fd, tail + done, got - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        done += (usize)w;
    }
    free(tail);
    return true;
}

static void history_record(const char *dir, const char *command,
                           bool completed, i32 exit_code, f64 duration) {
    if (!dir || !dir[0]) return;
    char line[CMD_HISTORY_ENTRY_CAP];
    if (!cmd_history_normalize(command, line, sizeof line)) return;
    char path[1100];
    if (!cmd_history_file_path(dir, path, sizeof path)) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || flock(fd, LOCK_EX)) {
        close(fd); return;
    }
    /* Another writer may have appended while this one waited for the
     * lock. */
    if (fstat(fd, &st)) { flock(fd, LOCK_UN); close(fd); return; }
    /* Refresh under the writer lock, before adopting a post-write key. */
    CmdHistoryKey previous = cmd_history_key_of(&st);
    (void)cmd_history_get(dir);
    char record[CMD_HISTORY_ENTRY_CAP * 2 + 128];
    usize n = 0;
    bool encoded = completed;
    if (completed) n = (usize)snprintf(record, sizeof record, "\0371 %d %.9g %lld\t",
                                     exit_code, duration, (long long)time(NULL));
    for (const unsigned char *p = (const unsigned char *)line; *p; p++)
        if (*p < 32 || *p == 127) encoded = true;
    if (encoded) {
        static const char hex[] = "0123456789abcdef";
        record[n++] = 0x1e;
        for (const unsigned char *p = (const unsigned char *)line; *p; p++) {
            record[n++] = hex[*p >> 4]; record[n++] = hex[*p & 15];
        }
    } else { n = strlen(line); memcpy(record, line, n); }
    record[n++] = '\n';
    usize done = 0;
    while (done < n) {
        ssize_t wrote = write(fd, record + done, n - done);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) break;
        done += (usize)wrote;
    }
    if (done != n) (void)ftruncate(fd, st.st_size);
    bool ok = done == n && fstat(fd, &st) == 0;
    bool trimmed = ok && (u64)st.st_size > CMD_HISTORY_MAX_FILE_BYTES && history_trim(fd, path, &st);
    flock(fd, LOCK_UN); close(fd);
    if (!ok) { g_cache.valid = false; return; }
    liu_project_data_stamp(dir);
    if (trimmed) { g_cache.valid = false; return; }      /* another file now: re-read it */
    /* Extend the snapshot in place only when it is exactly the pre-append
     * file; anything else (a failed load, a writer that skipped the lock)
     * leaves it for the next query to re-read under the new identity. */
    if (g_cache.valid && !strcmp(g_cache.dir, dir) &&
        cmd_history_key_equal(&g_cache.key, &previous)) {
        if (!cmd_history_push(line)) { g_cache.valid = false; return; }
        g_cache.key = cmd_history_key_of(&st);
        cmd_suggest_observe(dir, line, &previous, &g_cache.key);
    }
}

void cmd_history_record(const char *dir, const char *command) {
    history_record(dir, command, false, 0, 0);
}

void cmd_history_record_result(const char *dir, const char *command,
                               i32 exit_code, f64 duration) {
    history_record(dir, command, true, exit_code, duration);
}

/* (Re)load g_cache from the central history file if the dir or the file's
 * identity changed. */
static void cmd_history_load_fail(const char *dir, const CmdHistoryKey *key) {
    g_cache.valid     = false;
    g_cache.count     = 0;
    g_cache.pool_used = 0;
    bool again = !strcmp(g_cache.fail_dir, dir) &&
                 cmd_history_key_equal(&g_cache.fail_key, key);
    f64 backoff = again ? g_cache.fail_backoff * 2.0 : CMD_HISTORY_RETRY_MIN;
    if (backoff > CMD_HISTORY_RETRY_MAX) backoff = CMD_HISTORY_RETRY_MAX;
    snprintf(g_cache.fail_dir, sizeof g_cache.fail_dir, "%s", dir);
    g_cache.fail_key     = *key;
    g_cache.fail_backoff = backoff;
    g_cache.fail_until   = cmd_history_now() + backoff;
}

static void cmd_history_load(const char *dir) {
    char path[1100];
    if (!cmd_history_file_path(dir, path, sizeof path)) { g_cache.valid = false; return; }

    /* lstat'ed and required to be a plain file owned by this user. */
    struct stat st;
    bool exists = (lstat(path, &st) == 0);
    if (exists && (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
                   st.st_size < 0))
        exists = false;
    CmdHistoryKey key = {0};
    if (exists) key = cmd_history_key_of(&st);

    /* Freshness is the file's identity, not mtime seconds + size: a
     * same-length rewrite within one second, or a replace that restored the
     * old timestamp, keeps both and would serve the previous contents. */
    if (g_cache.valid && cmd_history_key_equal(&g_cache.key, &key) &&
        strcmp(g_cache.dir, dir) == 0) {
        return; /* still fresh */
    }
    /* The last attempt at exactly this file failed; wait out its backoff. */
    if (g_cache.fail_until > 0.0 && !strcmp(g_cache.fail_dir, dir) &&
        cmd_history_key_equal(&g_cache.fail_key, &key) &&
        cmd_history_now() < g_cache.fail_until) {
        g_cache.valid = false;
        return;
    }

    /* Reset the arena in place — the buffer is reused across folders, only
     * the bump cursor and the offset table are rewound. Nothing is marked
     * valid until the whole tail has been read. */
    snprintf(g_cache.dir, sizeof g_cache.dir, "%s", dir);
    g_cache.valid     = false;
    g_cache.count     = 0;
    g_cache.pool_used = 0;

    if (!exists) {                     /* no file -> valid but empty snapshot */
        g_cache.key        = key;
        g_cache.valid      = true;
        g_cache.fail_until = 0.0;
        return;
    }
    /* O_NOFOLLOW + a second look at the open fd: the lstat above and this
     * open are two syscalls, and the file can be swapped between them. */
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        /* Gone between lstat and open: an ordinary race, not a failure. */
        if (errno == ENOENT) g_cache.fail_until = 0.0;
        else cmd_history_load_fail(dir, &key);
        return;
    }
    struct stat fst;
    if (fstat(fd, &fst) != 0) { close(fd); cmd_history_load_fail(dir, &key); return; }
    if (!S_ISREG(fst.st_mode) || fst.st_uid != geteuid() || fst.st_size < 0) {
        /* Swapped for something that is refused: that answer is final for this
         * identity, so it is a fresh empty snapshot rather than a retry. */
        close(fd);
        g_cache.key        = key;
        g_cache.valid      = true;
        g_cache.fail_until = 0.0;
        return;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); cmd_history_load_fail(dir, &key); return; }

    /* Every valid record fits this bound. Read a bounded tail rather than
     * scanning an ever-growing log twice for a 512-entry snapshot. */
    const off_t tail = (off_t)(CMD_HISTORY_MAX_ENTRIES + 1) *
                       (CMD_HISTORY_ENTRY_CAP * 2 + 128);
    char line[CMD_HISTORY_ENTRY_CAP * 2 + 128];
    bool ok = true;
    if (fst.st_size > tail) {
        if (fseeko(f, fst.st_size - tail, SEEK_SET)) ok = false;
        else { int c; while ((c = fgetc(f)) != EOF && c != '\n') {} }
    }
    while (ok && fgets(line, sizeof line, f)) {
        usize n = strlen(line);
        if (!n || line[n-1] != '\n') {
            /* Refuse partial/oversized records, never replay a fragment. */
            int c; while ((c = fgetc(f)) != EOF && c != '\n') {}
            continue;
        }
        if (history_decode(line) && !cmd_history_push(line)) ok = false;
    }
    if (ferror(f)) ok = false;
    fclose(f);
    /* The key is the descriptor's, i.e. the file these rows came from. If it
     * changed after the open, the next query's lstat disagrees and reloads. */
    CmdHistoryKey read_key = cmd_history_key_of(&fst);
    if (!ok) { cmd_history_load_fail(dir, &read_key); return; }
    g_cache.key        = read_key;
    g_cache.valid      = true;
    g_cache.fail_until = 0.0;
}

const CmdHistory *cmd_history_get(const char *dir) {
    if (!dir || !dir[0]) return NULL;
    cmd_history_load(dir);
    if (!g_cache.valid || g_cache.count == 0) return NULL;
    return &g_cache;
}

i32 cmd_history_count(const CmdHistory *h) {
    return h ? h->count : 0;
}

const char *cmd_history_entry(const CmdHistory *h, i32 i) {
    if (!h || i < 0 || i >= h->count) return NULL;
    return cmd_history_at(h, i);
}

void cmd_history_cache_key(const CmdHistory *h, CmdHistoryKey *out) {
    if (!out) return;
    if (h) *out = h->key;
    else   memset(out, 0, sizeof *out);
}

const char *cmd_history_match(const CmdHistory *h, const char *prefix, i32 prefix_len) {
    if (!h || !prefix || prefix_len <= 0) return NULL;
    for (i32 i = h->count - 1; i >= 0; i--) {
        const char *e = cmd_history_at(h, i);
        bool control = false;
        for (const unsigned char *p = (const unsigned char *)e; *p; p++)
            if (*p < 32 || *p == 127) { control = true; break; }
        if (!control && strncmp(e, prefix, (usize)prefix_len) == 0 &&
            (i32)strlen(e) > prefix_len) {
            return e;
        }
    }
    return NULL;
}

void cmd_history_shutdown(void) {
    free(g_cache.pool);
    memset(&g_cache, 0, sizeof g_cache);
}
