/*
 * Liu — turning a stored session id into the argument its agent's resume
 * command actually takes.
 *
 * For every agent but one those are the same string, which is why this header
 * has a single function in it. Codex is the exception, and the reason is a
 * genuine disagreement between two stores: liu records a Codex session as its
 * rollout FILENAME
 *
 *     rollout-2026-08-29T01-30-19-01a04a7e-ce29-7271-904a-5a3c9f2751b0
 *
 * because that is what names the file on disk and what the scanner can see
 * without opening it — while `codex resume` takes the bare thread UUID.
 *
 * Getting this wrong is not a visible error. `codex resume <arg>` matches a
 * session NAME as well as an id, and codex accepts an unknown `-c key=value`
 * silently, exit 0 and no warning, so a wrong argument can open a brand new
 * empty session that looks exactly like a successful resume. A resume that
 * cannot be built must therefore degrade to "start a fresh session", never to
 * a guessed argument.
 *
 * The extraction lives here, apart from the picker that consumes it, so it
 * can be checked without a UI.
 */
#ifndef UI_AGENT_RESUME_H
#define UI_AGENT_RESUME_H

#include "core/types.h"

#include <ctype.h>
#include <string.h>

/* Copy the trailing thread UUID of a Codex rollout id (or of a rollout file's
 * basename) into `out`, which must hold at least 37 bytes.
 *
 * Accepts only a canonical 8-4-4-4-12 hex tail that is either the whole string
 * or preceded by a separator, so a 36-character tail of some longer token can
 * never be handed to codex as an id. Returns false when there is no such tail;
 * the caller must then offer a fresh session rather than a guess. */
static inline bool codex_thread_uuid(const char *sid, char *out, usize cap) {
    if (!sid || !out || cap < 37) return false;
    usize n = strlen(sid);
    if (n < 36) return false;
    const char *u = sid + (n - 36);
    for (i32 i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (u[i] != '-') return false;
        } else if (!isxdigit((unsigned char)u[i])) {
            return false;
        }
    }
    if (n > 36 && sid[n - 37] != '-' && sid[n - 37] != '/') return false;
    memcpy(out, u, 36);
    out[36] = '\0';
    return true;
}

#endif
