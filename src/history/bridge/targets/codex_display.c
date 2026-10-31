/*
 * See codex_display.h. The records written here are the event_msg payloads
 * Codex itself persists in a thread of this kind; the agent messages use the
 * Markdown Codex renders in any agent message.
 */
#include "history/bridge/targets/codex_display.h"
#include "history/bridge/tool_map.h"
#include "history/util.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOTE_CALL        "[external_agent_tool_call: "
#define NOTE_CALL_END    "[/external_agent_tool_call]"
#define NOTE_RESULT      "[external_agent_tool_result"
#define NOTE_RESULT_END  "[/external_agent_tool_result]"
#define IMPORT_MARKER    "<EXTERNAL SESSION IMPORTED>"

/* How far past the last matched call a note's name is looked for. The notes
 * and the IR come from one transcript, so a gap is calls one side kept and
 * the other did not. */
#define MATCH_AHEAD      16
/* How far past a call its result is looked for. */
#define RESULT_AHEAD     4096
/* Lines of output shown above and below the elision, as in Codex's own
 * command cell. */
#define PREVIEW_LINES    5
/* Call notes waiting for their result note. */
#define PENDING_MAX      256
#define COMMAND_CAP      4096
#define PATH_CAP         2048

/* ------------------------------------------------------------------ text -- */

typedef struct { char *p; usize len, cap; bool bad; } Text;

static void text_put(Text *t, const char *s, usize n) {
    if (t->bad || n == 0) return;
    if (t->len + n + 1 > t->cap) {
        usize ncap = t->cap ? t->cap : 256;
        while (ncap < t->len + n + 1) {
            if (ncap > ((usize)-1) / 2) { t->bad = true; return; }
            ncap *= 2;
        }
        char *q = realloc(t->p, ncap);
        if (!q) { t->bad = true; return; }
        t->p = q;
        t->cap = ncap;
    }
    memcpy(t->p + t->len, s, n);
    t->len += n;
    t->p[t->len] = '\0';
}

static void text_puts(Text *t, const char *s) { if (s) text_put(t, s, strlen(s)); }
static void text_clear(Text *t) { t->len = 0; if (t->p) t->p[0] = '\0'; }

static bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* `s` (`n` bytes) without the blanks around it. */
static const char *trimmed(const char *s, usize n, usize *out_n) {
    while (n > 0 && is_blank(*s)) { s++; n--; }
    while (n > 0 && is_blank(s[n - 1])) n--;
    *out_n = n;
    return s;
}

/* Longest run of backticks in `s`. */
static usize backtick_run(const char *s, usize n) {
    usize best = 0, run = 0;
    for (usize i = 0; i < n; i++) {
        run = (s[i] == '`') ? run + 1 : 0;
        if (run > best) best = run;
    }
    return best;
}

/* A control character, which a tool's output can hold and a terminal must
 * not be handed. */
static bool is_control(unsigned char c) { return (c < 0x20 && c != '\t') || c == 0x7F; }

/* `s` as inline code: delimited by more backticks than it holds, padded when
 * it starts or ends with one. */
static void put_code(Text *t, const char *s, usize n) {
    usize ticks = backtick_run(s, n) + 1;
    bool pad = n > 0 && (s[0] == '`' || s[n - 1] == '`');
    for (usize i = 0; i < ticks; i++) text_put(t, "`", 1);
    if (pad) text_put(t, " ", 1);
    for (usize i = 0; i < n; i++)
        if (!is_control((unsigned char)s[i])) text_put(t, s + i, 1);
    if (pad) text_put(t, " ", 1);
    for (usize i = 0; i < ticks; i++) text_put(t, "`", 1);
}

/* Punctuation that can begin Markdown: escaped wherever a tool's text is
 * shown, so it is read as the characters it is. */
static bool is_markup(char c) {
    return c && strchr("\\`*_{}[]<>()#+-.!|~=&", c) != NULL;
}

/* `body` — lines that start "└ " or with two blanks — under the title `t`
 * ends with, each on a line of its own (a hard break, which every CommonMark
 * renderer keeps), its leading blanks kept as no-break spaces, its markup
 * escaped and its control characters dropped. */
static void put_tree(Text *t, const char *body, usize n) {
    const char *p = body, *end = body + n;
    while (p < end) {
        const char *nl = memchr(p, '\n', (usize)(end - p));
        const char *le = nl ? nl : end;
        text_puts(t, "\\\n");
        while (p < le && *p == ' ') { text_puts(t, "\xC2\xA0"); p++; }
        for (; p < le; p++) {
            if (is_control((unsigned char)*p)) continue;
            if (is_markup(*p)) text_put(t, "\\", 1);
            text_put(t, p, 1);
        }
        p = nl ? nl + 1 : end;
    }
}

/* One line of a tree under a cell's title: "└ " for the first, two blanks
 * for the rest. */
static void put_branch(Text *t, bool first, const char *s, usize n) {
    if (!first) text_put(t, "\n", 1);
    text_puts(t, first ? "\xE2\x94\x94 " : "  ");
    text_put(t, s, n);
}

/* The output of a call under its title: every line when there are few, the
 * first and last PREVIEW_LINES around a count of the rest otherwise. */
static void put_preview(Text *t, const char *out) {
    if (!out) return;
    usize n_all = strlen(out);
    usize n;
    const char *s = trimmed(out, n_all, &n);
    if (n == 0) return;

    usize lines = 1;
    for (usize i = 0; i < n; i++) if (s[i] == '\n') lines++;

    Text body = {0};
    usize line = 0, start = 0;
    for (usize i = 0; i <= n; i++) {
        if (i < n && s[i] != '\n') continue;
        usize end = i;
        if (end > start && s[end - 1] == '\r') end--;
        bool head = line < PREVIEW_LINES;
        bool tail = line + PREVIEW_LINES >= lines;
        if (lines <= 2 * PREVIEW_LINES || head || tail) {
            put_branch(&body, line == 0, s + start, end - start);
        } else if (line == PREVIEW_LINES) {
            char gap[64];
            int g = snprintf(gap, sizeof gap, "\xE2\x80\xA6 +%llu lines",
                             (unsigned long long)(lines - 2 * PREVIEW_LINES));
            put_branch(&body, false, gap, (usize)g);
        }
        line++;
        start = i + 1;
    }
    if (body.bad) t->bad = true;
    else put_tree(t, body.p, body.len);
    free(body.p);
}

/* ------------------------------------------------------------- rewriting -- */

typedef struct {
    const BridgeHistory *h;
    usize  *calls;          /* event index of every TOOL_CALL, in order        */
    usize   n_calls, next_call;
    Text    out;            /* the rewritten rollout                           */
    Text    prose;          /* text waiting to become one agent message        */
    Text    explore;        /* the reads and searches in a row                 */
    bool    explore_read;   /* the last explore line lists reads               */
    bool    pending[PENDING_MAX];   /* call notes waiting for a result: shown? */
    u32     pending_head, pending_n;
    char    turn[160];
    /* The last record's time, which a record written here takes: Codex
     * reads none without one. */
    char    timestamp[64];
    CodexDisplayCounts *c;
} Ctx;

static void emit(Ctx *x, cJSON *payload) {
    cJSON *rec = cJSON_CreateObject();
    if (!rec || !payload) { cJSON_Delete(rec); cJSON_Delete(payload); x->out.bad = true; return; }
    cJSON_AddStringToObject(rec, "timestamp", x->timestamp);
    cJSON_AddStringToObject(rec, "type", "event_msg");
    cJSON_AddItemToObject(rec, "payload", payload);
    char *s = cJSON_PrintUnformatted(rec);
    cJSON_Delete(rec);
    if (!s) { x->out.bad = true; return; }
    text_puts(&x->out, s);
    text_put(&x->out, "\n", 1);
    free(s);
}

static void emit_agent(Ctx *x, const char *msg) {
    cJSON *p = cJSON_CreateObject();
    if (p) {
        cJSON_AddStringToObject(p, "type", "agent_message");
        cJSON_AddStringToObject(p, "message", msg);
        cJSON_AddNullToObject(p, "phase");
        cJSON_AddNullToObject(p, "memory_citation");
    }
    emit(x, p);
    x->c->agent_messages++;
}

static void flush_prose(Ctx *x) {
    if (x->prose.len == 0) return;
    emit_agent(x, x->prose.p);
    text_clear(&x->prose);
}

static void flush_explore(Ctx *x) {
    if (x->explore.len == 0) return;
    Text m = {0};
    text_puts(&m, "Explored");
    put_tree(&m, x->explore.p, x->explore.len);
    if (m.bad) x->out.bad = true;
    else emit_agent(x, m.p);
    free(m.p);
    text_clear(&x->explore);
    x->explore_read = false;
}

static void flush_all(Ctx *x) {
    flush_prose(x);
    flush_explore(x);
}

static void add_prose(Ctx *x, const char *s, usize n) {
    s = trimmed(s, n, &n);
    if (n == 0) return;
    flush_explore(x);
    if (x->prose.len) text_puts(&x->prose, "\n\n");
    text_put(&x->prose, s, n);
}

static void pending_push(Ctx *x, bool shown) {
    if (x->pending_n == PENDING_MAX) {   /* the oldest can no longer be paired */
        x->pending_head = (x->pending_head + 1) % PENDING_MAX;
        x->pending_n--;
    }
    x->pending[(x->pending_head + x->pending_n) % PENDING_MAX] = shown;
    x->pending_n++;
}

/* Whether the call the next result note answers was shown; false (keep the
 * note) when no call is waiting. */
static bool pending_pop(Ctx *x) {
    if (x->pending_n == 0) return false;
    bool shown = x->pending[x->pending_head];
    x->pending_head = (x->pending_head + 1) % PENDING_MAX;
    x->pending_n--;
    return shown;
}

static bool name_is(const char *a, const char *name, usize n) {
    return a && strlen(a) == n && memcmp(a, name, n) == 0;
}

/* The IR call a note names, or -1. A call staged from the IR wears the name
 * the transcript writer gave it; one copied from a Claude Code session, its
 * own. */
static isize match_call(Ctx *x, const char *name, usize n) {
    usize end = x->next_call + MATCH_AHEAD;
    if (end > x->n_calls) end = x->n_calls;
    for (usize j = x->next_call; j < end; j++) {
        const BridgeEvent *e = &x->h->events[x->calls[j]];
        if (name_is(e->tool_name, name, n) ||
            name_is(bridge_tool_name_for(CHAT_TOOL_CLAUDE, e->tool_kind), name, n)) {
            x->next_call = j + 1;
            return (isize)x->calls[j];
        }
    }
    return -1;
}

static const BridgeEvent *result_of(const Ctx *x, usize call) {
    const BridgeEvent *c = &x->h->events[call];
    if (!c->tool_call_id) return NULL;
    usize end = call + 1 + RESULT_AHEAD;
    if (end > x->h->event_count) end = x->h->event_count;
    for (usize k = call + 1; k < end; k++) {
        const BridgeEvent *r = &x->h->events[k];
        if (r->type == BRIDGE_EVENT_TOOL_RESULT && r->tool_call_id &&
            strcmp(r->tool_call_id, c->tool_call_id) == 0)
            return r;
    }
    return NULL;
}

/* `path` as the session's folder sees it: relative inside it, "" for the
 * folder itself, whole outside it. */
static const char *rel_path(const Ctx *x, const char *path) {
    const char *cwd = x->h->cwd;
    if (!path || !cwd || !cwd[0]) return path;
    usize n = strlen(cwd);
    while (n > 1 && cwd[n - 1] == '/') n--;
    if (strncmp(path, cwd, n) != 0) return path;
    if (path[n] == '\0') return "";
    if (path[n] == '/') return path + n + 1;
    return path;
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return (s && s[1]) ? s + 1 : path;
}

static const char *input_str(cJSON *in, const char *key) {
    cJSON *v = in ? cJSON_GetObjectItemCaseSensitive(in, key) : NULL;
    return (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) ? v->valuestring : NULL;
}

/* A read or a search, as one line of the "Explored" cell in progress. */
static void explore_line(Ctx *x, const char *verb, const char *what, const char *where) {
    flush_prose(x);
    bool read = strcmp(verb, "Read") == 0;
    if (read && x->explore_read) {
        text_puts(&x->explore, ", ");
        text_puts(&x->explore, what);
        return;
    }
    Text line = {0};
    text_puts(&line, verb);
    if (what && what[0]) { text_puts(&line, " "); text_puts(&line, what); }
    if (where && where[0]) { text_puts(&line, " in "); text_puts(&line, where); }
    if (!line.bad) put_branch(&x->explore, x->explore.len == 0, line.p, line.len);
    free(line.p);
    x->explore_read = read;
}

static void titled(Ctx *x, const char *title, const char *code, const char *out) {
    flush_all(x);
    Text m = {0};
    text_puts(&m, title);
    if (code && code[0]) { text_puts(&m, " "); put_code(&m, code, strlen(code)); }
    put_preview(&m, out);
    if (m.bad) x->out.bad = true;
    else emit_agent(x, m.p);
    free(m.p);
}

static cJSON *end_record(const char *type, const BridgeEvent *call, const char *turn) {
    cJSON *p = cJSON_CreateObject();
    if (!p) return NULL;
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "call_id", call->tool_call_id ? call->tool_call_id : "");
    if (turn) cJSON_AddStringToObject(p, "turn_id", turn);
    return p;
}

static cJSON *zero_duration(void) {
    cJSON *d = cJSON_CreateObject();
    if (d) { cJSON_AddNumberToObject(d, "secs", 0); cJSON_AddNumberToObject(d, "nanos", 0); }
    return d;
}

static bool cell_file_change(Ctx *x, const BridgeEvent *call, const BridgeEvent *res,
                             cJSON *in, const char *path) {
    if (!res || (!res->tool_patch && !res->tool_file_created) || !path) return false;
    const char *content = input_str(in, "content");
    if (res->tool_file_created && !content) return false;
    flush_all(x);
    cJSON *p = end_record("patch_apply_end", call, x->turn);
    cJSON *changes = p ? cJSON_AddObjectToObject(p, "changes") : NULL;
    cJSON *ch = changes ? cJSON_AddObjectToObject(changes, path) : NULL;
    if (!ch) { cJSON_Delete(p); x->out.bad = true; return true; }
    if (res->tool_patch) {
        cJSON_AddStringToObject(ch, "type", "update");
        cJSON_AddStringToObject(ch, "unified_diff", res->tool_patch);
        cJSON_AddNullToObject(ch, "move_path");
    } else {
        cJSON_AddStringToObject(ch, "type", "add");
        cJSON_AddStringToObject(ch, "content", content);
    }
    cJSON_AddStringToObject(p, "stdout", res->tool_output ? res->tool_output : "");
    cJSON_AddStringToObject(p, "stderr", "");
    cJSON_AddBoolToObject(p, "success", !res->tool_failed);
    cJSON_AddStringToObject(p, "status", res->tool_failed ? "failed" : "completed");
    emit(x, p);
    x->c->file_changes++;
    return true;
}

/* An MCP tool as Claude Code names it: mcp__<server>__<tool>. */
static bool cell_mcp(Ctx *x, const BridgeEvent *call, const BridgeEvent *res, cJSON *in) {
    const char *name = call->tool_name;
    if (!name || strncmp(name, "mcp__", 5) != 0) return false;
    const char *server = name + 5;
    const char *sep = strstr(server, "__");
    if (!sep || sep == server || !sep[2]) return false;
    flush_all(x);
    cJSON *p = end_record("mcp_tool_call_end", call, x->turn);
    cJSON *inv = p ? cJSON_AddObjectToObject(p, "invocation") : NULL;
    if (!inv) { cJSON_Delete(p); x->out.bad = true; return true; }
    char srv[256];
    usize sn = (usize)(sep - server);
    if (sn >= sizeof srv) sn = sizeof srv - 1;
    memcpy(srv, server, sn);
    srv[sn] = '\0';
    cJSON_AddStringToObject(inv, "server", srv);
    cJSON_AddStringToObject(inv, "tool", sep + 2);
    cJSON *args = in ? cJSON_Duplicate(in, 1) : NULL;
    cJSON_AddItemToObject(inv, "arguments", args ? args : cJSON_CreateNull());
    cJSON_AddItemToObject(p, "duration", zero_duration());
    cJSON *result = cJSON_AddObjectToObject(p, "result");
    cJSON *ok = result ? cJSON_AddObjectToObject(result, "Ok") : NULL;
    cJSON *content = ok ? cJSON_AddArrayToObject(ok, "content") : NULL;
    cJSON *block = content ? cJSON_CreateObject() : NULL;
    if (!block) { cJSON_Delete(p); x->out.bad = true; return true; }
    cJSON_AddStringToObject(block, "type", "text");
    cJSON_AddStringToObject(block, "text", res && res->tool_output ? res->tool_output : "");
    cJSON_AddItemToArray(content, block);
    cJSON_AddBoolToObject(ok, "isError", res && res->tool_failed);
    emit(x, p);
    x->c->mcp_calls++;
    return true;
}

static bool cell_web(Ctx *x, const BridgeEvent *call, cJSON *in, bool fetch) {
    const char *q = fetch ? input_str(in, "url") : input_str(in, "query");
    if (!q) return false;
    flush_all(x);
    cJSON *p = end_record("web_search_end", call, NULL);
    cJSON *action = p ? cJSON_AddObjectToObject(p, "action") : NULL;
    if (!action) { cJSON_Delete(p); x->out.bad = true; return true; }
    cJSON_AddStringToObject(p, "query", q);
    cJSON_AddStringToObject(action, "type", fetch ? "open_page" : "search");
    cJSON_AddStringToObject(action, fetch ? "url" : "query", q);
    emit(x, p);
    x->c->web_searches++;
    return true;
}

static bool cell_plan(Ctx *x, cJSON *in) {
    cJSON *todos = in ? cJSON_GetObjectItemCaseSensitive(in, "todos") : NULL;
    if (!cJSON_IsArray(todos) || cJSON_GetArraySize(todos) == 0) return false;
    Text body = {0};
    cJSON *t = NULL;
    bool first = true;
    cJSON_ArrayForEach(t, todos) {
        const char *what = input_str(t, "content");
        if (!what) continue;
        const char *status = input_str(t, "status");
        Text line = {0};
        text_puts(&line, status && strcmp(status, "completed") == 0 ? "\xE2\x9C\x94 " : "\xE2\x96\xA1 ");
        text_puts(&line, what);
        if (!line.bad) put_branch(&body, first, line.p, line.len);
        free(line.p);
        first = false;
    }
    if (body.len == 0 || body.bad) { free(body.p); return false; }
    flush_all(x);
    Text m = {0};
    text_puts(&m, "Updated Plan");
    put_tree(&m, body.p, body.len);
    if (m.bad) x->out.bad = true;
    else emit_agent(x, m.p);
    free(m.p);
    free(body.p);
    return true;
}

/* The cell for the IR call at `ci`. */
static void cell(Ctx *x, usize ci) {
    const BridgeEvent *call = &x->h->events[ci];
    const BridgeEvent *res = result_of(x, ci);
    const char *out = res ? res->tool_output : NULL;
    cJSON *in = call->tool_input ? cJSON_Parse(call->tool_input) : NULL;
    char path[PATH_CAP];
    bool has_path = bridge_tool_input_path(in, path, sizeof path) != NULL;
    bool done = false;

    switch (call->tool_kind) {
        case BRIDGE_TOOL_SHELL: {
            char cmd[COMMAND_CAP];
            if (!bridge_tool_input_command(in, cmd, sizeof cmd)) break;
            /* A command over several lines is titled by its first. */
            char *nl = strchr(cmd, '\n');
            if (nl) {
                usize keep = (usize)(nl - cmd);
                if (keep + 5 > sizeof cmd) keep = sizeof cmd - 5;
                memcpy(cmd + keep, " \xE2\x80\xA6", 5);
            }
            titled(x, "Ran", cmd, out);
            done = true;
            break;
        }
        case BRIDGE_TOOL_FILE_READ:
            if (!has_path) break;
            explore_line(x, "Read", base_name(path), NULL);
            done = true;
            break;
        case BRIDGE_TOOL_TEXT_SEARCH: {
            const char *q = input_str(in, "pattern");
            if (!q) q = input_str(in, "query");
            if (!q) q = input_str(in, "regex");
            explore_line(x, "Search", q, has_path ? rel_path(x, path) : NULL);
            done = true;
            break;
        }
        case BRIDGE_TOOL_FILE_SEARCH: {
            const char *q = input_str(in, "pattern");
            if (!q) q = input_str(in, "glob");
            explore_line(x, "List", q ? q : (has_path ? rel_path(x, path) : NULL),
                         q && has_path ? rel_path(x, path) : NULL);
            done = true;
            break;
        }
        case BRIDGE_TOOL_FILE_PATCH:
        case BRIDGE_TOOL_FILE_WRITE:
            done = cell_file_change(x, call, res, in, has_path ? path : NULL);
            if (!done && has_path) {
                titled(x, call->tool_kind == BRIDGE_TOOL_FILE_WRITE ? "Wrote" : "Edited",
                       rel_path(x, path), res && res->tool_failed ? out : NULL);
                done = true;
            }
            break;
        case BRIDGE_TOOL_WEB_SEARCH: done = cell_web(x, call, in, false); break;
        case BRIDGE_TOOL_WEB_FETCH:  done = cell_web(x, call, in, true);  break;
        case BRIDGE_TOOL_TASK:       done = cell_plan(x, in);             break;
        case BRIDGE_TOOL_MCP:        done = cell_mcp(x, call, res, in);   break;
        default:
            done = cell_mcp(x, call, res, in);
            break;
    }
    if (!done) titled(x, "Called", call->tool_name ? call->tool_name : "tool", out);
    cJSON_Delete(in);
}

/* An agent message holding notes: its prose, its calls as cells, the result
 * notes of the calls shown dropped. */
static void rewrite_message(Ctx *x, const char *msg) {
    const char *p = msg;
    for (;;) {
        const char *call = strstr(p, NOTE_CALL);
        const char *res = strstr(p, NOTE_RESULT);
        const char *at = (call && (!res || call < res)) ? call : res;
        if (!at) { add_prose(x, p, strlen(p)); return; }
        add_prose(x, p, (usize)(at - p));

        if (at == call) {
            const char *name = at + strlen(NOTE_CALL);
            const char *name_end = strchr(name, ']');
            const char *end = name_end ? strstr(name_end, NOTE_CALL_END) : NULL;
            if (!end || memchr(name, '\n', (usize)(name_end - name))) {
                add_prose(x, at, strlen(at));
                return;
            }
            const char *after = end + strlen(NOTE_CALL_END);
            isize ci = match_call(x, name, (usize)(name_end - name));
            pending_push(x, ci >= 0);
            if (ci >= 0) {
                cell(x, (usize)ci);
                x->c->shown++;
            } else {
                add_prose(x, at, (usize)(after - at));
                x->c->kept++;
            }
            p = after;
        } else {
            const char *end = strstr(at, NOTE_RESULT_END);
            if (!end) { add_prose(x, at, strlen(at)); return; }
            const char *after = end + strlen(NOTE_RESULT_END);
            if (!pending_pop(x)) add_prose(x, at, (usize)(after - at));
            p = after;
        }
    }
}

/* A user message that holds result notes (a tool result the user's own text
 * came with): the notes of the calls shown are taken out. NULL when nothing
 * would be, or nothing else would be left. */
static char *strip_results(Ctx *x, const char *msg) {
    Text t = {0};
    bool cut = false;
    const char *p = msg;
    for (;;) {
        const char *at = strstr(p, NOTE_RESULT);
        const char *end = at ? strstr(at, NOTE_RESULT_END) : NULL;
        if (!at || !end) { text_puts(&t, p); break; }
        const char *after = end + strlen(NOTE_RESULT_END);
        text_put(&t, p, (usize)(at - p));
        if (pending_pop(x)) cut = true;
        else text_put(&t, at, (usize)(after - at));
        p = after;
    }
    usize n = 0;
    const char *s = t.p ? trimmed(t.p, t.len, &n) : NULL;
    if (!cut || t.bad || n == 0) { free(t.p); return NULL; }
    memmove(t.p, s, n);
    t.p[n] = '\0';
    return t.p;
}

static void count_kept(Ctx *x, const char *ptype, cJSON *payload) {
    if (strcmp(ptype, "user_message") == 0) x->c->user_messages++;
    else if (strcmp(ptype, "agent_message") == 0) {
        const char *m = hist_cjson_str(payload, "message");
        if (m && m[0]) x->c->agent_messages++;
    }
    else if (strcmp(ptype, "patch_apply_end") == 0)   x->c->file_changes++;
    else if (strcmp(ptype, "mcp_tool_call_end") == 0) x->c->mcp_calls++;
    else if (strcmp(ptype, "web_search_end") == 0)    x->c->web_searches++;
}

static void copy_line(Ctx *x, const char *s, usize n) {
    text_put(&x->out, s, n);
    text_put(&x->out, "\n", 1);
}

bool codex_display_rewrite(const char *rollout, usize len, const BridgeHistory *h,
                           char **out, usize *out_len, CodexDisplayCounts *counts) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (counts) memset(counts, 0, sizeof *counts);
    if (!rollout || !h || !out || !out_len || !counts) return false;

    Ctx x;
    memset(&x, 0, sizeof x);
    x.h = h;
    x.c = counts;
    if (h->event_count > 0) {
        x.calls = malloc(h->event_count * sizeof *x.calls);
        if (!x.calls) return false;
        for (usize i = 0; i < h->event_count; i++)
            if (h->events[i].type == BRIDGE_EVENT_TOOL_CALL) x.calls[x.n_calls++] = i;
    }

    bool changed = false;
    usize start = 0;
    while (start < len && !x.out.bad) {
        const char *nl = memchr(rollout + start, '\n', len - start);
        usize n = nl ? (usize)(nl - (rollout + start)) : len - start;
        const char *line = rollout + start;
        start += n + (nl ? 1 : 0);
        if (n == 0) continue;

        cJSON *rec = cJSON_ParseWithLength(line, n);
        const char *type = rec ? hist_cjson_str(rec, "type") : NULL;
        cJSON *payload = rec ? cJSON_GetObjectItemCaseSensitive(rec, "payload") : NULL;
        const char *ptype = payload ? hist_cjson_str(payload, "type") : NULL;
        if (!type || strcmp(type, "event_msg") != 0 || !ptype) {
            /* The model's layer and the thread's metadata, as written. */
            copy_line(&x, line, n);
            cJSON_Delete(rec);
            continue;
        }
        const char *ts = hist_cjson_str(rec, "timestamp");
        if (ts) hist_copy_bounded(x.timestamp, sizeof x.timestamp, ts);

        const char *msg = hist_cjson_str(payload, "message");
        bool agent = strcmp(ptype, "agent_message") == 0;
        bool user = strcmp(ptype, "user_message") == 0;
        if (agent && msg && strcmp(msg, IMPORT_MARKER) == 0) {
            flush_all(&x);
            counts->marker_dropped++;
            changed = true;
        } else if (agent && msg && (strstr(msg, NOTE_CALL) || strstr(msg, NOTE_RESULT))) {
            rewrite_message(&x, msg);
            changed = true;
        } else {
            flush_all(&x);
            if (strcmp(ptype, "task_started") == 0) {
                const char *turn = hist_cjson_str(payload, "turn_id");
                hist_copy_bounded(x.turn, sizeof x.turn, turn ? turn : "");
                /* The calls waiting stay: a result the user cut short comes
                 * with the next turn's message. */
            }
            char *stripped = (user && msg && strstr(msg, NOTE_RESULT)) ? strip_results(&x, msg) : NULL;
            if (stripped) {
                cJSON_ReplaceItemInObjectCaseSensitive(payload, "message",
                                                       cJSON_CreateString(stripped));
                char *s = cJSON_PrintUnformatted(rec);
                if (s) copy_line(&x, s, strlen(s));
                else x.out.bad = true;
                free(s);
                free(stripped);
                changed = true;
            } else {
                copy_line(&x, line, n);
            }
            count_kept(&x, ptype, payload);
        }
        cJSON_Delete(rec);
    }
    flush_all(&x);

    free(x.calls);
    free(x.prose.p);
    free(x.explore.p);
    if (!changed || x.out.bad || !x.out.p) {
        free(x.out.p);
        return false;
    }
    *out = x.out.p;
    *out_len = x.out.len;
    return true;
}
