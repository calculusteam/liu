/*
 * Liu — agent activity scraped off the pane's screen. See agent_activity.h for
 * the grammar this implements and why it is a screen scan.
 */
#include "terminal/agent_activity.h"
#include "core/utf8.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------- glyph sets */

/* A rail drawn to the LEFT of the marker. Grok puts ❙ in front of the item it
 * is currently on and OpenCode rails its whole transcript with ┃, so "the
 * first glyph on the row" is not the marker for either of them. */
static bool cp_is_rail(u32 cp) {
    return cp == 0x2502 || cp == 0x2503 ||          /* │ ┃ */
           cp == 0x2506 || cp == 0x2507 ||
           cp == 0x250A || cp == 0x250B ||
           cp == 0x2758 || cp == 0x2759 || cp == 0x275A ||  /* ❘ ❙ ❚ */
           cp == 0x258C || cp == 0x258F || cp == 0x2590 ||  /* ▌ ▏ ▐ */
           cp == 0x2595;
}

/* Marks a tool call / step the agent took. */
static bool cp_is_action_marker(u32 cp) {
    switch (cp) {
    case 0x23FA:   /* ⏺  Claude Code */
    case 0x25CF:   /* ●  Antigravity */
    case 0x2022:   /* •  Codex */
    case 0x25C6:   /* ◆  Grok — thought */
    case 0x25C8:   /* ◈  Grok — tool */
    case 0x2192:   /* →  OpenCode */
    case 0x25B8:   /* ▸  Antigravity — thought */
    case 0x25B6:   /* ▶ */
    case 0x2023:   /* ‣ */
        return true;
    default:
        return false;
    }
}

/* One frame of a spinner. Claude cycles the sparkle family and drops to a
 * middle dot on its quiet frames; Grok and Antigravity use braille; OpenCode
 * runs a row of ⬝. */
static bool cp_is_spinner(u32 cp) {
    if (cp >= 0x2800 && cp <= 0x28FF) return true;   /* braille */
    if (cp >= 0x2720 && cp <= 0x273F) return true;   /* ✢ ✳ ✶ ✽ … */
    switch (cp) {
    case 0x00B7:   /* · */
    case 0x2B1D:   /* ⬝ */
    case 0x25D0: case 0x25D1: case 0x25D2: case 0x25D3:   /* ◐◑◒◓ */
    case 0x25E6:   /* ◦ */
    case 0x2219:   /* ∙ */
        return true;
    default:
        return false;
    }
}

/* Sigil an agent CLI echoes the USER's submitted prompt with. Reaching one
 * while walking up means the current turn has been fully scanned: everything
 * above belongs to an earlier exchange, and an action line from that would
 * describe something the agent finished doing minutes ago. */
static bool cp_is_prompt_sigil(u32 cp) {
    return cp == 0x276F ||    /* ❯  Claude, Grok */
           cp == 0x203A ||    /* ›  Codex */
           cp == '>' || cp == '$' || cp == '%' || cp == '#';
}

/* ------------------------------------------------------------ text helpers */

static bool ascii_lower_eq(const char *s, const char *lower, usize n) {
    for (usize i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lower[i]) return false;
    }
    return true;
}

/* Case-insensitive substring search over ASCII. */
static const char *find_ci(const char *hay, const char *needle) {
    usize n = strlen(needle);
    if (n == 0) return hay;
    usize hl = strlen(hay);
    if (hl < n) return NULL;
    for (usize i = 0; i + n <= hl; i++) {
        if (ascii_lower_eq(hay + i, needle, n)) return hay + i;
    }
    return NULL;
}

bool agent_activity_is_status_text(const char *text) {
    if (!text || !text[0]) return false;
    /* Every CLI that draws a spinner has to tell the user how to stop the turn
     * next to it. That, not the marker glyph, is what separates Codex's "•
     * Working (9s • esc to interrupt)" from its "• Ran 3 commands".
     *
     * The phrase, never the bare word: "interrupt" is a substring of
     * "interrupted", and agents write that about things that already happened
     * ("⎿ Login interrupted", "Request interrupted by user"), on lines that
     * are not status lines at all. */
    return find_ci(text, "to interrupt")  != NULL ||
           find_ci(text, "esc interrupt") != NULL ||
           find_ci(text, "[stop]")        != NULL ||
           find_ci(text, "to cancel")     != NULL;
}

/* Chrome, not activity: OpenCode's spinner row is a keybinding legend, and
 * once the "esc interrupt" that identified it as a status line is cut away
 * what is left ("tab agents  ctrl+p commands") is nothing anyone wants in a
 * tab preview. */
static bool text_is_keyhint(const char *s) {
    static const char *heads[] = { "esc", "ctrl", "shift", "alt", "cmd",
                                   "tab ", "enter", "?", NULL };
    for (i32 i = 0; heads[i]; i++) {
        usize n = strlen(heads[i]);
        if (ascii_lower_eq(s, heads[i], n)) return true;
    }
    return false;
}

/* A trailing elapsed-time stamp is deliberately KEPT. Grok's spinner reads
 * "Responding… 0.9s" and its action line "Thought for 0.2s" — the two are the
 * same shape, so a rule that stripped the stamp would either leave "Thought
 * for" dangling or need to know which line it was looking at. The scan reruns
 * while the pane works, so the number stays live. */
static void trim_tail(char *s) {
    usize n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') s[--n] = '\0';
}

/* Is this a byte a filesystem path can be made of? */
static bool path_byte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '/' || c == '.' || c == '_' || c == '-' || c == '+' ||
           c == '~' || c == '@';
}

/* Replace any long absolute path with its basename, in place.
 *
 * Antigravity names its tool calls with the full path — "Read(/Volumes/Work/
 * projects/liu/src/ui/tab_popup.c)" — and the popup's row is far too narrow
 * for that. Truncating it with an ellipsis would keep the machine's directory
 * layout and throw away the filename, which is the only part that answers the
 * question. The replacement is done on the PATH RUN, not on the whitespace
 * token, so the call's shape survives: "Read(tab_popup.c)". */
static void shorten_paths(char *s, usize cap) {
    const usize KEEP = 30;
    usize i = 0;
    while (s[i]) {
        if (s[i] != '/' && !(s[i] == '~' && s[i + 1] == '/')) { i++; continue; }
        /* A path run must start at a boundary, or "a/b" inside a word splits. */
        usize start = i;
        usize j = i;
        while (s[j] && path_byte(s[j])) j++;
        usize len = j - start;
        if (len <= KEEP) { i = j > i ? j : i + 1; continue; }
        usize base = start;
        for (usize k = start; k < j; k++) if (s[k] == '/') base = k + 1;
        if (base >= j) { i = j; continue; }
        usize blen = j - base;
        if (blen == 0 || start + blen >= cap) { i = j; continue; }
        memmove(s + start, s + base, blen);
        memmove(s + start + blen, s + j, strlen(s + j) + 1);
        i = start + blen;
    }
}

/* Drop a leading list bullet.
 *
 * Claude Code opens its ANSWER block with the same ⏺ marker it uses for tool
 * calls, so once the model starts replying the bottom-most action line is the
 * first line of the answer — "⏺ - src/ui/tab_popup.c — 885 lines". That still
 * describes what the pane is about and is kept; the orphaned "- " in front of
 * it is not. */
static void strip_list_marker(char *s) {
    usize skip = 0;
    if ((s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ') skip = 2;
    else if ((u8)s[0] == 0xE2 && (u8)s[1] == 0x80 && (u8)s[2] == 0xA2 &&
             s[3] == ' ') skip = 4;                      /* • */
    else {
        usize i = 0;
        while (s[i] >= '0' && s[i] <= '9') i++;
        if (i > 0 && (s[i] == '.' || s[i] == ')') && s[i + 1] == ' ') skip = i + 2;
    }
    if (!skip) return;
    while (s[skip] == ' ') skip++;
    memmove(s, s + skip, strlen(s + skip) + 1);
}

/* Cut the trailing chrome an agent appends to its own lines: a parenthesised
 * timer or hint ("(4s · ↓ 145 tokens)", "(ctrl+o to expand)"), a "· ctrl + t to
 * view transcript" tail, and the column gap before a right-aligned status
 * cluster. The 3-space rule is what handles the last one: a drawn TUI pads to
 * an absolute column, so anything past a run of three blanks belongs to a
 * different widget on the same row. */
static void cut_trailers(char *s) {
    strip_list_marker(s);

    /* " (" — every known trailer that is parenthesised has a space before
     * the paren, while a call like "Read(path)" does not. */
    char *p = strstr(s, " (");
    if (p) *p = '\0';

    /* " · <keyhint>". Not "everything past the first middle dot": the dot is
     * an ordinary separator inside a real action line too. */
    for (char *q = s; q[0] && q[1] && q[2]; q++) {
        if ((u8)q[0] == 0xC2 && (u8)q[1] == 0xB7 && q > s && q[-1] == ' ' &&
            q[2] == ' ' && text_is_keyhint(q + 3)) {
            q[-1] = '\0';
            break;
        }
    }

    /* A run of three or more spaces = a column gap between two widgets. */
    for (char *q = s; q[0] && q[1] && q[2]; q++) {
        if (q[0] == ' ' && q[1] == ' ' && q[2] == ' ') { *q = '\0'; break; }
    }
    trim_tail(s);
}

/* ------------------------------------------------------------- row reading */

/* Row -> UTF-8, skipping leading blanks and the rail glyph, and reporting the
 * marker codepoint. Returns false for a blank row. `text` gets everything
 * after the marker (and after any repeat of it — OpenCode's spinner is a row
 * of eight ⬝). */
static bool row_split(Terminal *t, i32 row, u32 *out_marker,
                      char *text, usize cap) {
    i32 len = 0;
    const Cell *cells = terminal_visible_row(t, row, &len);
    if (!cells) return false;
    if (len > t->cols) len = t->cols;

    i32 c = 0;
    while (c < len && (cells[c].codepoint == 0 || cells[c].codepoint == ' ')) c++;
    if (c >= len) return false;

    if (cp_is_rail(cells[c].codepoint)) {
        c++;
        while (c < len && (cells[c].codepoint == 0 || cells[c].codepoint == ' ')) c++;
        if (c >= len) return false;
    }

    u32 marker = cells[c].codepoint;
    *out_marker = marker;

    i32 body = c;
    if (cp_is_action_marker(marker) || cp_is_spinner(marker)) {
        body = c + 1;
        while (body < len && cells[body].codepoint == marker) body++;   /* ⬝⬝⬝⬝ */
        while (body < len &&
               (cells[body].codepoint == 0 || cells[body].codepoint == ' ')) body++;
    }

    usize pos = 0;
    for (i32 x = body; x < len && pos + 8 < cap; x++) {
        u32 cp = cells[x].codepoint;
        if (cp == 0) cp = ' ';
        u8 tmp[24];
        u32 n = (cp & (1u << 30)) ? utf8_cluster_encode(cp, tmp, sizeof tmp)
                                  : utf8_encode(cp, tmp);
        if (n == 0 || pos + n + 1 >= cap) break;
        /* Control bytes cannot reach a cell, but a cluster can decode to
         * anything; the result is drawn as a label, so keep it printable. */
        for (u32 k = 0; k < n; k++) text[pos++] = (tmp[k] < 0x20) ? ' ' : (char)tmp[k];
    }
    text[pos] = '\0';
    while (pos > 0 && text[pos - 1] == ' ') text[--pos] = '\0';
    return true;
}

/* Enough left to read? Rejects a line that survived the trailer cuts as pure
 * punctuation or a two-character stub. */
static bool text_is_useful(const char *s) {
    i32 letters = 0, cells_n = 0;
    for (const u8 *p = (const u8 *)s; *p; p++) {
        if ((*p & 0xC0) == 0x80) continue;
        cells_n++;
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) letters++;
    }
    return cells_n >= 3 && letters >= 2 && !text_is_keyhint(s);
}

/* UTF-8-safe copy into `cap` bytes. */
static void copy_fitted(char *dst, usize cap, const char *src) {
    if (cap == 0) return;
    usize n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((u8)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    while (n > 0 && dst[n - 1] == ' ') dst[--n] = '\0';
}

bool agent_activity_scan(Terminal *t, char *out, usize cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!t || !t->cells || t->cols <= 0 || t->rows <= 0) return false;
    /* A view scrolled into history shows an old turn; answering from it would
     * pin a stale line for as long as the user stays scrolled up. */
    if (t->scroll_offset != 0) return false;

    char status[256]; status[0] = '\0';
    char action[256]; action[0] = '\0';

    /* The spinner sits BELOW the input block for OpenCode and above it for
     * everyone else, so the status sweep covers the whole screen. An action
     * line is transcript, which is always above the cursor — bounding it there
     * keeps the user's own half-typed text out of the answer. */
    i32 cursor_row = t->cursor_y;
    if (cursor_row < 0) cursor_row = 0;
    if (cursor_row > t->rows) cursor_row = t->rows;

    for (i32 row = t->rows - 1; row >= 0; row--) {
        u32 marker = 0;
        char text[256];
        if (!row_split(t, row, &marker, text, sizeof text)) continue;

        bool is_status = agent_activity_is_status_text(text) || cp_is_spinner(marker);
        bool is_action = !is_status && cp_is_action_marker(marker) &&
                         row < cursor_row;

        if (is_status && !status[0]) {
            cut_trailers(text);
            if (text_is_useful(text)) copy_fitted(status, sizeof status, text);
            continue;
        }
        if (is_action) {
            cut_trailers(text);
            shorten_paths(text, sizeof text);
            if (text_is_useful(text)) {
                copy_fitted(action, sizeof action, text);
                break;                       /* bottom-most wins */
            }
            continue;
        }
        /* The user's own prompt, echoed into the transcript: everything above
         * it belongs to a turn that is already over. */
        if (row < cursor_row && cp_is_prompt_sigil(marker) && text[0]) break;
    }

    const char *pick = action[0] ? action : status;
    if (!pick[0]) return false;
    copy_fitted(out, cap, pick);
    return out[0] != '\0';
}
