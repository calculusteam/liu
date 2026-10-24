#ifndef LIU_INTRO_POLICY_H
#define LIU_INTRO_POLICY_H

/* Play the first-run intro when the Liu config directory was absent at startup,
 * unless LIU_INTRO overrides the decision or development hooks are active.
 * Write the marker when playback starts so an interrupted intro is not
 * repeated. */

#include "core/types.h"

/* Bump to show a reworked intro once more — to fresh installs only; see above. */
#define INTRO_POLICY_VERSION 1

typedef struct {
    /* LIU_INTRO, or NULL when unset. */
    const char *force;
    /* The config directory did not exist when the process started. */
    bool fresh_install;
    /* The marker's version, 0 when there is none. */
    i32 seen_version;
    /* A debug knob is set. */
    bool harness;
} IntroPolicyInput;

bool intro_policy_decide(const IntroPolicyInput *in);
/* True when LIU_INTRO forced the answer, either way. */
bool intro_policy_forced(const IntroPolicyInput *in);

bool intro_policy_dir_is_fresh(const char *config_dir);
i32  intro_policy_read_seen(const char *config_dir);
/* mkdir -p the directory and write the marker. False when it could not. */
bool intro_policy_write_seen(const char *config_dir);
/* `env` is a NULL-terminated "NAME=value" array (environ). */
bool intro_policy_env_is_harness(char *const *env);

#endif /* LIU_INTRO_POLICY_H */
