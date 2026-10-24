/*
 * Liu — the questions a program asks its terminal whose answer needs nothing
 * but the cursor, spelled once for both places that answer them.
 *
 * The parser (vt_parser.c) answers every question on the main thread, after
 * the pty reader thread has handed the bytes over: a cursor report waits for
 * that hop and for whatever the main thread is doing (a frame, an event pass).
 * A chunk that is NOTHING but such questions is answered by the reader thread
 * itself (core/pty_reader.c), from the cursor the main thread published after
 * it had parsed everything before it; anything else in the chunk, and every
 * other question, still goes to the parser.
 *
 * The replies here are the parser's own: vt_parser.c formats with the same
 * strings and the same function, so the two answer byte for byte alike.
 *
 * Depends on core/types.h only, like the reader.
 */
#ifndef CORE_TERM_REPLY_H
#define CORE_TERM_REPLY_H

#include "core/types.h"

#include <stdio.h>
#include <string.h>

#define TERM_REPLY_STATUS_OK  "\x1b[0n"          /* DSR 5: operating status */
#define TERM_REPLY_DA1        "\x1b[?62;22c"     /* primary DA: VT220, ANSI colour */
#define TERM_REPLY_DA2        "\x1b[>1;100;0c"   /* secondary DA: type 1, version 1.00 */
#define TERM_REPLY_XTVERSION  "\x1bP>|Liu 1.0\x1b\\"

/* The cursor position report, CSI 6 n (`dec` false: ESC [ row ; col R) and
 * CSI ? 6 n (`dec` true: ESC [ ? row ; col ; 1 R). `row` and `col` are
 * 1-based as reported (terminal_reply_position). Returns the length written,
 * 0 when it does not fit. */
static inline usize term_reply_cpr(char *out, usize cap, i32 row, i32 col, bool dec) {
    int n = dec ? snprintf(out, cap, "\x1b[?%d;%d;1R", row, col)
                : snprintf(out, cap, "\x1b[%d;%dR", row, col);
    return (n > 0 && (usize)n < cap) ? (usize)n : 0;
}

/* A chunk the reader may answer alone: one or more of the questions below,
 * back to back, in their plain spelling, and nothing else — not a byte before,
 * between or after them. Anything else (another parameter, a sub-parameter, a
 * leading zero, a question cut by the read) is the parser's. Returns the
 * length of the concatenated replies written to `out`, 0 when the chunk is not
 * entirely answerable or the replies do not fit.
 *
 *   CSI 5 n    CSI 6 n    CSI ? 6 n     status, cursor, cursor (DEC form)
 *   CSI c      CSI 0 c                  primary device attributes
 *   CSI > c    CSI > 0 c                secondary device attributes
 *   CSI > q    CSI > 0 q                XTVERSION */
static inline usize term_fast_reply(const u8 *b, usize n, i32 row, i32 col,
                                    u8 *out, usize cap) {
    static const struct { const char *q; u8 len; u8 kind; } QS[] = {
        { "\x1b[5n",  4, 0 }, { "\x1b[6n",  4, 1 }, { "\x1b[?6n", 5, 2 },
        { "\x1b[c",   3, 3 }, { "\x1b[0c",  4, 3 },
        { "\x1b[>c",  4, 4 }, { "\x1b[>0c", 5, 4 },
        { "\x1b[>q",  4, 5 }, { "\x1b[>0q", 5, 5 },
    };
    if (!b || !n || !out || !cap || row < 1 || col < 1) return 0;
    usize i = 0, w = 0;
    while (i < n) {
        i32 kind = -1;
        for (usize k = 0; k < sizeof QS / sizeof QS[0]; k++) {
            if (n - i >= QS[k].len && memcmp(b + i, QS[k].q, QS[k].len) == 0) {
                kind = QS[k].kind;
                i += QS[k].len;
                break;
            }
        }
        if (kind < 0) return 0;
        char tmp[48];
        usize len = 0;
        switch (kind) {
        case 0: len = sizeof TERM_REPLY_STATUS_OK - 1; memcpy(tmp, TERM_REPLY_STATUS_OK, len); break;
        case 1: len = term_reply_cpr(tmp, sizeof tmp, row, col, false); break;
        case 2: len = term_reply_cpr(tmp, sizeof tmp, row, col, true); break;
        case 3: len = sizeof TERM_REPLY_DA1 - 1; memcpy(tmp, TERM_REPLY_DA1, len); break;
        case 4: len = sizeof TERM_REPLY_DA2 - 1; memcpy(tmp, TERM_REPLY_DA2, len); break;
        default: len = sizeof TERM_REPLY_XTVERSION - 1; memcpy(tmp, TERM_REPLY_XTVERSION, len); break;
        }
        if (!len || cap - w < len) return 0;
        memcpy(out + w, tmp, len);
        w += len;
    }
    return w;
}

#endif
