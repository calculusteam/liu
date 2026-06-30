/*
 * Liu - string utilities implementation
 */
#include "string_utils.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

Str str_from_cstr(const char *s) {
    if (!s) return STR_NULL;
    return (Str){ .data = s, .len = strlen(s) };
}

char *str_to_cstr(Str s, Arena *a) {
    char *buf = (char *)arena_alloc(a, s.len + 1);
    if (!buf) return NULL;
    memcpy(buf, s.data, s.len);
    buf[s.len] = '\0';
    return buf;
}

bool str_eq(Str a, Str b) {
    if (a.len != b.len) return false;
    return memcmp(a.data, b.data, a.len) == 0;
}

bool str_eq_nocase(Str a, Str b) {
    if (a.len != b.len) return false;
    for (usize i = 0; i < a.len; i++) {
        if (tolower((unsigned char)a.data[i]) != tolower((unsigned char)b.data[i])) {
            return false;
        }
    }
    return true;
}

bool str_starts_with(Str s, Str prefix) {
    if (prefix.len > s.len) return false;
    return memcmp(s.data, prefix.data, prefix.len) == 0;
}

bool str_ends_with(Str s, Str suffix) {
    if (suffix.len > s.len) return false;
    return memcmp(s.data + s.len - suffix.len, suffix.data, suffix.len) == 0;
}

bool str_contains(Str haystack, Str needle) {
    return str_find(haystack, needle) >= 0;
}

Str str_trim(Str s) {
    while (s.len > 0 && isspace((unsigned char)s.data[0])) {
        s.data++;
        s.len--;
    }
    while (s.len > 0 && isspace((unsigned char)s.data[s.len - 1])) {
        s.len--;
    }
    return s;
}

Str str_substr(Str s, usize start, usize len) {
    if (start >= s.len) return STR_NULL;
    if (start + len > s.len) len = s.len - start;
    return (Str){ .data = s.data + start, .len = len };
}

i64 str_find(Str haystack, Str needle) {
    if (needle.len == 0) return 0;
    if (needle.len > haystack.len) return -1;
    for (usize i = 0; i <= haystack.len - needle.len; i++) {
        if (memcmp(haystack.data + i, needle.data, needle.len) == 0) {
            return (i64)i;
        }
    }
    return -1;
}

/* =========================================================================
 * String builder
 * ========================================================================= */

StringBuilder sb_create(Arena *a, usize initial_cap) {
    StringBuilder sb = {0};
    sb.arena = a;
    sb.cap   = initial_cap;
    sb.data  = (char *)arena_alloc(a, initial_cap);
    sb.len   = 0;
    return sb;
}

void sb_append(StringBuilder *sb, Str s) {
    if (sb->len + s.len > sb->cap) {
        /* Arena doesn't support realloc, so we re-allocate a larger block.
         * The old block is wasted (arena reclaims it on reset). */
        usize new_cap = (sb->len + s.len) * 2;
        char *new_data = (char *)arena_alloc(sb->arena, new_cap);
        if (!new_data) return;
        memcpy(new_data, sb->data, sb->len);
        sb->data = new_data;
        sb->cap  = new_cap;
    }
    memcpy(sb->data + sb->len, s.data, s.len);
    sb->len += s.len;
}

void sb_append_cstr(StringBuilder *sb, const char *s) {
    sb_append(sb, str_from_cstr(s));
}

void sb_append_char(StringBuilder *sb, char c) {
    sb_append(sb, (Str){ .data = &c, .len = 1 });
}

void sb_append_u32(StringBuilder *sb, u32 val) {
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%u", val);
    sb_append(sb, (Str){ .data = buf, .len = (usize)len });
}

Str sb_to_str(const StringBuilder *sb) {
    return (Str){ .data = sb->data, .len = sb->len };
}

/* =========================================================================
 * Path utilities
 * ========================================================================= */

bool liu_path_join(char *out, usize cap, const char *dir, const char *name) {
    if (!out || cap == 0) return false;
    i32 n = snprintf(out, cap, "%s/%s", dir ? dir : "", name ? name : "");
    return n >= 0 && (usize)n < cap;
}

/* =========================================================================
 * Base64 encode / decode
 * ========================================================================= */

static const char b64_encode_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Returns 0-63 for valid chars, -1 for '=', -2 for invalid */
static i32 b64_decode_char(u8 c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -1;
    return -2;
}

i32 base64_decode(const char *input, i32 input_len, u8 *output, i32 output_max) {
    if (!input || input_len <= 0 || !output || output_max <= 0) return -1;

    i32 out_len = 0;
    i32 i = 0;

    while (i < input_len) {
        /* Skip whitespace */
        while (i < input_len && ((u8)input[i] <= ' ')) i++;
        if (i >= input_len) break;

        /* Collect 4 base64 characters */
        i32 vals[4] = {0, 0, 0, 0};
        i32 pad = 0;
        i32 j;
        for (j = 0; j < 4 && i < input_len; ) {
            u8 c = (u8)input[i++];
            if (c <= ' ') continue; /* skip whitespace */
            i32 v = b64_decode_char(c);
            if (v == -2) return -1; /* invalid character */
            if (v == -1) { pad++; vals[j++] = 0; } /* padding */
            else { vals[j++] = v; }
        }
        if (j < 4) return -1; /* truncated input */

        /* Decode 3 bytes from 4 base64 chars */
        u32 triple = ((u32)vals[0] << 18) | ((u32)vals[1] << 12) |
                     ((u32)vals[2] << 6)  | (u32)vals[3];

        if (pad <= 2 && out_len < output_max) output[out_len++] = (u8)(triple >> 16);
        if (pad <= 1 && out_len < output_max) output[out_len++] = (u8)(triple >> 8);
        if (pad == 0 && out_len < output_max) output[out_len++] = (u8)(triple);

        if (pad > 0) break; /* padding means end of data */
    }

    return out_len;
}

i32 base64_encode(const u8 *input, i32 input_len, char *output, i32 output_max) {
    if (!output || output_max <= 0) return -1;
    if (!input || input_len <= 0) {
        /* Empty input produces empty output */
        if (input_len == 0) return 0;
        return -1;
    }

    i32 out_len = 0;
    i32 i = 0;

    while (i < input_len) {
        i32 group_start = i;
        u32 b0 = (u32)input[i++];
        u32 b1 = (i < input_len) ? (u32)input[i++] : 0;
        u32 b2 = (i < input_len) ? (u32)input[i++] : 0;

        i32 group_len = i - group_start; /* 1, 2, or 3 bytes consumed */

        u32 triple = (b0 << 16) | (b1 << 8) | b2;

        if (out_len + 4 > output_max) return -1; /* not enough space */

        output[out_len++] = b64_encode_table[(triple >> 18) & 0x3F];
        output[out_len++] = b64_encode_table[(triple >> 12) & 0x3F];
        output[out_len++] = (group_len > 1) ? b64_encode_table[(triple >> 6) & 0x3F] : '=';
        output[out_len++] = (group_len > 2) ? b64_encode_table[triple & 0x3F] : '=';
    }

    return out_len;
}
