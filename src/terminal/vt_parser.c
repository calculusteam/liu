/*
 * Liu - VT100/xterm escape sequence parser
 * State machine that processes raw PTY output into terminal operations.
 */
#include "terminal/terminal.h"
#include "platform/platform.h"
#include "core/utf8.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>
#include <sys/stat.h>

/* stb_image for PNG decoding (Liu graphics + inline images) */
#include "stb_image.h"

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

/* Parser states */
enum {
    STATE_GROUND = 0,
    STATE_ESC,          /* saw ESC */
    STATE_CSI,          /* saw ESC [ */
    STATE_OSC,          /* saw ESC ] */
    STATE_DCS,          /* saw ESC P */
    STATE_ESC_HASH,     /* saw ESC # */
    STATE_CHARSET,      /* saw ESC ( or ESC ) */
    STATE_APC,          /* saw ESC _ (for Liu graphics) */
};

/* =========================================================================
 * Response emit — send data back to PTY (for DSR, DA, DECRQM etc.)
 * ========================================================================= */

static void emit_response(Terminal *t, const char *data, i32 len) {
    if (t->on_response) {
        t->on_response(t, (const u8 *)data, len, t->userdata);
    }
}

static void emit_response_fmt(Terminal *t, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) emit_response(t, buf, n);
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

static inline i32 param(Terminal *t, i32 idx, i32 def) {
    if (idx >= t->param_count) return def;
    return t->params[idx] > 0 ? t->params[idx] : def;
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

    i32 start_row = (t->input_start_row >= 0 && t->input_start_row < t->rows)
                  ? t->input_start_row
                  : t->cursor_y;
    i32 end_row = t->cursor_y;

    if (start_row < 0 || start_row >= t->rows ||
        end_row < 0 || end_row >= t->rows ||
        start_row > end_row) {
        return;
    }

    while (end_row > start_row && command_row_content_end(t, end_row) == 0) {
        end_row--;
    }

    i32 out_len = 0;
    for (i32 row = start_row; row <= end_row && out_len < cap - 1; row++) {
        i32 row_end = command_row_content_end(t, row);
        for (i32 col = 0; col < row_end && out_len < cap - 1; col++) {
            Cell *c = &t->cells[row * t->cols + col];
            if (c->attr.flags & ATTR_WDUMMY) continue;

            u32 cp = c->codepoint;
            if (cp == 0) cp = ' ';
            if (cp < 32 && cp != '\t') continue;

            u8 utf8[4];
            u32 n = utf8_encode(cp, utf8);
            if (n == 0 || out_len + (i32)n >= cap) break;
            memcpy(&t->pending_command[out_len], utf8, n);
            out_len += (i32)n;
        }

        bool wrapped = (t->line_wrapped && row < t->rows - 1 && t->line_wrapped[row]);
        if (!wrapped && row < end_row && out_len < cap - 1) {
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

static void put_char(Terminal *t, u32 cp) {
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
            if (!(t->mode & MODE_ALT_SCREEN) &&
                t->line_wrapped && t->cursor_y >= 0 && t->cursor_y < t->rows)
                t->line_wrapped[t->cursor_y] = true;
            t->cursor_x = 0;
            t->cursor_y++;
            if (t->cursor_y > t->scroll_bottom) {
                t->cursor_y = t->scroll_bottom;
                buffer_scroll_up(t, 1);
                TERM_DIRTY_ALL(t); /* scroll affects all rows */
            }
        } else {
            t->cursor_x = t->cols - 1;
        }
    }

    /* Check if character is wide (CJK, emoji, etc.) */
    int width = utf8_char_width(cp);

    Cell *c = &t->cells[t->cursor_y * t->cols + t->cursor_x];
    c->codepoint = cp;
    c->attr = t->cursor_attr;
    mark_current_row_zone(t);

    /* Write underline data to sparse map if non-default */
    if (t->cursor_ul_color != 0 || t->cursor_ul_style != 0)
        terminal_set_underline(t, t->cursor_x, t->cursor_y, t->cursor_ul_color, t->cursor_ul_style);

    if (width == 2 && t->cursor_x + 1 < t->cols) {
        /* Wide character: mark first cell as WIDE, second as dummy */
        c->attr.flags |= ATTR_WIDE;
        Cell *d = &t->cells[t->cursor_y * t->cols + t->cursor_x + 1];
        d->codepoint = ' ';
        d->attr = t->cursor_attr;
        d->attr.flags |= ATTR_WDUMMY;
        t->cursor_x += 2;
    } else {
        t->cursor_x++;
    }
    t->last_printed_char = cp;
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

/* Decode base64 in-place. Returns decoded byte count. */
static i32 base64_decode(const u8 *src, i32 src_len, u8 *dst) {
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
            dst[out++] = (u8)(accum >> bits);
        }
    }
    return out;
}

/* =========================================================================
 * Liu graphics protocol handler (APC-based)
 * ========================================================================= */

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
                    val = val * 10 + (data[hi] - '0');
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
            case 'm': more_chunks = (val == 1); break;
            case 'd': delete_what = (hi > val_start + 1) ? (u32)data[val_start] : 0; break;
            }
        }
    }

    /* Handle delete action */
    if (action == 'd') {
        if (delete_what == 'a' || delete_what == 'A') {
            /* Delete all images */
            terminal_clear_images(t);
        } else if (delete_what == 'i' || delete_what == 'I') {
            /* Delete by image ID */
            terminal_delete_image(t, image_id);
        } else {
            /* Delete all visible */
            terminal_clear_images(t);
        }
        /* Send OK response */
        if (!quiet)
            emit_response_fmt(t, "\x1b_Gi=%u;OK\x1b\\", image_id);
        return;
    }

    (void)transmission; /* file/temp transmission types not yet supported */

    /* Only handle transmit and transmit+display */
    if (action != 't' && action != 'T') {
        if (!quiet)
            emit_response_fmt(t, "\x1b_Gi=%u;OK\x1b\\", image_id);
        return;
    }

    /* If more_chunks is set, we'd need multi-part reassembly.
     * For now, only handle single-chunk transmissions. */
    if (more_chunks) {
        /* Silently ignore multi-chunk for now */
        return;
    }

    /* Extract and decode payload */
    if (semi_pos < 0 || semi_pos + 1 >= len) {
        if (!quiet)
            emit_response_fmt(t, "\x1b_Gi=%u;ENODATA\x1b\\", image_id);
        return;
    }

    const u8 *payload = data + semi_pos + 1;
    i32 payload_len = len - semi_pos - 1;

    /* Decode base64 payload */
    u8 *decoded = malloc((usize)payload_len); /* decoded is always <= encoded */
    if (!decoded) return;
    i32 decoded_len = base64_decode(payload, payload_len, decoded);
    if (decoded_len <= 0) {
        free(decoded);
        if (!quiet)
            emit_response_fmt(t, "\x1b_Gi=%u;ENODATA\x1b\\", image_id);
        return;
    }

    /* Decode to RGBA pixels */
    u8 *pixels = NULL;
    i32 img_w = 0, img_h = 0;

    if (format == 100) {
        /* PNG format — decode with stb_image */
        int comp = 0;
        pixels = stbi_load_from_memory(decoded, decoded_len, &img_w, &img_h, &comp, 4);
        free(decoded);
        if (!pixels) {
            if (!quiet)
                emit_response_fmt(t, "\x1b_Gi=%u;EBADPNG\x1b\\", image_id);
            return;
        }
    } else if (format == 32) {
        /* Raw RGBA. Bound dimensions BEFORE any size math so src_w*src_h
         * cannot overflow i32 (the 4096 cap matches the dimension check
         * applied to PNG below). */
        if (src_w <= 0 || src_h <= 0 || src_w > 4096 || src_h > 4096 ||
            (i64)decoded_len < (i64)src_w * (i64)src_h * 4) {
            free(decoded);
            if (!quiet)
                emit_response_fmt(t, "\x1b_Gi=%u;ENOSIZE\x1b\\", image_id);
            return;
        }
        img_w = src_w;
        img_h = src_h;
        pixels = decoded; /* take ownership */
    } else if (format == 24) {
        /* Raw RGB — convert to RGBA. Bound dimensions BEFORE any size math so
         * src_w*src_h cannot overflow i32; do all index/size math in 64-bit. */
        if (src_w <= 0 || src_h <= 0 || src_w > 4096 || src_h > 4096 ||
            (i64)decoded_len < (i64)src_w * (i64)src_h * 3) {
            free(decoded);
            if (!quiet)
                emit_response_fmt(t, "\x1b_Gi=%u;ENOSIZE\x1b\\", image_id);
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
            emit_response_fmt(t, "\x1b_Gi=%u;EFORMAT\x1b\\", image_id);
        return;
    }

    /* Validate dimensions */
    if (img_w <= 0 || img_h <= 0 || img_w > 4096 || img_h > 4096) {
        free(pixels);
        if (!quiet)
            emit_response_fmt(t, "\x1b_Gi=%u;EDIM\x1b\\", image_id);
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
        emit_response_fmt(t, "\x1b_Gi=%u;OK\x1b\\", image_id);
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
                        width_cells = width_cells * 10 + (str[vi] - '0');
                    else break; /* stop at non-digit (e.g. "auto", "10px") */
                }
            } else if (key_len == 6 && memcmp(str + key_start, "height", 6) == 0) {
                height_cells = 0;
                for (i32 vi = val_start; vi < val_start + val_len; vi++) {
                    if (str[vi] >= '0' && str[vi] <= '9')
                        height_cells = height_cells * 10 + (str[vi] - '0');
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

    u8 *decoded = malloc((usize)b64_len);
    if (!decoded) return;
    i32 decoded_len = base64_decode(b64_data, b64_len, decoded);
    if (decoded_len <= 0) { free(decoded); return; }

    /* Decode image with stb_image (auto-detects PNG/JPEG/etc.) */
    int img_w = 0, img_h = 0, comp = 0;
    u8 *pixels = stbi_load_from_memory(decoded, decoded_len, &img_w, &img_h, &comp, 4);
    free(decoded);

    if (!pixels || img_w <= 0 || img_h <= 0 || img_w > 4096 || img_h > 4096) {
        if (pixels) free(pixels);
        return;
    }

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
    case 'A': /* CUU — Cursor Up */
        t->cursor_y -= p0;
        cursor_clamp(t);
        break;
    case 'B': /* CUD — Cursor Down */
    case 'e':
        t->cursor_y += p0;
        cursor_clamp(t);
        break;
    case 'C': /* CUF — Cursor Forward */
    case 'a':
        t->cursor_x += p0;
        cursor_clamp(t);
        break;
    case 'D': /* CUB — Cursor Back */
        t->cursor_x -= p0;
        cursor_clamp(t);
        break;
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
        t->cursor_y = param(t, 0, 1) - 1;
        t->cursor_x = param(t, 1, 1) - 1;
        cursor_clamp(t);
        break;
    case 'd': /* VPA — Vertical Position Absolute */
        t->cursor_y = p0 - 1;
        cursor_clamp(t);
        break;

    /* Erase */
    case 'J': { /* ED — Erase in Display */
        i32 mode = param(t, 0, 0);
        CellAttr a = t->cursor_attr;
        a.flags = 0;
        if (mode == 0) { /* cursor to end */
            buffer_clear_region(t->cells, t->cols, t->cursor_x, t->cursor_y, t->cols-1, t->cursor_y, a);
            if (t->cursor_y + 1 < t->rows)
                buffer_clear_region(t->cells, t->cols, 0, t->cursor_y+1, t->cols-1, t->rows-1, a);
        } else if (mode == 1) { /* start to cursor */
            buffer_clear_region(t->cells, t->cols, 0, 0, t->cols-1, t->cursor_y-1, a);
            buffer_clear_region(t->cells, t->cols, 0, t->cursor_y, t->cursor_x, t->cursor_y, a);
        } else if (mode == 2 || mode == 3) { /* entire screen */
            buffer_clear_region(t->cells, t->cols, 0, 0, t->cols-1, t->rows-1, a);
            terminal_clear_underline_map(t);
        }
        TERM_DIRTY_ALL(t);
        break;
    }
    case 'K': { /* EL — Erase in Line */
        i32 mode = param(t, 0, 0);
        CellAttr a = t->cursor_attr;
        a.flags = 0;
        if (mode == 0)
            buffer_clear_region(t->cells, t->cols, t->cursor_x, t->cursor_y, t->cols-1, t->cursor_y, a);
        else if (mode == 1)
            buffer_clear_region(t->cells, t->cols, 0, t->cursor_y, t->cursor_x, t->cursor_y, a);
        else if (mode == 2)
            buffer_clear_region(t->cells, t->cols, 0, t->cursor_y, t->cols-1, t->cursor_y, a);
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
        Cell blank = { .codepoint = ' ', .attr = t->cursor_attr };
        blank.attr.flags = 0;
        for (i32 i = 0; i < n; i++) row[t->cursor_x + i] = blank;
        TERM_DIRTY_ROW(t, t->cursor_y);
        break;
    }
    case 'P': { /* DCH — Delete Characters */
        i32 n = p0;
        if (n > t->cols - t->cursor_x) n = t->cols - t->cursor_x;
        Cell *row = &t->cells[t->cursor_y * t->cols];
        memmove(&row[t->cursor_x], &row[t->cursor_x + n],
                (usize)(t->cols - t->cursor_x - n) * sizeof(Cell));
        Cell blank = { .codepoint = ' ', .attr = t->cursor_attr };
        blank.attr.flags = 0;
        for (i32 i = t->cols - n; i < t->cols; i++) row[i] = blank;
        TERM_DIRTY_ROW(t, t->cursor_y);
        break;
    }
    case 'X': { /* ECH — Erase Characters */
        i32 n = p0;
        CellAttr a = t->cursor_attr;
        a.flags = 0;
        i32 end = t->cursor_x + n - 1;
        if (end >= t->cols) end = t->cols - 1;
        buffer_clear_region(t->cells, t->cols, t->cursor_x, t->cursor_y, end, t->cursor_y, a);
        TERM_DIRTY_ROW(t, t->cursor_y);
        break;
    }

    /* SGR — Select Graphic Rendition */
    case 'm': {
        if (t->param_count == 0) {
            /* Reset */
            t->cursor_attr.fg = FG_DEFAULT;
            t->cursor_attr.bg = BG_DEFAULT;
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
                t->cursor_attr.flags |= ATTR_UNDERLINE;
                /* Check for colon subparameters: 4:0-5 */
                if (i + 1 < t->param_count) {
                    i32 sub = t->params[i + 1];
                    /* Heuristic: if next param is 0-5 and was a colon subparam */
                    if (sub >= 0 && sub <= 5) {
                        t->cursor_ul_style = (u8)sub;
                        if (sub == 0) t->cursor_attr.flags &= ~ATTR_UNDERLINE;
                        i++; /* consume subparam */
                    } else {
                        t->cursor_ul_style = 1; /* single */
                    }
                } else {
                    t->cursor_ul_style = 1;
                }
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
                /* Underline color: 58:5:N or 58:2::R:G:B.  Clamp each channel
                 * to [0,255] so a malformed stream cannot inject a truecolor
                 * with bogus bits in the flag byte, and reject out-of-range
                 * 256-color indices instead of letting them leak downstream. */
                if (i + 1 < t->param_count && t->params[i+1] == 5 && i + 2 < t->param_count) {
                    i32 idx = t->params[i+2];
                    if (idx >= 0 && idx < 256) t->cursor_ul_color = (u32)idx;
                    i += 2;
                } else if (i + 1 < t->param_count && t->params[i+1] == 2 && i + 4 < t->param_count) {
                    i32 cr = t->params[i+2], cg = t->params[i+3], cb = t->params[i+4];
                    if (cr >= 0 && cr <= 255 && cg >= 0 && cg <= 255 && cb >= 0 && cb <= 255)
                        t->cursor_ul_color = FG_TRUECOLOR(cr, cg, cb);
                    i += 4;
                }
            } else if (p == 59) {
                t->cursor_ul_color = 0; /* reset underline color */
            } else if (p >= 30 && p <= 37) {
                t->cursor_attr.fg = (u32)(p - 30);
            } else if (p == 38) {
                /* Extended foreground */
                if (i + 1 < t->param_count && t->params[i+1] == 5 && i + 2 < t->param_count) {
                    i32 idx = t->params[i+2];
                    if (idx >= 0 && idx < 256) t->cursor_attr.fg = (u32)idx;
                    i += 2;
                } else if (i + 1 < t->param_count && t->params[i+1] == 2 && i + 4 < t->param_count) {
                    i32 cr = t->params[i+2], cg = t->params[i+3], cb = t->params[i+4];
                    if (cr >= 0 && cr <= 255 && cg >= 0 && cg <= 255 && cb >= 0 && cb <= 255)
                        t->cursor_attr.fg = FG_TRUECOLOR(cr, cg, cb);
                    i += 4;
                }
            } else if (p == 39) {
                t->cursor_attr.fg = FG_DEFAULT;
            } else if (p >= 40 && p <= 47) {
                t->cursor_attr.bg = (u32)(p - 40);
            } else if (p == 48) {
                /* Extended background */
                if (i + 1 < t->param_count && t->params[i+1] == 5 && i + 2 < t->param_count) {
                    i32 idx = t->params[i+2];
                    if (idx >= 0 && idx < 256) t->cursor_attr.bg = (u32)idx;
                    i += 2;
                } else if (i + 1 < t->param_count && t->params[i+1] == 2 && i + 4 < t->param_count) {
                    i32 cr = t->params[i+2], cg = t->params[i+3], cb = t->params[i+4];
                    if (cr >= 0 && cr <= 255 && cg >= 0 && cg <= 255 && cb >= 0 && cb <= 255)
                        t->cursor_attr.bg = BG_TRUECOLOR(cr, cg, cb);
                    i += 4;
                }
            } else if (p == 49) {
                t->cursor_attr.bg = BG_DEFAULT;
            } else if (p >= 90 && p <= 97) {
                t->cursor_attr.fg = (u32)(p - 90 + 8);
            } else if (p >= 100 && p <= 107) {
                t->cursor_attr.bg = (u32)(p - 100 + 8);
            }
        }
        break;
    }

    /* Scroll region */
    case 'r': /* DECSTBM — Set Top and Bottom Margins */
        t->scroll_top    = param(t, 0, 1) - 1;
        t->scroll_bottom = param(t, 1, t->rows) - 1;
        if (t->scroll_top < 0) t->scroll_top = 0;
        if (t->scroll_bottom >= t->rows) t->scroll_bottom = t->rows - 1;
        if (t->scroll_top >= t->scroll_bottom) {
            t->scroll_top = 0;
            t->scroll_bottom = t->rows - 1;
        }
        t->cursor_x = 0;
        t->cursor_y = (t->mode & MODE_ORIGIN) ? t->scroll_top : 0;
        break;

    /* Device status */
    case 'n': { /* DSR — Device Status Report */
        i32 ps = param(t, 0, 0);
        if (ps == 5) {
            /* Operating status → OK */
            emit_response_fmt(t, "\x1b[0n");
        } else if (ps == 6) {
            /* Cursor Position Report */
            emit_response_fmt(t, "\x1b[%d;%dR", t->cursor_y + 1, t->cursor_x + 1);
        }
        break;
    }
    case 'c': { /* DA — Device Attributes */
        /* Primary DA: report as VT220 with ANSI color + extended attributes */
        emit_response_fmt(t, "\x1b[?62;22c");
        break;
    }
    case 'b': { /* REP — Repeat previous character */
        i32 count = param(t, 0, 1);
        if (t->last_printed_char > 0) {
            for (i32 ri = 0; ri < count && ri < t->cols; ri++) {
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

    /* Save/Restore cursor */
    case 's':
        t->saved_x = t->cursor_x;
        t->saved_y = t->cursor_y;
        t->saved_attr = t->cursor_attr;
        t->saved_ul_color = t->cursor_ul_color;
        t->saved_ul_style = t->cursor_ul_style;
        break;
    case 'u':
        t->cursor_x = t->saved_x;
        t->cursor_y = t->saved_y;
        t->cursor_attr = t->saved_attr;
        t->cursor_ul_color = t->saved_ul_color;
        t->cursor_ul_style = t->saved_ul_style;
        cursor_clamp(t);
        break;

    default:
        break;
    }
}

/* =========================================================================
 * CSI ? (DEC private mode) handler
 * ========================================================================= */

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
                    t->alt_cells = calloc((usize)(t->cols * t->rows), sizeof(Cell));
                    if (!t->alt_cells) break;
                }
                Cell *tmp = t->cells;
                t->cells = t->alt_cells;
                t->alt_cells = tmp;
                t->alt_cursor_x = t->cursor_x;
                t->alt_cursor_y = t->cursor_y;
                t->mode |= MODE_ALT_SCREEN;
            } else if (!set && (t->mode & MODE_ALT_SCREEN)) {
                Cell *tmp = t->cells;     /* current alt content */
                t->cells = t->alt_cells;  /* restore primary */
                t->alt_cells = tmp;       /* alt buffer parked here briefly */
                t->cursor_x = t->alt_cursor_x;
                t->cursor_y = t->alt_cursor_y;
                t->mode &= ~MODE_ALT_SCREEN;
                /* Drop the alt buffer now that we've left it. Vim/less/htop
                 * are dominant alt-screen consumers, all of which exit
                 * cleanly via 1049/1047 unset; we don't need to keep their
                 * stale framebuffer alive across runs. */
                free(t->alt_cells);
                t->alt_cells = NULL;
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
                t->saved_x = t->cursor_x;
                t->saved_y = t->cursor_y;
                t->saved_attr = t->cursor_attr;
                t->saved_ul_color = t->cursor_ul_color;
                t->saved_ul_style = t->cursor_ul_style;
            } else {
                t->cursor_x = t->saved_x;
                t->cursor_y = t->saved_y;
                t->cursor_attr = t->saved_attr;
                t->cursor_ul_color = t->saved_ul_color;
                t->cursor_ul_style = t->saved_ul_style;
                cursor_clamp(t);
            }
            break;
        case 1049: /* Alternate screen + save cursor */
            if (set) {
                t->saved_x = t->cursor_x;
                t->saved_y = t->cursor_y;
                t->saved_attr = t->cursor_attr;
                t->saved_ul_color = t->cursor_ul_color;
                t->saved_ul_style = t->cursor_ul_style;
                if (!(t->mode & MODE_ALT_SCREEN)) {
                    /* Lazy-allocate alt_cells on first use */
                    if (!t->alt_cells) {
                        t->alt_cells = calloc((usize)(t->cols * t->rows), sizeof(Cell));
                        if (!t->alt_cells) break;
                    }
                    Cell *tmp = t->cells;
                    t->cells = t->alt_cells;
                    t->alt_cells = tmp;
                    t->mode |= MODE_ALT_SCREEN;
                    CellAttr a = { .fg = FG_DEFAULT, .bg = BG_DEFAULT, .flags = 0 };
                    buffer_clear_region(t->cells, t->cols, 0, 0, t->cols-1, t->rows-1, a);
                    terminal_clear_underline_map(t);
                }
            } else {
                if (t->mode & MODE_ALT_SCREEN) {
                    Cell *tmp = t->cells;     /* alt content */
                    t->cells = t->alt_cells;  /* restore primary */
                    t->alt_cells = tmp;
                    t->mode &= ~MODE_ALT_SCREEN;
                    terminal_clear_underline_map(t);
                    /* Release the alt buffer — it's stale and the next
                     * alt-screen entry will lazy-alloc a fresh one. */
                    free(t->alt_cells);
                    t->alt_cells = NULL;
                }
                t->cursor_x = t->saved_x;
                t->cursor_y = t->saved_y;
                t->cursor_attr = t->saved_attr;
                t->cursor_ul_color = t->saved_ul_color;
                t->cursor_ul_style = t->saved_ul_style;
                cursor_clamp(t);
                /* Reset Liu keyboard on alt-screen exit — apps should pop
                 * but some don't (e.g., if they crash or are killed) */
                t->Liu_kbd_flags = 0;
                t->Liu_kbd_stack_len = 0;
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
                t->sync_pending = true;
                t->sync_start_time = 0; /* caller should set actual time */
            } else {
                t->mode &= ~MODE_SYNC_OUTPUT;
                t->sync_pending = false;
            }
            TERM_DIRTY_ALL(t);
            break;
        case 2027: /* Grapheme cluster mode */
            if (set) t->mode |= MODE_GRAPHEME;
            else     t->mode &= ~MODE_GRAPHEME;
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

/* A window/icon title (OSC 0/1/2) must be clean printable UTF-8. Binary output
 * piped to the terminal — e.g. `cat <image>` — sprays arbitrary bytes that
 * would otherwise land in t->title, render as garbage in the tab bar, and (on
 * macOS) crash native -[NSWindow setTitle:] because invalid UTF-8 makes
 * +stringWithUTF8String: return nil. Reject any string with control bytes or
 * malformed UTF-8 so the title simply stays unchanged. */
static bool osc_title_is_clean(const char *s, i32 n) {
    i32 i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) return false;            /* control byte */
        i32 extra;
        if (c < 0x80)            extra = 0;
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false;                                  /* invalid lead */
        if (i + extra >= n) return false;                   /* truncated */
        for (i32 k = 1; k <= extra; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false; /* bad cont */
        i += extra + 1;
    }
    return true;
}

static void handle_osc(Terminal *t) {
    /* Parse "Ps;Pt" where Ps is the command number */
    i32 cmd = 0;
    i32 i = 0;
    while (i < t->esc_len && t->esc_buf[i] >= '0' && t->esc_buf[i] <= '9') {
        cmd = cmd * 10 + (t->esc_buf[i] - '0');
        i++;
    }
    if (i < t->esc_len && t->esc_buf[i] == ';') i++;

    const char *text = (const char *)&t->esc_buf[i];
    i32 text_len = t->esc_len - i;

    switch (cmd) {
    case 0:  /* Set icon name + window title */
    case 2: { /* Set window title */
        i32 n = text_len < 255 ? text_len : 255;
        /* Reject binary / invalid-UTF-8 titles (cat <image> etc.) — leave the
         * existing title untouched rather than store garbage that crashes the
         * native title bar. */
        if (!osc_title_is_clean(text, n)) break;
        memcpy(t->title, text, (usize)n);
        t->title[n] = '\0';
        t->title_changed = true;
        if (cmd == 0) {
            if (!t->icon_name) {
                t->icon_name = calloc(1, 128);
                if (!t->icon_name) break;
            }
            i32 m = n < 127 ? n : 127;
            memcpy(t->icon_name, text, (usize)m);
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
        memcpy(t->icon_name, text, (usize)n);
        t->icon_name[n] = '\0';
        break;
    }
    case 7: { /* Current working directory URI: file://hostname/path */
        char uri[1024];
        i32 n = text_len < 1023 ? text_len : 1023;
        memcpy(uri, text, (usize)n);
        uri[n] = '\0';
        /* Parse file:// URI → extract path */
        const char *path = uri;
        if (strncmp(uri, "file://", 7) == 0) {
            path = uri + 7;
            /* Skip hostname */
            const char *slash = strchr(path, '/');
            if (slash) path = slash;
        }
        /* URL-decode %XX sequences */
        char decoded[1024];
        i32 di = 0;
        for (const char *p = path; *p && di < 1023; p++) {
            if (*p == '%' && p[1] && p[2]) {
                char hex[3] = { p[1], p[2], 0 };
                decoded[di++] = (char)strtol(hex, NULL, 16);
                p += 2;
            } else {
                decoded[di++] = *p;
            }
        }
        decoded[di] = '\0';
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
    case 9: { /* OSC 9 — ConEmu/progress extensions */
        if (text_len >= 2 && text[0] == '4' && text[1] == ';') {
            /* OSC 9;4;state;value — progress indicator */
            /* Parse but don't act yet — could set dock badge */
        }
        break;
    }
    case 52: { /* OSC 52 — Clipboard access: OSC 52;selector;base64data ST
                * or OSC 52;selector;? ST (query — not supported, silent drop).
                * The selector chars (c/p/s/…) pick the clipboard target; we
                * only honor the primary system clipboard. */
        if (text_len >= 3 && text[1] == ';') {
            const char *payload = text + 2;
            i32 payload_len = text_len - 2;
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
                t->current_zone = ZONE_PROMPT;
                t->prompt_start_row = t->cursor_y;
                mark_current_row_zone(t);
                break;
            case 'B': /* Prompt end, input start */
                t->current_zone = ZONE_INPUT;
                t->input_start_row = t->cursor_y;
                mark_current_row_zone(t);
                break;
            case 'C': /* Input end, command start */
                capture_pending_command(t);
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
                        exit_code = atoi(exit_str + 5);
                    } else if (text[1] == ';') {
                        exit_code = atoi(text + 2);
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

                if (t->on_command_finished && t->pending_command && t->pending_command[0]) {
                    t->on_command_finished(t, t->pending_command, exit_code, t->command_userdata);
                }
                if (t->pending_command) t->pending_command[0] = '\0';
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
 * Agent inline-image detection
 *
 * When a program (typically an AI agent) prints a line that names a local
 * image file it just produced — e.g. "saved screenshot to /tmp/shot.png" — we
 * decode that file and stash it in t->pending_img so the line-feed handler can
 * drop it into the scrolling text flow right below the path. The strong filter
 * is stat(): the path must resolve to a real local file and decode, so remote
 * SSH paths (which won't exist locally) and non-image text fall through.
 * ========================================================================= */

static bool path_has_image_ext(const char *s, usize len) {
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

static u64 inline_img_fnv1a(const char *s, usize n) {
    u64 h = 1469598103934665603ULL;
    for (usize i = 0; i < n; i++) { h ^= (u8)s[i]; h *= 1099511628211ULL; }
    return h;
}

/* Decode the image named on the just-completed line (grid row cursor_y) into
 * t->pending_img. Returns true if one is parked and ready to place. */
static bool term_scan_line_for_image(Terminal *t) {
    if (!t || !t->inline_image_detect) return false;
    if (t->mode & MODE_ALT_SCREEN) return false;   /* TUIs repaint; never scan */
    if (t->pending_img) return false;              /* one already in flight */
    if (t->cursor_y < 0 || t->cursor_y >= t->rows || !t->cells) return false;

    /* Materialize the completed line as trimmed UTF-8. */
    char line[2048];
    usize ln = 0;
    Cell *row = &t->cells[t->cursor_y * t->cols];
    for (i32 x = 0; x < t->cols && ln + 4 < sizeof(line); x++) {
        u32 cp = row[x].codepoint ? row[x].codepoint : ' ';
        u8 enc[4];
        u32 n = utf8_encode(cp, enc);
        for (u32 i = 0; i < n && ln + 1 < sizeof(line); i++) line[ln++] = (char)enc[i];
    }
    while (ln > 0 && (line[ln-1] == ' ' || line[ln-1] == '\t')) ln--;
    line[ln] = '\0';
    if (ln < 5) return false;

    /* Find the LAST whitespace-delimited token that, after stripping common
     * wrappers (quotes, parens, brackets, markdown ![](...) ), ends in an image
     * extension. Right-to-left favours the trailing "… saved to <path>" shape. */
    char tok[1024];
    bool found = false;
    usize i = ln;
    while (i > 0) {
        usize b = i;
        while (b > 0 && (line[b-1] == ' ' || line[b-1] == '\t')) b--;
        usize a = b;
        while (a > 0 && line[a-1] != ' ' && line[a-1] != '\t') a--;
        if (b > a) {
            usize s0 = a, s1 = b;
            while (s0 < s1 && (line[s0]=='"'||line[s0]=='\''||line[s0]=='('||
                               line[s0]=='<'||line[s0]=='['||line[s0]=='`')) s0++;
            while (s1 > s0 && (line[s1-1]=='"'||line[s1-1]=='\''||line[s1-1]==')'||
                               line[s1-1]=='>'||line[s1-1]==']'||line[s1-1]=='`'||
                               line[s1-1]==',')) s1--;
            usize tl = s1 - s0;
            if (tl > 0 && tl < sizeof(tok) && path_has_image_ext(&line[s0], tl)) {
                /* Require a real path, not a bare filename: it must be
                 * absolute (/…), home-relative (~/…) or contain a directory
                 * separator (./out/x.png, shots/y.png). A column of bare
                 * `*.png` names from `ls` has no separator and is rejected — a
                 * second guard on top of the agent-foreground gate. */
                bool path_like = false;
                for (usize k = s0; k < s1; k++) {
                    if (line[k] == '/') { path_like = true; break; }
                }
                if (line[s0] == '~') path_like = true;
                if (path_like) {
                    memcpy(tok, &line[s0], tl);
                    tok[tl] = '\0';
                    found = true;
                    break;
                }
            }
        }
        i = a;
    }
    if (!found) return false;

    /* Resolve to an absolute path. */
    char path[2048];
    if (tok[0] == '/') {
        snprintf(path, sizeof(path), "%s", tok);
    } else if (tok[0] == '~' && tok[1] == '/') {
        const char *home = getenv("HOME");
        snprintf(path, sizeof(path), "%s/%s", home ? home : "", tok + 2);
    } else if (t->cwd && t->cwd[0]) {
        snprintf(path, sizeof(path), "%s/%s", t->cwd, tok);
    } else {
        snprintf(path, sizeof(path), "%s", tok);
    }

    /* Strong filter: real, regular, sanely-sized local file. */
    struct stat stbuf;
    if (stat(path, &stbuf) != 0) return false;
    if (!S_ISREG(stbuf.st_mode)) return false;
    if (stbuf.st_size <= 0 || stbuf.st_size > (off_t)(32 * 1024 * 1024)) return false;

    /* Dedup against a tight repaint loop; a rewritten file (new mtime) shows. */
    u64 key = inline_img_fnv1a(path, strlen(path)) ^
              ((u64)stbuf.st_mtime * 1099511628211ULL);
    if (key == t->last_inline_img_key) return false;

    int w = 0, h = 0, comp = 0;
    u8 *px = stbi_load(path, &w, &h, &comp, 4);
    if (!px) return false;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { stbi_image_free(px); return false; }

    t->pending_img         = px;
    t->pending_img_w       = w;
    t->pending_img_h       = h;
    /* Stash the resolved source so the placed thumbnail can be expanded to the
     * full-resolution file on click. */
    free(t->pending_img_path);
    t->pending_img_path = strdup(path);
    t->last_inline_img_key = key;
    return true;
}

/* =========================================================================
 * Main feed function — state machine
 * ========================================================================= */

void terminal_feed(Terminal *t, const u8 *data, usize len) {
    t->dirty = true;
    TERM_DIRTY_ROW(t, t->cursor_y);
    /* Do NOT yank the view to the bottom on output. While the user has scrolled
     * up into history (scroll_offset > 0) — e.g. reading back through a running
     * agent's stream — buffer_scroll_up keeps their view anchored as new lines
     * enter scrollback. The view returns to the live bottom only when the user
     * scrolls down to it or types (app_send_input snaps it). */

    for (usize pos = 0; pos < len; pos++) {
        u8 b = data[pos];

        switch (t->parse_state) {
        case STATE_GROUND:
            if (b == 0x1B) { /* ESC */
                t->parse_state = STATE_ESC;
                t->esc_len = 0;
                t->param_count = 0;
                t->param_has_value = false;
                memset(t->params, 0, sizeof(t->params));
            } else if (b == '\n' || b == '\x0B' || b == '\x0C') {
                /* Scan the line we're leaving for an agent-produced image path
                 * BEFORE advancing, so the row index is still valid. */
                bool place_img = term_scan_line_for_image(t);
                t->cursor_y++;
                if (t->cursor_y > t->scroll_bottom) {
                    t->cursor_y = t->scroll_bottom;
                    buffer_scroll_up(t, 1);
                }
                /* Place after the LF so the image anchors to the blank line
                 * below the path text and flows the rest of the output down. */
                if (place_img && t->pending_img) {
                    u8 *px = t->pending_img;
                    i32 pw = t->pending_img_w, ph = t->pending_img_h;
                    t->pending_img = NULL;
                    t->pending_img_w = t->pending_img_h = 0;
                    terminal_place_image_inline(t, px, pw, ph);
                }
            } else if (b == '\r') {
                t->cursor_x = 0;
            } else if (b == '\b') {
                if (t->cursor_x > 0) t->cursor_x--;
            } else if (b == '\t') {
                /* Advance to next tab stop */
                t->cursor_x++;
                while (t->cursor_x < t->cols && !t->tab_stops[t->cursor_x]) {
                    t->cursor_x++;
                }
                if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
            } else if (b == '\a') { /* Bell */
                if (t->on_bell) t->on_bell(t, t->userdata);
            } else if (b == 0x0E || b == 0x0F) {
                /* SO/SI — character set shift, ignore */
            } else if (b >= 0x20) {
                /* Printable character — handle UTF-8 */
                u32 codepoint;
                if (b < 0x80) {
#if HAVE_VT_ASM
                    usize remaining = len - pos;
                    int scan_len = remaining > (usize)INT_MAX ? INT_MAX : (int)remaining;
                    int run = asm_find_printable_run(data + pos, scan_len);
                    if (run > 1) {
                        for (int i = 0; i < run; i++) {
                            put_char(t, data[pos + (usize)i]);
                        }
                        pos += (usize)run - 1;
                        break;
                    }
#endif
                    put_char(t, b);
                } else {
                    u32 consumed = utf8_decode(data + pos, len - pos, &codepoint);
                    if (consumed > 0) {
                        /* Check for combining characters (U+0300-U+036F, U+20D0-U+20FF, etc.)
                         * These modify the previous character, not occupy a new cell */
                        bool is_combining =
                            (codepoint >= 0x0300 && codepoint <= 0x036F) ||  /* Combining Diacriticals */
                            (codepoint >= 0x1AB0 && codepoint <= 0x1AFF) ||  /* Combining Diacriticals Extended */
                            (codepoint >= 0x1DC0 && codepoint <= 0x1DFF) ||  /* Combining Diacriticals Supplement */
                            (codepoint >= 0x20D0 && codepoint <= 0x20FF) ||  /* Combining for Symbols */
                            (codepoint >= 0xFE00 && codepoint <= 0xFE0F) ||  /* Variation Selectors */
                            (codepoint >= 0xFE20 && codepoint <= 0xFE2F) ||  /* Combining Half Marks */
                            (codepoint == 0x200D);                            /* Zero-Width Joiner */

                        if (is_combining) {
                            /* Skip — the base character already occupies the cell.
                             * A full implementation would store combining marks
                             * in the cell and composite-render them. */
                        } else {
                            put_char(t, codepoint);
                        }
                        pos += consumed - 1;
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
                t->parse_state = STATE_CHARSET;
            } else if (b == '7') { /* DECSC — Save Cursor */
                t->saved_x = t->cursor_x;
                t->saved_y = t->cursor_y;
                t->saved_attr = t->cursor_attr;
                t->saved_ul_color = t->cursor_ul_color;
                t->saved_ul_style = t->cursor_ul_style;
                t->parse_state = STATE_GROUND;
            } else if (b == '8') { /* DECRC — Restore Cursor */
                t->cursor_x = t->saved_x;
                t->cursor_y = t->saved_y;
                t->cursor_attr = t->saved_attr;
                t->cursor_ul_color = t->saved_ul_color;
                t->cursor_ul_style = t->saved_ul_style;
                cursor_clamp(t);
                t->parse_state = STATE_GROUND;
            } else if (b == 'D') { /* IND — Index (scroll up) */
                t->cursor_y++;
                if (t->cursor_y > t->scroll_bottom) {
                    t->cursor_y = t->scroll_bottom;
                    buffer_scroll_up(t, 1);
                }
                t->parse_state = STATE_GROUND;
            } else if (b == 'M') { /* RI — Reverse Index (scroll down) */
                t->cursor_y--;
                if (t->cursor_y < t->scroll_top) {
                    t->cursor_y = t->scroll_top;
                    buffer_scroll_down(t, 1);
                }
                t->parse_state = STATE_GROUND;
            } else if (b == 'E') { /* NEL — Next Line */
                t->cursor_x = 0;
                t->cursor_y++;
                if (t->cursor_y > t->scroll_bottom) {
                    t->cursor_y = t->scroll_bottom;
                    buffer_scroll_up(t, 1);
                }
                t->parse_state = STATE_GROUND;
            } else if (b == 'c') { /* RIS — Full Reset */
                terminal_resize(t, t->cols, t->rows); /* Reinitializes everything */
                t->modify_other_keys = 0;
                t->mode &= ~MODE_MODIFY_OTHER_KEYS;
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

                if (t->apc_buf) { free(t->apc_buf); t->apc_buf = NULL; t->apc_len = 0; t->apc_cap = 0; }
                t->parse_state = STATE_GROUND;
            } else if (b == '\a') {
                /* APC terminated by BEL */
                u8 *apc_data = t->apc_buf ? t->apc_buf : t->esc_buf;
                i32 apc_len2 = t->apc_buf ? t->apc_len : t->esc_len;

                if (apc_len2 > 1 && apc_data[0] == 'G') {
                    handle_Liu_graphics(t, apc_data + 1, apc_len2 - 1);
                }

                if (t->apc_buf) { free(t->apc_buf); t->apc_buf = NULL; t->apc_len = 0; t->apc_cap = 0; }
                t->parse_state = STATE_GROUND;
            } else {
                /* Accumulate APC data — use dynamic buffer for large payloads (images) */
                if (t->esc_len < (i32)sizeof(t->esc_buf) - 1 && !t->apc_buf) {
                    t->esc_buf[t->esc_len++] = b;
                } else {
                    /* Overflow to dynamic buffer */
                    if (!t->apc_buf) {
                        t->apc_cap = 65536;
                        t->apc_buf = malloc((usize)t->apc_cap);
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
                                free(t->apc_buf); t->apc_buf = NULL;
                                t->parse_state = STATE_GROUND;
                                break;
                            }
                            u8 *grown = realloc(t->apc_buf, (usize)new_cap);
                            if (!grown) {
                                free(t->apc_buf); t->apc_buf = NULL;
                                t->parse_state = STATE_GROUND;
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

        case STATE_CSI:
            if (b >= '0' && b <= '9') {
                if (t->param_count == 0) t->param_count = 1;
                t->params[t->param_count - 1] = t->params[t->param_count - 1] * 10 + (b - '0');
                t->param_has_value = true;
            } else if (b == ';') {
                if (t->param_count < 16) {
                    if (!t->param_has_value && t->param_count == 0) t->param_count = 1;
                    t->param_count++;
                    t->param_has_value = false;
                }
            } else if (b == '?') {
                /* DEC private mode prefix — store in esc_buf */
                if (t->esc_len < (i32)sizeof(t->esc_buf) - 1) {
                    t->esc_buf[t->esc_len++] = b;
                }
            } else if (b == '>' || b == '<' || b == '!' || b == ' ' || b == '$' || b == '=') {
                /* Intermediate bytes */
                if (t->esc_len < (i32)sizeof(t->esc_buf) - 1) {
                    t->esc_buf[t->esc_len++] = b;
                }
            } else if (b == ':') {
                /* Colon subparameter separator — treated like ';' but marks subparam */
                if (t->param_count < 16) {
                    if (!t->param_has_value && t->param_count == 0) t->param_count = 1;
                    t->param_count++;
                    t->param_has_value = false;
                }
            } else if (b >= 0x40 && b <= 0x7E) {
                /* Final byte — dispatch */
                bool is_dec = (t->esc_len > 0 && t->esc_buf[0] == '?');
                bool is_gt  = (t->esc_len > 0 && t->esc_buf[0] == '>');
                bool is_bang = (t->esc_len > 0 && t->esc_buf[0] == '!');
                bool is_space = (t->esc_len > 0 && t->esc_buf[0] == ' ');
                bool is_dollar = false;
                for (i32 ei = 0; ei < t->esc_len; ei++) { if (t->esc_buf[ei] == '$') is_dollar = true; }

                if (is_dec) {
                    if (b == 'h') handle_dec_mode(t, true);
                    else if (b == 'l') handle_dec_mode(t, false);
                    else if (b == 'n') {
                        /* DEC DSR variants */
                        i32 ps = param(t, 0, 0);
                        if (ps == 6) {
                            /* Extended cursor position */
                            emit_response_fmt(t, "\x1b[?%d;%d;1R", t->cursor_y + 1, t->cursor_x + 1);
                        }
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
                        case 1049: val = (t->mode & MODE_ALT_SCREEN) ? 1 : 2; break;
                        case 2004: val = (t->mode & MODE_BRACKETED_PASTE) ? 1 : 2; break;
                        case 2026: val = (t->mode & MODE_SYNC_OUTPUT) ? 1 : 2; break;
                        case 2027: val = (t->mode & MODE_GRAPHEME) ? 1 : 2; break;
                        case 2048: val = (t->mode & MODE_IN_BAND_RESIZE) ? 1 : 2; break;
                        case 2016: val = (t->modify_other_keys > 0) ? 1 : 2; break;
                        }
                        emit_response_fmt(t, "\x1b[?%d;%d$y", mode, val);
                    }
                } else if (is_gt) {
                    if (b == 'c') {
                        /* Secondary DA */
                        emit_response_fmt(t, "\x1b[>1;100;0c"); /* type=1, version=1.00 */
                    } else if (b == 'q') {
                        /* XTVERSION */
                        emit_response_fmt(t, "\x1bP>|Liu 1.0\x1b\\");
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
                        if (t->Liu_kbd_stack_len < 8) {
                            t->Liu_kbd_stack[t->Liu_kbd_stack_len++] = t->Liu_kbd_flags;
                        }
                        t->Liu_kbd_flags = (u32)flags;
                    }
                } else if (is_bang && b == 'p') {
                    /* DECSTR — Soft Terminal Reset */
                    t->cursor_attr.fg = FG_DEFAULT;
                    t->cursor_attr.bg = BG_DEFAULT;
                    t->cursor_ul_color = 0;
                    t->cursor_attr.flags = 0;
                    t->cursor_ul_style = 0;
                    t->cursor_x = 0;
                    t->cursor_y = 0;
                    t->cursor_visible = true;
                    t->cursor_style = CURSOR_BLOCK;
                    t->scroll_top = 0;
                    t->scroll_bottom = t->rows - 1;
                    t->mode = MODE_WRAP;
                    t->Liu_kbd_flags = 0;
                    t->Liu_kbd_stack_len = 0;
                    t->modify_other_keys = 0;
                    TERM_DIRTY_ALL(t);
                } else if (is_space && b == 'q') {
                    /* DECSCUSR — Cursor shape (all 6 values) */
                    i32 style = param(t, 0, 1);
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
                    if (is_dec) {
                        /* CSI ? u — query current flags */
                        emit_response_fmt(t, "\x1b[?%uu", t->Liu_kbd_flags);
                    } else if (t->esc_len > 0 && t->esc_buf[0] == '<') {
                        /* CSI < n u — pop flags from stack */
                        i32 pop_count = param(t, 0, 1);
                        for (i32 pi = 0; pi < pop_count && t->Liu_kbd_stack_len > 0; pi++) {
                            t->Liu_kbd_flags = t->Liu_kbd_stack[--t->Liu_kbd_stack_len];
                        }
                        if (t->Liu_kbd_stack_len == 0) t->Liu_kbd_flags = 0;
                    } else if (is_gt) {
                        /* CSI > flags u — push and set (already handled above) */
                    } else {
                        /* CSI u — legacy encoding, ignore */
                    }
                } else if (b == 't') {
                    /* XTWINOPS — window operations */
                    i32 op = param(t, 0, 0);
                    switch (op) {
                    case 14: /* Report text area size in pixels */
                        emit_response_fmt(t, "\x1b[4;%d;%dt", t->rows * 16, t->cols * 8);
                        break;
                    case 16: /* Report cell size in pixels */
                        emit_response_fmt(t, "\x1b[6;16;8t");
                        break;
                    case 18: /* Report text area size in chars */
                        emit_response_fmt(t, "\x1b[8;%d;%dt", t->rows, t->cols);
                        break;
                    case 22: /* Push title */
                        if (param(t, 1, 0) == 2 && t->title_stack_len < 8) {
                            if (!t->title_stack) {
                                t->title_stack = calloc(8, 256);
                                if (!t->title_stack) break;
                            }
                            snprintf(t->title_stack[t->title_stack_len], 256, "%s", t->title);
                            t->title_stack_len++;
                        }
                        break;
                    case 23: /* Pop title */
                        if (param(t, 1, 0) == 2 && t->title_stack_len > 0) {
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
                    osc_cmd = osc_cmd * 10 + (osc_data[oi] - '0');
                    oi++;
                }

                if (osc_cmd == 1337 && oi < osc_len && osc_data[oi] == ';') {
                    /* Inline image (OSC 1337) */
                    oi++; /* skip ';' */
                    handle_iterm2_image(t, osc_data + oi, osc_len - oi);
                } else {
                    /* Standard OSC — use esc_buf path (small buffer is fine) */
                    if (!t->dcs_buf) {
                        t->esc_buf[t->esc_len] = '\0';
                    } else {
                        /* Copy back to esc_buf if it fits (standard OSCs are small) */
                        i32 copy_len = osc_len < (i32)sizeof(t->esc_buf) - 1 ? osc_len : (i32)sizeof(t->esc_buf) - 1;
                        memcpy(t->esc_buf, osc_data, (usize)copy_len);
                        t->esc_len = copy_len;
                        t->esc_buf[t->esc_len] = '\0';
                    }
                    handle_osc(t);
                }

                if (t->dcs_buf) { free(t->dcs_buf); t->dcs_buf = NULL; t->dcs_len = 0; t->dcs_cap = 0; }
                t->parse_state = STATE_GROUND;
            } else {
                /* Accumulate OSC data — use dynamic buffer for large payloads (inline images) */
                if (t->esc_len < (i32)sizeof(t->esc_buf) - 1 && !t->dcs_buf) {
                    t->esc_buf[t->esc_len++] = b;
                } else {
                    /* Overflow to dynamic buffer (reuse dcs_buf) */
                    if (!t->dcs_buf) {
                        t->dcs_cap = 65536;
                        t->dcs_buf = malloc((usize)t->dcs_cap);
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
                                free(t->dcs_buf); t->dcs_buf = NULL;
                                t->parse_state = STATE_GROUND;
                                break;
                            }
                            u8 *grown = realloc(t->dcs_buf, (usize)new_cap);
                            if (!grown) {
                                free(t->dcs_buf); t->dcs_buf = NULL;
                                t->parse_state = STATE_GROUND;
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
            /* DCS sequences — absorb until ST, then process */
            if (b == '\\' && t->esc_len > 0 && t->esc_buf[t->esc_len-1] == 0x1B) {
                t->esc_len--; /* remove ESC */
                t->esc_buf[t->esc_len] = '\0';
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
                    if (data_start > 0 && t->image_count < MAX_TERM_IMAGES) {
                        /* Parse sixel data into RGBA bitmap */
                        i32 sx = 0, sy = 0, max_x = 0, max_y = 0;
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
                        { i32 px = 0, py = 0;
                          for (i32 si = data_start; si < dcs_len2; si++) {
                              u8 c = dcs_data[si];
                              if (c >= 0x3F && c <= 0x7E) { px++; if (px > tw) tw = px; if (py + 6 > th) th = py + 6; }
                              else if (c == '$') px = 0;
                              else if (c == '-') { px = 0; py += 6; }
                              else if (c == '!') { /* repeat */
                                  i32 rep = 0; si++;
                                  while (si < dcs_len2 && dcs_data[si] >= '0' && dcs_data[si] <= '9')
                                      rep = rep * 10 + (dcs_data[si++] - '0');
                                  si--; px += rep; if (px > tw) tw = px;
                              }
                          }
                        }
                        if (tw > 0 && th > 0 && tw <= 2048 && th <= 2048) {
                            u8 *pixels = calloc(1, (usize)(tw * th * 4));
                            if (pixels) {
                                /* Second pass: decode pixels */
                                for (i32 si = data_start; si < dcs_len2; si++) {
                                    u8 c = dcs_data[si];
                                    if (c == '#') {
                                        /* Color command: #idx;type;r;g;b */
                                        si++;
                                        i32 idx = 0;
                                        while (si < dcs_len2 && dcs_data[si] >= '0' && dcs_data[si] <= '9')
                                            idx = idx * 10 + (dcs_data[si++] - '0');
                                        if (si < dcs_len2 && dcs_data[si] == ';') {
                                            si++; /* type */
                                            i32 type = 0;
                                            while (si < dcs_len2 && dcs_data[si] >= '0' && dcs_data[si] <= '9')
                                                type = type * 10 + (dcs_data[si++] - '0');
                                            i32 rgb[3] = {0};
                                            for (i32 ci2 = 0; ci2 < 3 && si < dcs_len2; ci2++) {
                                                if (dcs_data[si] == ';') si++;
                                                while (si < dcs_len2 && dcs_data[si] >= '0' && dcs_data[si] <= '9')
                                                    rgb[ci2] = rgb[ci2] * 10 + (dcs_data[si++] - '0');
                                            }
                                            if (type == 2 && idx < 256) {
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
                                        if (sx > max_x) max_x = sx;
                                        if (sy + 6 > max_y) max_y = sy + 6;
                                    } else if (c == '$') {
                                        sx = 0; /* carriage return */
                                    } else if (c == '-') {
                                        sx = 0; sy += 6; /* newline */
                                    } else if (c == '!') {
                                        /* Repeat: !count<sixel> */
                                        si++; i32 rep = 0;
                                        while (si < dcs_len2 && dcs_data[si] >= '0' && dcs_data[si] <= '9')
                                            rep = rep * 10 + (dcs_data[si++] - '0');
                                        if (si < dcs_len2) {
                                            u8 sc = dcs_data[si];
                                            if (sc >= 0x3F && sc <= 0x7E) {
                                                u8 bits = sc - 0x3F;
                                                for (i32 ri = 0; ri < rep; ri++) {
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
                                            }
                                        }
                                    }
                                }
                                /* Store image */
                                terminal_add_image(t, pixels,
                                    max_x > 0 ? max_x : tw,
                                    max_y > 0 ? max_y : th,
                                    t->cursor_x, t->cursor_y, 0, 0, 0, 0);
                            }
                        }
                    }
                    if (t->dcs_buf) { free(t->dcs_buf); t->dcs_buf = NULL; t->dcs_len = 0; t->dcs_cap = 0; }
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
                        char hex_key[128] = {0};
                        for (i32 ki = 0; cap_name[ki]; ki++) sprintf(hex_key + ki*2, "%02x", (u8)cap_name[ki]);
                        char hex_val[128] = {0};
                        for (i32 vi2 = 0; vbuf[vi2]; vi2++) sprintf(hex_val + vi2*2, "%02x", (u8)vbuf[vi2]);
                        emit_response_fmt(t, "\x1bP1+r%s=%s\x1b\\", hex_key, hex_val);
                        if (t->dcs_buf) { free(t->dcs_buf); t->dcs_buf = NULL; t->dcs_len = 0; t->dcs_cap = 0; }
                        t->parse_state = STATE_GROUND;
                        break;
                    }

                    if (val) {
                        /* Respond: DCS 1 + r hexkey=hexval ST */
                        char hex_key[128] = {0};
                        for (i32 ki = 0; cap_name[ki]; ki++) sprintf(hex_key + ki*2, "%02x", (u8)cap_name[ki]);
                        char hex_val[128] = {0};
                        for (i32 vi2 = 0; val[vi2]; vi2++) sprintf(hex_val + vi2*2, "%02x", (u8)val[vi2]);
                        emit_response_fmt(t, "\x1bP1+r%s=%s\x1b\\", hex_key, hex_val);
                    } else {
                        /* Not found */
                        char hex_key[128] = {0};
                        for (i32 ki = 0; cap_name[ki]; ki++) sprintf(hex_key + ki*2, "%02x", (u8)cap_name[ki]);
                        emit_response_fmt(t, "\x1bP0+r%s\x1b\\", hex_key);
                    }
                }
                /* Free any overflow buffer before leaving DCS — a non-sixel DCS
                 * that overflowed esc_buf and terminated with ST left dcs_buf
                 * allocated; without this it was reused stale across the next
                 * DCS/OSC (corruption) and its length crept toward the 4 MB cap. */
                if (t->dcs_buf) { free(t->dcs_buf); t->dcs_buf = NULL; t->dcs_len = 0; t->dcs_cap = 0; }
                t->parse_state = STATE_GROUND;
            } else if (b == '\a') {
                /* Free DCS buffer if used */
                if (t->dcs_buf) { free(t->dcs_buf); t->dcs_buf = NULL; t->dcs_len = 0; t->dcs_cap = 0; }
                t->parse_state = STATE_GROUND;
            } else {
                /* Accumulate DCS data — use dynamic buffer for large sequences (Sixel) */
                if (t->esc_len < (i32)sizeof(t->esc_buf) - 1) {
                    t->esc_buf[t->esc_len++] = b;
                } else {
                    /* Overflow to dynamic buffer */
                    if (!t->dcs_buf) {
                        t->dcs_cap = 65536;
                        t->dcs_buf = malloc((usize)t->dcs_cap);
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
                                free(t->dcs_buf); t->dcs_buf = NULL;
                                t->parse_state = STATE_GROUND;
                                break;
                            }
                            u8 *grown = realloc(t->dcs_buf, (usize)new_cap);
                            if (!grown) {
                                free(t->dcs_buf); t->dcs_buf = NULL;
                                t->parse_state = STATE_GROUND;
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
                for (i32 y = 0; y < t->rows; y++) {
                    for (i32 x = 0; x < t->cols; x++) {
                        t->cells[y * t->cols + x].codepoint = 'E';
                    }
                }
            }
            t->parse_state = STATE_GROUND;
            break;

        case STATE_CHARSET:
            /* Absorb one byte (charset designation), ignore */
            t->parse_state = STATE_GROUND;
            break;
        }
    }
}
