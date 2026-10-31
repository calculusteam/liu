/*
 * liu-history/bridge - Universal Agent History IR implementation.
 *
 * See ir.h for the model. This file owns exactly three policies:
 *   - what gets copied and what gets clipped (the size table in ir.h),
 *   - how a missing tool_call_id is synthesized so the call/result pairing
 *     survives into a target that requires one,
 *   - how the per-history fidelity counters are derived.
 */
#include "history/bridge/ir.h"

#include "core/checked.h"
#include "history/bridge/jsonw.h"
#include "history/bridge/tool_map.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */

const char *bridge_event_type_name(BridgeEventType t) {
    switch (t) {
        case BRIDGE_EVENT_SYSTEM:      return "system";
        case BRIDGE_EVENT_USER:        return "user";
        case BRIDGE_EVENT_ASSISTANT:   return "assistant";
        case BRIDGE_EVENT_TOOL_CALL:   return "tool_call";
        case BRIDGE_EVENT_TOOL_RESULT: return "tool_result";
        case BRIDGE_EVENT_COMPACTION:  return "compaction";
        case BRIDGE_EVENT_CONTEXT:     return "context";
        default:                       return "unknown";
    }
}

const char *bridge_fidelity_name(BridgeFidelity f) {
    switch (f) {
        case BRIDGE_FIDELITY_FULL:    return "full";
        case BRIDGE_FIDELITY_HIGH:    return "high";
        case BRIDGE_FIDELITY_PARTIAL: return "partial";
        default:                      return "handoff";
    }
}

BridgeFidelity bridge_fidelity_of(const BridgeFidelityStats *s) {
    if (!s) return BRIDGE_FIDELITY_HANDOFF;
    /* A dropped event or a clipped USER/ASSISTANT message is structural loss:
     * something the next agent will never see. A clipped tool OUTPUT is not —
     * the call, its arguments and the fact that it ran all survive, and the
     * marker says the tail was cut. Those are different grades on purpose. */
    if (s->events_dropped > 0 || s->messages_truncated > 0)
        return BRIDGE_FIDELITY_PARTIAL;
    if (s->tool_outputs_truncated > 0 || !s->metadata_complete)
        return BRIDGE_FIDELITY_HIGH;
    return BRIDGE_FIDELITY_FULL;
}

/* ------------------------------------------------------------------------- */

void bridge_history_init(BridgeHistory *h, Arena *arena, ChatTool source_tool) {
    if (!h) return;
    memset(h, 0, sizeof *h);
    h->arena       = arena;
    h->source_tool = source_tool;
    h->fidelity.metadata_complete = false;
    h->fidelity.history_complete  = true;
}

void bridge_history_free(BridgeHistory *h) {
    if (!h) return;
    free(h->events);
    h->events     = NULL;
    h->event_count = 0;
    h->event_cap   = 0;
}

/* Duplicate at most cap bytes of s without splitting a UTF-8 sequence. Set
 * clipped when the copy is short and append mark when provided so the target
 * can identify truncated content. */
/* ---------------------------------------------------------------------------
 * The one place bridge memory is taken from
 *
 * BRIDGE_TOTAL_MAX is a budget on what a migration may hold, and `bytes_used`
 * is what it is checked against. An arena is a bump allocator, so a
 * temporary (a raw prefix, an escape workspace) is never handed back: it
 * consumes the physical arena for its whole life. An allocation that is not
 * charged lets the arena run out while the budget believes it has room, and
 * arena_alloc returning NULL mid-parse is how entries get lost silently.
 *
 * So: one function that checks overflow, checks the budget, allocates and
 * charges. Every bridge-owned allocation goes through it. The physical arena
 * ceiling stays as the second line of defence.
 * ------------------------------------------------------------------------- */
static void *bridge_arena_alloc(BridgeHistory *h, usize bytes) {
    if (!h || !h->arena || bytes == 0) return NULL;
    usize used;
    if (!checked_add_usize(h->bytes_used, bytes, &used) ||
        used > BRIDGE_TOTAL_MAX) {
        h->overflowed = true;
        return NULL;
    }
    void *p = arena_alloc(h->arena, bytes);
    if (!p) { h->overflowed = true; return NULL; }
    h->bytes_used = used;
    return p;
}

static const char *dup_capped(BridgeHistory *h, const char *s, usize cap,
                              const char *mark, bool *clipped) {
    if (clipped) *clipped = false;
    if (!h || !h->arena || !s) return NULL;
    usize n = strlen(s);
    if (n == 0) return NULL;

    bool cut = false;
    if (n > cap) {
        n = cap;
        cut = true;
        /* Back off onto a lead byte: every consumer of these strings decodes
         * UTF-8, and a half codepoint would be re-escaped as garbage by the
         * JSON writers downstream. */
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }

    usize mark_len = (cut && mark) ? strlen(mark) : 0;
    usize total;
    if (!checked_add_usize(n, mark_len, &total)) return NULL;
    if (!checked_add_usize(total, 1, &total))    return NULL;

    char *p = (char *)bridge_arena_alloc(h, total);
    if (!p) return NULL;
    memcpy(p, s, n);
    if (mark_len) memcpy(p + n, mark, mark_len);
    p[n + mark_len] = '\0';
    if (clipped) *clipped = cut;
    return p;
}

static const char *dup_plain(BridgeHistory *h, const char *s) {
    return dup_capped(h, s, BRIDGE_MESSAGE_MAX, NULL, NULL);
}

/* A tool call's recorded arguments, kept VALID JSON across the size cap.
 *
 * Plain byte clipping is wrong here: a Bash call whose `command` is a heredoc
 * writing a script runs to tens of kilobytes, the 8 KB cap lands in the
 * middle of the string, and the result is a JSON object with an unterminated
 * value. Every target would then either splice broken JSON into another
 * application's store or fall back to writing it as a string, which
 * OpenCode's importer rejects with `Expected object, got "{…"`.
 *
 * So the clip produces a well-formed object instead. `liu_input` holds as
 * much of the original as fits, ESCAPED, and `liu_truncated` says it was cut.
 * The next agent still sees which tool ran and the legible head of what it
 * was given, which is what a historical record is for.
 *
 * The head alone is not enough to say what the call was ABOUT. A file write
 * is `{"file_path":…,"content":<the bulk>}` in one agent and the same members
 * the other way round in the next, and once the object is a string inside
 * `liu_input` nobody downstream can read a member of it at all, so the
 * handoff would list no large write and no long command, the ones that matter
 * most. So the path and the command are read out of the WHOLE input before it
 * is cut and written beside the body, under the keys the source used. */
static usize tool_subject_members(const char *input, char *out, usize cap) {
    out[0] = '\0';
    if (input[0] != '{') return 0;        /* a raw patch, a bare string */
    cJSON *o = cJSON_Parse(input);
    if (!o) return 0;

    usize off = 0;
    char  val[BRIDGE_TOOL_SUBJECT_MAX + 1];
    for (i32 pass = 0; pass < 2; pass++) {
        const char *key = pass == 0 ? bridge_tool_input_path(o, val, sizeof val)
                                    : bridge_tool_input_command(o, val, sizeof val);
        if (!key) continue;
        int k = snprintf(out + off, cap - off, "\"%s\":\"", key);
        if (k <= 0 || (usize)k >= cap - off) break;
        usize e = jsonw_escape(out + off + (usize)k, cap - off - (usize)k, val);
        if (e == 0 || off + (usize)k + e + 3 > cap) {
            out[off] = '\0';              /* does not fit escaped: leave it out */
            continue;
        }
        off += (usize)k + e;
        out[off++] = '"';
        out[off++] = ',';
        out[off]   = '\0';
    }
    cJSON_Delete(o);
    return off;
}

static const char *dup_tool_input(BridgeHistory *h, const char *s, bool *clipped) {
    if (clipped) *clipped = false;
    if (!h || !h->arena || !s || !s[0]) return NULL;
    if (strlen(s) <= BRIDGE_TOOL_INPUT_MAX)
        return dup_capped(h, s, BRIDGE_TOOL_INPUT_MAX, NULL, NULL);
    if (clipped) *clipped = true;

    /* Two values of BRIDGE_TOOL_SUBJECT_MAX with every newline escaped, and
     * their keys. A value that still does not fit is left out, not cut. */
    char  subject[BRIDGE_TOOL_SUBJECT_MAX * 4 + 128];
    usize subject_len = tool_subject_members(s, subject, sizeof subject);

    /* Escape a prefix that leaves room for the wrapper, the subject and
     * worst-case expansion (a control byte becomes six characters). The cut
     * must not land inside a code point, and it is the SOURCE that says
     * whether it does: the byte after the cut. */
    usize raw_keep = (BRIDGE_TOOL_INPUT_MAX - subject_len) / 6;
    while (raw_keep > 0 && ((unsigned char)s[raw_keep] & 0xC0) == 0x80) raw_keep--;
    char *raw = (char *)bridge_arena_alloc(h, raw_keep + 1);
    if (!raw) return NULL;
    memcpy(raw, s, raw_keep);
    raw[raw_keep] = '\0';

    usize esc_cap = BRIDGE_TOOL_INPUT_MAX;
    char *esc = (char *)bridge_arena_alloc(h, esc_cap);
    if (!esc) return NULL;
    if (jsonw_escape(esc, esc_cap, raw) == 0 && raw[0]) {
        /* Would not fit even escaped; report the truncation with no body
         * rather than emit something unparseable. */
        return dup_capped(h, "{\"liu_truncated\":true}", 64, NULL, NULL);
    }

    usize need = strlen(esc) + subject_len + 64;
    char *out = (char *)bridge_arena_alloc(h, need);
    if (!out) return NULL;
    int n = snprintf(out, need, "{\"liu_truncated\":true,%s\"liu_input\":\"%s\"}",
                     subject, esc);
    if (n <= 0 || (usize)n >= need) return NULL;
    return out;
}

/* ------------------------------------------------------------------------- */

void bridge_history_set_session_id(BridgeHistory *h, const char *id) {
    if (!h || !id || !id[0]) return;
    const char *p = dup_plain(h, id);
    if (p) h->source_session_id = p;
}

void bridge_history_set_path(BridgeHistory *h, const char *path) {
    if (!h || !path || !path[0]) return;
    const char *p = dup_plain(h, path);
    if (p) h->source_path = p;
}

void bridge_history_set_cwd(BridgeHistory *h, const char *cwd) {
    if (!h || !cwd || !cwd[0]) return;
    const char *p = dup_plain(h, cwd);
    if (p) h->cwd = p;
}

void bridge_history_set_title(BridgeHistory *h, const char *title) {
    if (!h || !title || !title[0]) return;

    /* A title is a ONE-LINE label. The fallback derives it from the first
     * user turn, which is routinely a multi-line prompt, and every consumer
     * treats it as a single field: a picker row, a session name, a JSON
     * value in another agent's store. Newlines and runs of whitespace are
     * therefore collapsed here rather than at each of those, and the cap is
     * applied after the collapse so a wrapped prompt does not spend its whole
     * budget on indentation. */
    char flat[512];
    usize o = 0;
    bool space = false;
    for (const char *s = title; *s && o + 1 < sizeof flat; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') { space = true; continue; }
        if (c < 0x20) continue;                    /* control bytes */
        if (space && o > 0) flat[o++] = ' ';
        space = false;
        if (o + 1 < sizeof flat) flat[o++] = (char)c;
    }
    /* Trailing whitespace is dropped by construction (space is only emitted
     * before a following character). */
    flat[o] = '\0';
    if (o == 0) return;

    /* Then a LABEL-length cap, not a field-length one. The fallback derives
     * the title from the first user turn, and a field-length title would
     * fill a session-list column with one prompt. 120 is about a picker
     * row; a source-supplied title (OpenCode's "Summary of tab_popup.c and
     * layout.h", Grok's session_summary) is far under it and is unaffected.
     * The ellipsis is appended by the same UTF-8-safe clip everything else
     * uses. */
    const char *p = dup_capped(h, flat, 120, "\xe2\x80\xa6", NULL);
    if (p) h->title = p;
}

void bridge_history_set_model(BridgeHistory *h, const char *model) {
    if (!h || !model || !model[0]) return;
    const char *p = dup_capped(h, model, 128, NULL, NULL);
    if (p) h->source_model = p;
}

void bridge_history_set_git(BridgeHistory *h, const char *root,
                            const char *branch, const char *head) {
    if (!h) return;
    if (root   && root[0])   { const char *p = dup_plain(h, root);   if (p) h->git_root   = p; }
    if (branch && branch[0]) { const char *p = dup_capped(h, branch, 256, NULL, NULL); if (p) h->git_branch = p; }
    if (head   && head[0])   { const char *p = dup_capped(h, head,    64, NULL, NULL); if (p) h->git_head   = p; }
}

/* ------------------------------------------------------------------------- */

static bool events_reserve(BridgeHistory *h) {
    if (h->event_count < h->event_cap) return true;
    usize ncap = h->event_cap ? h->event_cap * 2 : 64;
    if (ncap > BRIDGE_MAX_EVENTS) ncap = BRIDGE_MAX_EVENTS;
    if (ncap <= h->event_cap) return false;
    usize bytes;
    if (!checked_mul_usize(ncap, sizeof *h->events, &bytes)) return false;
    BridgeEvent *p = realloc(h->events, bytes);
    if (!p) return false;
    h->events   = p;
    h->event_cap = ncap;
    return true;
}

static bool is_tool_event(BridgeEventType t) {
    return t == BRIDGE_EVENT_TOOL_CALL || t == BRIDGE_EVENT_TOOL_RESULT;
}

bool bridge_history_push(BridgeHistory *h, const BridgeEvent *e) {
    if (!h || !e || !h->arena) return false;

    if (h->event_count >= BRIDGE_MAX_EVENTS || h->overflowed) {
        h->fidelity.events_dropped++;
        h->fidelity.history_complete = false;
        return false;
    }

    /* An event with neither text nor tool payload carries nothing a target
     * could write, so it is not a drop — there was never anything there. */
    bool has_payload = (e->text && e->text[0]) ||
                       (e->tool_name && e->tool_name[0]) ||
                       (e->tool_input && e->tool_input[0]) ||
                       (e->tool_output && e->tool_output[0]);
    if (!has_payload) return false;

    if (!events_reserve(h)) {
        h->fidelity.events_dropped++;
        h->fidelity.history_complete = false;
        return false;
    }

    BridgeEvent out;
    memset(&out, 0, sizeof out);
    out.type         = e->type;
    out.timestamp_ms = e->timestamp_ms;
    out.tool_kind    = e->tool_kind;
    out.tool_failed  = e->tool_failed;

    bool clipped = false;

    /* USER and ASSISTANT messages use the larger text cap. Truncation is
     * recorded in message_truncated and marks the migration PARTIAL, because
     * the imported conversation is incomplete. */
    if (e->text && e->text[0]) {
        usize cap = (e->type == BRIDGE_EVENT_USER ||
                     e->type == BRIDGE_EVENT_ASSISTANT)
                        ? BRIDGE_MESSAGE_MAX : BRIDGE_TOOL_OUTPUT_MAX;
        out.text = dup_capped(h, e->text, cap,
                              cap == BRIDGE_MESSAGE_MAX ? NULL : BRIDGE_TRUNC_MARK,
                              &clipped);
        if (clipped) {
            if (e->type == BRIDGE_EVENT_USER || e->type == BRIDGE_EVENT_ASSISTANT)
                h->fidelity.messages_truncated++;
            else
                h->fidelity.tool_outputs_truncated++;
        }
    }

    if (e->tool_name && e->tool_name[0])
        out.tool_name = dup_capped(h, e->tool_name, 128, NULL, NULL);

    if (e->tool_input && e->tool_input[0]) {
        /* A clipped INPUT is the same grade of loss as a clipped output — the
         * call, what it was about and the fact that it ran all survive — and
         * it is counted with them, so a migration that cut every file write
         * short cannot still say "full". */
        out.tool_input = dup_tool_input(h, e->tool_input, &clipped);
        if (clipped) h->fidelity.tool_outputs_truncated++;
    }

    if (e->tool_output && e->tool_output[0]) {
        out.tool_output = dup_capped(h, e->tool_output, BRIDGE_TOOL_OUTPUT_MAX,
                                     BRIDGE_TRUNC_MARK, &clipped);
        out.tool_output_truncated = clipped || e->tool_output_truncated;
        if (out.tool_output_truncated) h->fidelity.tool_outputs_truncated++;
    }

    if (e->raw_payload && e->raw_payload[0])
        out.raw_payload = dup_capped(h, e->raw_payload, BRIDGE_TOOL_OUTPUT_MAX,
                                     NULL, NULL);

    /* Call id. A target that writes tool records into a native format needs
     * the pairing; a source that never stored one (Cursor, the Cline
     * extension) would otherwise produce calls no result can attach to. */
    if (is_tool_event(e->type)) {
        if (e->tool_call_id && e->tool_call_id[0]) {
            out.tool_call_id = dup_capped(h, e->tool_call_id, 128, NULL, NULL);
        } else {
            char synth[48];
            snprintf(synth, sizeof synth, "liucall_%u", h->synth_call_seq++);
            out.tool_call_id = dup_capped(h, synth, sizeof synth, NULL, NULL);
        }
    }

    /* If the arena cannot copy a complete event, drop the event and
     * increment events_dropped. Partial event records are not published. */
    if (h->overflowed) {
        h->fidelity.events_dropped++;
        h->fidelity.history_complete = false;
        return false;
    }

    h->events[h->event_count++] = out;

    switch (out.type) {
        case BRIDGE_EVENT_USER:
        case BRIDGE_EVENT_ASSISTANT:   h->fidelity.messages++;     break;
        case BRIDGE_EVENT_TOOL_CALL:   h->fidelity.tool_calls++;   break;
        case BRIDGE_EVENT_TOOL_RESULT: h->fidelity.tool_results++; break;
        default: break;
    }
    return true;
}

bool bridge_history_set_file_change(BridgeHistory *h, usize index,
                                    const char *patch, bool created) {
    if (!h || index >= h->event_count) return false;
    BridgeEvent *e = &h->events[index];
    if (e->type != BRIDGE_EVENT_TOOL_RESULT) return false;
    const char *copy = NULL;
    if (patch && patch[0]) {
        usize n = strlen(patch);
        /* An optional extra: past the budget it is left out, so it can never
         * set `overflowed` and cost the conversation that follows. */
        if (n > BRIDGE_TOOL_OUTPUT_MAX || h->overflowed ||
            h->bytes_used + n + 1 > BRIDGE_TOTAL_MAX) return false;
        copy = dup_capped(h, patch, BRIDGE_TOOL_OUTPUT_MAX, NULL, NULL);
        if (!copy) return false;
    }
    e->tool_patch = copy;
    e->tool_file_created = created;
    return true;
}

bool bridge_push_message(BridgeHistory *h, BridgeEventType type,
                         i64 ts_ms, const char *text) {
    BridgeEvent e = {0};
    e.type = type;
    e.timestamp_ms = ts_ms;
    e.text = text;
    return bridge_history_push(h, &e);
}

/* The two tool wrappers NORMALIZE the name rather than leaving tool_kind at
 * UNKNOWN. A convenience helper that silently drops the semantic kind is a
 * trap: every target renames a call through bridge_tool_name_for(kind), and
 * the handoff writer decides what the previous agent CHANGED from
 * bridge_tool_kind_is_mutating(kind) — so a call pushed through here without
 * a kind would arrive in the target under its foreign name and vanish from
 * the "files changed" section. The source tool is already on the history, so
 * there is nothing for a caller to supply. */
bool bridge_push_tool_call(BridgeHistory *h, i64 ts_ms, const char *name,
                           const char *call_id, const char *input_json) {
    if (!h) return false;
    BridgeEvent e = {0};
    e.type = BRIDGE_EVENT_TOOL_CALL;
    e.timestamp_ms = ts_ms;
    e.tool_name    = name;
    e.tool_call_id = call_id;
    e.tool_input   = input_json;
    e.tool_kind    = bridge_tool_kind_for(h->source_tool, name);
    return bridge_history_push(h, &e);
}

bool bridge_push_tool_result(BridgeHistory *h, i64 ts_ms, const char *name,
                             const char *call_id, const char *output) {
    if (!h) return false;
    BridgeEvent e = {0};
    e.type = BRIDGE_EVENT_TOOL_RESULT;
    e.timestamp_ms = ts_ms;
    e.tool_name    = name;
    e.tool_call_id = call_id;
    e.tool_output  = output;
    e.tool_kind    = bridge_tool_kind_for(h->source_tool, name);
    return bridge_history_push(h, &e);
}

void bridge_history_finish(BridgeHistory *h) {
    if (!h) return;

    if (h->overflowed) h->fidelity.history_complete = false;

    /* Metadata is "complete" when a target can place the session correctly:
     * where it ran and what it was about. The model name is informational —
     * every target picks its own model — so it is deliberately not required. */
    h->fidelity.metadata_complete =
        (h->cwd && h->cwd[0]) && (h->title && h->title[0]);

    if (h->fidelity.events_dropped > 0 || h->fidelity.messages_truncated > 0)
        h->fidelity.history_complete = false;
}

bool bridge_history_is_empty(const BridgeHistory *h) {
    if (!h || h->event_count == 0) return true;
    for (usize i = 0; i < h->event_count; i++) {
        BridgeEventType t = h->events[i].type;
        if (t == BRIDGE_EVENT_USER || t == BRIDGE_EVENT_ASSISTANT) return false;
    }
    return true;
}

isize bridge_history_last_of(const BridgeHistory *h, BridgeEventType type) {
    if (!h) return -1;
    for (usize i = h->event_count; i > 0; i--) {
        if (h->events[i - 1].type == type) return (isize)(i - 1);
    }
    return -1;
}
