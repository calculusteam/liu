/*
 * Liu — image-file links in terminal output. See img_link.h for why this is
 * separate from vt_parser.c's inline-thumbnail scanner.
 */
#include "terminal/img_link.h"
#include "core/utf8.h"
#include "platform/platform.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <sys/param.h>
#include <sys/mount.h>
#endif

/* Is `path` on a filesystem that answers from this machine?
 *
 * The badge's existence check is a stat() of a path that OUTPUT named, made
 * with no click and on the render pass. On a network mount that has stopped
 * answering, stat() does not return, so a log line that mentions a file would
 * stop the window painting; and an automount point turns "a program printed a
 * path" into a mount attempt. So a path is only looked at when the mount it
 * falls under is a local one. The mount table is read with MNT_NOWAIT, which
 * never waits on a filesystem, and kept for a few seconds.
 *
 * (A symbolic link in the user's own tree that leads onto a dead mount still
 * blocks; so does everything else the user does there.) */
bool img_link_path_is_probeable(const char *path) {
    if (!path || path[0] != '/') return false;
#if defined(__APPLE__)
    enum { MOUNTS_MAX = 64 };
    static struct statfs mounts[MOUNTS_MAX];
    static int count = -1;
    static f64 read_at = 0.0;
    f64 now = platform_time_sec();
    if (count < 0 || now - read_at > 5.0) {
        count = getfsstat(mounts, (int)sizeof mounts, MNT_NOWAIT);
        if (count < 0) count = 0;
        read_at = now;
    }
    const struct statfs *best = NULL;
    usize best_len = 0;
    for (int i = 0; i < count; i++) {
        const char *m = mounts[i].f_mntonname;
        usize ml = strlen(m);
        bool under = strncmp(path, m, ml) == 0 && (ml == 1 || path[ml] == '/' || path[ml] == '\0');
        if (under && ml >= best_len) { best = &mounts[i]; best_len = ml; }
    }
    if (!best) return false;
    if (!(best->f_flags & MNT_LOCAL)) return false;
    return strcmp(best->f_fstypename, "autofs") != 0 && strcmp(best->f_fstypename, "devfs") != 0;
#else
    return true;
#endif
}

bool img_link_has_image_ext(const char *s, usize len) {
    static const char *exts[] = { ".png", ".jpg", ".jpeg", ".gif",
                                  ".webp", ".bmp", NULL };
    for (i32 e = 0; exts[e]; e++) {
        usize el = strlen(exts[e]);
        if (len < el) continue;
        bool ok = true;
        for (usize i = 0; i < el; i++) {
            char c = s[len - el + i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != exts[e][i]) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

/* =========================================================================
 * Existence memo
 *
 * The badge is rebuilt whenever the row is marked dirty, and an agent
 * streaming into the alt screen dirties its live region on every frame. A
 * stat() per row per frame would put a syscall storm on the render path for a
 * question whose answer almost never changes, so answers are memoised in a
 * tiny open-addressed table with a TTL.
 *
 * Negative answers are cached too, and that is the case that actually matters:
 * the overwhelmingly common row is one that names no image at all, and a miss
 * has to be as cheap as a hit. The TTL is what lets a file the agent is about
 * to write show up shortly after it lands.
 * ========================================================================= */

#define IMG_MEMO_SLOTS 128
#define IMG_MEMO_TTL_SEC 2.0

typedef struct {
    u64  hash;
    f64  checked_at;
    bool exists;
    bool used;
} ImgMemoEntry;

static ImgMemoEntry g_memo[IMG_MEMO_SLOTS];

static u64 img_fnv1a(const char *s, usize n) {
    u64 h = 1469598103934665603ULL;
    for (usize i = 0; i < n; i++) { h ^= (u8)s[i]; h *= 1099511628211ULL; }
    return h;
}

void img_link_forget_all(void) {
    memset(g_memo, 0, sizeof(g_memo));
}

/* Is `path` a regular file of a sane size? Memoised. */
static bool img_path_is_file(const char *path) {
    u64 h = img_fnv1a(path, strlen(path));
    if (h == 0) h = 1;
    f64 now = platform_time_sec();
    usize idx = (usize)(h % IMG_MEMO_SLOTS);

    /* Linear probe over a short run; the table is a cache, so a full run just
     * evicts the oldest of the probed slots rather than growing. */
    usize victim = idx;
    f64   victim_age = -1.0;
    for (usize p = 0; p < 8; p++) {
        usize i = (idx + p) % IMG_MEMO_SLOTS;
        ImgMemoEntry *e = &g_memo[i];
        if (!e->used) { victim = i; victim_age = 1e18; break; }
        if (e->hash == h) {
            if (now - e->checked_at < IMG_MEMO_TTL_SEC) return e->exists;
            victim = i; victim_age = 1e18;   /* stale: refresh in place */
            break;
        }
        f64 age = now - e->checked_at;
        if (age > victim_age) { victim = i; victim_age = age; }
    }

    struct stat st;
    bool ok = img_link_path_is_probeable(path) &&
              (stat(path, &st) == 0) && S_ISREG(st.st_mode) &&
              st.st_size > 0 && st.st_size <= (off_t)(64 * 1024 * 1024);

    g_memo[victim].hash = h;
    g_memo[victim].checked_at = now;
    g_memo[victim].exists = ok;
    g_memo[victim].used = true;
    return ok;
}

/* =========================================================================
 * Token -> path
 *
 * ONE rule, two callers. The row scan reads the cells an agent drew; the text
 * scan reads a prompt liu itself typed. They differ in where the bytes come
 * from and in nothing else, so the strip / extension / path-like / resolve /
 * stat ladder lives here and both go through it, and the two cannot drift
 * apart.
 * ========================================================================= */

/* Trim the wrappers an agent (or a shell paste) puts around a path, in place on
 * the [*s0, *s1) range. Returns false when nothing is left. */
static bool img_tok_trim(const char *buf, i32 *s0, i32 *s1) {
    i32 a = *s0, b = *s1;
    /* A markdown inline image is ONE whitespace-delimited token —
     * `![shot](out/shot.png)` — so the wrapper strip below can never reach the
     * path inside it: `!` is not a wrapper, and stopping at the `[` would take
     * the alt text. Cut at the last `](` first; what follows is the target, and
     * the trailing `)` then falls to the strip. */
    for (i32 k = b - 2; k >= a; k--) {
        if (buf[k] == ']' && buf[k + 1] == '(') { a = k + 2; break; }
    }
    while (a < b && (buf[a]=='"'||buf[a]=='\''||buf[a]=='('||
                     buf[a]=='<'||buf[a]=='['||buf[a]=='`')) a++;
    while (b > a && (buf[b-1]=='"'||buf[b-1]=='\''||buf[b-1]==')'||
                     buf[b-1]=='>'||buf[b-1]==']'||buf[b-1]=='`'||
                     buf[b-1]==',')) b--;
    *s0 = a; *s1 = b;
    return b > a;
}

/* Does the trimmed token [s0, s1) name a local image file? Fills `out` with the
 * resolved absolute path. `cwd` resolves relative tokens; NULL means "give up
 * on relative paths". */
static bool img_tok_resolve(const char *buf, i32 s0, i32 s1, const char *cwd,
                            char *out, usize out_cap) {
    i32 tl = s1 - s0;
    if (tl <= 0 || tl >= IMG_LINK_MAX_PATH) return false;
    if (!img_link_has_image_ext(&buf[s0], (usize)tl)) return false;

    /* A path, not a bare filename: absolute, home-relative, or carrying a
     * separator. A column of bare `*.png` names from `ls` has none, and without
     * this every directory listing would sprout badges. */
    bool path_like = (buf[s0] == '~');
    for (i32 k = s0; k < s1 && !path_like; k++)
        if (buf[k] == '/') path_like = true;
    if (!path_like) return false;

    char tok[IMG_LINK_MAX_PATH];
    memcpy(tok, &buf[s0], (usize)tl);
    tok[tl] = '\0';

    char path[IMG_LINK_MAX_PATH];
    if (tok[0] == '/') {
        snprintf(path, sizeof(path), "%s", tok);
    } else if (tok[0] == '~' && tok[1] == '/') {
        const char *home = getenv("HOME");
        snprintf(path, sizeof(path), "%s/%s", home ? home : "", tok + 2);
    } else {
        if (!cwd || !cwd[0]) return false;
        snprintf(path, sizeof(path), "%s/%s", cwd, tok);
    }

    if (!img_path_is_file(path)) return false;
    snprintf(out, out_cap, "%s", path);
    return true;
}

/* =========================================================================
 * Text scan
 * ========================================================================= */

i32 img_link_scan_text(const char *text, const char *cwd,
                       char (*out)[IMG_LINK_MAX_PATH], i32 max) {
    if (!text || !out || max <= 0) return 0;

    i32 n = 0;
    const i32 end = (i32)strlen(text);
    /* Left to right, unlike the row scan: a prompt names its attachments in the
     * order the user added them, and that is the order the card shows. */
    for (i32 i = 0; i < end && n < max; ) {
        while (i < end && (unsigned char)text[i] <= ' ') i++;
        i32 a = i;
        while (i < end && (unsigned char)text[i] > ' ') i++;
        i32 b = i;
        if (b <= a) continue;
        if (!img_tok_trim(text, &a, &b)) continue;

        char path[IMG_LINK_MAX_PATH];
        if (!img_tok_resolve(text, a, b, cwd, path, sizeof path)) continue;

        bool dup = false;
        for (i32 k = 0; k < n && !dup; k++) dup = (strcmp(out[k], path) == 0);
        if (dup) continue;
        snprintf(out[n], IMG_LINK_MAX_PATH, "%s", path);
        n++;
    }
    return n;
}

/* =========================================================================
 * Row scan
 * ========================================================================= */

/* Can this row possibly name an image path? A read-only pass, no writes.
 *
 * This is the fast path: the row that reaches this function is overwhelmingly a
 * line of agent prose naming nothing, and the answer for it has to be cheaper
 * than the answer for a hit. A hit needs BOTH a '.' (every accepted extension
 * ends in one) and a '/' or '~' (the path_like rule below — a bare filename is
 * rejected on purpose). Either one missing settles it without materialising the
 * row, without touching col_map, and without walking a single token.
 *
 * The cells are already in cache — the glyph and decoration passes read the
 * same row earlier in the same frame — so what this saves is the ~1 KB of
 * writes into two stack arrays and the token walk over them, not the read.
 *
 * `content_end` falls out of the same pass, so the survivors do not pay for a
 * second one. Non-ASCII cannot be '.' or '/', and a cluster handle carries bit
 * 30 so it is numerically far above either — neither can be mistaken for a path
 * character. */
static bool img_row_could_match_ex(const Cell *cells, i32 len,
                                   i32 *out_content_end,
                                   bool *io_has_dot, bool *io_has_sep) {
    i32 content_end = -1;
    for (i32 c = 0; c < len; c++) {
        u32 cp = cells[c].codepoint;
        if (cp == 0 || cp == ' ') continue;
        content_end = c;
        if (cp == '.') *io_has_dot = true;
        else if (cp == '/' || cp == '~') *io_has_sep = true;
    }
    *out_content_end = content_end;
    return *io_has_dot && *io_has_sep;
}

static bool img_row_could_match(const Cell *cells, i32 len, i32 *out_content_end) {
    bool has_dot = false, has_sep = false;
    return img_row_could_match_ex(cells, len, out_content_end, &has_dot, &has_sep);
}

/* Materialize a visible row as ASCII, with a byte -> column map.
 *
 * Non-ASCII collapses to one '\x01' byte per cell: it can never appear in a
 * path, so it terminates a token the way a space would, and keeping it to a
 * single byte keeps the map one-to-one with columns. A cluster handle (bit 30)
 * is numerically far above 127 and lands here too, which is correct — a
 * grapheme cluster is not part of a filename that will be stat()ed. Returns
 * the number of bytes written. */
static i32 img_row_text(const Cell *cells, i32 len, char *buf, i32 buf_cap,
                        i32 *col_map) {
    i32 pos = 0;
    for (i32 c = 0; c < len && pos < buf_cap - 1; c++) {
        u32 cp = cells[c].codepoint;
        if (cp == 0) cp = ' ';
        col_map[pos] = c;
        buf[pos++] = (cp < 128) ? (char)cp : '\x01';
    }
    buf[pos] = '\0';
    return pos;
}

/* How many screen rows of one soft-wrapped line will be joined. A path is
 * capped at IMG_LINK_MAX_PATH (1024) and the narrowest pane anyone splits to is
 * ~40 columns, so 26 rows covers the longest path that can exist; the buffer
 * below stops the join before this in every realistic case anyway. The cap is
 * here so a pathological screen — every row wrapped, no path anywhere — cannot
 * turn one row's scan into a walk over the whole grid. */
#define IMG_LINK_MAX_WRAP_ROWS 26

bool img_link_scan_row(Terminal *t, i32 row, const char *cwd, TermImgLink *out) {
    if (!t || !out || !t->cells || t->cols <= 0) return false;
    if (row < 0 || row >= t->rows) return false;
    if (t->cols > 4096) return false;          /* map is stack-allocated */

    /* A soft wrap is the terminal running out of columns, not the program
     * ending a line, so a path broken by one is still ONE path — and a split
     * pane is where that stops being an edge case, because halving the width
     * is what makes a long path wrap at all.
     *
     * Only the row where the line ENDS scans. The rows above it are its
     * continuation, and letting each of them scan the joined text would draw
     * one badge per screen row of a single line. */
    if (terminal_row_wrapped_unclipped(t, row)) return false;

    i32 top = row;
    while (top > 0 && (row - top + 1) < IMG_LINK_MAX_WRAP_ROWS &&
           terminal_row_wrapped_unclipped(t, top - 1)) {
        top--;
    }

    i32 len = 0;
    const Cell *cells = terminal_visible_row(t, row, &len);
    if (!cells) return false;
    if (len > t->cols) len = t->cols;

    i32 content_end = -1;
#ifdef LIU_IMG_LINK_SWITCH
    /* Switch that turns the pre-filter off, compiled only under
     * LIU_IMG_LINK_SWITCH so a benchmark can run both arms alternated inside one
     * process, where they see the same contention, the same frequency state and
     * the same cache. Not defined in any normal build. */
    extern bool liu_img_prefilter_disabled;
    if (liu_img_prefilter_disabled) {
        content_end = len - 1;              /* no trim, no pre-filter */
    } else
#endif
    {
        /* The flags are OR'd across the run because the two halves of a path
         * can land on different rows — a wrap between "…/marka-tess" and
         * "era.png" leaves the last row with a '.' and no '/', which on its own
         * reads as prose. The last row still supplies content_end: that is
         * where the line ends on screen and therefore where the badge goes.
         *
         * For an unwrapped row (top == row, the overwhelming majority) this is
         * exactly the single-row pre-filter — one pass, no extra reads, and the
         * early return below. */
        bool has_dot = false, has_sep = false;
        bool ok = img_row_could_match_ex(cells, len, &content_end,
                                         &has_dot, &has_sep);
        for (i32 rr = top; rr < row && !ok; rr++) {
            i32 plen = 0;
            const Cell *pcells = terminal_visible_row(t, rr, &plen);
            if (!pcells) continue;
            if (plen > t->cols) plen = t->cols;
            i32 dummy = -1;
            ok = img_row_could_match_ex(pcells, plen, &dummy, &has_dot, &has_sep);
        }
        if (!ok) return false;
        /* terminal_visible_row hands back a SCRATCH buffer that the next
         * resolve invalidates, and the loop above resolved other rows through
         * it — so `cells` is stale now and has to be taken again. */
        cells = terminal_visible_row(t, row, &len);
        if (!cells) return false;
        if (len > t->cols) len = t->cols;
    }
    if (content_end < 4 && top == row) return false;  /* shorter than any path */

    /* Only the survivors materialise, and only up to the last non-blank cell:
     * a terminal row is mostly trailing blanks and copying them buys a longer
     * token walk over nothing.
     *
     * A wrapped run is joined with NO separator between rows — the wrap is not
     * a character, it is where the terminal ran out of columns, so the path
     * continues straight across it. The continuation rows are copied WHOLE
     * (not trimmed to their last non-blank cell) for the same reason: a row
     * that wrapped is full by definition, and trimming one that happens to end
     * in a space would close the gap the program actually wrote.
     *
     * `last_start` is where the final row's bytes begin. The output columns are
     * that row's, because that is the row the badge lands on — a byte before
     * this offset belongs to a row above and has no column the caller can use. */
    /* col_map MUST have one entry per byte buf can hold: img_row_text writes
     * col_map[pos] for every byte it emits and is bounded by buf_cap alone,
     * and joined rows can fill buf to its end. */
    char buf[4200];
    i32  col_map[sizeof(buf)];
    _Static_assert(sizeof(col_map) / sizeof(col_map[0]) >= sizeof(buf),
                   "col_map must cover every byte of buf");
    i32  n = 0;
    i32  last_start = 0;
    for (i32 rr = top; rr <= row; rr++) {
        i32 rlen = 0;
        const Cell *rcells = (rr == row) ? cells : terminal_visible_row(t, rr, &rlen);
        if (rr == row) rlen = len;
        if (!rcells) { if (rr == row) return false; continue; }
        if (rlen > t->cols) rlen = t->cols;
        i32 take = (rr == row) ? (content_end + 1) : rlen;
        if (take < 0) take = 0;
        if (rr == row) last_start = n;
        if (n >= (i32)sizeof(buf) - 1) break;
        n += img_row_text(rcells, take, buf + n, (i32)sizeof(buf) - n,
                          col_map + n);
    }
    if (n < 5) return false;

    /* Walk tokens right to left. Right-to-left favours the trailing
     * "… saved to <path>" shape, and matches vt_parser.c's scanner so the two
     * pick the same token out of a line that reaches both. */
    i32 i = n;
    while (i > 0) {
        i32 b = i;
        while (b > 0 && (buf[b-1] == ' ' || buf[b-1] == '\t')) b--;
        i32 a = b;
        while (a > 0 && buf[a-1] != ' ' && buf[a-1] != '\t') a--;
        i = a;
        if (b <= a) continue;

        i32 s0 = a, s1 = b;
        if (!img_tok_trim(buf, &s0, &s1)) continue;

        const char *base = (cwd && cwd[0]) ? cwd
                         : ((t->cwd && t->cwd[0]) ? t->cwd : NULL);
        char path[IMG_LINK_MAX_PATH];
        if (!img_tok_resolve(buf, s0, s1, base, path, sizeof path)) continue;

        /* Columns are the LAST row's. A token that began on a row above has
         * no start column here, and `end_col` only exists to stop the caller
         * putting the badge before the token ended — a token that finished on
         * an earlier row cannot constrain it, so -1 disables that floor. */
        out->start_col       = (s0 >= last_start) ? col_map[s0] : 0;
        out->end_col         = (s1 - 1 >= last_start) ? col_map[s1 - 1] : -1;
        out->content_end_col = content_end;
        snprintf(out->path, sizeof(out->path), "%s", path);
        return true;
    }
    return false;
}
