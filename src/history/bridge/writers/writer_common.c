/*
 * liu-history/bridge - shared writer plumbing. See writers.h.
 */
#include "history/bridge/writers.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

bool bridge_path_exists(const char *path) {
    if (!path || !path[0]) return true;   /* treat "unknown" as occupied */
    struct stat st;
    /* lstat, not stat: a symlink at the target path exists as far as this
     * question is concerned, and following it is exactly what must not
     * happen. */
    return lstat(path, &st) == 0;
}

bool bridge_mkdir_p(const char *dir) {
    if (!dir || dir[0] != '/') return false;
    usize n = strlen(dir);
    if (n == 0 || n >= 4096) return false;
    while (n > 1 && dir[n - 1] == '/') n--;   /* a trailing slash is noise */

    char buf[4096];
    memcpy(buf, dir, n);
    buf[n] = '\0';

    for (usize i = 1; i <= n; i++) {
        if (i != n && buf[i] != '/') continue;
        bool last = (i == n);
        char saved = buf[i];
        buf[i] = '\0';

        struct stat st;
        if (lstat(buf, &st) == 0) {
            /* The LAST component is the session directory this call is about
             * to fill, so a symlink there is the redirect attack and is
             * refused outright. An ANCESTOR that is a symlink is followed:
             * /tmp is one on macOS and dotfile managers symlink ~ or
             * ~/.claude routinely, and refusing those would break the feature
             * on ordinary machines while protecting nothing — anyone able to
             * replace a component that high up already owns the account.
             * Following one still has to land on a directory. */
            if (last) {
                if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return false;
            } else {
                struct stat rst;
                if (stat(buf, &rst) != 0 || !S_ISDIR(rst.st_mode)) return false;
            }
        } else if (mkdir(buf, 0700) != 0 && errno != EEXIST) {
            return false;
        }
        buf[i] = saved;
    }
    return true;
}

void bridge_iso8601(char *out, usize cap, i64 ms) {
    if (!out || cap < 25) return;
    if (ms <= 0) ms = bridge_now_ms();
    time_t secs = (time_t)(ms / 1000);
    struct tm tmv;
    gmtime_r(&secs, &tmv);
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(ms % 1000));
}

/* Copy `text` into a fresh buffer with every occurrence of the closing form
 * of `tag` ("</tag>", any case) neutralised by escaping its '<' as "&lt;". A
 * target agent that wraps imported user text in its own inline tag reads the
 * first "</user_input>" inside that text as the END of the wrapper, and
 * whatever follows as fresh turns/instructions. Only the closing tag is
 * touched, so an ordinary message is byte-identical. Caller frees. */
char *bridge_escape_closing_tag(const char *text, const char *tag) {
    if (!text) return NULL;
    usize tl = tag ? strlen(tag) : 0;
    usize n = strlen(text);
    usize hits = 0;
    if (tl) {
        for (usize i = 0; i + tl + 2 < n + 1; i++) {
            if (text[i] == '<' && text[i + 1] == '/' &&
                strncasecmp(text + i + 2, tag, tl) == 0 && text[i + 2 + tl] == '>')
                hits++;
        }
    }
    char *out = (char *)malloc(n + hits * 3 + 1);
    if (!out) return NULL;
    usize o = 0;
    for (usize i = 0; i < n; i++) {
        if (tl && text[i] == '<' && i + 1 < n && text[i + 1] == '/' &&
            strncasecmp(text + i + 2, tag, tl) == 0 && text[i + 2 + tl] == '>') {
            memcpy(out + o, "&lt;", 4); o += 4;
            continue;
        }
        out[o++] = text[i];
    }
    out[o] = '\0';
    return out;
}
