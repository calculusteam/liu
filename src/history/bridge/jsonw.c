/*
 * liu-history/bridge - streaming JSON writer. See jsonw.h.
 */
#include "history/bridge/jsonw.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define JSONW_MAX_DEPTH ((i32)(sizeof(((JsonW *)0)->need_comma) / sizeof(u8)))

/* ------------------------------------------------------------ path safety - */

bool jsonw_path_safe(const char *path) {
    if (!path || path[0] != '/') return false;
    usize n = strlen(path);
    if (n < 2 || n >= ATOMIC_FILE_PATH_CAP) return false;
    if (path[n - 1] == '/') return false;   /* a file, not a directory */

    /* No ".." anywhere. A target adapter builds its paths from a session id
     * and a cwd that ultimately came off disk, and "../../.ssh" is a
     * perfectly good directory name as far as the scanner is concerned. */
    for (usize i = 0; i + 1 < n; i++) {
        if (path[i] == '.' && path[i + 1] == '.' &&
            (i == 0 || path[i - 1] == '/') &&
            (i + 2 == n || path[i + 2] == '/')) {
            return false;
        }
    }

    /* Symlinks: the FINAL component only, and that is a deliberate division
     * of labour rather than a relaxation.
     *
     * The destination is checked HERE because it is the redirect attack — a
     * symlink planted at the path an adapter is about to write sends the
     * user's whole conversation to a file of someone else's choosing. lstat,
     * not stat: the entire point is to notice that the component IS a
     * symlink, which stat() would silently follow. A path that does not exist
     * yet is fine; that is the normal case.
     *
     * The PARENT is checked by core/atomic_file.c's private variant, which
     * every write here goes through, and it checks it harder than this could:
     * lstat plus S_ISDIR (so a symlinked parent is refused), st_uid ==
     * geteuid(), and mode forced to 0700. Repeating a weaker version of that
     * test here would add nothing.
     *
     * Walking EVERY ancestor would be wrong in both directions. It buys no
     * security — an attacker who can replace $HOME or /Users with a symlink
     * already owns the account, so the check is not what stands between them
     * and the user's files. And it fails on real machines: /tmp is a symlink
     * to /private/tmp on macOS, and dotfile managers routinely symlink
     * ~/.claude or the whole home directory, which would refuse every
     * migration on those setups with an error the user cannot act on. */
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) return false;

    return true;
}

/* ---------------------------------------------------------------- escape -- */

usize jsonw_escape(char *out, usize cap, const char *s) {
    if (!out || cap == 0) return 0;
    usize o = 0;
    for (const unsigned char *p = (const unsigned char *)s; s && *p; p++) {
        const char *esc = NULL;
        char ubuf[7];
        switch (*p) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            case '\b': esc = "\\b";  break;
            case '\f': esc = "\\f";  break;
            default:
                if (*p < 0x20) {
                    /* Every other C0 control must be \u-escaped: a raw 0x1B
                     * inside a session file is an escape sequence the moment
                     * some other tool cats it to a terminal. */
                    snprintf(ubuf, sizeof ubuf, "\\u%04x", (unsigned)*p);
                    esc = ubuf;
                }
                break;
        }
        if (esc) {
            usize el = strlen(esc);
            if (o + el >= cap) return 0;
            memcpy(out + o, esc, el);
            o += el;
        } else {
            if (o + 1 >= cap) return 0;
            out[o++] = (char)*p;
        }
    }
    out[o] = '\0';
    return o;
}

/* ---------------------------------------------------------------- writer -- */

bool jsonw_ok(const JsonW *w) { return w && !w->failed; }

static void wput(JsonW *w, const char *s, usize n) {
    if (!w || w->failed || !w->f || n == 0) return;
    if (fwrite(s, 1, n, w->f) != n) w->failed = true;
}

static void wputs(JsonW *w, const char *s) { wput(w, s, strlen(s)); }

/* Emit the separator and (inside an object) the key. This is the ONLY place
 * that touches need_comma, so a container can never disagree with itself. */
static void wsep(JsonW *w, const char *key) {
    if (!w || w->failed) return;
    if (w->depth > 0 && w->depth <= JSONW_MAX_DEPTH) {
        if (w->need_comma[w->depth - 1]) wputs(w, ",");
        w->need_comma[w->depth - 1] = 1;
    }
    if (key) {
        char esc[512];
        if (jsonw_escape(esc, sizeof esc, key) == 0 && key[0]) {
            w->failed = true;
            return;
        }
        wputs(w, "\"");
        wputs(w, esc);
        wputs(w, "\":");
    }
}

static void wpush(JsonW *w) {
    if (w->depth >= JSONW_MAX_DEPTH) { w->failed = true; return; }
    w->need_comma[w->depth++] = 0;
}

static void wpop(JsonW *w) {
    if (w->depth > 0) w->depth--;
    else w->failed = true;
}

bool jsonw_open_file(JsonW *w, const char *path) {
    if (!w) return false;
    memset(w, 0, sizeof *w);
    if (!jsonw_path_safe(path)) { w->failed = true; return false; }
    if (!atomic_file_begin_private(&w->atomic, path)) { w->failed = true; return false; }
    w->atomic_open = true;
    w->f = w->atomic.stream;
    return true;
}

void jsonw_open_stream(JsonW *w, FILE *f) {
    if (!w) return;
    memset(w, 0, sizeof *w);
    w->f = f;
    if (!f) w->failed = true;
}

bool jsonw_close(JsonW *w, bool abort_write) {
    if (!w) return false;
    bool ok = !w->failed && w->depth == 0;
    if (w->atomic_open) {
        if (abort_write || !ok) {
            atomic_file_abort(&w->atomic);
            ok = false;
        } else {
            ok = atomic_file_commit(&w->atomic);
        }
        w->atomic_open = false;
    } else if (w->f) {
        if (fflush(w->f) != 0) ok = false;
    }
    w->f = NULL;
    return ok && !abort_write;
}

void jsonw_obj_begin(JsonW *w, const char *key) {
    wsep(w, key); wputs(w, "{"); wpush(w);
}
void jsonw_obj_end(JsonW *w) { wpop(w); wputs(w, "}"); }

void jsonw_arr_begin(JsonW *w, const char *key) {
    wsep(w, key); wputs(w, "["); wpush(w);
}
void jsonw_arr_end(JsonW *w) { wpop(w); wputs(w, "]"); }

void jsonw_raw_newline(JsonW *w) {
    if (!w || w->failed) return;
    /* A newline between documents is only meaningful at depth 0; inside a
     * container it would produce a JSONL file whose lines are fragments. */
    if (w->depth != 0) { w->failed = true; return; }
    wputs(w, "\n");
    memset(w->need_comma, 0, sizeof w->need_comma);
}

void jsonw_null(JsonW *w, const char *key) { wsep(w, key); wputs(w, "null"); }

void jsonw_bool(JsonW *w, const char *key, bool value) {
    wsep(w, key); wputs(w, value ? "true" : "false");
}

void jsonw_i64(JsonW *w, const char *key, i64 value) {
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)value);
    wsep(w, key); wputs(w, buf);
}

void jsonw_f64(JsonW *w, const char *key, f64 value) {
    /* JSON has no NaN or Infinity, and a target's parser will reject the
     * document rather than the field. Costs are the only f64 here, and 0
     * is the right answer for a session Liu did not pay for. */
    if (!isfinite(value)) value = 0.0;
    char buf[48];
    snprintf(buf, sizeof buf, "%.10g", value);
    wsep(w, key); wputs(w, buf);
}

void jsonw_strn(JsonW *w, const char *key, const char *value, usize len) {
    if (!w || w->failed) return;
    if (!value) { jsonw_null(w, key); return; }
    wsep(w, key);
    wputs(w, "\"");
    /* Escape in bounded chunks so an arbitrarily long value never needs an
     * allocation. The chunk boundary is pulled back off a UTF-8 continuation
     * byte: a split multi-byte sequence would be re-emitted as two invalid
     * bytes and some target parsers reject the whole file for it. */
    char esc[4096];
    usize off = 0;
    while (off < len) {
        usize take = len - off;
        if (take > 900) take = 900;   /* 900 * 6 (worst-case \uXXXX) < 4096 */
        while (take > 0 && (off + take) < len &&
               ((unsigned char)value[off + take] & 0xC0) == 0x80) {
            take--;
        }
        if (take == 0) { w->failed = true; break; }
        char tmp[1024];
        memcpy(tmp, value + off, take);
        tmp[take] = '\0';
        usize n = jsonw_escape(esc, sizeof esc, tmp);
        if (n == 0 && take > 0 && tmp[0] != '\0') { w->failed = true; break; }
        wput(w, esc, n);
        off += take;
    }
    wputs(w, "\"");
}

void jsonw_str(JsonW *w, const char *key, const char *value) {
    if (!value) { jsonw_null(w, key); return; }
    jsonw_strn(w, key, value, strlen(value));
}

/* ------------------------------------------------------------ validation -- */

typedef struct { const char *p; i32 depth; bool ok; } JChk;

static void jchk_ws(JChk *c) {
    while (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r') c->p++;
}

static void jchk_value(JChk *c);

static void jchk_string(JChk *c) {
    if (*c->p != '"') { c->ok = false; return; }
    c->p++;
    while (*c->p) {
        unsigned char ch = (unsigned char)*c->p;
        if (ch == '"') { c->p++; return; }
        if (ch == '\\') {
            c->p++;
            switch (*c->p) {
                case '"': case '\\': case '/': case 'b': case 'f':
                case 'n': case 'r': case 't':
                    c->p++; break;
                case 'u':
                    c->p++;
                    for (i32 i = 0; i < 4; i++) {
                        char h = *c->p;
                        bool hex = (h >= '0' && h <= '9') ||
                                   (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F');
                        if (!hex) { c->ok = false; return; }
                        c->p++;
                    }
                    break;
                default: c->ok = false; return;
            }
            continue;
        }
        if (ch < 0x20) { c->ok = false; return; }   /* raw control byte */
        c->p++;
    }
    c->ok = false;   /* unterminated */
}

static void jchk_number(JChk *c) {
    if (*c->p == '-') c->p++;
    if (*c->p < '0' || *c->p > '9') { c->ok = false; return; }
    while (*c->p >= '0' && *c->p <= '9') c->p++;
    if (*c->p == '.') {
        c->p++;
        if (*c->p < '0' || *c->p > '9') { c->ok = false; return; }
        while (*c->p >= '0' && *c->p <= '9') c->p++;
    }
    if (*c->p == 'e' || *c->p == 'E') {
        c->p++;
        if (*c->p == '+' || *c->p == '-') c->p++;
        if (*c->p < '0' || *c->p > '9') { c->ok = false; return; }
        while (*c->p >= '0' && *c->p <= '9') c->p++;
    }
}

static void jchk_lit(JChk *c, const char *lit) {
    usize n = strlen(lit);
    if (strncmp(c->p, lit, n) != 0) { c->ok = false; return; }
    c->p += n;
}

static void jchk_value(JChk *c) {
    if (!c->ok) return;
    if (++c->depth > 64) { c->ok = false; return; }
    jchk_ws(c);
    switch (*c->p) {
        case '{':
            c->p++;
            jchk_ws(c);
            if (*c->p == '}') { c->p++; break; }
            for (;;) {
                jchk_ws(c);
                jchk_string(c);
                if (!c->ok) return;
                jchk_ws(c);
                if (*c->p != ':') { c->ok = false; return; }
                c->p++;
                jchk_value(c);
                if (!c->ok) return;
                jchk_ws(c);
                if (*c->p == ',') { c->p++; continue; }
                if (*c->p == '}') { c->p++; break; }
                c->ok = false; return;
            }
            break;
        case '[':
            c->p++;
            jchk_ws(c);
            if (*c->p == ']') { c->p++; break; }
            for (;;) {
                jchk_value(c);
                if (!c->ok) return;
                jchk_ws(c);
                if (*c->p == ',') { c->p++; continue; }
                if (*c->p == ']') { c->p++; break; }
                c->ok = false; return;
            }
            break;
        case '"': jchk_string(c); break;
        case 't': jchk_lit(c, "true");  break;
        case 'f': jchk_lit(c, "false"); break;
        case 'n': jchk_lit(c, "null");  break;
        default:  jchk_number(c); break;
    }
    c->depth--;
}

bool jsonw_is_json(const char *s) {
    if (!s || !s[0]) return false;
    JChk c = { .p = s, .depth = 0, .ok = true };
    jchk_value(&c);
    if (!c.ok) return false;
    jchk_ws(&c);
    return *c.p == '\0';
}

void jsonw_obj_or_wrap(JsonW *w, const char *key, const char *raw) {
    if (!w || w->failed) return;
    /* An empty object represents "no arguments recorded"; null would fail
     * the same object schema check a string fails. */
    if (!raw || !raw[0] || raw[0] != '{' || !jsonw_is_json(raw)) {
        jsonw_obj_begin(w, key);
        if (raw && raw[0]) jsonw_str(w, "liu_input", raw);
        jsonw_obj_end(w);
        return;
    }
    wsep(w, key);
    wputs(w, raw);
}

void jsonw_raw_or_str(JsonW *w, const char *key, const char *raw) {
    if (!w || w->failed) return;
    if (!raw || !raw[0]) { jsonw_null(w, key); return; }
    if (jsonw_is_json(raw)) {
        wsep(w, key);
        wputs(w, raw);
    } else {
        jsonw_str(w, key, raw);
    }
}
