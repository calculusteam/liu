/*
 * Liu - string utilities implementation
 */
#include "string_utils.h"
#include "checked.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <limits.h>

Str str_from_cstr(const char *s) {
    if (!s) return STR_NULL;
    return (Str){ .data = s, .len = strlen(s) };
}

char *str_to_cstr(Str s, Arena *a) {
    if (!a || (s.len > 0 && !s.data)) return NULL;
    usize allocation_size;
    if (!checked_add_usize(s.len, 1u, &allocation_size)) return NULL;
    char *buf = (char *)arena_alloc(a, allocation_size);
    if (!buf) return NULL;
    if (s.len) memcpy(buf, s.data, s.len);
    buf[s.len] = '\0';
    return buf;
}

bool str_eq(Str a, Str b) {
    if (a.len != b.len) return false;
    if (a.len == 0) return true;
    if (!a.data || !b.data) return false;
    return memcmp(a.data, b.data, a.len) == 0;
}

bool str_eq_nocase(Str a, Str b) {
    if (a.len != b.len) return false;
    if (a.len > 0 && (!a.data || !b.data)) return false;
    for (usize i = 0; i < a.len; i++) {
        if (tolower((unsigned char)a.data[i]) != tolower((unsigned char)b.data[i])) {
            return false;
        }
    }
    return true;
}

bool str_starts_with(Str s, Str prefix) {
    if (prefix.len > s.len) return false;
    if (prefix.len == 0) return true;
    if (!s.data || !prefix.data) return false;
    return memcmp(s.data, prefix.data, prefix.len) == 0;
}

bool str_ends_with(Str s, Str suffix) {
    if (suffix.len > s.len) return false;
    if (suffix.len == 0) return true;
    if (!s.data || !suffix.data) return false;
    return memcmp(s.data + s.len - suffix.len, suffix.data, suffix.len) == 0;
}

bool str_contains(Str haystack, Str needle) {
    return str_find(haystack, needle) >= 0;
}

Str str_trim(Str s) {
    if (!s.data) return STR_NULL;
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
    if (!s.data || start > s.len) return STR_NULL;
    if (len > s.len - start) len = s.len - start;
    return (Str){ .data = s.data + start, .len = len };
}

i64 str_find(Str haystack, Str needle) {
    if (needle.len == 0) return 0;
    if (needle.len > haystack.len) return -1;
    if (!haystack.data || !needle.data) return -1;
    for (usize i = 0; i <= haystack.len - needle.len; i++) {
        if (memcmp(haystack.data + i, needle.data, needle.len) == 0) {
            if (i > (usize)INT64_MAX) return -1;
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
    sb.cap   = a ? initial_cap : 0;
    sb.data  = a && initial_cap > 0 ? (char *)arena_alloc(a, initial_cap) : NULL;
    if (initial_cap > 0 && !sb.data) sb.cap = 0;
    sb.len   = 0;
    return sb;
}

void sb_append(StringBuilder *sb, Str s) {
    if (!sb || !sb->arena || (s.len > 0 && !s.data) || sb->len > sb->cap) return;
    usize required;
    if (!checked_add_usize(sb->len, s.len, &required)) return;
    if (required > sb->cap) {
        /* Arena doesn't support realloc, so we re-allocate a larger block.
         * The old block is wasted (arena reclaims it on reset). */
        usize new_cap;
        if (!checked_mul_usize(required, 2u, &new_cap)) new_cap = required;
        char *new_data = (char *)arena_alloc(sb->arena, new_cap);
        if (!new_data) return;
        if (sb->len) memcpy(new_data, sb->data, sb->len);
        sb->data = new_data;
        sb->cap  = new_cap;
    }
    if (s.len) memcpy(sb->data + sb->len, s.data, s.len);
    sb->len = required;
}

void sb_append_cstr(StringBuilder *sb, const char *s) {
    if (!s) return;
    sb_append(sb, str_from_cstr(s));
}

void sb_append_char(StringBuilder *sb, char c) {
    sb_append(sb, (Str){ .data = &c, .len = 1 });
}

void sb_append_u32(StringBuilder *sb, u32 val) {
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%u", val);
    if (len > 0 && (usize)len < sizeof buf)
        sb_append(sb, (Str){ .data = buf, .len = (usize)len });
}

Str sb_to_str(const StringBuilder *sb) {
    if (!sb) return STR_NULL;
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

static i32 base64_decode_pass(const char *input, i32 input_len, u8 *output) {
    i32 out_len = 0;
    i32 i = 0;
    bool finished = false;

    while (i < input_len) {
        /* Skip whitespace */
        while (i < input_len && ((u8)input[i] <= ' ')) i++;
        if (i >= input_len) break;
        if (finished) return -1; /* non-whitespace data after '=' */

        /* Collect 4 base64 characters */
        i32 vals[4] = {-2, -2, -2, -2};
        i32 j;
        for (j = 0; j < 4 && i < input_len; ) {
            u8 c = (u8)input[i++];
            if (c <= ' ') continue; /* skip whitespace */
            i32 v = b64_decode_char(c);
            if (v == -2) return -1; /* invalid character */
            vals[j++] = v;
        }
        if (j < 4) return -1; /* truncated input */

        /* Padding is legal only as xx== or xxx= in the final quartet. The
         * unused low bits must be zero, rejecting ambiguous encodings. */
        if (vals[0] < 0 || vals[1] < 0) return -1;
        i32 bytes = 3;
        if (vals[2] == -1) {
            if (vals[3] != -1 || (vals[1] & 0x0F) != 0) return -1;
            vals[2] = vals[3] = 0;
            bytes = 1;
            finished = true;
        } else if (vals[2] < 0) {
            return -1;
        } else if (vals[3] == -1) {
            if ((vals[2] & 0x03) != 0) return -1;
            vals[3] = 0;
            bytes = 2;
            finished = true;
        } else if (vals[3] < 0) {
            return -1;
        }

        /* Decode 3 bytes from 4 base64 chars */
        u32 triple = ((u32)vals[0] << 18) | ((u32)vals[1] << 12) |
                     ((u32)vals[2] << 6)  | (u32)vals[3];

        if (out_len > INT32_MAX - bytes) return -1;
        if (output) {
            output[out_len] = (u8)(triple >> 16);
            if (bytes > 1) output[out_len + 1] = (u8)(triple >> 8);
            if (bytes > 2) output[out_len + 2] = (u8)triple;
        }
        out_len += bytes;
    }

    return out_len;
}

i32 base64_decode(const char *input, i32 input_len, u8 *output, i32 output_max) {
    if (input_len < 0 || output_max < 0 || (input_len > 0 && !input)) return -1;
    i32 required = base64_decode_pass(input ? input : "", input_len, NULL);
    if (required < 0 || required > output_max || (required > 0 && !output)) return -1;
    if (required == 0) return 0;
    return base64_decode_pass(input, input_len, output);
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

        if (out_len > output_max || output_max - out_len < 4) return -1;

        output[out_len++] = b64_encode_table[(triple >> 18) & 0x3F];
        output[out_len++] = b64_encode_table[(triple >> 12) & 0x3F];
        output[out_len++] = (group_len > 1) ? b64_encode_table[(triple >> 6) & 0x3F] : '=';
        output[out_len++] = (group_len > 2) ? b64_encode_table[triple & 0x3F] : '=';
    }

    return out_len;
}

bool liu_path_normalize(char *out, usize cap, const char *path) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!path) return false;

    /* Component stack holds offsets into `out`, so ".." can rewind. */
    usize starts[128];
    i32 depth = 0;
    usize o = 0;
    bool absolute = (path[0] == '/');
    if (absolute) {
        if (cap < 2) return false;
        out[o++] = '/';
    }

    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') p++;
        usize len = (usize)(p - seg);

        if (len == 1 && seg[0] == '.') continue;          /* "." — no-op */
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (depth > 0) {                               /* pop */
                o = starts[--depth];
                /* Popping the first component rewinds onto the leading '/',
                 * which would turn "/a/b/../../c" into a RELATIVE "c". The
                 * root slash is not a component and must survive. */
                if (absolute && o == 0) o = 1;
                continue;
            }
            if (absolute) continue;   /* ".." above root is still root */
            /* Relative path with no parent to pop: keep the ".." literally. */
        }

        if (depth >= (i32)(sizeof starts / sizeof starts[0])) return false;
        if (o > 0 && out[o - 1] != '/') {
            if (o + 1 >= cap) { out[o] = '\0'; return false; }
            out[o++] = '/';
        }
        starts[depth++] = (o > 0 && out[o - 1] == '/') ? o - 1 : o;
        if (o + len >= cap) { out[o] = '\0'; return false; }
        for (usize i = 0; i < len; i++) out[o++] = seg[i];
    }

    if (o == 0) {
        if (cap < 2) return false;
        out[o++] = absolute ? '/' : '.';
    }
    out[o] = '\0';
    return true;
}

bool liu_path_equiv(const char *a, const char *b) {
    if (!a || !b) return false;
    if (strcmp(a, b) == 0) return true;
    char na[1024], nb[1024];
    if (!liu_path_normalize(na, sizeof na, a)) return false;
    if (!liu_path_normalize(nb, sizeof nb, b)) return false;
    return strcmp(na, nb) == 0;
}
