/*
 * Liu - string utilities
 * Non-allocating string view + basic operations.
 */
#ifndef CORE_STRING_UTILS_H
#define CORE_STRING_UTILS_H

#include "types.h"
#include "memory.h"

/* Non-owning string view (pointer + length, not null-terminated) */
typedef struct {
    const char *data;
    usize       len;
} Str;

#define STR_LIT(s) ((Str){ .data = (s), .len = sizeof(s) - 1 })
#define STR_NULL    ((Str){ .data = NULL, .len = 0 })

Str    str_from_cstr(const char *s);
i64    str_find(Str haystack, Str needle);

/* String builder (arena-backed) */
typedef struct {
    Arena *arena;
    char  *data;
    usize  len;
    usize  cap;
} StringBuilder;

void          sb_append(StringBuilder *sb, Str s);

/* Base64 encode/decode */
i32  base64_decode(const char *input, i32 input_len, u8 *output, i32 output_max);
i32  base64_encode(const u8 *input, i32 input_len, char *output, i32 output_max);

/* Path join: writes "dir/name" into out. Always joins with '/' — Win32 file
 * APIs accept forward slashes, and existing code already mixes the two.
 * Returns false on truncation (out is still NUL-terminated). */
bool liu_path_join(char *out, usize cap, const char *dir, const char *name);

/* Lexically normalise a path: collapse "//", drop "." components and resolve
 * ".." against the preceding component. Purely textual — no filesystem access,
 * so it is safe to call per frame and works for paths that no longer exist.
 * Symlinks are NOT resolved (that needs realpath); this exists for comparing
 * paths that differ only in spelling. Returns false on truncation. */
bool liu_path_normalize(char *out, usize cap, const char *path);

/* Compare paths after lexical normalization so equivalent spellings, such as a
 * path containing a parent component, match without resolving symlinks or
 * accessing the filesystem. */
bool liu_path_equiv(const char *a, const char *b);

#endif /* CORE_STRING_UTILS_H */
