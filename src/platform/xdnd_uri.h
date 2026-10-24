/*
 * Liu — a file:// URI from an XDND drop, turned into a path.
 *
 * The drop comes from ANOTHER X client, so the URI is not Liu's data: whatever program
 * the user dragged from chose every byte of it. Three ways a plain percent-decoder gets
 * that wrong, each of which makes the app act on a path other than the one that was
 * dropped:
 *
 *   - `%00` decodes to a NUL, and every consumer is a C string, so
 *     file:///home/u/report.txt%00.png is the path /home/u/report.txt — the name a
 *     filter saw is not the file that opens;
 *   - a path longer than the buffer, cut at the buffer's end without a word, can name
 *     one of its PARENT directories, and the drop would act on a directory the user
 *     never dragged;
 *   - decoded control bytes (a newline, an ESC) would go into paths that are later
 *     drawn, logged and typed at a prompt.
 *
 * All three are refused: the result is a path or nothing. Only an empty or "localhost"
 * authority is local (file://host/... is a remote file). Pure, so it builds on any
 * platform.
 */
#ifndef PLATFORM_XDND_URI_H
#define PLATFORM_XDND_URI_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline int xdnd_hexv(int c) {
    return c >= '0' && c <= '9' ? c - '0' :
           c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Returns true and a NUL-terminated absolute path in `out`, or false (and an empty `out`). */
static inline bool xdnd_uri_to_path(const char *uri, size_t len, char *out, size_t cap) {
    static const char kPrefix[] = "file://";
    const size_t plen = sizeof(kPrefix) - 1;
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!uri || len <= plen || strncmp(uri, kPrefix, plen) != 0) return false;

    const char *p = uri + plen;
    size_t rest = len - plen;
    /* file://host/path — only an empty or "localhost" authority is ours. */
    if (rest > 0 && *p != '/') {
        const char *slash = (const char *)memchr(p, '/', rest);
        if (!slash) return false;
        size_t hostlen = (size_t)(slash - p);
        if (!(hostlen == 9 && strncmp(p, "localhost", 9) == 0)) return false;
        rest -= hostlen;
        p = slash;
    }

    size_t o = 0;
    for (size_t i = 0; i < rest; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == '%' && i + 2 < rest) {
            int h = xdnd_hexv((unsigned char)p[i + 1]), l = xdnd_hexv((unsigned char)p[i + 2]);
            if (h >= 0 && l >= 0) {
                c = (unsigned char)((h << 4) | l);
                i += 2;
            }
        }
        if (c < 0x20 || c == 0x7f) { out[0] = '\0'; return false; }   /* NUL included */
        if (o + 1 >= cap) { out[0] = '\0'; return false; }            /* would be cut: refuse */
        out[o++] = (char)c;
    }
    out[o] = '\0';
    if (o == 0 || out[0] != '/') { out[0] = '\0'; return false; }
    return true;
}

#endif /* PLATFORM_XDND_URI_H */
