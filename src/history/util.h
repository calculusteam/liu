/*
 * liu-history - small helpers shared across parsers/scanner.
 */
#ifndef HISTORY_UTIL_H
#define HISTORY_UTIL_H

#include "core/memory.h"
#include "core/types.h"
#include <stddef.h>
#include <stdio.h>

/* Forward-declare cJSON so callers don't need the header just to declare a
 * pointer. cJSON is a concrete struct tag, so this works in both headers. */
typedef struct cJSON cJSON;

/* arena-allocated strdup / strndup; return NULL on NULL input or OOM. */
char *hist_strdup (Arena *a, const char *s);
char *hist_strndup(Arena *a, const char *s, usize n);

/* Parse an ISO 8601 timestamp → milliseconds since the epoch; 0 when it is
 * not one. "YYYY-MM-DDTHH:MM:SS" (a space may stand for the T), an optional
 * fraction of any length (cut to milliseconds), and "Z", "±HH:MM", "±HHMM",
 * "±HH" or nothing (read as UTC). */
i64  hist_parse_iso8601_ms(const char *iso);

/* Copy `src` into fixed buffer `dst`[cap], always NUL-terminating.
 * Truncates silently; returns bytes copied (excluding NUL). */
usize hist_copy_bounded(char *dst, usize cap, const char *src);

/* Resolve a home-anchored path. `rel` is something like ".claude/projects".
 * Writes "<HOME>/<rel>" into `out` (NUL-terminated). Returns true on success. */
bool hist_home_path(char *out, usize cap, const char *rel);

/* Read an env override for a directory and canonicalize (strip trailing '/'),
 * or return NULL if unset/empty. */
const char *hist_env_dir(const char *name);

/* Resolve a scanner root: prefer "$<env_name>/<env_suffix>"; fall back to
 * "~/<home_rel>". Returns true on success. */
bool hist_resolve_root(char *out, usize cap,
                       const char *env_name, const char *env_suffix,
                       const char *home_rel);

/* "$<env_name>/<rel>" when the env var is set, else "~/<home_def_rel>/<rel>".
 * The XDG base-dir pattern — kept un-gated by platform because several tools
 * (amp, opencode) honor $XDG_DATA_HOME on every OS including Windows. */
bool hist_env_or_home_path(char *out, usize cap, const char *env_name,
                           const char *home_def_rel, const char *rel);

/* XDG conveniences over hist_env_or_home_path. */
bool hist_xdg_data_path  (char *out, usize cap, const char *rel); /* .local/share */
bool hist_xdg_config_path(char *out, usize cap, const char *rel); /* .config */
bool hist_xdg_state_path (char *out, usize cap, const char *rel); /* .local/state */

/* Per-OS desktop-app data roots, "<base>/<rel>":
 *   hist_appdata_path        — ROAMING base: mac ~/Library/Application Support,
 *                              win %APPDATA%, linux $XDG_CONFIG_HOME|~/.config.
 *                              (VS Code-family user data lives here.)
 *   hist_local_appdata_path  — LOCAL base (Rust dirs::data_local_dir()):
 *                              mac ~/Library/Application Support,
 *                              win %LOCALAPPDATA%,
 *                              linux $XDG_DATA_HOME|~/.local/share. */
bool hist_appdata_path      (char *out, usize cap, const char *rel);
bool hist_local_appdata_path(char *out, usize cap, const char *rel);

/* cJSON conveniences — return NULL / false when key absent or type mismatched. */
const char *hist_cjson_str (cJSON *obj, const char *key);
bool        hist_cjson_bool(cJSON *obj, const char *key);

/* Convert a JSON number to i64 without C's undefined out-of-range float
 * conversion. Fractional values are truncated toward zero. */
bool        hist_cjson_i64(cJSON *obj, const char *key, i64 *out);

/* Serialize a cJSON node to a compact arena-owned string. NULL on empty/OOM. */
char *hist_cjson_serialize_compact(Arena *a, cJSON *node);

/* Lowercase MD5 hex of `len` bytes, written to `out[33]` (32 chars + NUL).
 *
 * Not a security primitive — it exists because Kimi CLI names each work
 * directory's session bucket `md5(<canonical work dir>)` (kimi_cli
 * metadata.py, WorkDirMeta.sessions_dir), so resolving a tab's live Kimi
 * session means reproducing that digest exactly. Do not reach for this for
 * anything that needs collision or preimage resistance. */
void hist_md5_hex(const void *data, usize len, char out[33]);

/* Read a whole regular file into an arena-allocated NUL-terminated buffer.
 * Refuses to read files larger than `max_bytes`. Returns true on success and
 * sets *out_buf / *out_len. */
bool hist_slurp_file(Arena *a, const char *path, usize max_bytes,
                     char **out_buf, usize *out_len);

/* Same, but into a malloc'd buffer the caller frees.
 *
 * Use this when the bytes die before the arena does — the whole-file JSON
 * parsers are the case that matters: cJSON copies every string it keeps, so
 * the source is dead the moment the parse returns, but arena memory cannot be
 * handed back piecemeal. Slurping a 20 MB ui_messages.json into the arena kept
 * the file resident alongside the cJSON tree and the per-event dups, all three
 * live at once. Returns false (and allocates nothing) on any read error. */
bool hist_slurp_file_heap(const char *path, usize max_bytes,
                          char **out_buf, usize *out_len);

/* Lowercase-dash slug of an absolute path, as CommandCode names its project
 * directories: lowercase, every run of non-[a-z0-9] collapsed to one '-',
 * leading/trailing dashes trimmed, empty result → "root"
 * (cli.mjs `slugify`, used by `getProjectDirName`). So
 * "/Volumes/Work/todo-app" → "volumes-work-todo-app".
 *
 * The transform is lossy in two directions at once — case is gone and '/'
 * is indistinguishable from '-' — so it must never be inverted. Compare in
 * the slug direction instead: slug the known cwd and match it against the
 * stored directory name. Returns false only when out/cap or path is unusable. */
bool hist_slugify_lower(char *out, usize cap, const char *path);

/* The name Claude Code gives a project directory: EVERY byte of the path that
 * is not [A-Za-z0-9] becomes '-', one dash per UTF-16 code unit of the path (a
 * BMP character is one, an astral one two) — no case folding, no collapsing of
 * runs, no trimming. A '.' and a ' ' become dashes exactly like '/' does,
 * which is why the mapping cannot be reversed. Returns false when `out` is too
 * small (a truncated slug would name a different project). */
bool hist_slugify_claude(char *out, usize cap, const char *path);


/* ---------------------------------------------------------------------------
 * Bounded line reader
 *
 * getline() grows its buffer to whatever the file hands it BEFORE the caller
 * can apply a logical line cap, so a single huge line in a session store would
 * be allocated in full before it could be skipped. This reader makes the cap
 * a real allocation limit: the line buffer never exceeds `max_bytes + 1`, and
 * an overlong line is discarded as it streams past, so the reader
 * resynchronises onto the next record instead of aborting the file.
 *
 * It owns its own read buffer over a file descriptor rather than wrapping
 * getline(FILE*): the fast path is one memchr per chunk plus one memcpy per
 * line, well ahead of a getc_unlocked loop over stdio. It is also exact with
 * embedded NUL bytes, which any strlen/fgets-based reader is not.
 *
 * Lifetime: attach() takes ownership of the fd (close() releases it). The
 * pointer handed back by next() is owned by the reader and is valid until the
 * next call to next()/close(); it is always NUL-terminated at *out_len. */
typedef enum {
    HIST_LINE_OK = 0,       /* a line was read; *out_len is its length      */
    HIST_LINE_EOF,          /* clean end of file; nothing was read          */
    HIST_LINE_TOO_LONG,     /* line exceeded max_bytes; it has been skipped */
    HIST_LINE_IO_ERROR      /* read error or allocation failure             */
} HistLineResult;

typedef struct {
    int    fd;
    char  *line;            /* grows geometrically, capped at max_bytes+1 */
    usize  line_cap;
    char  *chunk;           /* raw read buffer, fixed size */
    usize  chunk_len, chunk_pos;
    bool   eof, error;
} HistLineReader;

/* Take ownership of `fd`. Returns false (and closes nothing) on bad args or
 * when the read buffer cannot be allocated. */
bool hist_line_reader_attach(HistLineReader *r, int fd);

/* Open `path` read-only. `nofollow` refuses symlinks (O_NOFOLLOW) — the guard
 * the agent-store readers use so a planted symlink cannot redirect the read.
 * Non-regular files are always refused. */
bool hist_line_reader_open(HistLineReader *r, const char *path, bool nofollow);

/* Read the next line. One trailing '\n' and then one trailing '\r' are removed
 * (exactly what every call site did by hand). On HIST_LINE_TOO_LONG the line
 * has been consumed and skipped; keep calling to reach the next record. */
HistLineResult hist_line_reader_next(HistLineReader *r, usize max_bytes,
                                     const char **out_line, usize *out_len);

void hist_line_reader_close(HistLineReader *r);

#endif
