/*
 * Liu — how a filesystem path is written into a terminal.
 *
 * Three places produce the same bytes and have to keep agreeing:
 *
 *   - the file-drop handler, which types a dropped path at the prompt;
 *   - "Add Vault Source" / Cmd+Shift+A, which types an `@<path>` mention of a
 *     Notes-Vault document into the running agent;
 *   - translate_segment.c, which has to recognise that shape again in the
 *     input shadow so it can hold a path OUT of a translation.
 *
 * The rule is in one place for the reason syntax.c's table and img_link.h's
 * extension predicate are single: a rule implemented twice drifts, and this
 * one drifts silently — a path that loses its escaping does not error, it
 * arrives at the agent truncated at the first space and names a file that is
 * not there.
 *
 * The escape set is a SHELL's, not a filename's, because the receiver may be
 * either. A shell has to get one token; an agent CLI's mention parser has to
 * get one token too, and both read a backslash the same way.
 */
#ifndef CORE_PATH_ESCAPE_H
#define CORE_PATH_ESCAPE_H

#include "core/types.h"

/* Backslash-escape `path` into `out` and append ONE trailing space.
 *
 * The trailing space is part of the contract, not a convenience: consecutive
 * drops must not run together, and an agent's `@` mention ends at whitespace,
 * so without it the mention would swallow whatever the user types next.
 *
 * Bytes >= 0x80 pass through untouched — a UTF-8 path (Turkish file names, an
 * em dash in a note title) is not a shell metacharacter and escaping its bytes
 * individually would corrupt the sequence.
 *
 * Returns bytes written, excluding the NUL. Truncates rather than overruns:
 * the loop reserves room for a backslash, its byte, the space and the NUL.
 */
/* A path with a C0 control byte (or DEL) in it is refused outright rather
 * than escaped: a backslash in front of a raw CR does nothing for a line
 * editor — the terminal still delivers the CR and the shell SUBMITS whatever
 * was typed so far — and no real filesystem entry needs one. A hostile SFTP
 * listing is where such a name comes from. */
static inline bool liu_path_has_control_bytes(const char *path) {
    for (const unsigned char *p = (const unsigned char *)path; p && *p; p++)
        if (*p < 0x20 || *p == 0x7f) return true;
    return false;
}

static inline usize liu_path_escape(char *out, usize cap, const char *path) {
    if (!out || cap == 0) return 0;
    /* Refused HERE, not at each caller, so the file drop, the Vault mention and
     * the agent branch cannot disagree: a dropped name with ESC[201~ and a CR
     * would end the bracketed paste and submit attacker text. Nothing is typed for
     * such a name. */
    if (liu_path_has_control_bytes(path)) { out[0] = '\0'; return 0; }
    usize j = 0;
    for (const char *p = path; p && *p && j + 2 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' '  || c == '\t' || c == '\n' || c == '\\' || c == '"'  ||
            c == '\'' || c == '('  || c == ')'  || c == '['  || c == ']'  ||
            c == '{'  || c == '}'  || c == '<'  || c == '>'  || c == '|'  ||
            c == '&'  || c == ';'  || c == '$'  || c == '`'  || c == '!'  ||
            c == '*'  || c == '?') {
            out[j++] = '\\';
        }
        out[j++] = (char)c;
    }
    if (j + 1 < cap) out[j++] = ' ';   /* trailing space, see above */
    out[j] = '\0';
    return j;
}

/* The exact bytes "Add Vault Source" types into the agent: the `@` mention
 * sigil when that agent reads one, the escaped absolute path, and the trailing
 * space liu_path_escape appends.
 *
 * `with_sigil` is per agent — ui/agent_mention.h holds the list. Claude Code,
 * OpenCode, Cline and Command Code attach the file; Codex and Kimi have no
 * mention syntax, and for those the '@' would be a stray character in front of
 * an otherwise perfectly good path. Either way the path itself must arrive
 * intact, which is why the escaping is not optional: the default vault sits
 * under ".../Application Support/..." and that space would end the token.
 *
 * Returns bytes written, excluding the NUL; 0 when there is nothing to say. */
static inline usize liu_agent_file_mention(char *out, usize cap,
                                           const char *abspath,
                                           bool with_sigil) {
    if (!out || cap < 2 || !abspath || !abspath[0]) {
        if (out && cap) out[0] = '\0';
        return 0;
    }
    if (!with_sigil) return liu_path_escape(out, cap, abspath);
    out[0] = '@';
    usize n = liu_path_escape(out + 1, cap - 1, abspath);
    if (n == 0) { out[0] = '\0'; return 0; }   /* a refused path leaves no bare '@' */
    return 1 + n;
}

#endif /* CORE_PATH_ESCAPE_H */
