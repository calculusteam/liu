/*
 * sftp_view.h — what one pane of the SFTP tab lists, and how its columns read.
 *
 * A pane shows a FileBrowser's listing through a VIEW: the indices of the
 * entries that pass the filter and the hidden-file switch, in the order the
 * column header asks for. The listing itself is never reordered (its indices
 * are what the selection bitmap names), so the view is rebuilt whenever the
 * listing, the filter or the order changes and nothing else moves.
 *
 * Also here, because they are pure and the painter needs them: the
 * permission string, the date and size cells, a file's kind when no language
 * claims it, the breadcrumb split, and the pane's back/forward list.
 *
 * Pure: no renderer, no AppState, no FileBrowser.
 */
#ifndef LIU_UI_SFTP_VIEW_H
#define LIU_UI_SFTP_VIEW_H

#include "core/types.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

typedef enum {
    SFTP_COL_NAME = 0,
    SFTP_COL_DATE,
    SFTP_COL_SIZE,
    SFTP_COL_KIND,
    SFTP_COL_PERMS,
    SFTP_COL_COUNT
} SftpCol;

/* What the view needs of one listed entry. */
typedef struct {
    const char *name;
    u64         size;
    u64         mtime;
    u32         perms;
    bool        is_dir;
    bool        stat_known;   /* size / mtime / perms are filled (a local listing stats lazily) */
} SftpRowKey;

/* ---- the filter -------------------------------------------------------- */

/* Does `name` contain `filter`, ignoring ASCII case? An empty filter matches
 * everything. Bytes past ASCII compare as they are, so a filter typed in any
 * script finds the same spelling in a name. */
static inline bool sftp_name_matches(const char *name, const char *filter) {
    if (!filter || !filter[0]) return true;
    if (!name) return false;
    usize nl = strlen(name), fl = strlen(filter);
    if (fl > nl) return false;
    for (usize i = 0; i + fl <= nl; i++) {
        usize k = 0;
        while (k < fl) {
            unsigned char a = (unsigned char)name[i + k], b = (unsigned char)filter[k];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
            if (a != b) break;
            k++;
        }
        if (k == fl) return true;
    }
    return false;
}

/* ---- the order --------------------------------------------------------- */

/* -1 / 0 / +1 for rows a and b under `col`, folders first whatever the
 * direction (a folder is where the user goes next, so it stays on top), the
 * name breaking every tie, then the listing's own position so two sorts of
 * the same listing come out the same. */
static inline i32 sftp_row_cmp(const SftpRowKey *rows, i32 a, i32 b, SftpCol col, bool desc) {
    const SftpRowKey *x = &rows[a], *y = &rows[b];
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    i32 c = 0;
    switch (col) {
    case SFTP_COL_SIZE:
        if (!x->is_dir) c = x->size < y->size ? -1 : x->size > y->size ? 1 : 0;
        break;
    case SFTP_COL_DATE:
        c = x->mtime < y->mtime ? -1 : x->mtime > y->mtime ? 1 : 0;
        break;
    case SFTP_COL_PERMS:
        c = (x->perms & 07777u) < (y->perms & 07777u) ? -1 : (x->perms & 07777u) > (y->perms & 07777u) ? 1 : 0;
        break;
    case SFTP_COL_KIND: {
        const char *ex = x->is_dir ? "" : strrchr(x->name ? x->name : "", '.');
        const char *ey = y->is_dir ? "" : strrchr(y->name ? y->name : "", '.');
        c = strcasecmp(ex ? ex : "", ey ? ey : "");
        c = c < 0 ? -1 : c > 0 ? 1 : 0;
        break;
    }
    default: break;
    }
    if (c == 0) {
        c = strcasecmp(x->name ? x->name : "", y->name ? y->name : "");
        c = c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    if (desc) c = -c;
    if (c == 0) c = a < b ? -1 : a > b ? 1 : 0;
    return c;
}

/* Fill `out` with the indices of `rows` this view shows, in order, and return
 * how many. `..` (when listed) always comes first and is never filtered out —
 * it is the way up, not a file. Hidden entries (a leading dot) only with
 * `show_hidden`. `tmp` is scratch of at least `n` slots (a merge sort: the
 * order is stable and the sort allocates nothing). */
static inline i32 sftp_view_build(const SftpRowKey *rows, i32 n, const char *filter,
                                  bool show_hidden, SftpCol col, bool desc,
                                  i32 *out, i32 *tmp) {
    if (!rows || !out || n <= 0) return 0;
    i32 count = 0, up = -1;
    for (i32 i = 0; i < n; i++) {
        const char *nm = rows[i].name ? rows[i].name : "";
        if (!strcmp(nm, "..")) { if (up < 0) up = i; continue; }
        if (!strcmp(nm, ".")) continue;
        if (!show_hidden && nm[0] == '.') continue;
        if (!sftp_name_matches(nm, filter)) continue;
        out[count++] = i;
    }
    /* Bottom-up merge sort of out[0..count) through tmp. */
    if (tmp && count > 1) {
        i32 *src = out, *dst = tmp;
        for (i32 width = 1; width < count; width *= 2) {
            for (i32 lo = 0; lo < count; lo += 2 * width) {
                i32 mid = lo + width < count ? lo + width : count;
                i32 hi  = lo + 2 * width < count ? lo + 2 * width : count;
                i32 i = lo, j = mid, k = lo;
                while (i < mid && j < hi)
                    dst[k++] = sftp_row_cmp(rows, src[i], src[j], col, desc) <= 0 ? src[i++] : src[j++];
                while (i < mid) dst[k++] = src[i++];
                while (j < hi)  dst[k++] = src[j++];
            }
            i32 *s = src; src = dst; dst = s;
        }
        if (src != out) memcpy(out, src, (usize)count * sizeof *out);
    }
    if (up >= 0) {
        memmove(out + 1, out, (usize)count * sizeof *out);
        out[0] = up;
        count++;
    }
    return count;
}

/* ---- cells ------------------------------------------------------------- */

/* "drwxr-xr-x" (10 characters + NUL), with the set-id and sticky letters ls
 * prints. `is_link` gives the leading 'l'. */
static inline void sftp_perm_string(u32 mode, bool is_dir, bool is_link, char out[11]) {
    out[0] = is_link ? 'l' : is_dir ? 'd' : '-';
    static const char rwx[] = "rwxrwxrwx";
    for (i32 i = 0; i < 9; i++) out[1 + i] = (mode & (0400u >> i)) ? rwx[i] : '-';
    if (mode & 04000u) out[3] = (mode & 0100u) ? 's' : 'S';
    if (mode & 02000u) out[6] = (mode & 0010u) ? 's' : 'S';
    if (mode & 01000u) out[9] = (mode & 0001u) ? 't' : 'T';
    out[10] = '\0';
}

/* "512 B", "4.1 KB", "12.0 MB", "1.4 GB" — binary units, one decimal past a KB. */
static inline void sftp_size_string(u64 bytes, char *out, usize cap) {
    if (!out || cap == 0) return;
    if (bytes < 1024u) { snprintf(out, cap, "%llu B", (unsigned long long)bytes); return; }
    static const char *const unit[] = { "KB", "MB", "GB", "TB", "PB" };
    f64 v = (f64)bytes / 1024.0;
    i32 u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    snprintf(out, cap, "%.1f %s", v, unit[u]);
}

/* The date cell, against `now` (both Unix seconds, local time):
 *   today          "Today 08:39"
 *   yesterday      "Yesterday 17:30"
 *   this year      "Oct 21 08:39"
 *   other years    "Oct 21, 2025"
 * A zero mtime (not known) is "--". */
static inline void sftp_date_string(u64 mtime, i64 now, char *out, usize cap) {
    if (!out || cap == 0) return;
    if (mtime == 0) { snprintf(out, cap, "--"); return; }
    time_t t = (time_t)mtime, n = (time_t)now;
    struct tm tm, tn;
    if (!localtime_r(&t, &tm) || !localtime_r(&n, &tn)) { snprintf(out, cap, "--"); return; }
    struct tm ty = tn;                      /* midnight yesterday, by the calendar */
    ty.tm_mday -= 1; ty.tm_hour = 12; ty.tm_min = ty.tm_sec = 0; ty.tm_isdst = -1;
    time_t yt = mktime(&ty);
    struct tm tyy;
    bool have_y = yt != (time_t)-1 && localtime_r(&yt, &tyy);
    if (tm.tm_year == tn.tm_year && tm.tm_yday == tn.tm_yday)
        strftime(out, cap, "Today %H:%M", &tm);
    else if (have_y && tm.tm_year == tyy.tm_year && tm.tm_yday == tyy.tm_yday)
        strftime(out, cap, "Yesterday %H:%M", &tm);
    else if (tm.tm_year == tn.tm_year)
        strftime(out, cap, "%b %e %H:%M", &tm);
    else
        strftime(out, cap, "%b %e, %Y", &tm);
    /* %e pads a one-digit day with a space; one space is enough. */
    char *dbl = strstr(out, "  ");
    if (dbl) memmove(dbl, dbl + 1, strlen(dbl + 1) + 1);
}

/* A file's kind from its extension when no language claims it: "PNG image",
 * "ZIP archive", "PDF document"… — else "<EXT> file", or "File" with none.
 * ASCII only, at most 12 characters of extension, so a name cannot put
 * anything else into the column. */
static inline void sftp_kind_fallback(const char *name, char *out, usize cap) {
    if (!out || cap == 0) return;
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (!dot || dot == name || !dot[1]) { snprintf(out, cap, "File"); return; }
    char ext[16];
    usize n = 0;
    for (const char *p = dot + 1; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (n + 1 >= sizeof ext || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            snprintf(out, cap, "File");
            return;
        }
        ext[n++] = (char)c;
    }
    ext[n] = '\0';
    static const struct { const char *ext, *kind; } table[] = {
        { "png", "image" }, { "jpg", "image" }, { "jpeg", "image" }, { "gif", "image" },
        { "webp", "image" }, { "bmp", "image" }, { "tif", "image" }, { "tiff", "image" },
        { "heic", "image" }, { "ico", "image" }, { "icns", "image" }, { "avif", "image" },
        { "zip", "archive" }, { "tar", "archive" }, { "gz", "archive" }, { "tgz", "archive" },
        { "bz2", "archive" }, { "xz", "archive" }, { "zst", "archive" }, { "7z", "archive" },
        { "rar", "archive" }, { "jar", "archive" }, { "deb", "package" }, { "rpm", "package" },
        { "pdf", "document" }, { "doc", "document" }, { "docx", "document" }, { "rtf", "document" },
        { "xls", "spreadsheet" }, { "xlsx", "spreadsheet" }, { "numbers", "spreadsheet" },
        { "ppt", "presentation" }, { "pptx", "presentation" }, { "key", "presentation" },
        { "mp3", "audio" }, { "wav", "audio" }, { "flac", "audio" }, { "aac", "audio" },
        { "m4a", "audio" }, { "ogg", "audio" }, { "opus", "audio" },
        { "mp4", "video" }, { "mov", "video" }, { "mkv", "video" }, { "webm", "video" },
        { "avi", "video" }, { "m4v", "video" },
        /* A leading '=' is the whole word: these kinds are not "<EXT> something". */
        { "dmg", "=Disk image" }, { "iso", "=Disk image" }, { "img", "=Disk image" },
        { "log", "=Log" }, { "txt", "=Plain text" }, { "csv", "table" }, { "tsv", "table" },
        { "pem", "=Certificate" }, { "crt", "=Certificate" }, { "pub", "=Public key" },
        { "ttf", "font" }, { "otf", "font" }, { "woff", "font" }, { "woff2", "font" },
        { "so", "=Shared library" }, { "dylib", "=Shared library" }, { "a", "=Static library" },
        { "o", "=Object file" }, { "bin", "=Binary" }, { "exe", "=Program" }, { "app", "=Application" },
    };
    char up[16];
    for (usize i = 0; i <= n; i++) {
        char c = ext[i];
        up[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    for (usize i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (!strcasecmp(ext, table[i].ext)) {
            if (table[i].kind[0] == '=') snprintf(out, cap, "%s", table[i].kind + 1);
            else snprintf(out, cap, "%s %s", up, table[i].kind);
            return;
        }
    }
    snprintf(out, cap, "%s file", up);
}

/* ---- the breadcrumb ---------------------------------------------------- */

#define SFTP_CRUMB_MAX 64

/* One folder of a path: its name is path[start, start + len); clicking it
 * goes to path[0, end) — "/" for the root. */
typedef struct { u16 start, len, end; } SftpCrumb;

/* Split an absolute path into its folders, the root first ("/" itself, then
 * one crumb per component; empty components from "//" are skipped). Returns
 * the count, at most `cap`: a deeper path keeps its LAST folders, which are
 * the ones a breadcrumb shows. */
static inline i32 sftp_crumbs(const char *path, SftpCrumb *out, i32 cap) {
    if (!path || !out || cap <= 0) return 0;
    usize n = strlen(path);
    if (n > 0xFFFFu) n = 0xFFFFu;
    SftpCrumb all[SFTP_CRUMB_MAX + 1];
    i32 count = 0;
    all[count++] = (SftpCrumb){ 0, 1, 1 };               /* the root */
    usize i = 0;
    while (i < n) {
        while (i < n && path[i] == '/') i++;
        usize s = i;
        while (i < n && path[i] != '/') i++;
        if (i > s) {
            SftpCrumb c = { (u16)s, (u16)(i - s), (u16)i };
            if (count < SFTP_CRUMB_MAX + 1) all[count++] = c;
            else { memmove(all + 1, all + 2, (usize)(count - 2) * sizeof *all); all[count - 1] = c; }
        }
    }
    i32 first = count > cap ? count - cap : 0;
    for (i32 k = first; k < count; k++) out[k - first] = all[k];
    return count - first;
}

/* ---- back / forward ---------------------------------------------------- */

#define SFTP_HIST_MAX   24
#define SFTP_HIST_PATH 1024

typedef struct {
    char path[SFTP_HIST_MAX][SFTP_HIST_PATH];
    i32  count;     /* entries held */
    i32  at;        /* the current one, -1 when empty */
} SftpHistory;

static inline void sftp_hist_reset(SftpHistory *h) { h->count = 0; h->at = -1; }

/* The pane went to `path` by itself (a click, a typed path, Up): what was
 * ahead of the current entry is dropped, as in a browser. Going to the folder
 * that is already current records nothing. */
static inline void sftp_hist_visit(SftpHistory *h, const char *path) {
    if (!h || !path || !path[0]) return;
    if (h->at >= 0 && !strcmp(h->path[h->at], path)) return;
    h->count = h->at + 1;
    if (h->count >= SFTP_HIST_MAX) {
        memmove(h->path[0], h->path[1], (usize)(SFTP_HIST_MAX - 1) * SFTP_HIST_PATH);
        h->count = SFTP_HIST_MAX - 1;
    }
    snprintf(h->path[h->count], SFTP_HIST_PATH, "%s", path);
    h->at = h->count;
    h->count++;
}

static inline bool sftp_hist_can_back(const SftpHistory *h)    { return h && h->at > 0; }
static inline bool sftp_hist_can_forward(const SftpHistory *h) { return h && h->at >= 0 && h->at + 1 < h->count; }

/* Step back / forward; the folder to go to, or NULL when there is none. The
 * caller navigates WITHOUT visiting (that would cut the list it stepped in). */
static inline const char *sftp_hist_back(SftpHistory *h) {
    if (!sftp_hist_can_back(h)) return NULL;
    return h->path[--h->at];
}
static inline const char *sftp_hist_forward(SftpHistory *h) {
    if (!sftp_hist_can_forward(h)) return NULL;
    return h->path[++h->at];
}

/* ---- paths ------------------------------------------------------------- */

/* `dir` + "/" + `name` into out (no doubled slash after the root). False when
 * it does not fit. */
static inline bool sftp_path_join(const char *dir, const char *name, char *out, usize cap) {
    if (!dir || !name || !out || cap == 0) return false;
    usize dl = strlen(dir);
    bool slash = dl > 0 && dir[dl - 1] == '/';
    int w = snprintf(out, cap, "%s%s%s", dir, slash ? "" : "/", name);
    return w >= 0 && (usize)w < cap;
}

/* The parent of an absolute path ("/a/b" -> "/a", "/a" -> "/", "/" -> "/"). */
static inline void sftp_path_parent(const char *path, char *out, usize cap) {
    if (!out || cap == 0) return;
    snprintf(out, cap, "%s", path && path[0] ? path : "/");
    usize n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = '\0';
    char *slash = strrchr(out, '/');
    if (!slash) { snprintf(out, cap, "/"); return; }
    if (slash == out) out[1] = '\0';
    else *slash = '\0';
}

#endif
