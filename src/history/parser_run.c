/*
 * liu-history - file driver: read a JSONL file line-by-line, dispatch to the
 * per-tool parser, hand events to the user callback.
 *
 * The arena is reset after every line so peak memory stays flat even for 4+
 * MB session files with 1000+ records.
 */
#include "history/parser.h"
#include "history/event.h"
#include "history/session_title.h"
#include "history/util.h"
#include "core/memory.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HIST_LINE_CAP (4u * 1024u * 1024u)
#define HIST_EVENTS_PER_LINE 32u

static u32 dispatch(Arena *a, ChatTool tool, const char *line, usize len,
                    ChatEvent *out, u32 cap) {
    switch (tool) {
        case CHAT_TOOL_CLAUDE:      return parser_claude_line     (a, line, len, out, cap);
        case CHAT_TOOL_CODEX:       return parser_codex_line      (a, line, len, out, cap);
        case CHAT_TOOL_COPILOT:     return parser_copilot_line    (a, line, len, out, cap);
        case CHAT_TOOL_ANTIGRAVITY: return parser_antigravity_line(a, line, len, out, cap);
        case CHAT_TOOL_XAI:         return parser_grok_line       (a, line, len, out, cap);
        case CHAT_TOOL_COMMANDCODE: return parser_commandcode_line(a, line, len, out, cap);
        case CHAT_TOOL_PI:
        case CHAT_TOOL_OMP:         return parser_pi_line         (a, tool, line, len, out, cap);
        default: return 0;
    }
}

/* The line-per-record tools. `max_lines` / `max_bytes` (0 = no limit) stop the
 * read after that much of the file whatever the callback says; a line without
 * the bytes `needle` (NULL = every line) is counted and not parsed; `st` (may
 * be NULL) is told how far it went. */
static u64 run_lines(Arena *a, ChatTool tool, const char *path, chat_event_cb cb, void *user,
                     u32 max_lines, u64 max_bytes, const char *needle, ChatTitleStats *st) {
    /* nofollow: a symlink under ~/.claude/projects/ must not be able to
     * redirect the read to an arbitrary location. The reader also caps its
     * buffer at HIST_LINE_CAP+1, so an oversized line is skipped without
     * ever being allocated in full. */
    HistLineReader rd;
    if (!hist_line_reader_open(&rd, path, true)) {
        if (errno != ELOOP)
            fprintf(stderr, "liu-history: open %s: %s\n", path, strerror(errno));
        return 0;
    }

    u64 total_events = 0, bytes = 0;
    u32 lines = 0;
    ChatEvent events[HIST_EVENTS_PER_LINE];

    for (;;) {
        if ((max_lines && lines >= max_lines) || (max_bytes && bytes >= max_bytes)) break;
        const char *line = NULL;
        usize nread = 0;
        HistLineResult r = hist_line_reader_next(&rd, HIST_LINE_CAP,
                                                 &line, &nread);
        if (r == HIST_LINE_EOF || r == HIST_LINE_IO_ERROR) break;
        lines++;
        if (r == HIST_LINE_TOO_LONG) { bytes += HIST_LINE_CAP; continue; } /* pathological; resync */
        bytes += nread + 1;
        if (nread == 0) continue;
        if (needle && !memmem(line, nread, needle, strlen(needle))) continue;

        arena_reset(a);
        u32 n = dispatch(a, tool, line, nread, events, HIST_EVENTS_PER_LINE);
        for (u32 i = 0; i < n; i++) {
            total_events++;
            if (!cb(&events[i], user)) { goto done; }
        }
    }

done:
    hist_line_reader_close(&rd);
    if (st) { st->lines = lines; st->bytes = bytes; st->events = total_events; }
    return total_events;
}

u64 parser_run_file(Arena *a, ChatTool tool, const char *path,
                    chat_event_cb cb, void *user) {
    if (!a || !path || !cb) return 0;

    /* Cline/Roo/Kilo keep the whole session in one JSON array, not JSONL —
     * delegate to the dedicated runner. */
    if (tool == CHAT_TOOL_CLINE || tool == CHAT_TOOL_ROO || tool == CHAT_TOOL_KILO) {
        return parser_cline_run(a, tool, path, cb, user);
    }

    /* The standalone Cline CLI keeps one {messages:[…]} object per session —
     * a different shape from the extension's array handled just above. */
    if (tool == CHAT_TOOL_CLINE_CLI) {
        return parser_cline_cli_run(a, path, cb, user);
    }

    /* Antigravity stores conversations as SQLite DBs with protobuf steps. */
    if (tool == CHAT_TOOL_ANTIGRAVITY) {
        return parser_antigravity_run(a, path, cb, user);
    }

    return run_lines(a, tool, path, cb, user, 0, 0, NULL, NULL);
}

/* ---- a session's title: its first real prompt -------------------------- */

typedef struct {
    char *out;
    usize cap;
    u32   seen;
    bool  got;
} TitleScan;

static bool title_scan_cb(const ChatEvent *e, void *user) {
    TitleScan *t = (TitleScan *)user;
    t->seen++;
    if (e->role == CHAT_ROLE_USER && e->text && chat_title_from_prompt(e->text, t->out, t->cap)) {
        t->got = true;
        return false;                       /* found: stop reading the file */
    }
    return t->seen < CHAT_TITLE_MAX_EVENTS;
}

bool chat_session_title_ex(Arena *line_arena, ChatTool tool, const char *path, i64 size_bytes,
                           char *out, usize cap, u32 max_lines, u64 max_bytes, ChatTitleStats *st) {
    if (cap) out[0] = '\0';
    if (st) memset(st, 0, sizeof *st);
    if (!path || !path[0] || cap < 8) return false;
    TitleScan t = { out, cap, 0, false };
    switch (tool) {
    case CHAT_TOOL_CLAUDE: case CHAT_TOOL_CODEX: case CHAT_TOOL_COPILOT:
    case CHAT_TOOL_XAI:    case CHAT_TOOL_COMMANDCODE:
    case CHAT_TOOL_PI:     case CHAT_TOOL_OMP:
        /* One record per line: the reader stops with the callback, or at the
         * limits — a file of records the parser drops emits no event to count.
         * Codex: the only record that carries a prompt is a message whose role
         * is the JSON string "user" (parser_codex.c), so a line without those
         * six bytes is not handed to the JSON parser at all. Its rollouts open
         * with hundreds of kilobytes of instructions and tool records before
         * the prompt, and a sub-agent's has no prompt at all. */
        if (!line_arena) return false;
        run_lines(line_arena, tool, path, title_scan_cb, &t, max_lines, max_bytes,
                  tool == CHAT_TOOL_CODEX ? "\"user\"" : NULL, st);
        break;
    case CHAT_TOOL_CLINE: case CHAT_TOOL_ROO: case CHAT_TOOL_KILO:
    case CHAT_TOOL_CLINE_CLI: case CHAT_TOOL_ANTIGRAVITY: {
        /* One document per session: it is read whole, so only a small one. */
        if (size_bytes <= 0 || size_bytes > CHAT_TITLE_WHOLE_FILE_MAX) return false;
        Arena whole = arena_create((usize)size_bytes * 3u + MB(4));
        if (!whole.base) return false;
        parser_run_file(&whole, tool, path, title_scan_cb, &t);
        arena_destroy(&whole);
        break;
    }
    default:
        return false;                       /* no file of its own to read */
    }
    if (!t.got && cap) out[0] = '\0';
    return t.got;
}

bool chat_session_title(Arena *line_arena, ChatTool tool, const char *path, i64 size_bytes,
                        char *out, usize cap) {
    return chat_session_title_ex(line_arena, tool, path, size_bytes, out, cap,
                                 CHAT_TITLE_MAX_LINES, CHAT_TITLE_MAX_BYTES, NULL);
}
