/*
 * liu-history - file driver: read a JSONL file line-by-line, dispatch to the
 * per-tool parser, hand events to the user callback.
 *
 * The arena is reset after every line so peak memory stays flat even for 4+
 * MB session files with 1000+ records.
 */
#include "history/parser.h"
#include "history/event.h"
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
        default: return 0;
    }
}

u64 parser_run_file(Arena *a, ChatTool tool, const char *path,
                    chat_event_cb cb, void *user) {
    if (!a || !path || !cb) return 0;

    /* Cline/Roo/Kilo keep the whole session in one JSON array, not JSONL —
     * delegate to the dedicated runner. */
    if (tool == CHAT_TOOL_CLINE || tool == CHAT_TOOL_ROO || tool == CHAT_TOOL_KILO) {
        return parser_cline_run(a, tool, path, cb, user);
    }

    /* O_NOFOLLOW so a symlink under ~/.claude/projects/ can't redirect the
     * read to an arbitrary location. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno != ELOOP)
            fprintf(stderr, "liu-history: open %s: %s\n", path, strerror(errno));
        return 0;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return 0; }

    /* Allocate a growable line buffer outside the arena (getline handles it). */
    char *line = NULL;
    size_t cap = 0;
    ssize_t nread;
    u64 total_events = 0;
    ChatEvent events[HIST_EVENTS_PER_LINE];

    while ((nread = getline(&line, &cap, f)) > 0) {
        if ((usize)nread > HIST_LINE_CAP) continue; /* pathological */
        /* Strip trailing newline for cleaner JSON start detection. */
        if (line[nread - 1] == '\n') nread--;
        if (nread > 0 && line[nread - 1] == '\r') nread--;
        if (nread <= 0) continue;

        arena_reset(a);
        u32 n = dispatch(a, tool, line, (usize)nread, events, HIST_EVENTS_PER_LINE);
        for (u32 i = 0; i < n; i++) {
            total_events++;
            if (!cb(&events[i], user)) { goto done; }
        }
    }

done:
    free(line);
    fclose(f);
    return total_events;
}
