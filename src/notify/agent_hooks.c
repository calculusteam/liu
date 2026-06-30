/*
 * Liu - multi-agent notify-hook installers (see agent_hooks.h for contract).
 *
 * grok/opencode own their whole config artifact (a dedicated file in the
 * agent's hooks/plugins dir), so install = atomic write, uninstall = unlink.
 * codex shares ~/.codex/config.toml with the user's settings, so its block is
 * fenced between sentinel comments and spliced in/out as text — never a TOML
 * parse, so the user's `notify`/`model`/project trust lines stay intact.
 */
#include "notify/agent_hooks.h"

#include <errno.h>
#include <fcntl.h>
#ifndef PLATFORM_WIN32
#include <pwd.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ----------------------------------------------------------------- *
 *  shared helpers                                                    *
 * ----------------------------------------------------------------- */

static const char *home_dir(void) {
    const char *home = getenv("HOME");
#ifndef PLATFORM_WIN32
    if (!home || !*home) {
        struct passwd *pw = getpwuid(geteuid());
        if (pw && pw->pw_dir) home = pw->pw_dir;
    }
#endif
    return (home && *home) ? home : NULL;
}

/* mkdir -p for the parent directory of `path`. */
static bool ensure_parent_dir(const char *path) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf) return true;
    *slash = '\0';
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

/* Atomic text write via tmp + rename. */
static bool atomic_write(const char *path, const char *text) {
    if (!ensure_parent_dir(path)) return false;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    size_t len = strlen(text);
    ssize_t w = write(fd, text, len);
    fsync(fd);
    close(fd);
    if (w < 0 || (size_t)w != len) { unlink(tmp); return false; }
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

/* Read a whole file into a malloc'd NUL-terminated buffer, or NULL. */
static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0 || sz > 8 * 1024 * 1024) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Read a config file we splice into, distinguishing "genuinely absent" from
 * "present but unusable". Mirrors claude_hooks.c load_or_empty so a transiently
 * unreadable or oversized config.toml is never silently overwritten:
 *   - returns NULL and sets *absent = true  → no file (ENOENT/empty): ok to create;
 *   - returns NULL and sets *absent = false → present but unreadable/oversized:
 *     caller MUST abort without writing;
 *   - returns the malloc'd contents on success (*absent = false). */
static char *read_text_distinguish(const char *path, bool *absent) {
    *absent = false;
    FILE *f = fopen(path, "rb");
    if (!f) { *absent = (errno == ENOENT); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz == 0) { fclose(f); *absent = true; return NULL; }   /* empty: start fresh */
    if (sz < 0 || sz > 8 * 1024 * 1024) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    buf[got] = '\0';
    return buf;
}

/* Escape a string for embedding inside a double-quoted JSON / JS / TOML
 * value: backslash and double-quote only (paths and CLI flags carry no
 * control bytes). */
static void escape_dq(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && o + 2 < cap; p++) {
        if (*p == '\\' || *p == '"') out[o++] = '\\';
        out[o++] = *p;
    }
    out[o] = '\0';
}

static AgentHookResult result(bool ok, const char *fmt, ...) {
    AgentHookResult r = { .ok = ok, .msg = {0} };
    va_list ap; va_start(ap, fmt);
    vsnprintf(r.msg, sizeof r.msg, fmt, ap);
    va_end(ap);
    return r;
}

/* ----------------------------------------------------------------- *
 *  grok — ~/.grok/hooks/liu-notify.json (our own file)              *
 * ----------------------------------------------------------------- */

static bool grok_path(char *out, size_t cap) {
    const char *home = home_dir();
    if (!home) return false;
    return (size_t)snprintf(out, cap, "%s/.grok/hooks/liu-notify.json", home) < cap;
}

static AgentHookResult grok_install(const char *bin) {
    char path[1024];
    if (!grok_path(path, sizeof path)) return result(false, "grok: path too long");
    char b[1024];
    escape_dq(bin, b, sizeof b);
    /* Stop → top-level turn complete; Notification → attention. No
     * SubagentStop (subagent completions must stay silent); --hook makes
     * liu-notify read Grok's stdin JSON and drop any SubagentStop that
     * still reaches us. Global ~/.grok/hooks JSON files are "always
     * trusted", so this works the moment the file lands. */
    char json[2048];
    int n = snprintf(json, sizeof json,
        "{\n"
        "  \"hooks\": {\n"
        "    \"Stop\": [\n"
        "      { \"hooks\": [ { \"type\": \"command\",\n"
        "                       \"command\": \"%s send --tool=grok --event=complete --hook\" } ] }\n"
        "    ],\n"
        "    \"Notification\": [\n"
        "      { \"hooks\": [ { \"type\": \"command\",\n"
        "                       \"command\": \"%s send --tool=grok --event=notify --hook\" } ] }\n"
        "    ]\n"
        "  }\n"
        "}\n", b, b);
    /* Never write truncated (invalid) JSON. */
    if (n < 0 || (size_t)n >= sizeof json) return result(false, "grok: hook path too long");
    if (!atomic_write(path, json)) return result(false, "grok: write failed");
    return result(true, "Grok hook installed (%s)", path);
}

static AgentHookResult grok_uninstall(void) {
    char path[1024];
    if (!grok_path(path, sizeof path)) return result(false, "grok: path too long");
    if (file_exists(path) && unlink(path) != 0 && errno != ENOENT)
        return result(false, "grok: remove failed");
    return result(true, "Grok hook removed");
}

static bool grok_is_installed(void) {
    char path[1024];
    return grok_path(path, sizeof path) && file_exists(path);
}

/* ----------------------------------------------------------------- *
 *  opencode — ~/.config/opencode/plugins/liu-notify.js (own file)   *
 * ----------------------------------------------------------------- */

static bool opencode_path(char *out, size_t cap) {
    const char *home = home_dir();
    if (!home) return false;
    return (size_t)snprintf(out, cap,
        "%s/.config/opencode/plugins/liu-notify.js", home) < cap;
}

static AgentHookResult opencode_install(const char *bin) {
    char path[1024];
    if (!opencode_path(path, sizeof path)) return result(false, "opencode: path too long");
    char b[1024];
    escape_dq(bin, b, sizeof b);
    /* Plugins in ~/.config/opencode/plugins/ auto-load — no config.json edit,
     * so the user's existing `plugin` array is untouched. session.idle fires
     * per session INCLUDING subagent/Task child sessions (they carry a non-null
     * parentID), so we fetch the session and notify ONLY when it is the
     * top-level one (no parentID) — otherwise a finished subagent would ding. */
    char js[2048];
    int n = snprintf(js, sizeof js,
        "// liu-notify plugin (managed by Liu; do not edit).\n"
        "// Notifies on TOP-LEVEL session completion only; subagent/Task child\n"
        "// sessions (non-null parentID) are suppressed.\n"
        "import { spawn } from \"child_process\";\n"
        "const BIN = \"%s\";\n"
        "export default async ({ client }) => ({\n"
        "  event: async ({ event }) => {\n"
        "    if (!event || event.type !== \"session.idle\") return;\n"
        "    try {\n"
        "      const id = event.properties && event.properties.sessionID;\n"
        "      if (id && client && client.session && client.session.get) {\n"
        "        const r = await client.session.get({ path: { id } });\n"
        "        const s = r && (r.data || r);\n"
        "        if (s && s.parentID) return;   /* subagent/child -> silent */\n"
        "      }\n"
        "    } catch (e) { /* lookup failed: fall through and notify */ }\n"
        "    try {\n"
        "      spawn(BIN, [\"send\", \"--tool=opencode\", \"--event=complete\"],\n"
        "            { stdio: \"ignore\", detached: true }).unref();\n"
        "    } catch (e) {}\n"
        "  },\n"
        "});\n", b);
    /* Never write a truncated (broken) plugin file. */
    if (n < 0 || (size_t)n >= sizeof js) return result(false, "opencode: plugin path too long");
    if (!atomic_write(path, js)) return result(false, "opencode: write failed");
    return result(true, "OpenCode plugin installed (%s)", path);
}

static AgentHookResult opencode_uninstall(void) {
    char path[1024];
    if (!opencode_path(path, sizeof path)) return result(false, "opencode: path too long");
    if (file_exists(path) && unlink(path) != 0 && errno != ENOENT)
        return result(false, "opencode: remove failed");
    return result(true, "OpenCode plugin removed");
}

static bool opencode_is_installed(void) {
    char path[1024];
    return opencode_path(path, sizeof path) && file_exists(path);
}

/* ----------------------------------------------------------------- *
 *  codex — fenced [[hooks.Stop]] block in ~/.codex/config.toml      *
 * ----------------------------------------------------------------- */

#define CODEX_MARK_START "# >>> liu-notify (managed by Liu; do not edit) >>>"
#define CODEX_MARK_END   "# <<< liu-notify <<<"

static bool codex_path(char *out, size_t cap) {
    const char *home = home_dir();
    if (!home) return false;
    return (size_t)snprintf(out, cap, "%s/.codex/config.toml", home) < cap;
}

/* Return a freshly-malloc'd copy of `text` with the sentinel block delimited by
 * (mark_start, mark_end) — and one preceding blank line, if any — removed.
 * Returns NULL on OOM. If no block is present the input is copied unchanged. */
static char *codex_strip_between(const char *text, const char *mark_start,
                                 const char *mark_end) {
    const char *s = strstr(text, mark_start);
    if (!s) { char *c = malloc(strlen(text) + 1); if (c) strcpy(c, text); return c; }
    const char *e = strstr(s, mark_end);
    const char *block_end = e ? e + strlen(mark_end) : (text + strlen(text));
    if (*block_end == '\n') block_end++;           /* drop block's trailing NL */
    /* Trim one blank line we inserted before the block. */
    const char *cut = s;
    if (cut > text && cut[-1] == '\n') {
        cut--;
        if (cut > text && cut[-1] == '\n') cut--;
    }
    size_t head = (size_t)(cut - text);
    size_t tail = strlen(block_end);
    char *out = malloc(head + tail + 2);   /* +2: room for a normalized NL */
    if (!out) return NULL;
    memcpy(out, text, head);
    memcpy(out + head, block_end, tail);
    size_t len = head + tail;
    out[len] = '\0';
    /* Normalize the trailing newline the splice may have eaten: a non-empty
     * remainder ends in exactly one '\n'. */
    while (len > 0 && out[len - 1] == '\n') out[--len] = '\0';
    if (len > 0) { out[len] = '\n'; out[len + 1] = '\0'; }
    return out;
}

static char *codex_strip(const char *text) {
    return codex_strip_between(text, CODEX_MARK_START, CODEX_MARK_END);
}

static AgentHookResult codex_install(const char *bin) {
    char path[1024];
    if (!codex_path(path, sizeof path)) return result(false, "codex: path too long");
    char b[1024];
    escape_dq(bin, b, sizeof b);

    /* Distinguish "no config" (ok to create) from "present but unreadable/
     * oversized" — never overwrite a config we couldn't read, or the user's
     * notify/model/trust lines vanish. */
    bool absent;
    char *existing = read_text_distinguish(path, &absent);
    if (!existing && !absent)
        return result(false, "codex: %s exists but is unreadable (config left untouched)", path);
    char *base = existing ? codex_strip(existing) : NULL;  /* idempotent */
    free(existing);
    const char *prefix = base ? base : "";

    /* Codex Stop hook = a conversation turn finished. --hook lets liu-notify
     * read Codex's stdin JSON and drop any SubagentStop.
     *
     * The whole file = existing config + our block, so the buffer MUST be
     * sized to the prefix (a real config.toml can be many KB — a fixed
     * buffer would let snprintf silently TRUNCATE the user's settings). */
    const char *sep = (prefix[0] && prefix[strlen(prefix) - 1] != '\n')
                      ? "\n\n" : "\n";
    size_t cap = strlen(prefix) + strlen(sep) + strlen(b) + 512;
    char *block = (char *)malloc(cap);
    if (!block) { free(base); return result(false, "codex: out of memory"); }
    int wrote = snprintf(block, cap,
        "%s%s"
        CODEX_MARK_START "\n"
        "[[hooks.Stop]]\n"
        "[[hooks.Stop.hooks]]\n"
        "type = \"command\"\n"
        "command = \"%s send --tool=codex --event=complete --hook\"\n"
        CODEX_MARK_END "\n",
        prefix, sep, b);
    /* Never write a truncated config — that would corrupt the user's file. */
    if (wrote < 0 || (size_t)wrote >= cap) {
        free(block); free(base);
        return result(false, "codex: render overflow (config left untouched)");
    }
    bool ok = atomic_write(path, block);
    free(block);
    free(base);
    if (!ok) return result(false, "codex: write failed");
    /* Codex requires hook trust before a freshly-added command runs. */
    return result(true, "Codex hook installed — run /hooks in Codex once to trust it");
}

static AgentHookResult codex_uninstall(void) {
    char path[1024];
    if (!codex_path(path, sizeof path)) return result(false, "codex: path too long");
    char *existing = read_text(path);
    if (!existing) return result(true, "Codex: nothing to remove");
    if (!strstr(existing, CODEX_MARK_START)) { free(existing); return result(true, "Codex: no Liu hook"); }
    char *stripped = codex_strip(existing);
    free(existing);
    if (!stripped) return result(false, "codex: out of memory");
    bool ok = atomic_write(path, stripped);
    free(stripped);
    return ok ? result(true, "Codex hook removed") : result(false, "codex: write failed");
}

static bool codex_is_installed(void) {
    char path[1024];
    if (!codex_path(path, sizeof path)) return false;
    char *t = read_text(path);
    if (!t) return false;
    bool found = strstr(t, CODEX_MARK_START) != NULL;
    free(t);
    return found;
}

/* ----------------------------------------------------------------- *
 *  dispatch                                                          *
 * ----------------------------------------------------------------- */

bool agent_hook_supported(const char *id) {
    if (!id) return false;
    return !strcmp(id, "claude") || !strcmp(id, "grok") ||
           !strcmp(id, "codex")  || !strcmp(id, "opencode");
}

AgentHookResult agent_hook_install(const char *id, const char *bin) {
    if (!id || !bin || !*bin) return result(false, "missing agent id / binary");
    if (!strcmp(id, "claude"))   return claude_hooks_install(bin);
    if (!strcmp(id, "grok"))     return grok_install(bin);
    if (!strcmp(id, "opencode")) return opencode_install(bin);
    if (!strcmp(id, "codex"))    return codex_install(bin);
    return result(false, "%s: no hook integration", id);
}

AgentHookResult agent_hook_uninstall(const char *id) {
    if (!id) return result(false, "missing agent id");
    if (!strcmp(id, "claude"))   return claude_hooks_uninstall();
    if (!strcmp(id, "grok"))     return grok_uninstall();
    if (!strcmp(id, "opencode")) return opencode_uninstall();
    if (!strcmp(id, "codex"))    return codex_uninstall();
    return result(false, "%s: no hook integration", id);
}

bool agent_hook_installed(const char *id) {
    if (!id) return false;
    if (!strcmp(id, "claude"))   return claude_hooks_installed();
    if (!strcmp(id, "grok"))     return grok_is_installed();
    if (!strcmp(id, "opencode")) return opencode_is_installed();
    if (!strcmp(id, "codex"))    return codex_is_installed();
    return false;
}
