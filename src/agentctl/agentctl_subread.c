/*
 * Liu — reading a subagent from its agent's own store (agentctl_subread.h).
 */
#include "agentctl/agentctl_subread.h"
#include "agentctl/agentctl_adapter.h"
#include "history/event.h"

#include "cJSON.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ------------------------------------------------------------- helpers --- */

static const char *jstr(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

static bool jnum(const cJSON *o, const char *key, double *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v)) return false;
    *out = v->valuedouble;
    return true;
}

static const cJSON *jobj(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsObject(v) ? v : NULL;
}

static const cJSON *jarr(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsArray(v) ? v : NULL;
}

static void emitf(AgentctlEmit emit, void *ctx, AgentctlStyle st, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static void emitf(AgentctlEmit emit, void *ctx, AgentctlStyle st, const char *fmt, ...) {
    if (!emit) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    usize len = (usize)n < sizeof buf ? (usize)n : sizeof buf - 1;
    emit(ctx, st, buf, len);
}

/* A value for one short line: sanitized, newlines folded, cut at `max` bytes
 * on a character boundary with an ellipsis. */
static void one_line(const char *s, usize n, char *out, usize cap, usize max) {
    if (!out || cap == 0) return;
    if (!s) { s = ""; n = 0; }
    if (n > 8192) n = 8192;
    char tmp[2048];
    usize w = agentctl_sanitize(s, n, tmp, sizeof tmp, false);
    const char *t = tmp;
    while (w > 0 && (*t == ' ' || *t == '\t')) { t++; w--; }
    while (w > 0 && (t[w - 1] == ' ' || t[w - 1] == '\t')) w--;
    bool cut = false;
    if (w > max) {
        usize k = max;
        while (k > 0 && ((unsigned char)t[k] & 0xC0) == 0x80) k--;
        w = k;
        cut = true;
    }
    snprintf(out, cap, "%.*s%s", (int)w, t, cut ? "\xE2\x80\xA6" : "");
}

static void one_line_z(const char *s, char *out, usize cap, usize max) {
    one_line(s, s ? strlen(s) : 0, out, cap, max);
}

/* The agent's own words, whole: sanitized with their line breaks, without
 * the blank lines at the end. */
static void emit_text(AgentctlSubRead *r, AgentctlEmit emit, void *ctx, AgentctlStyle st, const char *text, usize n) {
    if (!emit || !text || !n) return;
    if (n > 256u * 1024u) n = 256u * 1024u;
    usize cap = n * 3u + 1u;
    char *c = malloc(cap);
    if (!c) return;
    usize w = agentctl_sanitize(text, n, c, cap, true);
    while (w > 0 && (c[w - 1] == '\n' || c[w - 1] == ' ')) c[--w] = '\0';
    usize lead = 0;
    while (lead < w && c[lead] == '\n') lead++;
    if (w > lead) {
        emit(ctx, st, c + lead, w - lead);
        r->lines++;
    }
    free(c);
}

static void say(AgentctlSubRead *r, AgentctlEmit emit, void *ctx, AgentctlStyle st, const char *text) {
    if (!emit || !text || !text[0]) return;
    emit(ctx, st, text, strlen(text));
    r->lines++;
}

static void fmt_duration(i64 ms, char *out, usize cap) {
    if (ms < 0) ms = 0;
    i64 s = ms / 1000;
    if (s < 60)        snprintf(out, cap, "%llds", (long long)s);
    else if (s < 3600) snprintf(out, cap, "%lldm%02llds", (long long)(s / 60), (long long)(s % 60));
    else               snprintf(out, cap, "%lldh%02lldm", (long long)(s / 3600), (long long)((s / 60) % 60));
}

/* Days from 1970-01-01 to the civil date (proleptic Gregorian). */
static i64 days_from_civil(i64 y, i64 m, i64 d) {
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    i64 yoe = y - era * 400;
    i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static bool digits(const char *s, i32 n, i64 *out) {
    i64 v = 0;
    for (i32 i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

i64 agentctl_iso_ms(const char *s) {
    if (!s) return 0;
    i64 y, mo, d, h, mi, se;
    if (strlen(s) < 19 || !digits(s, 4, &y) || s[4] != '-' || !digits(s + 5, 2, &mo) || s[7] != '-' ||
        !digits(s + 8, 2, &d) || (s[10] != 'T' && s[10] != ' ') || !digits(s + 11, 2, &h) || s[13] != ':' ||
        !digits(s + 14, 2, &mi) || s[16] != ':' || !digits(s + 17, 2, &se))
        return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return 0;
    const char *p = s + 19;
    i64 ms = 0;
    if (*p == '.') {
        p++;
        i32 k = 0;
        while (*p >= '0' && *p <= '9') {
            if (k < 3) ms = ms * 10 + (*p - '0');
            k++;
            p++;
        }
        while (k > 0 && k < 3) { ms *= 10; k++; }
    }
    i64 off = 0;
    if (*p == '+' || *p == '-') {
        i64 oh = 0, om = 0;
        if (!digits(p + 1, 2, &oh)) return 0;
        const char *q = p + 3;
        if (*q == ':') q++;
        if (*q && !digits(q, 2, &om)) return 0;
        off = (oh * 60 + om) * 60 * (*p == '-' ? -1 : 1);
    }
    i64 secs = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se - off;
    return secs * 1000 + ms;
}

/* A number of seconds (Kimi's float timestamps) or of milliseconds, as ms. */
static i64 epoch_ms(double v) {
    if (v <= 0) return 0;
    if (v < 1e11) return (i64)(v * 1000.0);
    return (i64)v;
}

static void see_time(AgentctlSubRead *r, i64 ts) {
    if (ts <= 0) return;
    if (!r->start_ms || ts < r->start_ms) r->start_ms = ts;
    if (ts > r->last_ms) r->last_ms = ts;
}

static void set_model(AgentctlSubRead *r, const char *m) {
    if (!m || !m[0] || m[0] == '<') return;          /* "<synthetic>" is no model */
    agentctl_sanitize(m, strlen(m), r->model, sizeof r->model, false);
}

/* ---------------------------------------------------------------- task --- */

/* A message the subagent was given: the first line strong, up to `more` more
 * dim, then how many were left out. */
static void show_prompt(AgentctlSubRead *r, const char *text, usize n, i32 more, AgentctlEmit emit, void *ctx) {
    if (!text) return;
    const char *p = text, *end = text + n;
    while (p < end && (*p == '\n' || *p == ' ' || *p == '\r')) p++;
    i32 lines = 0;
    while (p < end && lines <= more) {
        const char *nl = memchr(p, '\n', (usize)(end - p));
        usize len = nl ? (usize)(nl - p) : (usize)(end - p);
        char clean[1024];
        one_line(p, len, clean, sizeof clean, 900);
        if (clean[0] || lines > 0) {
            emitf(emit, ctx, lines == 0 ? AGS_HEAD : AGS_DIM, "%s%s", lines == 0 ? "\xE2\x80\xBA " : "  ", clean);
            r->lines++;
            lines++;
        }
        if (!nl) { p = end; break; }
        p = nl + 1;
    }
    if (p < end) {
        i32 left = 1;
        for (const char *q = p; q < end; q++) if (*q == '\n') left++;
        emitf(emit, ctx, AGS_DIM, "  \xE2\x80\xA6 (%d more line%s)", left, left == 1 ? "" : "s");
        r->lines++;
    }
}

void agentctl_subread_task(AgentctlSubRead *r, const char *task, usize len, AgentctlEmit emit, void *ctx) {
    if (!r || r->task_shown || !task || !len) return;
    r->task_shown = true;
    const char *t = task, *end = task + len;
    while (t < end && (*t == '\n' || *t == ' ' || *t == '\r')) t++;
    const char *nl = memchr(t, '\n', (usize)(end - t));
    one_line(t, nl ? (usize)(nl - t) : (usize)(end - t), r->task_head, sizeof r->task_head, 150);
    show_prompt(r, task, len, 7, emit, ctx);
}

/* A later message (a follow-up the parent sent): shorter. */
static void prompt_or_task(AgentctlSubRead *r, const char *text, usize len, AgentctlEmit emit, void *ctx) {
    if (!text || !len) return;
    if (!r->task_shown) { agentctl_subread_task(r, text, len, emit, ctx); return; }
    show_prompt(r, text, len, 2, emit, ctx);
}

/* --------------------------------------------------------------- status --- */

void agentctl_subread_status(AgentctlSubRead *r, AgentctlSubStatus st, const char *why, i64 when_ms,
                             AgentctlEmit emit, void *ctx) {
    if (!r || st == AGSUB_UNKNOWN) return;
    if (st == AGSUB_RUNNING) {
        if (r->status != AGSUB_RUNNING) {
            r->status = AGSUB_RUNNING;
            r->settle_shown = false;
            r->end_ms = 0;
            r->error[0] = '\0';
        }
        return;
    }
    if (r->status == st && r->settle_shown) return;
    r->status = (u8)st;
    if (why && why[0]) agentctl_sanitize(why, strnlen(why, 4096), r->error, sizeof r->error, false);
    r->end_ms = when_ms > 0 ? when_ms : r->last_ms;
    if (r->settle_shown) return;
    r->settle_shown = true;
    char took[32] = "";
    if (r->start_ms > 0 && r->end_ms >= r->start_ms) fmt_duration(r->end_ms - r->start_ms, took, sizeof took);
    char tail[300] = "";
    if (st == AGSUB_FAILED && r->error[0]) one_line_z(r->error, tail, sizeof tail, 240);
    const char *word = st == AGSUB_DONE ? "done" : st == AGSUB_FAILED ? "failed" : "stopped";
    char line[600];
    snprintf(line, sizeof line, "\xE2\x94\x80\xE2\x94\x80 %s%s%s%s%s", word, took[0] ? " \xC2\xB7 " : "", took,
             tail[0] ? " \xC2\xB7 " : "", tail);
    say(r, emit, ctx, st == AGSUB_FAILED ? AGS_ERR : AGS_HEAD, line);
}

/* Something new in the transcript of one that had settled: it runs again. */
static void running(AgentctlSubRead *r) {
    if (r->status != AGSUB_RUNNING) agentctl_subread_status(r, AGSUB_RUNNING, NULL, 0, NULL, NULL);
}

void agentctl_subread_answer(AgentctlSubRead *r, const char *text, usize len, AgentctlEmit emit, void *ctx) {
    if (!r || !text || !len) return;
    emit_text(r, emit, ctx, AGS_TEXT, text, len);
}

/* ---------------------------------------------------------- tool calls --- */

static bool name_is(const char *name, const char *const *list) {
    for (; *list; list++) if (!strcasecmp(name, *list)) return true;
    return false;
}

static const char *const k_shell_tools[] = {
    "bash", "shell", "sh", "run_terminal_command", "run_shell_command", "exec_command", "execute_command",
    "run_command", "terminal", "command", "exec", "execute", "local_shell", NULL,
};
static const char *const k_edit_tools[] = {
    "edit", "write", "multiedit", "notebookedit", "edit_file", "write_file", "create_file", "apply_patch",
    "patch", "str_replace", "strreplacefile", "writefile", "replace", "search_replace", "str_replace_editor",
    "insert", "create", NULL,
};

/* The first of the keys `o` has as a string. */
static const char *first_str(const cJSON *o, const char *const *keys) {
    for (; *keys; keys++) {
        const char *v = jstr(o, *keys);
        if (v && v[0]) return v;
    }
    return NULL;
}

static const char *const k_path_keys[] = {
    "file_path", "filePath", "path", "notebook_path", "target_file", "filename", "file", NULL,
};
static const char *const k_subject_keys[] = {
    "file_path", "filePath", "path", "pattern", "query", "url", "description", "i", "intent", "prompt", NULL,
};

void agentctl_subread_call(const char *name, const char *args, usize args_len, AgentctlEmit emit, void *ctx) {
    if (!emit) return;
    char nm[96];
    one_line_z(name && name[0] ? name : "tool", nm, sizeof nm, 60);
    cJSON *in = NULL;
    if (args && args_len) {
        in = cJSON_ParseWithLength(args, args_len);
        if (cJSON_IsString(in) && in->valuestring) {        /* arguments handed over as a JSON string */
            cJSON *inner = cJSON_Parse(in->valuestring);
            cJSON_Delete(in);
            in = inner;
        }
    }
    const cJSON *o = cJSON_IsObject(in) ? in : NULL;
    char arg[512] = "";
    const char *cmd = o ? jstr(o, "command") : NULL;
    if (!cmd && o) cmd = jstr(o, "cmd");
    if (!cmd && o) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(o, "command");
        if (cJSON_IsArray(c)) {                             /* ["bash", "-lc", "…"] */
            const cJSON *last = NULL, *it;
            cJSON_ArrayForEach(it, c) last = it;
            if (cJSON_IsString(last)) cmd = last->valuestring;
        }
    }
    if (name && name_is(name, k_shell_tools) && cmd) {
        one_line_z(cmd, arg, sizeof arg, 240);
        emitf(emit, ctx, AGS_TOOL, "\xE2\x96\xB8 $ %s", arg);
    } else if (name && name_is(name, k_edit_tools)) {
        const char *p = o ? first_str(o, k_path_keys) : NULL;
        one_line_z(p ? p : nm, arg, sizeof arg, 240);
        emitf(emit, ctx, AGS_EDIT, "\xE2\x9C\x8E %s", arg);
    } else if (cmd && !o) {
        one_line_z(cmd, arg, sizeof arg, 240);
        emitf(emit, ctx, AGS_TOOL, "\xE2\x96\xB8 $ %s", arg);
    } else {
        const char *v = o ? first_str(o, k_subject_keys) : NULL;
        if (!v && cmd) v = cmd;
        one_line_z(v, arg, sizeof arg, 240);
        if (arg[0]) emitf(emit, ctx, AGS_TOOL, "\xE2\x9A\x99 %s %s", nm, arg);
        else        emitf(emit, ctx, AGS_TOOL, "\xE2\x9A\x99 %s", nm);
    }
    cJSON_Delete(in);
}

static void call_json(AgentctlSubRead *r, const char *name, const cJSON *args, AgentctlEmit emit, void *ctx) {
    char *text = NULL;
    const char *a = NULL;
    usize alen = 0;
    if (cJSON_IsString(args) && args->valuestring) {
        a = args->valuestring;
        alen = strlen(a);
    } else if (args) {
        text = cJSON_PrintUnformatted(args);
        a = text;
        alen = text ? strlen(text) : 0;
    }
    agentctl_subread_call(name, a, alen, emit, ctx);
    r->lines++;
    free(text);
}

static void tool_failed(AgentctlSubRead *r, const char *why, AgentctlEmit emit, void *ctx) {
    char line[320];
    one_line_z(why && why[0] ? why : "tool error", line, sizeof line, 240);
    emitf(emit, ctx, AGS_ERR, "  \xE2\x9C\x97 %s", line);
    r->lines++;
}

/* The text of a content value: a string, or the text parts of an array. */
static char *content_text(const cJSON *c, const char *text_key) {
    if (cJSON_IsString(c) && c->valuestring) return strdup(c->valuestring);
    if (!cJSON_IsArray(c)) return NULL;
    usize total = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, c) {
        const char *t = jstr(it, text_key);
        const char *ty = jstr(it, "type");
        if (t && (!ty || !strcasecmp(ty, "text") || !strcasecmp(ty, "input_text") || !strcasecmp(ty, "output_text")))
            total += strlen(t) + 1;
    }
    if (!total) return NULL;
    char *out = malloc(total + 1);
    if (!out) return NULL;
    usize w = 0;
    cJSON_ArrayForEach(it, c) {
        const char *t = jstr(it, text_key);
        const char *ty = jstr(it, "type");
        if (!t || (ty && strcasecmp(ty, "text") && strcasecmp(ty, "input_text") && strcasecmp(ty, "output_text")))
            continue;
        usize l = strlen(t);
        if (w && out[w - 1] != '\n') out[w++] = '\n';
        memcpy(out + w, t, l);
        w += l;
    }
    out[w] = '\0';
    return out;
}

/* ------------------------------------------------------------- Claude --- */

static void claude_line(AgentctlSubRead *r, const cJSON *root, const char *line, usize len,
                        AgentctlEmit emit, void *ctx) {
    const char *type = jstr(root, "type");
    if (!type) return;
    i64 ts = agentctl_iso_ms(jstr(root, "timestamp"));
    if (!strcmp(type, "user")) {
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "isMeta"))) return;
        const cJSON *msg = jobj(root, "message");
        const cJSON *content = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
        bool has_result = false;
        const cJSON *b;
        if (cJSON_IsArray(content))
            cJSON_ArrayForEach(b, content) {
                const char *bt = jstr(b, "type");
                if (bt && !strcmp(bt, "tool_result")) has_result = true;
            }
        see_time(r, ts);
        if (has_result) {
            agentctl_stream_line(&r->claude, line, len, emit, ctx);
            r->claude_answered = false;
            running(r);
            return;
        }
        char *text = content_text(content, "text");
        if (text) {
            if (!strncmp(text, "[Request interrupted", 20)) {
                agentctl_subread_status(r, AGSUB_STOPPED, NULL, ts, emit, ctx);
            } else {
                running(r);
                prompt_or_task(r, text, strlen(text), emit, ctx);
                r->claude_answered = false;
            }
            free(text);
        }
        return;
    }
    if (!strcmp(type, "assistant")) {
        see_time(r, ts);
        running(r);
        u32 before = r->claude.messages + r->claude.commands + r->claude.edits + r->claude.tools;
        agentctl_stream_line(&r->claude, line, len, emit, ctx);
        u32 after = r->claude.messages + r->claude.commands + r->claude.edits + r->claude.tools;
        r->lines += after - before;
        const cJSON *msg = jobj(root, "message");
        if (!msg) return;
        set_model(r, jstr(msg, "model"));
        bool calls = false;
        const cJSON *b;
        cJSON_ArrayForEach(b, jarr(msg, "content")) {
            const char *bt = jstr(b, "type");
            if (bt && !strcmp(bt, "tool_use")) calls = true;
        }
        const char *stop = jstr(msg, "stop_reason");
        if (calls) r->claude_answered = false;
        else if (stop && !strcmp(stop, "end_turn")) r->claude_answered = true;
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "isApiErrorMessage"))) {
            char *t = content_text(jarr(msg, "content"), "text");
            if (t) {
                agentctl_sanitize(t, strlen(t), r->error, sizeof r->error, false);
                free(t);
            }
        }
        const cJSON *u = jobj(msg, "usage");
        double in = 0, cr = 0, cc = 0, out = 0;
        if (u && jnum(u, "input_tokens", &in)) {
            jnum(u, "cache_read_input_tokens", &cr);
            jnum(u, "cache_creation_input_tokens", &cc);
            jnum(u, "output_tokens", &out);
            r->tok_in = (u64)(in + cr + cc);
            r->tok_out += (u64)out;
        }
    }
}

/* -------------------------------------------------------------- Codex --- */

/* A command as a person reads it: the line inside `<shell> -lc '…'`. */
static void codex_command(const cJSON *cmd, char *out, usize cap) {
    out[0] = '\0';
    if (cJSON_IsString(cmd) && cmd->valuestring) { one_line_z(cmd->valuestring, out, cap, 240); return; }
    if (!cJSON_IsArray(cmd)) return;
    i32 n = cJSON_GetArraySize(cmd);
    const cJSON *a1 = n >= 3 ? cJSON_GetArrayItem(cmd, 1) : NULL;
    const char *flag = cJSON_IsString(a1) ? a1->valuestring : NULL;
    if (flag && (!strcmp(flag, "-lc") || !strcmp(flag, "-c"))) {
        const cJSON *last = cJSON_GetArrayItem(cmd, n - 1);
        if (cJSON_IsString(last)) { one_line_z(last->valuestring, out, cap, 240); return; }
    }
    char joined[2048];
    usize w = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, cmd) {
        if (!cJSON_IsString(it)) continue;
        int k = snprintf(joined + w, sizeof joined - w, "%s%s", w ? " " : "", it->valuestring);
        if (k < 0 || (usize)k >= sizeof joined - w) break;
        w += (usize)k;
    }
    joined[w] = '\0';
    one_line_z(joined, out, cap, 240);
}

static void codex_changes(AgentctlSubRead *r, const cJSON *changes, AgentctlEmit emit, void *ctx) {
    char line[300];
    const cJSON *ch;
    if (cJSON_IsObject(changes)) {
        cJSON_ArrayForEach(ch, changes) {
            const char *kind = cJSON_IsObject(ch) ? jstr(ch, "type") : NULL;
            if (!kind && cJSON_IsObject(ch)) {
                const cJSON *first = ch->child;
                kind = first ? first->string : NULL;          /* {"add": {...}} */
            }
            one_line_z(ch->string, line, sizeof line, 240);
            const char *mark = kind && !strcasecmp(kind, "add") ? "+" : kind && !strcasecmp(kind, "delete") ? "-" : "~";
            emitf(emit, ctx, AGS_EDIT, "\xE2\x9C\x8E %s %s", mark, line);
            r->lines++;
        }
    } else if (cJSON_IsArray(changes)) {
        cJSON_ArrayForEach(ch, changes) {
            const char *kind = jstr(ch, "kind");
            const cJSON *kobj = jobj(ch, "kind");
            if (!kind && kobj) kind = jstr(kobj, "type");
            one_line_z(jstr(ch, "path"), line, sizeof line, 240);
            const char *mark = kind && !strcasecmp(kind, "add") ? "+" : kind && !strcasecmp(kind, "delete") ? "-" : "~";
            emitf(emit, ctx, AGS_EDIT, "\xE2\x9C\x8E %s %s", mark, line);
            r->lines++;
        }
    }
}

static void codex_exec_result(AgentctlSubRead *r, const cJSON *o, AgentctlEmit emit, void *ctx) {
    double code = 0;
    bool has_code = jnum(o, "exit_code", &code);
    const char *status = jstr(o, "status");
    bool failed = (has_code && code != 0) || (status && !strcasecmp(status, "failed"));
    if (failed) {
        if (has_code) emitf(emit, ctx, AGS_ERR, "  \xE2\x9C\x97 exit %.0f", code);
        else          emitf(emit, ctx, AGS_ERR, "  \xE2\x9C\x97 failed");
    } else if (status && !strcasecmp(status, "declined")) {
        emitf(emit, ctx, AGS_ERR, "  \xE2\x9C\x97 not allowed");
    } else {
        emitf(emit, ctx, AGS_OK, "  \xE2\x9C\x93 exit %.0f", has_code ? code : 0.0);
    }
    r->lines++;
}

static void codex_item(AgentctlSubRead *r, const cJSON *item, AgentctlEmit emit, void *ctx) {
    const char *it = jstr(item, "type");
    if (!it) return;
    char line[600];
    if (!strcasecmp(it, "CommandExecution") || !strcasecmp(it, "command_execution")) {
        codex_command(cJSON_GetObjectItemCaseSensitive(item, "command"), line, sizeof line);
        emitf(emit, ctx, AGS_TOOL, "\xE2\x96\xB8 $ %s", line);
        r->lines++;
        codex_exec_result(r, item, emit, ctx);
    } else if (!strcasecmp(it, "AgentMessage") || !strcasecmp(it, "agent_message")) {
        const char *t = jstr(item, "text");
        char *joined = t ? NULL : content_text(cJSON_GetObjectItemCaseSensitive(item, "content"), "text");
        const char *x = t ? t : joined;
        if (x) emit_text(r, emit, ctx, AGS_TEXT, x, strlen(x));
        free(joined);
    } else if (!strcasecmp(it, "FileChange") || !strcasecmp(it, "file_change")) {
        codex_changes(r, cJSON_GetObjectItemCaseSensitive(item, "changes"), emit, ctx);
    } else if (!strcasecmp(it, "Reasoning") || !strcasecmp(it, "reasoning")) {
        const cJSON *sum = cJSON_GetObjectItemCaseSensitive(item, "summary");
        char *t = content_text(sum, "text");
        if (!t && cJSON_IsArray(sum)) {
            const cJSON *first = cJSON_GetArrayItem(sum, 0);
            if (cJSON_IsString(first)) t = strdup(first->valuestring);
        }
        if (t) {
            one_line_z(t, line, sizeof line, 160);
            if (line[0]) { emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", line); r->lines++; }
            free(t);
        }
    } else if (!strcasecmp(it, "WebSearch") || !strcasecmp(it, "web_search")) {
        one_line_z(jstr(item, "query"), line, sizeof line, 200);
        emitf(emit, ctx, AGS_TOOL, "\xE2\x8C\x95 %s", line);
        r->lines++;
    } else if (!strcasecmp(it, "McpToolCall") || !strcasecmp(it, "mcp_tool_call")) {
        char server[128], tool[128];
        one_line_z(jstr(item, "server"), server, sizeof server, 60);
        one_line_z(jstr(item, "tool"), tool, sizeof tool, 60);
        emitf(emit, ctx, AGS_TOOL, "\xE2\x9A\x99 %s.%s", server, tool);
        r->lines++;
    } else if (!strcasecmp(it, "UserMessage") || !strcasecmp(it, "user_message")) {
        char *t = content_text(cJSON_GetObjectItemCaseSensitive(item, "content"), "text");
        if (t && t[0] != '<') prompt_or_task(r, t, strlen(t), emit, ctx);
        free(t);
    }
}

/* A message one agent of a team sends another (Codex's multi-agent v2): its
 * header is readable, its payload may be sealed. */
static void codex_agent_message(AgentctlSubRead *r, const cJSON *p, AgentctlEmit emit, void *ctx) {
    char *t = content_text(cJSON_GetObjectItemCaseSensitive(p, "content"), "text");
    const char *author = jstr(p, "author");
    const char *pay = t ? strstr(t, "Payload:") : NULL;
    const char *body = NULL;
    if (pay) {
        body = pay + 8;
        while (*body == ' ' || *body == '\n' || *body == '\r') body++;
    }
    if (body && body[0]) {
        prompt_or_task(r, body, strlen(body), emit, ctx);
    } else {
        char kind[64] = "a message";
        const char *mt = t ? strstr(t, "Message Type:") : NULL;
        if (mt) {
            mt += 13;
            while (*mt == ' ') mt++;
            usize k = 0;
            while (mt[k] && mt[k] != '\n' && k < 40) k++;
            if (k) {
                char raw[48];
                snprintf(raw, sizeof raw, "%.*s", (int)k, mt);
                for (char *c = raw; *c; c++) *c = *c == '_' ? ' ' : (char)((*c >= 'A' && *c <= 'Z') ? *c + 32 : *c);
                one_line_z(raw, kind, sizeof kind, 40);
            }
        }
        char who[96];
        one_line_z(author ? author : "its parent", who, sizeof who, 60);
        char head[200];
        snprintf(head, sizeof head, "%s from %s", kind, who);
        bool first = !r->task_shown;
        r->task_shown = true;
        emitf(emit, ctx, first ? AGS_HEAD : AGS_DIM, "\xE2\x80\xBA %s", head);
        r->lines++;
    }
    free(t);
}

static void codex_line(AgentctlSubRead *r, const cJSON *root, AgentctlEmit emit, void *ctx) {
    double ord = 0;
    if (r->ordinal_from && jnum(root, "ordinal", &ord) && ord >= 1 && ord < (double)r->ordinal_from) return;
    const char *type = jstr(root, "type");
    const cJSON *p = jobj(root, "payload");
    if (!type || !p) return;
    i64 ts = agentctl_iso_ms(jstr(root, "timestamp"));
    const char *pt = jstr(p, "type");
    if (!strcmp(type, "turn_context")) {
        set_model(r, jstr(p, "model"));
        return;
    }
    if (!strcmp(type, "response_item")) {
        if (!pt) return;
        if (!strcmp(pt, "message")) {
            const char *role = jstr(p, "role");
            if (!role || strcmp(role, "user") || r->task_shown) return;
            char *t = content_text(cJSON_GetObjectItemCaseSensitive(p, "content"), "text");
            if (t && t[0] && t[0] != '<') {
                see_time(r, ts);
                agentctl_subread_task(r, t, strlen(t), emit, ctx);
            }
            free(t);
        } else if (!strcmp(pt, "agent_message")) {
            see_time(r, ts);
            codex_agent_message(r, p, emit, ctx);
        }
        return;
    }
    if (strcmp(type, "event_msg") || !pt) return;
    see_time(r, ts);
    if (!strcmp(pt, "task_started")) {
        running(r);
    } else if (!strcmp(pt, "task_complete")) {
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(p, "error");
        const char *why = cJSON_IsObject(err) ? jstr(err, "message") : cJSON_IsString(err) ? err->valuestring : NULL;
        if (why || (cJSON_IsObject(err))) agentctl_subread_status(r, AGSUB_FAILED, why ? why : "the turn failed", ts, emit, ctx);
        else                               agentctl_subread_status(r, AGSUB_DONE, NULL, ts, emit, ctx);
    } else if (!strcmp(pt, "turn_aborted")) {
        agentctl_subread_status(r, AGSUB_STOPPED, NULL, ts, emit, ctx);
    } else if (!strcmp(pt, "item_completed")) {
        r->codex_items = true;
        running(r);
        const cJSON *item = jobj(p, "item");
        if (item) codex_item(r, item, emit, ctx);
    } else if (!strcmp(pt, "token_count")) {
        const cJSON *info = jobj(p, "info");
        const cJSON *tot = info ? jobj(info, "total_token_usage") : NULL;
        double in = 0, out = 0;
        if (tot && jnum(tot, "input_tokens", &in)) {
            jnum(tot, "output_tokens", &out);
            r->tok_in = (u64)in;
            r->tok_out = (u64)out;
        }
    } else if (!strcmp(pt, "error")) {
        const char *m = jstr(p, "message");
        if (m) {
            agentctl_sanitize(m, strlen(m), r->error, sizeof r->error, false);
            char line[320];
            one_line_z(m, line, sizeof line, 240);
            emitf(emit, ctx, AGS_ERR, "! %s", line);
            r->lines++;
        }
    } else if (!r->codex_items) {
        /* the events of a Codex that writes no item_completed */
        char line[600];
        if (!strcmp(pt, "user_message")) {
            const char *m = jstr(p, "message");
            if (m && m[0] != '<') prompt_or_task(r, m, strlen(m), emit, ctx);
        } else if (!strcmp(pt, "agent_message")) {
            const char *m = jstr(p, "message");
            if (m) emit_text(r, emit, ctx, AGS_TEXT, m, strlen(m));
        } else if (!strcmp(pt, "agent_reasoning")) {
            one_line_z(jstr(p, "text"), line, sizeof line, 160);
            if (line[0]) { emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", line); r->lines++; }
        } else if (!strcmp(pt, "exec_command_begin")) {
            codex_command(cJSON_GetObjectItemCaseSensitive(p, "command"), line, sizeof line);
            emitf(emit, ctx, AGS_TOOL, "\xE2\x96\xB8 $ %s", line);
            r->lines++;
        } else if (!strcmp(pt, "exec_command_end")) {
            codex_exec_result(r, p, emit, ctx);
        } else if (!strcmp(pt, "patch_apply_end")) {
            codex_changes(r, cJSON_GetObjectItemCaseSensitive(p, "changes"), emit, ctx);
            const cJSON *ok = cJSON_GetObjectItemCaseSensitive(p, "success");
            if (cJSON_IsFalse(ok)) tool_failed(r, "the patch did not apply", emit, ctx);
        }
    }
}

/* --------------------------------------------------------------- Grok --- */

static void grok_line(AgentctlSubRead *r, const cJSON *root, AgentctlEmit emit, void *ctx) {
    const char *type = jstr(root, "type");
    if (!type) return;
    if (!strcmp(type, "user")) {
        if (cJSON_GetObjectItemCaseSensitive(root, "synthetic_reason")) return;
        char *t = content_text(cJSON_GetObjectItemCaseSensitive(root, "content"), "text");
        if (t) {
            running(r);
            prompt_or_task(r, t, strlen(t), emit, ctx);
            free(t);
        }
    } else if (!strcmp(type, "assistant")) {
        running(r);
        set_model(r, jstr(root, "model_id"));
        char *t = content_text(cJSON_GetObjectItemCaseSensitive(root, "content"), "text");
        if (t) {
            emit_text(r, emit, ctx, AGS_TEXT, t, strlen(t));
            free(t);
        }
        const cJSON *c;
        cJSON_ArrayForEach(c, jarr(root, "tool_calls")) {
            const cJSON *fn = jobj(c, "function");
            const char *name = jstr(c, "name");
            const cJSON *args = cJSON_GetObjectItemCaseSensitive(c, "arguments");
            if (!name && fn) { name = jstr(fn, "name"); args = cJSON_GetObjectItemCaseSensitive(fn, "arguments"); }
            call_json(r, name, args, emit, ctx);
        }
    } else if (!strcmp(type, "tool_result")) {
        const char *t = jstr(root, "content");
        long code = 0;
        if (t && !strncmp(t, "exit: ", 6) && (code = strtol(t + 6, NULL, 10)) != 0) {
            emitf(emit, ctx, AGS_ERR, "  \xE2\x9C\x97 exit %ld", code);
            r->lines++;
        }
    } else if (!strcmp(type, "reasoning")) {
        char *t = content_text(cJSON_GetObjectItemCaseSensitive(root, "summary"), "text");
        if (!t) {
            const cJSON *sum = jarr(root, "summary");
            const cJSON *first = sum ? cJSON_GetArrayItem(sum, 0) : NULL;
            const char *x = first ? jstr(first, "text") : NULL;
            if (x) t = strdup(x);
        }
        if (t) {
            char line[200];
            one_line_z(t, line, sizeof line, 160);
            if (line[0]) { emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", line); r->lines++; }
            free(t);
        }
    }
}

/* --------------------------------------------------------------- Kimi --- */

static void kimi_line(AgentctlSubRead *r, const cJSON *root, AgentctlEmit emit, void *ctx) {
    const char *role = jstr(root, "role");
    if (!role) return;
    if (!strcmp(role, "user")) {
        char *t = content_text(cJSON_GetObjectItemCaseSensitive(root, "content"), "text");
        if (t) {
            running(r);
            prompt_or_task(r, t, strlen(t), emit, ctx);
            free(t);
        }
    } else if (!strcmp(role, "assistant")) {
        running(r);
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
        const cJSON *part, *parts = cJSON_IsArray(content) ? content : NULL;
        cJSON_ArrayForEach(part, parts) {
            const char *pt = jstr(part, "type");
            if (pt && !strcmp(pt, "think")) {
                char line[200];
                one_line_z(jstr(part, "think"), line, sizeof line, 160);
                if (line[0]) { emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", line); r->lines++; }
            }
        }
        char *t = content_text(content, "text");
        if (t) {
            emit_text(r, emit, ctx, AGS_TEXT, t, strlen(t));
            free(t);
        }
        const cJSON *c;
        cJSON_ArrayForEach(c, jarr(root, "tool_calls")) {
            const cJSON *fn = jobj(c, "function");
            call_json(r, fn ? jstr(fn, "name") : jstr(c, "name"),
                      fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : cJSON_GetObjectItemCaseSensitive(c, "arguments"),
                      emit, ctx);
        }
    } else if (!strcmp(role, "_usage")) {
        double n = 0;
        if (jnum(root, "token_count", &n)) r->tok_in = (u64)n;
    }
}

/* ----------------------------------------------------------- Oh My Pi --- */

static void omp_line(AgentctlSubRead *r, const cJSON *root, AgentctlEmit emit, void *ctx) {
    const char *type = jstr(root, "type");
    if (!type) return;
    i64 ts = agentctl_iso_ms(jstr(root, "timestamp"));
    if (!strcmp(type, "model_change")) {
        set_model(r, jstr(root, "model"));
        return;
    }
    if (!strcmp(type, "session_init")) {
        const char *agent = jstr(root, "agent");
        if (agent) one_line_z(agent, r->kind, sizeof r->kind, 60);
        return;
    }
    if (!strcmp(type, "custom")) {
        const char *ct = jstr(root, "customType");
        if (ct && !strcmp(ct, "session_exit")) {
            see_time(r, ts);
            const cJSON *d = jobj(root, "data");
            const char *kind = d ? jstr(d, "kind") : NULL;
            if (r->error[0]) agentctl_subread_status(r, AGSUB_FAILED, r->error, ts, emit, ctx);
            else if (kind && strcmp(kind, "normal")) agentctl_subread_status(r, AGSUB_STOPPED, NULL, ts, emit, ctx);
            else agentctl_subread_status(r, AGSUB_DONE, NULL, ts, emit, ctx);
        }
        return;
    }
    if (strcmp(type, "message")) return;
    const cJSON *msg = jobj(root, "message");
    const char *role = msg ? jstr(msg, "role") : NULL;
    if (!role) return;
    see_time(r, ts);
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
    if (!strcmp(role, "user")) {
        char *t = content_text(content, "text");
        if (t) {
            running(r);
            prompt_or_task(r, t, strlen(t), emit, ctx);
            free(t);
        }
    } else if (!strcmp(role, "assistant")) {
        running(r);
        set_model(r, jstr(msg, "model"));
        const cJSON *b, *blocks = cJSON_IsArray(content) ? content : NULL;
        cJSON_ArrayForEach(b, blocks) {
            const char *bt = jstr(b, "type");
            if (!bt) continue;
            if (!strcmp(bt, "text")) {
                const char *t = jstr(b, "text");
                if (t) emit_text(r, emit, ctx, AGS_TEXT, t, strlen(t));
            } else if (!strcmp(bt, "thinking")) {
                char line[200];
                one_line_z(jstr(b, "thinking"), line, sizeof line, 160);
                if (line[0]) { emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", line); r->lines++; }
            } else if (!strcmp(bt, "toolCall")) {
                call_json(r, jstr(b, "name"), cJSON_GetObjectItemCaseSensitive(b, "arguments"), emit, ctx);
            }
        }
        const char *stop = jstr(msg, "stopReason");
        const char *em = jstr(msg, "errorMessage");
        if (stop && !strcmp(stop, "error"))
            agentctl_sanitize(em ? em : "the model failed", strlen(em ? em : "the model failed"), r->error, sizeof r->error, false);
    } else if (!strcmp(role, "toolResult")) {
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(msg, "isError"))) {
            char *t = content_text(content, "text");
            tool_failed(r, t, emit, ctx);
            free(t);
        }
    }
}

/* ------------------------------------------------------------ OpenCode --- */

bool agentctl_subread_opencode_row(AgentctlSubRead *r, const char *type, const char *data, usize len,
                                   AgentctlEmit emit, void *ctx) {
    if (!r || !type) return true;
    if (strcmp(type, "user") && strcmp(type, "assistant") && strcmp(type, "idle")) return true;
    cJSON *root = data && len ? cJSON_ParseWithLength(data, len) : NULL;
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return true; }
    const cJSON *time = jobj(root, "time");
    double created = 0, completed = 0;
    if (time) { jnum(time, "created", &created); jnum(time, "completed", &completed); }
    if (!strcmp(type, "assistant") && completed <= 0) {
        bool failed = cJSON_GetObjectItemCaseSensitive(root, "error") != NULL;
        if (!failed) { cJSON_Delete(root); return false; }      /* still being written */
    }
    see_time(r, epoch_ms(created));
    if (completed > 0) see_time(r, epoch_ms(completed));
    if (!strcmp(type, "user")) {
        const char *t = jstr(root, "text");
        if (t) {
            running(r);
            prompt_or_task(r, t, strlen(t), emit, ctx);
        }
    } else if (!strcmp(type, "idle")) {
        const char *o = jstr(root, "outcome");
        if (o && !strcmp(o, "succeeded")) agentctl_subread_status(r, AGSUB_DONE, NULL, epoch_ms(created), emit, ctx);
        else if (o && !strcmp(o, "failed")) agentctl_subread_status(r, AGSUB_FAILED, r->error[0] ? r->error : "it failed", epoch_ms(created), emit, ctx);
        else if (o) agentctl_subread_status(r, AGSUB_STOPPED, NULL, epoch_ms(created), emit, ctx);
    } else {
        running(r);
        const cJSON *model = jobj(root, "model");
        if (model) set_model(r, jstr(model, "id"));
        const cJSON *b;
        cJSON_ArrayForEach(b, jarr(root, "content")) {
            const char *bt = jstr(b, "type");
            if (!bt) continue;
            if (!strcmp(bt, "text")) {
                const char *t = jstr(b, "text");
                if (t) emit_text(r, emit, ctx, AGS_TEXT, t, strlen(t));
            } else if (!strcmp(bt, "reasoning")) {
                char line[200];
                one_line_z(jstr(b, "text"), line, sizeof line, 160);
                if (line[0]) { emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", line); r->lines++; }
            } else if (!strcmp(bt, "tool")) {
                const cJSON *st = jobj(b, "state");
                call_json(r, jstr(b, "name"), st ? cJSON_GetObjectItemCaseSensitive(st, "input") : NULL, emit, ctx);
                const char *status = st ? jstr(st, "status") : NULL;
                if (status && !strcmp(status, "error")) {
                    const cJSON *e = cJSON_GetObjectItemCaseSensitive(st, "error");
                    const char *why = cJSON_IsString(e) ? e->valuestring : cJSON_IsObject(e) ? jstr(e, "message") : NULL;
                    tool_failed(r, why, emit, ctx);
                }
            }
        }
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (err) {
            const char *why = cJSON_IsString(err) ? err->valuestring : cJSON_IsObject(err) ? jstr(err, "message") : NULL;
            if (!why && cJSON_IsObject(err)) {
                const cJSON *d = jobj(err, "data");
                why = d ? jstr(d, "message") : NULL;
            }
            agentctl_sanitize(why ? why : "the model failed", strlen(why ? why : "the model failed"), r->error,
                              sizeof r->error, false);
            char line[320];
            one_line_z(r->error, line, sizeof line, 240);
            emitf(emit, ctx, AGS_ERR, "! %s", line);
            r->lines++;
        }
    }
    cJSON_Delete(root);
    return true;
}

/* ---------------------------------------------- Anthropic-shaped messages --- */

/* What Cline CLI wraps around a typed prompt: `<user_input mode="act">…</user_input>`. */
static void unwrap_user_input(char *t) {
    if (!t || strncmp(t, "<user_input", 11)) return;
    char *gt = strchr(t, '>');
    if (!gt) return;
    char *end = strstr(gt + 1, "</user_input>");
    usize n = end ? (usize)(end - (gt + 1)) : strlen(gt + 1);
    memmove(t, gt + 1, n);
    t[n] = '\0';
}

/* One message of the CLIs that keep Anthropic's shape (Droid, Cline CLI): a
 * role and a content that is a string or blocks of text, thinking, tool_use
 * and tool_result. A final answer — text and no call — marks it answered. */
static void anth_message(AgentctlSubRead *r, const char *role, const cJSON *content, i64 ts,
                         AgentctlEmit emit, void *ctx) {
    if (!role) return;
    see_time(r, ts);
    if (!strcmp(role, "user")) {
        bool result = false;
        const cJSON *b, *blocks = cJSON_IsArray(content) ? content : NULL;
        cJSON_ArrayForEach(b, blocks) {
            const char *bt = jstr(b, "type");
            if (!bt || strcmp(bt, "tool_result")) continue;
            result = true;
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "is_error"))) {
                char *t = content_text(cJSON_GetObjectItemCaseSensitive(b, "content"), "text");
                tool_failed(r, t, emit, ctx);
                free(t);
            }
        }
        if (result) { r->claude_answered = false; running(r); return; }
        char *t = content_text(content, "text");
        if (t) {
            unwrap_user_input(t);
            running(r);
            prompt_or_task(r, t, strlen(t), emit, ctx);
            r->claude_answered = false;
            free(t);
        }
        return;
    }
    if (strcmp(role, "assistant")) return;
    running(r);
    bool calls = false, said = false;
    if (cJSON_IsString(content) && content->valuestring && content->valuestring[0]) {
        emit_text(r, emit, ctx, AGS_TEXT, content->valuestring, strlen(content->valuestring));
        said = true;
    }
    const cJSON *b, *blocks = cJSON_IsArray(content) ? content : NULL;
    cJSON_ArrayForEach(b, blocks) {
        const char *bt = jstr(b, "type");
        if (!bt) continue;
        if (!strcmp(bt, "text")) {
            const char *t = jstr(b, "text");
            if (t && t[0]) { emit_text(r, emit, ctx, AGS_TEXT, t, strlen(t)); said = true; }
        } else if (!strcmp(bt, "thinking")) {
            char line[200];
            one_line_z(jstr(b, "thinking"), line, sizeof line, 160);
            if (line[0]) { emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", line); r->lines++; }
        } else if (!strcmp(bt, "tool_use")) {
            call_json(r, jstr(b, "name"), cJSON_GetObjectItemCaseSensitive(b, "input"), emit, ctx);
            calls = true;
        }
    }
    if (calls) r->claude_answered = false;
    else if (said) r->claude_answered = true;
}

/* -------------------------------------------------------------- Droid --- */

/* `{"type":"message","id":…,"timestamp":…,"message":{"role":…,"content":[…]}}`;
 * the first line is `session_start`. */
static void droid_line(AgentctlSubRead *r, const cJSON *root, AgentctlEmit emit, void *ctx) {
    const char *type = jstr(root, "type");
    if (!type || strcmp(type, "message")) return;
    const cJSON *msg = jobj(root, "message");
    if (!msg) return;
    set_model(r, jstr(msg, "model"));
    anth_message(r, jstr(msg, "role"), cJSON_GetObjectItemCaseSensitive(msg, "content"),
                 agentctl_iso_ms(jstr(root, "timestamp")), emit, ctx);
}

/* ------------------------------------------------------------- Cursor --- */

/* `{"role":…,"message":{"content":[…]}}`, a turn's end `{"type":"turn_ended",
 * "status":…}`. Cursor keeps no tool results in its transcripts. */
static void cursor_line(AgentctlSubRead *r, const cJSON *root, AgentctlEmit emit, void *ctx) {
    const char *type = jstr(root, "type");
    if (type && !strcmp(type, "turn_ended")) {
        const char *st = jstr(root, "status");
        const char *why = jstr(root, "error");
        if (st && !strcmp(st, "error")) {
            bool user = why && strstr(why, "aborted");
            agentctl_subread_status(r, user ? AGSUB_STOPPED : AGSUB_FAILED, user ? NULL : why, 0, emit, ctx);
        } else if (st) {
            agentctl_subread_status(r, AGSUB_DONE, NULL, 0, emit, ctx);
        }
        return;
    }
    const cJSON *msg = jobj(root, "message");
    if (!msg) return;
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
    const char *role = jstr(root, "role");
    if (role && !strcmp(role, "user")) {
        /* the request sits inside <user_query>…</user_query> */
        char *t = content_text(content, "text");
        if (t) {
            char *q = strstr(t, "<user_query>");
            char *e = q ? strstr(q, "</user_query>") : NULL;
            if (q && e) {
                *e = '\0';
                memmove(t, q + 12, strlen(q + 12) + 1);
            }
            running(r);
            prompt_or_task(r, t, strlen(t), emit, ctx);
            r->claude_answered = false;
            free(t);
        }
        return;
    }
    anth_message(r, role, content, 0, emit, ctx);
}

/* --------------------------------------------------------------- Qwen --- */

/* Qwen Code's records: `{"type":"user"|"assistant"|"tool_result"|"system",
 * "message":{"role":…,"parts":[{text}|{text,thought}|{functionCall}|
 * {functionResponse}]},"toolCallResult":{…},"timestamp":…}`. */
static void qwen_line(AgentctlSubRead *r, const cJSON *root, AgentctlEmit emit, void *ctx) {
    const char *type = jstr(root, "type");
    if (!type) return;
    i64 ts = agentctl_iso_ms(jstr(root, "timestamp"));
    const cJSON *msg = jobj(root, "message");
    const cJSON *parts = msg ? jarr(msg, "parts") : NULL;
    const cJSON *p;
    if (!strcmp(type, "user")) {
        see_time(r, ts);
        char *t = content_text(parts, "text");
        if (t) {
            running(r);
            prompt_or_task(r, t, strlen(t), emit, ctx);
            r->claude_answered = false;
            free(t);
        }
    } else if (!strcmp(type, "assistant")) {
        see_time(r, ts);
        running(r);
        set_model(r, jstr(root, "model"));
        bool calls = false, said = false;
        cJSON_ArrayForEach(p, parts) {
            const cJSON *fc = jobj(p, "functionCall");
            const char *t = jstr(p, "text");
            if (fc) {
                call_json(r, jstr(fc, "name"), cJSON_GetObjectItemCaseSensitive(fc, "args"), emit, ctx);
                calls = true;
            } else if (t && t[0] && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "thought"))) {
                char line[200];
                one_line_z(t, line, sizeof line, 160);
                if (line[0]) { emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", line); r->lines++; }
            } else if (t && t[0]) {
                emit_text(r, emit, ctx, AGS_TEXT, t, strlen(t));
                said = true;
            }
        }
        if (calls) r->claude_answered = false;
        else if (said) r->claude_answered = true;
        const cJSON *u = jobj(root, "usageMetadata");
        double in = 0, out = 0;
        if (u && jnum(u, "promptTokenCount", &in)) {
            r->tok_in = (u64)in;
            if (jnum(u, "candidatesTokenCount", &out)) r->tok_out += (u64)out;
        }
    } else if (!strcmp(type, "tool_result")) {
        see_time(r, ts);
        const cJSON *tcr = jobj(root, "toolCallResult");
        const char *st = tcr ? jstr(tcr, "status") : NULL;
        if (st && !strcmp(st, "error")) {
            const char *why = NULL;
            cJSON_ArrayForEach(p, parts) {
                const cJSON *fr = jobj(p, "functionResponse");
                const cJSON *resp = fr ? jobj(fr, "response") : NULL;
                if (resp && (why = jstr(resp, "error")) != NULL) break;
            }
            tool_failed(r, why, emit, ctx);
        }
        r->claude_answered = false;
    } else if (!strcmp(type, "system")) {
        const char *sub = jstr(root, "subtype");
        const cJSON *pl = jobj(root, "systemPayload");
        const char *t = pl ? jstr(pl, "displayText") : NULL;
        if (sub && !strcmp(sub, "agent_launch_prompt") && t) prompt_or_task(r, t, strlen(t), emit, ctx);
    }
}

/* ------------------------------------------------------------ Copilot --- */

/* One event of a subagent's, out of its parent's events.jsonl:
 * `{"type":…,"data":{…},"timestamp":…}`. */
static void copilot_line(AgentctlSubRead *r, const cJSON *root, AgentctlEmit emit, void *ctx) {
    const char *type = jstr(root, "type");
    const cJSON *d = jobj(root, "data");
    if (!type || !d) return;
    see_time(r, agentctl_iso_ms(jstr(root, "timestamp")));
    set_model(r, jstr(d, "model"));
    if (!strcmp(type, "user.message")) {
        const char *t = jstr(d, "content");
        if (t) { running(r); prompt_or_task(r, t, strlen(t), emit, ctx); }
    } else if (!strcmp(type, "assistant.message")) {
        running(r);
        const char *t = jstr(d, "content");
        if (t && t[0]) emit_text(r, emit, ctx, AGS_TEXT, t, strlen(t));
    } else if (!strcmp(type, "assistant.reasoning")) {
        char line[200];
        one_line_z(jstr(d, "content"), line, sizeof line, 160);
        if (line[0]) { emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", line); r->lines++; }
    } else if (!strcmp(type, "tool.execution_start")) {
        running(r);
        call_json(r, jstr(d, "toolName"), cJSON_GetObjectItemCaseSensitive(d, "arguments"), emit, ctx);
    } else if (!strcmp(type, "tool.execution_complete")) {
        const cJSON *ok = cJSON_GetObjectItemCaseSensitive(d, "success");
        if (cJSON_IsFalse(ok)) {
            const cJSON *e = cJSON_GetObjectItemCaseSensitive(d, "error");
            tool_failed(r, cJSON_IsString(e) ? e->valuestring : e ? jstr(e, "message") : NULL, emit, ctx);
        }
    }
}

/* ---------------------------------------------------------- the reader --- */

void agentctl_subread_init(AgentctlSubRead *r, i32 tool) {
    memset(r, 0, sizeof *r);
    r->tool = tool;
    r->status = AGSUB_RUNNING;
    if (tool == CHAT_TOOL_CLAUDE || tool == CHAT_TOOL_QODER) {
        agentctl_stream_init(&r->claude, AGENTCTL_AGENT_CLAUDE);
        r->claude_on = true;
    }
}

void agentctl_subread_free(AgentctlSubRead *r) {
    if (!r) return;
    if (r->claude_on) agentctl_stream_free(&r->claude);
    r->claude_on = false;
}

void agentctl_subread_line(AgentctlSubRead *r, const char *line, usize len, AgentctlEmit emit, void *ctx) {
    if (!r || !line) return;
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    if (!len || len > AGENTCTL_STREAM_LINE_MAX) return;
    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return; }
    switch (r->tool) {
    case CHAT_TOOL_CLAUDE:
    case CHAT_TOOL_QODER:  claude_line(r, root, line, len, emit, ctx); break;
    case CHAT_TOOL_CURSOR: cursor_line(r, root, emit, ctx); break;
    case CHAT_TOOL_CODEX:  codex_line(r, root, emit, ctx); break;
    case CHAT_TOOL_XAI:    grok_line(r, root, emit, ctx); break;
    case CHAT_TOOL_KIMI:   kimi_line(r, root, emit, ctx); break;
    case CHAT_TOOL_OMP:
    case CHAT_TOOL_OPENCLAW: omp_line(r, root, emit, ctx); break;
    case CHAT_TOOL_QWEN:   qwen_line(r, root, emit, ctx); break;
    case CHAT_TOOL_DROID:  droid_line(r, root, emit, ctx); break;
    case CHAT_TOOL_COPILOT: copilot_line(r, root, emit, ctx); break;
    default: break;
    }
    cJSON_Delete(root);
}

/* ------------------------------------------------------- database rows --- */

bool agentctl_subread_crush_row(AgentctlSubRead *r, const char *role, const char *parts, usize len, bool finished,
                                i64 ts_ms, AgentctlEmit emit, void *ctx) {
    if (!r || !role) return true;
    cJSON *root = parts && len ? cJSON_ParseWithLength(parts, len) : NULL;
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); return true; }
    const cJSON *p;
    const char *reason = NULL;
    cJSON_ArrayForEach(p, root) {
        const char *ty = jstr(p, "type");
        const cJSON *d = jobj(p, "data");
        if (ty && d && !strcmp(ty, "finish")) reason = jstr(d, "reason");
    }
    if (!strcmp(role, "assistant") && !finished && !reason) { cJSON_Delete(root); return false; }
    see_time(r, ts_ms);
    bool calls = false, said = false;
    cJSON_ArrayForEach(p, root) {
        const char *ty = jstr(p, "type");
        const cJSON *d = jobj(p, "data");
        if (!ty || !d) continue;
        if (!strcmp(ty, "text")) {
            const char *t = jstr(d, "text");
            if (!t || !t[0] || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "hidden"))) continue;
            if (!strcmp(role, "user")) {
                running(r);
                prompt_or_task(r, t, strlen(t), emit, ctx);
                r->claude_answered = false;
            } else if (!strcmp(role, "assistant")) {
                running(r);
                emit_text(r, emit, ctx, AGS_TEXT, t, strlen(t));
                said = true;
            }
        } else if (!strcmp(ty, "reasoning")) {
            char line[200];
            one_line_z(jstr(d, "thinking"), line, sizeof line, 160);
            if (line[0]) { emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", line); r->lines++; }
        } else if (!strcmp(ty, "tool_call")) {
            running(r);
            call_json(r, jstr(d, "name"), cJSON_GetObjectItemCaseSensitive(d, "input"), emit, ctx);
            calls = true;
        } else if (!strcmp(ty, "tool_result")) {
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "is_error"))) tool_failed(r, jstr(d, "content"), emit, ctx);
            r->claude_answered = false;
        }
    }
    if (!strcmp(role, "assistant")) {
        if (calls) r->claude_answered = false;
        else if (said) r->claude_answered = true;
        if (reason && (!strcmp(reason, "error") || !strcmp(reason, "permission_denied"))) {
            const cJSON *f = NULL;
            cJSON_ArrayForEach(p, root) {
                const char *ty = jstr(p, "type");
                if (ty && !strcmp(ty, "finish")) f = jobj(p, "data");
            }
            const char *why = f ? jstr(f, "message") : NULL;
            if (!why || !why[0]) why = !strcmp(reason, "error") ? "the model failed" : "permission denied";
            agentctl_subread_status(r, AGSUB_FAILED, why, ts_ms, emit, ctx);
        } else if (reason && !strcmp(reason, "canceled")) {
            agentctl_subread_status(r, AGSUB_STOPPED, NULL, ts_ms, emit, ctx);
        }
    }
    cJSON_Delete(root);
    return true;
}

void agentctl_subread_chat_row(AgentctlSubRead *r, const char *role, const char *content, const char *tool_calls,
                               i64 ts_ms, AgentctlEmit emit, void *ctx) {
    if (!r || !role) return;
    see_time(r, ts_ms);
    if (!strcmp(role, "user")) {
        if (content && content[0]) {
            running(r);
            prompt_or_task(r, content, strlen(content), emit, ctx);
            r->claude_answered = false;
        }
        return;
    }
    if (strcmp(role, "assistant")) return;
    running(r);
    bool calls = false, said = false;
    if (content && content[0]) {
        emit_text(r, emit, ctx, AGS_TEXT, content, strlen(content));
        said = true;
    }
    cJSON *tc = tool_calls && tool_calls[0] ? cJSON_Parse(tool_calls) : NULL;
    const cJSON *c, *calls_arr = cJSON_IsArray(tc) ? tc : NULL;
    cJSON_ArrayForEach(c, calls_arr) {
        const cJSON *fn = jobj(c, "function");
        call_json(r, fn ? jstr(fn, "name") : jstr(c, "name"),
                  fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : cJSON_GetObjectItemCaseSensitive(c, "arguments"),
                  emit, ctx);
        calls = true;
    }
    cJSON_Delete(tc);
    if (calls) r->claude_answered = false;
    else if (said) r->claude_answered = true;
}

u32 agentctl_subread_cline_messages(AgentctlSubRead *r, const char *json, usize len, u32 from,
                                    AgentctlEmit emit, void *ctx) {
    if (!r || !json || !len) return from;
    cJSON *root = cJSON_ParseWithLength(json, len);
    const cJSON *msgs = root ? jarr(root, "messages") : NULL;
    if (!msgs) { cJSON_Delete(root); return from; }
    u32 n = (u32)cJSON_GetArraySize(msgs);
    if (n < from) from = 0;                          /* rewritten shorter: a new run */
    u32 i = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, msgs) {
        if (i++ < from) continue;
        const cJSON *ts = cJSON_GetObjectItemCaseSensitive(m, "ts");
        i64 at = cJSON_IsNumber(ts) ? epoch_ms(ts->valuedouble)
               : cJSON_IsString(ts) && ts->valuestring ? epoch_ms(strtod(ts->valuestring, NULL)) : 0;
        set_model(r, jstr(m, "model"));
        anth_message(r, jstr(m, "role"), cJSON_GetObjectItemCaseSensitive(m, "content"), at, emit, ctx);
    }
    cJSON_Delete(root);
    return n;
}

/* ------------------------------------------------------------- records --- */

static void copy_clean(char *dst, usize cap, const char *src, usize max) {
    if (!src) { if (cap) dst[0] = '\0'; return; }
    one_line_z(src, dst, cap, max);
}

static u8 status_word(const char *s) {
    if (!s) return AGSUB_UNKNOWN;
    if (!strcasecmp(s, "completed") || !strcasecmp(s, "succeeded") || !strcasecmp(s, "done") ||
        !strcasecmp(s, "success") || !strcasecmp(s, "finished"))
        return AGSUB_DONE;
    if (!strcasecmp(s, "failed") || !strcasecmp(s, "error") || !strcasecmp(s, "errored")) return AGSUB_FAILED;
    if (!strcasecmp(s, "cancelled") || !strcasecmp(s, "canceled") || !strcasecmp(s, "killed") ||
        !strcasecmp(s, "interrupted") || !strcasecmp(s, "stopped") || !strcasecmp(s, "aborted"))
        return AGSUB_STOPPED;
    if (!strncasecmp(s, "running", 7) || !strcasecmp(s, "pending") || !strcasecmp(s, "started") ||
        !strcasecmp(s, "in_progress") || !strcasecmp(s, "queued"))
        return AGSUB_RUNNING;
    return AGSUB_UNKNOWN;
}

bool agentctl_submeta_claude(const char *json, usize len, AgentctlSubMeta *m) {
    memset(m, 0, sizeof *m);
    cJSON *root = json && len ? cJSON_ParseWithLength(json, len) : NULL;
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    copy_clean(m->label, sizeof m->label, jstr(root, "description"), 150);
    copy_clean(m->kind, sizeof m->kind, jstr(root, "agentType"), 60);
    copy_clean(m->parent, sizeof m->parent, jstr(root, "toolUseId"), 200);
    const char *model = jstr(root, "model");
    if (model && strcmp(model, "inherit")) copy_clean(m->model, sizeof m->model, model, 90);
    double depth = 0;
    if (jnum(root, "spawnDepth", &depth)) m->depth = (i32)depth;
    const cJSON *fin = cJSON_GetObjectItemCaseSensitive(root, "finished");
    if (cJSON_IsTrue(fin)) m->status = AGSUB_DONE;
    const char *st = jstr(root, "status");
    if (st && status_word(st) != AGSUB_UNKNOWN) m->status = status_word(st);
    cJSON_Delete(root);
    return true;
}

bool agentctl_submeta_grok(const char *json, usize len, AgentctlSubMeta *m) {
    memset(m, 0, sizeof *m);
    cJSON *root = json && len ? cJSON_ParseWithLength(json, len) : NULL;
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    const char *id = jstr(root, "child_session_id");
    if (!id) id = jstr(root, "subagent_id");
    copy_clean(m->id, sizeof m->id, id, 120);
    copy_clean(m->parent, sizeof m->parent, jstr(root, "parent_session_id"), 200);
    copy_clean(m->label, sizeof m->label, jstr(root, "description"), 150);
    copy_clean(m->kind, sizeof m->kind, jstr(root, "subagent_type"), 60);
    m->status = status_word(jstr(root, "status"));
    m->start_ms = agentctl_iso_ms(jstr(root, "started_at"));
    m->end_ms = agentctl_iso_ms(jstr(root, "completed_at"));
    cJSON_Delete(root);
    return m->id[0] != '\0';
}

bool agentctl_submeta_kimi(const char *json, usize len, AgentctlSubMeta *m) {
    memset(m, 0, sizeof *m);
    cJSON *root = json && len ? cJSON_ParseWithLength(json, len) : NULL;
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    copy_clean(m->id, sizeof m->id, jstr(root, "agent_id"), 120);
    copy_clean(m->label, sizeof m->label, jstr(root, "description"), 150);
    copy_clean(m->kind, sizeof m->kind, jstr(root, "subagent_type"), 60);
    const char *st = jstr(root, "status");
    /* "idle" is a subagent between two tasks: its last one is over */
    m->status = st && !strcmp(st, "idle") ? AGSUB_DONE : status_word(st);
    double v = 0;
    if (jnum(root, "created_at", &v)) m->start_ms = epoch_ms(v);
    if (m->status != AGSUB_RUNNING && jnum(root, "updated_at", &v)) m->end_ms = epoch_ms(v);
    const cJSON *spec = jobj(root, "launch_spec");
    if (spec) copy_clean(m->model, sizeof m->model, jstr(spec, "effective_model"), 90);
    cJSON_Delete(root);
    return m->id[0] != '\0';
}

bool agentctl_submeta_codex(const char *line, usize len, AgentctlSubMeta *m) {
    memset(m, 0, sizeof *m);
    cJSON *root = line && len ? cJSON_ParseWithLength(line, len) : NULL;
    const char *type = root ? jstr(root, "type") : NULL;
    const cJSON *p = root ? jobj(root, "payload") : NULL;
    if (!type || strcmp(type, "session_meta") || !p) { cJSON_Delete(root); return false; }
    const char *parent = jstr(p, "parent_thread_id");
    const cJSON *src = jobj(p, "source");
    const cJSON *sub = src ? jobj(src, "subagent") : NULL;
    const cJSON *spawn = sub ? jobj(sub, "thread_spawn") : NULL;
    if (!parent && spawn) parent = jstr(spawn, "parent_thread_id");
    if (!parent || !parent[0]) { cJSON_Delete(root); return false; }
    copy_clean(m->id, sizeof m->id, jstr(p, "id"), 120);
    copy_clean(m->parent, sizeof m->parent, parent, 200);
    const char *nick = jstr(p, "agent_nickname");
    const char *path = jstr(p, "agent_path");
    if (!nick && spawn) nick = jstr(spawn, "agent_nickname");
    if (!path && spawn) path = jstr(spawn, "agent_path");
    const char *role = spawn ? jstr(spawn, "agent_role") : NULL;
    const char *other = sub ? jstr(sub, "other") : NULL;         /* {"other": "guardian"} */
    const char *tsrc = jstr(p, "thread_source");
    if (!path && !nick && tsrc && strcmp(tsrc, "subagent")) path = tsrc;     /* "guardian_review" */
    if (!role && !nick) role = other;
    const char *leaf = path ? strrchr(path, '/') : NULL;
    leaf = leaf ? leaf + 1 : path;
    char lab[160];
    if (leaf && leaf[0]) {
        snprintf(lab, sizeof lab, "%s", leaf);
        for (char *c = lab; *c; c++) if (*c == '_') *c = ' ';
        copy_clean(m->label, sizeof m->label, lab, 150);
    } else if (nick) {
        copy_clean(m->label, sizeof m->label, nick, 150);
    }
    copy_clean(m->kind, sizeof m->kind, role ? role : nick, 60);
    double depth = 0;
    if (spawn && jnum(spawn, "depth", &depth)) m->depth = (i32)depth;
    double ord = 0;
    if (jnum(p, "subagent_history_start_ordinal", &ord) && ord > 0) m->ordinal_from = (u32)ord;
    m->start_ms = agentctl_iso_ms(jstr(p, "timestamp"));
    cJSON_Delete(root);
    return m->id[0] != '\0';
}

bool agentctl_submeta_omp(const char *line, usize len, AgentctlSubMeta *m) {
    memset(m, 0, sizeof *m);
    cJSON *root = line && len ? cJSON_ParseWithLength(line, len) : NULL;
    const char *type = root ? jstr(root, "type") : NULL;
    const char *parent = root ? jstr(root, "parentSession") : NULL;
    if (!type || strcmp(type, "session") || !parent || !parent[0]) { cJSON_Delete(root); return false; }
    copy_clean(m->id, sizeof m->id, jstr(root, "id"), 120);
    snprintf(m->parent, sizeof m->parent, "%s", parent);
    m->start_ms = agentctl_iso_ms(jstr(root, "timestamp"));
    cJSON_Delete(root);
    return true;
}

/* ------------------------------------------------------- Command Code --- */

void agentctl_subread_cc_line(const char *line, usize len, AgentctlCcFn fn, void *ctx) {
    if (!line || !fn) return;
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    if (!len || len > AGENTCTL_STREAM_LINE_MAX) return;
    /* only a line that holds a call of the agent tool or a result is parsed */
    if (!memmem(line, len, "\"tool_use\"", 10) && !memmem(line, len, "\"tool_result\"", 13)) return;
    cJSON *root = cJSON_ParseWithLength(line, len);
    const cJSON *msg = root ? jobj(root, "message") : NULL;
    if (!msg) { cJSON_Delete(root); return; }
    i64 ts = agentctl_iso_ms(jstr(root, "timestamp"));
    const cJSON *b;
    cJSON_ArrayForEach(b, jarr(msg, "content")) {
        const char *bt = jstr(b, "type");
        if (!bt) continue;
        AgentctlCcEvent e;
        memset(&e, 0, sizeof e);
        e.ts_ms = ts;
        if (!strcmp(bt, "tool_use")) {
            const char *name = jstr(b, "name");
            const cJSON *in = jobj(b, "input");
            if (!name || strcmp(name, "agent") || !in) continue;
            e.kind = AGCC_SPAWN;
            e.call_id = jstr(b, "id");
            e.label = jstr(in, "description");
            e.agent_type = jstr(in, "subagent_type");
            e.text = jstr(in, "prompt");
            e.text_len = e.text ? strlen(e.text) : 0;
            e.background = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(in, "run_in_background"));
            if (e.call_id) fn(ctx, &e);
        } else if (!strcmp(bt, "tool_result")) {
            e.kind = AGCC_RESULT;
            e.call_id = jstr(b, "tool_use_id");
            e.failed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "is_error"));
            char *t = content_text(cJSON_GetObjectItemCaseSensitive(b, "content"), "text");
            e.text = t;
            e.text_len = t ? strlen(t) : 0;
            if (e.call_id) fn(ctx, &e);
            free(t);
        }
    }
    cJSON_Delete(root);
}

/* The value after `key` on a line of its own ("agent_id: bg-1-…"). */
static bool field_line(const char *text, usize len, const char *key, char *out, usize cap) {
    usize kl = strlen(key);
    const char *p = text, *end = text + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (usize)(end - p));
        const char *le = nl ? nl : end;
        if ((usize)(le - p) > kl && !memcmp(p, key, kl)) {
            const char *v = p + kl;
            while (v < le && *v == ' ') v++;
            usize n = (usize)(le - v);
            while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\r')) n--;
            if (n == 0 || n >= cap) return false;
            memcpy(out, v, n);
            out[n] = '\0';
            return true;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

bool agentctl_subread_cc_background(const char *text, usize len, char *bg_id, usize bg_cap,
                                    u8 *status, const char **report, usize *report_len) {
    if (report) *report = NULL;
    if (report_len) *report_len = 0;
    if (!text || !len || !bg_id || !bg_cap) return false;
    char st[64];
    if (!field_line(text, len, "agent_id:", bg_id, bg_cap)) return false;
    if (len >= 26 && !memcmp(text, "Background agent launched.", 26)) {
        if (status) *status = AGSUB_RUNNING;
        return true;
    }
    if (!field_line(text, len, "status:", st, sizeof st)) return false;
    u8 s = status_word(st);
    if (s == AGSUB_UNKNOWN) return false;
    if (status) *status = s;
    /* the report follows the first blank line */
    const char *blank = NULL;
    for (usize i = 0; i + 1 < len; i++)
        if (text[i] == '\n' && text[i + 1] == '\n') { blank = text + i + 2; break; }
    if (blank && report) {
        *report = blank;
        if (report_len) *report_len = (usize)(text + len - blank);
    }
    return true;
}

/* ------------------------------------------------- more agents' records --- */

bool agentctl_submeta_qwen(const char *json, usize len, AgentctlSubMeta *m) {
    memset(m, 0, sizeof *m);
    cJSON *root = json && len ? cJSON_ParseWithLength(json, len) : NULL;
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    copy_clean(m->id, sizeof m->id, jstr(root, "agentId"), 120);
    copy_clean(m->label, sizeof m->label, jstr(root, "description"), 150);
    const char *kind = jstr(root, "subagentName");
    copy_clean(m->kind, sizeof m->kind, kind ? kind : jstr(root, "agentType"), 60);
    copy_clean(m->parent, sizeof m->parent, jstr(root, "parentSessionId"), 200);
    copy_clean(m->model, sizeof m->model, jstr(root, "model"), 90);
    m->status = status_word(jstr(root, "status"));
    if (m->status == AGSUB_UNKNOWN && jstr(root, "status") && !strcmp(jstr(root, "status"), "paused"))
        m->status = AGSUB_STOPPED;
    m->start_ms = agentctl_iso_ms(jstr(root, "createdAt"));
    if (m->status != AGSUB_RUNNING) m->end_ms = agentctl_iso_ms(jstr(root, "lastUpdatedAt"));
    double depth = 0;
    if (jnum(root, "depth", &depth)) m->depth = (i32)depth;
    cJSON_Delete(root);
    return m->id[0] != '\0';
}

bool agentctl_submeta_qwen_error(const char *json, usize len, char *out, usize cap) {
    if (!out || !cap) return false;
    out[0] = '\0';
    cJSON *root = json && len ? cJSON_ParseWithLength(json, len) : NULL;
    const char *err = root ? jstr(root, "lastError") : NULL;
    if (err && err[0]) agentctl_sanitize(err, strlen(err), out, cap, false);
    cJSON_Delete(root);
    return out[0] != '\0';
}

bool agentctl_submeta_droid(const char *line, usize len, AgentctlSubMeta *m) {
    memset(m, 0, sizeof *m);
    cJSON *root = line && len ? cJSON_ParseWithLength(line, len) : NULL;
    const char *type = root ? jstr(root, "type") : NULL;
    if (!type || strcmp(type, "session_start")) { cJSON_Delete(root); return false; }
    copy_clean(m->id, sizeof m->id, jstr(root, "id"), 120);
    copy_clean(m->parent, sizeof m->parent, jstr(root, "callingSessionId"), 200);
    copy_clean(m->label, sizeof m->label, jstr(root, "title"), 150);
    cJSON_Delete(root);
    return m->id[0] != '\0';
}

/* ------------------------------------------- subagents in the parent's log --- */

/* The first of `keys` that `o` has as a string. */
static const char *any_str(const cJSON *o, const char *const *keys) {
    return o ? first_str(o, keys) : NULL;
}

static const char *const k_label_keys[] = { "description", "title", "name", "task", "prompt", NULL };
static const char *const k_kind_keys[] = {
    "subagent_type", "agent_type", "agentType", "profile", "profile_name", "subagent_profile", "agent", NULL,
};
static const char *const k_task_keys[] = { "prompt", "task", "instructions", "message", "goal", NULL };

/* A call's arguments as an object: an object, or a string holding one. */
static cJSON *args_object(const cJSON *a, bool *owned) {
    *owned = false;
    if (cJSON_IsObject(a)) return (cJSON *)a;
    if (cJSON_IsString(a) && a->valuestring) {
        cJSON *o = cJSON_Parse(a->valuestring);
        if (cJSON_IsObject(o)) { *owned = true; return o; }
        cJSON_Delete(o);
    }
    return NULL;
}

/* Pi's `subagent` extension: one call runs one agent, several in parallel
 * (`tasks`) or a chain (`chain`), each a `pi` process with no session of its
 * own; the call's result carries every run's messages in `details.results`.
 * A run of several is shown as one subagent each, `<call id>#<index>`. */
void agentctl_subread_pi_line(const char *line, usize len, AgentctlCcFn fn, void *ctx) {
    if (!line || !fn) return;
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    if (!len || len > AGENTCTL_STREAM_LINE_MAX || !memmem(line, len, "\"subagent\"", 10)) return;
    cJSON *root = cJSON_ParseWithLength(line, len);
    const char *type = root ? jstr(root, "type") : NULL;
    const cJSON *msg = type && !strcmp(type, "message") ? jobj(root, "message") : NULL;
    const char *role = msg ? jstr(msg, "role") : NULL;
    if (!role) { cJSON_Delete(root); return; }
    i64 ts = agentctl_iso_ms(jstr(root, "timestamp"));
    char id[160];
    if (!strcmp(role, "assistant")) {
        const cJSON *b;
        cJSON_ArrayForEach(b, jarr(msg, "content")) {
            const char *bt = jstr(b, "type"), *name = jstr(b, "name"), *cid = jstr(b, "id");
            if (!bt || strcmp(bt, "toolCall") || !name || strcmp(name, "subagent") || !cid) continue;
            bool owned;
            cJSON *a = args_object(cJSON_GetObjectItemCaseSensitive(b, "arguments"), &owned);
            if (!a) continue;
            const cJSON *list = jarr(a, "tasks");
            if (!list) list = jarr(a, "chain");
            AgentctlCcEvent e;
            if (list) {
                i32 i = 0;
                const cJSON *t;
                cJSON_ArrayForEach(t, list) {
                    memset(&e, 0, sizeof e);
                    snprintf(id, sizeof id, "%s#%d", cid, i++);
                    e.kind = AGCC_SPAWN;
                    e.call_id = id;
                    e.ts_ms = ts;
                    e.label = jstr(t, "task");
                    e.agent_type = jstr(t, "agent");
                    e.text = e.label;
                    e.text_len = e.text ? strlen(e.text) : 0;
                    fn(ctx, &e);
                }
            } else if (jstr(a, "task")) {
                memset(&e, 0, sizeof e);
                e.kind = AGCC_SPAWN;
                e.call_id = cid;
                e.ts_ms = ts;
                e.label = jstr(a, "task");
                e.agent_type = jstr(a, "agent");
                e.text = e.label;
                e.text_len = strlen(e.text);
                fn(ctx, &e);
            }
            if (owned) cJSON_Delete(a);
        }
    } else if (!strcmp(role, "toolResult")) {
        const char *tn = jstr(msg, "toolName"), *cid = jstr(msg, "toolCallId");
        if (!tn || strcmp(tn, "subagent") || !cid) { cJSON_Delete(root); return; }
        const cJSON *d = jobj(msg, "details");
        const cJSON *results = d ? jarr(d, "results") : NULL;
        const char *mode = d ? jstr(d, "mode") : NULL;
        bool single = !mode || !strcmp(mode, "single");
        i32 i = 0;
        const cJSON *res;
        cJSON_ArrayForEach(res, results) {
            AgentctlCcEvent e;
            memset(&e, 0, sizeof e);
            if (single) snprintf(id, sizeof id, "%s", cid);
            else snprintf(id, sizeof id, "%s#%d", cid, i);
            i++;
            char *printed = cJSON_PrintUnformatted(res);
            e.kind = AGCC_RESULT;
            e.call_id = id;
            e.ts_ms = ts;
            e.line = printed;
            e.line_len = printed ? strlen(printed) : 0;
            fn(ctx, &e);
            free(printed);
        }
        if (!results && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(msg, "isError"))) {
            AgentctlCcEvent e;
            memset(&e, 0, sizeof e);
            char *t = content_text(cJSON_GetObjectItemCaseSensitive(msg, "content"), "text");
            e.kind = AGCC_RESULT;
            e.call_id = cid;
            e.ts_ms = ts;
            e.failed = true;
            e.text = t;
            e.text_len = t ? strlen(t) : 0;
            fn(ctx, &e);
            free(t);
        }
    }
    cJSON_Delete(root);
}

void agentctl_subread_pi_result(AgentctlSubRead *r, const char *json, usize len, i64 ts_ms,
                                AgentctlEmit emit, void *ctx) {
    if (!r || !json || !len) return;
    cJSON *res = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(res)) { cJSON_Delete(res); return; }
    set_model(r, jstr(res, "model"));
    const cJSON *m;
    cJSON_ArrayForEach(m, jarr(res, "messages")) {
        /* each one as the session line it would be */
        cJSON *wrap = cJSON_CreateObject();
        if (!wrap) break;
        cJSON_AddStringToObject(wrap, "type", "message");
        cJSON_AddItemReferenceToObject(wrap, "message", (cJSON *)m);
        const char *role = jstr(m, "role");
        if (!role || strcmp(role, "user")) omp_line(r, wrap, emit, ctx);
        cJSON_Delete(wrap);
    }
    double code = 0;
    bool has_code = jnum(res, "exitCode", &code);
    const char *stop = jstr(res, "stopReason");
    const char *why = jstr(res, "errorMessage");
    if (!why || !why[0]) why = jstr(res, "stderr");
    if (stop && !strcmp(stop, "aborted")) agentctl_subread_status(r, AGSUB_STOPPED, NULL, ts_ms, emit, ctx);
    else if ((has_code && code != 0) || (stop && !strcmp(stop, "error")))
        agentctl_subread_status(r, AGSUB_FAILED, why && why[0] ? why : "it failed", ts_ms, emit, ctx);
    else agentctl_subread_status(r, AGSUB_DONE, NULL, ts_ms, emit, ctx);
    cJSON_Delete(res);
}

static bool copilot_spawn_tool(const char *name) {
    static const char *const k[] = { "task", "agent", "subagent", "run_subagent", "delegate", NULL };
    return name && name_is(name, k);
}

/* GitHub Copilot CLI: `<session-state>/<id>/events.jsonl`. A subagent is
 * `subagent.started` (its `toolCallId` the parent's call), its own events
 * carry that call as `data.parentToolCallId` or its instance as `agentId`,
 * and `subagent.completed` / `subagent.failed` settle it. */
void agentctl_subread_copilot_line(const char *line, usize len, AgentctlCcFn fn, void *ctx) {
    if (!line || !fn) return;
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    if (!len || len > AGENTCTL_STREAM_LINE_MAX) return;
    cJSON *root = cJSON_ParseWithLength(line, len);
    const char *type = root ? jstr(root, "type") : NULL;
    const cJSON *d = root ? jobj(root, "data") : NULL;
    if (!type || !d) { cJSON_Delete(root); return; }
    AgentctlCcEvent e;
    memset(&e, 0, sizeof e);
    e.ts_ms = agentctl_iso_ms(jstr(root, "timestamp"));
    const char *agent = jstr(root, "agentId");
    const char *ptc = jstr(d, "parentToolCallId");
    if (!strcmp(type, "subagent.started")) {
        e.kind = AGCC_SPAWN;
        e.call_id = jstr(d, "toolCallId");
        e.label = jstr(d, "agentDisplayName");
        e.agent_type = jstr(d, "agentName");
        e.model = jstr(d, "model");
        e.background = jstr(d, "executionMode") && !strcmp(jstr(d, "executionMode"), "background");
    } else if (!strcmp(type, "subagent.completed")) {
        e.kind = AGCC_SETTLE;
        e.call_id = jstr(d, "toolCallId");
        e.status = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "cancelled")) ? AGSUB_STOPPED : AGSUB_DONE;
    } else if (!strcmp(type, "subagent.failed")) {
        e.kind = AGCC_SETTLE;
        e.call_id = jstr(d, "toolCallId");
        e.status = AGSUB_FAILED;
        e.text = jstr(d, "error");
        e.text_len = e.text ? strlen(e.text) : 0;
    } else if (agent || ptc) {
        e.kind = AGCC_CHILD;
        e.call_id = ptc;
        e.agent_id = agent;
        e.line = line;
        e.line_len = len;
    } else if (!strcmp(type, "tool.execution_start") && copilot_spawn_tool(jstr(d, "toolName"))) {
        bool owned;
        cJSON *a = args_object(cJSON_GetObjectItemCaseSensitive(d, "arguments"), &owned);
        e.kind = AGCC_SPAWN;
        e.call_id = jstr(d, "toolCallId");
        e.label = any_str(a, k_label_keys);
        e.agent_type = any_str(a, k_kind_keys);
        e.text = any_str(a, k_task_keys);
        e.text_len = e.text ? strlen(e.text) : 0;
        if (e.call_id && a && e.text) fn(ctx, &e);
        if (owned) cJSON_Delete(a);
        cJSON_Delete(root);
        return;
    } else if (!strcmp(type, "tool.execution_complete")) {
        e.kind = AGCC_RESULT;
        e.call_id = jstr(d, "toolCallId");
        e.failed = cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(d, "success"));
    } else {
        cJSON_Delete(root);
        return;
    }
    if (e.call_id || e.agent_id) fn(ctx, &e);
    cJSON_Delete(root);
}

/* Devin for Terminal: a row of `message_nodes` (`chat_message`); a subagent
 * is a `run_subagent` call of the main conversation, settled by its result. */
void agentctl_subread_devin_row(const char *json, usize len, i64 ts_ms, AgentctlCcFn fn, void *ctx) {
    if (!json || !len || !fn) return;
    if (!memmem(json, len, "run_subagent", 12) && !memmem(json, len, "\"tool\"", 6)) return;
    cJSON *root = cJSON_ParseWithLength(json, len);
    const char *role = root ? jstr(root, "role") : NULL;
    if (!role) { cJSON_Delete(root); return; }
    AgentctlCcEvent e;
    if (!strcmp(role, "assistant")) {
        const cJSON *c;
        cJSON_ArrayForEach(c, jarr(root, "tool_calls")) {
            const cJSON *fnc = jobj(c, "function");
            const char *name = jstr(c, "name");
            if (!name) name = jstr(c, "tool_name");
            if (!name && fnc) name = jstr(fnc, "name");
            if (!name || strcmp(name, "run_subagent")) continue;
            const char *id = jstr(c, "id");
            if (!id) id = jstr(c, "tool_call_id");
            if (!id) id = jstr(c, "call_id");
            const cJSON *raw = cJSON_GetObjectItemCaseSensitive(c, "arguments");
            if (!raw) raw = cJSON_GetObjectItemCaseSensitive(c, "args");
            if (!raw) raw = cJSON_GetObjectItemCaseSensitive(c, "input");
            if (!raw) raw = cJSON_GetObjectItemCaseSensitive(c, "tool_args");
            if (!raw && fnc) raw = cJSON_GetObjectItemCaseSensitive(fnc, "arguments");
            bool owned;
            cJSON *a = args_object(raw, &owned);
            memset(&e, 0, sizeof e);
            e.kind = AGCC_SPAWN;
            e.call_id = id;
            e.ts_ms = ts_ms;
            e.label = any_str(a, k_label_keys);
            e.agent_type = any_str(a, k_kind_keys);
            e.text = any_str(a, k_task_keys);
            e.text_len = e.text ? strlen(e.text) : 0;
            e.background = a && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a, "is_background"));
            if (e.call_id) fn(ctx, &e);
            if (owned) cJSON_Delete(a);
        }
    } else if (!strcmp(role, "tool")) {
        const char *id = jstr(root, "tool_call_id");
        if (id) {
            memset(&e, 0, sizeof e);
            char *t = content_text(cJSON_GetObjectItemCaseSensitive(root, "content"), "text");
            e.kind = AGCC_RESULT;
            e.call_id = id;
            e.ts_ms = ts_ms;
            e.text = t;
            e.text_len = t ? strlen(t) : 0;
            const cJSON *md = jobj(root, "metadata");
            e.failed = md && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(md, "success"));
            fn(ctx, &e);
            free(t);
        }
    }
    cJSON_Delete(root);
}

bool agentctl_subread_copilot_start(const char *line, usize len, char *cwd, usize cap) {
    if (!cwd || !cap) return false;
    cwd[0] = '\0';
    cJSON *root = line && len ? cJSON_ParseWithLength(line, len) : NULL;
    const char *type = root ? jstr(root, "type") : NULL;
    const cJSON *d = root ? jobj(root, "data") : NULL;
    if (!type || strcmp(type, "session.start") || !d) { cJSON_Delete(root); return false; }
    const cJSON *c = jobj(d, "context");
    const char *v = c ? jstr(c, "cwd") : NULL;
    if (!v) v = jstr(d, "cwd");
    if (v) snprintf(cwd, cap, "%s", v);
    cJSON_Delete(root);
    return cwd[0] != '\0';
}

/* ------------------------------------------------------- OpenCode 1.x --- */

bool agentctl_subread_oc1_message(AgentctlSubRead *r, const char *data, usize len, char *role, usize cap,
                                  AgentctlEmit emit, void *ctx) {
    if (role && cap) role[0] = '\0';
    if (!r || !data || !len) return true;
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return true; }
    const char *ro = jstr(root, "role");
    if (ro && role && cap) snprintf(role, cap, "%s", ro);
    const cJSON *tm = jobj(root, "time");
    double created = 0, completed = 0;
    if (tm) { jnum(tm, "created", &created); jnum(tm, "completed", &completed); }
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (ro && !strcmp(ro, "assistant") && completed <= 0 && !err) { cJSON_Delete(root); return false; }
    see_time(r, epoch_ms(created));
    if (completed > 0) see_time(r, epoch_ms(completed));
    const char *model = jstr(root, "modelID");
    set_model(r, model);
    if (err) {
        const char *name = jstr(err, "name");
        const cJSON *d = jobj(err, "data");
        const char *why = d ? jstr(d, "message") : NULL;
        if (name && !strcmp(name, "MessageAbortedError")) {
            agentctl_subread_status(r, AGSUB_STOPPED, NULL, epoch_ms(completed), emit, ctx);
        } else {
            if (!why) why = name ? name : "the model failed";
            agentctl_subread_status(r, AGSUB_FAILED, why, epoch_ms(completed), emit, ctx);
        }
    }
    cJSON_Delete(root);
    return true;
}

void agentctl_subread_oc1_part(AgentctlSubRead *r, const char *role, const char *data, usize len,
                               AgentctlEmit emit, void *ctx) {
    if (!r || !role || !data || !len) return;
    cJSON *p = cJSON_ParseWithLength(data, len);
    const char *ty = p ? jstr(p, "type") : NULL;
    if (!ty) { cJSON_Delete(p); return; }
    if (!strcmp(ty, "text")) {
        const char *t = jstr(p, "text");
        bool synthetic = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "synthetic"));
        if (t && t[0] && !synthetic) {
            running(r);
            if (!strcmp(role, "user")) {
                prompt_or_task(r, t, strlen(t), emit, ctx);
                r->claude_answered = false;
            } else {
                emit_text(r, emit, ctx, AGS_TEXT, t, strlen(t));
                r->claude_answered = true;
            }
        }
    } else if (!strcmp(ty, "reasoning")) {
        char line[200];
        one_line_z(jstr(p, "text"), line, sizeof line, 160);
        if (line[0]) { emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", line); r->lines++; }
    } else if (!strcmp(ty, "tool")) {
        running(r);
        const cJSON *st = jobj(p, "state");
        call_json(r, jstr(p, "tool"), st ? cJSON_GetObjectItemCaseSensitive(st, "input") : NULL, emit, ctx);
        const char *status = st ? jstr(st, "status") : NULL;
        if (status && !strcmp(status, "error")) tool_failed(r, jstr(st, "error"), emit, ctx);
        r->claude_answered = false;
    }
    cJSON_Delete(p);
}

/* ----------------------------------------------------------- Amazon Q --- */

bool agentctl_submeta_amazonq(const char *json, usize len, AgentctlSubMeta *m) {
    memset(m, 0, sizeof *m);
    cJSON *root = json && len ? cJSON_ParseWithLength(json, len) : NULL;
    const char *agent = root ? jstr(root, "agent") : NULL;
    double launched = 0, done = 0;
    if (!agent || !agent[0] || !jnum(root, "launched_at", &launched)) { cJSON_Delete(root); return false; }
    snprintf(m->id, sizeof m->id, "%.80s@%.0f", agent, launched);
    copy_clean(m->kind, sizeof m->kind, agent, 60);
    const char *task = jstr(root, "task");
    copy_clean(m->label, sizeof m->label, task, 150);
    m->status = status_word(jstr(root, "status"));
    m->start_ms = epoch_ms(launched);
    if (jnum(root, "completed_at", &done)) m->end_ms = epoch_ms(done);
    cJSON_Delete(root);
    return true;
}

void agentctl_subread_amazonq(AgentctlSubRead *r, const char *json, usize len, AgentctlEmit emit, void *ctx) {
    if (!r || !json || !len) return;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return; }
    const char *task = jstr(root, "task");
    if (task) agentctl_subread_task(r, task, strlen(task), emit, ctx);
    double launched = 0, done = 0;
    if (jnum(root, "launched_at", &launched)) see_time(r, epoch_ms(launched));
    u8 st = status_word(jstr(root, "status"));
    if (st == AGSUB_DONE || st == AGSUB_FAILED) {
        if (jnum(root, "completed_at", &done)) see_time(r, epoch_ms(done));
        const char *out = jstr(root, "summary");
        if (!out || !out[0]) out = jstr(root, "output");
        if (out && out[0] && !r->settle_shown) emit_text(r, emit, ctx, AGS_TEXT, out, strlen(out));
        double code = 0;
        char why[64] = "";
        if (st == AGSUB_FAILED && jnum(root, "exit_code", &code)) snprintf(why, sizeof why, "exit %d", (int)code);
        agentctl_subread_status(r, (AgentctlSubStatus)st, why[0] ? why : NULL, epoch_ms(done), emit, ctx);
    }
    cJSON_Delete(root);
}
