/*
 * hostkey_prompt — what a key press or a click does in a dialog that asks for
 * trust or for a secret (the SSH host-key dialog, the server's
 * keyboard-interactive prompt).
 *
 * A dialog is raised by the connection, not by the user: a link that drops is
 * retried by itself, and a retry that meets a different key raises "Host Key
 * Changed" under the hands of somebody typing a command. The Enter that ends
 * the command must not accept the key, retire every stored key for the host
 * and send the stored password to whoever answered.
 *
 * The rule (pure):
 *
 *   - for the first HOSTKEY_ARM_SEC after a dialog appears nothing accepts —
 *     no key, no click. What was already on its way to the terminal is not an
 *     answer to a question the user has not read yet;
 *   - a CHANGED key is never accepted from the keyboard. It takes a click on
 *     the button that says so. OpenSSH refuses a changed key outright;
 *   - Escape (and the Reject button) refuse at once: refusing is always safe.
 *
 * ssh/link_policy.h holds the other half: an automatic reconnect does not
 * raise the changed-key dialog at all.
 */
#ifndef UI_HOSTKEY_PROMPT_H
#define UI_HOSTKEY_PROMPT_H

#include "core/types.h"

/* How long a freshly raised trust dialog ignores every way of saying yes. */
#define HOSTKEY_ARM_SEC 1.0

typedef enum {
    HOSTKEY_ANSWER_NONE = 0,   /* the input is swallowed and nothing is decided */
    HOSTKEY_ANSWER_ACCEPT,
    HOSTKEY_ANSWER_REJECT,
} HostkeyAnswer;

static inline bool hostkey_prompt_armed(f64 shown_for) {
    return shown_for >= HOSTKEY_ARM_SEC;
}

/* A key press. `quiet_for`: seconds the keyboard has been quiet in front of
 * the dialog — since it appeared, or since the last key it swallowed. Counted
 * from the appearance alone, someone typing a password into the form under it
 * for longer than a second ended with an Enter that accepted the new key. */
static inline HostkeyAnswer hostkey_prompt_key(bool is_change, bool escape, bool enter, f64 quiet_for) {
    if (escape) return HOSTKEY_ANSWER_REJECT;
    if (enter && !is_change && hostkey_prompt_armed(quiet_for)) return HOSTKEY_ANSWER_ACCEPT;
    return HOSTKEY_ANSWER_NONE;
}

/* A click. `on_accept` / `on_reject`: which button it landed on, if any. */
static inline HostkeyAnswer hostkey_prompt_click(bool on_accept, bool on_reject, f64 shown_for) {
    if (on_reject) return HOSTKEY_ANSWER_REJECT;
    if (on_accept && hostkey_prompt_armed(shown_for)) return HOSTKEY_ANSWER_ACCEPT;
    return HOSTKEY_ANSWER_NONE;
}

/* A dialog that collects a SECRET for somebody else (the server's
 * keyboard-interactive prompt): Enter submits only once it is armed, for the
 * same reason — the Enter may have been meant for the pane underneath. */
static inline bool secret_prompt_submit_ok(f64 shown_for) {
    return hostkey_prompt_armed(shown_for);
}

#endif /* UI_HOSTKEY_PROMPT_H */
