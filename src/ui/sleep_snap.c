/*
 * Liu — tab-sleep snapshot serialisation. See sleep_snap.h.
 */
#include "ui/sleep_snap.h"
#include "core/checked.h"
#include "core/config.h"   /* CONFIG_SCROLLBACK_MAX_LINES */
#include "core/private_tmp.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>

#include <limits.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Tab-sleep snapshots persist the primary screen and scrollback to a
 * temporary file so a sleeping pane can be restored without reconnecting.
 *
 * The format is versioned: v1 stores scrollback rows as Cell streams; v2
 * stores compressed codepoints, RLE attribute runs, and a wide bitmap; v3
 * adds screen wrap flags and terminal mode, pen, scroll-region,
 * saved-cursor, and keyboard state; v4 stores wide lead words in the row
 * metadata. The reader accepts supported earlier versions. */
#define SLEEP_SNAP_MAGIC          0x4C535031u   /* 'LSP1' */
#define SLEEP_SNAP_VERSION_V1     1u
#define SLEEP_SNAP_VERSION_V2     2u
/* v3 = v2 + what a wake needs to give the terminal back as it was and not just
 * as it looked: the wrap flag of every SCREEN row, and the state a running
 * program set and will not set again (modes, pen, scroll region, saved cursor,
 * keyboard protocol). Both sit between the screen cells and the scrollback
 * rows. v1/v2 files are still read; they never recorded either, and none is
 * invented for them. */
#define SLEEP_SNAP_VERSION_V3     3u
/* v4 = v3 whose scrollback rows can carry the wide LEAD words after the dummy
 * bitmap (TermLineCompressed.has_leads, bit 1 of the row's wrapped byte): the
 * lead bit left the attribute runs. Older rows keep it in their runs, which
 * termline_materialize still honours. */
#define SLEEP_SNAP_VERSION_V4     4u
#define SLEEP_SNAP_VERSION        SLEEP_SNAP_VERSION_V4

typedef struct {
    u32 magic;
    u32 version;
    i32 cols;
    i32 rows;
    i32 cursor_x;
    i32 cursor_y;
    i32 sb_count;
    u32 _reserved;
} SleepSnapHeader;

/* The continuation state, v3. Raw like the Cell stream beside it: a snapshot is
 * read by the binary that wrote it (the file is keyed to one pid). Every field
 * is validated on the way back in. */
typedef struct {
    u32      mode;
    u32      flags;               /* bit 0: cursor_visible */
    CellAttr cursor_attr;
    u32      cursor_ul_color;
    u8       cursor_ul_style;
    u8       modify_other_keys;
    u8       kbd_stack_len;
    u8       _pad0;
    i32      scroll_top, scroll_bottom;
    i32      saved_x, saved_y;
    CellAttr saved_attr;
    u32      saved_ul_color;
    u8       saved_ul_style;
    u8       _pad1[3];
    u32      kbd_flags;
    u32      kbd_stack[8];
} SleepSnapState;
#define SNAP_STATE_CURSOR_VISIBLE 1u

/* Modes a snapshot must never carry: the grid it holds is the PRIMARY screen,
 * and a synchronized-output latch is a moment in a stream, not a setting. */
#define SNAP_MODE_NOT_RESTORED (MODE_ALT_SCREEN | MODE_SYNC_OUTPUT)

/* Forward decls from buffer.c — exposed for the v1/v2 snapshot readers so
 * they can install rows directly into the scrollback ring. */
extern void sb_push_cold(Terminal *t, TermLineCompressed *cold);
extern void sb_push(Terminal *t, Cell *row_cells, i32 len, bool wrapped);

static _Atomic u64 g_sleep_path_serial;

/* A snapshot contains a pane's scrollback in plaintext while its tab sleeps.
 * Store it only in a private directory; otherwise the tab stays awake. Create
 * files exclusively with mode 0600 and O_NOFOLLOW, and read only regular
 * files owned by the current effective UID. Startup removes stale snapshots.
 */
bool sleep_snap_dir(char *out, usize n) {
    return liu_private_tmp_dir(out, n, "liu-sleep");
}

void sleep_snap_path(char *out, usize n, i32 tab_idx, i32 pane) {
    char dir[1024];
    if (!out || n == 0) return;
    if (!sleep_snap_dir(dir, sizeof dir)) { out[0] = '\0'; return; }
    u64 serial = atomic_fetch_add_explicit(&g_sleep_path_serial, 1, memory_order_relaxed) + 1;
    int w = snprintf(out, n, "%s/liu-sleep-%d-t%d-p%d-g%llu.bin",
                     dir, (int)getpid(), tab_idx, pane, (unsigned long long)serial);
    if (w <= 0 || (usize)w >= n) out[0] = '\0';
}

static FILE *snap_create(const char *path) {
    const int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC;
    int fd = open(path, flags, 0600);
    if (fd < 0 && errno == EEXIST) {
        /* A pre-existing path with this process ID may be stale. Replace it
         * only if it is a regular file owned by the current effective UID;
         * never follow a symbolic link or remove another owner's file. */
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
            unlink(path) == 0)
            fd = open(path, flags, 0600);
    }
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(path); }
    return f;
}

static FILE *snap_open(const char *path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid()) { close(fd); return NULL; }
    FILE *f = fdopen(fd, "rb");
    if (!f) close(fd);
    return f;
}

/* Remove snapshot files whose process ID is not running. A wake or tab
 * teardown removes its own file; this sweep handles files left by a process
 * that exited without cleanup. Returns the number removed. */
i32 sleep_snap_sweep_stale(void) {
    char dir[1024];
    if (!sleep_snap_dir(dir, sizeof dir)) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    i32 removed = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        static const char prefix[] = "liu-sleep-";
        if (strncmp(e->d_name, prefix, sizeof prefix - 1) != 0) continue;
        const char *q = e->d_name + sizeof prefix - 1;
        long pid = 0;
        int digits = 0;
        while (*q >= '0' && *q <= '9' && digits < 10) { pid = pid * 10 + (*q - '0'); q++; digits++; }
        if (!digits || *q != '-' || pid <= 0 || pid == (long)getpid()) continue;
        if (kill((pid_t)pid, 0) == 0 || errno != ESRCH) continue;      /* alive, or not ours to ask about */
        char path[1400];
        int w = snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (w <= 0 || (usize)w >= sizeof path || lstat(path, &st) != 0 ||
            !S_ISREG(st.st_mode) || st.st_uid != geteuid()) continue;
        if (unlink(path) == 0) removed++;
    }
    closedir(d);
    return removed;
}

/* Write a single scrollback row in the v2 native compressed format:
 *   i32   len
 *   u8    wrapped            (v4: bit 0 wrapped, bit 1 has_leads)
 *   u16   run_count
 *   u32   codepoints[len]
 *   u64   wide_bits[(len+63)/64]   (twice as many with has_leads)
 *   AttrRun runs[run_count]
 *
 * Hot rows (no cold pointer) are compressed inline into a transient stack
 * record. Cold rows write straight from their existing buffers — no
 * materialize, no re-encode. */
static bool snap_v2_write_row(FILE *f, const TermLine *ln) {
    /* Build a temporary view; if hot, compress on demand. */
    TermLineCompressed *temp = NULL;
    bool free_temp = false;
    const TermLineCompressed *src = ln->cold;
    if (!src) {
        if (!termline_compress(ln->cells, ln->len, ln->wrapped, &temp)) return false;
        free_temp = true;
        src = temp;
    }

    i32 len      = src->len;
    u8  wrapped  = (u8)((src->wrapped ? 1u : 0u) | (src->has_leads ? 2u : 0u));
    u16 runs     = src->run_count;
    bool ok = true;
    if (fwrite(&len,     sizeof(i32), 1, f) != 1) ok = false;
    if (ok && fwrite(&wrapped,  1,    1, f) != 1) ok = false;
    if (ok && fwrite(&runs,     sizeof(u16), 1, f) != 1) ok = false;
    if (ok && len > 0 && src->codepoints) {
        if (fwrite(src->codepoints, sizeof(u32), (usize)len, f) != (usize)len) ok = false;
    }
    if (ok && len > 0 && src->wide_bits) {
        usize wb_words = (usize)((len + 63) / 64) * (src->has_leads ? 2u : 1u);
        if (fwrite(src->wide_bits, sizeof(u64), wb_words, f) != wb_words) ok = false;
    }
    if (ok && runs > 0 && src->runs) {
        if (fwrite(src->runs, sizeof(AttrRun), runs, f) != runs) ok = false;
    }
    if (free_temp) termline_compressed_destroy(temp);
    return ok;
}

/* One stdio buffer for the whole snapshot, both directions.
 *
 * Every scrollback row is several small fwrite calls — the length, the wrap
 * flag, the run count, the codepoints, the wide bitmap and the runs — and a
 * snapshot at the scrollback maximum has tens of thousands of rows. Through
 * the platform default buffer (4 KiB on macOS) that is thousands of write(2)
 * syscalls for one sleep, and the same shape on the way back.
 *
 * A megabyte of buffer turns those into a handful. It changes nothing else:
 * the format is identical, every error path still unlinks, and a failed
 * setvbuf simply leaves the platform default in place. */
#define SNAP_IO_BUF (1024u * 1024u)

static bool snap_cancelled(const SleepSnapControl *control) {
    return control && control->cancel && atomic_load_explicit(control->cancel, memory_order_acquire);
}
static void snap_progress(const SleepSnapControl *control, const Terminal *t, usize staging) {
    if (control && control->progress) control->progress(control->context, t, staging);
}

/* Underline styles for the primary screen are stored in an optional trailer.
 * Cell rows record whether a cell is underlined, while style and color live in
 * the terminal's sparse map and are not present in the Cell stream.
 *
 * At EOF, the trailer contains SleepSnapUnderline records[count], followed by
 * count and a magic value. A reader can detect it by seeking back from EOF;
 * files without the trailer have no matching magic. Scrollback rows do not
 * include underline styles because the scrollback ring does not store them. */
#define SLEEP_SNAP_UL_MAGIC 0x314C5553u   /* 'SUL1' */
typedef struct {
    u16 row, col;
    u32 color;
    u8  style;
    u8  _pad[3];
} SleepSnapUnderline;

static bool snap_write_underlines(FILE *f, Terminal *t) {
    u32 count = 0;
    if (t->cells && t->ul_map_count > 0 && t->rows <= 0xFFFF && t->cols <= 0xFFFF) {
        for (i32 row = 0; row < t->rows; row++) {
            const Cell *line = &t->cells[(usize)row * (usize)t->cols];
            for (i32 col = 0; col < t->cols; col++) {
                if (!(line[col].attr.flags & ATTR_UNDERLINE)) continue;
                u32 color = 0; u8 style = 0;
                terminal_get_underline(t, col, row, &color, &style);
                if (color == 0 && style == 0) continue;
                SleepSnapUnderline r = {.row = (u16)row, .col = (u16)col,
                                        .color = color, .style = style};
                if (fwrite(&r, sizeof r, 1, f) != 1) return false;
                count++;
            }
        }
    }
    u32 tail[2] = {count, SLEEP_SNAP_UL_MAGIC};
    return fwrite(tail, sizeof tail, 1, f) == 1;
}

/* Best effort: a snapshot without the trailer, or with one that does not add
 * up, restores its plain underlines only. `screen_cells` is rows x cols of
 * the SNAPSHOT — a valid trailer names no more cells than the screen had. */
static void snap_read_underlines(FILE *f, Terminal *t, usize screen_cells) {
    u32 tail[2];
    if (!t->cells || fseek(f, 0, SEEK_END) != 0) return;
    long size = ftell(f);
    if (size < (long)(sizeof(SleepSnapHeader) + sizeof tail)) return;
    if (fseek(f, size - (long)sizeof tail, SEEK_SET) != 0 ||
        fread(tail, sizeof tail, 1, f) != 1) return;
    if (tail[1] != SLEEP_SNAP_UL_MAGIC || tail[0] == 0 || (usize)tail[0] > screen_cells) return;
    long body = (long)tail[0] * (long)sizeof(SleepSnapUnderline);
    long start = size - (long)sizeof tail - body;
    if (start < (long)sizeof(SleepSnapHeader) || fseek(f, start, SEEK_SET) != 0) return;
    for (u32 i = 0; i < tail[0]; i++) {
        SleepSnapUnderline r;
        if (fread(&r, sizeof r, 1, f) != 1) return;
        if ((i32)r.row >= t->rows || (i32)r.col >= t->cols) continue;
        const Cell *c = &t->cells[(usize)r.row * (usize)t->cols + r.col];
        if (!(c->attr.flags & ATTR_UNDERLINE)) continue;
        terminal_set_underline(t, (i32)r.col, (i32)r.row, r.color, r.style);
    }
}

bool sleep_snap_write_control(const char *path, Terminal *t, const SleepSnapControl *control) {
    if (!t || !path || !path[0] || snap_cancelled(control)) return false;
    FILE *f = snap_create(path);
    if (!f) return false;
    char *iobuf = (char *)malloc(SNAP_IO_BUF);
    if (iobuf) setvbuf(f, iobuf, _IOFBF, SNAP_IO_BUF);
    usize staging = iobuf ? SNAP_IO_BUF : 0;
    snap_progress(control, t, staging);
    SleepSnapHeader h = {
        .magic    = SLEEP_SNAP_MAGIC,
        .version  = SLEEP_SNAP_VERSION,
        .cols     = t->cols,
        .rows     = t->rows,
        .cursor_x = t->cursor_x,
        .cursor_y = t->cursor_y,
        .sb_count = t->sb_count,
        ._reserved = 0,
    };
    if (fwrite(&h, sizeof(h), 1, f) != 1) goto err;

    /* Primary screen stays as a flat Cell stream — it's small (rows*cols
     * cells, no compression upside) and simplifies wake-time restore. */
    if (t->cells && t->rows > 0 && t->cols > 0) {
        usize n;
        if (!checked_mul_usize((usize)t->rows, (usize)t->cols, &n)) goto err;
        if (fwrite(t->cells, sizeof(Cell), n, f) != n) goto err;
    }
    /* v3: the wrap flag of each screen row, then the continuation state. A
     * soft-wrapped line is one line; without its flag a copy of it, and the
     * next reflow, saw two. */
    if (t->rows > 0) {
        u8 *wrap = calloc((usize)t->rows, 1);
        if (!wrap) goto err;
        if (t->line_wrapped)
            for (i32 y = 0; y < t->rows; y++) wrap[y] = t->line_wrapped[y] ? 1 : 0;
        bool wrote = fwrite(wrap, 1, (usize)t->rows, f) == (usize)t->rows;
        free(wrap);
        if (!wrote) goto err;
    }
    {
        SleepSnapState st;
        memset(&st, 0, sizeof st);
        st.mode              = t->mode;
        st.flags             = t->cursor_visible ? SNAP_STATE_CURSOR_VISIBLE : 0;
        st.cursor_attr       = t->cursor_attr;
        st.cursor_ul_color   = t->cursor_ul_color;
        st.cursor_ul_style   = t->cursor_ul_style;
        st.modify_other_keys = t->modify_other_keys;
        i32 kn = t->Liu_kbd_stack_len;
        if (kn < 0) kn = 0;
        if (kn > 8) kn = 8;
        st.kbd_stack_len     = (u8)kn;
        st.scroll_top        = t->scroll_top;
        st.scroll_bottom     = t->scroll_bottom;
        st.saved_x           = t->saved_x;
        st.saved_y           = t->saved_y;
        st.saved_attr        = t->saved_attr;
        st.saved_ul_color    = t->saved_ul_color;
        st.saved_ul_style    = t->saved_ul_style;
        st.kbd_flags         = t->Liu_kbd_flags;
        memcpy(st.kbd_stack, t->Liu_kbd_stack, sizeof st.kbd_stack);
        if (fwrite(&st, sizeof st, 1, f) != 1) goto err;
    }
    /* Scrollback rows in native compressed format — direct write from the
     * cold ring representation, no intermediate Cell scratch. */
    if (t->sb_ring && t->sb_capacity > 0) {
        for (i32 i = 0; i < t->sb_count; i++) {
            i32 idx = (t->sb_head + i) % t->sb_capacity;
            const TermLine *ln = &t->sb_ring[idx];
            if (snap_cancelled(control)) goto err;
            if (!snap_v2_write_row(f, ln)) goto err;
        }
    }
    if (snap_cancelled(control)) goto err;
    if (!snap_write_underlines(f, t)) goto err;
    bool flushed = fclose(f) == 0; /* final buffer flush can fail too */
    free(iobuf);
    snap_progress(control, t, 0);
    if (!flushed) unlink(path);
    return flushed;
err:
    fclose(f);
    free(iobuf);
    snap_progress(control, t, 0);
    unlink(path);
    return false;
}

/* V1 row reader for the flat Cell stream format. It feeds each row
 * through `sb_push`, which compresses it, and returns false on a read
 * error. */
static bool snap_v1_read_row(FILE *f, Terminal *t,
                             Cell **row_buf, i32 *row_cap, bool skip) {
    i32 len; u8 wrapped;
    if (fread(&len, sizeof(i32), 1, f) != 1) return false;
    if (fread(&wrapped, 1, 1, f) != 1)        return false;
    if (len < 0 || len > 16384) return false;
    usize row_bytes;
    if (!checked_mul_usize((usize)len, sizeof(Cell), &row_bytes) ||
        row_bytes > (usize)LONG_MAX) return false;
    if (skip) {
        if (row_bytes > 0 && fseek(f, (long)row_bytes, SEEK_CUR) != 0)
            return false;
        return true;
    }
    if (len > 0) {
        if (len > *row_cap) {
            Cell *grown = realloc(*row_buf, row_bytes);
            if (!grown) {
                return fseek(f, (long)row_bytes, SEEK_CUR) == 0;
            }
            *row_buf = grown;
            *row_cap = len;
        }
        if (fread(*row_buf, sizeof(Cell), (usize)len, f) != (usize)len) return false;
        sb_push(t, *row_buf, len, wrapped != 0);
    } else {
        sb_push(t, NULL, 0, wrapped != 0);
    }
    return true;
}

/* v2 row reader: native compressed format. Reads codepoints + wide_bits +
 * RLE runs directly into a heap TermLineCompressed and hands it to
 * sb_push_cold which installs it without re-compression. Returns false on
 * read error. */
static bool snap_v2_read_row(FILE *f, Terminal *t, bool skip, u32 version) {
    i32 len; u8 wrapped; u16 run_count;
    if (fread(&len, sizeof(i32), 1, f) != 1) return false;
    if (fread(&wrapped, 1, 1, f) != 1)        return false;
    if (fread(&run_count, sizeof(u16), 1, f) != 1) return false;

    /* Defensive bounds: discourage absurd allocations from a tampered file. */
    if (len < 0 || len > 16384 ||
        (len == 0 ? run_count != 0 : (run_count == 0 || run_count > (u16)len)))
        return false;
    bool leads = false;
    if (version >= SLEEP_SNAP_VERSION_V4) {
        if (wrapped > 3u || (len == 0 && (wrapped & 2u))) return false;
        leads = (wrapped & 2u) != 0;
        wrapped &= 1u;
    }
    usize wb_words = (usize)((len + 63) / 64) * (leads ? 2u : 1u);

    if (skip) {
        long advance = (long)len * (long)sizeof(u32)
                     + (long)wb_words * (long)sizeof(u64)
                     + (long)run_count * (long)sizeof(AttrRun);
        if (advance > 0 && fseek(f, advance, SEEK_CUR) != 0) return false;
        return true;
    }

    /* Build the line as a single contiguous block (same layout the compressor
     * produces) so termline_compressed_destroy / eviction free it correctly.
     */
    TermLineCompressed *cold = termline_alloc_block_leads(len, run_count, wrapped != 0, leads);
    if (!cold) return false;

    if (len > 0) {
        if (fread(cold->codepoints, sizeof(u32), (usize)len, f) != (usize)len) goto fail;
        if (wb_words > 0 &&
            fread(cold->wide_bits, sizeof(u64), wb_words, f) != wb_words) goto fail;
    }
    if (run_count > 0) {
        if (fread(cold->runs, sizeof(AttrRun), run_count, f) != run_count) goto fail;
        u32 covered = 0;
        for (u16 i = 0; i < run_count; i++) {
            if (cold->runs[i].length == 0 ||
                cold->runs[i].length > (u32)len - covered) goto fail;
            covered += cold->runs[i].length;
        }
        if (covered != (u32)len) goto fail;
    }

    sb_push_cold(t, cold);   /* takes ownership */
    return true;

fail:
    termline_compressed_destroy(cold);
    return false;
}

bool sleep_snap_read_control(const char *path, Terminal *t, const SleepSnapControl *control) {
    if (!t || !path || !path[0] || snap_cancelled(control)) return false;
    FILE *f = snap_open(path);
    if (!f) return false;
    /* Same reason as the write side, and a bigger effect: the read makes six
     * fread calls per scrollback row. See SNAP_IO_BUF. Registered with an
     * atexit-free lifetime — the buffer must outlive every fread and is
     * released after fclose on every one of this function's six exits, which
     * is what snap_read_done() below is for. */
    char *iobuf = (char *)malloc(SNAP_IO_BUF);
    if (iobuf) setvbuf(f, iobuf, _IOFBF, SNAP_IO_BUF);
    usize staging = iobuf ? SNAP_IO_BUF : 0;
    snap_progress(control, t, staging);
    #define SNAP_READ_DONE(rv) do { fclose(f); free(iobuf); snap_progress(control, t, 0); return (rv); } while (0)
    SleepSnapHeader h;
    if (fread(&h, sizeof(h), 1, f) != 1) SNAP_READ_DONE(false);
    if (h.magic != SLEEP_SNAP_MAGIC ||
        (h.version != SLEEP_SNAP_VERSION_V1 &&
         h.version != SLEEP_SNAP_VERSION_V2 &&
         h.version != SLEEP_SNAP_VERSION_V3 &&
         h.version != SLEEP_SNAP_VERSION_V4) ||
        h.cols <= 0 || h.rows <= 0 ||
        h.cols > 4096 || h.rows > 4096 ||
        h.sb_count < 0 || h.sb_count > CONFIG_SCROLLBACK_MAX_LINES) {
        unlink(path);
        SNAP_READ_DONE(false);
    }

    /* Primary screen — same flat Cell stream in v1 and v2. */
    usize src_n, src_bytes;
    if (!checked_mul_usize((usize)h.rows, (usize)h.cols, &src_n) ||
        !checked_mul_usize(src_n, sizeof(Cell), &src_bytes) ||
        src_bytes > MB(512)) {
        unlink(path);
        SNAP_READ_DONE(false);
    }
    /* The terminal is rebuilt in the geometry it was SAVED in. `t` arrives
     * sized for the pane it is waking into, and copying only the rectangle
     * the two share would drop the columns and rows that do not fit, then
     * push scrollback rows wider than the grid. A fresh terminal has nothing
     * to reflow, so resizing it first is cheap; the reflow back to the target
     * geometry is the same one a live resize uses, and it runs here, on the
     * worker, after everything is in. */
    const i32 target_cols = t->cols, target_rows = t->rows;
    if (h.cols != t->cols || h.rows != t->rows) {
        terminal_resize(t, h.cols, h.rows);
        if (t->cols != h.cols || t->rows != h.rows || !t->cells)
            SNAP_READ_DONE(false);
    }
    snap_progress(control, t, staging);
    if (snap_cancelled(control)) SNAP_READ_DONE(false);
    if (fread(t->cells, sizeof(Cell), src_n, f) != src_n) SNAP_READ_DONE(false);

    /* v3: wrap flags and continuation state. */
    SleepSnapState st;
    memset(&st, 0, sizeof st);
    bool have_state = false;
    if (h.version >= SLEEP_SNAP_VERSION_V3) {
        u8 *wrap = malloc((usize)h.rows);
        if (!wrap) SNAP_READ_DONE(false);
        if (fread(wrap, 1, (usize)h.rows, f) != (usize)h.rows) { free(wrap); SNAP_READ_DONE(false); }
        if (t->line_wrapped)
            for (i32 y = 0; y < h.rows; y++) t->line_wrapped[y] = wrap[y] != 0;
        free(wrap);
        if (fread(&st, sizeof st, 1, f) != 1) SNAP_READ_DONE(false);
        have_state = true;
    }

    /* The cursor, as recorded. cursor_x == cols is the deferred-wrap column —
     * the position after a row has been filled exactly, where the next
     * character wraps instead of overwriting the last one — and the parser
     * carries it as a normal state. Clamping it to cols-1 here is what made
     * the first character typed after a wake land on top of the last one. */
    t->cursor_x = h.cursor_x;
    t->cursor_y = h.cursor_y;
    if (t->cursor_x < 0) t->cursor_x = 0;
    if (t->cursor_y < 0) t->cursor_y = 0;
    if (t->cursor_x > t->cols) t->cursor_x = t->cols;
    if (t->cursor_y >= t->rows) t->cursor_y = t->rows - 1;

    /* Scrollback — version-dispatched per row. */
    bool complete = true;
    i32 want = h.sb_count;
    if (want > 0 && (!control || t->scrollback_limit > 0)) {
        i32 cap = t->scrollback_limit > 0 ? t->scrollback_limit : 2000;
        i32 skip = want > cap ? want - cap : 0;

        if (!t->sb_ring) {
            t->sb_capacity = cap;
            t->sb_ring = calloc((usize)t->sb_capacity, sizeof(TermLine));
            t->sb_head = 0;
            t->sb_count = 0;
        }
        if (t->sb_ring) {
            Cell *row_buf = NULL;
            i32   row_cap = 0;
            /* Skip the oldest rows that won't fit under the current limit. */
            for (i32 i = 0; i < skip; i++) {
                if (snap_cancelled(control)) { complete = false; break; }
                bool ok = (h.version == SLEEP_SNAP_VERSION_V1)
                        ? snap_v1_read_row(f, t, &row_buf, &row_cap, true)
                        : snap_v2_read_row(f, t, true, h.version);
                if (!ok) { complete = false; want = skip + i; break; }
            }
            for (i32 i = skip; i < want && complete; i++) {
                if (snap_cancelled(control)) { complete = false; break; }
                bool ok = (h.version == SLEEP_SNAP_VERSION_V1)
                        ? snap_v1_read_row(f, t, &row_buf, &row_cap, false)
                        : snap_v2_read_row(f, t, false, h.version);
                if (!ok) { complete = false; break; }
                if ((i & 31) == 0) snap_progress(control, t, staging);
            }
            free(row_buf);
            terminal_set_scrollback_limit(t, t->scrollback_limit);
        } else complete = false;
    }

    if (!snap_cancelled(control)) snap_read_underlines(f, t, src_n);

    /* Back to the size the pane has now — the live resize's own reflow. */
    if (!snap_cancelled(control) &&
        (t->cols != target_cols || t->rows != target_rows)) {
        terminal_resize(t, target_cols, target_rows);
        if (t->cols != target_cols || t->rows != target_rows) complete = false;
    }
    /* What the program set is applied last: the resize above resets the scroll
     * region, and a region or a saved cursor means nothing in another size. */
    if (have_state && complete && !snap_cancelled(control)) {
        bool same_geometry = h.cols == t->cols && h.rows == t->rows;
        t->mode = (st.mode & ~(u32)SNAP_MODE_NOT_RESTORED) |
                  (t->mode & (u32)SNAP_MODE_NOT_RESTORED);
        t->cursor_visible   = (st.flags & SNAP_STATE_CURSOR_VISIBLE) != 0;
        t->cursor_attr      = st.cursor_attr;
        t->cursor_ul_color  = st.cursor_ul_color;
        t->cursor_ul_style  = st.cursor_ul_style;
        t->modify_other_keys = st.modify_other_keys;
        t->Liu_kbd_flags    = st.kbd_flags;
        i32 kn = st.kbd_stack_len > 8 ? 8 : (i32)st.kbd_stack_len;
        t->Liu_kbd_stack_len = kn;
        memcpy(t->Liu_kbd_stack, st.kbd_stack, sizeof t->Liu_kbd_stack);
        if (same_geometry) {
            if (st.scroll_top >= 0 && st.scroll_bottom >= st.scroll_top &&
                st.scroll_bottom < t->rows) {
                t->scroll_top = st.scroll_top;
                t->scroll_bottom = st.scroll_bottom;
            }
            if (st.saved_x >= 0 && st.saved_x <= t->cols &&
                st.saved_y >= 0 && st.saved_y < t->rows) {
                t->saved_x = st.saved_x;
                t->saved_y = st.saved_y;
                t->saved_attr = st.saved_attr;
                t->saved_ul_color = st.saved_ul_color;
                t->saved_ul_style = st.saved_ul_style;
                /* The snapshot keeps one saved cursor. A tab that slept under
                 * a full-screen program gives it to both screens, so leaving
                 * that program puts the shell's cursor where it was. */
                t->parked.saved_x = st.saved_x;
                t->parked.saved_y = st.saved_y;
                t->parked.saved_attr = st.saved_attr;
                t->parked.saved_ul_color = st.saved_ul_color;
                t->parked.saved_ul_style = st.saved_ul_style;
            }
        }
    }

    if (t->dirty_rows) {
        for (i32 w = 0; w < t->dirty_words; w++) t->dirty_rows[w] = ~(u64)0;
    }
    t->dirty = true;
    fclose(f);
    free(iobuf);
    snap_progress(control, t, 0);
    bool restored = complete && !snap_cancelled(control);
    if (!control || (restored && !control->preserve_file)) unlink(path);
    return control ? restored : true;
    #undef SNAP_READ_DONE
}

/* Synchronous callers use the same format and ownership contract. */
bool sleep_snap_write(const char *path, Terminal *t) {
    return sleep_snap_write_control(path, t, NULL);
}
bool sleep_snap_read(const char *path, Terminal *t) {
    return sleep_snap_read_control(path, t, NULL);
}
