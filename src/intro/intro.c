#include "intro/intro.h"
#include "intro/intro_policy.h"
#include "core/config.h"

#include <stdlib.h>

/* The intro draws on a Metal layer of its own, so it exists only where the
 * build links Metal. Everywhere else the policy answers "no" and the rest of
 * the seam is empty. */
#if defined(__APPLE__) && defined(USE_METAL)
#define LIU_INTRO_AVAILABLE 1
#else
#define LIU_INTRO_AVAILABLE 0
#endif

extern char **environ;

static bool g_sampled;
static bool g_wanted;
static bool g_forced;

void liu_intro_sample_first_run(void) {
    if (g_sampled) return;
    g_sampled = true;
#if LIU_INTRO_AVAILABLE
    const char *dir = config_user_dir();
    IntroPolicyInput in = {
        .force = liu_dev_env("LIU_INTRO"),
        .fresh_install = intro_policy_dir_is_fresh(dir),
        .seen_version = intro_policy_read_seen(dir),
        .harness = intro_policy_env_is_harness(environ),
    };
    g_forced = intro_policy_forced(&in);
    g_wanted = intro_policy_decide(&in);
#endif
}

bool liu_intro_wanted(void) { return g_wanted; }

void liu_intro_mark_seen(void) {
    /* A forced run is a demo: it must not change what a later real launch
     * decides. */
    if (g_forced) return;
    (void)intro_policy_write_seen(config_user_dir());
}

#if !LIU_INTRO_AVAILABLE
void liu_intro_begin(PlatformWindow *w) { (void)w; }
bool liu_intro_active(void) { return false; }
bool liu_intro_tour_clock(f64 *seconds) { (void)seconds; return false; }
bool liu_intro_leaving(void) { return false; }
void liu_intro_end(void) {}
void liu_intro_cue(i32 cue, i32 variant) { (void)cue; (void)variant; }
#endif
