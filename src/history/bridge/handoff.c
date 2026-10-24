/*
 * liu-history/bridge - structured context handoff. See handoff.h.
 */
#include "history/bridge/handoff.h"
#include "core/atomic_file.h"
#include "history/bridge/tool_map.h"
#include "history/bridge/writers.h"
#include "history/util.h"

#include "cJSON.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------------------
 * A set of tool-call ids
 *
 * Open addressing over string keys the BridgeHistory arena owns, so nothing is
 * copied and nothing outlives the arena. Power-of-two capacity, load factor
 * under 1/2, linear probing — enough to avoid an O(n^2) forward scan and small
 * enough not to be worth a dependency.
 * ------------------------------------------------------------------------- */
typedef struct {
    const char **slot;
    usize        mask;      /* capacity - 1 */
} HoIdSet;

static u64 ho_hash(const char *s) {
    u64 hv = 1469598103934665603ull;              /* FNV-1a */
    for (; *s; s++) { hv ^= (u8)*s; hv *= 1099511628211ull; }
    return hv;
}

static bool ho_set_init(HoIdSet *s, usize expect) {
    s->slot = NULL;
    s->mask = 0;
    usize cap = 64;
    while (cap < expect * 2u && cap < (usize)1 << 22) cap <<= 1;
    s->slot = (const char **)calloc(cap, sizeof *s->slot);
    if (!s->slot) return false;
    s->mask = cap - 1;
    return true;
}

static void ho_set_free(HoIdSet *s) {
    if (!s) return;
    free(s->slot);
    s->slot = NULL;
    s->mask = 0;
}

static void ho_set_add(HoIdSet *s, const char *key) {
    if (!s->slot || !key || !*key) return;
    usize i = (usize)ho_hash(key) & s->mask;
    for (usize probe = 0; probe <= s->mask; probe++) {
        const char *cur = s->slot[i];
        if (!cur) { s->slot[i] = key; return; }
        if (cur == key || strcmp(cur, key) == 0) return;   /* already in */
        i = (i + 1) & s->mask;
    }
    /* Full — cannot happen at load factor 1/2, and losing an id would only
     * report an answered call as unresolved. */
}

static bool ho_set_has(const HoIdSet *s, const char *key) {
    if (!s->slot || !key || !*key) return false;
    usize i = (usize)ho_hash(key) & s->mask;
    for (usize probe = 0; probe <= s->mask; probe++) {
        const char *cur = s->slot[i];
        if (!cur) return false;
        if (cur == key || strcmp(cur, key) == 0) return true;
        i = (i + 1) & s->mask;
    }
    return false;
}

/* Per-section budgets. The document is a briefing, so every section is
 * bounded: an unbounded "files touched" list from a refactor is a thousand
 * paths, which pushes the objective off the top of what the next agent
 * reads. */
#define HO_MAX_USER_TURNS     40
#define HO_TURN_CLIP         1200
#define HO_MAX_FILES          60
#define HO_MAX_UNRESOLVED     12
#define HO_MAX_OUTPUT_TAIL     3
#define HO_OUTPUT_CLIP       1500
#define HO_PATH_CAP           512

typedef struct { FILE *f; } Doc;

static void doc_line(Doc *d, const char *s) {
    if (d->f && s) fputs(s, d->f);
}

static void doc_fmt(Doc *d, const char *fmt, ...) {
    if (!d->f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(d->f, fmt, ap);
    va_end(ap);
}

/* A byte of a value from the SOURCE agent's store as it goes onto ONE line
 * of the briefing: CR/LF become a space, other control bytes are dropped (0). */
static int doc_line_byte(unsigned char c) {
    if (c == '\n' || c == '\r') return ' ';
    if (c < 0x20 || c == 0x7f) return 0;
    return (int)c;
}

/* One value as an inline code span: its exact text, on one line, in a span
 * that nothing in it can close.
 *
 * The briefing is what the next agent is told to trust first, and its cwd,
 * branch, file paths and commands come out of the source's store — so a value
 * carrying a newline could start a fresh heading or bullet of an attacker's
 * choosing, and a backtick could close the code span around it. doc_line_byte
 * keeps the value on one line. Backticks are not rewritten, because that
 * would change what is quoted: a command substitution would become a quoted
 * word, a different command, in the one list that says what already ran.
 * CommonMark ends a span only at a backtick run of the SAME length as the one
 * that opened it, so the delimiter is made one longer than the longest run
 * inside and the text is left alone. A reader strips one space from each end
 * of a span that has one at both, which is what lets a value begin or end
 * with a backtick — and why a value that itself begins and ends with a space
 * is padded too. */
static void doc_code_span(Doc *d, const char *value) {
    if (!d->f || !value) return;
    usize run = 0, longest = 0;
    int first = 0, last = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        int c = doc_line_byte(*p);
        if (!c) continue;
        if (!first) first = c;
        last = c;
        run = (c == '`') ? run + 1 : 0;
        if (run > longest) longest = run;
    }
    if (!first) first = last = '`';   /* nothing visible: pad, or the delimiters run together */
    bool pad = first == '`' || last == '`' || (first == ' ' && last == ' ');

    for (usize k = 0; k <= longest; k++) fputc('`', d->f);
    if (pad) fputc(' ', d->f);
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        int c = doc_line_byte(*p);
        if (c) fputc(c, d->f);
    }
    if (pad) fputc(' ', d->f);
    for (usize k = 0; k <= longest; k++) fputc('`', d->f);
}

/* One metadata value, or one entry of a list, on ONE line of the briefing.
 * `note` (may be NULL) follows the span in plain text. */
static void doc_item_code(Doc *d, const char *label, const char *value, const char *note) {
    if (!d->f || !value) return;
    if (label) fprintf(d->f, "- **%s** ", label);
    else       fputs("- ", d->f);
    doc_code_span(d, value);
    if (note) fprintf(d->f, " %s", note);
    fputc('\n', d->f);
}

/* `s` as plain text on one line: same line discipline, backticks left alone. */
static void doc_one_line(Doc *d, const char *s) {
    if (!d->f || !s) return;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int c = doc_line_byte(*p);
        if (c) fputc(c, d->f);
    }
}

/* Plain (non-code) metadata. */
static void doc_item_text(Doc *d, const char *label, const char *value) {
    if (!d->f || !value) return;
    fprintf(d->f, "- **%s** ", label);
    doc_one_line(d, value);
    fputc('\n', d->f);
}

/* Write at most `cap` bytes of `s`, never splitting a UTF-8 sequence. */
static void doc_clip(Doc *d, const char *s, usize cap) {
    if (!d->f || !s) return;
    usize n = strlen(s);
    bool cut = false;
    if (n > cap) {
        n = cap;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        cut = true;
    }
    fwrite(s, 1, n, d->f);
    if (cut) fputs("\n  […]", d->f);
}

/* What follows the path or the command of a call whose recorded input was
 * over the size cap: the reader should know the briefing saw a head of it. */
#define HO_NOTE_TRUNCATED "(input truncated)"

/* Pull a filesystem path out of a recorded tool input (the key sweep is
 * bridge_tool_input_path's). An input over the size cap arrives as
 * {"liu_truncated":true,…} with its path kept beside the clipped body — ir.c
 * puts it there for this reader — and `*truncated` says so. */
static bool tool_path(const char *input_json, char *out, usize cap, bool *truncated) {
    *truncated = false;
    if (!input_json || !input_json[0] || input_json[0] != '{') return false;
    cJSON *o = cJSON_Parse(input_json);
    if (!o) return false;
    bool ok = bridge_tool_input_path(o, out, cap) != NULL;
    *truncated = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "liu_truncated"));
    cJSON_Delete(o);
    return ok;
}

/* Shell commands: the command string, clipped to one line. */
static bool tool_command(const char *input_json, char *out, usize cap, bool *truncated) {
    *truncated = false;
    if (!input_json || !input_json[0] || input_json[0] != '{') return false;
    cJSON *o = cJSON_Parse(input_json);
    if (!o) return false;
    bool ok = bridge_tool_input_command(o, out, cap) != NULL;
    *truncated = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "liu_truncated"));
    cJSON_Delete(o);
    if (ok) {
        /* One line — its first that says anything: a heredoc in the briefing
         * turns the file list into a wall of script. */
        char *p = out;
        while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') p++;
        memmove(out, p, strlen(p) + 1);
        for (p = out; *p; p++) if (*p == '\n' || *p == '\r') { *p = '\0'; break; }
    }
    return ok && out[0];
}

/* Write at most `cap` bytes of `s` as a fenced block that `s` cannot close.
 *
 * Tool output is whatever the tool printed, and a README, a Markdown file or
 * an agent's own report routinely holds a ``` fence. Inside a three-backtick
 * fence that line ENDS the block: the rest of the output — its headings, its
 * lists, its instructions — is then read as the briefing's own text, by the
 * agent that was just told to trust the briefing first. CommonMark closes a
 * fence only with a run at least as long as the opener, so the opener is made
 * longer than any run in what is written. */
static void doc_fenced(Doc *d, const char *s, usize cap) {
    if (!d->f || !s) return;
    usize n = strlen(s);
    if (n > cap) {
        n = cap;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }
    usize run = 0, longest = 0;
    for (usize i = 0; i < n; i++) {
        run = (s[i] == '`') ? run + 1 : 0;
        if (run > longest) longest = run;
    }
    usize fence = longest < 3 ? 3 : longest + 1;
    for (usize k = 0; k < fence; k++) fputc('`', d->f);
    fputc('\n', d->f);
    doc_clip(d, s, cap);
    fputc('\n', d->f);
    for (usize k = 0; k < fence; k++) fputc('`', d->f);
    fputs("\n\n", d->f);
}

bool bridge_handoff_write(const BridgeHistory *h, char *out_path, usize cap) {
    if (!h || !out_path || cap == 0) return false;

    char dir[BRIDGE_PATH_CAP];
    if (!bridge_state_dir(dir, sizeof dir)) return false;

    char id[64];
    if (!bridge_new_short_id(id, sizeof id)) return false;
    int n = snprintf(out_path, cap, "%s/handoff-%s.md", dir, id);
    if (n <= 0 || (usize)n >= cap) return false;

    AtomicFileWriter afw;
    if (!atomic_file_begin_private(&afw, out_path)) return false;
    Doc d = { afw.stream };

    doc_fmt(&d, "# Session handoff from %s\n\n", chat_tool_name(h->source_tool));
    doc_line(&d,
        "This document is a briefing, not a transcript. It summarises a "
        "conversation that happened in a different coding agent so that you "
        "can continue the same task. **The tool actions listed below have "
        "already run — do not repeat them.** Read the current state of any "
        "file you need rather than trusting the outputs quoted here.\n\n");

    if (h->title)        doc_item_text(&d, "Session:", h->title);
    if (h->cwd)          doc_item_code(&d, "Working directory:", h->cwd, NULL);
    if (h->source_model) doc_item_text(&d, "Original model:", h->source_model);
    if (h->git_branch)   doc_item_code(&d, "Git branch:", h->git_branch, NULL);
    if (h->git_head)     doc_item_code(&d, "Git HEAD:", h->git_head, NULL);
    if (h->git_root)     doc_item_code(&d, "Repository:", h->git_root, NULL);
    doc_line(&d, "\n");

    /* ---- objective, requests, current task --------------------------- */
    isize first_user = -1, last_user = -1;
    u32 user_turns = 0;
    for (usize i = 0; i < h->event_count; i++) {
        if (h->events[i].type != BRIDGE_EVENT_USER) continue;
        if (!h->events[i].text || !h->events[i].text[0]) continue;
        if (first_user < 0) first_user = (isize)i;
        last_user = (isize)i;
        user_turns++;
    }

    if (first_user >= 0) {
        doc_line(&d, "## Session objective\n\n");
        doc_clip(&d, h->events[first_user].text, HO_TURN_CLIP);
        doc_line(&d, "\n\n");
    }

    if (user_turns > 1) {
        doc_line(&d, "## What the user asked for\n\n");
        u32 shown = 0;
        for (usize i = 0; i < h->event_count && shown < HO_MAX_USER_TURNS; i++) {
            if (h->events[i].type != BRIDGE_EVENT_USER) continue;
            if ((isize)i == first_user) continue;
            const char *t = h->events[i].text;
            if (!t || !t[0]) continue;
            doc_line(&d, "- ");
            doc_clip(&d, t, HO_TURN_CLIP);
            doc_line(&d, "\n");
            shown++;
        }
        if (user_turns - 1 > shown)
            doc_fmt(&d, "\n_(%u earlier requests omitted.)_\n", user_turns - 1 - shown);
        doc_line(&d, "\n");
    }

    /* ---- decisions: the assistant's closing conclusions ---------------- */
    {
        /* The last two assistant turns, which is where an agent states what
         * it did and what it decided. Earlier ones are working notes and a
         * briefing made of those reads as a diary. */
        isize picks[2] = { -1, -1 };
        u32 got = 0;
        for (usize i = h->event_count; i > 0 && got < 2; i--) {
            const BridgeEvent *e = &h->events[i - 1];
            if (e->type == BRIDGE_EVENT_ASSISTANT && e->text && e->text[0])
                picks[got++] = (isize)(i - 1);
        }
        if (got > 0) {
            doc_line(&d, "## Where the previous agent left off\n\n");
            for (i32 k = (i32)got - 1; k >= 0; k--) {
                doc_clip(&d, h->events[picks[k]].text, HO_TURN_CLIP * 2);
                doc_line(&d, "\n\n");
            }
        }
    }

    /* ---- files touched, from MUTATING calls only ---------------------- */
    {
        char (*files)[HO_PATH_CAP] = calloc(HO_MAX_FILES, HO_PATH_CAP);
        bool cut[HO_MAX_FILES] = {0};      /* some write to it was over the cap */
        u32 nfiles = 0, extra = 0;
        if (files) {
            for (usize i = 0; i < h->event_count; i++) {
                const BridgeEvent *e = &h->events[i];
                if (e->type != BRIDGE_EVENT_TOOL_CALL) continue;
                if (!bridge_tool_kind_is_mutating(e->tool_kind)) continue;
                if (e->tool_kind == BRIDGE_TOOL_SHELL) continue;   /* listed below */
                char p[HO_PATH_CAP];
                bool truncated = false;
                if (!tool_path(e->tool_input, p, sizeof p, &truncated)) continue;
                bool dup = false;
                for (u32 k = 0; k < nfiles; k++)
                    if (strcmp(files[k], p) == 0) {
                        dup = true;
                        cut[k] = cut[k] || truncated;
                        break;
                    }
                if (dup) continue;
                if (nfiles < HO_MAX_FILES) {
                    cut[nfiles] = truncated;
                    hist_copy_bounded(files[nfiles++], HO_PATH_CAP, p);
                } else {
                    extra++;
                }
            }
            if (nfiles) {
                doc_line(&d, "## Files the previous agent changed\n\n");
                for (u32 k = 0; k < nfiles; k++)
                    doc_item_code(&d, NULL, files[k], cut[k] ? HO_NOTE_TRUNCATED : NULL);
                if (extra) doc_fmt(&d, "- _(and %u more)_\n", extra);
                doc_line(&d, "\n");
            }
            free(files);
        }
    }

    /* ---- shell commands run ------------------------------------------- */
    {
        u32 shown = 0, total = 0;
        for (usize i = 0; i < h->event_count; i++) {
            const BridgeEvent *e = &h->events[i];
            if (e->type != BRIDGE_EVENT_TOOL_CALL) continue;
            if (e->tool_kind != BRIDGE_TOOL_SHELL) continue;
            total++;
        }
        if (total) {
            doc_line(&d, "## Commands the previous agent ran\n\n");
            doc_line(&d, "_Already executed. Listed so you know what state the "
                         "working tree is in._\n\n");
            for (usize i = 0; i < h->event_count && shown < HO_MAX_FILES; i++) {
                const BridgeEvent *e = &h->events[i];
                if (e->type != BRIDGE_EVENT_TOOL_CALL) continue;
                if (e->tool_kind != BRIDGE_TOOL_SHELL) continue;
                char cmd[HO_PATH_CAP];
                bool truncated = false;
                if (!tool_command(e->tool_input, cmd, sizeof cmd, &truncated)) continue;
                doc_item_code(&d, NULL, cmd, truncated ? HO_NOTE_TRUNCATED : NULL);
                shown++;
            }
            if (total > shown) doc_fmt(&d, "- _(and %u more)_\n", total - shown);
            doc_line(&d, "\n");
        }
    }

    /* ---- tool activity summary, by semantic kind ---------------------- */
    {
        u32 counts[BRIDGE_TOOL_KIND_COUNT_] = {0};
        u32 any = 0;
        for (usize i = 0; i < h->event_count; i++) {
            const BridgeEvent *e = &h->events[i];
            if (e->type != BRIDGE_EVENT_TOOL_CALL) continue;
            if (e->tool_kind < BRIDGE_TOOL_KIND_COUNT_) counts[e->tool_kind]++;
            any++;
        }
        if (any) {
            doc_line(&d, "## Tool activity\n\n");
            for (i32 k = 1; k < BRIDGE_TOOL_KIND_COUNT_; k++) {
                if (!counts[k]) continue;
                doc_fmt(&d, "- %s: %u call%s\n", bridge_tool_kind_id((BridgeToolKind)k),
                        counts[k], counts[k] == 1 ? "" : "s");
            }
            if (counts[BRIDGE_TOOL_UNKNOWN])
                doc_fmt(&d, "- unrecognised tools: %u calls\n", counts[BRIDGE_TOOL_UNKNOWN]);
            doc_line(&d, "\n");
        }
    }

    /* Collect returned call IDs in one pass, then check unresolved calls
     * against the set. Scanning forward from every call would perform O(n²)
     * string comparisons for up to BRIDGE_MAX_EVENTS events, especially when
     * many calls have no result. */
    {
        HoIdSet answered_ids;
        bool have_set = ho_set_init(&answered_ids, h->event_count);
        if (have_set) {
            for (usize j = 0; j < h->event_count; j++) {
                const BridgeEvent *r = &h->events[j];
                if (r->type == BRIDGE_EVENT_TOOL_RESULT && r->tool_call_id)
                    ho_set_add(&answered_ids, r->tool_call_id);
            }
        }

        u32 shown = 0, total = 0;
        for (usize i = 0; i < h->event_count; i++) {
            const BridgeEvent *e = &h->events[i];
            if (e->type != BRIDGE_EVENT_TOOL_CALL) continue;
            bool answered = false;
            if (have_set) {
                answered = e->tool_call_id && ho_set_has(&answered_ids, e->tool_call_id);
            } else {
                /* Allocation failed: the original scan, which is slow but
                 * produces the same briefing. */
                for (usize j = i + 1; j < h->event_count; j++) {
                    const BridgeEvent *r = &h->events[j];
                    if (r->type == BRIDGE_EVENT_TOOL_RESULT && r->tool_call_id &&
                        e->tool_call_id &&
                        strcmp(r->tool_call_id, e->tool_call_id) == 0) {
                        answered = true;
                        break;
                    }
                }
            }
            if (answered) continue;
            total++;
            if (shown >= HO_MAX_UNRESOLVED) continue;
            if (shown == 0) doc_line(&d, "## Unfinished at the end of the session\n\n");
            /* The tool's NAME is the source's too: the same span as a path. */
            doc_line(&d, "- ");
            doc_code_span(&d, (e->tool_name && e->tool_name[0]) ? e->tool_name : "tool");
            doc_fmt(&d, " (%s) — no result recorded\n", bridge_tool_kind_verb(e->tool_kind));
            shown++;
        }
        if (shown) {
            if (total > shown) doc_fmt(&d, "- _(and %u more)_\n", total - shown);
            doc_line(&d, "\n");
        }
        ho_set_free(&answered_ids);
    }

    /* ---- the tail of the last few tool outputs ------------------------ */
    {
        isize picks[HO_MAX_OUTPUT_TAIL];
        u32 got = 0;
        for (usize i = h->event_count; i > 0 && got < HO_MAX_OUTPUT_TAIL; i--) {
            const BridgeEvent *e = &h->events[i - 1];
            if (e->type == BRIDGE_EVENT_TOOL_RESULT && e->tool_output && e->tool_output[0])
                picks[got++] = (isize)(i - 1);
        }
        if (got) {
            doc_line(&d, "## Most recent tool output\n\n");
            for (i32 k = (i32)got - 1; k >= 0; k--) {
                const BridgeEvent *e = &h->events[picks[k]];
                /* A result usually carries no tool NAME: the Anthropic shape
                 * every Claude-family source uses puts the name on the CALL
                 * and only a tool_use_id on the result. Without this lookup
                 * every such section would be headed "### tool", which tells
                 * the next agent nothing about what produced the output it is
                 * reading. */
                const char *name = e->tool_name;
                if ((!name || !name[0]) && e->tool_call_id) {
                    for (usize j = (usize)picks[k]; j > 0; j--) {
                        const BridgeEvent *c = &h->events[j - 1];
                        if (c->type == BRIDGE_EVENT_TOOL_CALL && c->tool_call_id &&
                            strcmp(c->tool_call_id, e->tool_call_id) == 0) {
                            name = c->tool_name;
                            break;
                        }
                    }
                }
                /* A heading is one line, whatever the source called its tool. */
                doc_line(&d, "### ");
                doc_one_line(&d, (name && name[0]) ? name : "tool");
                doc_line(&d, "\n\n");
                doc_fenced(&d, e->tool_output, HO_OUTPUT_CLIP);
            }
        }
    }

    /* ---- current task ------------------------------------------------- */
    if (last_user >= 0 && last_user != first_user) {
        doc_line(&d, "## The user's most recent request\n\n");
        doc_clip(&d, h->events[last_user].text, HO_TURN_CLIP * 2);
        doc_line(&d, "\n\n");
    }

    doc_line(&d, "---\n\n_Prepared by Liu. Fidelity: ");
    doc_fmt(&d, "%s. %u messages, %u tool calls, %u tool results carried over._\n",
            bridge_fidelity_name(bridge_fidelity_of(&h->fidelity)),
            h->fidelity.messages, h->fidelity.tool_calls, h->fidelity.tool_results);

    if (!atomic_file_commit(&afw)) return false;
    return true;
}

void bridge_handoff_prompt(const char *doc_path, ChatTool source,
                           char *out, usize cap) {
    /* Short on purpose. This ends up inside a single-quoted argv on a shell
     * line the user watches being typed into their pane; a long paragraph
     * wraps several times before the closing quote and can overrun the picker
     * row's command buffer, leaving the shell at a `quote>` prompt.
     *
     * The detail is not lost: the document itself opens with the full
     * do-not-repeat instruction, and it is the document the agent reads. This
     * only has to get it to open the file. */
    snprintf(out, cap,
             "Read %s first — a briefing on a %s session to continue. "
             "Its work is already done; check the current file state before "
             "changing anything.",
             doc_path ? doc_path : "", chat_tool_name(source));
}

bool bridge_handoff_import(const BridgeHistory *h, BridgeImportResult *r,
                           const char *cmd_fmt) {
    r->mode = BRIDGE_IMPORT_CONTEXT_HANDOFF;

    char doc[BRIDGE_PATH_CAP];
    if (!bridge_handoff_write(h, doc, sizeof doc)) {
        snprintf(r->error, sizeof r->error, "Could not write the handoff briefing.");
        return false;
    }
    hist_copy_bounded(r->session_path, sizeof r->session_path, doc);

    /* There is no target session, so `session_id` names the DOCUMENT. Saying
     * nothing would leave the lineage journal with an empty target and the
     * user with no way to find the briefing again. */
    const char *base = strrchr(doc, '/');
    hist_copy_bounded(r->session_id, sizeof r->session_id, base ? base + 1 : doc);

    if (cmd_fmt && strstr(cmd_fmt, "%s")) {
        char prompt[BRIDGE_CMD_CAP];
        bridge_handoff_prompt(doc, h->source_tool, prompt, sizeof prompt);
        /* Single-quoted for the shell, with the standard '\'' escape for any
         * quote inside. The path is Liu's own state dir plus a base32 id so
         * it cannot contain one, but the prompt carries the source tool name
         * and a briefing path, and quoting by construction is cheaper than
         * arguing about whether it can. */
        char quoted[BRIDGE_CMD_CAP];
        usize o = 0;
        if (o + 1 < sizeof quoted) quoted[o++] = '\'';
        for (const char *p = prompt; *p && o + 5 < sizeof quoted; p++) {
            if (*p == '\'') {
                memcpy(quoted + o, "'\\''", 4);
                o += 4;
            } else {
                quoted[o++] = *p;
            }
        }
        if (o + 1 < sizeof quoted) quoted[o++] = '\'';
        quoted[o] = '\0';
        snprintf(r->launch_cmd, sizeof r->launch_cmd, cmd_fmt, quoted);
    }

    /* A handoff is never full fidelity, whatever the parse achieved: the next
     * agent gets a briefing, not the conversation. */
    r->fidelity = BRIDGE_FIDELITY_HANDOFF;
    return true;
}

bool bridge_handoff_validate(const BridgeImportResult *r) {
    if (!r || !r->session_path[0]) return false;
    struct stat st;
    return lstat(r->session_path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}
