/*
 * Liu — a JSON object laid out one member a line.
 *
 * Liu's own settings files are read a line at a time: config.json by
 * config_parse_kv and its sscanf chain, keybindings.json by keybind_load. The
 * files Liu writes have one member a line, but a file written elsewhere may
 * not: a minified file — one line, the whole object on it, what a formatter or
 * a dotfiles tool leaves — would read as nothing at all, and the next save
 * would replace it with the defaults.
 *
 * So the text is laid out before it is read: outside a string, the top-level
 * object's opening brace, each comma between its members and its closing brace
 * end a line. A nested value stays on its member's line, and a string is never
 * touched (an escaped quote does not end one). A file that already has one
 * member a line gains blank lines and nothing else. The layout is pure;
 * json_read_file is the read both loaders share.
 */
#ifndef LIU_CORE_JSON_LINES_H
#define LIU_CORE_JSON_LINES_H

#include "core/types.h"

#include <stdio.h>
#include <stdlib.h>

/* `src` (n bytes) laid out, NUL-terminated, in a malloc'd buffer of at most
 * 2n + 1 bytes; its length in `*out_n`. NULL when memory runs out. */
static inline char *json_lay_out_members(const char *src, usize n, usize *out_n) {
    char *out = (char *)malloc(2 * n + 1);
    if (!out) return NULL;
    usize o = 0;
    i32 depth = 0;
    bool in_str = false, esc = false;
    for (usize i = 0; i < n; i++) {
        char c = src[i];
        if (in_str) {
            out[o++] = c;
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; out[o++] = c; continue; }
        if (c == '{' || c == '[') {
            out[o++] = c;
            if (c == '{' && depth == 0) out[o++] = '\n';
            depth++;
            continue;
        }
        if (c == '}' || c == ']') {
            if (c == '}' && depth == 1) out[o++] = '\n';
            out[o++] = c;
            if (depth > 0) depth--;
            continue;
        }
        out[o++] = c;
        if (c == ',' && depth == 1) out[o++] = '\n';
    }
    out[o] = '\0';
    *out_n = o;
    return out;
}

/* A whole settings file, NUL-terminated, in a malloc'd buffer; its length in
 * `*n_out`. NULL when it is missing, unreadable or longer than `max` — the
 * files are hand-editable, not unbounded. */
static inline char *json_read_file(const char *path, usize max, usize *n_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    usize cap = 4096, n = 0;
    char *buf = (char *)malloc(cap);
    while (buf) {
        if (n + 1 >= cap) {
            if (cap >= max) { free(buf); buf = NULL; break; }
            char *grown = (char *)realloc(buf, cap * 2);
            if (!grown) { free(buf); buf = NULL; break; }
            buf = grown;
            cap *= 2;
        }
        usize got = fread(buf + n, 1, cap - 1 - n, f);
        n += got;
        if (got == 0) {
            if (ferror(f)) { free(buf); buf = NULL; }
            break;
        }
    }
    fclose(f);
    if (!buf) return NULL;
    buf[n] = '\0';
    *n_out = n;
    return buf;
}

#endif /* LIU_CORE_JSON_LINES_H */
