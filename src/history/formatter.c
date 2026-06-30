/*
 * liu-history - ChatEvent streaming formatter (text / markdown / json).
 *
 * When stdout is a TTY, the text and markdown paths strip non-print C0 bytes
 * from tool results. Tool results can carry raw shell output with escape
 * sequences; passing them through would let a session file corrupt a
 * reviewer's terminal.
 */
#include "history/formatter.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static bool role_enabled(u32 mask, ChatRole r) {
    return (mask & (1u << r)) != 0;
}

static void iso_local(i64 ms, char *out, usize cap) {
    if (ms > 0) {
        time_t s = (time_t)(ms / 1000);
        struct tm tm;
        if (gmtime_r(&s, &tm)) {
            strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
            return;
        }
    }
    snprintf(out, cap, "-");
}

/* Strip C0 control chars (except \t, \n) from `s` into `out[cap]`, stopping
 * at NUL.  Returns bytes written (excl NUL). */
static usize strip_controls(char *out, usize cap, const char *s) {
    if (cap == 0) return 0;
    usize o = 0;
    for (usize i = 0; s && s[i] && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t' || c == '\n' || c >= 0x20) {
            out[o++] = s[i];
        }
    }
    out[o] = '\0';
    return o;
}

static void json_escape_and_print(const char *s) {
    if (!s) { fputs("null", stdout); return; }
    fputc('"', stdout);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '"')       fputs("\\\"", stdout);
        else if (c == '\\') fputs("\\\\", stdout);
        else if (c == '\b') fputs("\\b",  stdout);
        else if (c == '\f') fputs("\\f",  stdout);
        else if (c == '\n') fputs("\\n",  stdout);
        else if (c == '\r') fputs("\\r",  stdout);
        else if (c == '\t') fputs("\\t",  stdout);
        else if (c < 0x20)  printf("\\u%04x", c);
        else                fputc((int)c, stdout);
    }
    fputc('"', stdout);
}

/* ------------------------------------------------------------------------ */

void hist_formatter_init(HistFormatter *f, HistFormat fmt, u32 role_mask,
                         bool is_tty) {
    f->fmt           = fmt;
    f->role_mask     = role_mask ? role_mask : ROLE_MASK_ALL;
    f->is_tty_stdout = is_tty;
    f->event_index   = 0;
}

void hist_formatter_begin(HistFormatter *f) {
    if (f->fmt == HIST_FMT_JSON) fputc('[', stdout);
}

void hist_formatter_emit(HistFormatter *f, const ChatEvent *e) {
    if (!e || !role_enabled(f->role_mask, e->role)) return;

    char ts[40]; iso_local(e->timestamp_ms, ts, sizeof ts);
    const char *role = chat_role_name(e->role);
    const char *icon = chat_role_icon(e->role);
    const char *tool = chat_tool_name(e->origin_tool);
    const char *sanitized = e->text;
    char scratch[65536];
    if (f->is_tty_stdout && e->text &&
        (e->role == CHAT_ROLE_TOOL_RESULT || e->role == CHAT_ROLE_TOOL_USE)) {
        strip_controls(scratch, sizeof scratch, e->text);
        sanitized = scratch;
    }

    switch (f->fmt) {
    case HIST_FMT_TEXT: {
        if (e->role == CHAT_ROLE_TOOL_USE || e->role == CHAT_ROLE_TOOL_RESULT) {
            printf("%s %s[%s] %s\n  %s\n",
                   icon, role, e->tool_name ? e->tool_name : "-",
                   ts,
                   sanitized ? sanitized : "");
        } else {
            printf("%s %s %s\n%s\n\n",
                   icon, role, ts,
                   sanitized ? sanitized : "");
        }
        break;
    }
    case HIST_FMT_MARKDOWN: {
        printf("## %s `%s`  \n", icon, role);
        printf("_%s • %s_\n\n", tool, ts);
        if (e->tool_name && *e->tool_name)
            printf("**Tool:** `%s`\n\n", e->tool_name);
        if (sanitized) {
            if (e->role == CHAT_ROLE_TOOL_USE || e->role == CHAT_ROLE_TOOL_RESULT) {
                printf("```\n%s\n```\n\n", sanitized);
            } else {
                printf("%s\n\n", sanitized);
            }
        }
        break;
    }
    case HIST_FMT_JSON: {
        if (f->event_index > 0) fputc(',', stdout);
        fputs("{\"tool\":",  stdout); json_escape_and_print(tool);
        fputs(",\"role\":",  stdout); json_escape_and_print(role);
        if (e->tool_name) { fputs(",\"tool_name\":", stdout); json_escape_and_print(e->tool_name); }
        if (e->timestamp_ms) printf(",\"timestamp_ms\":%lld", (long long)e->timestamp_ms);
        if (e->session_id) { fputs(",\"session_id\":", stdout); json_escape_and_print(e->session_id); }
        fputs(",\"text\":", stdout); json_escape_and_print(e->text);
        fputc('}', stdout);
        break;
    }
    }
    f->event_index++;
}

void hist_formatter_finish(HistFormatter *f) {
    if (f->fmt == HIST_FMT_JSON) { fputs("]\n", stdout); }
}
