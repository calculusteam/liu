/*
 * Liu — folder history (see folder_history.h).
 */
#include "core/folder_history.h"
#include "core/net.h"          /* net_home_dir */
#include "core/utf8.h"         /* grapheme walk + display columns */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FH_PATH_CAP     1024
#define FH_NAME_CAP      256   /* NAME_MAX + 1 — a slug IS one directory name */
#define FH_MAX_TOKENS    128   /* '-'-separated pieces of one slug            */
#define FH_STAT_BUDGET    64   /* filesystem probes per slug, hard cap        */

struct FolderHistory {
    i32    count;
    u32    off[FOLDER_HISTORY_MAX];   /* entry i starts at pool + off[i] */
    i64    ts[FOLDER_HISTORY_MAX];    /* unix seconds, last command run  */
    char  *pool;                      /* NUL-separated path arena        */
    u32    pool_cap;
    u32    pool_used;
};

/* Single-slot cache: the picker asks for one list at a time. */
static struct FolderHistory g_fh;

/* ---------------------------------------------------------------------------
 * Slug -> path
 * ------------------------------------------------------------------------- */

static bool fh_is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

typedef struct {
    const char *tok[FH_MAX_TOKENS];
    u32         tlen[FH_MAX_TOKENS];
    i32         ntok;
    char        cand[FH_PATH_CAP];
    char        first[FH_PATH_CAP];   /* first solution found */
    i32         solutions;            /* capped at 2 — only "is it ambiguous" matters */
    i32         budget;               /* stat() calls left */
} FhResolve;

/* Depth-first search over "was this '-' a separator or a literal dash?".
 *
 * `i` is the next token to append and `clen` the bytes of `cand` already built,
 * which is always a complete path prefix when the previous joiner was '/'. The
 * separator branch stat()s that prefix before recursing, and an interior
 * component that does not exist kills the whole subtree — which is what keeps a
 * choice-per-dash from being 2^n on real paths. The literal branch never needs
 * a probe, so the cost is one stat per separator actually tried.
 *
 * Separator-first, because that is the branch that makes progress; a name like
 * "my-project" falls into the literal branch only after "/…/my" fails to exist. */
static void fh_dfs(FhResolve *R, i32 i, u32 clen) {
    if (R->solutions >= 2 || R->budget <= 0) return;

    if (i >= R->ntok) {
        R->cand[clen] = '\0';
        if (clen == 0) return;                 /* an empty candidate is not a path */
        R->budget--;
        if (!fh_is_dir(R->cand)) return;
        if (R->solutions == 0)
            snprintf(R->first, sizeof R->first, "%s", R->cand);
        R->solutions++;
        return;
    }

    u32 need = clen + 1u + R->tlen[i];
    if (need + 1u > sizeof R->cand) return;    /* would not fit — abandon branch */

    /* A: this '-' was a path separator. */
    bool sep_ok;
    if (clen == 0) {
        sep_ok = true;                         /* the filesystem root itself */
    } else {
        R->cand[clen] = '\0';
        R->budget--;
        if (R->budget < 0) return;
        sep_ok = fh_is_dir(R->cand);
    }
    if (sep_ok) {
        R->cand[clen] = '/';
        memcpy(R->cand + clen + 1, R->tok[i], R->tlen[i]);
        fh_dfs(R, i + 1, need);
        if (R->solutions >= 2) return;
    }

    /* B: this '-' was part of the folder's own name. A leading dash cannot be:
     * every root Liu slugs is absolute, so token 0 is empty and the first
     * joiner is always the separator. */
    if (clen == 0) return;
    R->cand[clen] = '-';
    memcpy(R->cand + clen + 1, R->tok[i], R->tlen[i]);
    fh_dfs(R, i + 1, need);
}

/* Recover the absolute path `slug` was made from, writing it to `out`.
 * Sets *ambiguous when more than one existing directory slugs to it — the
 * caller must not cache a guess it cannot distinguish from a wrong one. */
static bool fh_resolve_slug(const char *slug, char *out, usize cap,
                            bool *ambiguous) {
    if (ambiguous) *ambiguous = false;
    if (!slug || !slug[0] || !out || cap == 0) return false;

    /* Every root Liu slugs is an absolute path, so the slug always begins with
     * the '-' that was its leading '/'. Anything else is a stray directory
     * somebody dropped in the store, and resolving it would run the search
     * against the PROCESS's cwd and hand back a RELATIVE path — which the
     * sidecar branch already refuses, and which would later be cd'd into
     * relative to whatever the shell's cwd happened to be. */
    if (slug[0] != '-') return false;

    static FhResolve R;          /* ~2.4 KB; scan is single-threaded */
    memset(&R, 0, sizeof R);
    R.budget = FH_STAT_BUDGET;

    /* Split on '-'. The leading '-' produces an empty token 0, which is the
     * anchor the separator branch turns back into the leading '/'. */
    const char *p = slug;
    for (;;) {
        const char *dash = strchr(p, '-');
        const char *end  = dash ? dash : p + strlen(p);
        if (R.ntok >= FH_MAX_TOKENS) return false;
        R.tok[R.ntok]  = p;
        R.tlen[R.ntok] = (u32)(end - p);
        R.ntok++;
        if (!dash) break;
        p = dash + 1;
    }
    if (R.ntok < 2) return false;                /* "-" alone, or no dash at all */

    /* Token 0 is emitted bare; every later token carries its joiner. */
    if (R.tlen[0] + 1u > sizeof R.cand) return false;
    memcpy(R.cand, R.tok[0], R.tlen[0]);
    fh_dfs(&R, 1, R.tlen[0]);

    if (R.solutions == 0) return false;
    if (R.first[0] != '/') return false;   /* belt: never hand back a relative path */
    /* A search that ran out of probes did not finish, so "only one solution
     * was found" says nothing about how many exist. Reported as ambiguous:
     * that is the answer that neither shows a possibly-wrong row nor freezes
     * it into the sidecar. */
    if (ambiguous) *ambiguous = (R.solutions > 1) || (R.budget <= 0);
    int w = snprintf(out, cap, "%s", R.first);
    return w > 0 && (usize)w < cap;
}

/* ---------------------------------------------------------------------------
 * Snapshot arena
 * ------------------------------------------------------------------------- */

static bool fh_push(const char *path, i64 ts) {
    if (g_fh.count >= FOLDER_HISTORY_MAX) return false;
    u32 need = (u32)strlen(path) + 1u;
    if (g_fh.pool_used + need > g_fh.pool_cap) {
        u32 cap = g_fh.pool_cap ? g_fh.pool_cap : 8192u;
        while (cap < g_fh.pool_used + need) {
            u32 next = cap * 2u;
            if (next < cap) return false;
            cap = next;
        }
        char *np = realloc(g_fh.pool, cap);
        if (!np) return false;
        g_fh.pool = np;
        g_fh.pool_cap = cap;
    }
    memcpy(g_fh.pool + g_fh.pool_used, path, need);
    g_fh.off[g_fh.count] = g_fh.pool_used;
    g_fh.ts[g_fh.count]  = ts;
    g_fh.pool_used += need;
    g_fh.count++;
    return true;
}

/* ---------------------------------------------------------------------------
 * Scan
 * ------------------------------------------------------------------------- */

typedef struct { i64 ts; char name[FH_NAME_CAP]; } FhCandidate;

static int fh_cmp_recent(const void *a, const void *b) {
    const FhCandidate *x = (const FhCandidate *)a, *y = (const FhCandidate *)b;
    if (x->ts > y->ts) return -1;
    if (x->ts < y->ts) return  1;
    return strcmp(x->name, y->name);   /* stable, and deterministic across runs */
}

/* Keep only the top K project candidates so collection memory and sorting work
 * remain bounded. */
#define FH_TOPK (FOLDER_HISTORY_MAX * 2)

typedef struct {
    FhCandidate e[FH_TOPK];
    u32         n;
} FhTopK;

/* Min-heap on "least recent first", using the same total order as the sort so
 * the two can never disagree about a tie. */
static bool fh_topk_worse(const FhCandidate *a, const FhCandidate *b) {
    return fh_cmp_recent(a, b) > 0;     /* a is older / sorts later than b */
}

static void fh_topk_push(FhTopK *h, const FhCandidate *c) {
    if (h->n < FH_TOPK) {
        u32 i = h->n++;
        h->e[i] = *c;
        /* Worst (oldest) at the root, so a new entry rises only while it is
         * WORSE than its parent. Comparing the other way round builds a heap
         * with the NEWEST at the root, and then the eviction below throws away
         * exactly the entries it is supposed to keep. */
        while (i > 0) {                            /* sift up */
            u32 parent = (i - 1) / 2;
            if (!fh_topk_worse(&h->e[i], &h->e[parent])) break;
            FhCandidate t = h->e[parent]; h->e[parent] = h->e[i]; h->e[i] = t;
            i = parent;
        }
        return;
    }
    /* Full: the root is the oldest kept. Anything not newer than it can never
     * appear in the result, so it is rejected in one comparison. */
    if (!fh_topk_worse(&h->e[0], c)) return;
    h->e[0] = *c;
    u32 i = 0;
    for (;;) {                                     /* sift down */
        u32 l = i * 2 + 1, r = l + 1, worst = i;
        if (l < h->n && fh_topk_worse(&h->e[l], &h->e[worst])) worst = l;
        if (r < h->n && fh_topk_worse(&h->e[r], &h->e[worst])) worst = r;
        if (worst == i) break;
        FhCandidate t = h->e[worst]; h->e[worst] = h->e[i]; h->e[i] = t;
        i = worst;
    }
}

/* The sidecar liu_project_data_dir() writes. Read here, and back-filled below
 * for directories that predate it so the unslug guess is made at most once. */
static bool fh_read_path_sidecar(const char *dir, char *out, usize cap) {
    char sc[FH_PATH_CAP];
    int w = snprintf(sc, sizeof sc, "%s/path", dir);
    if (w <= 0 || (usize)w >= sizeof sc) return false;
    FILE *f = fopen(sc, "rb");
    if (!f) return false;
    char buf[FH_PATH_CAP];
    usize n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    if (!buf[0] || buf[0] != '/') return false;
    int w2 = snprintf(out, cap, "%s", buf);
    return w2 > 0 && (usize)w2 < cap;
}

static void fh_write_path_sidecar(const char *dir, const char *path) {
    char sc[FH_PATH_CAP];
    int w = snprintf(sc, sizeof sc, "%s/path", dir);
    if (w <= 0 || (usize)w >= sizeof sc) return;
    FILE *f = fopen(sc, "wb");
    if (!f) return;
    fprintf(f, "%s\n", path);
    fclose(f);
}

const FolderHistory *folder_history_scan(const char *exclude) {
    g_fh.count = 0;
    g_fh.pool_used = 0;

    const char *home = net_home_dir();
    if (!home || !home[0]) return NULL;

    char root[FH_PATH_CAP];
    int w = snprintf(root, sizeof root, "%s/.config/Liu/projects", home);
    if (w <= 0 || (usize)w >= sizeof root) return NULL;

    DIR *d = opendir(root);
    if (!d) return NULL;

    /* Pass 1 — every sidecar directory and when it was last written to. Only
     * the name is kept; resolving a path is the expensive half and only the
     * rows that survive the sort are worth paying it for. */
    static FhTopK heap;          /* 512 entries; kept off the stack, not grown */
    heap.n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;          /* ".", "..", dotfiles */
        char sub[FH_PATH_CAP];
        int n = snprintf(sub, sizeof sub, "%s/%s", root, de->d_name);
        if (n <= 0 || (usize)n >= sizeof sub) continue;

        /* cmdhistory is the membership test AND the clock: it is created by
         * the first command run in that folder and rewritten by every one
         * since, so its mtime is exactly "when was I last working there".
         * Requiring it also keeps out sidecar dirs the other per-project
         * subsystems create (coord/, canvases/) for folders the user never
         * ran anything in — those are not folder history. */
        char hist[FH_PATH_CAP];
        int hn = snprintf(hist, sizeof hist, "%s/cmdhistory", sub);
        if (hn <= 0 || (usize)hn >= sizeof hist) continue;
        struct stat hs;
        if (stat(hist, &hs) != 0 || !S_ISREG(hs.st_mode) || hs.st_size <= 0)
            continue;
        i64 ts = (i64)hs.st_mtime;

        FhCandidate c;
        c.ts = ts;
        snprintf(c.name, sizeof c.name, "%s", de->d_name);
        fh_topk_push(&heap, &c);
    }
    closedir(d);

    if (heap.n == 0) return NULL;
    /* At most FH_TOPK entries, so this sort is bounded no matter how many
     * project directories exist. */
    FhCandidate *cand = heap.e;
    u32 ccount = heap.n;
    qsort(cand, ccount, sizeof *cand, fh_cmp_recent);

    /* Pass 2 — resolve newest first and stop once the picker is full, so a
     * machine with hundreds of stale project dirs pays for the rows it shows,
     * not for the ones it never draws. */
    for (u32 i = 0; i < ccount && g_fh.count < FOLDER_HISTORY_MAX; i++) {
        char sub[FH_PATH_CAP];
        int n = snprintf(sub, sizeof sub, "%s/%s", root, cand[i].name);
        if (n <= 0 || (usize)n >= sizeof sub) continue;

        char path[FH_PATH_CAP];
        bool from_sidecar = fh_read_path_sidecar(sub, path, sizeof path);
        if (!from_sidecar) {
            bool ambiguous = false;
            if (!fh_resolve_slug(cand[i].name, path, sizeof path, &ambiguous))
                continue;
            /* Two live directories slug to this name and nothing on disk says
             * which one was used. Guessing is the worst option available: the
             * row would carry a plausible path and silently cd the user into a
             * sibling folder. Drop it instead — the entry comes back, exact,
             * the next time a command runs there, because that write stamps
             * the folder's real path (liu_project_data_stamp). */
            if (ambiguous) continue;
        }

        /* A row you cannot cd into is worse than a missing one. */
        if (!fh_is_dir(path)) continue;

        if (exclude && exclude[0] && strcmp(exclude, path) == 0) continue;

        bool dup = false;
        for (i32 j = 0; j < g_fh.count && !dup; j++)
            if (strcmp(g_fh.pool + g_fh.off[j], path) == 0) dup = true;
        if (dup) continue;

        /* Only unambiguous resolutions reach here, so this cannot freeze a
         * wrong answer — and it spares the next scan the search. */
        if (!from_sidecar) fh_write_path_sidecar(sub, path);

        if (!fh_push(path, cand[i].ts)) break;
    }

    /* `cand` points into the static heap — nothing to free. */
    return g_fh.count > 0 ? &g_fh : NULL;
}

i32 folder_history_count(const FolderHistory *h) {
    return h ? h->count : 0;
}

const char *folder_history_path(const FolderHistory *h, i32 i) {
    if (!h || i < 0 || i >= h->count) return "";
    return h->pool + h->off[i];
}

i64 folder_history_used_at(const FolderHistory *h, i32 i) {
    if (!h || i < 0 || i >= h->count) return 0;
    return h->ts[i];
}

/* Columns of `s`, as the renderer will draw it. */
static i32 fh_cols(const char *s) {
    return (i32)utf8_display_cols((const u8 *)s, strlen(s));
}

void folder_history_label(const char *path, const char *home,
                          i32 max_cols, char *out, usize cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!path || !path[0] || max_cols < 1) return;

    /* 1. "~" for the home prefix. Only on a component boundary: "/Users/alice2"
     *    must not become "~2". */
    char abbrev[FH_PATH_CAP];
    usize hlen = home ? strlen(home) : 0;
    if (hlen > 1 && strncmp(path, home, hlen) == 0 &&
        (path[hlen] == '/' || path[hlen] == '\0')) {
        snprintf(abbrev, sizeof abbrev, "~%s", path + hlen);
    } else {
        snprintf(abbrev, sizeof abbrev, "%s", path);
    }

    /* Fits the COLUMN budget — but not necessarily the byte budget: combining
     * marks are zero columns and wide glyphs are four bytes, so a 46-column
     * path can be hundreds of bytes. snprintf would cut it mid-sequence and
     * the row would draw a replacement box, so an over-long one falls through
     * to the elision below, which cuts on a grapheme boundary. */
    if (fh_cols(abbrev) <= max_cols && strlen(abbrev) < cap) {
        snprintf(out, cap, "%s", abbrev);
        return;
    }

    /* 2. Drop whole components from the front while the remainder still needs
     *    to shrink. The leading "…" costs one column. */
    const char *tail = abbrev;
    for (;;) {
        const char *slash = strchr(tail + (tail[0] == '/' ? 1 : 0), '/');
        if (!slash) break;
        if (fh_cols(slash) + 1 <= max_cols) { tail = slash; break; }
        tail = slash;   /* still too wide — try the next boundary in */
    }

    /* 3. A single component that is still too wide is cut at a grapheme
     *    boundary — never mid-sequence, which would draw a replacement box. */
    if (fh_cols(tail) + 1 > max_cols) {
        const u8 *p = (const u8 *)tail;
        usize left = strlen(tail);
        i32 cols = fh_cols(tail);
        while (left > 0 && cols + 1 > max_cols) {
            i32 gcols = 0;
            u32 n = utf8_next_grapheme(p, left, NULL, &gcols);
            /* A byte the decoder cannot start a sequence at is skipped rather
             * than treated as the end of the string: a single stray byte in a
             * filename (paths are not required to be UTF-8) would otherwise
             * stop the trim and return a label many times its budget. It is
             * one column in fh_cols' accounting, so charge one. */
            if (n == 0) { p += 1; left -= 1; cols -= 1; continue; }
            if (n > left) break;
            p += n; left -= n; cols -= gcols;
        }
        tail = (const char *)p;
    }

    /* Last line of defence on the BYTE budget: a tail that fits the column
     * budget can still be longer than the caller's buffer (zero-column marks
     * cost bytes and no columns). Drop further graphemes off the FRONT — the
     * same direction the elision above works in — rather than letting snprintf
     * cut the end off mid-sequence. */
    if (cap < 8) { out[0] = '\0'; return; }
    {
        usize room = cap - 4;                       /* ellipsis (3) + NUL */
        usize left = strlen(tail);
        while (left > room) {
            u32 n = utf8_next_grapheme((const u8 *)tail, left, NULL, NULL);
            if (n == 0) n = 1;
            if (n > left) break;
            tail += n; left -= n;
        }
        snprintf(out, cap, "\xe2\x80\xa6%s", tail);   /* U+2026 ELLIPSIS */
    }
}

void folder_history_shutdown(void) {
    free(g_fh.pool);
    memset(&g_fh, 0, sizeof g_fh);
}
