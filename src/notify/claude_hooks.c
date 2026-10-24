/*
 * Claude-schema hook installer for liu-notify (see header for contract).
 *
 * Serves Claude Code and every CLI that reuses its settings format. Command
 * Code is a Claude Code fork and reads the identical shape out of
 * ~/.commandcode/settings.json — same "hooks" object, same matcher arrays,
 * same {"type":"command","command":…} handlers — so it shares this file
 * rather than getting a near-duplicate installer of its own. What differs is
 * per-agent and lives in a HookSchema: the settings path, the --tool= id, and
 * WHICH events that CLI actually fires (Command Code has no Notification or
 * UserPromptSubmit event, so it registers Stop alone).
 *
 * The on-disk schema this reads and writes (a subset of Claude Code's
 * settings):
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
 * Each "block" (the inner array element) can carry multiple inner hooks; Liu
 * appends its own block instead of mutating an existing one so the user's
 * matcher rules stay intact. A hook is Liu's by the command it RUNS — its
 * first shell word is a liu-notify (hook_cmd_is_liu, hook_scan.h) — and only
 * that hook is ever removed; a block goes when its last hook was Liu's.
 */
#include "notify/claude_hooks.h"
#include "notify/hook_paths.h"
#include "core/atomic_file.h"
#include "notify/agent_hooks.h"

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

#include "notify/hook_scan.h"   /* hook_cmd_is_liu: the one ownership rule */

/* UserPromptSubmit is not a notification — it is the only signal Claude gives
 * that a turn STARTED, and the GUI needs both edges to drive the tab's working
 * animation from the agent instead of guessing from output volume. liu-notify
 * routes it to the agent-state ring and never speaks or plays a sound for it
 * (see EVT_START in daemon.c). */
static const char *kEventNames[3] = { "Notification", "Stop",     "UserPromptSubmit" };
static const char *kEventArgs[3]  = { "notify",       "complete", "start" };
#define CLAUDE_HOOK_EVENT_COUNT ((int)(sizeof kEventNames / sizeof kEventNames[0]))

/* Command Code fires exactly four hook events — PreToolUse, PostToolUse, Stop
 * and SessionStart. Only Stop maps to anything Liu reports.
 *
 * SessionStart is deliberately NOT wired to --event=start: it fires when the
 * CLI boots, not when a turn begins, so latching the pane's hook-driven mode
 * on it would pin the working animation on for the whole session. With Stop
 * alone Command Code behaves like Codex and Grok — the accent bar animates
 * from output volume, and the agent's own signal is what closes the turn and
 * opens the done panel. */
static const char *kCmdCodeEventNames[1] = { "Stop"     };
static const char *kCmdCodeEventArgs[1]  = { "complete" };

/* Hook events Liu must NOT keep registered: SubagentStop fires every time a
 * Task-tool subagent finishes, and only the top-level Stop should notify. It
 * is never added, and every install/uninstall also strips any stale Liu-tagged
 * SubagentStop block left by an older build or a manual edit. */
static const char *kLegacyEventNames[1] = { "SubagentStop" };

/* Everything that differs between two CLIs sharing this settings format. */
typedef struct {
    const char  *rel_path;    /* settings file, relative to $HOME */
    const char  *tool_arg;    /* value passed to liu-notify --tool= */
    const char **event_names; /* CLI-side hook event names */
    const char **event_args;  /* matching liu-notify --event= values */
    int          event_count;
} HookSchema;

static const HookSchema kClaudeSchema = {
    "/" LIU_HOOK_CLAUDE_REL, "claude",
    kEventNames, kEventArgs, CLAUDE_HOOK_EVENT_COUNT
};

static const HookSchema kCommandCodeSchema = {
    "/" LIU_HOOK_COMMANDCODE_REL, "commandcode",
    kCmdCodeEventNames, kCmdCodeEventArgs, 1
};

static bool resolve_settings_path(char *out, size_t cap, const char *rel) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(geteuid());
        if (!pw || !pw->pw_dir) return false;
        home = pw->pw_dir;
    }
    int n = snprintf(out, cap, "%s%s", home, rel);
    return n > 0 && (size_t)n < cap;
}



/* Load the JSON config at `path`. Returns:
 *   - a fresh empty object when the file genuinely does not exist (ENOENT) or
 *     is empty — safe to populate;
 *   - the parsed object on success;
 *   - NULL when the file is present but cannot be read or parsed (permission
 *     error, oversized, short read, malformed JSON). Callers MUST treat NULL as
 *     "abort" and never write — otherwise a transiently-unreadable or malformed
 *     settings file gets silently reset to {}, wiping the user's settings.
 *
 * "Malformed" includes text after the JSON value: cJSON_Parse stops at the end
 * of the first value and calls that success, so a write-back would drop
 * everything behind it (a second object, half a merge conflict). The text has
 * to END where the value does.
 *
 * `*wide_int` (optional) reports an integer the write-back would re-spell —
 * see hook_json_has_wide_integer. A caller about to WRITE must refuse on it;
 * one that only reads passes NULL. */
static cJSON *load_or_empty(const char *path, bool *wide_int) {
    if (wide_int) *wide_int = false;
    FILE *f = fopen(path, "rb");
    if (!f) return (errno == ENOENT) ? cJSON_CreateObject() : NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz == 0) { fclose(f); return cJSON_CreateObject(); } /* empty file: start fresh */
    if (sz < 0 || sz > 4 * 1024 * 1024) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    buf[got] = '\0';
    cJSON *root = cJSON_ParseWithOpts(buf, NULL, true);
    if (root && wide_int) *wide_int = hook_json_has_wide_integer(buf);
    free(buf);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool atomic_write_json(const char *path, cJSON *root) {
    char *txt = cJSON_Print(root);
    if (!txt) return false;
    AtomicFileWriter writer;
    if (!atomic_file_begin_personal(&writer, path)) { free(txt); return false; }
    usize len = strlen(txt);
    bool ok = fwrite(txt, 1, len, writer.stream) == len;
    free(txt);
    if (!ok) { atomic_file_abort(&writer); return false; }
    return atomic_file_commit(&writer);
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

/* True if inner hook `h` is one Liu wrote: its command is a liu-notify command
 * line by the shared rule. */
static bool hook_is_liu(const cJSON *h) {
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(h, "command");
    return cJSON_IsString(cmd) && cmd->valuestring && hook_cmd_is_liu(cmd->valuestring);
}

/* True if `block` carries at least one Liu hook. */
static bool block_has_liu(const cJSON *block) {
    if (!cJSON_IsObject(block)) return false;
    const cJSON *inner = cJSON_GetObjectItemCaseSensitive(block, "hooks");
    if (!cJSON_IsArray(inner)) return false;
    const cJSON *h;
    cJSON_ArrayForEach(h, inner) {
        if (hook_is_liu(h)) return true;
    }
    return false;
}

/* Remove Liu's hooks from the given event array, in place; returns how many
 * went. Hook by hook, never block by block: the block is a MATCHER and the
 * user may have put a hook of their own under the same one (a `say done`
 * beside liu-notify), which must stay. A block is dropped only when taking
 * Liu's out left its `hooks` empty — one that was already empty is the
 * user's and stays. */
static int strip_liu_hooks(cJSON *arr) {
    if (!cJSON_IsArray(arr)) return 0;
    int removed = 0, i = 0;
    cJSON *block = NULL;
    while ((block = cJSON_GetArrayItem(arr, i)) != NULL) {
        cJSON *inner = cJSON_IsObject(block)
                     ? cJSON_GetObjectItemCaseSensitive(block, "hooks") : NULL;
        int mine = 0, j = 0;
        cJSON *h = NULL;
        while (cJSON_IsArray(inner) && (h = cJSON_GetArrayItem(inner, j)) != NULL) {
            if (hook_is_liu(h)) { cJSON_DeleteItemFromArray(inner, j); mine++; }
            else j++;
        }
        removed += mine;
        if (mine > 0 && cJSON_GetArraySize(inner) == 0) cJSON_DeleteItemFromArray(arr, i);
        else i++;
    }
    return removed;
}

/* Drop any stale Liu hooks under events Liu no longer registers
 * (SubagentStop), removing the event key once that emptied it. Returns how
 * many hooks went. */
static int strip_legacy_liu_events(cJSON *hooks) {
    if (!cJSON_IsObject(hooks)) return 0;
    int removed = 0;
    for (size_t i = 0; i < sizeof kLegacyEventNames / sizeof kLegacyEventNames[0]; i++) {
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(hooks, kLegacyEventNames[i]);
        int n = strip_liu_hooks(arr);
        removed += n;
        if (n > 0 && cJSON_GetArraySize(arr) == 0)
            cJSON_DeleteItemFromObjectCaseSensitive(hooks, kLegacyEventNames[i]);
    }
    return removed;
}

/* Why a settings file that parsed is still not rewritten. */
#define WIDE_INT_MSG "%s holds an integer too long to rewrite exactly — left untouched"

static ClaudeHookResult hooks_install_schema(const char *notify_bin_path,
                                             const HookSchema *sc) {
    ClaudeHookResult res = { .ok = false, .msg = {0} };
    if (!notify_bin_path || !*notify_bin_path) {
        snprintf(res.msg, sizeof res.msg, "Missing liu-notify binary path.");
        return res;
    }

    char path[512];
    if (!resolve_settings_path(path, sizeof path, sc->rel_path)) {
        snprintf(res.msg, sizeof res.msg, "Could not resolve ~%s.", sc->rel_path);
        return res;
    }

    bool wide_int = false;
    cJSON *root  = load_or_empty(path, &wide_int);
    if (!root) {
        snprintf(res.msg, sizeof res.msg,
                 "Refusing to install: %s exists but is unreadable or malformed", path);
        return res;   /* never overwrite a present-but-unparsable settings file */
    }
    /* A settings file reached through a link is written at the link's target
     * (agent_hook_write_path); a link that cannot be followed is not replaced.
     * `target` is no larger than `path` on purpose: the writer below sizes its
     * temp name for a path of that length. */
    char target[sizeof path];
    if (wide_int || !agent_hook_write_path(path, target, sizeof target, NULL)) {
        cJSON_Delete(root);
        if (wide_int) snprintf(res.msg, sizeof res.msg, "Refusing to install: " WIDE_INT_MSG, path);
        else snprintf(res.msg, sizeof res.msg, "Refusing to install: cannot follow the link at %s", path);
        return res;
    }
    cJSON *hooks = get_or_make_hooks(root);

    for (int i = 0; i < sc->event_count; i++) {
        cJSON *arr = get_or_make_event_array(hooks, sc->event_names[i]);
        strip_liu_hooks(arr);    /* drop stale entries from a prior install */

        cJSON *block = cJSON_CreateObject();
        cJSON_AddStringToObject(block, "matcher", "");
        cJSON *inner = cJSON_AddArrayToObject(block, "hooks");
        cJSON *h = cJSON_CreateObject();
        cJSON_AddStringToObject(h, "type", "command");
        char cmd[1024], qbin[1024];
        /* --hook tells liu-notify to read the event JSON on stdin and decide
         * from the real hook_event_name — so a subagent stop that reaches the
         * Stop hook is dropped and only the top-level turn notifies. The
         * binary path is shell-quoted: the agent runs this through sh. */
        int cmd_len = agent_hook_shell_quote(notify_bin_path, qbin, sizeof qbin)
                    ? snprintf(cmd, sizeof cmd, "%s send --tool=%s --event=%s --hook",
                               qbin, sc->tool_arg, sc->event_args[i])
                    : -1;
        if (cmd_len < 0 || (size_t)cmd_len >= sizeof cmd) {
            /* `h` is not attached to anything yet — cJSON_AddItemToArray(inner,
             * h) is below this branch — so deleting `block` does not reach it. */
            cJSON_Delete(h);
            cJSON_Delete(block);
            cJSON_Delete(root);
            snprintf(res.msg, sizeof res.msg, "Notify binary path is too long.");
            return res;
        }
        cJSON_AddStringToObject(h, "command", cmd);
        cJSON_AddItemToArray(inner, h);
        cJSON_AddItemToArray(arr, block);
    }
    strip_legacy_liu_events(hooks);   /* purge any stale SubagentStop block */

    bool ok = atomic_write_json(target, root);
    cJSON_Delete(root);
    if (!ok) {
        snprintf(res.msg, sizeof res.msg, "Failed to write %s", path);
        return res;
    }
    res.ok = true;
    snprintf(res.msg, sizeof res.msg, "Installed hooks in %s", path);
    return res;
}

static ClaudeHookResult hooks_uninstall_schema(const HookSchema *sc) {
    ClaudeHookResult res = { .ok = false, .msg = {0} };
    char path[512];
    if (!resolve_settings_path(path, sizeof path, sc->rel_path)) {
        snprintf(res.msg, sizeof res.msg, "Could not resolve ~%s.", sc->rel_path);
        return res;
    }
    /* If the file doesn't exist, there's nothing to uninstall — treat
     * that as success so the UI doesn't show an error message. */
    struct stat st;
    if (stat(path, &st) != 0) {
        res.ok = true;
        snprintf(res.msg, sizeof res.msg, "No settings file at ~%s — nothing to remove.",
                 sc->rel_path);
        return res;
    }

    bool wide_int = false;
    cJSON *root  = load_or_empty(path, &wide_int);
    if (!root) {
        snprintf(res.msg, sizeof res.msg,
                 "%s is unreadable or malformed — left untouched", path);
        return res;   /* whatever is in there, a rewrite would not keep it */
    }
    int removed = 0;
    cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
    if (cJSON_IsObject(hooks)) {
        for (int i = 0; i < sc->event_count; i++) {
            cJSON *arr = cJSON_GetObjectItemCaseSensitive(hooks, sc->event_names[i]);
            int n = strip_liu_hooks(arr);
            removed += n;
            /* Drop the event key entirely if no other entries remain,
             * so no empty key is left behind. */
            if (n > 0 && cJSON_GetArraySize(arr) == 0)
                cJSON_DeleteItemFromObjectCaseSensitive(hooks, sc->event_names[i]);
        }
        removed += strip_legacy_liu_events(hooks);   /* also clear any SubagentStop block */
        if (removed > 0 && cJSON_GetArraySize(hooks) == 0)
            cJSON_DeleteItemFromObjectCaseSensitive(root, "hooks");
    }

    /* When no Liu hook is present, leave the file unchanged. Re-serializing
     * JSON can change spacing and number spelling even when key order is
     * preserved. */
    if (removed == 0) {
        cJSON_Delete(root);
        res.ok = true;
        snprintf(res.msg, sizeof res.msg, "No Liu hooks in %s — nothing to remove.", path);
        return res;
    }
    char target[sizeof path];
    if (wide_int || !agent_hook_write_path(path, target, sizeof target, NULL)) {
        cJSON_Delete(root);
        if (wide_int) snprintf(res.msg, sizeof res.msg, WIDE_INT_MSG, path);
        else snprintf(res.msg, sizeof res.msg, "Cannot follow the link at %s — left untouched", path);
        return res;
    }

    bool ok = atomic_write_json(target, root);
    cJSON_Delete(root);
    if (!ok) {
        snprintf(res.msg, sizeof res.msg, "Failed to write %s", path);
        return res;
    }
    res.ok = true;
    snprintf(res.msg, sizeof res.msg, "Removed Liu hooks from %s", path);
    return res;
}

static bool hooks_installed_schema(const HookSchema *sc) {
    char path[512];
    if (!resolve_settings_path(path, sizeof path, sc->rel_path)) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;

    cJSON *root  = load_or_empty(path, NULL);
    cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
    bool found = false;
    if (cJSON_IsObject(hooks)) {
        for (int i = 0; i < sc->event_count && !found; i++) {
            cJSON *arr = cJSON_GetObjectItemCaseSensitive(hooks, sc->event_names[i]);
            if (!cJSON_IsArray(arr)) continue;
            const cJSON *block;
            cJSON_ArrayForEach(block, arr) {
                if (block_has_liu(block)) { found = true; break; }
            }
        }
    }
    cJSON_Delete(root);
    return found;
}

/* ----------------------------------------------------------------- *
 *  Per-agent entry points                                            *
 * ----------------------------------------------------------------- */

ClaudeHookResult claude_hooks_install(const char *notify_bin_path) {
    return hooks_install_schema(notify_bin_path, &kClaudeSchema);
}
ClaudeHookResult claude_hooks_uninstall(void) {
    return hooks_uninstall_schema(&kClaudeSchema);
}
bool claude_hooks_installed(void) {
    return hooks_installed_schema(&kClaudeSchema);
}

ClaudeHookResult commandcode_hooks_install(const char *notify_bin_path) {
    return hooks_install_schema(notify_bin_path, &kCommandCodeSchema);
}
ClaudeHookResult commandcode_hooks_uninstall(void) {
    return hooks_uninstall_schema(&kCommandCodeSchema);
}
bool commandcode_hooks_installed(void) {
    return hooks_installed_schema(&kCommandCodeSchema);
}
