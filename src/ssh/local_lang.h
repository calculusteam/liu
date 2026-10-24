/*
 * Select the UTF-8 locale assigned to programs started in a pane. An explicit
 * system request maps the system locale to a .UTF-8 locale when that locale is
 * available; an explicit valid UTF-8 locale is used when loadable. If neither
 * request applies, preserve an inherited locale when present. Otherwise choose
 * C.UTF-8 when available, falling back to en_US.UTF-8.
 *
 * This policy keeps locale selection explicit while ensuring the terminal's
 * pane processes receive a UTF-8 locale when Liu supplies one. A shell's own
 * startup configuration can still set LANG after launch.
 */
#ifndef SSH_LOCAL_LANG_H
#define SSH_LOCAL_LANG_H

#include "core/types.h"

#include <stdio.h>
#include <string.h>

#define LOCAL_LANG_CAP 64

/* Can this machine load the locale called `name`? */
typedef bool (*LocalLangExists)(const char *name);

/* A locale's name as a config file may carry it: letters, digits and
 * `_ . @ -`, at most LOCAL_LANG_CAP - 1 bytes. It becomes an environment
 * value, so nothing else gets through. */
static inline bool local_lang_name_ok(const char *s) {
    if (!s || !s[0]) return false;
    usize n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '.' || c == '@' || c == '-';
        if (!ok || n + 1 >= LOCAL_LANG_CAP) return false;
    }
    return true;
}

/* Does the name say UTF-8 (`.UTF-8`, `.utf8`, in any case, before an `@`)? */
static inline bool local_lang_is_utf8(const char *s) {
    const char *dot = s ? strchr(s, '.') : NULL;
    if (!dot) return false;
    char cs[16];
    usize n = 0;
    for (const char *p = dot + 1; *p && *p != '@' && n + 1 < sizeof cs; p++) {
        char c = *p;
        if (c == '-') continue;
        cs[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    cs[n] = '\0';
    return strcmp(cs, "utf8") == 0;
}

/* What LANG becomes for a pane's programs, written into `buf`; NULL = leave
 * the environment as it is.
 *   asked           the config's "lang": "" (Liu decides), "system", or a name
 *   system_locale   the system's own as language_COUNTRY ("tr_TR"), or ""
 *   env_names_one   Liu's own environment already names a locale */
static inline const char *local_lang_pick(const char *asked, const char *system_locale,
                                          bool env_names_one, LocalLangExists exists,
                                          char *buf, usize cap) {
    if (!buf || cap < LOCAL_LANG_CAP || !exists) return NULL;
    if (asked && asked[0]) {
        if (strcmp(asked, "system") == 0) {
            if (local_lang_name_ok(system_locale) && !strchr(system_locale, '.')) {
                int n = snprintf(buf, cap, "%s.UTF-8", system_locale);
                if (n > 0 && (usize)n < cap && exists(buf)) return buf;
            }
        } else if (local_lang_name_ok(asked) && local_lang_is_utf8(asked) && exists(asked)) {
            snprintf(buf, cap, "%s", asked);
            return buf;
        }
    }
    if (env_names_one) return NULL;
    snprintf(buf, cap, "%s", exists("C.UTF-8") ? "C.UTF-8" : "en_US.UTF-8");
    return buf;
}

#endif /* SSH_LOCAL_LANG_H */
