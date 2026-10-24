/* http_status.h — the HTTP status curl's `--write-out '%{http_code}'` prints.
 *
 * curl's exit code cannot tell a missing feed from a broken connection: an
 * HTTP/2 404 under --fail is 22 on one curl and 56 on another. The shims ask
 * curl to print the last response's status and read it back with this. */
#ifndef LIU_UPDATE_HTTP_STATUS_H
#define LIU_UPDATE_HTTP_STATUS_H

#include <stddef.h>

/* The status at the end of `text`: 100-599, or 0 when no response came ("000")
 * or the text ends in no three-digit status. */
static inline int updater_http_status(const char *text, size_t len) {
    if (!text) return 0;
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' ||
                       text[len - 1] == ' '  || text[len - 1] == '\t'))
        len--;
    if (len < 3) return 0;
    const char *d = text + len - 3;
    if (len > 3 && d[-1] >= '0' && d[-1] <= '9') return 0;   /* more than three digits */
    int code = 0;
    for (int i = 0; i < 3; i++) {
        if (d[i] < '0' || d[i] > '9') return 0;
        code = code * 10 + (d[i] - '0');
    }
    return (code >= 100 && code <= 599) ? code : 0;
}

#endif /* LIU_UPDATE_HTTP_STATUS_H */
