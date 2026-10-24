/*
 * Policy for URLs passed to the platform opener. Accept only http, https,
 * mailto, and file schemes. Open ordinary non-executable files and non-bundle
 * directories; reveal executable files, launchable bundles, and extensions the
 * OS runs on open. Refuse unsupported schemes and targets that cannot be
 * classified.
 */
#ifndef LIU_OPEN_URL_POLICY_H
#define LIU_OPEN_URL_POLICY_H

#include "core/types.h"
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <limits.h>   /* PATH_MAX */
#include <stdlib.h>   /* realpath */

/* Scheme of `url` (lower-cased into `out`), or false when it has none — a
 * bare filesystem path, "www.example.com" already normalised by the caller.
 * A scheme is RFC 3986's: ALPHA *(ALPHA / DIGIT / "+" / "-" / "."), ending at
 * the first ':'. Anything else before the colon is not a scheme. */
static inline bool liu_url_scheme(const char *url, char *out, usize cap) {
    if (!url || !out || cap == 0) return false;
    usize i = 0;
    for (; url[i] && url[i] != ':'; i++) {
        unsigned char c = (unsigned char)url[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (i > 0 && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'));
        if (!ok || i + 1 >= cap) return false;
        out[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    if (i == 0 || url[i] != ':') return false;
    /* A single letter before ':' on a path ("C:\") is a drive, not a scheme;
     * no scheme Liu opens is one character long. */
    if (i == 1) return false;
    out[i] = '\0';
    return true;
}

static inline bool liu_open_url_scheme_allowed(const char *scheme) {
    static const char *const allow[] = { "http", "https", "mailto", "file" };
    if (!scheme || !scheme[0]) return false;
    for (usize i = 0; i < sizeof allow / sizeof allow[0]; i++)
        if (strcasecmp(scheme, allow[i]) == 0) return true;
    return false;
}

/* Extensions the OS runs or installs on open regardless of the execute bit.
 * Plain scripts (.sh, .py) are NOT here: their default handler is an editor,
 * and one with the execute bit set is caught by the mode test below. */
static inline bool liu_open_path_runs_on_open(const char *path) {
    static const char *const run[] = {
        ".command", ".tool", ".terminal", ".app", ".action", ".workflow",
        ".desktop", ".jar", ".pkg", ".mpkg", ".prefpane", ".qlgenerator",
        ".saver", ".service", ".xpc", ".kext", ".appex", ".plugin",
        /* Location files: opening one opens what it POINTS at — any scheme,
         * any local file — which would walk round the allow-list above. And
         * the two that install something when opened. */
        ".inetloc", ".fileloc", ".webloc", ".url", ".mobileconfig", ".shortcut",
    };
    usize n = path ? strlen(path) : 0;
    while (n > 0 && path[n - 1] == '/') n--;
    for (usize i = 0; i < sizeof run / sizeof run[0]; i++) {
        usize el = strlen(run[i]);
        if (n >= el && strncasecmp(path + n - el, run[i], el) == 0) return true;
    }
    return false;
}

typedef enum {
    LIU_OPEN_PATH_REFUSE = 0,   /* does not exist / cannot be classified */
    LIU_OPEN_PATH_OPEN,         /* safe to hand to the default opener */
    LIU_OPEN_PATH_REVEAL,       /* show in the file manager; do NOT launch */
} LiuOpenPathVerdict;

/* `st` is the target's lstat/stat result; `is_bundle` is the platform's own
 * answer for "does the OS treat this directory as a launchable package" (an
 * .app on macOS), false where the concept does not exist. */
static inline LiuOpenPathVerdict liu_open_path_verdict(const char *path,
                                                       const struct stat *st,
                                                       bool is_bundle) {
    if (!path || !st) return LIU_OPEN_PATH_REFUSE;
    if (liu_open_path_runs_on_open(path)) return LIU_OPEN_PATH_REVEAL;
    if (S_ISDIR(st->st_mode)) return is_bundle ? LIU_OPEN_PATH_REVEAL : LIU_OPEN_PATH_OPEN;
    if (!S_ISREG(st->st_mode)) return LIU_OPEN_PATH_REFUSE;   /* device, fifo, socket */
    if (st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) return LIU_OPEN_PATH_REVEAL;
    return LIU_OPEN_PATH_OPEN;
}

/* Resolve a file target before applying the open/reveal policy. A symlink
 * may have a harmless name while pointing to a launchable file or bundle,
 * so the verdict and OS action must use the resolved target. Return false
 * if resolution fails. */
static inline bool liu_open_path_resolve(const char *path, char *out, usize cap,
                                         struct stat *st) {
    char real[PATH_MAX];
    if (!path || !out || cap == 0 || !st || !realpath(path, real)) return false;
    if (strlen(real) >= cap || stat(real, st) != 0) return false;
    memcpy(out, real, strlen(real) + 1);
    return true;
}

#endif /* LIU_OPEN_URL_POLICY_H */
