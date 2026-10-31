/*
 * See tool_args.h. Each vocabulary below is the tool set of the version this
 * was written against, read from that agent's own tool definitions and its own
 * sessions: Claude Code 2.1, Kimi Code CLI 1.49, Command Code 1.88, OpenCode
 * 1.x and 2.2, Grok CLI 1.0.
 */
#include "history/bridge/tool_args.h"
#include "history/util.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- reading -- */

static const char *str_of(const cJSON *o, const char *key) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    return (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) ? v->valuestring : NULL;
}

/* The first of `keys` that `o` holds as a non-empty string. */
static const char *first_str(const cJSON *o, const char *const *keys, usize n) {
    for (usize i = 0; i < n; i++) {
        const char *s = str_of(o, keys[i]);
        if (s) return s;
    }
    return NULL;
}

#define FIRST(o, ...) first_str((o), (const char *const[]){ __VA_ARGS__ }, \
                                sizeof((const char *const[]){ __VA_ARGS__ }) / sizeof(const char *))

static char *keep(BridgeCallFacts *f, char *s) {
    if (!s) return NULL;
    if (f->n_owned >= sizeof f->owned / sizeof f->owned[0]) { free(s); return NULL; }
    f->owned[f->n_owned++] = s;
    return s;
}

static char *keep_n(BridgeCallFacts *f, const char *s, usize n) {
    char *c = malloc(n + 1);
    if (!c) return NULL;
    memcpy(c, s, n);
    c[n] = '\0';
    return keep(f, c);
}

static const char *base_of(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* A command given as argv: the script of `sh -c`, `bash -lc` and the like,
 * the words joined with blanks otherwise. */
static const char *argv_command(BridgeCallFacts *f, const cJSON *arr) {
    int n = cJSON_GetArraySize(arr);
    if (n <= 0) return NULL;
    const cJSON *a0 = cJSON_GetArrayItem(arr, 0), *a1 = cJSON_GetArrayItem(arr, 1);
    const cJSON *a2 = cJSON_GetArrayItem(arr, 2);
    if (n == 3 && cJSON_IsString(a0) && cJSON_IsString(a1) && cJSON_IsString(a2)) {
        const char *sh = base_of(a0->valuestring);
        const char *fl = a1->valuestring;
        bool shell = !strcmp(sh, "bash") || !strcmp(sh, "zsh") || !strcmp(sh, "sh") ||
                     !strcmp(sh, "fish") || !strcmp(sh, "dash");
        bool cflag = !strcmp(fl, "-c") || !strcmp(fl, "-lc") || !strcmp(fl, "-ic") ||
                     !strcmp(fl, "-lic");
        if (shell && cflag) return a2->valuestring;
    }
    usize len = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) if (cJSON_IsString(it)) len += strlen(it->valuestring) + 1;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    usize off = 0;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsString(it)) continue;
        if (off) out[off++] = ' ';
        usize k = strlen(it->valuestring);
        memcpy(out + off, it->valuestring, k);
        off += k;
    }
    out[off] = '\0';
    return keep(f, out);
}

static void add_edit(BridgeCallFacts *f, const char *old_text, const char *new_text) {
    if (f->n_edits >= BRIDGE_CALL_EDITS_MAX || (!old_text && !new_text)) return;
    f->edits[f->n_edits].old_text = old_text ? old_text : "";
    f->edits[f->n_edits].new_text = new_text ? new_text : "";
    f->n_edits++;
}

/* One {old,new} replacement in any of the spellings agents record it in. */
static bool edit_from(BridgeCallFacts *f, const cJSON *o) {
    static const char *const pairs[][2] = {
        { "old_string", "new_string" }, { "oldString", "newString" },
        { "old_str", "new_str" },       { "old_text", "new_text" },
        { "old", "new" },               { "search", "replace" },
    };
    for (usize i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, pairs[i][0]);
        const cJSON *b = cJSON_GetObjectItemCaseSensitive(o, pairs[i][1]);
        if (cJSON_IsString(a) || cJSON_IsString(b)) {
            add_edit(f, cJSON_IsString(a) ? a->valuestring : "", cJSON_IsString(b) ? b->valuestring : "");
            return true;
        }
    }
    return false;
}

/* Replacements out of unified-diff hunks: each hunk's context and removed
 * lines as the old text, its context and added lines as the new. Lines start
 * with ' ', '-' or '+'; "\ No newline" lines and headers are skipped. */
static void edits_from_hunks(BridgeCallFacts *f, const char *hunks) {
    const char *p = hunks;
    while (p && *p && f->n_edits < BRIDGE_CALL_EDITS_MAX) {
        const char *h = strstr(p, "@@");
        if (!h) break;
        const char *line = strchr(h, '\n');
        if (!line) break;
        line++;
        usize cap = strlen(line) + 1;
        char *o = malloc(cap), *n = malloc(cap);
        if (!o || !n) { free(o); free(n); return; }
        usize ol = 0, nl = 0;
        while (*line && strncmp(line, "@@", 2) != 0) {
            const char *e = strchr(line, '\n');
            usize len = e ? (usize)(e - line) : strlen(line);
            char c = line[0];
            if (c == ' ' || c == '-' || c == '+') {
                if (c != '+') { memcpy(o + ol, line + 1, len - 1); ol += len - 1; o[ol++] = '\n'; }
                if (c != '-') { memcpy(n + nl, line + 1, len - 1); nl += len - 1; n[nl++] = '\n'; }
            } else if (len == 0) {
                o[ol++] = '\n';
                n[nl++] = '\n';
            } else if (c != '\\') {
                break;   /* a header of the next file */
            }
            line = e ? e + 1 : line + len;
        }
        /* The last line's break is the hunk's end, not text. */
        if (ol && o[ol - 1] == '\n') ol--;
        if (nl && n[nl - 1] == '\n') nl--;
        o[ol] = '\0';
        n[nl] = '\0';
        add_edit(f, keep(f, o), keep(f, n));
        p = line;
    }
}

/* Codex's apply_patch envelope: "*** Begin Patch" / "*** Update File: P" or
 * "*** Add File: P" / hunks / "*** End Patch". Only its first file is read. */
static void read_apply_patch(BridgeCallFacts *f, const char *text) {
    const char *add = strstr(text, "*** Add File: ");
    const char *upd = strstr(text, "*** Update File: ");
    const char *at = upd && (!add || upd < add) ? upd : add;
    if (!at) return;
    bool adding = at == add;
    at += adding ? strlen("*** Add File: ") : strlen("*** Update File: ");
    const char *eol = strchr(at, '\n');
    if (!f->path) f->path = keep_n(f, at, eol ? (usize)(eol - at) : strlen(at));
    if (!eol) return;
    const char *body = eol + 1;
    const char *end = strstr(body, "\n*** ");
    usize len = end ? (usize)(end - body) + 1 : strlen(body);
    if (adding) {
        /* Every line of an added file is a '+' line. */
        char *c = malloc(len + 1);
        if (!c) return;
        usize k = 0;
        for (const char *p = body; p < body + len;) {
            const char *e = memchr(p, '\n', (usize)(body + len - p));
            usize l = e ? (usize)(e - p) : (usize)(body + len - p);
            if (l && p[0] == '+') { memcpy(c + k, p + 1, l - 1); k += l - 1; c[k++] = '\n'; }
            p = e ? e + 1 : body + len;
        }
        c[k] = '\0';
        f->content = keep(f, c);
        f->created = true;
        return;
    }
    /* An update's hunks; a section without "@@" is one hunk. */
    char *h = keep_n(f, body, len);
    if (!h) return;
    if (strstr(h, "@@")) edits_from_hunks(f, h);
    else {
        char *w = malloc(len + 4);
        if (!w) return;
        snprintf(w, len + 4, "@@\n%s", h);
        edits_from_hunks(f, keep(f, w));
    }
}

static const char *todo_status(const char *s) {
    if (!s) return "pending";
    if (!strcmp(s, "completed") || !strcmp(s, "done") || !strcmp(s, "complete") ||
        !strcmp(s, "finished")) return "completed";
    if (!strcmp(s, "in_progress") || !strcmp(s, "in-progress") || !strcmp(s, "active") ||
        !strcmp(s, "doing") || !strcmp(s, "running")) return "in_progress";
    return "pending";
}

static void read_todos(BridgeCallFacts *f) {
    const cJSON *arr = NULL;
    static const char *const keys[] = { "todos", "plan", "items", "tasks", "steps" };
    for (usize i = 0; i < sizeof keys / sizeof keys[0] && !cJSON_IsArray(arr); i++)
        arr = cJSON_GetObjectItemCaseSensitive(f->in, keys[i]);
    if (!cJSON_IsArray(arr)) return;
    cJSON *out = cJSON_CreateArray();
    if (!out) return;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        const char *text = cJSON_IsString(it) ? it->valuestring
                         : FIRST(it, "content", "title", "text", "step", "description", "task", "name");
        if (!text) continue;
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddStringToObject(o, "text", text);
        cJSON_AddStringToObject(o, "status", todo_status(cJSON_IsObject(it) ? str_of(it, "status") : NULL));
        cJSON_AddItemToArray(out, o);
    }
    if (cJSON_GetArraySize(out) > 0) f->todos = out;
    else cJSON_Delete(out);
}

void bridge_call_facts(const BridgeEvent *call, const BridgeEvent *result, BridgeCallFacts *f) {
    memset(f, 0, sizeof *f);
    if (!call) return;
    f->kind = call->tool_kind;
    f->name = call->tool_name;
    if (call->tool_input && call->tool_input[0]) f->in = cJSON_Parse(call->tool_input);
    if (f->in && !cJSON_IsObject(f->in)) { cJSON_Delete(f->in); f->in = NULL; }
    if (result) {
        f->output  = result->tool_output;
        f->failed  = result->tool_failed;
        f->patch   = result->tool_patch;
        f->created = result->tool_file_created;
    }
    const cJSON *in = f->in;

    f->path = FIRST(in, "file_path", "filePath", "path", "target_file", "absolute_path",
                    "file", "filename", "notebook_path", "target_directory", "directory",
                    "dir", "cwd");
    if (!f->path) {
        const cJSON *paths = in ? cJSON_GetObjectItemCaseSensitive(in, "paths") : NULL;
        const cJSON *p0 = cJSON_IsArray(paths) ? cJSON_GetArrayItem(paths, 0) : NULL;
        if (cJSON_IsString(p0) && p0->valuestring[0]) f->path = p0->valuestring;
    }

    const cJSON *cmd = in ? cJSON_GetObjectItemCaseSensitive(in, "command") : NULL;
    if (!cmd && in) cmd = cJSON_GetObjectItemCaseSensitive(in, "cmd");
    if (cJSON_IsArray(cmd)) f->command = argv_command(f, cmd);
    else f->command = FIRST(in, "command", "cmd", "script", "code");

    f->query = FIRST(in, "query", "q", "search_query");
    if (!f->query && in) {
        const cJSON *qs = cJSON_GetObjectItemCaseSensitive(in, "queries");
        const cJSON *q0 = cJSON_IsArray(qs) ? cJSON_GetArrayItem(qs, 0) : NULL;
        if (cJSON_IsString(q0) && q0->valuestring[0]) f->query = q0->valuestring;
    }
    f->pattern = FIRST(in, "pattern", "regex", "glob", "name_pattern", "include", "name");
    if (!f->pattern && (f->kind == BRIDGE_TOOL_TEXT_SEARCH || f->kind == BRIDGE_TOOL_FILE_SEARCH))
        f->pattern = f->query;
    f->url = FIRST(in, "url", "uri", "href");
    f->content = FIRST(in, "content", "contents", "file_text", "new_content");
    if (!f->content && f->kind == BRIDGE_TOOL_FILE_WRITE) f->content = FIRST(in, "text");
    f->description = FIRST(in, "description", "instruction", "title");
    f->prompt = FIRST(in, "prompt", "task", "message");

    if (in) {
        if (!edit_from(f, in)) {
            static const char *const lists[] = { "edits", "edit", "replacements", "changes" };
            for (usize i = 0; i < sizeof lists / sizeof lists[0]; i++) {
                const cJSON *e = cJSON_GetObjectItemCaseSensitive(in, lists[i]);
                if (cJSON_IsObject(e)) { edit_from(f, e); break; }
                if (cJSON_IsArray(e)) {
                    const cJSON *it = NULL;
                    cJSON_ArrayForEach(it, e) if (cJSON_IsObject(it)) edit_from(f, it);
                    if (f->n_edits) break;
                }
            }
        }
        const char *patch_text = FIRST(in, "patchText", "patch", "input");
        if (!f->n_edits && patch_text && strstr(patch_text, "*** Begin Patch"))
            read_apply_patch(f, patch_text);
        read_todos(f);
    }
    if (!f->n_edits && f->patch && f->kind == BRIDGE_TOOL_FILE_PATCH)
        edits_from_hunks(f, f->patch);

    /* An MCP call named the way Claude Code names one. */
    const char *nm = f->name;
    if (nm && strncmp(nm, "mcp__", 5) == 0) {
        const char *srv = nm + 5, *sep = strstr(srv, "__");
        if (sep && sep > srv && sep[2]) {
            usize sn = (usize)(sep - srv);
            if (sn < sizeof f->mcp_server) {
                memcpy(f->mcp_server, srv, sn);
                f->mcp_server[sn] = '\0';
                hist_copy_bounded(f->mcp_tool, sizeof f->mcp_tool, sep + 2);
            }
        }
    }
}

void bridge_call_facts_free(BridgeCallFacts *f) {
    if (!f) return;
    for (u32 i = 0; i < f->n_owned; i++) free(f->owned[i]);
    cJSON_Delete(f->in);
    cJSON_Delete(f->todos);
    memset(f, 0, sizeof *f);
}

/* ------------------------------------------------------------- writing -- */

static void put(cJSON *o, const char *key, const char *v) {
    if (o && v) cJSON_AddStringToObject(o, key, v);
}

static cJSON *own_args(const BridgeCallFacts *f) {
    return f->in ? cJSON_Duplicate(f->in, 1) : cJSON_CreateObject();
}

/* A todo list as `item(text, status)` objects in `key`. */
typedef cJSON *(*TodoItem)(const char *text, const char *status, int index);

static cJSON *todos_as(const BridgeCallFacts *f, TodoItem make) {
    cJSON *arr = cJSON_CreateArray();
    const cJSON *t = NULL;
    int i = 0;
    cJSON_ArrayForEach(t, f->todos) {
        cJSON *o = make(str_of(t, "text"), str_of(t, "status"), i++);
        if (o) cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static cJSON *todo_claude(const char *text, const char *status, int i) {
    (void)i;
    cJSON *o = cJSON_CreateObject();
    put(o, "content", text);
    put(o, "status", status);
    put(o, "activeForm", text);
    return o;
}

static cJSON *todo_kimi(const char *text, const char *status, int i) {
    (void)i;
    cJSON *o = cJSON_CreateObject();
    put(o, "title", text);
    put(o, "status", !strcmp(status, "completed") ? "done" : status);
    return o;
}

static cJSON *todo_with_id(const char *text, const char *status, int i) {
    cJSON *o = cJSON_CreateObject();
    char id[16];
    snprintf(id, sizeof id, "%d", i + 1);
    put(o, "content", text);
    put(o, "status", status);
    put(o, "id", id);
    return o;
}

static cJSON *todo_commandcode(const char *text, const char *status, int i) {
    cJSON *o = todo_with_id(text, status, i);
    put(o, "activeForm", text);
    return o;
}

static cJSON *todo_opencode(const char *text, const char *status, int i) {
    cJSON *o = todo_with_id(text, status, i);
    put(o, "priority", "medium");
    return o;
}

/* Replacements as [{<old_key>, <new_key>}]. */
static cJSON *edits_as(const BridgeCallFacts *f, const char *ok, const char *nk) {
    cJSON *arr = cJSON_CreateArray();
    for (u32 i = 0; arr && i < f->n_edits; i++) {
        cJSON *o = cJSON_CreateObject();
        put(o, ok, f->edits[i].old_text);
        put(o, nk, f->edits[i].new_text);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

/* The edits of `f` in Codex's apply_patch envelope, for one file. */
static char *patch_text(const BridgeCallFacts *f) {
    usize cap = 64 + strlen(f->path);
    for (u32 i = 0; i < f->n_edits; i++)
        cap += 2 * (strlen(f->edits[i].old_text) + strlen(f->edits[i].new_text)) + 16;
    char *out = malloc(cap);
    if (!out) return NULL;
    usize off = (usize)snprintf(out, cap, "*** Begin Patch\n*** Update File: %s\n", f->path);
    for (u32 i = 0; i < f->n_edits; i++) {
        off += (usize)snprintf(out + off, cap - off, "@@\n");
        for (int side = 0; side < 2; side++) {
            const char *t = side ? f->edits[i].new_text : f->edits[i].old_text;
            while (*t) {
                const char *e = strchr(t, '\n');
                usize n = e ? (usize)(e - t) : strlen(t);
                out[off++] = side ? '+' : '-';
                memcpy(out + off, t, n);
                off += n;
                out[off++] = '\n';
                t = e ? e + 1 : t + n;
            }
        }
    }
    off += (usize)snprintf(out + off, cap - off, "*** End Patch");
    out[off] = '\0';
    return out;
}

/* An MCP call as `<server><sep><tool>`; false when the name says neither. */
static bool mcp_name(const BridgeCallFacts *f, const char *prefix, const char *sep,
                     char *name, usize cap) {
    if (!f->mcp_server[0]) return false;
    int n = snprintf(name, cap, "%s%s%s%s", prefix, f->mcp_server, sep, f->mcp_tool);
    return n > 0 && (usize)n < cap;
}

static bool keep_own(const BridgeCallFacts *f, char *name, usize cap, cJSON **args) {
    hist_copy_bounded(name, cap, f->name && f->name[0] ? f->name : "tool");
    *args = own_args(f);
    return *args != NULL;
}

static bool named(const char *tool, char *name, usize cap, cJSON *args, cJSON **out) {
    if (!args) return false;
    hist_copy_bounded(name, cap, tool);
    *out = args;
    return true;
}

static bool vocab_claude(const BridgeCallFacts *f, char *name, usize cap, cJSON **args) {
    cJSON *a = cJSON_CreateObject();
    switch (f->kind) {
        case BRIDGE_TOOL_SHELL:
            if (!f->command) break;
            put(a, "command", f->command);
            put(a, "description", f->description);
            return named("Bash", name, cap, a, args);
        case BRIDGE_TOOL_FILE_READ:
            if (!f->path) break;
            put(a, "file_path", f->path);
            return named("Read", name, cap, a, args);
        case BRIDGE_TOOL_FILE_WRITE:
        case BRIDGE_TOOL_FILE_PATCH:
            if (!f->path) break;
            put(a, "file_path", f->path);
            if (f->n_edits == 1 && f->kind == BRIDGE_TOOL_FILE_PATCH) {
                put(a, "old_string", f->edits[0].old_text);
                put(a, "new_string", f->edits[0].new_text);
                return named("Edit", name, cap, a, args);
            }
            if (f->n_edits > 1 && f->kind == BRIDGE_TOOL_FILE_PATCH) {
                cJSON_AddItemToObject(a, "edits", edits_as(f, "old_string", "new_string"));
                return named("MultiEdit", name, cap, a, args);
            }
            if (f->content) {
                put(a, "content", f->content);
                return named("Write", name, cap, a, args);
            }
            return named(f->kind == BRIDGE_TOOL_FILE_WRITE ? "Write" : "Edit", name, cap, a, args);
        case BRIDGE_TOOL_TEXT_SEARCH:
            if (!f->pattern) break;
            put(a, "pattern", f->pattern);
            put(a, "path", f->path);
            return named("Grep", name, cap, a, args);
        case BRIDGE_TOOL_FILE_SEARCH:
            put(a, "pattern", f->pattern ? f->pattern : "*");
            put(a, "path", f->path);
            return named("Glob", name, cap, a, args);
        case BRIDGE_TOOL_WEB_SEARCH:
            if (!f->query) break;
            put(a, "query", f->query);
            return named("WebSearch", name, cap, a, args);
        case BRIDGE_TOOL_WEB_FETCH:
            if (!f->url) break;
            put(a, "url", f->url);
            put(a, "prompt", f->prompt);
            return named("WebFetch", name, cap, a, args);
        case BRIDGE_TOOL_TASK:
            if (!f->todos) break;
            cJSON_AddItemToObject(a, "todos", todos_as(f, todo_claude));
            return named("TodoWrite", name, cap, a, args);
        case BRIDGE_TOOL_SUBAGENT:
            put(a, "description", f->description ? f->description : "Subagent");
            put(a, "prompt", f->prompt ? f->prompt : "");
            put(a, "subagent_type", "general-purpose");
            return named("Task", name, cap, a, args);
        case BRIDGE_TOOL_MCP:
            if (mcp_name(f, "mcp__", "__", name, cap)) {
                cJSON_Delete(a);
                *args = own_args(f);
                return *args != NULL;
            }
            break;
        default:
            break;
    }
    cJSON_Delete(a);
    return keep_own(f, name, cap, args);
}

static bool vocab_kimi(const BridgeCallFacts *f, char *name, usize cap, cJSON **args) {
    cJSON *a = cJSON_CreateObject();
    switch (f->kind) {
        case BRIDGE_TOOL_SHELL:
            if (!f->command) break;
            put(a, "command", f->command);
            return named("Shell", name, cap, a, args);
        case BRIDGE_TOOL_FILE_READ:
            if (!f->path) break;
            put(a, "path", f->path);
            return named("ReadFile", name, cap, a, args);
        case BRIDGE_TOOL_FILE_WRITE:
        case BRIDGE_TOOL_FILE_PATCH:
            if (!f->path) break;
            put(a, "path", f->path);
            if (f->n_edits && f->kind == BRIDGE_TOOL_FILE_PATCH) {
                if (f->n_edits == 1) {
                    cJSON *e = cJSON_CreateObject();
                    put(e, "old", f->edits[0].old_text);
                    put(e, "new", f->edits[0].new_text);
                    cJSON_AddItemToObject(a, "edit", e);
                } else {
                    cJSON_AddItemToObject(a, "edit", edits_as(f, "old", "new"));
                }
                return named("StrReplaceFile", name, cap, a, args);
            }
            put(a, "content", f->content ? f->content : "");
            return named("WriteFile", name, cap, a, args);
        case BRIDGE_TOOL_TEXT_SEARCH:
            if (!f->pattern) break;
            put(a, "pattern", f->pattern);
            put(a, "path", f->path);
            return named("Grep", name, cap, a, args);
        case BRIDGE_TOOL_FILE_SEARCH:
            put(a, "pattern", f->pattern ? f->pattern : "*");
            put(a, "directory", f->path);
            return named("Glob", name, cap, a, args);
        case BRIDGE_TOOL_WEB_SEARCH:
            if (!f->query) break;
            put(a, "query", f->query);
            return named("SearchWeb", name, cap, a, args);
        case BRIDGE_TOOL_WEB_FETCH:
            if (!f->url) break;
            put(a, "url", f->url);
            return named("FetchURL", name, cap, a, args);
        case BRIDGE_TOOL_TASK:
            if (!f->todos) break;
            cJSON_AddItemToObject(a, "todos", todos_as(f, todo_kimi));
            return named("SetTodoList", name, cap, a, args);
        case BRIDGE_TOOL_SUBAGENT:
            put(a, "description", f->description ? f->description : "Subagent");
            put(a, "prompt", f->prompt ? f->prompt : "");
            return named("Agent", name, cap, a, args);
        default:
            break;
    }
    cJSON_Delete(a);
    return keep_own(f, name, cap, args);
}

static bool vocab_commandcode(const BridgeCallFacts *f, char *name, usize cap, cJSON **args) {
    cJSON *a = cJSON_CreateObject();
    switch (f->kind) {
        case BRIDGE_TOOL_SHELL:
            if (!f->command) break;
            put(a, "command", f->command);
            put(a, "description", f->description);
            return named("shell_command", name, cap, a, args);
        case BRIDGE_TOOL_FILE_READ: {
            if (!f->path) break;
            cJSON *paths = cJSON_AddArrayToObject(a, "paths");
            cJSON_AddItemToArray(paths, cJSON_CreateString(f->path));
            return named("read_file", name, cap, a, args);
        }
        case BRIDGE_TOOL_FILE_WRITE:
        case BRIDGE_TOOL_FILE_PATCH:
            if (!f->path) break;
            put(a, "file_path", f->path);
            if (f->n_edits && f->kind == BRIDGE_TOOL_FILE_PATCH) {
                put(a, "old_string", f->edits[0].old_text);
                put(a, "new_string", f->edits[0].new_text);
                return named("edit_file", name, cap, a, args);
            }
            put(a, "content", f->content ? f->content : "");
            return named("write_file", name, cap, a, args);
        case BRIDGE_TOOL_TEXT_SEARCH:
            if (!f->pattern) break;
            put(a, "pattern", f->pattern);
            put(a, "path", f->path);
            return named("grep", name, cap, a, args);
        case BRIDGE_TOOL_FILE_SEARCH:
            put(a, "pattern", f->pattern ? f->pattern : "*");
            put(a, "path", f->path);
            return named("glob", name, cap, a, args);
        case BRIDGE_TOOL_WEB_SEARCH:
            if (!f->query) break;
            put(a, "query", f->query);
            return named("web_search", name, cap, a, args);
        case BRIDGE_TOOL_WEB_FETCH:
            if (!f->url) break;
            put(a, "url", f->url);
            put(a, "format", "markdown");
            return named("web_fetch", name, cap, a, args);
        case BRIDGE_TOOL_TASK:
            if (!f->todos) break;
            cJSON_AddItemToObject(a, "todos", todos_as(f, todo_commandcode));
            return named("todo_write", name, cap, a, args);
        case BRIDGE_TOOL_SUBAGENT:
            put(a, "subagent_type", "general-purpose");
            put(a, "description", f->description ? f->description : "Subagent");
            put(a, "prompt", f->prompt ? f->prompt : "");
            return named("agent", name, cap, a, args);
        case BRIDGE_TOOL_MCP:
            if (mcp_name(f, "mcp__", "__", name, cap)) {
                cJSON_Delete(a);
                *args = own_args(f);
                return *args != NULL;
            }
            break;
        default:
            break;
    }
    cJSON_Delete(a);
    return keep_own(f, name, cap, args);
}

static bool vocab_opencode(const BridgeCallFacts *f, bool v2, char *name, usize cap,
                           cJSON **args) {
    const char *pk = v2 ? "path" : "filePath";
    cJSON *a = cJSON_CreateObject();
    switch (f->kind) {
        case BRIDGE_TOOL_SHELL:
            if (!f->command) break;
            put(a, "command", f->command);
            if (!v2) put(a, "description", f->description);
            return named(v2 ? "shell" : "bash", name, cap, a, args);
        case BRIDGE_TOOL_FILE_READ:
            if (!f->path) break;
            put(a, pk, f->path);
            return named("read", name, cap, a, args);
        case BRIDGE_TOOL_FILE_WRITE:
        case BRIDGE_TOOL_FILE_PATCH:
            if (!f->path) break;
            if (f->n_edits && f->kind == BRIDGE_TOOL_FILE_PATCH) {
                if (f->n_edits > 1 && v2) {
                    char *pt = patch_text(f);
                    if (pt) {
                        put(a, "patchText", pt);
                        free(pt);
                        return named("patch", name, cap, a, args);
                    }
                }
                put(a, pk, f->path);
                put(a, "oldString", f->edits[0].old_text);
                put(a, "newString", f->edits[0].new_text);
                return named("edit", name, cap, a, args);
            }
            put(a, pk, f->path);
            put(a, "content", f->content ? f->content : "");
            return named("write", name, cap, a, args);
        case BRIDGE_TOOL_TEXT_SEARCH:
            if (!f->pattern) break;
            put(a, "pattern", f->pattern);
            put(a, "path", f->path);
            return named("grep", name, cap, a, args);
        case BRIDGE_TOOL_FILE_SEARCH:
            put(a, "pattern", f->pattern ? f->pattern : "*");
            put(a, "path", f->path);
            return named("glob", name, cap, a, args);
        case BRIDGE_TOOL_WEB_SEARCH:
            if (!f->query) break;
            put(a, "query", f->query);
            return named("websearch", name, cap, a, args);
        case BRIDGE_TOOL_WEB_FETCH:
            if (!f->url) break;
            put(a, "url", f->url);
            put(a, "format", "markdown");
            return named("webfetch", name, cap, a, args);
        case BRIDGE_TOOL_TASK:
            if (!f->todos) break;
            cJSON_AddItemToObject(a, "todos", todos_as(f, todo_opencode));
            return named("todowrite", name, cap, a, args);
        case BRIDGE_TOOL_SUBAGENT:
            if (v2) put(a, "agent", "general");
            put(a, "description", f->description ? f->description : "Subagent");
            put(a, "prompt", f->prompt ? f->prompt : "");
            if (!v2) put(a, "subagent_type", "general");
            return named(v2 ? "subagent" : "task", name, cap, a, args);
        case BRIDGE_TOOL_MCP:
            /* OpenCode names an MCP tool <server>_<tool>. */
            if (mcp_name(f, "", "_", name, cap)) {
                cJSON_Delete(a);
                *args = own_args(f);
                return *args != NULL;
            }
            break;
        default:
            break;
    }
    cJSON_Delete(a);
    return keep_own(f, name, cap, args);
}

static bool vocab_grok(const BridgeCallFacts *f, char *name, usize cap, cJSON **args) {
    cJSON *a = cJSON_CreateObject();
    switch (f->kind) {
        case BRIDGE_TOOL_SHELL:
            if (!f->command) break;
            put(a, "command", f->command);
            put(a, "description", f->description);
            return named("run_terminal_command", name, cap, a, args);
        case BRIDGE_TOOL_FILE_READ:
            if (!f->path) break;
            put(a, "target_file", f->path);
            return named("read_file", name, cap, a, args);
        case BRIDGE_TOOL_FILE_WRITE:
        case BRIDGE_TOOL_FILE_PATCH:
            if (!f->path) break;
            put(a, "file_path", f->path);
            if (f->n_edits && f->kind == BRIDGE_TOOL_FILE_PATCH) {
                put(a, "old_string", f->edits[0].old_text);
                put(a, "new_string", f->edits[0].new_text);
                return named("search_replace", name, cap, a, args);
            }
            put(a, "content", f->content ? f->content : "");
            return named("write", name, cap, a, args);
        case BRIDGE_TOOL_TEXT_SEARCH:
            if (!f->pattern) break;
            put(a, "pattern", f->pattern);
            put(a, "path", f->path);
            return named("grep", name, cap, a, args);
        case BRIDGE_TOOL_FILE_SEARCH:
            if (!f->pattern && f->path) {
                put(a, "target_directory", f->path);
                return named("list_dir", name, cap, a, args);
            }
            put(a, "pattern", f->pattern ? f->pattern : "*");
            return named("glob", name, cap, a, args);
        case BRIDGE_TOOL_WEB_SEARCH:
            if (!f->query) break;
            put(a, "query", f->query);
            return named("web_search", name, cap, a, args);
        case BRIDGE_TOOL_WEB_FETCH:
            if (!f->url) break;
            put(a, "url", f->url);
            return named("web_fetch", name, cap, a, args);
        case BRIDGE_TOOL_TASK:
            if (!f->todos) break;
            cJSON_AddItemToObject(a, "todos", todos_as(f, todo_with_id));
            cJSON_AddBoolToObject(a, "merge", false);
            return named("todo_write", name, cap, a, args);
        case BRIDGE_TOOL_SUBAGENT:
            put(a, "description", f->description ? f->description : "Subagent");
            put(a, "prompt", f->prompt ? f->prompt : "");
            return named("spawn_subagent", name, cap, a, args);
        case BRIDGE_TOOL_MCP:
            if (mcp_name(f, "", "__", name, cap)) {
                cJSON_Delete(a);
                *args = own_args(f);
                return *args != NULL;
            }
            break;
        default:
            break;
    }
    cJSON_Delete(a);
    return keep_own(f, name, cap, args);
}

static bool vocab_pi(const BridgeCallFacts *f, char *name, usize cap, cJSON **args) {
    cJSON *a = cJSON_CreateObject();
    switch (f->kind) {
        case BRIDGE_TOOL_SHELL:
            if (!f->command) break;
            put(a, "command", f->command);
            return named("bash", name, cap, a, args);
        case BRIDGE_TOOL_FILE_READ:
            if (!f->path) break;
            put(a, "path", f->path);
            return named("read", name, cap, a, args);
        case BRIDGE_TOOL_FILE_WRITE:
        case BRIDGE_TOOL_FILE_PATCH:
            if (!f->path) break;
            put(a, "path", f->path);
            if (f->n_edits && f->kind == BRIDGE_TOOL_FILE_PATCH) {
                cJSON_AddItemToObject(a, "edits", edits_as(f, "oldText", "newText"));
                return named("edit", name, cap, a, args);
            }
            put(a, "content", f->content ? f->content : "");
            return named("write", name, cap, a, args);
        case BRIDGE_TOOL_TEXT_SEARCH:
            if (!f->pattern) break;
            put(a, "pattern", f->pattern);
            put(a, "path", f->path);
            return named("grep", name, cap, a, args);
        case BRIDGE_TOOL_FILE_SEARCH:
            put(a, "pattern", f->pattern ? f->pattern : "*");
            put(a, "path", f->path);
            return named("find", name, cap, a, args);
        default:
            break;
    }
    cJSON_Delete(a);
    return keep_own(f, name, cap, args);
}

static cJSON *todo_text(const char *text, const char *status, int i) {
    (void)status;
    (void)i;
    return text ? cJSON_CreateString(text) : NULL;
}

static cJSON *todo_omp(const char *text, const char *status, int i) {
    (void)i;
    if (!text) return NULL;
    cJSON *o = cJSON_CreateObject();
    put(o, "content", text);
    put(o, "status", status ? status : "pending");
    return o;
}

#define OMP_TODO_PHASE "Todos"

cJSON *bridge_omp_todo_details(const BridgeCallFacts *f) {
    if (!f || !f->todos || cJSON_GetArraySize(f->todos) == 0) return NULL;
    cJSON *d = cJSON_CreateObject();
    put(d, "op", "init");
    cJSON *phases = cJSON_AddArrayToObject(d, "phases");
    cJSON *ph = cJSON_CreateObject();
    put(ph, "name", OMP_TODO_PHASE);
    cJSON_AddItemToObject(ph, "tasks", todos_as(f, todo_omp));
    cJSON_AddItemToArray(phases, ph);
    put(d, "storage", "session");
    return d;
}

/* Oh My Pi keeps Pi's read, bash, edit, write and grep, finds files with
 * `glob` (one `path` that holds the pattern), reads a web page with `read`,
 * and has `web_search` and an op-based `todo`. */
static bool vocab_omp(const BridgeCallFacts *f, char *name, usize cap, cJSON **args) {
    cJSON *a = cJSON_CreateObject();
    switch (f->kind) {
        case BRIDGE_TOOL_FILE_SEARCH: {
            const char *pat = f->pattern, *dir = f->path;
            if (pat && dir && pat[0] != '/') {
                usize n = strlen(dir) + strlen(pat) + 2;
                char *both = malloc(n);
                if (both) {
                    snprintf(both, n, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] == '/' ? "" : "/", pat);
                    put(a, "path", both);
                    free(both);
                }
            } else if (pat || dir) {
                put(a, "path", pat ? pat : dir);
            }
            return named("glob", name, cap, a, args);
        }
        case BRIDGE_TOOL_WEB_SEARCH:
            if (!f->query) break;
            put(a, "query", f->query);
            return named("web_search", name, cap, a, args);
        case BRIDGE_TOOL_WEB_FETCH:
            if (!f->url) break;
            put(a, "path", f->url);
            return named("read", name, cap, a, args);
        case BRIDGE_TOOL_TASK: {
            if (!f->todos || cJSON_GetArraySize(f->todos) == 0) break;
            put(a, "op", "init");
            cJSON *list = cJSON_AddArrayToObject(a, "list");
            cJSON *ph = cJSON_CreateObject();
            put(ph, "phase", OMP_TODO_PHASE);
            cJSON_AddItemToObject(ph, "items", todos_as(f, todo_text));
            cJSON_AddItemToArray(list, ph);
            return named("todo", name, cap, a, args);
        }
        default:
            cJSON_Delete(a);
            return vocab_pi(f, name, cap, args);
    }
    cJSON_Delete(a);
    return keep_own(f, name, cap, args);
}

bool bridge_call_in_vocab(BridgeVocab vocab, const BridgeCallFacts *f,
                          char *name, usize name_cap, cJSON **args) {
    if (!f || !name || name_cap == 0 || !args) return false;
    *args = NULL;
    switch (vocab) {
        case BRIDGE_VOCAB_CLAUDE:      return vocab_claude(f, name, name_cap, args);
        case BRIDGE_VOCAB_KIMI:        return vocab_kimi(f, name, name_cap, args);
        case BRIDGE_VOCAB_COMMANDCODE: return vocab_commandcode(f, name, name_cap, args);
        case BRIDGE_VOCAB_OPENCODE_V1: return vocab_opencode(f, false, name, name_cap, args);
        case BRIDGE_VOCAB_OPENCODE_V2: return vocab_opencode(f, true, name, name_cap, args);
        case BRIDGE_VOCAB_GROK:        return vocab_grok(f, name, name_cap, args);
        case BRIDGE_VOCAB_PI:          return vocab_pi(f, name, name_cap, args);
        case BRIDGE_VOCAB_OMP:         return vocab_omp(f, name, name_cap, args);
    }
    return keep_own(f, name, name_cap, args);
}

/* ------------------------------------------------------------ results --- */

const BridgeEvent *bridge_result_for(const BridgeHistory *h, usize call_index) {
    if (!h || call_index >= h->event_count) return NULL;
    const BridgeEvent *c = &h->events[call_index];
    if (c->type != BRIDGE_EVENT_TOOL_CALL || !c->tool_call_id) return NULL;
    usize end = call_index + 1 + 4096;
    if (end > h->event_count) end = h->event_count;
    for (usize k = call_index + 1; k < end; k++) {
        const BridgeEvent *r = &h->events[k];
        if (r->type == BRIDGE_EVENT_TOOL_RESULT && r->tool_call_id &&
            strcmp(r->tool_call_id, c->tool_call_id) == 0)
            return r;
    }
    return NULL;
}

void bridge_patch_counts(const char *hunks, u32 *added, u32 *removed) {
    u32 a = 0, r = 0;
    bool in_hunk = false;
    for (const char *p = hunks; p && *p;) {
        const char *e = strchr(p, '\n');
        usize n = e ? (usize)(e - p) : strlen(p);
        if (n >= 2 && p[0] == '@' && p[1] == '@') in_hunk = true;
        else if (in_hunk && n && p[0] == '+') a++;
        else if (in_hunk && n && p[0] == '-') r++;
        p = e ? e + 1 : p + n;
    }
    if (added) *added = a;
    if (removed) *removed = r;
}

cJSON *bridge_patch_structured(const char *hunks) {
    if (!hunks) return NULL;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    cJSON *cur = NULL, *lines = NULL;
    for (const char *p = hunks; *p;) {
        const char *e = strchr(p, '\n');
        usize n = e ? (usize)(e - p) : strlen(p);
        long os = 0, ol = 1, ns = 0, nl = 1;
        if (n >= 2 && p[0] == '@' && p[1] == '@') {
            char head[128];
            usize k = n < sizeof head - 1 ? n : sizeof head - 1;
            memcpy(head, p, k);
            head[k] = '\0';
            if (sscanf(head, "@@ -%ld,%ld +%ld,%ld", &os, &ol, &ns, &nl) == 4 ||
                sscanf(head, "@@ -%ld +%ld,%ld", &os, &ns, &nl) == 3 ||
                sscanf(head, "@@ -%ld,%ld +%ld", &os, &ol, &ns) == 3) {
                cur = cJSON_CreateObject();
                cJSON_AddNumberToObject(cur, "oldStart", (double)os);
                cJSON_AddNumberToObject(cur, "oldLines", (double)ol);
                cJSON_AddNumberToObject(cur, "newStart", (double)ns);
                cJSON_AddNumberToObject(cur, "newLines", (double)nl);
                lines = cJSON_AddArrayToObject(cur, "lines");
                cJSON_AddItemToArray(arr, cur);
            } else {
                cur = NULL;
                lines = NULL;
            }
        } else if (lines && n && (p[0] == ' ' || p[0] == '-' || p[0] == '+' || p[0] == '\\')) {
            char *l = malloc(n + 1);
            if (l) {
                memcpy(l, p, n);
                l[n] = '\0';
                cJSON_AddItemToArray(lines, cJSON_CreateString(l));
                free(l);
            }
        }
        p = e ? e + 1 : p + n;
    }
    if (cJSON_GetArraySize(arr) == 0) { cJSON_Delete(arr); return NULL; }
    return arr;
}
