/*
 * Input shape and editing behavior used when reading prompts from supported
 * agent CLIs. The adapters account for visible row limits, prompt wrapping,
 * cursor motion, and special paste labels so text capture and editing actions
 * match the prompt shown. Unknown agents use a conservative single-row model.
 */
#ifndef UI_AGENT_INPUT_H
#define UI_AGENT_INPUT_H

#include "core/types.h"
#include "history/event.h"
#include "terminal/cmdline.h"

/* Rows of a prompt Claude Code 2.1.296 draws in a pane of `pane_rows`. */
static inline i32 agent_input_claude_rows_drawn(i32 pane_rows) {
    i32 drawn = pane_rows / 2 - 5;
    return drawn > 3 ? drawn : 3;
}

static inline CmdInputApp agent_input_app(ChatTool tool, i32 pane_rows, i32 pane_cols) {
    CmdInputApp app = { 1, false, 0, 0, false, false };
    if (tool == CHAT_TOOL_CLAUDE) {
        /* Leave two rows of safety margin below the estimated visible
         * prompt height. */
        i32 whole = pane_rows >= 12 ? agent_input_claude_rows_drawn(pane_rows) - 2 : 1;
        app.whole_rows = whole > 1 ? whole : 1;
        app.tight_frame = true;
        app.wrap_width = pane_cols > 12 ? pane_cols - 4 : 0;
        app.other_sigil = '!';               /* its shell mode */
        app.dim_hint = true;
    } else if (tool == CHAT_TOOL_PI) {
        /* Leave two rows of safety margin below the estimated visible
         * prompt height. */
        i32 whole = pane_rows >= 24 ? pane_rows * 3 / 10 - 2 : 1;
        app.whole_rows = whole > 1 ? whole : 1;
        app.tight_frame = true;
    } else if (tool == CHAT_TOOL_OMP) {
        app.ghost = true;
    }
    return app;
}

/* Whether boundary Backspace and Delete behavior allows the prompt to be
 * cleared without calculating its character count. */
static inline bool agent_input_erase_all(ChatTool tool) {
    return tool == CHAT_TOOL_CLAUDE || tool == CHAT_TOOL_PI || tool == CHAT_TOOL_OMP;
}

/* For Claude Code, Pi, and Oh My Pi, Ctrl+E moves the caret to the end of
 * the current line and Right at the line end advances to the next line. Liu
 * uses this behavior to move through multiline prompt input one line at a
 * time. */
static inline bool agent_input_ctrl_e_ends_line(ChatTool tool) {
    return tool == CHAT_TOOL_CLAUDE || tool == CHAT_TOOL_PI || tool == CHAT_TOOL_OMP;
}

/* For Claude Code, Pi, and Oh My Pi, Shift+Enter inserts a line break when
 * sent as CSI 13;2u rather than submitting the prompt. */
static inline bool agent_input_shift_enter_breaks(ChatTool tool) {
    return tool == CHAT_TOOL_CLAUDE || tool == CHAT_TOOL_PI || tool == CHAT_TOOL_OMP;
}

/* Return the byte length of a recognized paste-label token, or 0 when the
 * string does not start with one. Match the label syntax exactly so cursor
 * movement stays aligned with the prompt editor's single-unit treatment. */
static inline i32 agent_input_pi_label_unit(const char *s, i32 len) {
    /* Pi paste-label forms recognized by the parser: [paste #N +M lines] and
     * [paste #N C chars].
     */
    static const char head[] = "[paste #";
    if (len < 14) return 0;
    for (i32 i = 0; head[i]; i++) if (s[i] != head[i]) return 0;
    i32 i = (i32)sizeof head - 1, d = i;
    while (i < len && s[i] >= '0' && s[i] <= '9') i++;
    if (i == d || i >= len || s[i] != ' ') return 0;
    i++;
    bool plus = i < len && s[i] == '+';
    if (plus) i++;
    d = i;
    while (i < len && s[i] >= '0' && s[i] <= '9') i++;
    if (i == d) return 0;
    const char *tail = plus ? " lines]" : " chars]";
    for (i32 k = 0; tail[k]; k++, i++)
        if (i >= len || s[i] != tail[k]) return 0;
    return i;
}

static inline i32 agent_input_label_unit(ChatTool tool, const char *s, i32 len) {
    if (tool == CHAT_TOOL_PI) return s ? agent_input_pi_label_unit(s, len) : 0;
    if (tool != CHAT_TOOL_CLAUDE || !s || len < 9 || s[0] != '[') return 0;
    i32 i = 1;
    bool dots = len > 4 && s[1] == '.' && s[2] == '.' && s[3] == '.';
    const char *word = NULL;
    bool lines = false, need_lines = false;
    if (dots) { i = 4; word = "Truncated text #"; lines = need_lines = true; }
    else if (s[1] == 'I') word = "Image #";
    else if (s[1] == 'P') { word = "Pasted text #"; lines = true; }
    else return 0;
    for (const char *w = word; *w; w++, i++)
        if (i >= len || s[i] != *w) return 0;
    i32 digits = i;
    while (i < len && s[i] >= '0' && s[i] <= '9') i++;
    if (i == digits) return 0;
    if (lines && i + 1 < len && s[i] == ' ' && s[i + 1] == '+') {
        i += 2;
        digits = i;
        while (i < len && s[i] >= '0' && s[i] <= '9') i++;
        static const char tail[] = " lines";
        if (i == digits) return 0;
        for (i32 k = 0; tail[k]; k++, i++)
            if (i >= len || s[i] != tail[k]) return 0;
    } else if (need_lines) return 0;
    if (dots) {
        if (i + 2 >= len || s[i] != '.' || s[i + 1] != '.' || s[i + 2] != '.') return 0;
        i += 3;
    }
    return i < len && s[i] == ']' ? i + 1 : 0;
}

#endif /* UI_AGENT_INPUT_H */
