#include "intro/intro_policy.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MARKER_NAME "intro_state"
#define MARKER_KEY  "intro_seen="

bool intro_policy_forced(const IntroPolicyInput *in) {
    return in && in->force && in->force[0];
}

bool intro_policy_decide(const IntroPolicyInput *in) {
    if (!in) return false;
    /* LIU_INTRO overrides everything, for harnesses and demos. */
    if (intro_policy_forced(in)) return strcmp(in->force, "1") == 0;
    if (in->harness) return false;
    return in->fresh_install && in->seen_version < INTRO_POLICY_VERSION;
}

bool intro_policy_dir_is_fresh(const char *config_dir) {
    if (!config_dir || !config_dir[0]) return false;
    struct stat st;
    /* Anything already at that path — a directory, or a file in its place —
     * means this is not a first run. Only "nothing there" is fresh; an error
     * other than ENOENT (no permission to look) is not evidence of a first
     * run either. */
    if (lstat(config_dir, &st) == 0) return false;
    return errno == ENOENT;
}

static bool marker_path(const char *config_dir, char *out, usize cap) {
    if (!config_dir || !config_dir[0]) return false;
    int n = snprintf(out, cap, "%s/%s", config_dir, MARKER_NAME);
    return n > 0 && (usize)n < cap;
}

i32 intro_policy_read_seen(const char *config_dir) {
    char path[1024];
    if (!marker_path(config_dir, path, sizeof path)) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[64];
    i32 version = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, MARKER_KEY, sizeof MARKER_KEY - 1) == 0) {
            long v = strtol(line + sizeof MARKER_KEY - 1, NULL, 10);
            if (v > 0 && v < 1000000) version = (i32)v;
        }
    }
    fclose(f);
    return version;
}

/* mkdir -p, each component, ignoring EEXIST. */
static bool make_dirs(const char *dir) {
    char tmp[1024];
    int n = snprintf(tmp, sizeof tmp, "%s", dir);
    if (n <= 0 || (usize)n >= sizeof tmp) return false;
    for (int i = 1; tmp[i]; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
        tmp[i] = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

bool intro_policy_write_seen(const char *config_dir) {
    char path[1024];
    if (!marker_path(config_dir, path, sizeof path)) return false;
    if (!make_dirs(config_dir)) return false;
    FILE *f = fopen(path, "w");
    if (!f) return false;
    bool ok = fprintf(f, "%s%d\n", MARKER_KEY, INTRO_POLICY_VERSION) > 0;
    return fclose(f) == 0 && ok;
}

static bool name_is(const char *entry, usize len, const char *name) {
    return strlen(name) == len && strncmp(entry, name, len) == 0;
}

bool intro_policy_env_is_harness(char *const *env) {
    if (!env) return false;
    for (; *env; env++) {
        const char *entry = *env;
        if (strncmp(entry, "LIU_", 4) != 0) continue;
        const char *eq = strchr(entry, '=');
        usize len = eq ? (usize)(eq - entry) : strlen(entry);
        if (name_is(entry, len, "LIU_VAULT") || name_is(entry, len, "LIU_VAULT_HINT")) continue;
        if (len >= 9 && strncmp(entry, "LIU_INTRO", 9) == 0) continue;
        /* A child Liu process inherits LIU_PANE_ID from its parent pane.
         * Ignore this variable when deciding whether other LIU_* environment
         * variables indicate a development launch. */
        if (name_is(entry, len, "LIU_PANE_ID")) continue;
        return true;
    }
    return false;
}
