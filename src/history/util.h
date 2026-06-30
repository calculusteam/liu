/*
 * liu-history - small helpers shared across parsers/scanner.
 */
#ifndef HISTORY_UTIL_H
#define HISTORY_UTIL_H

#include "core/memory.h"
#include "core/types.h"
#include <stddef.h>

/* Forward-declare cJSON so callers don't need the header just to declare a
 * pointer. cJSON is a concrete struct tag, so this works in both headers. */
typedef struct cJSON cJSON;

/* arena-allocated strdup / strndup; return NULL on NULL input or OOM. */
char *hist_strdup (Arena *a, const char *s);
char *hist_strndup(Arena *a, const char *s, usize n);

/* Parse "YYYY-MM-DDTHH:MM:SS(.ms)?Z" → milliseconds since epoch. 0 on failure. */
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

/* Serialize a cJSON node to a compact arena-owned string. NULL on empty/OOM. */
char *hist_cjson_serialize_compact(Arena *a, cJSON *node);

/* Read a whole regular file into an arena-allocated NUL-terminated buffer.
 * Refuses to read files larger than `max_bytes`. Returns true on success and
 * sets *out_buf / *out_len. */
bool hist_slurp_file(Arena *a, const char *path, usize max_bytes,
                     char **out_buf, usize *out_len);

#endif
