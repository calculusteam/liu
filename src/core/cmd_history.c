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
#include <sys/stat.h>

struct CmdHistory {
    char    dir[1024];                          /* folder this snapshot is for   */
    time_t  mtime;                              /* backing file mtime when loaded */
    off_t   size;                               /* backing file size when loaded  */
    bool    valid;                              /* dir/mtime/size reflect a read  */
    i32     count;
    char  (*entries)[CMD_HISTORY_ENTRY_CAP];    /* heap, CMD_HISTORY_MAX_ENTRIES rows */
};

/* Single-slot cache: autosuggest and the popup always query the focused
 * terminal's cwd, so one folder is in play at a time. */
static struct CmdHistory g_cache = {0};

/* Central per-folder history file: ~/.config/Liu/projects/<slug>/cmdhistory.
 * Nothing is written into the user's folders anymore (no .liu/, no
 * .gitignore) — the old in-repo path survives below only as a read-only
 * fallback so pre-existing histories keep working. */
static bool cmd_history_file_path(const char *dir, char *out, usize cap) {
    char data_dir[1024];
    if (!liu_project_data_dir(dir, data_dir, sizeof data_dir)) return false;
    int n = snprintf(out, cap, "%s/cmdhistory", data_dir);
    return n > 0 && (usize)n < cap;
}

static void cmd_history_legacy_path(const char *dir, char *out, usize cap) {
    snprintf(out, cap, "%s/.liu/cmdhistory", dir);
}

/* Trim leading whitespace and stop at the first newline so a command is always
 * stored as a single line. Returns false when nothing usable remains. */
static bool cmd_history_normalize(const char *command, char *out, usize cap) {
    if (!command || cap == 0) return false;
    while (*command == ' ' || *command == '\t') command++;
    usize n = 0;
    for (; command[n] && command[n] != '\n' && command[n] != '\r' && n + 1 < cap; n++)
        out[n] = command[n];
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) n--;
    out[n] = '\0';
    return n > 0;
}

void cmd_history_record(const char *dir, const char *command) {
    if (!dir || !dir[0] || !command || !command[0]) return;

    char line[CMD_HISTORY_ENTRY_CAP];
    if (!cmd_history_normalize(command, line, sizeof line)) return;

    /* Collapse consecutive duplicates (a re-run command, `ls;ls`, ...). */
    const CmdHistory *cur = cmd_history_get(dir);
    if (cur && cur->count > 0 &&
        strcmp(cur->entries[cur->count - 1], line) == 0) {
        return;
    }

    char path[1100];
    if (!cmd_history_file_path(dir, path, sizeof path)) return;
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);

    /* Keep the in-memory cache consistent with the file we just appended to,
     * so the next cmd_history_get() (e.g. the very next keystroke's autosuggest
     * refresh) is a cache hit instead of re-reading + re-parsing the whole file
     * on every recorded command. cmd_history_get() above already loaded this
     * dir's snapshot, so the cache reflects it here. */
    if (g_cache.valid && g_cache.entries && strcmp(g_cache.dir, dir) == 0) {
        if (g_cache.count >= CMD_HISTORY_MAX_ENTRIES) {
            memmove(g_cache.entries[0], g_cache.entries[1],
                    (usize)(CMD_HISTORY_MAX_ENTRIES - 1) * CMD_HISTORY_ENTRY_CAP);
            g_cache.count = CMD_HISTORY_MAX_ENTRIES - 1;
        }
        snprintf(g_cache.entries[g_cache.count++], CMD_HISTORY_ENTRY_CAP, "%s", line);
        struct stat st;
        if (stat(path, &st) == 0) {
            g_cache.mtime = st.st_mtime;
            g_cache.size  = st.st_size;
        }
        /* Feed the learned-suggestion model with the SAME freshness key the
         * cache just adopted, so the next keystroke's lookup stays a cache
         * hit in both layers (no rebuild per executed command). */
        cmd_suggest_observe(dir, line, (i64)g_cache.mtime, (i64)g_cache.size);
    }
}

/* (Re)load g_cache from the central history file if the dir or mtime
 * changed. Pre-migration folders may still carry "<dir>/.liu/cmdhistory" —
 * read that when the central file doesn't exist yet (never write it). */
static void cmd_history_load(const char *dir) {
    char path[1100];
    if (!cmd_history_file_path(dir, path, sizeof path)) return;

    struct stat st;
    bool exists = (stat(path, &st) == 0);
    if (!exists) {
        char legacy[1100];
        cmd_history_legacy_path(dir, legacy, sizeof legacy);
        if (stat(legacy, &st) == 0) {
            snprintf(path, sizeof path, "%s", legacy);
            exists = true;
        }
    }
    time_t mtime = exists ? st.st_mtime : 0;
    off_t  size  = exists ? st.st_size  : 0;

    /* Freshness keys on size as well as mtime: st_mtime is only
     * second-granularity, so several commands recorded in the same second
     * would otherwise share an mtime and the cache would go stale. Every
     * append grows the file, so size reliably signals a change. */
    if (g_cache.valid && g_cache.mtime == mtime && g_cache.size == size &&
        strcmp(g_cache.dir, dir) == 0) {
        return; /* still fresh */
    }

    if (!g_cache.entries) {
        g_cache.entries = calloc(CMD_HISTORY_MAX_ENTRIES, CMD_HISTORY_ENTRY_CAP);
        if (!g_cache.entries) return;
    }
    snprintf(g_cache.dir, sizeof g_cache.dir, "%s", dir);
    g_cache.mtime = mtime;
    g_cache.size  = size;
    g_cache.count = 0;
    g_cache.valid = true;

    FILE *f = fopen(path, "r");
    if (!f) return; /* no file -> valid but empty snapshot */

    char line[CMD_HISTORY_ENTRY_CAP * 2];
    char prev[CMD_HISTORY_ENTRY_CAP];

    /* Pass 1: count the lines kept after collapsing consecutive duplicates, so
     * pass 2 can skip straight to the last MAX_ENTRIES. The old single-pass
     * window-slide did an O(MAX) memmove per excess line — O(n*MAX) for a long
     * file (gigabytes of memcpy on a multi-thousand-line history); two linear
     * passes are O(n) with no shuffling. */
    i32 total = 0;
    bool have_prev = false;
    while (fgets(line, sizeof line, f)) {
        usize len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;
        if (have_prev && strcmp(prev, line) == 0) continue;
        snprintf(prev, sizeof prev, "%s", line);
        have_prev = true;
        total++;
    }
    i32 skip = total > CMD_HISTORY_MAX_ENTRIES ? total - CMD_HISTORY_MAX_ENTRIES : 0;

    /* Pass 2: store only the last MAX_ENTRIES kept lines, oldest-first. */
    rewind(f);
    i32 seen = 0;
    have_prev = false;
    while (fgets(line, sizeof line, f)) {
        usize len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;
        if (have_prev && strcmp(prev, line) == 0) continue;
        snprintf(prev, sizeof prev, "%s", line);
        have_prev = true;
        if (seen++ < skip) continue;
        snprintf(g_cache.entries[g_cache.count++], CMD_HISTORY_ENTRY_CAP, "%s", line);
    }
    fclose(f);
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
    return h->entries[i];
}

void cmd_history_cache_key(const CmdHistory *h, i64 *mtime, i64 *size) {
    if (mtime) *mtime = h ? (i64)h->mtime : 0;
    if (size)  *size  = h ? (i64)h->size  : 0;
}

const char *cmd_history_match(const CmdHistory *h, const char *prefix, i32 prefix_len) {
    if (!h || !prefix || prefix_len <= 0) return NULL;
    for (i32 i = h->count - 1; i >= 0; i--) {
        const char *e = h->entries[i];
        if (strncmp(e, prefix, (usize)prefix_len) == 0 &&
            (i32)strlen(e) > prefix_len) {
            return e;
        }
    }
    return NULL;
}

void cmd_history_shutdown(void) {
    free(g_cache.entries);
    memset(&g_cache, 0, sizeof g_cache);
}
