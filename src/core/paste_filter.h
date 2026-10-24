/*
 * Liu — what a paste may carry into a PTY, and why ESC is never part of it.
 *
 * A paste is DATA until the user presses Enter. Two promises protect that, and
 * both are broken by control bytes inside the clipboard:
 *
 *   1. Bracketed paste (ESC[200~ ... ESC[201~) tells the far end that
 *      everything between the brackets is data. A clipboard that carries the
 *      terminator itself ends the paste early; the remainder arrives as
 *      KEYSTROKES, and a newline in it submits the line with nobody having
 *      pressed anything: a clipboard of "harmless" + ESC[201~ + newline + a
 *      command, written the way app_send_bracketed_paste_ex writes it, runs
 *      the command in zsh.
 *   2. Without bracketing the clipboard is typed input by definition, so a
 *      raw ^C, ^D, ^Z, ESC or DEL in it is a KEYSTROKE the user never made
 *      (an invisible 0x03 in pasted or dropped text interrupts the program).
 *
 * Removing the six bytes of the terminator in one pass is not enough:
 * "ESC[20" + ESC[201~ + "1~" collapses to ESC[201~ once the middle is gone.
 * A pattern removed in one pass can be rebuilt from what surrounds it, so the
 * rule is not a pattern at all: every C0 control byte except TAB, LF and CR,
 * and DEL, is dropped, in BOTH modes. ESC is a C0 byte, so no terminator, no
 * CSI and no OSC can be formed from a paste, and nothing is left to reassemble
 * because the filter is per byte. The cost is deliberate: a paste of coloured
 * terminal output loses its ESC bytes and shows "[31m" as text — raw ESC
 * pasted into a prompt is never useful, and into vim's insert mode it is
 * harmful.
 *
 * LF and CR are KEPT. A multi-line paste is what bracketed paste exists for,
 * and without it a newline submitting the line is the terminal's contract with
 * the user; ui/paste_confirm.h shows such a paste and asks before it is sent.
 *
 * Kept here, as pure byte arithmetic, for the reason core/path_escape.h and
 * ui/model_delete.h are: one rule, one irreversible consequence if it is
 * wrong, and code that can be checked without the app.
 */
#ifndef CORE_PASTE_FILTER_H
#define CORE_PASTE_FILTER_H

#include "core/types.h"
#include <string.h>

/* Every byte the filter removes is exactly one byte long, so a caller that
 * streams a clipboard in segments steps over a hit with this. */
#define PASTE_DROP_LEN 1

/* True when `c` may not travel in a paste: a C0 control other than TAB / LF /
 * CR, or DEL. */
static inline bool paste_byte_unsafe(unsigned char c) {
    return (c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7f;
}

/* Byte offset of the next byte that must be dropped, at or after `s`, or -1
 * when there is none. Lets a caller stream an arbitrarily large clipboard in
 * segments without allocating a filtered copy of it. */
static inline isize paste_filter_find(const char *s) {
    if (!s) return -1;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (paste_byte_unsafe(*p)) return (isize)(p - (const unsigned char *)s);
    return -1;
}

/* True when `s` contains anything that has to be filtered. */
static inline bool paste_filter_needed(const char *s) {
    return paste_filter_find(s) >= 0;
}

/* Copy `src` into `dst` with every unsafe byte removed, NUL-terminated and
 * truncated at cap-1. Returns the bytes written, excluding the NUL.
 *
 * The result never contains an unsafe byte, whatever the truncation: the
 * filter is per byte, so there is no multi-byte pattern for a cut to split. */
static inline usize paste_filter_copy(char *dst, usize cap, const char *src) {
    if (!dst || cap == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }
    usize w = 0;
    const char *p = src;
    for (;;) {
        isize at = paste_filter_find(p);
        usize seg = (at >= 0) ? (usize)at : strlen(p);
        if (seg > cap - 1 - w) seg = cap - 1 - w;
        memcpy(dst + w, p, seg);
        w += seg;
        if (at < 0 || w >= cap - 1) break;
        p += (usize)at + PASTE_DROP_LEN;
    }
    dst[w] = '\0';
    return w;
}

#endif /* CORE_PASTE_FILTER_H */
