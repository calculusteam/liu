/*
 * Liu - VT escape sequence parser
 * State machine that processes raw PTY output into terminal operations.
 */
#include "terminal/terminal.h"
#include "terminal/img_link.h"
#include "platform/platform.h"
#include "core/checked.h"
#include "core/imgproc.h"
#include "core/text_sanitize.h"
#include "core/mem_budget.h"
#include "core/utf8.h"
#include "core/term_reply.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>

/* stb_image for PNG decoding (Liu graphics + inline images) */
#include "stb_image.h"

/* Weak fallback for builds without a platform backend. The application supplies
 * the platform implementation. */
#if defined(__GNUC__)
__attribute__((weak)) void platform_wake_event_loop(void) { }
#endif

/* Forward declarations from buffer.c */
extern void buffer_clear_region(Cell *cells, i32 cols, i32 x0, i32 y0, i32 x1, i32 y1, CellAttr attr);
extern void buffer_scroll_up(Terminal *t, i32 n);
extern void buffer_scroll_down(Terminal *t, i32 n);

/* ARM64 NEON (Apple Silicon) or x86-64 SSE2 printable-run scanner. */
#if defined(USE_ASM) && (defined(__aarch64__) || defined(__arm64__) || defined(__x86_64__) || defined(__amd64__))
#define HAVE_VT_ASM 1
extern int asm_find_printable_run(const u8 *data, int len);
#else
#define HAVE_VT_ASM 0
#endif

/* Length of the leading run of printable ASCII (0x20..0x7E) — the contract the
 * NEON/SSE2 routine implements. The portable version exists so the bulk write
 * path below is not Apple-and-x86 only: the scan is the cheap half, the
 * per-character state machine it avoids is the expensive one, and a non-ASM
 * build (or a non-Apple ARM, which has no .S here) wants that just as much. */
#if HAVE_VT_ASM
#define vt_printable_run(p, n) asm_find_printable_run((p), (n))
#else
static int vt_printable_run(const u8 *p, int n) {
    int i = 0;
    while (i < n && p[i] >= 0x20 && p[i] < 0x7F) i++;
    return i;
}
#endif

/* Parser states */
enum {
    STATE_GROUND = 0,
    STATE_ESC,          /* saw ESC */
    STATE_CSI,          /* saw ESC [ */
    STATE_OSC,          /* saw ESC ] */
    STATE_DCS,          /* saw ESC P */
    STATE_ESC_HASH,     /* saw ESC # */
    STATE_CHARSET,      /* saw ESC ( or ESC ) */
    STATE_ESC_SKIP,     /* saw ESC + an intermediate Liu has no function for: swallow up to the final byte */
    STATE_APC,          /* saw ESC _ (for Liu graphics) */
    /* A string sequence whose payload was refused — over the cap, or the
     * buffer could not grow. Its bytes still BELONG to the sequence, so they
     * are consumed and dropped until the terminator. Going to STATE_GROUND
     * instead would print the rest of the payload on screen, which for a
     * Liu-graphics or Sixel image is megabytes of base64 typed into the
     * scrollback. */
    STATE_STR_SINK,
};

/* --- APC / DCS payload buffers -------------------------------------------
 * Both string states start in the fixed esc_buf and spill into a heap buffer
 * that reaches 16 MiB (APC, and the OSC path that borrows dcs_buf) or 4 MiB
 * (Sixel). That is per TERMINAL, and Liu runs up to 8 windows x 32 tabs x 8
 * panes, so all of it is counted in the memory ledger, the one place the
 * process answers "how much am I holding".
 *
 * These three helpers own the (pointer, capacity, ledger) triple together
 * because twelve sites free one of these buffers and three grow one: a ledger
 * line written at each of them would drift on the first new path. The
 * invariant every caller keeps is that `cap` is non-zero exactly while `buf`
 * is non-NULL, and the ledger holds exactly `cap` bytes.
 *
 * MEM_IMAGE_CPU is the class: an APC payload is a Liu-graphics image and a
 * DCS payload is Sixel, both on their way to a decoder that accounts its
 * output under that same class. Main-thread only, like the ledger — VT parsing
 * stays on the main thread; the PTY reader threads are a gather stage. */
static u8 *str_buf_alloc(i32 cap) {
    u8 *p = malloc((usize)cap);
    if (p) mem_budget_adjust(MEM_IMAGE_CPU, (isize)cap);
    return p;
}

static u8 *str_buf_grow(u8 *buf, i32 old_cap, i32 new_cap) {
    u8 *grown = realloc(buf, (usize)new_cap);
    if (grown) mem_budget_adjust(MEM_IMAGE_CPU, (isize)new_cap - (isize)old_cap);
    return grown;
}

static void str_buf_free(u8 **buf, i32 *len, i32 *cap) {
    if (*buf) {
        free(*buf);
        mem_budget_adjust(MEM_IMAGE_CPU, -(isize)*cap);
    }
    *buf = NULL;
    *len = 0;
    *cap = 0;
}

/* --- A graphics transmission that arrives in pieces -----------------------
 *
 * Senders of the APC _G graphics protocol split the payload into 4096-byte
 * base64 pieces: the first command carries the control keys and m=1, the
 * following ones carry only `m`, the last m=0. This holds what has arrived
 * until the last piece does. One per terminal, and at most
 * TERM_GFX_TRANSFER_MAX encoded bytes — what a single command's buffer may
 * already reach — on the same ledger as that buffer.
 *
 * There is no timer: a transfer nobody finishes costs at most the bound, like
 * an APC nobody terminates, and it is let go by the first thing that is not
 * its next piece (see handle_Liu_graphics). */
#ifndef TERM_GFX_TRANSFER_MAX
#define TERM_GFX_TRANSFER_MAX (16 * 1024 * 1024)
#endif

typedef struct {
    u8         *b64;
    i32         len, cap;
    /* Set when the transfer can no longer become an image (past the bound, an
     * unsupported format, no memory): the buffer is gone, the remaining pieces
     * are swallowed, and the sender gets `err` once, with the last piece. */
    const char *err;
    u32         format;
    i32         src_w, src_h, disp_cols, disp_rows;
    u32         image_id, placement_id;
    i32         z_index;
    bool        quiet;
} TermGfxTransfer;

static void gfx_transfer_free(Terminal *t) {
    TermGfxTransfer *x = t ? (TermGfxTransfer *)t->gfx_transfer : NULL;
    if (!x) return;
    str_buf_free(&x->b64, &x->len, &x->cap);
    free(x);
    t->gfx_transfer = NULL;
}

static void gfx_transfer_fail(TermGfxTransfer *x, const char *err) {
    str_buf_free(&x->b64, &x->len, &x->cap);
    if (!x->err) x->err = err;
}

static void gfx_transfer_append(TermGfxTransfer *x, const u8 *bytes, i32 n) {
    if (x->err || n <= 0) return;
    if (n > TERM_GFX_TRANSFER_MAX - x->len) { gfx_transfer_fail(x, "EFBIG"); return; }
    i32 need = x->len + n;
    if (need > x->cap) {
        i32 ncap = x->cap ? x->cap + (x->cap >> 1) : 16384;
        if (ncap < need) ncap = need;
        if (ncap > TERM_GFX_TRANSFER_MAX) ncap = TERM_GFX_TRANSFER_MAX;
        u8 *grown = x->b64 ? str_buf_grow(x->b64, x->cap, ncap) : str_buf_alloc(ncap);
        if (!grown) { gfx_transfer_fail(x, "ENOMEM"); return; }
        x->b64 = grown;
        x->cap = ncap;
    }
    memcpy(x->b64 + x->len, bytes, (usize)n);
    x->len = need;
}

/* terminal_destroy frees these too, and must not be the one place that knows
 * the ledger rule. */
void terminal_str_bufs_free(Terminal *t) {
    if (!t) return;
    str_buf_free(&t->apc_buf, &t->apc_len, &t->apc_cap);
    str_buf_free(&t->dcs_buf, &t->dcs_len, &t->dcs_cap);
    gfx_transfer_free(t);
}

/* =========================================================================
 * Response emit — send data back to PTY (for DSR, DA, DECRQM etc.)
 * ========================================================================= */

static void emit_response(Terminal *t, const char *data, i32 len) {
    if (t && data && len > 0 && t->on_response) {
        t->on_response(t, (const u8 *)data, len, t->userdata);
    }
}

static void emit_response_fmt(Terminal *t, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        i32 written = n < (int)sizeof(buf) ? (i32)n : (i32)sizeof(buf) - 1;
        emit_response(t, buf, written);
    }
}

/* Where a cursor position report puts the cursor. With the wrap pending the
 * cursor is parked past the last column, which is not a column: the report
 * says the last one. Under origin mode rows count from the top margin, as
 * CUP's do. */
static void cursor_report_position(const Terminal *t, i32 *row, i32 *col) {
    i32 c = t->cursor_x >= t->cols ? t->cols : t->cursor_x + 1;
    i32 r = t->cursor_y + 1 - ((t->mode & MODE_ORIGIN) ? t->scroll_top : 0);
    *row = r < 1 ? 1 : r;
    *col = c;
}

bool terminal_reply_position(const Terminal *t, i32 *row, i32 *col) {
    if (!t || !row || !col || t->parse_state != STATE_GROUND || t->utf8_pending_len != 0)
        return false;
    cursor_report_position(t, row, col);
    return *col >= 1;
}

void terminal_color_scheme_changed(Terminal *t) {
    bool dark;
    if (!t || !(t->mode & MODE_COLOR_SCHEME) || !terminal_theme_is_dark(&dark)) return;
    emit_response_fmt(t, "\x1b[?997;%dn", dark ? 1 : 2);
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

static inline void cursor_clamp(Terminal *t) {
    if (t->cursor_x < 0) t->cursor_x = 0;
    if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
    if (t->cursor_y < 0) t->cursor_y = 0;
    if (t->cursor_y >= t->rows) t->cursor_y = t->rows - 1;
}

/* One line down, as LF, IND, NEL and an autowrap all mean it. Only a cursor ON
 * the bottom margin scrolls the region. Anywhere else it moves, and stops at
 * the last row: below the region there is nothing to scroll into, so a cursor
 * UNDER the region (a status line below the margin that wraps or ends in LF)
 * never enters or scrolls it. Returns true when the region moved. */
static bool cursor_index(Terminal *t) {
    if (t->cursor_y == t->scroll_bottom) {
        buffer_scroll_up(t, 1);
        return true;
    }
    if (t->cursor_y < t->rows - 1) t->cursor_y++;
    return false;
}

/* One line up (RI): the mirror of cursor_index. Above the region the cursor
 * moves and stops at row 0. */
static bool cursor_reverse_index(Terminal *t) {
    if (t->cursor_y == t->scroll_top) {
        buffer_scroll_down(t, 1);
        return true;
    }
    if (t->cursor_y > 0) t->cursor_y--;
    return false;
}

/* The cell at (x, y) is about to be overwritten or erased: if it is half of a
 * wide character, the other half goes with it. Without this a narrow
 * character written over the right half left the lead (still flagged WIDE,
 * still drawn two cells wide, under the new glyph), and one written over the
 * lead left a dummy that every text reader skips — a space that copies as
 * nothing. A dummy with no lead to its left is the spacer a wrap leaves in
 * the last column (see put_char) and has nothing to take with it. */
static inline void wide_unpair(Terminal *t, i32 x, i32 y) {
    if (x < 0 || x >= t->cols || y < 0 || y >= t->rows) return;
    Cell *c = &t->cells[y * t->cols + x];
    if (c->attr.flags & ATTR_WDUMMY) {
        if (x > 0 && (c[-1].attr.flags & ATTR_WIDE)) {
            c[-1].codepoint = ' ';
            c[-1].attr.flags &= (u16)~ATTR_WIDE;
        }
        c->codepoint = ' ';
        c->attr.flags &= (u16)~ATTR_WDUMMY;
    }
    if (c->attr.flags & ATTR_WIDE) {
        if (x + 1 < t->cols && (c[1].attr.flags & ATTR_WDUMMY)) {
            c[1].codepoint = ' ';
            c[1].attr.flags &= (u16)~ATTR_WDUMMY;
        }
        c->attr.flags &= (u16)~ATTR_WIDE;
    }
}

/* A range of one row is about to be erased or overwritten whole: only its two
 * ends can cut a wide character. */
static inline void wide_unpair_span(Terminal *t, i32 y, i32 x0, i32 x1) {
    if (x0 > x1) return;
    wide_unpair(t, x0, y);
    if (x1 != x0) wide_unpair(t, x1, y);
}

/* DECSC: the position, the rendition, origin mode and the character sets. */
static void cursor_save(Terminal *t) {
    t->saved_x = t->cursor_x;
    t->saved_y = t->cursor_y;
    t->saved_attr = t->cursor_attr;
    t->saved_ul_color = t->cursor_ul_color;
    t->saved_ul_style = t->cursor_ul_style;
    t->saved_origin = (t->mode & MODE_ORIGIN) != 0;
    t->saved_charset_g[0] = t->charset_g[0];
    t->saved_charset_g[1] = t->charset_g[1];
    t->saved_charset_gl = t->charset_gl;
}

/* DECRC. A slot nothing was saved into holds home and the defaults. */
static void cursor_restore(Terminal *t) {
    t->cursor_x = t->saved_x;
    t->cursor_y = t->saved_y;
    t->cursor_attr = t->saved_attr;
    t->cursor_ul_color = t->saved_ul_color;
    t->cursor_ul_style = t->saved_ul_style;
    if (t->saved_origin) t->mode |= MODE_ORIGIN; else t->mode &= ~MODE_ORIGIN;
    t->charset_g[0] = t->saved_charset_g[0];
    t->charset_g[1] = t->saved_charset_g[1];
    t->charset_gl = t->saved_charset_gl;
    cursor_clamp(t);
}

/* The saved-cursor slot of the showing screen, back to "nothing saved". */
static void cursor_saved_reset(Terminal *t) {
    t->saved_x = 0;
    t->saved_y = 0;
    t->saved_attr = (CellAttr){ .fg = FG_DEFAULT, .bg = BG_DEFAULT, .flags = 0 };
    t->saved_ul_color = 0;
    t->saved_ul_style = 0;
    t->saved_origin = false;
    t->saved_charset_g[0] = t->saved_charset_g[1] = 0;
    t->saved_charset_gl = 0;
}

/* The main and the alternate screen each keep a saved cursor and a keyboard
 * flag stack. Called wherever the grids change places, right after the swap:
 * what belonged to the screen that left is parked, what was parked is live. So
 * a full-screen program's ESC 7 cannot replace the position `?1049h` saved,
 * and the flags a shell asked for are off inside the program and back after
 * it, even when the program dies without popping its own. `entering_alt`
 * starts the alternate screen's side clean. */
static void screen_state_swap(Terminal *t, bool entering_alt) {
    i32 sx = t->saved_x, sy = t->saved_y;
    CellAttr sa = t->saved_attr;
    u32 suc = t->saved_ul_color;
    u8 sus = t->saved_ul_style, sgl = t->saved_charset_gl;
    u8 sg0 = t->saved_charset_g[0], sg1 = t->saved_charset_g[1];
    bool so = t->saved_origin;
    u32 kf = t->Liu_kbd_flags, ks[8];
    i32 kl = t->Liu_kbd_stack_len;
    memcpy(ks, t->Liu_kbd_stack, sizeof ks);

    t->saved_x = t->parked.saved_x;
    t->saved_y = t->parked.saved_y;
    t->saved_attr = t->parked.saved_attr;
    t->saved_ul_color = t->parked.saved_ul_color;
    t->saved_ul_style = t->parked.saved_ul_style;
    t->saved_origin = t->parked.saved_origin;
    t->saved_charset_g[0] = t->parked.saved_charset_g[0];
    t->saved_charset_g[1] = t->parked.saved_charset_g[1];
    t->saved_charset_gl = t->parked.saved_charset_gl;
    t->Liu_kbd_flags = t->parked.kbd_flags;
    t->Liu_kbd_stack_len = t->parked.kbd_stack_len;
    memcpy(t->Liu_kbd_stack, t->parked.kbd_stack, sizeof t->Liu_kbd_stack);

    t->parked.saved_x = sx;
    t->parked.saved_y = sy;
    t->parked.saved_attr = sa;
    t->parked.saved_ul_color = suc;
    t->parked.saved_ul_style = sus;
    t->parked.saved_origin = so;
    t->parked.saved_charset_g[0] = sg0;
    t->parked.saved_charset_g[1] = sg1;
    t->parked.saved_charset_gl = sgl;
    t->parked.kbd_flags = kf;
    t->parked.kbd_stack_len = kl;
    memcpy(t->parked.kbd_stack, ks, sizeof t->parked.kbd_stack);

    if (entering_alt) {
        cursor_saved_reset(t);
        t->Liu_kbd_flags = 0;
        t->Liu_kbd_stack_len = 0;
    }
}

/* DEC Special Graphics (`ESC ( 0`): what bytes 0x5F-0x7E mean while that set is
 * in use. terminfo's `acsc` for xterm-256color is written against exactly this
 * table, so box(), dialog, menuconfig, gdb -tui and pstree -G draw with it;
 * without it they draw `lqqqk x mqqqj`. */
static u32 dec_graphics(u8 b) {
    static const u16 map[32] = {
        0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,   /* _ ` a b c d e f */
        0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,   /* g h i j k l m n */
        0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,   /* o p q r s t u v */
        0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,   /* w x y z { | } ~ */
    };
    return (b >= 0x5F && b <= 0x7E) ? map[b - 0x5F] : b;
}

/* After a shift inside a row (ICH, DCH, insert mode) a wide character can have
 * lost its other half to the cut or to the row's end. Whatever is left
 * unpaired becomes a blank; the last column's lone dummy is the wrap spacer
 * and stays. */
static void wide_row_repair(Terminal *t, i32 y) {
    if (y < 0 || y >= t->rows) return;
    Cell *row = &t->cells[y * t->cols];
    for (i32 x = 0; x < t->cols; x++) {
        u16 f = row[x].attr.flags;
        if (f & ATTR_WIDE) {
            if (x + 1 < t->cols && (row[x + 1].attr.flags & ATTR_WDUMMY)) { x++; continue; }
            row[x].codepoint = ' ';
            row[x].attr.flags &= (u16)~ATTR_WIDE;
        } else if ((f & ATTR_WDUMMY) && x != t->cols - 1) {
            row[x].codepoint = ' ';
            row[x].attr.flags &= (u16)~ATTR_WDUMMY;
        }
    }
}

/* Absolute row addressing under DECOM (origin mode).
 *
 * With origin mode set, row 1 means the FIRST ROW OF THE SCROLL REGION and the
 * cursor may not leave it, which lets a full-screen program address a pager
 * pane without knowing where on the screen the pane sits. CUP and VPA both come
 * through here.
 *
 * `row` is zero-based and already has the parameter's 1-based bias removed. */
static inline void cursor_set_row(Terminal *t, i32 row) {
    if (t->mode & MODE_ORIGIN) {
        t->cursor_y = t->scroll_top + row;
        if (t->cursor_y < t->scroll_top)    t->cursor_y = t->scroll_top;
        if (t->cursor_y > t->scroll_bottom) t->cursor_y = t->scroll_bottom;
        return;
    }
    t->cursor_y = row;
}

static inline i32 param(Terminal *t, i32 idx, i32 def) {
    if (idx >= t->param_count) return def;
    return t->params[idx] > 0 ? t->params[idx] : def;
}

/* Parameter i was introduced by ':' — it belongs to the parameter before it. */
#define PARAM_IS_SUB(t, i) ((((t)->param_sub) >> (i)) & 1u)

/* SGR 38 / 48 / 58: one colour, in either spelling.
 *
 *   38;5;n          38;2;r;g;b             the semicolon form
 *   38:5:n          38:2:r:g:b             the colon form without …
 *                   38:2:<cs>:r:g:b        … and with the colour-space slot,
 *                                          which is normally EMPTY (38:2::r:g:b)
 *
 * The last one is how ITU T.416 writes it and how crossterm, Neovim and
 * `Setulc` in Liu's own terminfo entry send an underline colour. The empty
 * slot is not red: read that way, blue would be left over to run as an
 * attribute of its own, and `58:2::255:0:0` would end in a 0 that resets
 * everything.
 *
 * `used` is how many parameters after the 38 belong to it. Channels and
 * indexes are checked, so a malformed colour is dropped, not truncated. */
typedef struct { bool ok, rgb; i32 idx, r, g, b, used; } SgrColor;
static SgrColor sgr_ext_color(const Terminal *t, i32 i) {
    SgrColor c = {0};
    i32 subs = 0;
    while (i + 1 + subs < t->param_count && PARAM_IS_SUB(t, i + 1 + subs)) subs++;
    const i32 *v = &t->params[i + 1];
    if (subs > 0) {
        c.used = subs;
        if (v[0] == 5 && subs >= 2) {
            c.idx = v[1];
            c.ok = c.idx >= 0 && c.idx < 256;
        } else if (v[0] == 2 && subs >= 4) {
            const i32 *ch = subs >= 5 ? &v[2] : &v[1];
            c.r = ch[0]; c.g = ch[1]; c.b = ch[2];
            c.rgb = true;
            c.ok = c.r >= 0 && c.r <= 255 && c.g >= 0 && c.g <= 255 && c.b >= 0 && c.b <= 255;
        }
        return c;
    }
    if (i + 2 < t->param_count && v[0] == 5) {
        c.idx = v[1];
        c.used = 2;
        c.ok = c.idx >= 0 && c.idx < 256;
    } else if (i + 4 < t->param_count && v[0] == 2) {
        c.r = v[1]; c.g = v[2]; c.b = v[3];
        c.used = 4;
        c.rgb = true;
        c.ok = c.r >= 0 && c.r <= 255 && c.g >= 0 && c.g <= 255 && c.b >= 0 && c.b <= 255;
    }
    return c;
}

/* Untrusted escape sequences can contain arbitrarily long digit runs. Keep
 * their accumulators defined and bounded instead of allowing signed `v*10+d`
 * overflow. Callers choose a semantic maximum appropriate to the field. */
static inline i32 decimal_push_bounded(i32 value, u8 digit, i32 maximum) {
    i32 d = (i32)(digit - (u8)'0');
    if (value >= maximum || value > (maximum - d) / 10) return maximum;
    return value * 10 + d;
}

static inline i32 ascii_hex_value(u8 c) {
    if (c >= '0' && c <= '9') return (i32)(c - '0');
    if (c >= 'a' && c <= 'f') return (i32)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (i32)(c - 'A') + 10;
    return -1;
}

static inline i32 add_bounded(i32 value, i32 amount, i32 maximum) {
    if (value >= maximum || amount >= maximum - value) return maximum;
    return value + amount;
}

/* Hex-encode src into dst (two lowercase digits per byte). Clamps to
 * dst_cap-1 and always NUL-terminates. Safe for untrusted DCS input. */
static void hex_encode_bounded(char *dst, usize dst_cap, const char *src) {
    if (!dst || dst_cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    static const char hexdig[] = "0123456789abcdef";
    usize di = 0;
    for (usize si = 0; src[si]; si++) {
        if (di + 2 >= dst_cap) break;
        u8 b = (u8)src[si];
        dst[di++] = hexdig[b >> 4];
        dst[di++] = hexdig[b & 0x0f];
    }
    dst[di] = '\0';
}

static void mark_current_row_zone(Terminal *t) {
    if (!t || !t->row_zones) return;
    if (t->cursor_y < 0 || t->cursor_y >= t->rows) return;
    t->row_zones[t->cursor_y] = t->current_zone;
}

static i32 command_row_content_end(Terminal *t, i32 row) {
    if (!t || row < 0 || row >= t->rows) return 0;

    i32 end = t->cols;
    while (end > 0) {
        Cell *c = &t->cells[row * t->cols + end - 1];
        if (c->attr.flags & ATTR_WDUMMY) {
            end--;
            continue;
        }
        if (c->codepoint == 0 || c->codepoint == ' ') {
            end--;
            continue;
        }
        break;
    }
    return end;
}

static void capture_pending_command(Terminal *t) {
    if (!t) return;

    /* Lazy-allocate the 4 KB capture buffer on first use. */
    if (!t->pending_command) {
        t->pending_command_cap = 4096;
        t->pending_command = calloc(1, (usize)t->pending_command_cap);
        if (!t->pending_command) { t->pending_command_cap = 0; return; }
    }
    const i32 cap = t->pending_command_cap;
    t->pending_command[0] = '\0';

    /* No anchor, or one that has scrolled off the top (a command longer than
     * the screen): there is no row to start from, and a fragment is worse
     * than nothing in a history that is offered back. */
    if (t->input_start_row < 0 || t->input_start_row >= t->rows || t->input_start_col < 0) return;
    i32 start_row = t->input_start_row;
    i32 end_row = t->cursor_y;

    if (start_row < 0 || start_row >= t->rows ||
        end_row < 0 || end_row >= t->rows ||
        start_row > end_row) {
        return;
    }

    while (end_row > start_row && command_row_content_end(t, end_row) == 0) {
        end_row--;
    }

    /* Without a B anchor the first row can contain the prompt itself. */
    if (t->input_start_row < 0 || t->input_start_col < 0) return;
    i32 out_len = 0;
    for (i32 row = start_row; row <= end_row; row++) {
        i32 row_end = command_row_content_end(t, row);
        i32 first_col = row == start_row ? t->input_start_col : 0;
        for (i32 col = first_col; col < row_end; col++) {
            Cell *c = &t->cells[row * t->cols + col];
            if (c->attr.flags & ATTR_WDUMMY) continue;

            u32 cp = c->codepoint;
            if (cp == 0) cp = ' ';
            if (cp < 32 && cp != '\t') continue;

            u8 utf8[UTF8_CLUSTER_MAX * 4];
            u32 n = utf8_cluster_encode(cp, utf8, sizeof utf8);
            if (n == 0 || out_len + (i32)n >= cap) {
                t->pending_command[0] = 0; return;
            }
            memcpy(&t->pending_command[out_len], utf8, n);
            out_len += (i32)n;
        }

        bool wrapped = (t->line_wrapped && row < t->rows - 1 && t->line_wrapped[row]);
        if (!wrapped && row < end_row) {
            if (out_len >= cap - 1) { t->pending_command[0] = 0; return; }
            t->pending_command[out_len++] = '\n';
        }
    }

    while (out_len > 0) {
        char ch = t->pending_command[out_len - 1];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') break;
        out_len--;
    }
    t->pending_command[out_len] = '\0';
}

/* =========================================================================
 * Grapheme clusters
 *
 * A cell holds one u32, but one visible emoji is often several codepoints
 * (❤️ = U+2764 U+FE0F, 👍🏽 = base + skin tone, 👨‍👩‍👧 = three emoji + two ZWJs,
 * 🇹🇷 = two regional indicators). The trailing codepoints fold back into the
 * cell the base opened, via the interning table in utf8.c, so none of these
 * is split across cells.
 * ========================================================================= */

/* The renderer reads side data for all three decoration kinds. */
static inline bool cell_has_decoration(u16 flags) {
    return (flags & (ATTR_UNDERLINE | ATTR_STRIKETHROUGH | ATTR_OVERLINE)) != 0;
}

static void cluster_copy_decoration(Terminal *t) {
    u32 color = 0;
    u8 style = 0;
    terminal_get_underline(t, t->cluster_x, t->cluster_y, &color, &style);
    terminal_set_underline(t, t->cluster_x + 1, t->cluster_y, color, style);
}

/* The cell put_char last wrote, or NULL if the run was broken. The value check
 * is the guard: any escape sequence, control character or cursor move that ran
 * in between either reset cluster_x or overwrote the cell, and a cluster must
 * never attach to a cell the parser did not just print. */
static Cell *cluster_cell(Terminal *t) {
    if (t->cluster_x < 0 || t->cluster_y < 0) return NULL;
    if (t->cluster_x >= t->cols || t->cluster_y >= t->rows) return NULL;
    if (!t->cells) return NULL;
    Cell *c = &t->cells[t->cluster_y * t->cols + t->cluster_x];
    if (c->codepoint != t->last_printed_char) return NULL;
    /* The cursor must still sit immediately after that cell. Anything that
     * moved it — CUP, CR, a scroll, a resize — ends the cluster. */
    i32 w = (c->attr.flags & ATTR_WIDE) ? 2 : 1;
    if (t->cursor_y != t->cluster_y || t->cursor_x != t->cluster_x + w) return NULL;
    return c;
}

/* Append `cp` to the cluster in that cell. Widens the cell to two columns when
 * the addition turns a text-presentation base into an emoji (❤ → ❤️). */
static bool cluster_extend(Terminal *t, u32 cp) {
    Cell *c = cluster_cell(t);
    if (!c) return false;

    u32 cps[UTF8_CLUSTER_MAX];
    u32 n = 0;
    const u32 *old = utf8_cluster_lookup(c->codepoint, &n);
    if (old) {
        if (n >= UTF8_CLUSTER_MAX) return true;   /* saturated — swallow, don't split */
        memcpy(cps, old, n * sizeof(u32));
    } else {
        cps[0] = c->codepoint;
        n = 1;
    }
    cps[n++] = cp;

    u32 handle = utf8_cluster_intern(cps, n);
    if (handle == c->codepoint) return true;      /* table full: keep the base */

    bool was_wide = (c->attr.flags & ATTR_WIDE) != 0;
    bool now_wide = utf8_char_width(handle) == 2;

    c->codepoint = handle;
    t->last_printed_char = handle;

    if (was_wide && !now_wide) {
        /* VS15 (U+FE0E) asks for text presentation, which narrows a wide emoji
         * back to one column — the text-presentation rule, and the mirror of the VS16 case
         * below. Hand the second column back only if the cursor is still
         * parked right after this cell. */
        if (t->cursor_y == t->cluster_y && t->cursor_x == t->cluster_x + 2) {
            c->attr.flags &= (u16)~ATTR_WIDE;
            Cell *d = &t->cells[t->cluster_y * t->cols + t->cluster_x + 1];
            d->codepoint = ' ';
            d->attr = c->attr;
            d->attr.flags &= (u16)~(ATTR_WIDE | ATTR_WDUMMY);
            if (cell_has_decoration(c->attr.flags)) cluster_copy_decoration(t);
            t->cursor_x--;
        }
    } else if (now_wide && !was_wide) {
        /* Needs the next column. Only take it if the cursor is still parked
         * right after this cell and the row has room — otherwise leave the
         * cluster single-width rather than clobbering a neighbour. */
        if (t->cluster_x + 1 < t->cols && t->cursor_y == t->cluster_y &&
            t->cursor_x == t->cluster_x + 1) {
            c->attr.flags |= ATTR_WIDE;
            Cell *d = &t->cells[t->cluster_y * t->cols + t->cluster_x + 1];
            d->codepoint = ' ';
            d->attr = c->attr;
            d->attr.flags &= (u16)~ATTR_WIDE;
            d->attr.flags |= ATTR_WDUMMY;
            if (cell_has_decoration(c->attr.flags)) cluster_copy_decoration(t);
            t->cursor_x++;
        }
    }
    TERM_DIRTY_ROW(t, t->cluster_y);
    return true;
}

/* Should `cp` join the previous cell instead of opening its own? */
/* `width` is utf8_char_width(cp), which put_char needs anyway. A joiner is
 * always width 0 — utf8_char_width_ladder asks utf8_is_joiner before anything
 * that could give it a cell, and the table remembers the ladder — so a
 * codepoint that takes a cell is no joiner and skips the search: most of what
 * a Cyrillic, Greek or box-drawing line paid per character here. */
static bool cluster_absorb(Terminal *t, u32 cp, int width) {
    /* DEC private mode 2027. Off means the caller wants per-codepoint wcwidth
     * arithmetic, so joiners go back to occupying nothing and each emoji of a
     * ZWJ sequence keeps its own cell — the legacy layout its width
     * calculation assumes. On by default; see terminal_create. */
    if (!(t->mode & MODE_GRAPHEME)) {
        /* A combining mark still belongs to the letter before it — that is
         * what width 0 has always meant. Only the emoji machinery (ZWJ,
         * variation selectors, skin tones, flags) is what the mode turns off;
         * returning false for everything threw the accent away. */
        bool mark = width == 0 && utf8_is_joiner(cp) && cp != 0x200D && cp < 0x1F000 &&
                    !(cp >= 0xFE00 && cp <= 0xFE0F) && cp != 0x20E3;
        if (!mark) return false;
        Cell *base = cluster_cell(t);
        return base ? cluster_extend(t, cp) : false;
    }

    /* Almost every non-ASCII codepoint is a standalone grapheme. A regular
     * predecessor cannot absorb another regular character: only an explicit
     * joiner, a second regional indicator, or a cluster ending in ZWJ can do
     * that. The latter necessarily has a cluster handle, so reject the common
     * case before cluster_cell() and the global cluster-table lookup. This is
     * exact, not a heuristic; the slower checks below remain authoritative for
     * every codepoint that can actually join. */
    bool is_joiner = width == 0 && utf8_is_joiner(cp);
    bool is_regional = cp >= 0x1F1E6 && cp <= 0x1F1FF;
    if (!is_joiner && !is_regional &&
        !utf8_is_cluster(t->last_printed_char)) return false;

    Cell *c = cluster_cell(t);
    if (!c) return false;

    /* Modifiers always attach: VS15/VS16, ZWJ, skin tones, combining marks,
     * keycap enclosures, flag tag characters. */
    if (is_joiner) return cluster_extend(t, cp);

    u32 n = 0;
    const u32 *cps = utf8_cluster_lookup(c->codepoint, &n);

    /* A pending ZWJ means the sequence is mid-join: 👩‍ + 💻 is one glyph. */
    if (cps && n > 0 && cps[n - 1] == 0x200D) return cluster_extend(t, cp);

    /* Flags are pairs of regional indicators — and only pairs, so a third one
     * starts the next flag rather than growing this one. */
    if (!cps && is_regional &&
        c->codepoint >= 0x1F1E6 && c->codepoint <= 0x1F1FF)
        return cluster_extend(t, cp);

    return false;
}

/* Bulk-write a run of printable ASCII (0x20..0x7E) into the CURRENT row.
 *
 * The SIMD scan next to the call site finds where such a run ends; this writes
 * the run without paying put_char's per-character state machine for every byte
 * of it.
 *
 * Everything put_char does per character that does not depend on the character
 * is hoisted out of the loop: the width lookup (always 1 here), the
 * cluster-absorb probe, the row-zone stamp, and TERM_DIRTY_ROW, which would
 * otherwise set the same bit on the same row once per character.
 *
 * Returns how many bytes it wrote. It stops at the right edge rather than
 * wrapping: the caller runs that step through put_char, which owns the
 * soft-wrap flag and the scroll. Returns 0 when the slow path has to keep the
 * run — a sparse underline is per-cell side data and is left to put_char. */
/* A row's soft-wrap flag means "my last cell was written by a character that
 * then overflowed onto the row below". It is SET at the overflow, and it has
 * to go when the text it describes does: `clear` (ED 2), zsh's line editor
 * rewriting the prompt row with EL, or a TUI overwriting a row through CUP
 * would otherwise leave it standing, and every reader of it would join that
 * row to the next one — a copy glued together with a run of blanks, a triple
 * click taking two rows, a double click growing a word across the seam.
 *
 * Two rules keep it true. An erase that reaches the row's END breaks the
 * continuation (ED, EL 0/2, ECH/DCH to the last column). And a character
 * written INTO the last column resets the flag: the wrap re-sets it a moment
 * later if that character really overflows, so a wrapped line is unaffected,
 * while a row repainted end to end by cursor addressing sheds the stale one.
 * Both paths that write cells — put_char and the bulk put_ascii_run — apply
 * the second rule, so the two produce the same cells.
 *
 * The array is shared with the alt screen, which never marks a wrap and whose
 * erases must not wipe the primary screen's flags underneath it. */
static inline void wrap_break_rows(Terminal *t, i32 y0, i32 y1) {
    if (!t->line_wrapped || (t->mode & MODE_ALT_SCREEN)) return;
    if (y0 < 0) y0 = 0;
    if (y1 >= t->rows) y1 = t->rows - 1;
    for (i32 y = y0; y <= y1; y++) t->line_wrapped[y] = false;
}

/* The START of row 0 is gone (erased, or written over from column 0): the
 * newest line of history, if it was continuing onto it, ends where it is. */
static inline void wrap_break_above_screen(Terminal *t) {
    if (!(t->mode & MODE_ALT_SCREEN)) sb_newest_break_wrap(t);
}

static usize put_ascii_run(Terminal *t, const u8 *src, usize len) {
    if (len == 0) return 0;
#ifdef LIU_ASCII_RUN_SWITCH
    /* Switch that turns the bulk ASCII path off, for comparing it with the
     * per-character path. Read once; costs one predictable branch per CHUNK,
     * never per character. */
    extern bool liu_ascii_run_disabled;
    if (liu_ascii_run_disabled) return 0;
#endif
    /* A plain single underline in the default colour writes no entry
     * (terminal_set_underline), so with an empty map there is nothing per
     * cell to record and the run stays on the fast path. */
    if (cell_has_decoration(t->cursor_attr.flags) &&
        (t->cursor_ul_color != 0 || t->cursor_ul_style > 1 || t->ul_map_count > 0))
        return 0;
    /* IRM must shift the remaining row for every character. */
    if (t->mode & MODE_INSERT) return 0;
    if (t->cursor_y < 0 || t->cursor_y >= t->rows) return 0;
    if (t->cursor_x < 0 || t->cursor_x >= t->cols) return 0;

    const i32 room = t->cols - t->cursor_x;
    usize n = len < (usize)room ? len : (usize)room;

    Cell *c = &t->cells[t->cursor_y * t->cols + t->cursor_x];
    const CellAttr attr = t->cursor_attr;

    /* Same accounting as put_char: a repaint rewriting the same glyph is not
     * new output, and Terminal.content_writes feeds the agent-busy heuristic.
     *
     * Only the run's two ends can cut a wide character in half; one that lies
     * wholly inside is overwritten whole. When an end does, the cells are
     * counted BEFORE the pair is split: splitting at the far end blanks the
     * wide character the run is about to cover, which put_char — writing one
     * cell at a time — still compares with what it held, so counting after
     * the split would miss a space written over an emoji. A right half always
     * holds a blank, so the cells as they were are exactly what put_char
     * compares with. */
    u64 changed = 0;
    if ((c[0].attr.flags | c[n - 1].attr.flags) & (ATTR_WIDE | ATTR_WDUMMY)) {
        for (usize i = 0; i < n; i++)
            if (c[i].codepoint != (u32)src[i]) changed++;
        wide_unpair_span(t, t->cursor_y, t->cursor_x, t->cursor_x + (i32)n - 1);
        for (usize i = 0; i < n; i++) {
            c[i].codepoint = src[i];
            c[i].attr = attr;
        }
    } else {
        for (usize i = 0; i < n; i++) {
            const u32 cp = src[i];
            if (c[i].codepoint != cp) changed++;
            c[i].codepoint = cp;
            c[i].attr = attr;
        }
    }
    t->content_writes += changed;

    /* Where the LAST byte landed, so a joiner arriving next folds into it —
     * put_char records the same thing for the single character it wrote. */
    t->cluster_x = t->cursor_x + (i32)n - 1;
    t->cluster_y = t->cursor_y;
    t->last_printed_char = src[n - 1];
    t->cursor_x += (i32)n;
    if (t->cursor_x >= t->cols) {
        wrap_break_rows(t, t->cursor_y, t->cursor_y);
        if (!(t->mode & MODE_WRAP)) t->cursor_x = t->cols - 1;   /* as put_char */
    }

    /* Both are redundant while the caller runs put_char for the first byte of
     * every row chunk — it has already stamped this row. They stay because
     * they are two stores per CHUNK against a per-CHARACTER cost, and because
     * without them this function silently depends on that caller invariant:
     * a row the bulk path started on its own would never be repainted. */
    mark_current_row_zone(t);
    TERM_DIRTY_ROW(t, t->cursor_y);
    return n;
}

static void put_char(Terminal *t, u32 cp) {
    /* One call, reused by the cluster probe and the wide-cell branch below:
     * the width of `cp` cannot change in between. */
    int width = utf8_char_width(cp);
    if (cluster_absorb(t, cp, width)) return;
    /* Joiner with nothing to join (leading VS16, stray ZWJ): it has no cell of
     * its own — dropping it is what every other terminal does too. */
    if (width == 0 && cp != 0) return;

    /* A wide character with ONE column left does not fit: it starts the next
     * row, which is what programs' width arithmetic expects. Squeezed into
     * the last column as a narrow cell, it would cost mixed ASCII + CJK text
     * a column on wrapped lines and hang over the pane's edge. The column it
     * leaves behind holds a spacer — a dummy with no lead — which text
     * readers and the reflow skip, so the gap never becomes a space in a copy
     * or in a rewrapped line. With autowrap off there is no next row: the
     * character takes the last two columns. */
    if (width == 2 && t->cols >= 2 && t->cursor_x == t->cols - 1) {
        if (t->mode & MODE_WRAP) {
            Cell *pad = &t->cells[t->cursor_y * t->cols + t->cursor_x];
            wide_unpair(t, t->cursor_x, t->cursor_y);
            pad->codepoint = ' ';
            pad->attr = t->cursor_attr;
            pad->attr.flags = ATTR_WDUMMY;
            TERM_DIRTY_ROW(t, t->cursor_y);
            t->cursor_x = t->cols;
        } else {
            t->cursor_x = t->cols - 2;
        }
    }

    /* This character is the continuation of a wrap made just below. */
    bool continues = false;
    if (t->cursor_x >= t->cols) {
        if (t->mode & MODE_WRAP) {
            /* Mark the row being left as soft-wrapped: its content continues
             * onto the next physical row (no newline between them). Set this
             * BEFORE cursor_y++/scroll so a row scrolling into the scrollback
             * carries the flag (buffer_scroll_up reads line_wrapped). Without
             * it line_wrapped is never true, so terminal_resize()'s reflow
             * treats every row as a standalone logical line and re-wraps /
             * duplicates long lines on resize.
             *
             * Skip it on the alt screen: line_wrapped is a single array shared
             * with the primary buffer (the alt swap doesn't touch it), it is
             * only consumed by primary-screen reflow/scrollback, and marking it
             * from alt-screen autowrap leaves stale flags that corrupt the next
             * primary reflow. */
            /* Not on the last row under a region either: the cursor stays
             * there (cursor_index), so nothing continues anywhere. */
            if (!(t->mode & MODE_ALT_SCREEN) &&
                t->line_wrapped && t->cursor_y >= 0 && t->cursor_y < t->rows &&
                (t->cursor_y == t->scroll_bottom || t->cursor_y < t->rows - 1))
                t->line_wrapped[t->cursor_y] = true;
            t->cursor_x = 0;
            if (cursor_index(t)) TERM_DIRTY_ALL(t); /* scroll affects all rows */
            continues = true;
        } else {
            t->cursor_x = width == 2 && t->cols >= 2 ? t->cols - 2 : t->cols - 1;
        }
    }

    /* IRM (CSI 4 h): make room instead of overwriting. The rest of the row
     * slides right by the glyph's width and falls off the end — the same shift
     * ICH performs, which is what the mode is defined as. Skipped for a
     * zero-width cell and when the cursor is already at the last column, where
     * there is nothing to move. */
    if ((t->mode & MODE_INSERT) && width > 0 && t->cursor_x < t->cols - 1) {
        i32 n = width;
        if (n > t->cols - t->cursor_x) n = t->cols - t->cursor_x;
        Cell *row = &t->cells[t->cursor_y * t->cols];
        memmove(&row[t->cursor_x + n], &row[t->cursor_x],
                (usize)(t->cols - t->cursor_x - n) * sizeof(Cell));
        if (t->ul_map_count > 0) terminal_underline_shift_cols(t, t->cursor_y, t->cursor_x, n);
        wide_row_repair(t, t->cursor_y);
        TERM_DIRTY_ROW(t, t->cursor_y);
    }

    /* Width (CJK, emoji, …) was computed above. */
    i32 start_x = t->cursor_x;
    Cell *c = &t->cells[t->cursor_y * t->cols + t->cursor_x];
    /* Row 0 written from its start ends the history line above it — unless
     * this character is that line's continuation: in a one-row screen (or a
     * one-row region at the top) every wrap scrolls and lands here. */
    if (t->cursor_x == 0 && t->cursor_y == 0 && !continues) wrap_break_above_screen(t);
    /* What is overwritten may be half of a wide character (wide_unpair). */
    if (c->attr.flags & (ATTR_WIDE | ATTR_WDUMMY)) wide_unpair(t, t->cursor_x, t->cursor_y);
    if (width == 2 && t->cursor_x + 1 < t->cols &&
        (c[1].attr.flags & (ATTR_WIDE | ATTR_WDUMMY)))
        wide_unpair(t, t->cursor_x + 1, t->cursor_y);
    /* Count only real content changes — a repaint rewriting the same glyph is
     * not new output. See Terminal.content_writes. */
    if (c->codepoint != cp) t->content_writes++;
    c->codepoint = cp;
    c->attr = t->cursor_attr;
    mark_current_row_zone(t);

    /* Remembered colors do not belong to undecorated cells. For a decorated
     * overwrite, zero metadata must neutralize any old entry at this position. */
    if (cell_has_decoration(t->cursor_attr.flags))
        terminal_set_underline(t, t->cursor_x, t->cursor_y, t->cursor_ul_color, t->cursor_ul_style);

    if (width == 2 && t->cursor_x + 1 < t->cols) {
        /* Wide character: mark first cell as WIDE, second as dummy */
        c->attr.flags |= ATTR_WIDE;
        Cell *d = &t->cells[t->cursor_y * t->cols + t->cursor_x + 1];
        d->codepoint = ' ';
        d->attr = t->cursor_attr;
        d->attr.flags |= ATTR_WDUMMY;
        if (cell_has_decoration(t->cursor_attr.flags))
            terminal_set_underline(t, t->cursor_x + 1, t->cursor_y,
                                   t->cursor_ul_color, t->cursor_ul_style);
        t->cursor_x += 2;
    } else {
        t->cursor_x++;
    }
    /* Landed in the last column: whatever wrap this row carried described
     * text that is gone now (see wrap_break_rows). */
    if (t->cursor_x >= t->cols) {
        wrap_break_rows(t, t->cursor_y, t->cursor_y);
        /* No autowrap, no pending wrap: the cursor stays ON the last column,
         * so turning DECAWM back on does not wrap the very next character. */
        if (!(t->mode & MODE_WRAP)) t->cursor_x = t->cols - 1;
    }
    t->last_printed_char = cp;
    /* Remember where it landed so a joiner arriving next can fold into it. */
    t->cluster_x = start_x;
    t->cluster_y = t->cursor_y;
    TERM_DIRTY_ROW(t, t->cursor_y);
}

/* =========================================================================
 * Base64 decoding (inline, for Liu/inline image payloads)
 * ========================================================================= */

static const u8 b64_table[256] = {
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,
    52,53,54,55,56,57,58,59,60,61,64,64,64,65,64,64,
    64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
    64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
};

/* How many bytes base64_decode_cap() will produce for this text: one per 8
 * bits of the characters it does not skip. Lets a caller allocate EXACTLY the
 * decoded size rather than the size of the base64 text, a third more: that
 * allocation becomes the image's pixels, and the ledger counts only the
 * pixels. */
static i32 base64_decoded_len(const u8 *src, i32 src_len) {
    i64 valid = 0;
    for (i32 i = 0; i < src_len; i++)
        if (b64_table[src[i]] < 64) valid++;
    i64 n = (valid * 6) / 8;
    return n > INT32_MAX ? INT32_MAX : (i32)n;
}

/* Decode base64 (lenient: padding, whitespace and invalid bytes are skipped).
 * Never writes past `dst_cap`. Returns the decoded byte count, which is the
 * size base64_decoded_len() promised when the buffer was sized from it. */
static i32 base64_decode_cap(const u8 *src, i32 src_len, u8 *dst, i32 dst_cap) {
    i32 out = 0;
    u32 accum = 0;
    i32 bits = 0;
    for (i32 i = 0; i < src_len; i++) {
        u8 v = b64_table[src[i]];
        if (v >= 64) continue; /* skip padding, whitespace, invalid */
        accum = (accum << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out >= dst_cap) return out;
            dst[out++] = (u8)(accum >> bits);
        }
    }
    return out;
}

/* For callers that sized `dst` from the text (one byte per character is more
 * than enough). */
static i32 base64_decode(const u8 *src, i32 src_len, u8 *dst) {
    return base64_decode_cap(src, src_len, dst, INT32_MAX);
}

/* =========================================================================
 * Pre-decode dimension gate
 *
 * Every path below hands attacker-controlled bytes to stb_image, and
 * stbi_load* allocates w*h*4 *before* it returns. Validating dimensions on the
 * result is therefore far too late: a small PNG declaring a 16384x16384 canvas
 * would take about 1 GB and stall the main thread before the check that
 * rejects it could run. stbi_info / stbi_info_from_memory parse only the
 * header, with no pixel allocation, so the same file is refused while it is
 * still small.
 *
 * `max_dim` bounds each edge and `max_bytes` bounds the decoded RGBA footprint;
 * both must pass. Formats stb cannot identify are refused, which is the same
 * answer stbi_load would eventually have given.
 * ========================================================================= */

/* Sub-second mtime field name differs across platforms; both spellings resolve
 * to the same nanoseconds value. */
#if defined(__APPLE__)
#define INLINE_IMG_MTIME_NSEC(st) ((st).st_mtimespec.tv_nsec)
#elif defined(st_mtime) || defined(__linux__)
#define INLINE_IMG_MTIME_NSEC(st) ((st).st_mtim.tv_nsec)
#else
#define INLINE_IMG_MTIME_NSEC(st) (0)
#endif

static bool img_dims_within(int w, int h, i32 max_dim, usize max_bytes) {
    usize pixels, bytes;
    return w > 0 && h > 0 && w <= max_dim && h <= max_dim &&
           checked_mul_usize((usize)w, (usize)h, &pixels) &&
           checked_mul_usize(pixels, 4u, &bytes) &&
           bytes <= max_bytes;
}

static bool img_dims_ok_mem(const u8 *buf, i32 len, i32 max_dim, usize max_bytes) {
    int w = 0, h = 0, comp = 0;
    if (!buf || len <= 0) return false;
    /* Every terminal image protocol decodes through this gate, so the format
     * allow-list lives here: PSD/PIC/PNM/TGA are not something a program prints
     * (see img_is_terminal_format). */
    if (!img_is_terminal_format(buf, (usize)len)) return false;
    if (!stbi_info_from_memory(buf, len, &w, &h, &comp)) return false;
    return img_dims_within(w, h, max_dim, max_bytes);
}

/* =========================================================================
 * Async inline-image decode
 *
 * PTY parsing remains main-thread-owned, but base64 + stb + premultiply can be
 * tens of milliseconds and is attacker-triggerable through SSH output. Each
 * terminal owns a small queue/worker so destruction can join it safely; one
 * process-global mutex serialises the heavy decode itself, bounding aggregate
 * transient RGBA even when several panes receive images at once.
 * ========================================================================= */
#define TERM_IMAGE_JOB_CAP 4
#define TERM_IMAGE_JOB_BYTES ((usize)32 * 1024 * 1024)
#define TERM_IMAGE_DECODE_BYTES ((usize)32 * 1024 * 1024)

typedef struct {
    u8   *b64;
    i32   b64_len;
    u32   format;
    i32   src_w, src_h;
    i32   col, row, cols, rows;
    u32   image_id, placement_id;
    i32   z_index;
    i64   abs_line;
    u64   epoch;
    bool  alt;
    /* Set by a delete-by-id that arrived while this job was queued or being
     * decoded. The terminal-wide `epoch` only moves for delete-ALL, so without
     * this a job named by nothing else would come back after `a=d,d=i` and put
     * the image back. */
    bool  cancelled;
} TermImageJob;

typedef struct {
    u8   *pixels;
    i32   w, h;
    i32   col, row, cols, rows;
    u32   image_id, placement_id;
    i32   z_index;
    usize transient_bytes;
    i64 abs_line;
    u64 epoch;
    bool alt;
    bool cancelled;      /* deleted by id while it was decoding: free, do not publish */
} TermImageDone;

typedef struct {
    pthread_t       thread;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool            stop;
    TermImageJob    jobs[TERM_IMAGE_JOB_CAP];
    u32             job_head, job_tail;
    usize           queued_bytes;
    TermImageDone   done[TERM_IMAGE_JOB_CAP];
    u32             done_head, done_tail;
    _Atomic bool    done_ready;
    /* The job the worker holds right now — out of the queue, not yet in the
     * done queue — so a delete can still reach it. */
    bool            busy_active;
    u32             busy_id;
    bool            busy_cancelled;
} TermImageAsync;

static pthread_mutex_t g_term_image_decode_mu = PTHREAD_MUTEX_INITIALIZER;
static inline u32 tiq_next(u32 i) { return (i + 1u) % TERM_IMAGE_JOB_CAP; }

/* Sixel uses the same bounded worker queue as other image protocols. The
 * work cap also bounds repeated repainting of the same small rectangle. */
static TermImageDone term_sixel_decode(TermImageJob *j) {
    TermImageDone d = {.col=j->col, .row=j->row, .abs_line=j->abs_line, .epoch=j->epoch, .alt=j->alt};
    const u8 *dcs_data = j->b64;
    i32 dcs_len2 = j->b64_len, data_start = 0;
                        /* Parse sixel data into RGBA bitmap */
                        /* No max_x: the stored width must be the buffer stride
                         * (tw), never the measured extent. */
                        i32 sx = 0, sy = 0, max_y = 0;
                        u8 palette[256][3]; /* RGB palette */
                        memset(palette, 0, sizeof(palette));
                        /* Default VGA palette */
                        palette[0][0]=0;   palette[0][1]=0;   palette[0][2]=0;
                        palette[1][0]=0;   palette[1][1]=0;   palette[1][2]=255;
                        palette[2][0]=255; palette[2][1]=0;   palette[2][2]=0;
                        palette[3][0]=0;   palette[3][1]=255; palette[3][2]=0;
                        palette[4][0]=0;   palette[4][1]=255; palette[4][2]=255;
                        palette[5][0]=255; palette[5][1]=0;   palette[5][2]=255;
                        palette[6][0]=255; palette[6][1]=255; palette[6][2]=0;
                        palette[7][0]=255; palette[7][1]=255; palette[7][2]=255;
                        i32 cur_color = 7;

                        /* First pass: determine image dimensions */
                        i32 tw = 0, th = 0;
                        usize work = 0;
                        { i32 px = 0, py = 0;
                          for (i32 si = data_start; si < dcs_len2; si++) {
                              u8 c = dcs_data[si];
                              if (c >= 0x3F && c <= 0x7E) {
                                  if (++work > 8u * 1024u * 1024u) return d;
                                  px = add_bounded(px, 1, 2049);
                                  if (px > tw) tw = px;
                                  i32 row_end = add_bounded(py, 6, 2049);
                                  if (row_end > th) th = row_end;
                              }
                              else if (c == '$') px = 0;
                              else if (c == '-') {
                                  px = 0;
                                  py = add_bounded(py, 6, 2049);
                              }
                              else if (c == '!') { /* repeat: !<count><sixel> */
                                  i32 rep = 0; si++;
                                  while (si < dcs_len2 && dcs_data[si] >= '0' && dcs_data[si] <= '9')
                                      rep = decimal_push_bounded(rep,
                                                                 dcs_data[si++],
                                                                 2049);
                                  /* `si` now sits on the byte the run repeats,
                                   * and the loop's own si++ consumes it.
                                   *
                                   * That byte is not also fed to the
                                   * plain-sixel branch above: pass one has to
                                   * count the run as exactly rep columns, as
                                   * pass two advances, or the buffer is
                                   * allocated at one width and handed to the
                                   * renderer at another, which shears every
                                   * row. So the height bookkeeping that branch
                                   * would do happens here.
                                   *
                                   * The 0x3F..0x7E guard mirrors pass two,
                                   * which only advances its cursor when the
                                   * repeated byte really is a sixel byte — the
                                   * two passes have to agree on malformed input
                                   * as well as valid input. */
                                  if (si < dcs_len2 &&
                                      dcs_data[si] >= 0x3F && dcs_data[si] <= 0x7E) {
                                      work += (usize)rep;
                                      if (work > 8u * 1024u * 1024u) return d;
                                      px = add_bounded(px, rep, 2049);
                                      if (px > tw) tw = px;
                                      i32 row_end = add_bounded(py, 6, 2049);
                                      if (row_end > th) th = row_end;
                                  }
                              }
                          }
                        }
                        if (tw > 0 && th > 0 && tw <= 2048 && th <= 2048) {
                            u8 *pixels = calloc((usize)tw * (usize)th, 4);
                            if (pixels) {
                                /* Second pass: decode pixels */
                                for (i32 si = data_start; si < dcs_len2; si++) {
                                    u8 c = dcs_data[si];
                                    if (c == '#') {
                                        /* Color command: #idx;type;r;g;b */
                                        si++;
                                        i32 idx = 0;
                                        while (si < dcs_len2 && dcs_data[si] >= '0' && dcs_data[si] <= '9')
                                            idx = decimal_push_bounded(idx,
                                                                       dcs_data[si++],
                                                                       256);
                                        if (si < dcs_len2 && dcs_data[si] == ';') {
                                            si++; /* type */
                                            i32 type = 0;
                                            while (si < dcs_len2 && dcs_data[si] >= '0' && dcs_data[si] <= '9')
                                                type = decimal_push_bounded(type,
                                                                            dcs_data[si++],
                                                                            3);
                                            i32 rgb[3] = {0};
                                            for (i32 ci2 = 0; ci2 < 3 && si < dcs_len2; ci2++) {
                                                if (dcs_data[si] == ';') si++;
                                                while (si < dcs_len2 && dcs_data[si] >= '0' && dcs_data[si] <= '9')
                                                    rgb[ci2] = decimal_push_bounded(
                                                        rgb[ci2], dcs_data[si++], 101);
                                            }
                                            if (type == 2 && idx < 256 &&
                                                rgb[0] <= 100 && rgb[1] <= 100 &&
                                                rgb[2] <= 100) {
                                                palette[idx][0] = (u8)(rgb[0] * 255 / 100);
                                                palette[idx][1] = (u8)(rgb[1] * 255 / 100);
                                                palette[idx][2] = (u8)(rgb[2] * 255 / 100);
                                            }
                                        }
                                        cur_color = idx & 0xFF;
                                        si--;
                                    } else if (c >= 0x3F && c <= 0x7E) {
                                        /* Sixel data byte: 6 vertical pixels */
                                        u8 bits = c - 0x3F;
                                        for (i32 bit = 0; bit < 6; bit++) {
                                            if (bits & (1 << bit)) {
                                                i32 py = sy + bit;
                                                if (sx < tw && py < th) {
                                                    i32 pi = (py * tw + sx) * 4;
                                                    pixels[pi+0] = palette[cur_color][0];
                                                    pixels[pi+1] = palette[cur_color][1];
                                                    pixels[pi+2] = palette[cur_color][2];
                                                    pixels[pi+3] = 255;
                                                }
                                            }
                                        }
                                        sx++;
                                        if (sy + 6 > max_y) max_y = sy + 6;
                                    } else if (c == '$') {
                                        sx = 0; /* carriage return */
                                    } else if (c == '-') {
                                        sx = 0;
                                        sy = add_bounded(sy, 6, th); /* newline */
                                    } else if (c == '!') {
                                        /* Repeat: !count<sixel> */
                                        si++; i32 rep = 0;
                                        while (si < dcs_len2 && dcs_data[si] >= '0' && dcs_data[si] <= '9')
                                            rep = decimal_push_bounded(rep,
                                                                       dcs_data[si++],
                                                                       2049);
                                        if (si < dcs_len2) {
                                            u8 sc = dcs_data[si];
                                            if (sc >= 0x3F && sc <= 0x7E) {
                                                u8 bits = sc - 0x3F;
                                                for (i32 ri = 0; ri < rep && sx < tw; ri++) {
                                                    for (i32 bit = 0; bit < 6; bit++) {
                                                        if (bits & (1 << bit)) {
                                                            i32 py = sy + bit;
                                                            if (sx < tw && py < th) {
                                                                i32 pi = (py * tw + sx) * 4;
                                                                pixels[pi+0] = palette[cur_color][0];
                                                                pixels[pi+1] = palette[cur_color][1];
                                                                pixels[pi+2] = palette[cur_color][2];
                                                                pixels[pi+3] = 255;
                                                            }
                                                        }
                                                    }
                                                    sx++;
                                                }
                                                /* Mirror the plain-byte branch.
                                                 * A run advances the cursor just
                                                 * as a bare sixel byte does, so
                                                 * it has to extend the measured
                                                 * extent too — otherwise max_y
                                                 * would track only rows that
                                                 * contain a non-repeated byte,
                                                 * and an image whose final band
                                                 * is pure runs (as libsixel
                                                 * encodes it) would lose its
                                                 * bottom rows. */
                                                if (sy + 6 > max_y) max_y = sy + 6;
                                            }
                                        }
                                    }
                                }
                                /* Store the image at the buffer's OWN geometry.
                                 *
                                 * `pixels` is a tw x th block written with a
                                 * tw-pixel stride, and every consumer treats the
                                 * declared width as the stride: Metal's
                                 * replaceRegion passes bytesPerRow = img_w * 4,
                                 * OpenGL's glTexImage2D assumes tightly packed.
                                 * Passing the *measured* extent here would make
                                 * the two disagree whenever it falls short of
                                 * tw, shifting every output row by (tw -
                                 * measured width) source pixels — a diagonal
                                 * smear.
                                 *
                                 * Height is safe to trim (rows don't affect
                                 * stride), so a stream that measured taller than
                                 * it drew still gets its trailing blank band
                                 * dropped. */
                                d.pixels = pixels; d.w = tw;
                                d.h = max_y > 0 ? MIN(max_y, th) : th;
                                d.transient_bytes = (usize)tw * (usize)th * 4;
                                mem_budget_transient_adjust(MEM_IMAGE_CPU, (isize)d.transient_bytes);
                            }
                        }
    return d;
}

static TermImageDone term_image_decode_job(TermImageJob *j) {
    if (j->format == 6) return term_sixel_decode(j);
    TermImageDone d = {
        .col = j->col, .row = j->row, .cols = j->cols, .rows = j->rows,
        .image_id = j->image_id, .placement_id = j->placement_id,
        .z_index = j->z_index, .abs_line=j->abs_line, .epoch=j->epoch, .alt=j->alt,
    };
    /* The binary form of the payload, between the base64 copy (accounted by
     * the caller as job_bytes) and the RGBA (accounted below). It coexists
     * with BOTH — stbi_load_from_memory reads it while allocating the pixels
     * — so it is counted too, and the ledger sees every link in that chain.
     * Bounded by TERM_IMAGE_JOB_BYTES like the copy it decodes from. */
    /* Sized from what the text decodes to, not from the text: exactly the
     * bytes there are, so the allocation, the ledger and — for raw RGBA, where
     * this buffer BECOMES the pixels — the retained image all agree. */
    i32 decoded_need = base64_decoded_len(j->b64, j->b64_len);
    if (decoded_need <= 0) return d;
    u8 *decoded = malloc((usize)decoded_need);
    if (!decoded) return d;
    usize decoded_acct = (usize)decoded_need;
    mem_budget_transient_adjust(MEM_IMAGE_CPU, (isize)decoded_acct);
    /* Every `VT_DECODED_RELEASE(); free(decoded)` below must be paired with the release; the format
     * 32 path hands the buffer on as `pixels` instead, so it releases the
     * accounting there and lets d.transient_bytes take over. */
    #define VT_DECODED_RELEASE() \
        do { if (decoded_acct) { \
                 mem_budget_transient_adjust(MEM_IMAGE_CPU, -(isize)decoded_acct); \
                 decoded_acct = 0; } } while (0)
    i32 decoded_len = base64_decode_cap(j->b64, j->b64_len, decoded, decoded_need);
    if (decoded_len <= 0) { VT_DECODED_RELEASE(); free(decoded); return d; }

    u8 *pixels = NULL;
    i32 w = 0, h = 0;
    if (j->format == 100) {
        if (!img_dims_ok_mem(decoded, decoded_len, 4096,
                             TERM_IMAGE_DECODE_BYTES)) {
            VT_DECODED_RELEASE(); free(decoded); return d;
        }
        int comp = 0;
        (void)stbi_info_from_memory(decoded, decoded_len, &w, &h, &comp);
        usize rgba_bytes = 0;
        if (!img_rgba_bytes_checked(w, h, &rgba_bytes)) { VT_DECODED_RELEASE(); free(decoded); return d; }
        d.transient_bytes = rgba_bytes;
        mem_budget_transient_adjust(MEM_IMAGE_CPU, (isize)rgba_bytes);
        pixels = stbi_load_from_memory(decoded, decoded_len, &w, &h, &comp, 4);
        VT_DECODED_RELEASE(); free(decoded);
        if (!pixels) return d;
        img_premultiply_rgba(pixels, w, h);
    } else if (j->format == 32) {
        usize bytes = 0;
        if (!img_dims_within(j->src_w, j->src_h, 4096,
                             TERM_IMAGE_DECODE_BYTES) ||
            !img_rgba_bytes_checked(j->src_w, j->src_h, &bytes) ||
            (usize)decoded_len != bytes) {      /* exactly w*h*4: short or long is not this image */
            VT_DECODED_RELEASE(); free(decoded); return d;
        }
        /* This format hands the buffer ON as the pixels rather than freeing
         * it, so the intermediate accounting has to be handed on too —
         * d.transient_bytes takes over the same memory. Counting both would
         * report it twice. */
        w = j->src_w; h = j->src_h; pixels = decoded;
        d.transient_bytes = bytes;
        mem_budget_transient_adjust(MEM_IMAGE_CPU, (isize)bytes);
        VT_DECODED_RELEASE();
        img_premultiply_rgba(pixels, w, h);
    } else if (j->format == 24) {
        usize px_count = 0, rgb_bytes = 0;
        if (!img_dims_within(j->src_w, j->src_h, 4096,
                             TERM_IMAGE_DECODE_BYTES) ||
            !checked_mul_usize((usize)j->src_w, (usize)j->src_h, &px_count) ||
            !checked_mul_usize(px_count, 3u, &rgb_bytes) ||
            (usize)decoded_len != rgb_bytes) {
            VT_DECODED_RELEASE(); free(decoded); return d;
        }
        pixels = malloc(px_count * 4u);
        if (!pixels) { VT_DECODED_RELEASE(); free(decoded); return d; }
        d.transient_bytes = px_count * 4u;
        mem_budget_transient_adjust(MEM_IMAGE_CPU, (isize)d.transient_bytes);
        for (usize i = 0; i < px_count; i++) {
            pixels[i*4+0] = decoded[i*3+0];
            pixels[i*4+1] = decoded[i*3+1];
            pixels[i*4+2] = decoded[i*3+2];
            pixels[i*4+3] = 255;
        }
        VT_DECODED_RELEASE(); free(decoded); w = j->src_w; h = j->src_h;
    } else {
        VT_DECODED_RELEASE(); free(decoded); return d;
    }
    if (!img_dims_within(w, h, 4096, TERM_IMAGE_DECODE_BYTES)) {
        /* d.transient_bytes stays set: the caller releases it for a job that
         * came back with no pixels, which is the same path a failed decode
         * takes. VT_DECODED_RELEASE is already spent by every branch that
         * reaches here. */
        free(pixels); return d;
    }
    d.pixels = pixels; d.w = w; d.h = h;
    return d;
    #undef VT_DECODED_RELEASE
}

static void term_image_job_free(TermImageJob *j) {
    if (!j) return;
    free(j->b64);
    memset(j, 0, sizeof(*j));
}

static void *term_image_worker(void *arg) {
    TermImageAsync *a = arg;
#ifdef __APPLE__
    pthread_setname_np("liu-term-image");
#else
    pthread_setname_np(pthread_self(), "liu-term-image");
#endif
    for (;;) {
        pthread_mutex_lock(&a->mu);
        while (!a->stop && a->job_head == a->job_tail)
            pthread_cond_wait(&a->cv, &a->mu);
        if (a->stop) { pthread_mutex_unlock(&a->mu); break; }
        TermImageJob job = a->jobs[a->job_head];
        i32 job_bytes = job.b64_len;
        memset(&a->jobs[a->job_head], 0, sizeof(a->jobs[a->job_head]));
        a->job_head = tiq_next(a->job_head);
        a->busy_active = true;
        a->busy_id = job.image_id;
        a->busy_cancelled = job.cancelled;
        pthread_mutex_unlock(&a->mu);

        TermImageDone done;
        if (job.cancelled) {
            /* Deleted before it started: nothing to decode. */
            memset(&done, 0, sizeof done);
            done.cancelled = true;
        } else {
            pthread_mutex_lock(&g_term_image_decode_mu);
            done = term_image_decode_job(&job);
            pthread_mutex_unlock(&g_term_image_decode_mu);
        }
        term_image_job_free(&job);
        if (job_bytes > 0)
            mem_budget_transient_adjust(MEM_IMAGE_CPU, -(isize)job_bytes);

        pthread_mutex_lock(&a->mu);
        if (job_bytes > 0 && a->queued_bytes >= (usize)job_bytes)
            a->queued_bytes -= (usize)job_bytes;
        else a->queued_bytes = 0;
        while (!a->stop && tiq_next(a->done_tail) == a->done_head)
            pthread_cond_wait(&a->cv, &a->mu);
        if (a->stop) {
            pthread_mutex_unlock(&a->mu);
            free(done.pixels);
            if (done.transient_bytes)
                mem_budget_transient_adjust(MEM_IMAGE_CPU,
                                            -(isize)done.transient_bytes);
            break;
        }
        /* A delete that came in while the decode ran, or while this thread
         * waited for room above, marks the job in hand — read it here, under
         * the lock the store happens under. */
        if (a->busy_cancelled) done.cancelled = true;
        a->busy_active = false;
        a->busy_cancelled = false;
        a->done[a->done_tail] = done;
        a->done_tail = tiq_next(a->done_tail);
        atomic_store_explicit(&a->done_ready, true, memory_order_release);
        pthread_mutex_unlock(&a->mu);
        if (platform_wake_event_loop) platform_wake_event_loop();
    }
    return NULL;
}

static TermImageAsync *term_image_async_get(Terminal *t) {
    TermImageAsync *a = t ? t->image_async : NULL;
    if (a) return a;
    a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    pthread_mutex_init(&a->mu, NULL);
    pthread_cond_init(&a->cv, NULL);
    atomic_init(&a->done_ready, false);
    if (pthread_create(&a->thread, NULL, term_image_worker, a) != 0) {
        pthread_cond_destroy(&a->cv); pthread_mutex_destroy(&a->mu); free(a);
        return NULL;
    }
    t->image_async = a;
    return a;
}

static bool term_image_enqueue_b64(Terminal *t, const u8 *b64, i32 b64_len,
                                   u32 format, i32 src_w, i32 src_h,
                                   i32 cols, i32 rows, u32 image_id,
                                   u32 placement_id, i32 z_index) {
    if (!t || !b64 || b64_len <= 0 || (usize)b64_len > TERM_IMAGE_JOB_BYTES)
        return false;
    TermImageAsync *a = term_image_async_get(t);
    if (!a) return false;
    u8 *copy = malloc((usize)b64_len);
    if (!copy) return false;
    memcpy(copy, b64, (usize)b64_len);

    pthread_mutex_lock(&a->mu);
    u32 next = tiq_next(a->job_tail);
    if (next == a->job_head ||
        a->queued_bytes > TERM_IMAGE_JOB_BYTES - (usize)b64_len) {
        pthread_mutex_unlock(&a->mu); free(copy); return false;
    }
    TermImageJob *j = &a->jobs[a->job_tail];
    *j = (TermImageJob){
        .b64 = copy, .b64_len = b64_len, .format = format,
        .src_w = src_w, .src_h = src_h,
        .col = t->cursor_x, .row = t->cursor_y, .cols = cols, .rows = rows,
        .image_id = image_id, .placement_id = placement_id, .z_index = z_index,
        .epoch=t->image_epoch, .alt=(t->mode & MODE_ALT_SCREEN)!=0,
        .abs_line=t->sb_abs_base+(i64)t->sb_count+t->cursor_y,
    };
    a->job_tail = next;
    a->queued_bytes += (usize)b64_len;
    mem_budget_transient_adjust(MEM_IMAGE_CPU, (isize)b64_len);
    pthread_cond_signal(&a->cv);
    pthread_mutex_unlock(&a->mu);
    return true;
}

/* A delete by image id reaches every stage a transmission can be in: queued,
 * in the worker's hands, or finished and waiting for the pump. The slot that
 * already exists is the caller's business (terminal_delete_image); this is the
 * part of the id that has not become a slot yet. */
static void term_image_cancel_id(Terminal *t, u32 image_id) {
    TermImageAsync *a = t ? t->image_async : NULL;
    if (!a) return;
    pthread_mutex_lock(&a->mu);
    for (u32 i = a->job_head; i != a->job_tail; i = tiq_next(i))
        if (a->jobs[i].image_id == image_id) a->jobs[i].cancelled = true;
    if (a->busy_active && a->busy_id == image_id) a->busy_cancelled = true;
    for (u32 i = a->done_head; i != a->done_tail; i = tiq_next(i))
        if (a->done[i].image_id == image_id) a->done[i].cancelled = true;
    pthread_mutex_unlock(&a->mu);
}

bool terminal_image_async_pending(const Terminal *t) {
    TermImageAsync *a = t ? t->image_async : NULL;
    if (!a) return false;
    pthread_mutex_lock(&a->mu);
    bool pending = a->queued_bytes != 0 || a->done_head != a->done_tail;
    pthread_mutex_unlock(&a->mu);
    return pending;
}

bool terminal_image_async_completion_ready(const Terminal *t) {
    const TermImageAsync *a = t ? t->image_async : NULL;
    return a && atomic_load_explicit(&a->done_ready, memory_order_acquire);
}

bool terminal_image_async_pump(Terminal *t) {
    TermImageAsync *a = t ? t->image_async : NULL;
    if (!a || !atomic_load_explicit(&a->done_ready, memory_order_acquire))
        return false;
    TermImageDone local[TERM_IMAGE_JOB_CAP];
    i32 n = 0;
    pthread_mutex_lock(&a->mu);
    while (a->done_head != a->done_tail && n < TERM_IMAGE_JOB_CAP) {
        local[n++] = a->done[a->done_head];
        memset(&a->done[a->done_head], 0, sizeof(a->done[a->done_head]));
        a->done_head = tiq_next(a->done_head);
    }
    atomic_store_explicit(&a->done_ready, a->done_head != a->done_tail,
                          memory_order_release);
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);

    bool changed = false;
    for (i32 i = 0; i < n; i++) {
        TermImageDone *d = &local[i];
        if (d->transient_bytes)
            mem_budget_transient_adjust(MEM_IMAGE_CPU,
                                        -(isize)d->transient_bytes);
        if (!d->pixels) continue;
        if (d->cancelled ||
            d->epoch != t->image_epoch || d->alt != ((t->mode & MODE_ALT_SCREEN)!=0)) {
            free(d->pixels); continue;
        }
        i32 slot = terminal_add_image(t, d->pixels, d->w, d->h,
                                      d->col, d->row, d->cols, d->rows,
                                      d->image_id, d->placement_id);
        if (slot >= 0) { t->images[slot].z_index = d->z_index;
            t->images[slot].abs_line=d->alt ? -1 : d->abs_line; changed = true; }
        d->pixels = NULL; /* terminal_add_image consumes success and failure */
    }
    return changed;
}

void terminal_image_async_shutdown(Terminal *t) {
    TermImageAsync *a = t ? t->image_async : NULL;
    if (!a) return;
    pthread_mutex_lock(&a->mu);
    a->stop = true;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
    pthread_join(a->thread, NULL);
    for (u32 i = a->job_head; i != a->job_tail; i = tiq_next(i)) {
        if (a->jobs[i].b64_len > 0)
            mem_budget_transient_adjust(MEM_IMAGE_CPU,
                                        -(isize)a->jobs[i].b64_len);
        term_image_job_free(&a->jobs[i]);
    }
    for (u32 i = a->done_head; i != a->done_tail; i = tiq_next(i)) {
        free(a->done[i].pixels);
        if (a->done[i].transient_bytes)
            mem_budget_transient_adjust(MEM_IMAGE_CPU,
                                        -(isize)a->done[i].transient_bytes);
    }
    pthread_cond_destroy(&a->cv); pthread_mutex_destroy(&a->mu);
    free(a); t->image_async = NULL;
}

/* =========================================================================
 * Liu graphics protocol handler (APC-based)
 * ========================================================================= */

/* The reply to a graphics command, and ONLY to one that asked for it.
 *
 * The graphics protocol answers a command only when it carried an explicit
 * non-zero image id (i=). Answering every command would let anything that
 * prints untrusted text make the terminal write `ESC _ G i=777 ; ENODATA ESC \`
 * into the shell's input — attacker digits, a ';' and a status word delivered
 * to the line editor (a vi-mode zsh turns the leading ESC into a mode switch,
 * bash and zsh split the line at the ';'). So a reply must be solicited: no id,
 * no reply, and a malformed command never gets an error answer, which is also
 * the VT100 state machine's rule for invalid input. */
static void gfx_ack(Terminal *t, u32 image_id, const char *status) {
    if (image_id == 0) return;
    emit_response_fmt(t, "\x1b_Gi=%u;%s\x1b\\", image_id, status);
}

static void handle_Liu_graphics(Terminal *t, const u8 *data, i32 len) {
    /* Format: G<key=value,...>;<base64 payload>
     * The 'G' prefix has been stripped before calling this function. */

    /* Find the semicolon separating header from payload */
    i32 semi_pos = -1;
    for (i32 i = 0; i < len; i++) {
        if (data[i] == ';') { semi_pos = i; break; }
    }

    /* Parse header key=value pairs */
    u32 action = 't';   /* default: transmit */
    u32 format = 32;    /* default: RGBA */
    u32 transmission = 'd'; /* default: direct */
    i32 src_w = 0, src_h = 0;
    i32 disp_cols = 0, disp_rows = 0;
    u32 image_id = 0;
    u32 placement_id = 0;
    i32 z_index = 0;
    bool quiet = false;
    u32 delete_what = 0;
    bool more_chunks = false;
    bool saw_m = false;          /* the command carries an `m` key */
    bool only_piece_keys = true; /* ...and nothing but `m` (and `q`) */

    i32 hdr_end = (semi_pos >= 0) ? semi_pos : len;
    {
        i32 hi = 0;
        while (hi < hdr_end) {
            /* Parse key */
            u8 key = 0;
            if (hi < hdr_end) key = data[hi++];
            /* Skip '=' */
            if (hi < hdr_end && data[hi] == '=') hi++;
            /* Parse value (could be string or integer) */
            i32 val = 0;
            i32 val_start = hi;
            while (hi < hdr_end && data[hi] != ',') {
                if (data[hi] >= '0' && data[hi] <= '9')
                    val = decimal_push_bounded(val, data[hi], INT32_MAX);
                hi++;
            }
            if (hi < hdr_end && data[hi] == ',') hi++;

            switch (key) {
            case 'a': action = (hi > val_start + 1) ? (u32)data[val_start] : (u32)'t'; break;
            case 'f': format = (u32)val; break;
            case 't': transmission = (hi > val_start + 1) ? (u32)data[val_start] : (u32)'d'; break;
            case 's': src_w = val; break;
            case 'v': src_h = val; break;
            case 'c': disp_cols = val; break;
            case 'r': disp_rows = val; break;
            case 'i': image_id = (u32)val; break;
            case 'p': placement_id = (u32)val; break;
            case 'z': z_index = val; break;
            case 'q': quiet = (val >= 1); break;
            case 'm': more_chunks = (val == 1); saw_m = true; break;
            case 'd': delete_what = (hi > val_start + 1) ? (u32)data[val_start] : 0; break;
            }
            if (key != 'm' && key != 'q' && key != 0) only_piece_keys = false;
        }
    }

    /* A transfer in pieces is waiting for its next one.
     *
     * A piece is a command that carries `m` and no control key of its own —
     * that is what every sender emits after the first command. It is appended,
     * and with m=0 the whole payload goes down the same path a single command
     * takes, under the FIRST piece's keys.
     *
     * Anything else ends the transfer: it is a new command, so the sender of
     * the unfinished one is gone or broke the protocol. Without this a program
     * killed mid-image would leave its pieces to be glued onto the front of
     * the next image anyone sent. */
    TermGfxTransfer *xfer = (TermGfxTransfer *)t->gfx_transfer;
    if (xfer) {
        if (saw_m && only_piece_keys) {
            if (semi_pos >= 0 && semi_pos + 1 < len)
                gfx_transfer_append(xfer, data + semi_pos + 1, len - semi_pos - 1);
            if (more_chunks) return;                       /* more to come */
            const char *status = xfer->err;
            if (!status) {
                bool queued = xfer->len > 0 &&
                    term_image_enqueue_b64(t, xfer->b64, xfer->len, xfer->format,
                                           xfer->src_w, xfer->src_h,
                                           xfer->disp_cols, xfer->disp_rows,
                                           xfer->image_id, xfer->placement_id,
                                           xfer->z_index);
                status = queued ? "OK" : (xfer->len > 0 ? "ENOMEM" : "ENODATA");
            }
            if (!xfer->quiet) gfx_ack(t, xfer->image_id, status);
            gfx_transfer_free(t);
            return;
        }
        gfx_transfer_free(t);
    }

    /* Handle delete action */
    if (action == 'd') {
        if (delete_what == 'a' || delete_what == 'A') {
            /* Delete all images */
            terminal_clear_images(t);
        } else if (delete_what == 'i' || delete_what == 'I') {
            /* Delete by image ID: the slots, and the transmissions of that
             * id still on their way to becoming one. */
            term_image_cancel_id(t, image_id);
            terminal_delete_image(t, image_id);
        } else {
            /* Delete all visible */
            terminal_clear_images(t);
        }
        /* Send OK response */
        if (!quiet)
            gfx_ack(t, image_id, "OK");
        return;
    }

    (void)transmission; /* file/temp transmission types not yet supported */

    /* Only handle transmit and transmit+display */
    if (action != 't' && action != 'T') {
        if (!quiet)
            gfx_ack(t, image_id, "OK");
        return;
    }

    /* The first piece of a transmission sent in pieces (m=1): keep its keys
     * and what it carried, and wait. Nothing is decoded, and nothing is
     * answered, until the last piece. A first piece nothing can come of — a
     * format Liu does not take — is remembered as failed rather than refused
     * here, so its remaining pieces are swallowed instead of each being read
     * as a command of its own. */
    if (more_chunks) {
        TermGfxTransfer *nx = calloc(1, sizeof *nx);
        if (!nx) return;
        nx->format = format;
        nx->src_w = src_w; nx->src_h = src_h;
        nx->disp_cols = disp_cols; nx->disp_rows = disp_rows;
        nx->image_id = image_id; nx->placement_id = placement_id;
        nx->z_index = z_index;
        nx->quiet = quiet;
        if (format != 100 && format != 32 && format != 24) nx->err = "EFORMAT";
        t->gfx_transfer = nx;
        if (semi_pos >= 0 && semi_pos + 1 < len)
            gfx_transfer_append(nx, data + semi_pos + 1, len - semi_pos - 1);
        return;
    }

    /* Extract and decode payload */
    if (semi_pos < 0 || semi_pos + 1 >= len) {
        if (!quiet)
            gfx_ack(t, image_id, "ENODATA");
        return;
    }

    const u8 *payload = data + semi_pos + 1;
    i32 payload_len = len - semi_pos - 1;

    if (format != 100 && format != 32 && format != 24) {
        if (!quiet) gfx_ack(t, image_id, "EFORMAT");
        return;
    }
    bool queued = term_image_enqueue_b64(t, payload, payload_len, format,
                                          src_w, src_h, disp_cols, disp_rows,
                                          image_id, placement_id, z_index);
    if (!quiet)
        gfx_ack(t, image_id, queued ? "OK" : "ENOMEM");
    return;

    /* Decode base64 payload */
    u8 *decoded = malloc((usize)payload_len); /* decoded is always <= encoded */
    if (!decoded) return;
    i32 decoded_len = base64_decode(payload, payload_len, decoded);
    if (decoded_len <= 0) {
        free(decoded);
        if (!quiet)
            gfx_ack(t, image_id, "ENODATA");
        return;
    }

    /* Decode to RGBA pixels */
    u8 *pixels = NULL;
    i32 img_w = 0, img_h = 0;

    if (format == 100) {
        /* Inspect dimensions before decode — see img_dims_ok_mem. */
        if (!img_dims_ok_mem(decoded, decoded_len, 4096, TERM_IMAGES_BUDGET_BYTES)) {
            free(decoded);
            if (!quiet)
                gfx_ack(t, image_id, "EDIM");
            return;
        }
        /* PNG format — decode with stb_image */
        int comp = 0;
        pixels = stbi_load_from_memory(decoded, decoded_len, &img_w, &img_h, &comp, 4);
        free(decoded);
        if (!pixels) {
            if (!quiet)
                gfx_ack(t, image_id, "EBADPNG");
            return;
        }
        /* stb returns straight alpha; the renderer's image blend is
         * (ONE, 1-SrcAlpha) and consumes premultiplied. */
        img_premultiply_rgba(pixels, img_w, img_h);
    } else if (format == 32) {
        /* Raw RGBA. Bound dimensions BEFORE any size math so src_w*src_h
         * cannot overflow i32 (the 4096 cap matches the dimension check
         * applied to PNG below). */
        if (src_w <= 0 || src_h <= 0 || src_w > 4096 || src_h > 4096 ||
            (i64)decoded_len < (i64)src_w * (i64)src_h * 4) {
            free(decoded);
            if (!quiet)
                gfx_ack(t, image_id, "ENOSIZE");
            return;
        }
        img_w = src_w;
        img_h = src_h;
        pixels = decoded; /* take ownership */
        /* The protocol's f=32 payload is straight-alpha RGBA, so it needs the
         * same conversion as the PNG branch. */
        img_premultiply_rgba(pixels, img_w, img_h);
    } else if (format == 24) {
        /* Raw RGB — convert to RGBA. Bound dimensions BEFORE any size math so
         * src_w*src_h cannot overflow i32; do all index/size math in 64-bit. */
        if (src_w <= 0 || src_h <= 0 || src_w > 4096 || src_h > 4096 ||
            (i64)decoded_len < (i64)src_w * (i64)src_h * 3) {
            free(decoded);
            if (!quiet)
                gfx_ack(t, image_id, "ENOSIZE");
            return;
        }
        img_w = src_w;
        img_h = src_h;
        usize pixel_count = (usize)src_w * (usize)src_h;
        pixels = malloc(pixel_count * 4);
        if (!pixels) { free(decoded); return; }
        for (usize pi = 0; pi < pixel_count; pi++) {
            pixels[pi * 4 + 0] = decoded[pi * 3 + 0];
            pixels[pi * 4 + 1] = decoded[pi * 3 + 1];
            pixels[pi * 4 + 2] = decoded[pi * 3 + 2];
            pixels[pi * 4 + 3] = 255;
        }
        free(decoded);
    } else {
        free(decoded);
        if (!quiet)
            gfx_ack(t, image_id, "EFORMAT");
        return;
    }

    /* Validate dimensions */
    if (img_w <= 0 || img_h <= 0 || img_w > 4096 || img_h > 4096) {
        free(pixels);
        if (!quiet)
            gfx_ack(t, image_id, "EDIM");
        return;
    }

    /* Store image at cursor position */
    i32 slot = terminal_add_image(t, pixels, img_w, img_h,
                                   t->cursor_x, t->cursor_y,
                                   disp_cols, disp_rows,
                                   image_id, placement_id);
    if (slot >= 0) {
        t->images[slot].z_index = z_index;
    }

    /* Send OK response */
    if (!quiet)
        gfx_ack(t, image_id, "OK");
}

/* =========================================================================
 * Inline image handler (OSC 1337)
 * ========================================================================= */

static void handle_iterm2_image(Terminal *t, const u8 *data, i32 len) {
    /* Format: File=<params>:<base64 data>
     * Params: name=<b64>, size=<bytes>, width=<val>, height=<val>, inline=1 */

    /* Skip "File=" prefix */
    const char *str = (const char *)data;
    if (len < 6 || memcmp(str, "File=", 5) != 0) return;
    str += 5;
    len -= 5;

    /* Find colon separating params from data */
    i32 colon_pos = -1;
    for (i32 i = 0; i < len; i++) {
        if (str[i] == ':') { colon_pos = i; break; }
    }
    if (colon_pos < 0) return;

    /* Parse parameters */
    bool is_inline = false;
    i32 width_cells = 0, height_cells = 0;

    {
        i32 pi = 0;
        while (pi < colon_pos) {
            /* Find key */
            i32 key_start = pi;
            while (pi < colon_pos && str[pi] != '=') pi++;
            i32 key_len = pi - key_start;
            if (pi < colon_pos) pi++; /* skip '=' */
            /* Find value */
            i32 val_start = pi;
            while (pi < colon_pos && str[pi] != ';') pi++;
            i32 val_len = pi - val_start;
            if (pi < colon_pos) pi++; /* skip ';' */

            if (key_len == 6 && memcmp(str + key_start, "inline", 6) == 0) {
                is_inline = (val_len == 1 && str[val_start] == '1');
            } else if (key_len == 5 && memcmp(str + key_start, "width", 5) == 0) {
                width_cells = 0;
                for (i32 vi = val_start; vi < val_start + val_len; vi++) {
                    if (str[vi] >= '0' && str[vi] <= '9')
                        width_cells = decimal_push_bounded(width_cells,
                                                           (u8)str[vi],
                                                           1000000);
                    else break; /* stop at non-digit (e.g. "auto", "10px") */
                }
            } else if (key_len == 6 && memcmp(str + key_start, "height", 6) == 0) {
                height_cells = 0;
                for (i32 vi = val_start; vi < val_start + val_len; vi++) {
                    if (str[vi] >= '0' && str[vi] <= '9')
                        height_cells = decimal_push_bounded(height_cells,
                                                            (u8)str[vi],
                                                            1000000);
                    else break;
                }
            }
        }
    }

    if (!is_inline) return; /* Non-inline downloads not supported */

    /* Decode base64 payload */
    const u8 *b64_data = (const u8 *)(str + colon_pos + 1);
    i32 b64_len = len - colon_pos - 1;
    if (b64_len <= 0) return;

    (void)term_image_enqueue_b64(t, b64_data, b64_len, 100, 0, 0,
                                 width_cells, height_cells, 0, 0, 0);
    return;

    u8 *decoded = malloc((usize)b64_len);
    if (!decoded) return;
    i32 decoded_len = base64_decode(b64_data, b64_len, decoded);
    if (decoded_len <= 0) { free(decoded); return; }

    /* Gate on the header before decoding: a small base64 payload declaring
     * 16384x16384 would otherwise make stbi_load_from_memory allocate about 1
     * GB before img_w/img_h could be checked, and anything that can write to
     * the tty (cat, an untrusted SSH host, agent tool output) can send one.
     */
    if (!img_dims_ok_mem(decoded, decoded_len, 4096, TERM_IMAGES_BUDGET_BYTES)) {
        free(decoded);
        return;
    }

    /* Decode image with stb_image (auto-detects PNG/JPEG/etc.) */
    int img_w = 0, img_h = 0, comp = 0;
    u8 *pixels = stbi_load_from_memory(decoded, decoded_len, &img_w, &img_h, &comp, 4);
    free(decoded);

    /* Re-check: a format whose header disagrees with its payload could still
     * come back larger than advertised. */
    if (!pixels || img_w <= 0 || img_h <= 0 || img_w > 4096 || img_h > 4096) {
        if (pixels) free(pixels);
        return;
    }

    /* Straight alpha out of stb -> premultiplied, matching the renderer's
     * (ONE, 1-SrcAlpha) image blend. */
    img_premultiply_rgba(pixels, img_w, img_h);

    /* Store at cursor position */
    terminal_add_image(t, pixels, img_w, img_h,
                       t->cursor_x, t->cursor_y,
                       width_cells, height_cells, 0, 0);
}

/* =========================================================================
 * CSI (Control Sequence Introducer) handler
 * ========================================================================= */

static void handle_csi(Terminal *t, u8 final_byte) {
    i32 p0 = param(t, 0, 1);

    switch (final_byte) {
    /* Cursor movement */
    case 'A': { /* CUU — Cursor Up: stops at the top margin unless it began above it */
        i32 floor_y = t->cursor_y >= t->scroll_top ? t->scroll_top : 0;
        if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
        t->cursor_y = p0 > t->cursor_y - floor_y ? floor_y : t->cursor_y - p0;
        cursor_clamp(t);
        break;
    }
    case 'B': /* CUD — Cursor Down: stops at the bottom margin unless it began below it */
    case 'e': {
        i32 ceil_y = t->cursor_y <= t->scroll_bottom ? t->scroll_bottom : t->rows - 1;
        if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
        t->cursor_y = p0 > ceil_y - t->cursor_y ? ceil_y : t->cursor_y + p0;
        cursor_clamp(t);
        break;
    }
    case 'C': /* CUF — Cursor Forward */
    case 'a':
        t->cursor_x += p0;
        cursor_clamp(t);
        break;
    case 'D': /* CUB — Cursor Back */
        t->cursor_x -= p0;
        cursor_clamp(t);
        break;
    /* Tabulation moves. A line editor that redraws the tail of its input and
     * then walks the cursor back home uses these interchangeably with CUB —
     * Antigravity CLI emits `CSI 30 D` for most keystrokes and `CSI 4 Z` for
     * the ones that happen to land on a tab boundary. Dropping CBT would
     * leave the cursor parked at the end of the text it had just written, so
     * every following redraw would land at the wrong column and the prompt
     * would scramble into duplicated fragments of itself. Both walk the same
     * `tab_stops` array the HT control does, so a `CSI 3 g` (TBC) is honoured
     * here too. The count is capped at `cols`: no tab walk can cross more
     * stops than there are columns, and an untrusted `CSI 99999 Z` must not
     * turn into a 99999 x cols loop. */
    case 'I': { /* CHT — Cursor Forward Tabulation */
        i32 n = p0 > t->cols ? t->cols : p0;
        if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
        for (i32 k = 0; k < n; k++) {
            t->cursor_x++;
            while (t->cursor_x < t->cols && !t->tab_stops[t->cursor_x])
                t->cursor_x++;
            if (t->cursor_x >= t->cols) break;
        }
        cursor_clamp(t);
        break;
    }
    case 'Z': { /* CBT — Cursor Backward Tabulation */
        i32 n = p0 > t->cols ? t->cols : p0;
        /* Deferred wrap parks the cursor at `cols`; a cursor move cancels it,
         * and the array has no such column. */
        if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
        for (i32 k = 0; k < n; k++) {
            if (t->cursor_x <= 0) break;
            t->cursor_x--;
            while (t->cursor_x > 0 && !t->tab_stops[t->cursor_x])
                t->cursor_x--;
        }
        cursor_clamp(t);
        break;
    }
    case 'E': /* CNL — Cursor Next Line */
        t->cursor_y += p0;
        t->cursor_x = 0;
        cursor_clamp(t);
        break;
    case 'F': /* CPL — Cursor Previous Line */
        t->cursor_y -= p0;
        t->cursor_x = 0;
        cursor_clamp(t);
        break;
    case 'G': /* CHA — Cursor Horizontal Absolute */
    case '`':
        t->cursor_x = param(t, 0, 1) - 1;
        cursor_clamp(t);
        break;
    case 'H': /* CUP — Cursor Position */
    case 'f':
        cursor_set_row(t, param(t, 0, 1) - 1);
        t->cursor_x = param(t, 1, 1) - 1;
        cursor_clamp(t);
        break;
    case 'd': /* VPA — Vertical Position Absolute */
        cursor_set_row(t, p0 - 1);
        cursor_clamp(t);
        break;

    /* Erase */
    case 'J': { /* ED — Erase in Display */
        i32 mode = param(t, 0, 0);
        CellAttr a = t->cursor_attr;
        a.flags = 0;
        if (mode == 0 || mode == 1) wide_unpair(t, t->cursor_x, t->cursor_y);
        if (mode == 0 && t->cursor_x == 0 && t->cursor_y == 0) wrap_break_above_screen(t);
        if (mode == 0) { /* cursor to end */
            buffer_clear_region(t->cells, t->cols, t->cursor_x, t->cursor_y, t->cols-1, t->cursor_y, a);
            if (t->cursor_y + 1 < t->rows)
                buffer_clear_region(t->cells, t->cols, 0, t->cursor_y+1, t->cols-1, t->rows-1, a);
            wrap_break_rows(t, t->cursor_y, t->rows - 1);
            for (i32 row = t->cursor_y; row < t->rows; row++) TERM_DIRTY_ROW(t, row);
        } else if (mode == 1) { /* start to cursor */
            buffer_clear_region(t->cells, t->cols, 0, 0, t->cols-1, t->cursor_y-1, a);
            buffer_clear_region(t->cells, t->cols, 0, t->cursor_y, t->cursor_x, t->cursor_y, a);
            /* The cursor row keeps its tail, so it may still continue. */
            wrap_break_rows(t, 0, t->cursor_y - 1);
            wrap_break_above_screen(t);
            for (i32 row = 0; row <= t->cursor_y; row++) TERM_DIRTY_ROW(t, row);
        } else if (mode == 2) { /* entire screen */
            buffer_clear_region(t->cells, t->cols, 0, 0, t->cols-1, t->rows-1, a);
            terminal_clear_underline_map(t);
            wrap_break_rows(t, 0, t->rows - 1);
            wrap_break_above_screen(t);
            TERM_DIRTY_ALL(t);
        } else if (mode == 3) {
            /* Mode 3 is the protocol's ERASE SAVED LINES — the history and
             * nothing on the screen (`clear` sends 2J for that, right before).
             * Only on the primary screen: the alt screen has no scrollback of
             * its own, and a full-screen program clearing itself must not take
             * the shell's history with it. */
            if (!(t->mode & MODE_ALT_SCREEN)) sb_clear(t);
            TERM_DIRTY_ALL(t);
        }
        break;
    }
    case 'K': { /* EL — Erase in Line */
        i32 mode = param(t, 0, 0);
        CellAttr a = t->cursor_attr;
        a.flags = 0;
        /* The erase can start or end in the middle of a wide character. */
        if (mode == 0 || mode == 1) wide_unpair(t, t->cursor_x, t->cursor_y);
        if (mode == 0)
            buffer_clear_region(t->cells, t->cols, t->cursor_x, t->cursor_y, t->cols-1, t->cursor_y, a);
        else if (mode == 1)
            buffer_clear_region(t->cells, t->cols, 0, t->cursor_y, t->cursor_x, t->cursor_y, a);
        else if (mode == 2)
            buffer_clear_region(t->cells, t->cols, 0, t->cursor_y, t->cols-1, t->cursor_y, a);
        /* EL 1 leaves the tail, and with it the continuation. */
        if (mode == 0 || mode == 2) wrap_break_rows(t, t->cursor_y, t->cursor_y);
        if (t->cursor_y == 0 && (mode != 0 || t->cursor_x == 0)) wrap_break_above_screen(t);
        TERM_DIRTY_ROW(t, t->cursor_y);
        break;
    }

    /* Scroll */
    case 'S': /* SU — Scroll Up */
        buffer_scroll_up(t, p0);
        TERM_DIRTY_ALL(t);
        break;
    case 'T': /* SD — Scroll Down */
        buffer_scroll_down(t, p0);
        TERM_DIRTY_ALL(t);
        break;

    /* Insert/Delete */
    case 'L': { /* IL — Insert Lines */
        i32 n = p0;
        i32 save_top = t->scroll_top;
        t->scroll_top = t->cursor_y;
        buffer_scroll_down(t, n);
        t->scroll_top = save_top;
        TERM_DIRTY_ALL(t);
        break;
    }
    case 'M': { /* DL — Delete Lines */
        i32 n = p0;
        i32 save_top = t->scroll_top;
        t->scroll_top = t->cursor_y;
        buffer_scroll_up(t, n);
        t->scroll_top = save_top;
        TERM_DIRTY_ALL(t);
        break;
    }
    case '@': { /* ICH — Insert Characters */
        i32 n = p0;
        if (n > t->cols - t->cursor_x) n = t->cols - t->cursor_x;
        Cell *row = &t->cells[t->cursor_y * t->cols];
        memmove(&row[t->cursor_x + n], &row[t->cursor_x],
                (usize)(t->cols - t->cursor_x - n) * sizeof(Cell));
        if (t->ul_map_count > 0) terminal_underline_shift_cols(t, t->cursor_y, t->cursor_x, n);
        Cell blank = { .codepoint = ' ', .attr = t->cursor_attr };
        blank.attr.flags = 0;
        for (i32 i = 0; i < n; i++) row[t->cursor_x + i] = blank;
        wide_row_repair(t, t->cursor_y);
        TERM_DIRTY_ROW(t, t->cursor_y);
        break;
    }
    case 'P': { /* DCH — Delete Characters */
        i32 n = p0;
        if (n > t->cols - t->cursor_x) n = t->cols - t->cursor_x;
        Cell *row = &t->cells[t->cursor_y * t->cols];
        memmove(&row[t->cursor_x], &row[t->cursor_x + n],
                (usize)(t->cols - t->cursor_x - n) * sizeof(Cell));
        if (t->ul_map_count > 0) terminal_underline_shift_cols(t, t->cursor_y, t->cursor_x, -n);
        Cell blank = { .codepoint = ' ', .attr = t->cursor_attr };
        blank.attr.flags = 0;
        for (i32 i = t->cols - n; i < t->cols; i++) row[i] = blank;
        wide_row_repair(t, t->cursor_y);
        if (n > 0) wrap_break_rows(t, t->cursor_y, t->cursor_y);
        TERM_DIRTY_ROW(t, t->cursor_y);
        break;
    }
    case 'X': { /* ECH — Erase Characters */
        i32 n = p0;
        CellAttr a = t->cursor_attr;
        a.flags = 0;
        i32 end = t->cursor_x + n - 1;
        if (end >= t->cols) end = t->cols - 1;
        wide_unpair_span(t, t->cursor_y, t->cursor_x, end);
        buffer_clear_region(t->cells, t->cols, t->cursor_x, t->cursor_y, end, t->cursor_y, a);
        if (end >= t->cols - 1) wrap_break_rows(t, t->cursor_y, t->cursor_y);
        TERM_DIRTY_ROW(t, t->cursor_y);
        break;
    }

    /* SGR — Select Graphic Rendition */
    case 'm': {
        if (t->param_count == 0) {
            /* Reset */
            t->cursor_attr.fg = FG_DEFAULT;
            t->cursor_attr.bg = BG_DEFAULT;
            t->cursor_ul_color = 0;
            t->cursor_ul_style = 0;
            t->cursor_attr.flags = 0;
            break;
        }
        for (i32 i = 0; i < t->param_count; i++) {
            i32 p = t->params[i];
            if (p == 0) {
                t->cursor_attr.fg = FG_DEFAULT;
                t->cursor_attr.bg = BG_DEFAULT;
                t->cursor_ul_color = 0;
                t->cursor_ul_style = 0;
                t->cursor_attr.flags = 0;
            } else if (p == 1) {
                t->cursor_attr.flags |= ATTR_BOLD;
            } else if (p == 2) {
                t->cursor_attr.flags |= ATTR_DIM;
            } else if (p == 3) {
                t->cursor_attr.flags |= ATTR_ITALIC;
            } else if (p == 4) {
                /* `4:n` is an underline STYLE; `4;n` is underline and then
                 * attribute n. Only the colon makes a sub-parameter, so
                 * `4;1` keeps its bold and `1;4;0` ends in a reset. */
                t->cursor_attr.flags |= ATTR_UNDERLINE;
                t->cursor_ul_style = 1;
                if (i + 1 < t->param_count && PARAM_IS_SUB(t, i + 1)) {
                    i32 sub = t->params[i + 1];
                    if (sub == 0) {
                        t->cursor_attr.flags &= ~ATTR_UNDERLINE;
                        t->cursor_ul_style = 0;
                    } else if (sub >= 1 && sub <= 5) {
                        t->cursor_ul_style = (u8)sub;
                    }
                }
            } else if (p == 21) {
                t->cursor_attr.flags |= ATTR_UNDERLINE;      /* doubly underlined */
                t->cursor_ul_style = 2;
            } else if (p == 5 || p == 6) {
                t->cursor_attr.flags |= ATTR_BLINK;
            } else if (p == 7) {
                t->cursor_attr.flags |= ATTR_INVERSE;
            } else if (p == 8) {
                t->cursor_attr.flags |= ATTR_HIDDEN;
            } else if (p == 9) {
                t->cursor_attr.flags |= ATTR_STRIKETHROUGH;
            } else if (p == 22) {
                t->cursor_attr.flags &= ~(ATTR_BOLD | ATTR_DIM);
            } else if (p == 23) {
                t->cursor_attr.flags &= ~ATTR_ITALIC;
            } else if (p == 24) {
                t->cursor_attr.flags &= ~ATTR_UNDERLINE;
                t->cursor_ul_style = 0;
            } else if (p == 25) {
                t->cursor_attr.flags &= ~ATTR_BLINK;
            } else if (p == 27) {
                t->cursor_attr.flags &= ~ATTR_INVERSE;
            } else if (p == 28) {
                t->cursor_attr.flags &= ~ATTR_HIDDEN;
            } else if (p == 29) {
                t->cursor_attr.flags &= ~ATTR_STRIKETHROUGH;
            } else if (p == 53) {
                t->cursor_attr.flags |= ATTR_OVERLINE;
            } else if (p == 55) {
                t->cursor_attr.flags &= ~ATTR_OVERLINE;
            } else if (p == 58) {
                /* Underline colour (sgr_ext_color). */
                SgrColor c = sgr_ext_color(t, i);
                if (c.ok) t->cursor_ul_color = c.rgb ? FG_TRUECOLOR(c.r, c.g, c.b) : (u32)c.idx;
                i += c.used;
            } else if (p == 59) {
                t->cursor_ul_color = 0; /* reset underline color */
            } else if (p >= 30 && p <= 37) {
                t->cursor_attr.fg = (u32)(p - 30);
            } else if (p == 38) {
                /* Extended foreground */
                SgrColor c = sgr_ext_color(t, i);
                if (c.ok) t->cursor_attr.fg = c.rgb ? FG_TRUECOLOR(c.r, c.g, c.b) : (u32)c.idx;
                i += c.used;
            } else if (p == 39) {
                t->cursor_attr.fg = FG_DEFAULT;
            } else if (p >= 40 && p <= 47) {
                t->cursor_attr.bg = (u32)(p - 40);
            } else if (p == 48) {
                /* Extended background */
                SgrColor c = sgr_ext_color(t, i);
                if (c.ok) t->cursor_attr.bg = c.rgb ? BG_TRUECOLOR(c.r, c.g, c.b) : (u32)c.idx;
                i += c.used;
            } else if (p == 49) {
                t->cursor_attr.bg = BG_DEFAULT;
            } else if (p >= 90 && p <= 97) {
                t->cursor_attr.fg = (u32)(p - 90 + 8);
            } else if (p >= 100 && p <= 107) {
                t->cursor_attr.bg = (u32)(p - 100 + 8);
            }
            /* Whatever sub-parameters the attribute had are part of it, read
             * or not: none of them is an attribute of its own. */
            while (i + 1 < t->param_count && PARAM_IS_SUB(t, i + 1)) i++;
        }
        break;
    }

    /* Scroll region */
    case 'r': { /* DECSTBM — Set Top and Bottom Margins */
        i32 top = param(t, 0, 1) - 1;
        i32 bot = param(t, 1, t->rows) - 1;
        if (bot >= t->rows) bot = t->rows - 1;
        /* A region needs two rows. A pair that does not make one is IGNORED
         * (VT510), so one malformed sequence cannot undo the region a
         * program is using. */
        if (top < 0 || top >= bot) break;
        t->scroll_top = top;
        t->scroll_bottom = bot;
        t->cursor_x = 0;
        t->cursor_y = (t->mode & MODE_ORIGIN) ? t->scroll_top : 0;
        break;
    }

    /* Device status */
    case 'n': { /* DSR — Device Status Report */
        i32 ps = param(t, 0, 0);
        if (ps == 5) {
            /* Operating status → OK */
            emit_response(t, TERM_REPLY_STATUS_OK, (i32)sizeof TERM_REPLY_STATUS_OK - 1);
        } else if (ps == 6) {
            /* Cursor Position Report (cursor_report_position has the rule). */
            i32 row, col;
            char buf[48];
            cursor_report_position(t, &row, &col);
            usize n = term_reply_cpr(buf, sizeof buf, row, col, false);
            if (n) emit_response(t, buf, (i32)n);
        }
        break;
    }
    case 'c': { /* DA — Device Attributes */
        /* Primary DA: report as VT220 with ANSI color + extended attributes.
         * Only `CSI c` / `CSI 0 c` ask; another parameter is not a request. */
        if (param(t, 0, 0) == 0) emit_response(t, TERM_REPLY_DA1, (i32)sizeof TERM_REPLY_DA1 - 1);
        break;
    }
    case 'b': { /* REP — Repeat previous character */
        i32 count = param(t, 0, 1);
        if (t->last_printed_char > 0) {
            /* Bounded by one screenful: the count is a program's to choose. */
            i32 most = t->cols * t->rows;
            for (i32 ri = 0; ri < count && ri < most; ri++) {
                put_char(t, t->last_printed_char);
            }
        }
        break;
    }

    /* Cursor style (DECSCUSR) */
    case ' ':
        /* Handled below with intermediate bytes */
        break;

    /* Tab clear */
    case 'g':
        if (param(t, 0, 0) == 0) {
            /* cursor_x may be == cols in deferred-wrap state; clamp the index. */
            i32 tx = t->cursor_x < t->cols ? t->cursor_x : t->cols - 1;
            if (tx >= 0) t->tab_stops[tx] = false;
        } else if (param(t, 0, 0) == 3) {
            memset(t->tab_stops, 0, (usize)t->cols * sizeof(bool));
        }
        break;

    /* CHT / CBT are dispatched with the other cursor moves above — see the 'I' /
     * 'Z' cases there, which cap the repeat count at `cols` (an untrusted
     * `CSI 99999 Z` must not become a 99999 x cols walk) and clamp the cursor out
     * of the deferred-wrap column `cols`, which `tab_stops` does not have. */

    /* Save/Restore cursor */
    case 's':
        cursor_save(t);
        break;
    case 'u':
        cursor_restore(t);
        break;

    default:
        break;
    }
}

/* =========================================================================
 * CSI ? (DEC private mode) handler
 * ========================================================================= */

/* ANSI mode set/reset — CSI Pn h / CSI Pn l with no private-marker prefix.
 *
 * Only the two the rest of the terminal can act on are handled; an unknown mode
 * is ignored, which is what the standard asks for and what every terminal does.
 * The DEC private modes (CSI ? Pn h) are a separate namespace and live in
 * handle_dec_mode. */
static void handle_ansi_mode(Terminal *t, bool set) {
    for (i32 i = 0; i < t->param_count; i++) {
        switch (t->params[i]) {
        case 4:  /* IRM — Insert/Replace. Printing shifts the rest of the row
                  * right instead of overwriting it; see put_char. */
            if (set) t->mode |= MODE_INSERT;
            else     t->mode &= ~MODE_INSERT;
            break;
        case 20: /* LNM — Line Feed / New Line. Makes LF also return the
                  * carriage, and makes Return send CR LF. */
            if (set) t->mode |= MODE_CRLF;
            else     t->mode &= ~MODE_CRLF;
            break;
        default:
            break;
        }
    }
}

static void handle_dec_mode(Terminal *t, bool set) {
    for (i32 i = 0; i < t->param_count; i++) {
        i32 p = t->params[i];
        switch (p) {
        case 1:    /* DECCKM — Application Cursor Keys */
            if (set) t->mode |= MODE_APP_CURSOR;
            else     t->mode &= ~MODE_APP_CURSOR;
            break;
        case 6:    /* DECOM — Origin Mode */
            if (set) t->mode |= MODE_ORIGIN;
            else     t->mode &= ~MODE_ORIGIN;
            t->cursor_x = 0;
            t->cursor_y = set ? t->scroll_top : 0;
            break;
        case 7:    /* DECAWM — Auto-Wrap */
            if (set) t->mode |= MODE_WRAP;
            else     t->mode &= ~MODE_WRAP;
            break;
        case 12:   /* Cursor blink — ignore */
            break;
        case 25:   /* DECTCEM — Text Cursor Enable */
            t->cursor_visible = set;
            break;
        case 47:   /* Alternate Screen Buffer (old) */
        case 1047: /* Alternate Screen Buffer */
            if (set && !(t->mode & MODE_ALT_SCREEN)) {
                /* Lazy-allocate alt_cells on first use — keeps idle terminals
                 * (those that never enter alt screen) from paying the
                 * primary-screen-sized buffer. */
                if (!t->alt_cells) {
                    t->alt_cells = calloc((usize)t->cols * (usize)t->rows,
                                          sizeof(Cell));
                    if (!t->alt_cells) break;
                }
                terminal_grid_swap_alt(t);
                screen_state_swap(t, true);
                t->search_layout_generation++;
                t->alt_cursor_x = t->cursor_x;
                t->alt_cursor_y = t->cursor_y;
                t->mode |= MODE_ALT_SCREEN;
                /* Default alternateScroll ON with the alt screen, as every
                 * mainstream terminal does: a full-screen app that does not
                 * grab the mouse still has to be scrollable. `CSI ? 1007 l`
                 * turns it back off for one that wants the raw wheel. */
                t->mode |= MODE_ALT_SCROLL;
                /* Show what just took the screen. Launching a TUI from a
                 * scrolled-back view would otherwise paint the alt buffer
                 * behind the scrollback the user was reading — the app looks
                 * like it never started. */
                t->scroll_offset = 0;
            } else if (!set && (t->mode & MODE_ALT_SCREEN)) {
                terminal_grid_swap_alt(t);  /* primary live, alt parked */
                screen_state_swap(t, false);
                t->search_layout_generation++;
                t->cursor_x = t->alt_cursor_x;
                t->cursor_y = t->alt_cursor_y;
                /* The window may have been resized while the program was up:
                 * the position remembered at entry can lie outside the grid
                 * now, and nothing else brings it back in. */
                cursor_clamp(t);
                t->mode &= ~MODE_ALT_SCREEN;
                /* The swap brought the primary's underline styles back with its
                 * grid; what is parked now is the program's, and it has left. */
                terminal_underline_drop_parked(t);
                /* Drop the alt buffer once it is left. Vim/less/htop are
                 * the dominant alt-screen consumers, all of which exit
                 * cleanly via 1049/1047 unset; their stale framebuffer need
                 * not stay alive across runs. */
                terminal_grid_free_alt(t);
                /* Same cleanup as the 1049 branch — kill mouse/focus tracking
                 * so a TUI that exited via signal can't flood the shell with
                 * raw scroll reports. See the 1049 comment for the full
                 * rationale. */
                t->mode &= ~(MODE_MOUSE_BTN | MODE_MOUSE_MOTION |
                             MODE_MOUSE_SGR | MODE_MOUSE_ANY |
                             MODE_MOUSE_URXVT | MODE_MOUSE_PIXEL |
                             MODE_FOCUS_EVENT | MODE_ALT_SCROLL);
            }
            break;
        case 1048: /* Save/Restore cursor */
            if (set) {
                cursor_save(t);
            } else {
                cursor_restore(t);
            }
            break;
        case 1049: /* Alternate screen + save cursor */
            if (set) {
                cursor_save(t);
                if (!(t->mode & MODE_ALT_SCREEN)) {
                    /* Lazy-allocate alt_cells on first use */
                    if (!t->alt_cells) {
                        t->alt_cells = calloc((usize)t->cols * (usize)t->rows,
                                              sizeof(Cell));
                        if (!t->alt_cells) break;
                    }
                    terminal_grid_swap_alt(t);
                    screen_state_swap(t, true);
                    t->search_layout_generation++;
                    t->mode |= MODE_ALT_SCREEN;
                    /* Default alternateScroll ON with the alt screen, as every
                     * mainstream terminal does: a full-screen app that does not
                     * grab the mouse still has to be scrollable. `CSI ? 1007 l`
                     * turns it back off for one that wants the raw wheel. */
                    t->mode |= MODE_ALT_SCROLL;
                    /* See the 1047 branch: a TUI started from a scrolled-back
                     * view has to be visible when it arrives. */
                    t->scroll_offset = 0;
                    CellAttr a = { .fg = FG_DEFAULT, .bg = BG_DEFAULT, .flags = 0 };
                    buffer_clear_region(t->cells, t->cols, 0, 0, t->cols-1, t->rows-1, a);
                    terminal_clear_underline_map(t);
                }
            } else {
                if (t->mode & MODE_ALT_SCREEN) {
                    terminal_grid_swap_alt(t);  /* primary live, alt parked */
                    screen_state_swap(t, false);
                    t->search_layout_generation++;
                    t->mode &= ~MODE_ALT_SCREEN;
                    /* The swap brought the primary's underline styles back
                     * with its grid, so the styled underlines on the shell's
                     * screen come back from vim as they were. What is parked
                     * now is the program's. */
                    terminal_underline_drop_parked(t);
                    /* Release the alt buffer — it's stale and the next
                     * alt-screen entry will lazy-alloc a fresh one. */
                    terminal_grid_free_alt(t);
                }
                cursor_restore(t);
                t->cursor_style_set = false;   /* the full-screen program's cursor shape ends with its screen */
                /* The keyboard-protocol flags a program set on the alternate
                 * screen went with that screen (screen_state_swap), whether it
                 * popped them or died first; the shell's own are back.
                 * modifyOtherKeys has no per-screen state, so it is reset. */
                t->modify_other_keys = 0;
                t->mode &= ~MODE_MODIFY_OTHER_KEYS;

                /* Same logic for mouse tracking — TUIs that enable SGR mouse
                 * (?1000 / ?1002 / ?1003 / ?1006 / ?1015 / ?1016 / ?1004) on
                 * entry are supposed to clear them on exit, but a SIGINT or
                 * crash skips that cleanup. The shell ends up receiving raw
                 * mouse reports (\e[<64;57;20M…) on every scroll, gets the
                 * \e[< chewed by readline, and dumps the parameter tail into
                 * the prompt as literal text. Handle this by
                 * tying the mouse + focus mode reset to the alt-screen exit
                 * boundary that 1049 already represents. */
                t->mode &= ~(MODE_MOUSE_BTN | MODE_MOUSE_MOTION |
                             MODE_MOUSE_SGR | MODE_MOUSE_ANY |
                             MODE_MOUSE_URXVT | MODE_MOUSE_PIXEL |
                             MODE_FOCUS_EVENT | MODE_ALT_SCROLL);
            }
            break;
        case 1000: /* Mouse button tracking */
            if (set) t->mode |= MODE_MOUSE_BTN;
            else     t->mode &= ~MODE_MOUSE_BTN;
            break;
        case 1002: /* Mouse motion tracking */
            if (set) t->mode |= MODE_MOUSE_MOTION;
            else     t->mode &= ~MODE_MOUSE_MOTION;
            break;
        case 1006: /* SGR mouse mode */
            if (set) t->mode |= MODE_MOUSE_SGR;
            else     t->mode &= ~MODE_MOUSE_SGR;
            break;
        case 2004: /* Bracketed Paste */
            if (set) t->mode |= MODE_BRACKETED_PASTE;
            else     t->mode &= ~MODE_BRACKETED_PASTE;
            break;
        case 1003: /* Any-event mouse tracking */
            if (set) t->mode |= MODE_MOUSE_ANY;
            else     t->mode &= ~MODE_MOUSE_ANY;
            break;
        case 1004: /* Focus events */
            if (set) t->mode |= MODE_FOCUS_EVENT;
            else     t->mode &= ~MODE_FOCUS_EVENT;
            break;
        case 1007: /* Alternate scroll mode */
            if (set) t->mode |= MODE_ALT_SCROLL;
            else     t->mode &= ~MODE_ALT_SCROLL;
            break;
        case 1015: /* URXVT mouse mode */
            if (set) t->mode |= MODE_MOUSE_URXVT;
            else     t->mode &= ~MODE_MOUSE_URXVT;
            break;
        case 1016: /* SGR pixel mouse mode */
            if (set) t->mode |= MODE_MOUSE_PIXEL;
            else     t->mode &= ~MODE_MOUSE_PIXEL;
            break;
        case 2026: /* Synchronized output */
            if (set) {
                t->mode |= MODE_SYNC_OUTPUT;
                /* Stamp only on the RISING edge. Re-stamping on a repeated BSU
                 * with no ESU in between would re-arm the deferral deadline
                 * every time and make the timeout unreachable by exactly the
                 * broken program it exists to bound. Keeping the running stamp
                 * means the frame is forced at most SYNC_UPDATE_TIMEOUT_SEC
                 * after the FIRST BSU, however many follow. Zero means
                 * "unstamped" — the parser has no clock, the main loop stamps
                 * it on first sight. */
                if (!t->sync_pending) {
                    (void)terminal_sync_capture(t);
                    t->sync_start_time = 0;
                }
                t->sync_pending = true;
            } else {
                t->mode &= ~MODE_SYNC_OUTPUT;
                t->sync_pending = false;
                t->sync_start_time = 0;   /* so the next BSU starts clean */
                terminal_sync_release(t);
            }
            /* Drawing the captured picture consumes only its dirty rows.
             * The live rows and palette keep accumulating changes until ESU,
             * so a one-row update stays a one-row rebuild. */
            break;
        case 2027: /* Grapheme cluster mode */
            if (set) t->mode |= MODE_GRAPHEME;
            else     t->mode &= ~MODE_GRAPHEME;
            break;
        case 2031: /* Colour-scheme change notifications (terminal_color_scheme_changed) */
            if (set) t->mode |= MODE_COLOR_SCHEME;
            else     t->mode &= ~MODE_COLOR_SCHEME;
            break;
        case 2048: /* In-band resize */
            if (set) {
                t->mode |= MODE_IN_BAND_RESIZE;
                /* Immediately report current size */
                emit_response_fmt(t, "\x1b[48;%d;%d;0;0t", t->rows, t->cols);
            } else {
                t->mode &= ~MODE_IN_BAND_RESIZE;
            }
            break;
        case 8800: /* BiDi — implicit bidirectional text reordering */
            t->bidi_enabled = set;
            TERM_DIRTY_ALL(t);
            break;
        }
    }
}

/* =========================================================================
 * OSC (Operating System Command) handler
 * ========================================================================= */

/* The OSC accumulator hands the handler at most ~255 bytes, cutting wherever
 * the buffer ended — often in the middle of a multi-byte character. Trim an
 * incomplete trailing sequence rather than rejecting a title for it (only
 * applied when the string is near the cap: elsewhere a partial tail is a real
 * encoding error and the title is rejected). */
static i32 osc_title_trim_partial_tail(const char *s, i32 n) {
    for (i32 k = 1; k <= 3 && k <= n; k++) {
        unsigned char c = (unsigned char)s[n - k];
        if ((c & 0xC0) == 0x80) continue;                  /* continuation: keep looking */
        i32 need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        return need > k ? n - k : n;                       /* lead: partial or whole */
    }
    return n;
}

/* A window/icon title (OSC 0/1/2) must be clean printable UTF-8. Binary output
 * piped to the terminal — e.g. `cat <image>` — sprays arbitrary bytes that
 * would otherwise land in t->title, render as garbage in the tab bar, and (on
 * macOS) crash native -[NSWindow setTitle:] because invalid UTF-8 makes
 * +stringWithUTF8String: return nil. Reject any string with control bytes or
 * malformed UTF-8 so the title simply stays unchanged.
 *
 * Valid UTF-8 can still lie, though, and the title is drawn in the tab pill and
 * persisted in sessions.json: C1 controls (U+0080-U+009F — a 8-bit CSI to any
 * consumer that decodes them), the bidi embedding / override / isolate controls
 * (U+202A-202E, U+2066-2069 — "Trojan Source": a title that displays as
 * "invoice.pdf" and is not, or that reorders the text around it), the invisible
 * spaces (U+200B, U+2060, U+FEFF) and the line / paragraph separators (U+2028,
 * U+2029). Those are DROPPED rather than rejecting the title, so a legitimate
 * RTL title (plain LRM / RLM marks, kept) still works — and so do ZWNJ / ZWJ
 * (U+200C / U+200D), which Persian, Indic scripts and emoji sequences need to
 * be spelled at all. `dst` must hold n + 1 bytes; returns the new length, or -1 to reject. */
static i32 osc_title_clean_copy(char *dst, const char *s, i32 n) {
    i32 w = 0, i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) return -1;               /* control byte */
        u32 cp = 0;
        u32 used = utf8_decode((const u8 *)s + i, (usize)(n - i), &cp);
        if (used == 0) return -1;                           /* invalid / truncated */
        /* core/text_sanitize.h's list — ZWNJ / ZWJ are spelling and are kept. */
        bool deceptive = liu_is_deceptive_cp(cp);
        if (!deceptive) {
            memcpy(dst + w, s + i, used);
            w += (i32)used;
        }
        i += (i32)used;
    }
    dst[w] = '\0';
    return w;
}

/* X11 colour specs, as OSC 4 and OSC 10/11/12 carry them.
 *
 * Four forms, and the component WIDTH is the part that is easy to get wrong:
 * `rgb:` takes 1-4 hex digits per channel and they are scaled, not truncated —
 * `rgb:f/f/f` is white, not (15,15,15). `#` takes 3, 6 or 12 digits total and
 * the terminal protocol reads them the same way. Colour NAMES ("red", "CornflowerBlue") and
 * `rgbi:` float specs are not accepted: a name table is 750 entries the tree
 * has no other use for, and an unparsable spec leaves the index alone, which
 * is the same thing that happened before this parser existed.
 *
 * Returns 0xRRGGBB. */
static bool osc_hex_component(const char *p, i32 n, u32 *out) {
    if (n < 1 || n > 4) return false;
    u32 v = 0;
    for (i32 i = 0; i < n; i++) {
        char c = p[i];
        u32 d;
        if (c >= '0' && c <= '9')      d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A') + 10u;
        else return false;
        v = (v << 4) | d;
    }
    /* Scale n nibbles to 8 bits: 0 stays 0 and the maximum stays the maximum,
     * which is what makes `rgb:f/f/f` white and `#fff` white too. */
    u32 max = (1u << (4 * (u32)n)) - 1u;
    *out = (v * 255u + max / 2u) / max;
    return true;
}

static bool osc_parse_color(const char *sp, i32 len, u32 *out_rgb) {
    if (!sp || len <= 0 || !out_rgb) return false;
    u32 r, g, b;
    if (sp[0] == '#') {
        i32 n = len - 1;
        if (n != 3 && n != 6 && n != 12) return false;
        i32 w = n / 3;
        if (!osc_hex_component(sp + 1,         w, &r) ||
            !osc_hex_component(sp + 1 + w,     w, &g) ||
            !osc_hex_component(sp + 1 + 2 * w, w, &b)) return false;
        *out_rgb = (r << 16) | (g << 8) | b;
        return true;
    }
    if (len > 4 && (sp[0] == 'r' || sp[0] == 'R') &&
        (sp[1] == 'g' || sp[1] == 'G') && (sp[2] == 'b' || sp[2] == 'B') &&
        sp[3] == ':') {
        const char *p = sp + 4;
        i32 rest = len - 4;
        i32 f[3] = { -1, -1, -1 };            /* the two '/' offsets, then end */
        i32 nf = 0;
        for (i32 i = 0; i < rest && nf < 2; i++)
            if (p[i] == '/') f[nf++] = i;
        if (nf != 2) return false;
        f[2] = rest;
        if (!osc_hex_component(p,            f[0],                &r) ||
            !osc_hex_component(p + f[0] + 1, f[1] - f[0] - 1,     &g) ||
            !osc_hex_component(p + f[1] + 1, f[2] - f[1] - 1,     &b)) return false;
        *out_rgb = (r << 16) | (g << 8) | b;
        return true;
    }
    return false;
}

/* ---- in-band notifications and progress ---------------------------------
 *
 * Three spellings of "tell the user": OSC 9 ; text, OSC 777 ; notify ; title
 * ; body and OSC 99 (in pieces). They let a program speak without a hook in
 * an agent's settings file — including one on the far side of an SSH session
 * or one that has no hook to write.
 *
 * What reaches the callback is cleaned here — valid UTF-8, no control bytes,
 * no deceptive code points, bounded — because it is a program's text on its
 * way to the user's notification centre. How OFTEN it may speak is the
 * receiver's business (ui.c). Nothing is ever replied: OSC 99's `p=?` query
 * and its close/activation reports are not answered. */
/* A notification's text as one clean line: control bytes and runs of white
 * space become one space (a body may arrive with line breaks; a title must
 * not carry any), the deceptive code points of core/text_sanitize.h are
 * dropped, and text that is not valid UTF-8 is refused whole (-1). `dst` holds
 * n + 1 bytes. */
static i32 note_clean_copy(char *dst, const char *s, i32 n) {
    i32 w = 0, i = 0;
    bool gap = false;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c == 0x7f) { gap = w > 0; i++; continue; }
        u32 cp = 0;
        u32 used = utf8_decode((const u8 *)s + i, (usize)(n - i), &cp);
        if (used == 0) return -1;
        if (!liu_is_deceptive_cp(cp)) {
            if (gap) dst[w++] = ' ';
            gap = false;
            memcpy(dst + w, s + i, used);
            w += (i32)used;
        }
        i += (i32)used;
    }
    dst[w] = '\0';
    return w;
}

static void notify_emit(Terminal *t, const char *title, i32 title_len, const char *body, i32 body_len) {
    if (!t->on_notify) return;
    char ct[TERM_NOTIFY_TITLE_CAP], cb[TERM_NOTIFY_BODY_CAP];
    if (title_len > (i32)sizeof ct - 1) title_len = osc_title_trim_partial_tail(title, (i32)sizeof ct - 1);
    if (body_len  > (i32)sizeof cb - 1) body_len  = osc_title_trim_partial_tail(body,  (i32)sizeof cb - 1);
    if (title_len <= 0 || note_clean_copy(ct, title, title_len) < 0) ct[0] = '\0';
    if (body_len  <= 0 || note_clean_copy(cb, body,  body_len)  < 0) cb[0] = '\0';
    if (!ct[0] && !cb[0]) return;
    t->on_notify(t, ct, cb, t->userdata);
}

/* OSC 9 ; 4 ; state [; value] — `p` is what follows the 4. */
static void osc_progress(Terminal *t, const char *p, i32 n) {
    i32 st = 0, val = -1, pos = 0;
    if (pos < n && p[pos] == ';') {
        pos++;
        i32 d = 0;
        while (pos < n && p[pos] >= '0' && p[pos] <= '9' && d < 3) { st = st * 10 + (p[pos] - '0'); pos++; d++; }
        if (pos < n && p[pos] == ';') {
            pos++;
            i32 v = 0; d = 0;
            while (pos < n && p[pos] >= '0' && p[pos] <= '9' && d < 4) { v = v * 10 + (p[pos] - '0'); pos++; d++; }
            if (d) val = v > 100 ? 100 : v;
        }
    }
    switch (st) {
        case 0: t->progress_state = TERM_PROGRESS_NONE; t->progress_value = 0; break;
        case 1: t->progress_state = TERM_PROGRESS_SET; t->progress_value = (u8)(val < 0 ? 0 : val); break;
        case 2: t->progress_state = TERM_PROGRESS_ERROR; if (val >= 0) t->progress_value = (u8)val; break;
        case 3: t->progress_state = TERM_PROGRESS_INDETERMINATE; break;
        case 4: t->progress_state = TERM_PROGRESS_PAUSED; if (val >= 0) t->progress_value = (u8)val; break;
        default: break;                          /* a state nobody defined: left as it was */
    }
}

/* Append to one of OSC 99's pieces, never past its end and never cutting a
 * code point (the cleaner refuses what is not valid UTF-8). */
/* Append one piece. A piece that did not fit CLOSES the text (`*full`):
 * putting the next piece's first bytes after the cut would make a sentence
 * that was never sent and, when the cut fell inside a letter, not UTF-8
 * either, and the whole notification would then be refused. */
static void note99_append(char *dst, usize cap, const char *src, i32 n, bool *full) {
    usize have = strlen(dst);
    if (n <= 0 || *full) return;
    if (have + 1 >= cap) { *full = true; return; }
    usize room = cap - 1 - have;
    if ((usize)n > room) {
        n = osc_title_trim_partial_tail(src, (i32)room);
        *full = true;
    }
    if (n <= 0) return;
    memcpy(dst + have, src, (usize)n);
    dst[have + (usize)n] = '\0';
}

/* Does this OSC 133 mark carry this pane's key (`;liu=<key>`)?
 *
 * OSC 133 is believed from any output: that is what lets a remote shell's
 * prompt be shaded. But text that only PRINTS the marks — a file, a log
 * line, a host on the far side of ssh — must not get a "command" of its
 * choosing written into the folder's history, to be offered back at the
 * prompt as the user's own. The shell Liu started knows a key the output
 * does not (core/shell_integration.h); only a mark that carries it is taken
 * as the shell's word for what was typed and how it ended. The marks' other
 * effects — zones, exit status, the time a command took — need no key. */
static bool osc133_mark_is_ours(const Terminal *t, const char *text, i32 len) {
    usize kl = strlen(t->shell_mark_key);
    if (kl == 0) return false;
    for (i32 i = 0; i + 5 <= len; i++) {
        if (memcmp(text + i, ";liu=", 5) != 0) continue;
        const char *val = text + i + 5;
        i32 vl = 0;
        while (i + 5 + vl < len && val[vl] != ';') vl++;
        return (usize)vl == kl && memcmp(val, t->shell_mark_key, kl) == 0;
    }
    return false;
}

/* `OSC 133 ; C ; liu=<key> ; cmd=<percent-encoded command>`: the shell's own
 * copy of the line it is about to run (zsh's preexec has it).
 *
 * The grid is not the command: read back off it, a right prompt on the same
 * row becomes part of it, a multi-line command leaves one line, a redrawn
 * prompt (Ctrl+L, a job notice) leaves none, and anything longer than the
 * screen is cut. The shell knows exactly what it was given. The grid stays as
 * the fallback for a shell that sends no text.
 *
 * Returns true when the mark carried a `cmd=` (an empty one means "run, but
 * not for the history" — a line the user kept out of the shell's own). Text
 * that is not clean UTF-8 without control bytes is refused whole. */
static bool osc133_take_command(Terminal *t, const char *text, i32 len) {
    i32 at = -1;
    for (i32 i = 0; i + 5 <= len; i++)
        if (memcmp(text + i, ";cmd=", 5) == 0) { at = i + 5; break; }
    if (at < 0) return false;
    if (!t->pending_command) {
        t->pending_command_cap = 4096;
        t->pending_command = calloc(1, (usize)t->pending_command_cap);
        if (!t->pending_command) { t->pending_command_cap = 0; return true; }
    }
    char *out = t->pending_command;
    const i32 cap = t->pending_command_cap;
    i32 n = 0;
    bool ok = true;
    for (i32 i = at; i < len && text[i] != ';'; i++) {
        u8 byte = (u8)text[i];
        if (byte == '%') {
            i32 hi = i + 2 < len ? ascii_hex_value((u8)text[i + 1]) : -1;
            i32 lo = i + 2 < len ? ascii_hex_value((u8)text[i + 2]) : -1;
            if (hi < 0 || lo < 0) { ok = false; break; }
            byte = (u8)((hi << 4) | lo);
            i += 2;
        }
        /* A command is text: line breaks and tabs, no other control byte. */
        if ((byte < 0x20 && byte != '\n' && byte != '\t') || byte == 0x7f) { ok = false; break; }
        if (n >= cap - 1) { ok = false; break; }
        out[n++] = (char)byte;
    }
    if (!ok || !utf8_validate((const u8 *)out, (usize)n)) n = 0;
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t' || out[n - 1] == '\n')) n--;
    out[n] = '\0';
    return true;
}

/* `bel`: the string ended with BEL rather than ST. A reply ends the way its
 * query did, which is what the program's reader is waiting for. */
/* `osc` is the whole string, `osc_len` bytes and a NUL after them. */
static void handle_osc(Terminal *t, const u8 *osc, i32 osc_len, bool bel) {
    /* Parse "Ps;Pt" where Ps is the command number */
    i32 cmd = 0;
    i32 i = 0;
    while (i < osc_len && osc[i] >= '0' && osc[i] <= '9') {
        cmd = decimal_push_bounded(cmd, osc[i], 1000000);
        i++;
    }
    if (i < osc_len && osc[i] == ';') i++;

    const char *text = (const char *)&osc[i];
    i32 text_len = osc_len - i;

    switch (cmd) {
    case 0:  /* Set icon name + window title */
    case 2: { /* Set window title */
        i32 n = text_len < 255 ? text_len : 255;
        /* A cap that lands inside a multi-byte sequence would make a long,
         * perfectly valid title look truncated and be rejected: cut it at the
         * sequence's first byte. */
        if (n >= 240) n = osc_title_trim_partial_tail(text, n);
        /* Reject binary / invalid-UTF-8 titles (cat <image> etc.) — leave the
         * existing title untouched rather than store garbage that crashes the
         * native title bar — and drop the code points that only deceive. */
        char clean[256];
        i32 cn = osc_title_clean_copy(clean, text, n);
        if (cn < 0) break;
        memcpy(t->title, clean, (usize)cn + 1);
        t->title_changed = true;
        if (cmd == 0) {
            if (!t->icon_name) {
                t->icon_name = calloc(1, 128);
                if (!t->icon_name) break;
            }
            /* Cut at a code point boundary: a byte cap through the middle of a
             * UTF-8 sequence would store invalid UTF-8. */
            i32 m = cn < 127 ? cn : 127;
            while (m > 0 && ((unsigned char)clean[m] & 0xC0) == 0x80) m--;
            memcpy(t->icon_name, clean, (usize)m);
            t->icon_name[m] = '\0';
        }
        if (t->on_title) t->on_title(t, t->title, t->userdata);
        break;
    }
    case 1: { /* Set icon name */
        if (!t->icon_name) {
            t->icon_name = calloc(1, 128);
            if (!t->icon_name) break;
        }
        i32 n = text_len < 127 ? text_len : 127;
        if (n >= 110) n = osc_title_trim_partial_tail(text, n);
        char clean[128];
        i32 cn = osc_title_clean_copy(clean, text, n);
        if (cn < 0) break;                     /* same gate as the title */
        memcpy(t->icon_name, clean, (usize)cn + 1);
        break;
    }
    case 7: { /* Current working directory URI: file://hostname/path */
        /* A report that does not fit is no report: cut, it names another
         * folder (a parent, when the cut lands on a separator). The limit is
         * the PATH's (1023 bytes, checked after decoding); the encoded form
         * of a legal path is up to three times that, and capping it at 1023
         * dropped every long folder name outside ASCII. */
        char uri[4096];
        if (text_len > (i32)sizeof uri - 1) break;
        i32 n = text_len;
        memcpy(uri, text, (usize)n);
        uri[n] = '\0';
        /* Parse file:// URI → extract path */
        const char *path = uri;
        if (strncmp(uri, "file://", 7) == 0) {
            path = uri + 7;
            /* Skip hostname */
            const char *slash = strchr(path, '/');
            if (!slash) break;
            path = slash;
        }
        if (path[0] != '/') break;
        /* URL-decode %XX sequences */
        char decoded[1024];
        i32 di = 0;
        usize plen = strlen(path);
        usize pi = 0;
        bool clean = true;
        for (; pi < plen && di < 1023; pi++) {
            u8 byte = (u8)path[pi];
            if (byte == '%' && pi + 2u < plen) {
                i32 hi = ascii_hex_value((u8)path[pi + 1u]);
                i32 lo = ascii_hex_value((u8)path[pi + 2u]);
                if (hi < 0 || lo < 0) { clean = false; break; }
                byte = (u8)((hi << 4) | lo);
                pi += 2u;
            } else if (byte == '%') {
                clean = false;
                break;
            }
            if (byte == 0 || byte < 0x20 || byte == 0x7f) {
                clean = false;
                break;
            }
            decoded[di++] = (char)byte;
        }
        decoded[di] = '\0';
        if (!clean || pi < plen ||
            !utf8_validate((const u8 *)decoded, (usize)di)) break;
        if (!t->cwd) {
            t->cwd = calloc(1, 1024);
            if (!t->cwd) break;
        }
        snprintf(t->cwd, 1024, "%s", decoded);
        t->cwd_seq++;
        t->title_changed = true;
        if (t->on_cwd) t->on_cwd(t, t->cwd, t->userdata);
        break;
    }
    case 8: { /* OSC 8 — Hyperlinks */
        /* Format: OSC 8;params;URI ST (begin) or OSC 8;; ST (end) */
        const char *semi1 = memchr(text, ';', (usize)text_len);
        if (semi1) {
            const char *uri_start = semi1 + 1;
            i32 uri_len = text_len - (i32)(uri_start - text);
            if (uri_len > 0 && uri_len < 1023) {
                if (!t->hyperlink_uri) {
                    t->hyperlink_uri = calloc(1, 1024);
                    if (!t->hyperlink_uri) break;
                }
                memcpy(t->hyperlink_uri, uri_start, (usize)uri_len);
                t->hyperlink_uri[uri_len] = '\0';
                t->in_hyperlink = true;
                t->cursor_attr.flags |= ATTR_HYPERLINK;
            } else {
                /* End hyperlink */
                if (t->hyperlink_uri) t->hyperlink_uri[0] = '\0';
                t->in_hyperlink = false;
                t->cursor_attr.flags &= ~ATTR_HYPERLINK;
            }
        }
        break;
    }
    case 4: {  /* OSC 4;index;spec[;index;spec…] — set indexed colours.
                *
                * Writes the per-terminal override table, which the render
                * cache's own LUT reads, so one shell's palette stays out of
                * every other window.
                *
                * `?` in the spec position is the protocol's QUERY, and it is
                * answered under OSC 10 / 11's rule: THE ANSWER IS THE THEME'S
                * COLOUR, as the screen draws an index nobody set
                * (terminal_theme_palette_set, from app_apply_config) — never
                * one a program set with this very sequence, so a program
                * cannot put twelve digits of its choosing into another
                * program's input. A TUI in its "system" theme (OpenCode asks
                * `4;0;?` first and stops there) builds its panels from the
                * answer; unanswered, it falls back to a palette it cached
                * elsewhere. Silent until the app has said the theme, and for
                * an index past 255; a chained query answers each of its
                * pairs, at most 256 of them (a long OSC must not make the
                * terminal answer more than the palette). */
        i32 pos = 0, answered = 0;
        while (pos < text_len) {
            /* index */
            i32 idx = -1, digits = 0;
            while (pos < text_len && text[pos] >= '0' && text[pos] <= '9') {
                if (idx < 0) idx = 0;
                idx = decimal_push_bounded(idx, text[pos], 100000);
                pos++; digits++;
            }
            if (!digits || pos >= text_len || text[pos] != ';') break;
            pos++;                                   /* the ';' after the index */
            /* spec, up to the next ';' */
            i32 start = pos;
            while (pos < text_len && text[pos] != ';') pos++;
            i32 slen = pos - start;
            if (pos < text_len) pos++;               /* the ';' after the spec */
            if (slen <= 0) continue;
            u32 rgb;
            if (slen == 1 && text[start] == '?') {
                if (answered < 256 && terminal_theme_index_color(idx, &rgb)) {
                    answered++;
                    u32 r = (rgb >> 16) & 0xFFu, g = (rgb >> 8) & 0xFFu, bl = rgb & 0xFFu;
                    emit_response_fmt(t, "\033]4;%d;rgb:%02x%02x/%02x%02x/%02x%02x%s",
                                      (int)idx, r, r, g, g, bl, bl, bel ? "\a" : "\033\\");
                }
                continue;
            }
            if (osc_parse_color(text + start, slen, &rgb))
                terminal_palette_set(t, idx, rgb);
            /* An unparsable spec (a colour NAME, rgbi:, a query) leaves the
             * index alone and keeps parsing the rest — the protocol applies the pairs
             * it understands rather than dropping the whole sequence. */
        }
        break;
    }
    case 10: case 11: case 12: {
        /* OSC 10 / 11 / 12 ; ? — "what is the default foreground / background
         * / cursor colour?" It is how a TUI tells a dark terminal from a light
         * one, and an agent CLI that gets no answer draws for a dark
         * background whatever the theme is.
         *
         * Like OSC 4's query, it is a reply that is not a fixed string or a
         * number of its own, so the rule is spelled out: THE ANSWER IS THE
         * THEME'S COLOUR, never one a program set. What comes back is `rgb:`
         * and twelve hex digits out of the user's own theme — nothing in it is
         * chosen by whatever wrote the query, so it cannot carry text to a
         * shell prompt the way a title or a clipboard report can. Liu has no
         * SET half for these three (a spec here is ignored); a SET half must
         * never change this answer, or a program gets to write twelve digits
         * of its choosing into another program's input.
         *
         * `OSC 10;?;?` asks for 10 and then 11, as the protocol's chained form
         * does. Not answered until the app has said what the theme is. */
        i32 code = cmd, pos = 0;
        while (pos < text_len && code <= 12) {
            i32 start = pos;
            while (pos < text_len && text[pos] != ';') pos++;
            bool query = (pos - start == 1 && text[start] == '?');
            if (pos < text_len) pos++;               /* the ';' after it */
            u32 rgb;
            if (query && terminal_theme_color(code, &rgb)) {
                u32 r = (rgb >> 16) & 0xFFu, g = (rgb >> 8) & 0xFFu, bl = rgb & 0xFFu;
                emit_response_fmt(t, "\033]%d;rgb:%02x%02x/%02x%02x/%02x%02x%s",
                                  code, r, r, g, g, bl, bl, bel ? "\a" : "\033\\");
            }
            code++;
        }
        break;
    }
    case 104: { /* OSC 104[;index…] — reset indexed colours to the theme's. */
        if (text_len <= 0) { terminal_palette_reset(t, -1); break; }
        i32 pos = 0;
        while (pos < text_len) {
            i32 idx = -1, digits = 0;
            while (pos < text_len && text[pos] >= '0' && text[pos] <= '9') {
                if (idx < 0) idx = 0;
                idx = decimal_push_bounded(idx, text[pos], 100000);
                pos++; digits++;
            }
            if (digits) terminal_palette_reset(t, idx);
            if (pos < text_len && text[pos] == ';') pos++;
            else if (!digits) break;                 /* no progress: stop */
        }
        break;
    }
    case 9: { /* OSC 9 — one number, two protocols.
               * `9 ; <1-12> [; …]` is a numbered command; of those Liu takes
               * the progress report (4) and leaves the rest alone. Anything
               * else after `9;` is notification text. */
        i32 k = 0, num = 0;
        while (k < text_len && k < 3 && text[k] >= '0' && text[k] <= '9') { num = num * 10 + (text[k] - '0'); k++; }
        if (k > 0 && (k == text_len || text[k] == ';') && num >= 1 && num <= 12) {
            if (num == 4) osc_progress(t, text + k, text_len - k);
            break;
        }
        notify_emit(t, "", 0, text, text_len);
        break;
    }
    case 777: { /* OSC 777 ; notify ; title ; body */
        if (text_len > 7 && memcmp(text, "notify;", 7) == 0) {
            const char *title = text + 7;
            i32 rest = text_len - 7;
            const char *semi = memchr(title, ';', (usize)rest);
            if (semi) notify_emit(t, title, (i32)(semi - title), semi + 1, rest - (i32)(semi - title) - 1);
            else      notify_emit(t, "", 0, title, rest);
        }
        break;
    }
    case 99: { /* OSC 99 ; key=value:key=value ; payload — a notification.
                * Taken: i (which notification), d (0 = more pieces follow),
                * p (title | body), e (1 = the payload is base64). A piece
                * that is neither title nor body — a query, a close, an icon,
                * buttons — is not Liu's, and nothing is ever answered. */
        const char *semi = memchr(text, ';', (usize)text_len);
        i32 meta_len = semi ? (i32)(semi - text) : text_len;
        const char *payload = semi ? semi + 1 : text + text_len;
        i32 payload_len = semi ? text_len - meta_len - 1 : 0;
        char id[sizeof t->note99_id] = "";
        bool done = true, b64 = false, body = false, ours = true;
        for (i32 pos = 0; pos < meta_len; ) {
            i32 end = pos;
            while (end < meta_len && text[end] != ':') end++;
            if (end - pos >= 2 && text[pos + 1] == '=') {
                const char *val = text + pos + 2;
                i32 vlen = end - pos - 2;
                switch (text[pos]) {
                    case 'i': {
                        usize w = 0;
                        for (i32 q = 0; q < vlen && w + 1 < sizeof id; q++) {
                            char c = val[q];
                            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                c == '_' || c == '-' || c == '+' || c == '.') id[w++] = c;
                        }
                        id[w] = '\0';
                        break;
                    }
                    case 'd': done = !(vlen == 1 && val[0] == '0'); break;
                    case 'e': b64 = (vlen == 1 && val[0] == '1'); break;
                    case 'p':
                        if (vlen == 4 && memcmp(val, "body", 4) == 0) body = true;
                        else if (!(vlen == 5 && memcmp(val, "title", 5) == 0)) ours = false;
                        break;
                    default: break;
                }
            }
            pos = end + 1;
        }
        if (!ours) break;
        if (strcmp(id, t->note99_id) != 0) {            /* another notification: start over */
            snprintf(t->note99_id, sizeof t->note99_id, "%s", id);
            t->note99_title[0] = t->note99_body[0] = '\0';
            t->note99_title_full = t->note99_body_full = false;
        }
        char *dst = body ? t->note99_body : t->note99_title;
        usize cap = body ? sizeof t->note99_body : sizeof t->note99_title;
        bool *full = body ? &t->note99_body_full : &t->note99_title_full;
        if (b64 && payload_len > 0) {
            u8 raw[TERM_NOTIFY_BODY_CAP];
            /* 508 base64 characters are 381 bytes, all the text there is room
             * for. A SHORTER piece is decoded as it is, padded or not:
             * rounding its length down to a multiple of four would cut the
             * last letters off an unpadded payload. */
            bool cut = payload_len > 508;
            i32 take = cut ? 508 : payload_len;
            i32 n = base64_decode_cap((const u8 *)payload, take, raw, (i32)sizeof raw);
            /* The decoded bytes can stop inside a letter (where 381 bytes
             * end, or where the sender cut its pieces). */
            if (n > 0) n = osc_title_trim_partial_tail((const char *)raw, n);
            note99_append(dst, cap, (const char *)raw, n, full);
            if (cut) *full = true;
        } else {
            note99_append(dst, cap, payload, payload_len, full);
        }
        if (done) {
            notify_emit(t, t->note99_title, (i32)strlen(t->note99_title),
                        t->note99_body, (i32)strlen(t->note99_body));
            t->note99_id[0] = t->note99_title[0] = t->note99_body[0] = '\0';
            t->note99_title_full = t->note99_body_full = false;
        }
        break;
    }
    case 52: { /* OSC 52 — Clipboard access: OSC 52;selector;base64data ST
                * or OSC 52;selector;? ST (query — not supported, silent drop).
                * The selector chars (c/p/s/…) pick the clipboard target; only
                * the primary system clipboard is honoured. */
        /* The selector is ZERO or more of c p q s 0-7 — `52;;data` is what
         * tmux writes for its own copies and what `tput Ms ''` expands to,
         * and `52;pc;data` is legal. Whatever it names, there is one
         * clipboard here. */
        i32 sel = 0;
        while (sel < text_len && sel < 8 && text[sel] != ';' &&
               (strchr("cpqs", text[sel]) || (text[sel] >= '0' && text[sel] <= '7'))) sel++;
        if (sel < text_len && text[sel] == ';' && text_len >= sel + 2) {
            const char *payload = text + sel + 1;
            i32 payload_len = text_len - sel - 1;
            if (payload_len == 1 && payload[0] == '?') {
                /* Query: refuse silently. Honoring it would exfiltrate the
                 * clipboard to whatever process is currently writing to the
                 * terminal, which is a well-known exfiltration vector. */
            } else if (payload_len > 0 && payload_len < 1024 * 1024 &&
                       t->on_clipboard_set) {
                /* Route through the app callback so it can gate on
                 * config (allow_osc52_write) — writing the clipboard
                 * unconditionally lets any process that can emit bytes to
                 * this terminal silently replace the system clipboard. */
                u8 *decoded = malloc((usize)payload_len + 1);
                if (decoded) {
                    i32 n = base64_decode((const u8 *)payload,
                                          payload_len, decoded);
                    if (n > 0) {
                        decoded[n] = '\0';
                        t->on_clipboard_set(t, (const char *)decoded,
                                            t->userdata);
                    }
                    free(decoded);
                }
            }
        }
        break;
    }
    case 133: { /* OSC 133 — Semantic prompts (shell integration) */
        if (text_len >= 1) {
            char marker = text[0];
            switch (marker) {
            case 'A': /* Prompt start */
                t->progress_state = TERM_PROGRESS_NONE;   /* the command that reported it is over */
                t->progress_value = 0;
                t->current_zone = ZONE_PROMPT;
                t->prompt_start_row = t->cursor_y;
                t->input_start_row = -1;
                t->input_start_col = -1;
                /* The program that asked for a cursor shape is over. Most
                 * "reset" it on the way out with terminfo's Se, which for
                 * xterm-256color is `CSI 2 SP q` — a steady BLOCK, not "the
                 * user's own" — so the shape goes back to the setting here; a
                 * shell that wants one for its line editor sends it after. */
                t->cursor_style_set = false;
                /* A prompt from the shell Liu started: that shell will report
                 * the NEXT command itself (its C carries the text), so what is
                 * typed is not recorded a second time. The word is good for
                 * one command — C takes it back — which is what lets a pane
                 * whose shell was replaced (`exec zsh`, a nested bash, a
                 * plugin that unhooked the marks) go back to recording what is
                 * typed. */
                if (osc133_mark_is_ours(t, text, text_len)) {
                    bool was = t->history_integrated;
                    t->history_integrated = true;
                    if (!was && t->on_command_prompt)
                        t->on_command_prompt(t, t->command_userdata);
                }
                mark_current_row_zone(t);
                break;
            case 'B': /* Prompt end, input start */
                /* An unkeyed B is somebody else's (a shell with its own
                 * integration, a remote host): what is typed is recorded. */
                if (osc133_mark_is_ours(t, text, text_len)) {
                    bool was = t->history_integrated;
                    t->history_integrated = true;
                    if (!was && t->on_command_prompt)
                        t->on_command_prompt(t, t->command_userdata);
                } else {
                    t->history_integrated = false;
                }
                t->current_zone = ZONE_INPUT;
                /* The line editor starts again on every continuation line of
                 * a multi-line command and sends B each time. The FIRST one
                 * after a prompt is where the command begins; taking the last
                 * recorded `done` for a whole `for` loop. */
                if (t->input_start_row < 0 || t->input_start_col < 0) {
                    t->input_start_row = t->cursor_y;
                    t->input_start_col = t->cursor_x;
                }
                mark_current_row_zone(t);
                break;
            case 'C': /* Input end, command start */
                if (osc133_mark_is_ours(t, text, text_len)) {
                    /* Recorded from here only when this shell's prompt told
                     * the typed path to stand down — otherwise that path has
                     * the command already and D must not add it again. */
                    if (t->history_integrated) {
                        if (!osc133_take_command(t, text, text_len)) capture_pending_command(t);
                    } else if (t->pending_command) {
                        t->pending_command[0] = '\0';
                    }
                    t->history_integrated = false;
                }
                t->input_start_col = -1;   /* input area is gone; row stays for
                                            * the prompt-row shading in ui.c */
                t->current_zone = ZONE_OUTPUT;
                t->output_start_row = t->cursor_y;
                mark_current_row_zone(t);
                t->cmd_start_time = platform_time_sec();
                break;
            case 'D': { /* Command end */
                i32 exit_code = 0;
                if (text_len > 2) {
                    const char *exit_str = strstr(text, "exit=");
                    if (exit_str) {
                        (void)checked_parse_i32_prefix(exit_str + 5, &exit_code, NULL);
                    } else if (text[1] == ';') {
                        (void)checked_parse_i32_prefix(text + 2, &exit_code, NULL);
                    }
                }
                t->last_exit_code = exit_code;
                t->last_cmd_failed = (exit_code != 0);

                /* Record command end time and duration */
                t->cmd_end_time = platform_time_sec();
                if (t->cmd_start_time > 0) {
                    t->last_cmd_duration = t->cmd_end_time - t->cmd_start_time;
                    t->cmd_start_time = 0; /* reset for next command */
                    /* Notify if long-running command callback is set */
                    if (t->on_long_command) {
                        t->on_long_command(t, t->last_cmd_duration, exit_code, t->long_cmd_userdata);
                    }
                }

                /* The command's text is recorded only on the shell's own word
                 * (osc133_mark_is_ours); a D without the key leaves what the
                 * shell's C captured for the shell's D. */
                if (osc133_mark_is_ours(t, text, text_len)) {
                    if (t->on_command_finished && t->pending_command && t->pending_command[0])
                        t->on_command_finished(t, t->pending_command, exit_code, t->command_userdata);
                    if (t->pending_command) t->pending_command[0] = '\0';
                }
                t->current_zone = ZONE_NONE;
                break;
            }
            }
        }
        break;
    }
    }
}

/* =========================================================================
 * Main feed function — state machine
 * ========================================================================= */

/* Expected length of the UTF-8 sequence this lead byte starts (0 = not a
 * lead byte). Used to tell "sequence continues in the next read" apart from
 * "malformed", which utf8_decode reports identically. */
static u32 utf8_seq_len(u8 b) {
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 0;
}

/* Bulk-write the printable text that follows a non-ASCII character in the
 * CURRENT row: put_ascii_run for text that is not all ASCII.
 *
 * Without it, a Cyrillic, Greek, Turkish or box-drawing line pays put_char's
 * whole per-character machinery for every letter, plus a turn of the byte
 * loop and its state checks for the lead byte.
 *
 * What put_char does for a character that opens a fresh cell in the row, the
 * run does too, in the same order: the (0,0) history rule, splitting a wide
 * pair it overwrites, the content_writes count, the lead + spacer of a wide
 * character, and where the last one landed for a joiner to fold into. It
 * STOPS — and leaves the byte to the per-character path — at anything that
 * path decides differently: a joiner or anything else of width 0 (it folds
 * into the cell before it), a regional indicator (the second of a flag pair
 * folds into the first), a wide character with one column left (the spacer
 * and the wrap), the end of the row (the wrap and the scroll), a control
 * byte, a sequence cut by the end of the read or not UTF-8 at all, and ASCII
 * while a DEC character set is in use. Like put_ascii_run it declines an
 * armed sparse underline and IRM, and it declines a cluster still open on a
 * ZWJ, which takes the next character whatever it is.
 *
 * Returns how many bytes it consumed. The terminal it leaves is the one
 * byte-at-a-time feeding, which never forms a run, would leave. */
static usize put_text_run(Terminal *t, const u8 *src, usize len) {
    if (len == 0) return 0;
#ifdef LIU_ASCII_RUN_SWITCH
    extern bool liu_ascii_run_disabled;
    if (liu_ascii_run_disabled) return 0;
#endif
    if (cell_has_decoration(t->cursor_attr.flags) &&
        (t->cursor_ul_color != 0 || t->cursor_ul_style > 1 || t->ul_map_count > 0))
        return 0;
    if (t->mode & MODE_INSERT) return 0;
    if (t->cursor_y < 0 || t->cursor_y >= t->rows || t->cursor_x < 0) return 0;
    if (utf8_is_cluster(t->last_printed_char)) return 0;

    const bool dec = t->charset_g[t->charset_gl] != 0;
    const CellAttr attr = t->cursor_attr;
    const i32 cols = t->cols, y = t->cursor_y;
    Cell *row = &t->cells[y * cols];
    i32 x = t->cursor_x, last_x = x;
    u32 last_cp = 0;
    u64 changed = 0;
    usize i = 0;
    while (i < len && x < cols) {
        const u8 b = src[i];
        u32 cp, used;
        int width;
        if (b < 0x80) {
            if (b < 0x20 || b == 0x7F || dec) break;
            cp = b; used = 1; width = 1;
        } else {
            const u32 need = utf8_seq_len(b);
            if (need < 2 || len - i < need) break;
            used = utf8_decode(src + i, len - i, &cp);
            if (used == 0) break;
            width = utf8_char_width(cp);
            if (width <= 0 || (cp >= 0x1F1E6 && cp <= 0x1F1FF)) break;
            if (width == 2 && x + 1 >= cols) break;
        }
        Cell *c = &row[x];
        if (x == 0 && y == 0) wrap_break_above_screen(t);
        if (c->attr.flags & (ATTR_WIDE | ATTR_WDUMMY)) wide_unpair(t, x, y);
        if (width == 2 && (c[1].attr.flags & (ATTR_WIDE | ATTR_WDUMMY))) wide_unpair(t, x + 1, y);
        if (c->codepoint != cp) changed++;
        c->codepoint = cp;
        c->attr = attr;
        last_x = x;
        last_cp = cp;
        if (width == 2) {
            c->attr.flags |= ATTR_WIDE;
            c[1].codepoint = ' ';
            c[1].attr = attr;
            c[1].attr.flags |= ATTR_WDUMMY;
            x += 2;
        } else {
            x++;
        }
        i += used;
    }
    if (i == 0) return 0;

    t->content_writes += changed;
    t->cluster_x = last_x;
    t->cluster_y = y;
    t->last_printed_char = last_cp;
    t->cursor_x = x;
    if (t->cursor_x >= cols) {
        wrap_break_rows(t, y, y);
        if (!(t->mode & MODE_WRAP)) t->cursor_x = cols - 1;   /* as put_char */
    }
    mark_current_row_zone(t);
    TERM_DIRTY_ROW(t, y);
    return i;
}

/* ---- control strings can be cancelled ----------------------------------
 *
 * ECMA-48 and the DEC ANSI parser state machine both let a stream escape from
 * a control string it did not mean to start: CAN (0x18) and SUB (0x1A) abort
 * it, and an ESC that is not the start of ST (`ESC \`) abandons it and begins
 * whatever sequence that ESC introduces. Waiting for BEL or ST and nothing
 * else would let a lone `printf '\033]2;x'` — anything that prints a
 * truncated or hostile string, a `cat` of a binary — leave the parser inside
 * the OSC for good: CAN, SUB, `ESC c` (RIS, i.e. `reset`) and `ESC [` would
 * all be swallowed as payload, every following byte hidden, and a RESET could
 * not recover it. In CSI too the ESC aborts the sequence and begins its own,
 * so `ESC [ 1 ESC c` resets.
 *
 * A cancelled string is DROPPED, never dispatched: acting on a truncated OSC 52
 * or APC payload is exactly what a hostile writer wants. */
static bool str_prev_was_esc(const Terminal *t) {
    switch (t->parse_state) {
    case STATE_OSC:
    case STATE_DCS:
        return t->dcs_buf ? (t->dcs_len > 0 && t->dcs_buf[t->dcs_len - 1] == 0x1B)
                          : (t->esc_len > 0 && t->esc_buf[t->esc_len - 1] == 0x1B);
    case STATE_APC:
        return t->apc_buf ? (t->apc_len > 0 && t->apc_buf[t->apc_len - 1] == 0x1B)
                          : (t->esc_len > 0 && t->esc_buf[t->esc_len - 1] == 0x1B);
    case STATE_STR_SINK:
        return t->str_sink_esc;
    default:
        return false;
    }
}

static void str_cancel(Terminal *t) {
    terminal_str_bufs_free(t);
    t->esc_len = 0;
    t->str_sink_esc = false;
}

/* A C0 control met in the middle of a CSI sequence. ESC, CAN and SUB never
 * get here: they end the sequence before the state switch. */
static void exec_c0_in_sequence(Terminal *t, u8 b) {
    switch (b) {
    case '\n': case 0x0B: case 0x0C: cursor_index(t); break;
    case '\r': t->cursor_x = 0; break;
    case '\b':
        if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
        if (t->cursor_x > 0) t->cursor_x--;
        break;
    case '\t':
        if (t->cursor_x < t->cols) {
            t->cursor_x++;
            while (t->cursor_x < t->cols && !t->tab_stops[t->cursor_x]) t->cursor_x++;
            if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
        }
        break;
    case '\a': if (t->on_bell) t->on_bell(t, t->userdata); break;
    default: break;
    }
}

/* What GROUND does when it sees ESC; the cancel paths enter STATE_ESC the same
 * way so the next byte is read as an escape-sequence introducer. */
static void enter_escape_state(Terminal *t) {
    t->parse_state = STATE_ESC;
    t->esc_len = 0;
    t->param_count = 0;
    t->param_has_value = false;
    t->param_sub = 0;
    t->param_overflow = false;
    memset(t->params, 0, sizeof(t->params));
}

void terminal_feed(Terminal *t, const u8 *data, usize len) {
    if (t && len > 0) t->search_revision++;
    t->dirty = true;
    TERM_DIRTY_ROW(t, t->cursor_y);

    /* Finish a sequence the previous read cut in half. Only GROUND can be
     * mid-codepoint: escape sequences are ASCII, so nothing else carries a
     * pending head. */
    if (t->utf8_pending_len) {
        u8 head_len = t->utf8_pending_len;
        t->utf8_pending_len = 0;
        u32 need = utf8_seq_len(t->utf8_pending[0]);
        if (need >= 2 && need <= 4 && t->parse_state == STATE_GROUND) {
            usize want = need - head_len;
            /* Only continuation bytes can finish it. Taking whatever came next
             * — a newline, an ESC — would make legacy-encoded text (a
             * Windows-1254 file) lose line breaks and print the tails of its
             * escape sequences, differently for every way the pty happened to
             * cut the stream. */
            usize have = len < want ? len : want;
            bool tail_ok = true;
            for (usize k = 0; k < have; k++)
                if ((data[k] & 0xC0) != 0x80) { tail_ok = false; break; }
            if (!tail_ok) {
                /* The head was not UTF-8. It and whatever continuation bytes
                 * followed it are ONE ill-formed sequence and get one
                 * replacement character — the same count a single read gives. */
                usize lead_tail = 0;
                while (lead_tail < have && (data[lead_tail] & 0xC0) == 0x80) lead_tail++;
                data += lead_tail;
                len  -= lead_tail;
                put_char(t, 0xFFFD);
            } else if (len < want) {
                /* Still short (a 4-byte emoji can span three tiny reads).
                 * Keep accumulating rather than dropping it. */
                if ((usize)head_len + len <= sizeof(t->utf8_pending)) {
                    memcpy(t->utf8_pending + head_len, data, len);
                    t->utf8_pending_len = (u8)(head_len + len);
                    return;
                }
            } else {
                u8 seq[4];
                memcpy(seq, t->utf8_pending, head_len);
                memcpy(seq + head_len, data, want);
                u32 cp;
                if (utf8_decode(seq, need, &cp) == need) put_char(t, cp);
                else put_char(t, 0xFFFD);
                data += want;
                len  -= want;
            }
        }
    }
    /* Do NOT yank the view to the bottom on output. While the user has scrolled
     * up into history (scroll_offset > 0) — e.g. reading back through a running
     * agent's stream — buffer_scroll_up keeps their view anchored as new lines
     * enter scrollback. The view returns to the live bottom only when the user
     * scrolls down to it or types (app_send_input snaps it). */

    for (usize pos = 0; pos < len; pos++) {
        u8 b = data[pos];

        /* Cancel paths — see str_prev_was_esc(). Before the state switch so a
         * cancelled string never reaches its own accumulator, and the byte that
         * follows an abandoning ESC is read by STATE_ESC below. */
        if (t->parse_state == STATE_OSC || t->parse_state == STATE_DCS ||
            t->parse_state == STATE_APC || t->parse_state == STATE_STR_SINK) {
            if (b == 0x18 || b == 0x1A) {          /* CAN, SUB */
                str_cancel(t);
                t->parse_state = STATE_GROUND;
                continue;
            }
            if (b != '\\' && b != '\a' && b != 0x1B && str_prev_was_esc(t)) {
                str_cancel(t);
                enter_escape_state(t);              /* b is read as the introducer */
            }
        } else if (t->parse_state == STATE_CSI) {
            if (b == 0x18 || b == 0x1A) {          /* CAN, SUB: abort, as before */
                t->parse_state = STATE_GROUND;
                continue;
            }
            if (b == 0x1B) {                       /* ESC: abort AND start a new one */
                enter_escape_state(t);
                continue;
            }
        }

        switch (t->parse_state) {
        case STATE_GROUND:
            if (b == 0x1B) { /* ESC */
                t->parse_state = STATE_ESC;
                t->esc_len = 0;
                t->param_count = 0;
                t->param_has_value = false;
                t->param_sub = 0;
                t->param_overflow = false;
                memset(t->params, 0, sizeof(t->params));
                /* Complete, single-parameter ANSI colors are common in logs.
                 * Fold their five byte transitions only when the whole exact
                 * sequence is available; all partial/extended forms still go
                 * through the general state machine. Keep its scratch state
                 * as well as the resulting color identical. */
                if (len - pos >= 5 && data[pos + 4] == 'm' && data[pos + 1] == '[' &&
                    (data[pos + 2] == '3' || data[pos + 2] == '4' ||
                     data[pos + 2] == '9') &&
                    data[pos + 3] >= '0' && data[pos + 3] <= '7') {
                    i32 sgr = (data[pos + 2] - '0') * 10 + data[pos + 3] - '0';
                    u32 color = (u32)(data[pos + 3] - '0');
                    if (data[pos + 2] == '9') color += 8;
                    if (data[pos + 2] == '4') t->cursor_attr.bg = color;
                    else t->cursor_attr.fg = color;
                    t->params[0] = sgr;
                    t->param_count = 1;
                    t->param_has_value = true;
                    t->parse_state = STATE_GROUND;
                    pos += 4;
                }
            } else if (b == '\n' || b == '\x0B' || b == '\x0C') {
                cursor_index(t);
                /* New-line mode (LNM, CSI 20 h): a line feed also returns the
                 * carriage. */
                if (t->mode & MODE_CRLF) t->cursor_x = 0;
            } else if (b == '\r') {
                t->cursor_x = 0;
            } else if (b == '\b') {
                if (t->cursor_x > 0) t->cursor_x--;
            } else if (b == '\t') {
                /* Advance to next tab stop. With the wrap pending (the cursor
                 * parked past the last column) a tab moves nothing and the
                 * wrap stays pending: clamping here would cancel it, and the
                 * next character would overwrite the last column. */
                if (t->cursor_x < t->cols) {
                    t->cursor_x++;
                    while (t->cursor_x < t->cols && !t->tab_stops[t->cursor_x]) {
                        t->cursor_x++;
                    }
                    if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
                }
            } else if (b == '\a') { /* Bell */
                if (t->on_bell) t->on_bell(t, t->userdata);
            } else if (b == 0x0E || b == 0x0F) {
                /* SO / SI: printable bytes go through G1 / G0 from here. */
                t->charset_gl = (b == 0x0E) ? 1 : 0;
            } else if (b == 0x7F) {
                /* DEL is ignored. As a character it would join a cluster left
                 * open by a ZWJ and put a control byte inside a cell. */
            } else if (b >= 0x20) {
                /* Printable character — handle UTF-8 */
                u32 codepoint;
                if (b < 0x80) {
                    /* Line drawing in use: one character at a time, through
                     * the table (the bulk path writes bytes as they are). */
                    if (t->charset_g[t->charset_gl]) {
                        put_char(t, dec_graphics(b));
                        break;
                    }
                    usize remaining = len - pos;
                    int scan_len = remaining > (usize)INT_MAX ? INT_MAX : (int)remaining;
                    int run = vt_printable_run(data + pos, scan_len);
                    if (run > 1) {
                        /* put_char owns the first byte of every row-chunk, and
                         * that is not just tidiness. It is the only byte that
                         * can fold into a pending cluster — an ASCII byte right
                         * after a ZWJ extends it — and it is what performs the
                         * wrap and the scroll when the cursor is parked at the
                         * right edge. Every cell put_ascii_run then fills was
                         * written by this run, so nothing after it can absorb.
                         *
                         * put_char always advances `done`, so a run that
                         * declines the bulk path (sparse underline armed)
                         * degrades to a per-character loop rather than
                         * spinning. */
                        usize done = 0;
                        while (done < (usize)run) {
                            put_char(t, data[pos + done]);
                            done++;
                            if (done >= (usize)run) break;
                            done += put_ascii_run(t, data + pos + done,
                                                  (usize)run - done);
                        }
                        pos += (usize)run - 1;
                        break;
                    }
                    put_char(t, b);
                } else {
                    /* Sequence runs past the end of this read: stash the head
                     * and pick it up when the rest arrives. Dropping it here
                     * is what turned 🇯🇵 into a lone 🇯 whenever a PTY read
                     * landed between the two halves of the flag. */
                    u32 need = utf8_seq_len(b);
                    if (need > 1 && (len - pos) < need) {
                        /* Stash it only while it can still become a character:
                         * every byte after the lead has to be a continuation. */
                        bool tail_ok = true;
                        for (usize k = pos + 1; k < len; k++)
                            if ((data[k] & 0xC0) != 0x80) { tail_ok = false; break; }
                        if (tail_ok) {
                            memcpy(t->utf8_pending, data + pos, len - pos);
                            t->utf8_pending_len = (u8)(len - pos);
                            return;
                        }
                    }
                    u32 consumed = utf8_decode(data + pos, len - pos, &codepoint);
                    if (consumed > 0) {
                        /* Joiners (ZWJ, variation selectors, skin tones,
                         * combining marks) are not dropped here: put_char
                         * folds them into the cell the base opened. See the
                         * cluster banner above put_char. */
                        put_char(t, codepoint);
                        pos += consumed - 1;
                        /* ...and the rest of the row's text in one pass. */
                        pos += put_text_run(t, data + pos + 1, len - pos - 1);
                    } else {
                        /* Not UTF-8 (a stray continuation byte, a lead with
                         * the wrong tail, an overlong form): one replacement
                         * character for the lead and the continuation bytes
                         * that came with it, and the next byte is read for
                         * what it is. Dropping them would shift every
                         * column. */
                        u32 tail = 0;
                        while (need > 1 && tail < need - 1 && pos + 1 + tail < len &&
                               (data[pos + 1 + tail] & 0xC0) == 0x80) tail++;
                        put_char(t, 0xFFFD);
                        pos += tail;
                    }
                }
            }
            break;

        case STATE_ESC:
            if (b == '[') {
                t->parse_state = STATE_CSI;
                t->esc_len = 0;
            } else if (b == ']') {
                t->parse_state = STATE_OSC;
                t->esc_len = 0;
            } else if (b == 'P') {
                t->parse_state = STATE_DCS;
                t->esc_len = 0;
            } else if (b == '#') {
                t->parse_state = STATE_ESC_HASH;
            } else if (b == '(' || b == ')') {
                t->esc_buf[0] = (char)b;             /* which of G0 / G1 the next byte designates */
                t->esc_len = 1;
                t->parse_state = STATE_CHARSET;
            } else if (b >= 0x20 && b <= 0x2F) {
                t->parse_state = STATE_ESC_SKIP;
            } else if (b == '7') { /* DECSC — Save Cursor */
                cursor_save(t);
                t->parse_state = STATE_GROUND;
            } else if (b == '8') { /* DECRC — Restore Cursor */
                cursor_restore(t);
                t->parse_state = STATE_GROUND;
            } else if (b == 'H') { /* HTS — Horizontal Tab Set */
                /* The other half of the tab family. Liu.terminfo advertises
                 * `hts=\EH` and `tbc=\E[3g`, so a program is entitled to
                 * clear the default stops and lay down its own — and with HTS
                 * dropped it got a terminal that still tabbed every 8 columns.
                 * cursor_x can be `cols` in the deferred-wrap state, which is
                 * not a column the array has. */
                if (t->tab_stops && t->cursor_x >= 0 && t->cursor_x < t->cols)
                    t->tab_stops[t->cursor_x] = true;
                t->parse_state = STATE_GROUND;
            } else if (b == 'D') { /* IND — Index (scroll up) */
                cursor_index(t);
                t->parse_state = STATE_GROUND;
            } else if (b == 'M') { /* RI — Reverse Index (scroll down) */
                cursor_reverse_index(t);
                t->parse_state = STATE_GROUND;
            /* HTS is handled once, further up this chain. At the deferred-wrap
             * column it is ignored: clamping to `cols - 1` and setting a stop
             * there would invent a stop the program never asked for. */
            } else if (b == 'E') { /* NEL — Next Line */
                t->cursor_x = 0;
                cursor_index(t);
                t->parse_state = STATE_GROUND;
            } else if (b == 'c') { /* RIS — Full Reset */
                t->search_layout_generation++;
                /* Everything RIS is defined to restore is done by hand below —
                 * terminal_resize early-outs when neither dimension changes, so
                 * it cannot serve as a reset. DECSTR (CSI ! p) does the
                 * soft-reset subset of the same list and the two are
                 * deliberately kept in step.
                 *
                 * Leaving the alt screen comes FIRST: the erase has to land on
                 * the primary buffer, and swapping afterwards would put the
                 * cleared alt grid back on screen. The alt allocation is then
                 * released rather than parked — a reset that kept one would
                 * leave its scroll slide behind, and a full reset owning a
                 * hidden second grid is wrong on its own terms. Unsliding the
                 * live grid afterwards is the same discipline terminal_resize
                 * applies before it touches t->cells. */
                if (t->mode & MODE_ALT_SCREEN) {
                    terminal_grid_swap_alt(t);
                    t->mode &= ~MODE_ALT_SCREEN;
                }
                memset(&t->parked, 0, sizeof t->parked);
                t->charset_g[0] = t->charset_g[1] = 0;
                t->charset_gl = 0;
                terminal_grid_free_alt(t);
                terminal_underline_drop_parked(t);
                terminal_grid_unslide(t);
                gfx_transfer_free(t);   /* an image half sent is not something a reset keeps */
                t->progress_state = TERM_PROGRESS_NONE;
                t->progress_value = 0;
                t->note99_id[0] = t->note99_title[0] = t->note99_body[0] = '\0';
                t->note99_title_full = t->note99_body_full = false;

                t->mode = MODE_WRAP | MODE_GRAPHEME;  /* terminal_create's default */
                t->modify_other_keys = 0;
                t->sync_pending = false;
                t->sync_start_time = 0;
                t->Liu_kbd_flags = 0;
                t->Liu_kbd_stack_len = 0;

                t->cursor_attr = (CellAttr){ .fg = FG_DEFAULT, .bg = BG_DEFAULT, .flags = 0 };
                t->cursor_ul_color = 0;
                t->cursor_ul_style = 0;
                t->cursor_x = 0;
                t->cursor_y = 0;
                t->cursor_visible = true;
                t->cursor_style = CURSOR_BLOCK;
                t->cursor_style_set = false;
                cursor_saved_reset(t);
                t->parked.saved_attr = t->saved_attr;

                t->scroll_top = 0;
                t->scroll_bottom = t->rows - 1;

                /* Tab stops back to every eighth column, the power-on state. */
                if (t->tab_stops) {
                    memset(t->tab_stops, 0, (usize)t->cols * sizeof(bool));
                    for (i32 tx = 8; tx < t->cols; tx += 8) t->tab_stops[tx] = true;
                }

                /* Erase the screen and the decorations that hang off it. The
                 * scrollback deliberately SURVIVES: a stray `reset` must not
                 * throw away the user's history. */
                buffer_clear_region(t->cells, t->cols, 0, 0, t->cols - 1,
                                    t->rows - 1, t->cursor_attr);
                terminal_clear_underline_map(t);
                if (t->line_wrapped)
                    memset(t->line_wrapped, 0, (usize)t->rows * sizeof(bool));
                t->title[0] = '\0';
                t->title_changed = true;
                t->title_stack_len = 0;
                TERM_DIRTY_ALL(t);

                t->parse_state = STATE_GROUND;
            } else if (b == '=') { /* DECKPAM */
                t->mode |= MODE_APP_KEYPAD;
                t->parse_state = STATE_GROUND;
            } else if (b == '>') { /* DECKPNM */
                t->mode &= ~MODE_APP_KEYPAD;
                t->parse_state = STATE_GROUND;
            } else if (b == '_') { /* APC — Application Program Command */
                t->parse_state = STATE_APC;
                t->esc_len = 0;
            } else if (b == '^' || b == 'X') {
                /* PM (privacy message) and SOS (start of string): control
                 * strings Liu never acts on. They are swallowed to ST so their
                 * BODY is not printed into the grid as ordinary text; the sink
                 * is cancellable like the rest (CAN / SUB / ESC), so an
                 * unterminated one cannot wedge the parser. */
                t->parse_state = STATE_STR_SINK;
                t->str_sink_esc = false;
            } else {
                t->parse_state = STATE_GROUND;
            }
            break;

        case STATE_APC:
            /* APC — collect until ST (ESC \) or BEL, then dispatch */
            if (b == '\\' && ((t->apc_buf && t->apc_len > 0 && t->apc_buf[t->apc_len-1] == 0x1B) ||
                              (!t->apc_buf && t->esc_len > 0 && t->esc_buf[t->esc_len-1] == 0x1B))) {
                /* APC terminated by ST */
                u8 *apc_data = t->apc_buf ? t->apc_buf : t->esc_buf;
                i32 apc_len2 = t->apc_buf ? t->apc_len : t->esc_len;
                apc_len2--; /* remove trailing ESC */

                /* Liu graphics: starts with 'G' */
                if (apc_len2 > 1 && apc_data[0] == 'G') {
                    handle_Liu_graphics(t, apc_data + 1, apc_len2 - 1);
                }

                str_buf_free(&t->apc_buf, &t->apc_len, &t->apc_cap);
                t->parse_state = STATE_GROUND;
            } else if (b == '\a') {
                /* APC terminated by BEL */
                u8 *apc_data = t->apc_buf ? t->apc_buf : t->esc_buf;
                i32 apc_len2 = t->apc_buf ? t->apc_len : t->esc_len;

                if (apc_len2 > 1 && apc_data[0] == 'G') {
                    handle_Liu_graphics(t, apc_data + 1, apc_len2 - 1);
                }

                str_buf_free(&t->apc_buf, &t->apc_len, &t->apc_cap);
                t->parse_state = STATE_GROUND;
            } else {
                /* Accumulate APC data — use dynamic buffer for large payloads (images) */
                if (t->esc_len < (i32)sizeof(t->esc_buf) - 1 && !t->apc_buf) {
                    t->esc_buf[t->esc_len++] = b;
                } else {
                    /* Overflow to dynamic buffer */
                    if (!t->apc_buf) {
                        t->apc_buf = str_buf_alloc(65536);
                        /* cap stays 0 on failure: it is the accounted amount,
                         * not a wish. */
                        t->apc_cap = t->apc_buf ? 65536 : 0;
                        if (t->apc_buf) {
                            memcpy(t->apc_buf, t->esc_buf, (usize)t->esc_len);
                            t->apc_len = t->esc_len;
                        }
                    }
                    if (t->apc_buf) {
                        if (t->apc_len >= t->apc_cap) {
                            i32 new_cap = t->apc_cap + (t->apc_cap >> 1); /* 1.5x growth */
                            if (new_cap < t->apc_cap + 4096) new_cap = t->apc_cap + 4096;
                            if (new_cap > 16 * 1024 * 1024) {
                                /* Cap at 16MB to prevent runaway */
                                str_buf_free(&t->apc_buf, &t->apc_len, &t->apc_cap);
                                t->parse_state = STATE_STR_SINK;
                                t->str_sink_esc = false;
                                break;
                            }
                            u8 *grown = str_buf_grow(t->apc_buf, t->apc_cap, new_cap);
                            if (!grown) {
                                str_buf_free(&t->apc_buf, &t->apc_len, &t->apc_cap);
                                t->parse_state = STATE_STR_SINK;
                                t->str_sink_esc = false;
                                break;
                            }
                            t->apc_buf = grown;
                            t->apc_cap = new_cap;
                        }
                        if (t->apc_buf) t->apc_buf[t->apc_len++] = b;
                    }
                }
            }
            break;

        case STATE_STR_SINK:
            /* Drop the rest of a string sequence that was refused, terminator
             * and all. Termination is tested exactly the way the accumulating
             * paths above test it — BEL, or `\` with ESC immediately before —
             * so a stream that ends one way ends the other way too, and a
             * refused payload can never desynchronise the parser relative to
             * an accepted one.
             *
             * Deliberately UNBOUNDED. There is no byte count at which printing
             * a program's payload becomes the better answer, and a cap would
             * not restore sync anyway: an unterminated sequence that simply
             * STOPS parks the parser forever, cap or no cap. All a cap would
             * decide is whether megabytes of base64 get typed into the
             * scrollback on the way. */
            if (b == '\a') {
                t->parse_state = STATE_GROUND;
                t->str_sink_esc = false;
            } else if (b == '\\' && t->str_sink_esc) {
                t->parse_state = STATE_GROUND;
                t->str_sink_esc = false;
            } else {
                t->str_sink_esc = (b == 0x1B);
            }
            break;

        case STATE_CSI:
            if (b >= '0' && b <= '9') {
                /* Past the last slot the digits are dropped, not appended to
                 * it: appended, `…;3;1` would become `31` and a long SGR
                 * would end in an attribute nobody sent. */
                if (!t->param_overflow) {
                    if (t->param_count == 0) t->param_count = 1;
                    t->params[t->param_count - 1] = decimal_push_bounded(
                        t->params[t->param_count - 1], b, 1000000);
                    t->param_has_value = true;
                }
            } else if (b == ';' || b == ':') {
                /* `;` separates parameters; `:` introduces a SUB-parameter of
                 * the one before it (4:3, 38:2::r:g:b). The two are kept apart,
                 * so a handler never has to guess which a number is. */
                if (!t->param_has_value && t->param_count == 0) t->param_count = 1;
                if (t->param_count < TERM_MAX_PARAMS) {
                    if (b == ':') t->param_sub |= 1u << t->param_count;
                    t->param_count++;
                    t->param_has_value = false;
                } else {
                    t->param_overflow = true;
                }
            } else if ((b >= 0x20 && b <= 0x2F) || (b >= 0x3C && b <= 0x3F)) {
                /* A private marker (< = > ?) or an intermediate (SP ! " # $ %
                 * & ' ( ) * + , - . /): part of the sequence's NAME. Every one
                 * is kept; ending the sequence at one would print its final
                 * byte (`CSI 1 " q` would leave a q on the screen). */
                if (t->esc_len < (i32)sizeof(t->esc_buf) - 1) {
                    t->esc_buf[t->esc_len++] = b;
                }
            } else if (b < 0x20) {
                /* A C0 control inside a sequence is carried out and the
                 * sequence goes on (ECMA-48). */
                exec_c0_in_sequence(t, b);
            } else if (b >= 0x40 && b <= 0x7E) {
                /* Final byte — dispatch */
                bool is_dec = (t->esc_len > 0 && t->esc_buf[0] == '?');
                bool is_gt  = (t->esc_len > 0 && t->esc_buf[0] == '>');
                bool is_bang = (t->esc_len > 0 && t->esc_buf[0] == '!');
                bool is_space = (t->esc_len > 0 && t->esc_buf[0] == ' ');
                bool is_equal = (t->esc_len > 0 && t->esc_buf[0] == '=');
                bool is_dollar = false;
                for (i32 ei = 0; ei < t->esc_len; ei++) { if (t->esc_buf[ei] == '$') is_dollar = true; }
                /* The intermediate is part of which function this is. The
                 * three Liu implements with one are named here; any other
                 * combination is a function Liu does not have, and it is
                 * dropped whole. If the final byte alone chose the handler,
                 * DECCARA (`CSI … $ r`) would run DECSTBM and home the
                 * cursor, `CSI 2 SP @` (scroll left) would insert blanks,
                 * and `CSI = c` would be answered as if it were `CSI c`. */
                bool has_inter = false;
                char marker = 0;
                for (i32 ei = 0; ei < t->esc_len; ei++) {
                    u8 e = (u8)t->esc_buf[ei];
                    if (e >= 0x20 && e <= 0x2F) has_inter = true;
                    else if (!marker) marker = (char)e;
                }
                bool known =
                    !has_inter ||
                    (is_space && b == 'q' && !marker) ||
                    (is_bang && b == 'p' && !marker) ||
                    (is_dollar && b == 'p' && (!marker || marker == '?'));
                if (known && (marker == '=' || marker == '<') && b != 'u') known = false;
                if (!known) {
                    t->parse_state = STATE_GROUND;
                    break;
                }

                if (!is_dec && !is_gt && !is_bang && !is_space &&
                    (b == 'h' || b == 'l')) {
                    /* ANSI SM/RM — the non-private twin of CSI ? h/l, and what
                     * sets MODE_INSERT and MODE_CRLF. */
                    handle_ansi_mode(t, b == 'h');
                } else if (is_dec) {
                    if (b == 'h') handle_dec_mode(t, true);
                    else if (b == 'l') handle_dec_mode(t, false);
                    else if (b == 'n') {
                        /* DEC DSR variants */
                        i32 ps = param(t, 0, 0);
                        if (ps == 6) {
                            /* Extended cursor position (see CPR) */
                            i32 row, col;
                            char buf[48];
                            cursor_report_position(t, &row, &col);
                            usize n = term_reply_cpr(buf, sizeof buf, row, col, true);
                            if (n) emit_response(t, buf, (i32)n);
                        } else if (ps == 996) {
                            /* "Is the terminal dark or light?" — 1 dark,
                             * 2 light, from the theme's background; silent
                             * while no theme is known. */
                            bool dark;
                            if (terminal_theme_is_dark(&dark))
                                emit_response_fmt(t, "\x1b[?997;%dn", dark ? 1 : 2);
                        }
                    }
                    else if (b == 'u') {
                        /* CSI ? u — which keyboard-protocol flags are on. It
                         * is how a program finds out the protocol exists
                         * (the query, then DA1): without an answer Neovim,
                         * fish and everything built on crossterm's
                         * supports_keyboard_enhancement leave it off.
                         *
                         * The answer is a number of the terminal's own, like
                         * DECRQM's: the five flag bits and nothing else of
                         * whatever a program pushed. */
                        emit_response_fmt(t, "\x1b[?%uu", t->Liu_kbd_flags & 31u);
                    }
                    else if (b == 'p' && is_dollar) {
                        /* DECRQM — Request Mode (CSI ? Ps $ p) */
                        i32 mode = param(t, 0, 0);
                        i32 val = 0; /* 0=not recognized */
                        switch (mode) {
                        case 1: val = (t->mode & MODE_APP_CURSOR) ? 1 : 2; break;
                        case 6: val = (t->mode & MODE_ORIGIN) ? 1 : 2; break;
                        case 7: val = (t->mode & MODE_WRAP) ? 1 : 2; break;
                        case 25: val = t->cursor_visible ? 1 : 2; break;
                        case 1000: val = (t->mode & MODE_MOUSE_BTN) ? 1 : 2; break;
                        case 1002: val = (t->mode & MODE_MOUSE_MOTION) ? 1 : 2; break;
                        case 1003: val = (t->mode & MODE_MOUSE_ANY) ? 1 : 2; break;
                        case 1004: val = (t->mode & MODE_FOCUS_EVENT) ? 1 : 2; break;
                        case 1006: val = (t->mode & MODE_MOUSE_SGR) ? 1 : 2; break;
                        case 1007: val = (t->mode & MODE_ALT_SCROLL) ? 1 : 2; break;
                        case 47: case 1047:
                        case 1049: val = (t->mode & MODE_ALT_SCREEN) ? 1 : 2; break;
                        case 1015: val = (t->mode & MODE_MOUSE_URXVT) ? 1 : 2; break;
                        case 1016: val = (t->mode & MODE_MOUSE_PIXEL) ? 1 : 2; break;
                        case 2004: val = (t->mode & MODE_BRACKETED_PASTE) ? 1 : 2; break;
                        case 2026: val = (t->mode & MODE_SYNC_OUTPUT) ? 1 : 2; break;
                        case 2027: val = (t->mode & MODE_GRAPHEME) ? 1 : 2; break;
                        case 2031: val = (t->mode & MODE_COLOR_SCHEME) ? 1 : 2; break;
                        case 2048: val = (t->mode & MODE_IN_BAND_RESIZE) ? 1 : 2; break;
                        case 2016: val = (t->modify_other_keys > 0) ? 1 : 2; break;
                        }
                        emit_response_fmt(t, "\x1b[?%d;%d$y", mode, val);
                    }
                } else if (is_gt) {
                    if (b == 'c') {
                        /* Secondary DA */
                        emit_response(t, TERM_REPLY_DA2, (i32)sizeof TERM_REPLY_DA2 - 1); /* type=1, version=1.00 */
                    } else if (b == 'q') {
                        /* XTVERSION */
                        emit_response(t, TERM_REPLY_XTVERSION, (i32)sizeof TERM_REPLY_XTVERSION - 1);
                    } else if (b == 'm') {
                        /* CSI > 4 ; N m — modifyOtherKeys */
                        i32 key_res = param(t, 0, 0);
                        if (key_res == 4) {
                            i32 level = param(t, 1, 0);
                            if (level >= 0 && level <= 2) {
                                t->modify_other_keys = (u8)level;
                                if (level > 0)
                                    t->mode |= MODE_MODIFY_OTHER_KEYS;
                                else
                                    t->mode &= ~MODE_MODIFY_OTHER_KEYS;
                            }
                        }
                    } else if (b == 'u') {
                        /* Liu keyboard: CSI > flags u — push */
                        i32 flags = param(t, 0, 0);
                        if (t->Liu_kbd_stack_len >= 8) {
                            /* Full: the oldest entry goes, as the protocol says. */
                            memmove(&t->Liu_kbd_stack[0], &t->Liu_kbd_stack[1],
                                    7 * sizeof t->Liu_kbd_stack[0]);
                            t->Liu_kbd_stack_len = 7;
                        }
                        t->Liu_kbd_stack[t->Liu_kbd_stack_len++] = t->Liu_kbd_flags;
                        t->Liu_kbd_flags = (u32)flags;
                    }
                } else if (is_bang && b == 'p') {
                    /* DECSTR — Soft Terminal Reset. It puts back what the DEC
                     * table lists and nothing else: the rendition, the cursor's
                     * visibility, the margins, insert / origin / cursor-key /
                     * keypad modes, the character sets, and the SAVED cursor
                     * (to home). Two things it must not do:
                     *   - move the cursor. `tput init` sends DECSTR with no
                     *     clear, and the prompt would jump to the top-left over
                     *     whatever was there;
                     *   - assign the mode word wholesale. That would drop
                     *     MODE_ALT_SCREEN without swapping the grids, so a
                     *     soft reset inside a full-screen program would leave
                     *     the shell's screen parked for good and feed the
                     *     program's rows into the scrollback. */
                    t->cursor_attr.fg = FG_DEFAULT;
                    t->cursor_attr.bg = BG_DEFAULT;
                    t->cursor_ul_color = 0;
                    t->cursor_attr.flags = 0;
                    t->cursor_ul_style = 0;
                    t->cursor_visible = true;
                    t->cursor_style = CURSOR_BLOCK;
                    t->cursor_style_set = false;
                    t->scroll_top = 0;
                    t->scroll_bottom = t->rows - 1;
                    t->mode &= ~(MODE_INSERT | MODE_ORIGIN | MODE_APP_CURSOR |
                                 MODE_APP_KEYPAD | MODE_SYNC_OUTPUT);
                    t->mode |= MODE_WRAP;
                    t->charset_g[0] = t->charset_g[1] = 0;
                    t->charset_gl = 0;
                    cursor_saved_reset(t);
                    /* The synchronized-output latch goes with its mode, or the
                     * terminal defers every frame with the mode already clear
                     * until the timeout fires. */
                    t->sync_pending = false;
                    t->sync_start_time = 0;
                    t->Liu_kbd_flags = 0;
                    t->Liu_kbd_stack_len = 0;
                    t->modify_other_keys = 0;
                    t->mode &= ~MODE_MODIFY_OTHER_KEYS;
                    TERM_DIRTY_ALL(t);
                } else if (is_space && b == 'q') {
                    /* DECSCUSR — Cursor shape (all 6 values). 0 hands the
                     * shape back to the user's setting. */
                    i32 style = param(t, 0, 0);
                    t->cursor_style_set = style != 0;
                    t->cursor_blink_set = style == 0 ? 0 : (style & 1) ? 1 : 2;
                    switch (style) {
                    case 0: case 1: t->cursor_style = CURSOR_BLOCK; break;
                    case 2: t->cursor_style = CURSOR_BLOCK; break;
                    case 3: t->cursor_style = CURSOR_UNDERLINE; break;
                    case 4: t->cursor_style = CURSOR_UNDERLINE; break;
                    case 5: t->cursor_style = CURSOR_BAR; break;
                    case 6: t->cursor_style = CURSOR_BAR; break;
                    }
                } else if (b == 'u') {
                    /* Liu keyboard protocol */
                    if (t->esc_len > 0 && t->esc_buf[0] == '<') {
                        /* CSI < n u — pop n entries and restore what the last
                         * one held.
                         *
                         * The reset-to-zero applies only when there was NOTHING
                         * to pop. Keying it on "the stack ended empty" would
                         * throw away the value just restored in the common
                         * case: one push and one pop is the whole idiom, so an
                         * application that set its base flags, pushed for a
                         * sub-mode and popped back would come out at 0 with its
                         * protocol silently off. */
                        i32 pop_count = param(t, 0, 1);
                        i32 popped = 0;
                        for (i32 pi = 0; pi < pop_count && t->Liu_kbd_stack_len > 0; pi++) {
                            t->Liu_kbd_flags = t->Liu_kbd_stack[--t->Liu_kbd_stack_len];
                            popped++;
                        }
                        if (popped == 0) t->Liu_kbd_flags = 0;
                    } else if (is_gt) {
                        /* CSI > flags u — push and set (already handled above) */
                    } else if (is_equal) {
                        /* CSI = flags ; mode u — SET the flags. This is the
                         * protocol's primary "request enhancements" form; the
                         * `>` push above is the stack variant beside it.
                         *
                         * Without this branch '=' would match none of the
                         * others and fall through to the bare-CSI-u case
                         * below, which is SCORC: an application asking for the
                         * keyboard protocol the documented way would get no
                         * protocol AND have its cursor moved to whatever CSI s
                         * last saved.
                         *
                         * mode 1 (and the default) replaces the flags, 2 sets
                         * the named bits, 3 clears them. */
                        i32 flags = param(t, 0, 0);
                        i32 fmode = param(t, 1, 1);
                        if (fmode <= 1)      t->Liu_kbd_flags  = (u32)flags;
                        else if (fmode == 2) t->Liu_kbd_flags |= (u32)flags;
                        else if (fmode == 3) t->Liu_kbd_flags &= ~(u32)flags;
                    } else {
                        /* Bare CSI u with no intermediate is SCORC — restore the
                         * cursor CSI s saved — and goes to handle_csi's
                         * `case 'u'`. The keyboard protocol only ever uses `u`
                         * WITH an intermediate (?, <, >, =), all handled above,
                         * so forwarding the bare form costs it nothing. */
                        handle_csi(t, 'u');
                    }
                } else if (b == 't') {
                    /* XTWINOPS — window operations */
                    i32 op = param(t, 0, 0);
                    switch (op) {
                    case 14: /* Report text area size in pixels */
                    case 16: { /* Report cell size in pixels */
                        /* The cell the pane is really drawn with
                         * (terminal_set_cell_pixels); 8 x 16 until the app has
                         * said, which is what this always answered. */
                        i32 cw = t->cell_px_w > 0 ? t->cell_px_w : 8;
                        i32 ch = t->cell_px_h > 0 ? t->cell_px_h : 16;
                        if (op == 14) emit_response_fmt(t, "\x1b[4;%d;%dt", t->rows * ch, t->cols * cw);
                        else          emit_response_fmt(t, "\x1b[6;%d;%dt", ch, cw);
                        break;
                    }
                    case 18: /* Report text area size in chars */
                        emit_response_fmt(t, "\x1b[8;%d;%dt", t->rows, t->cols);
                        break;
                    case 22: /* Push title. The second parameter picks icon name (1),
                              * window title (2) or both (0, or left out); Liu has
                              * one title, so all but the icon-only form mean it. */
                        if (param(t, 1, 0) != 1 && t->title_stack_len < 8) {
                            if (!t->title_stack) {
                                t->title_stack = calloc(8, 256);
                                if (!t->title_stack) break;
                            }
                            snprintf(t->title_stack[t->title_stack_len], 256, "%s", t->title);
                            t->title_stack_len++;
                        }
                        break;
                    case 23: /* Pop title */
                        if (param(t, 1, 0) != 1 && t->title_stack_len > 0) {
                            t->title_stack_len--;
                            if (t->title_stack) {
                                snprintf(t->title, sizeof(t->title), "%s", t->title_stack[t->title_stack_len]);
                            }
                            t->title_changed = true;
                            if (t->on_title) t->on_title(t, t->title, t->userdata);
                        }
                        break;
                    }
                } else {
                    handle_csi(t, b);
                }
                t->parse_state = STATE_GROUND;
            } else {
                /* Unknown — abort */
                t->parse_state = STATE_GROUND;
            }
            break;

        case STATE_OSC:
            if (b == '\a' || (b == '\\' &&
                ((t->dcs_buf && t->dcs_len > 0 && t->dcs_buf[t->dcs_len-1] == 0x1B) ||
                 (!t->dcs_buf && t->esc_len > 0 && t->esc_buf[t->esc_len-1] == 0x1B)))) {
                /* OSC terminated by BEL or ST (ESC \) */
                u8 *osc_data;
                i32 osc_len;
                if (t->dcs_buf) {
                    osc_data = t->dcs_buf;
                    osc_len = t->dcs_len;
                } else {
                    osc_data = t->esc_buf;
                    osc_len = t->esc_len;
                }
                if (b == '\\' && osc_len > 0) osc_len--; /* remove ESC */

                /* Parse OSC number to check for large-payload sequences */
                i32 osc_cmd = 0;
                i32 oi = 0;
                while (oi < osc_len && osc_data[oi] >= '0' && osc_data[oi] <= '9') {
                    osc_cmd = decimal_push_bounded(osc_cmd, osc_data[oi],
                                                   1000000);
                    oi++;
                }

                if (osc_cmd == 1337 && oi < osc_len && osc_data[oi] == ';') {
                    /* Inline image (OSC 1337) */
                    oi++; /* skip ';' */
                    handle_iterm2_image(t, osc_data + oi, osc_len - oi);
                } else {
                    /* Standard OSC — use esc_buf path (small buffer is fine) */
                    if (!t->dcs_buf) {
                        /* Terminate at osc_len, NOT esc_len. They differ by
                         * exactly the byte the ST strip above removed; a NUL
                         * written at the undecremented length would leave the
                         * trailing 0x1B inside the payload, and handle_osc
                         * would drop every ST-terminated OSC as a string ending
                         * in ESC. BEL-terminated ones have nothing stripped. */
                        t->esc_len = osc_len;
                        t->esc_buf[t->esc_len] = '\0';
                        handle_osc(t, t->esc_buf, osc_len, b == '\a');
                    } else {
                        /* A string that outgrew esc_buf is dispatched WHOLE,
                         * from the buffer it grew into, never cut to what
                         * esc_buf holds (255 bytes): a longer OSC 8 target, OSC
                         * 52 payload or OSC 7 path must not be acted on as if
                         * its first 255 bytes were all of it. Each handler has
                         * its own bound for what it takes. */
                        bool room = osc_len < t->dcs_cap;
                        if (!room) {
                            u8 *grown = str_buf_grow(t->dcs_buf, t->dcs_cap, t->dcs_cap + 1);
                            if (grown) { t->dcs_buf = grown; t->dcs_cap += 1; room = true; }
                        }
                        if (room) {
                            t->dcs_buf[osc_len] = '\0';
                            handle_osc(t, t->dcs_buf, osc_len, b == '\a');
                        }
                    }
                }

                str_buf_free(&t->dcs_buf, &t->dcs_len, &t->dcs_cap);
                t->parse_state = STATE_GROUND;
            } else {
                /* Accumulate OSC data — use dynamic buffer for large payloads (inline images) */
                if (t->esc_len < (i32)sizeof(t->esc_buf) - 1 && !t->dcs_buf) {
                    t->esc_buf[t->esc_len++] = b;
                } else {
                    /* Overflow to dynamic buffer (reuse dcs_buf) */
                    if (!t->dcs_buf) {
                        t->dcs_buf = str_buf_alloc(65536);
                        /* cap stays 0 on failure: it is the accounted amount,
                         * not a wish. */
                        t->dcs_cap = t->dcs_buf ? 65536 : 0;
                        if (t->dcs_buf) {
                            memcpy(t->dcs_buf, t->esc_buf, (usize)t->esc_len);
                            t->dcs_len = t->esc_len;
                        }
                    }
                    if (t->dcs_buf) {
                        if (t->dcs_len >= t->dcs_cap) {
                            i32 new_cap = t->dcs_cap + (t->dcs_cap >> 1); /* 1.5x growth */
                            if (new_cap < t->dcs_cap + 4096) new_cap = t->dcs_cap + 4096;
                            if (new_cap > 16 * 1024 * 1024) {
                                /* Cap at 16MB */
                                str_buf_free(&t->dcs_buf, &t->dcs_len, &t->dcs_cap);
                                t->parse_state = STATE_STR_SINK;
                                t->str_sink_esc = false;
                                break;
                            }
                            u8 *grown = str_buf_grow(t->dcs_buf, t->dcs_cap, new_cap);
                            if (!grown) {
                                str_buf_free(&t->dcs_buf, &t->dcs_len, &t->dcs_cap);
                                t->parse_state = STATE_STR_SINK;
                                t->str_sink_esc = false;
                                break;
                            }
                            t->dcs_buf = grown;
                            t->dcs_cap = new_cap;
                        }
                        if (t->dcs_buf) t->dcs_buf[t->dcs_len++] = b;
                    }
                }
            }
            break;

        case STATE_DCS:
            /* DCS sequences — absorb until ST, then process.
             *
             * The ST test has to look at whichever buffer is actually live. The
             * accumulator below fills the fixed esc_buf only up to 255 bytes and
             * then diverts everything after that into the heap dcs_buf, so for
             * any payload past that point the ESC of the closing "ESC \" lands in
             * dcs_buf while esc_buf[esc_len-1] still holds a stale Sixel data
             * byte. Testing esc_buf alone would never match: the sequence would
             * never terminate, no image would be produced, and the parser would
             * stay in STATE_DCS swallowing every subsequent byte until a BEL or
             * the 4 MiB dcs_cap trip — a dead terminal, reachable from anything
             * that can write to the tty.
             *
             * 255 bytes is about 250 Sixel columns, so this concerns every image
             * a real encoder produces (img2sixel, chafa -f sixel, lsix). The APC
             * handler above has the same shape. */
            if (b == '\\' &&
                ((t->dcs_buf && t->dcs_len > 0 && t->dcs_buf[t->dcs_len-1] == 0x1B) ||
                 (!t->dcs_buf && t->esc_len > 0 && t->esc_buf[t->esc_len-1] == 0x1B))) {
                /* Drop the trailing ESC from whichever buffer carries it —
                 * dcs_len2 below is read from t->dcs_len when dcs_buf is live. */
                if (t->dcs_buf) {
                    t->dcs_len--;
                } else {
                    t->esc_len--;
                    t->esc_buf[t->esc_len] = '\0';
                }
                /* Process DCS content */

                /* Sixel graphics: DCS [params] q <sixel data> ST */
                {
                    /* Use dynamic buffer if available, else esc_buf */
                    u8 *dcs_data = t->dcs_buf ? t->dcs_buf : t->esc_buf;
                    i32 dcs_len2 = t->dcs_buf ? t->dcs_len : t->esc_len;
                    bool is_sixel = false;
                    if (dcs_len2 > 1) {
                        for (i32 si = 0; si < dcs_len2 && si < 10; si++) {
                            if (dcs_data[si] == 'q') { is_sixel = true; break; }
                        }
                    }
                if (is_sixel) {
                    /* Find 'q' to locate start of sixel data */
                    i32 data_start = 0;
                    for (i32 si = 0; si < dcs_len2; si++) {
                        if (dcs_data[si] == 'q') { data_start = si + 1; break; }
                    }
                    /* No image_count gate here: terminal_add_image already
                     * evicts the LRU slot (and enough slots to fit the byte
                     * budget) when the pool is full, exactly as it does for the
                     * other inline-image paths. Refusing at 32 made Sixel the one
                     * protocol that silently stopped rendering for the rest of
                     * the session once a terminal had accumulated that many
                     * images. */
                    if (data_start > 0) {
                        (void)term_image_enqueue_b64(t, dcs_data + data_start,
                            dcs_len2 - data_start, 6, 0, 0, 0, 0, 0, 0, 0);
                    }
                    str_buf_free(&t->dcs_buf, &t->dcs_len, &t->dcs_cap);
                    t->parse_state = STATE_GROUND;
                    break;
                }
                } /* end is_sixel block */

                if (t->esc_len >= 2 && t->esc_buf[0] == '+' && t->esc_buf[1] == 'q') {
                    /* XTGETTCAP — query terminfo capability */
                    /* Capability name is hex-encoded after "+q" */
                    char cap_name[64] = {0};
                    i32 ci = 0;
                    for (i32 hi = 2; hi < t->esc_len - 1 && ci < 63; hi += 2) {
                        char hx[3] = { (char)t->esc_buf[hi], (char)t->esc_buf[hi+1], 0 };
                        cap_name[ci++] = (char)strtol(hx, NULL, 16);
                    }
                    cap_name[ci] = '\0';

                    /* Capability table */
                    const char *val = NULL;
                    if (strcmp(cap_name, "Tc") == 0) val = "";  /* true color supported (boolean) */
                    else if (strcmp(cap_name, "RGB") == 0) val = "";
                    else if (strcmp(cap_name, "Su") == 0) val = ""; /* styled underlines */
                    else if (strcmp(cap_name, "Smulx") == 0) val = "\\E[4:%p1%dm";
                    else if (strcmp(cap_name, "Setulc") == 0) val = "\\E[58:2::%p1%{65536}%/%d::%p1%{256}%/%{255}%&%d::%p1%{255}%&%dm";
                    else if (strcmp(cap_name, "Se") == 0) val = "\\E[2 q";
                    else if (strcmp(cap_name, "Ss") == 0) val = "\\E[%p1%d q";
                    else if (strcmp(cap_name, "Ms") == 0) val = "\\E]52;%p1%s;%p2%s\\007";
                    else if (strcmp(cap_name, "cols") == 0) {
                        char vbuf[16]; snprintf(vbuf, sizeof(vbuf), "%d", t->cols);
                        /* Hex encode the value */
                        char hex_key[128];
                        hex_encode_bounded(hex_key, sizeof(hex_key), cap_name);
                        char hex_val[128];
                        hex_encode_bounded(hex_val, sizeof(hex_val), vbuf);
                        emit_response_fmt(t, "\x1bP1+r%s=%s\x1b\\", hex_key, hex_val);
                        str_buf_free(&t->dcs_buf, &t->dcs_len, &t->dcs_cap);
                        t->parse_state = STATE_GROUND;
                        break;
                    }

                    if (val) {
                        /* Respond: DCS 1 + r hexkey=hexval ST */
                        char hex_key[128];
                        hex_encode_bounded(hex_key, sizeof(hex_key), cap_name);
                        char hex_val[128];
                        hex_encode_bounded(hex_val, sizeof(hex_val), val);
                        emit_response_fmt(t, "\x1bP1+r%s=%s\x1b\\", hex_key, hex_val);
                    } else {
                        /* Not found */
                        char hex_key[128];
                        hex_encode_bounded(hex_key, sizeof(hex_key), cap_name);
                        emit_response_fmt(t, "\x1bP0+r%s\x1b\\", hex_key);
                    }
                }
                /* Free any overflow buffer before leaving DCS: a non-sixel DCS that
                 * overflowed esc_buf and terminated with ST still holds dcs_buf,
                 * which would otherwise be reused stale by the next DCS/OSC
                 * (corruption) and its length would creep toward the 4 MB cap. */
                str_buf_free(&t->dcs_buf, &t->dcs_len, &t->dcs_cap);
                t->parse_state = STATE_GROUND;
            } else if (b == '\a') {
                /* Free DCS buffer if used */
                str_buf_free(&t->dcs_buf, &t->dcs_len, &t->dcs_cap);
                t->parse_state = STATE_GROUND;
            } else {
                /* Accumulate DCS data — use dynamic buffer for large sequences (Sixel) */
                if (t->esc_len < (i32)sizeof(t->esc_buf) - 1) {
                    t->esc_buf[t->esc_len++] = b;
                } else {
                    /* Overflow to dynamic buffer */
                    if (!t->dcs_buf) {
                        t->dcs_buf = str_buf_alloc(65536);
                        /* cap stays 0 on failure: it is the accounted amount,
                         * not a wish. */
                        t->dcs_cap = t->dcs_buf ? 65536 : 0;
                        if (t->dcs_buf) {
                            memcpy(t->dcs_buf, t->esc_buf, (usize)t->esc_len);
                            t->dcs_len = t->esc_len;
                        }
                    }
                    if (t->dcs_buf) {
                        if (t->dcs_len >= t->dcs_cap) {
                            i32 new_cap = t->dcs_cap + (t->dcs_cap >> 1); /* 1.5x growth */
                            if (new_cap < t->dcs_cap + 4096) new_cap = t->dcs_cap + 4096;
                            if (new_cap > 4 * 1024 * 1024) {
                                /* Cap at 4MB to prevent runaway */
                                str_buf_free(&t->dcs_buf, &t->dcs_len, &t->dcs_cap);
                                t->parse_state = STATE_STR_SINK;
                                t->str_sink_esc = false;
                                break;
                            }
                            u8 *grown = str_buf_grow(t->dcs_buf, t->dcs_cap, new_cap);
                            if (!grown) {
                                str_buf_free(&t->dcs_buf, &t->dcs_len, &t->dcs_cap);
                                t->parse_state = STATE_STR_SINK;
                                t->str_sink_esc = false;
                                break;
                            }
                            t->dcs_buf = grown;
                            t->dcs_cap = new_cap;
                        }
                        if (t->dcs_buf) t->dcs_buf[t->dcs_len++] = b;
                    }
                }
            }
            break;

        case STATE_ESC_HASH:
            /* ESC # 8 — DECALN (fill screen with 'E') */
            if (b == '8') {
                /* Every cell an E in the default rendition (a wide character's
                 * flags must not outlive it), the margins open, the cursor home. */
                CellAttr plain = { .fg = FG_DEFAULT, .bg = BG_DEFAULT, .flags = 0 };
                for (i32 y = 0; y < t->rows; y++) {
                    for (i32 x = 0; x < t->cols; x++) {
                        t->cells[y * t->cols + x].codepoint = 'E';
                        t->cells[y * t->cols + x].attr = plain;
                    }
                }
                terminal_clear_underline_map(t);
                wrap_break_rows(t, 0, t->rows - 1);
                t->scroll_top = 0;
                t->scroll_bottom = t->rows - 1;
                t->cursor_x = 0;
                t->cursor_y = 0;
                TERM_DIRTY_ALL(t);
            }
            t->parse_state = STATE_GROUND;
            break;

        case STATE_CHARSET: {
            /* `ESC ( F` / `ESC ) F`: F names the set G0 / G1 holds from now
             * on. `0` is DEC Special Graphics; everything else Liu has is
             * ASCII (B, and the national sets that differ in a symbol or two). */
            i32 g = (t->esc_len > 0 && t->esc_buf[0] == ')') ? 1 : 0;
            t->charset_g[g] = (b == '0') ? 1 : 0;
            t->esc_len = 0;
            t->parse_state = STATE_GROUND;
            break;
        }

        case STATE_ESC_SKIP:
            /* An escape sequence with an intermediate Liu has no function for
             * (`ESC % G`, `ESC SP F`, `ESC * 0`): swallowed up to and including
             * its final byte. Ending it at the intermediate printed the final. */
            if (b >= 0x30) t->parse_state = STATE_GROUND;
            break;
        }
    }
}
