/*
 * liu-history - ChatEvent streaming formatter (text / markdown / json).
 *
 * When stdout is a TTY, the text and markdown paths strip C0 control bytes and
 * DEL from EVERYTHING they print — tool results, user and assistant text, and
 * the tool-name header. A session file is attacker-writable content: it holds
 * pasted terminal output (ESC and all), LLM output that echoes a hostile page,
 * and tool names an MCP server chose. A user or assistant turn carrying
 * `ESC ] 7;file://...` or `ESC [ 2 J` would otherwise reach the reviewer's
 * terminal through `liu-history`, and an OSC 7 or a title set from a session
 * file is a terminal-injection vector.
 */
#include "history/formatter.h"
#include "core/checked.h"

#include <stdio.h>
#include <stdlib.h>
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

/* Stream sanitized tool text in fixed scratch space. Large events need no
 * second full copy, and allocation failure cannot expose raw control bytes. */
static void print_text(const char *s, bool sanitize) {
    if (!s) return;
    if (!sanitize) { fputs(s, stdout); return; }
    char out[4096];
    usize used = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\t' || c == '\n' || (c >= 0x20 && c != 0x7f)) {
            out[used++] = *s;
            if (used == sizeof out) { fwrite(out, 1, used, stdout); used = 0; }
        }
    }
    if (used) fwrite(out, 1, used, stdout);
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
    bool sanitize = f->is_tty_stdout;   /* every role: see the header comment */

    switch (f->fmt) {
    case HIST_FMT_TEXT: {
        if (e->role == CHAT_ROLE_TOOL_USE || e->role == CHAT_ROLE_TOOL_RESULT) {
            printf("%s %s[", icon, role);
            print_text(e->tool_name ? e->tool_name : "-", sanitize);
            printf("] %s\n  ", ts);
            print_text(e->text, sanitize);
            fputc('\n', stdout);
        } else {
            printf("%s %s %s\n", icon, role, ts);
            print_text(e->text, sanitize);
            fputs("\n\n", stdout);
        }
        break;
    }
    case HIST_FMT_MARKDOWN: {
        printf("## %s `%s`  \n", icon, role);
        printf("_%s • %s_\n\n", tool, ts);
        if (e->tool_name && *e->tool_name) {
            fputs("**Tool:** `", stdout);
            print_text(e->tool_name, sanitize);
            fputs("`\n\n", stdout);
        }
        if (e->text) {
            if (e->role == CHAT_ROLE_TOOL_USE || e->role == CHAT_ROLE_TOOL_RESULT) {
                fputs("```\n", stdout);
                print_text(e->text, sanitize);
                fputs("\n```\n\n", stdout);
            } else {
                print_text(e->text, sanitize);
                fputs("\n\n", stdout);
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
