/*
 * liu-history/bridge - streaming JSON writer for target session files.
 *
 * Every native-format writer here emits into another application's session
 * store, so this is deliberately not cJSON: building a 24 MB cJSON tree only
 * to print it doubles peak memory over a transcript that is already the
 * largest thing in the process, and cJSON_PrintUnformatted has no failure
 * mode short of returning NULL after it has already allocated everything.
 * A streaming writer costs one buffer.
 *
 * The destination half matters as much as the serializer. `jsonw_open_file`
 * routes through core/atomic_file.h's private variant, which
 *   - creates the parent directory 0700 when needed,
 *   - writes a sibling temp file and rename(2)s it into place, so a target
 *     agent scanning its own store never sees a half-written session,
 *   - chmods the destination 0600,
 * and this file adds an O_NOFOLLOW check on the destination so a symlink
 * planted at the target path cannot redirect the write. See jsonw_path_safe.
 */
#ifndef HISTORY_BRIDGE_JSONW_H
#define HISTORY_BRIDGE_JSONW_H

#include "core/atomic_file.h"
#include "core/types.h"

#include <stdio.h>

typedef struct {
    FILE *f;
    AtomicFileWriter atomic;
    bool  atomic_open;
    bool  failed;
    /* Per-container comma state. JSON's separator rule is positional, and a
     * writer that tracks it with a single flag emits `[1,2],[3` the moment a
     * nested array closes — so the depth is explicit. */
    u8    need_comma[32];
    i32   depth;
} JsonW;

/* Open `path` for atomic private (0600) writing. Refuses a destination that
 * is a symlink, and the atomic writer refuses a symlinked parent. Returns
 * false on failure; the writer is then inert and every call on it is a
 * no-op. */
bool jsonw_open_file(JsonW *w, const char *path);

/* Write into an already-open stream for writers that emit multiple
 * documents into one JSONL file. jsonw_close does not close the stream. */
void jsonw_open_stream(JsonW *w, FILE *f);

/* Commit (atomic rename) or, when `abort_write` is true, discard. Returns
 * true only when everything written succeeded AND the commit succeeded. */
bool jsonw_close(JsonW *w, bool abort_write);

/* Containers. `key` is NULL inside an array, non-NULL inside an object. */
void jsonw_obj_begin(JsonW *w, const char *key);
void jsonw_obj_end  (JsonW *w);
void jsonw_arr_begin(JsonW *w, const char *key);
void jsonw_arr_end  (JsonW *w);

/* Scalars. A NULL string value writes JSON null, which is what every target
 * schema here means by an absent optional field. */
void jsonw_str  (JsonW *w, const char *key, const char *value);
void jsonw_strn (JsonW *w, const char *key, const char *value, usize len);
void jsonw_i64  (JsonW *w, const char *key, i64 value);
void jsonw_f64  (JsonW *w, const char *key, f64 value);
void jsonw_bool (JsonW *w, const char *key, bool value);
void jsonw_null (JsonW *w, const char *key);

/* End one document of a JSONL file: writes '\n' and clears the separator
 * state so the next document starts fresh. Without the reset the writer would
 * emit a comma before the second object, because at depth 0 it has no
 * container to tell it a new document began. */
void jsonw_raw_newline(JsonW *w);

/* Splice a pre-formed JSON document in as a value. Used for a tool call's
 * recorded arguments, which the source already stored as JSON — reparsing
 * and re-emitting them would only be a chance to corrupt them.
 *
 * `raw` is VALIDATED as well-formed JSON before it is spliced (jsonw_is_json);
 * anything else is written as a JSON string instead, because splicing an
 * unvalidated blob into another application's session file is an injection
 * primitive. Callers therefore never need to trust their input. */
void jsonw_raw_or_str(JsonW *w, const char *key, const char *raw);

/* Like jsonw_raw_or_str, but for a field the target schema requires to be an
 * OBJECT. Splices `raw` when it is a well-formed JSON object; otherwise emits
 * `{"liu_input": "<raw as a string>"}` so the field is still an object.
 *
 * The distinction matters: OpenCode's importer rejects a whole migration with
 * `Expected object, got "{\"command\":…"` when a clipped tool input is no
 * longer valid JSON and is written as a string, reported as one line with no
 * indication of which record caused it. Any field a target validates as an
 * object must go through this. */
void jsonw_obj_or_wrap(JsonW *w, const char *key, const char *raw);

/* Cheap well-formedness check over a complete JSON document. Not a parser:
 * it verifies structure, string escaping and nesting depth, which is exactly
 * what "is it safe to splice" needs. */
bool jsonw_is_json(const char *s);

/* Escape `s` into `out` as the CONTENTS of a JSON string (no quotes).
 * Returns the number of bytes written, or 0 when it did not fit. */
usize jsonw_escape(char *out, usize cap, const char *s);

/* True when `path` is safe to write: absolute, no "..", and neither the
 * final component nor any parent directory is a symlink. */
bool jsonw_path_safe(const char *path);

/* True when the writer has seen no error so far. */
bool jsonw_ok(const JsonW *w);

#endif /* HISTORY_BRIDGE_JSONW_H */
