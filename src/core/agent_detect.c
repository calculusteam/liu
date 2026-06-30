/*
 * Liu - CLI agent autodetect
 * Scans PATH for known coding-agent binaries.
 */
#include "core/agent_detect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct {
    const char *id;
    const char *display;
    const char *binary;
    /* NULL-terminated prelude args inserted between binary and prompt.
     * Codex needs --color/--skip-git-repo-check so it doesn't emit ANSI
     * escapes (which break our JSON brace-walker) or bail when launched
     * outside a git repo. */
    const char *args[AGENT_MAX_ARGS];
    bool        stdin_prompt;
} AgentSpec;

static const AgentSpec g_known_agents[] = {
    { "claude", "Claude Code", "claude",
      { "-p", NULL }, false },
    { "codex",  "Codex",       "codex",
      { "exec", "--color", "never", "--skip-git-repo-check", NULL }, false },
    { "cursor", "Cursor Agent","cursor-agent",
      { "-p", NULL }, false },
    { "amp",    "Amp",         "amp",
      { NULL }, false },
    { "cline",  "Cline",       "cline",
      { NULL }, true },
    { "opencode", "OpenCode",  "opencode",
      { "run", NULL }, false },
    { "grok",   "Grok",        "grok",
      { "-p", NULL }, false },
};

static const i32 g_known_agent_count =
    (i32)(sizeof(g_known_agents) / sizeof(g_known_agents[0]));

/* Build "<dir>/<binary>" with leading '~' expanded, then check execute. */
static bool probe_dir(const char *dir, usize dlen, const char *binary,
                       char *out_path, usize cap) {
    char buf[AGENT_PATH_CAP];
    usize off = 0;
    if (dlen >= 1 && dir[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) return false;
        usize hn = strlen(home);
        if (hn + dlen + strlen(binary) + 2 >= sizeof(buf)) return false;
        memcpy(buf + off, home, hn); off += hn;
        memcpy(buf + off, dir + 1, dlen - 1); off += dlen - 1;
    } else {
        if (dlen + strlen(binary) + 2 >= sizeof(buf)) return false;
        memcpy(buf + off, dir, dlen); off += dlen;
    }
    buf[off++] = '/';
    snprintf(buf + off, sizeof(buf) - off, "%s", binary);
    struct stat st;
    if (stat(buf, &st) != 0) return false;
    if ((st.st_mode & S_IFMT) != S_IFREG) return false;
    if (access(buf, X_OK) != 0) return false;
    snprintf(out_path, cap, "%s", buf);
    return true;
}

/* Resolve a binary by walking $PATH, then a curated set of common install
 * dirs that GUI launches (Finder, `open -a`) miss because launchd hands
 * apps a stripped PATH (`/usr/bin:/bin:...`) without `/opt/homebrew/bin`,
 * `~/.local/bin`, etc.  Without these fallbacks an installed `claude`
 * appears as "not installed" the moment Liu is launched as an .app. */
static bool find_in_path(const char *binary, char *out_path, usize cap) {
    const char *path = getenv("PATH");
    if (path && *path) {
        const char *p = path;
        while (*p) {
            const char *colon = strchr(p, ':');
            usize dlen = colon ? (usize)(colon - p) : strlen(p);
            if (dlen > 0 && probe_dir(p, dlen, binary, out_path, cap)) return true;
            if (!colon) break;
            p = colon + 1;
        }
    }
    static const char *const extras[] = {
        "/opt/homebrew/bin",
        "/opt/homebrew/sbin",
        "/usr/local/bin",
        "/usr/local/sbin",
        "~/.local/bin",
        "~/bin",
        "~/.cargo/bin",
        "~/.bun/bin",
        "~/.volta/bin",
        "~/go/bin",
        "~/.grok/bin",
    };
    for (usize i = 0; i < sizeof(extras) / sizeof(extras[0]); i++) {
        if (probe_dir(extras[i], strlen(extras[i]), binary, out_path, cap)) return true;
    }
    return false;
}

const char *agent_id_for_basename(const char *name) {
    if (!name || !*name) return "";
    for (i32 i = 0; i < g_known_agent_count; i++) {
        if (strcmp(name, g_known_agents[i].binary) == 0) return g_known_agents[i].id;
    }
    return "";
}

i32 agent_detect_available(AgentInfo *out, i32 cap) {
    if (!out || cap <= 0) return 0;

    i32 n = 0;
    for (i32 i = 0; i < g_known_agent_count && n < cap; i++) {
        const AgentSpec *s = &g_known_agents[i];
        char resolved[AGENT_PATH_CAP];
        if (!find_in_path(s->binary, resolved, sizeof(resolved))) continue;

        AgentInfo *a = &out[n++];
        snprintf(a->id,         sizeof(a->id),         "%s", s->id);
        snprintf(a->display,    sizeof(a->display),    "%s", s->display);
        snprintf(a->binary,     sizeof(a->binary),     "%s", s->binary);
        snprintf(a->path,       sizeof(a->path),       "%s", resolved);
        a->args_count = 0;
        for (i32 k = 0; k < AGENT_MAX_ARGS && s->args[k]; k++) {
            snprintf(a->args[k], sizeof(a->args[k]), "%s", s->args[k]);
            a->args_count++;
        }
        a->stdin_prompt = s->stdin_prompt;
    }
    return n;
}
