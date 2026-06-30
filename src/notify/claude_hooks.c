/*
 * Claude Code hook installer for liu-notify (see header for contract).
 *
 * The on-disk schema we care about (a subset of Claude Code's settings):
 *
 *   {
 *     "hooks": {
 *       "Notification": [
 *         { "matcher": "", "hooks": [ { "type": "command",
 *                                       "command": "<liu-notify ...>" } ] }
 *       ],
 *       "Stop": [ ... same shape ... ]
 *     }
 *   }
 *
 * Each "block" (the inner array element) can carry multiple inner hooks; we
 * append our own block instead of mutating an existing one so the user's
 * matcher rules stay intact. Liu's blocks are identified by the substring
 * "liu-notify" inside any of their inner commands.
 */
#include "notify/claude_hooks.h"

#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#ifndef PLATFORM_WIN32
#include <pwd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *kEventNames[2] = { "Notification", "Stop" };
static const char *kEventArgs[2]  = { "notify",       "complete" };

/* Hook events Liu must NOT keep registered: SubagentStop fires every time a
 * Task-tool subagent finishes, which is exactly the per-subagent notification
 * spam we want gone — only the top-level Stop should notify. We never add it;
 * we also strip any stale liu-tagged SubagentStop block left by an older build
 * or a manual edit on every install/uninstall. */
static const char *kLegacyEventNames[1] = { "SubagentStop" };

static bool resolve_settings_path(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(geteuid());
        if (!pw || !pw->pw_dir) return false;
        home = pw->pw_dir;
    }
    int n = snprintf(out, cap, "%s/.claude/settings.json", home);
    return n > 0 && (size_t)n < cap;
}

static bool ensure_parent_dir(const char *path) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf) return true;
    *slash = '\0';
    /* Skip leading "/" then mkdir -p. */
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

/* Load the JSON config at `path`. Returns:
 *   - a fresh empty object when the file genuinely does not exist (ENOENT) or
 *     is empty — safe to populate;
 *   - the parsed object on success;
 *   - NULL when the file is present but cannot be read or parsed (permission
 *     error, oversized, short read, malformed JSON). Callers MUST treat NULL as
 *     "abort" and never write — otherwise a transiently-unreadable or malformed
 *     settings file gets silently reset to {}, wiping the user's settings. */
static cJSON *load_or_empty(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return (errno == ENOENT) ? cJSON_CreateObject() : NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz == 0) { fclose(f); return cJSON_CreateObject(); } /* empty file: start fresh */
    if (sz < 0 || sz > 4 * 1024 * 1024) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    buf[got] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool atomic_write_json(const char *path, cJSON *root) {
    if (!ensure_parent_dir(path)) return false;
    char *txt = cJSON_Print(root);
    if (!txt) return false;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { free(txt); return false; }
    size_t len = strlen(txt);
    ssize_t w = write(fd, txt, len);
    fsync(fd);
    close(fd);
    free(txt);
    if (w < 0 || (size_t)w != len) { unlink(tmp); return false; }
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

/* Get-or-create cJSON object at root.hooks; returns a borrowed pointer
 * owned by `root`. */
static cJSON *get_or_make_hooks(cJSON *root) {
    cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
    if (!hooks || !cJSON_IsObject(hooks)) {
        if (hooks) cJSON_DeleteItemFromObject(root, "hooks");
        hooks = cJSON_AddObjectToObject(root, "hooks");
    }
    return hooks;
}

static cJSON *get_or_make_event_array(cJSON *hooks, const char *evt) {
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(hooks, evt);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_DeleteItemFromObject(hooks, evt);
        arr = cJSON_AddArrayToObject(hooks, evt);
    }
    return arr;
}

/* True if `block` is a Liu-installed entry (has at least one inner hook
 * whose `command` contains "liu-notify"). */
static bool block_is_liu(const cJSON *block) {
    if (!cJSON_IsObject(block)) return false;
    const cJSON *inner = cJSON_GetObjectItemCaseSensitive(block, "hooks");
    if (!cJSON_IsArray(inner)) return false;
    const cJSON *h;
    cJSON_ArrayForEach(h, inner) {
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(h, "command");
        if (cJSON_IsString(cmd) && cmd->valuestring &&
            strstr(cmd->valuestring, "liu-notify")) return true;
    }
    return false;
}

/* Remove every Liu-tagged block from the given event array. Modifies
 * the array in-place. */
static void strip_liu_blocks(cJSON *arr) {
    if (!cJSON_IsArray(arr)) return;
    int i = 0;
    cJSON *block = NULL;
    while ((block = cJSON_GetArrayItem(arr, i)) != NULL) {
        if (block_is_liu(block)) cJSON_DeleteItemFromArray(arr, i);
        else i++;
    }
}

/* Drop any stale liu-tagged blocks under events Liu no longer registers
 * (SubagentStop), removing the event key entirely once empty. */
static void strip_legacy_liu_events(cJSON *hooks) {
    if (!cJSON_IsObject(hooks)) return;
    for (size_t i = 0; i < sizeof kLegacyEventNames / sizeof kLegacyEventNames[0]; i++) {
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(hooks, kLegacyEventNames[i]);
        if (!cJSON_IsArray(arr)) continue;
        strip_liu_blocks(arr);
        if (cJSON_GetArraySize(arr) == 0)
            cJSON_DeleteItemFromObject(hooks, kLegacyEventNames[i]);
    }
}

ClaudeHookResult claude_hooks_install(const char *notify_bin_path) {
    ClaudeHookResult res = { .ok = false, .msg = {0} };
    if (!notify_bin_path || !*notify_bin_path) {
        snprintf(res.msg, sizeof res.msg, "Missing liu-notify binary path.");
        return res;
    }

    char path[512];
    if (!resolve_settings_path(path, sizeof path)) {
        snprintf(res.msg, sizeof res.msg, "Could not resolve ~/.claude/settings.json.");
        return res;
    }

    cJSON *root  = load_or_empty(path);
    if (!root) {
        snprintf(res.msg, sizeof res.msg,
                 "Refusing to install: %s exists but is unreadable or malformed", path);
        return res;   /* never overwrite a present-but-unparsable settings file */
    }
    cJSON *hooks = get_or_make_hooks(root);

    for (int i = 0; i < 2; i++) {
        cJSON *arr = get_or_make_event_array(hooks, kEventNames[i]);
        strip_liu_blocks(arr);   /* drop stale entries from a prior install */

        cJSON *block = cJSON_CreateObject();
        cJSON_AddStringToObject(block, "matcher", "");
        cJSON *inner = cJSON_AddArrayToObject(block, "hooks");
        cJSON *h = cJSON_CreateObject();
        cJSON_AddStringToObject(h, "type", "command");
        char cmd[1024];
        /* --hook tells liu-notify to read the event JSON on stdin and decide
         * from the real hook_event_name — so a subagent stop that reaches the
         * Stop hook is dropped and only the top-level turn notifies. */
        snprintf(cmd, sizeof cmd, "%s send --tool=claude --event=%s --hook",
                 notify_bin_path, kEventArgs[i]);
        cJSON_AddStringToObject(h, "command", cmd);
        cJSON_AddItemToArray(inner, h);
        cJSON_AddItemToArray(arr, block);
    }
    strip_legacy_liu_events(hooks);   /* purge any stale SubagentStop block */

    bool ok = atomic_write_json(path, root);
    cJSON_Delete(root);
    if (!ok) {
        snprintf(res.msg, sizeof res.msg, "Failed to write %s", path);
        return res;
    }
    res.ok = true;
    snprintf(res.msg, sizeof res.msg, "Installed hooks in %s", path);
    return res;
}

ClaudeHookResult claude_hooks_uninstall(void) {
    ClaudeHookResult res = { .ok = false, .msg = {0} };
    char path[512];
    if (!resolve_settings_path(path, sizeof path)) {
        snprintf(res.msg, sizeof res.msg, "Could not resolve ~/.claude/settings.json.");
        return res;
    }
    /* If the file doesn't exist, there's nothing to uninstall — treat
     * that as success so the UI doesn't show an error message. */
    struct stat st;
    if (stat(path, &st) != 0) {
        res.ok = true;
        snprintf(res.msg, sizeof res.msg, "No Claude settings file — nothing to remove.");
        return res;
    }

    cJSON *root  = load_or_empty(path);
    cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
    if (cJSON_IsObject(hooks)) {
        for (int i = 0; i < 2; i++) {
            cJSON *arr = cJSON_GetObjectItemCaseSensitive(hooks, kEventNames[i]);
            strip_liu_blocks(arr);
            /* Drop the event key entirely if no other entries remain so
             * we don't leave noise behind for a curious user. */
            if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 0)
                cJSON_DeleteItemFromObject(hooks, kEventNames[i]);
        }
        strip_legacy_liu_events(hooks);   /* also clear any SubagentStop block */
        if (cJSON_GetArraySize(hooks) == 0)
            cJSON_DeleteItemFromObject(root, "hooks");
    }

    bool ok = atomic_write_json(path, root);
    cJSON_Delete(root);
    if (!ok) {
        snprintf(res.msg, sizeof res.msg, "Failed to write %s", path);
        return res;
    }
    res.ok = true;
    snprintf(res.msg, sizeof res.msg, "Removed Liu hooks from %s", path);
    return res;
}

bool claude_hooks_installed(void) {
    char path[512];
    if (!resolve_settings_path(path, sizeof path)) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;

    cJSON *root  = load_or_empty(path);
    cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
    bool found = false;
    if (cJSON_IsObject(hooks)) {
        for (int i = 0; i < 2 && !found; i++) {
            cJSON *arr = cJSON_GetObjectItemCaseSensitive(hooks, kEventNames[i]);
            if (!cJSON_IsArray(arr)) continue;
            const cJSON *block;
            cJSON_ArrayForEach(block, arr) {
                if (block_is_liu(block)) { found = true; break; }
            }
        }
    }
    cJSON_Delete(root);
    return found;
}
