/*
 * Liu — codex / claude structured output into lines, a session id and a
 * result. See agentctl_stream.h.
 */
#include "agentctl/agentctl_stream.h"
#include "agentctl/agentctl_adapter.h"
#include "agentctl/agentctl_team.h"

#include "cJSON.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ sanitize --- */

/* Length of the UTF-8 sequence starting at p (1-4), or 0 when it is not a
 * well-formed, shortest-form encoding of a scalar value. */
static usize utf8_seq_len(const unsigned char *p, usize n) {
    unsigned char c = p[0];
    if (c < 0x80) return 1;
    usize len;
    u32 cp;
    if ((c & 0xE0) == 0xC0)      { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else return 0;
    if (len > n) return 0;
    for (usize i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return 0;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

static u32 utf8_decode(const unsigned char *p, usize len) {
    if (len == 1) return p[0];
    u32 cp = p[0] & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
    for (usize i = 1; i < len; i++) cp = (cp << 6) | (p[i] & 0x3F);
    return cp;
}

usize agentctl_sanitize(const char *in, usize n, char *out, usize cap, bool keep_newlines) {
    if (!out || cap == 0) return 0;
    usize o = 0;
    const unsigned char *p = (const unsigned char *)(in ? in : "");
    for (usize i = 0; i < n && p[i]; ) {
        usize len = utf8_seq_len(p + i, n - i);
        const char *emit = NULL;
        usize emit_len = 0;
        char one;
        if (len == 0) {
            emit = "\xEF\xBF\xBD"; emit_len = 3;   /* U+FFFD */
            len = 1;
        } else if (len == 1) {
            unsigned char c = p[i];
            if (c == '\n')      { one = keep_newlines ? '\n' : ' '; emit = &one; emit_len = 1; }
            else if (c == '\t') { one = '\t'; emit = &one; emit_len = 1; }
            else if (c >= 0x20 && c < 0x7F) { one = (char)c; emit = &one; emit_len = 1; }
            /* every other C0 control, ESC and DEL are dropped */
        } else {
            u32 cp = utf8_decode(p + i, len);
            bool drop = (cp >= 0x80 && cp <= 0x9F) ||              /* C1 controls */
                        (cp >= 0x202A && cp <= 0x202E) ||          /* bidi embeddings, overrides */
                        (cp >= 0x2066 && cp <= 0x2069) ||          /* bidi isolates */
                        cp == 0x200E || cp == 0x200F || cp == 0x061C;
            if (!drop) { emit = (const char *)(p + i); emit_len = len; }
        }
        if (emit) {
            if (o + emit_len >= cap) break;
            memcpy(out + o, emit, emit_len);
            o += emit_len;
        }
        i += len;
    }
    out[o] = '\0';
    return o;
}

/* Sanitized into a buffer the caller frees; NULL on allocation failure. */
static char *sanitize_dup(const char *s, usize n, bool keep_newlines, usize *out_len) {
    usize cap = n * 3u + 1u;   /* an invalid byte can grow into U+FFFD */
    char *o = malloc(cap);
    if (!o) return NULL;
    usize w = agentctl_sanitize(s, n, o, cap, keep_newlines);
    if (out_len) *out_len = w;
    return o;
}

/* --------------------------------------------------------------- emit --- */

static void emitf(AgentctlEmit emit, void *ctx, AgentctlStyle st, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static void emitf(AgentctlEmit emit, void *ctx, AgentctlStyle st, const char *fmt, ...) {
    if (!emit) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    usize len = (usize)n < sizeof buf ? (usize)n : sizeof buf - 1;
    emit(ctx, st, buf, len);
}

/* A value for one short line: sanitized, newlines folded, cut at `max` bytes
 * on a character boundary with an ellipsis. `out` holds max + 4 bytes. */
static void one_line(const char *s, char *out, usize cap, usize max) {
    if (!out || cap == 0) return;
    if (!s) s = "";
    char tmp[2048];
    usize w = agentctl_sanitize(s, strnlen(s, 8192), tmp, sizeof tmp, false);
    const char *t = tmp;
    while (w > 0 && (*t == ' ' || *t == '\t')) { t++; w--; }
    while (w > 0 && (t[w - 1] == ' ' || t[w - 1] == '\t')) w--;
    bool cut = false;
    if (w > max) {
        usize k = max;
        while (k > 0 && ((unsigned char)t[k] & 0xC0) == 0x80) k--;
        w = k;
        cut = true;
    }
    snprintf(out, cap, "%.*s%s", (int)w, t, cut ? "\xE2\x80\xA6" : "");
}

static const char *jstr(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

static bool jnum(const cJSON *o, const char *key, double *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v)) return false;
    *out = v->valuedouble;
    return true;
}

static void set_final(AgentctlStream *s, const char *text) {
    if (!text) return;
    usize n = strlen(text);
    if (n > AGENTCTL_STREAM_FINAL_MAX) n = AGENTCTL_STREAM_FINAL_MAX;
    usize w = 0;
    char *c = sanitize_dup(text, n, true, &w);
    if (!c) return;
    free(s->final_text);
    s->final_text = c;
    s->final_len = w;
}

static void set_session(AgentctlStream *s, const char *id) {
    if (id && agentctl_session_ok(id)) snprintf(s->session, sizeof s->session, "%s", id);
}

static void set_error(AgentctlStream *s, const char *msg) {
    if (!msg || !msg[0]) return;
    agentctl_sanitize(msg, strlen(msg), s->error, sizeof s->error, false);
}

/* "12.3k" for a token count. */
/* A count or an amount the agent reported, as a whole number: anything not a
 * positive finite number is 0, and the most is AGC_COUNT_MAX (a token count
 * or millionths of a dollar no turn reaches; the agent's number is not
 * trusted to fit a u64). */
#define AGC_COUNT_MAX 1e15
static u64 count_u64(double v) {
    if (!(v > 0.0)) return 0;                 /* NaN too */
    if (v >= AGC_COUNT_MAX) return (u64)AGC_COUNT_MAX;
    return (u64)v;
}

/* Adds without passing UINT64_MAX. */
static void count_add(u64 *to, u64 v) {
    *to = *to > UINT64_MAX - v ? UINT64_MAX : *to + v;
}

static void count_str(double v, char *out, usize cap) {
    if (!(v > 0.0)) v = 0;
    if (v > AGC_COUNT_MAX) v = AGC_COUNT_MAX;
    if (v < 1000.0)            snprintf(out, cap, "%.0f", v);
    else if (v < 1000000.0)    snprintf(out, cap, "%.1fk", v / 1000.0);
    else                       snprintf(out, cap, "%.2fM", v / 1000000.0);
}

/* A notice the agent gave that is not its turn failing (a configuration
 * warning, a tool server that would not start): shown dim, and once a turn
 * however often the agent repeats it. */
static void notice(AgentctlStream *s, AgentctlEmit emit, void *ctx, const char *text) {
    if (!text || !text[0]) return;
    /* its first sentence: the rest (which settings, where) is the agent's
     * own business and in its own logs */
    char line[400];
    one_line(text, line, sizeof line, 300);
    char *stop = strstr(line, ". ");
    if (stop && stop - line >= 12) stop[1] = '\0';
    u32 h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)line; *p; p++) h = (h ^ *p) * 16777619u;
    u32 n = s->nnotices < 8 ? s->nnotices : 8;
    for (u32 i = 0; i < n; i++) if (s->notices[i] == h) return;
    s->notices[s->nnotices++ % 8] = h;
    emitf(emit, ctx, AGS_DIM, "! %s", line);
}

/* One shell word from `p`, as the shell reads it: single quotes literal,
 * double quotes with their four escapes, backslashes outside quotes, and
 * pieces quoted differently run together ("a"'b'c). Returns where the word
 * ended, or NULL when a quote is left open or it does not fit. */
static const char *shell_word(const char *p, char *out, usize cap) {
    usize o = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
        if (*p == '\'') {
            p++;
            while (*p && *p != '\'') { if (o + 1 >= cap) return NULL; out[o++] = *p++; }
            if (!*p) return NULL;
            p++;
        } else if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$' || p[1] == '`')) p++;
                if (o + 1 >= cap) return NULL;
                out[o++] = *p++;
            }
            if (!*p) return NULL;
            p++;
        } else {
            if (*p == '\\' && p[1]) p++;
            if (o + 1 >= cap) return NULL;
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return p;
}

/* A command as a person reads it: the line inside `<shell> -lc '…'` (or
 * -c), which is how an agent's tool hands every command to a shell — when
 * what follows is that one word. Anything else is left as it is. */
static void command_shown(const char *cmd, char *out, usize cap) {
    out[0] = '\0';
    if (!cmd) return;
    const char *p = cmd;
    const char *base = p;
    while (*p && *p != ' ') { if (*p == '/') base = p + 1; p++; }
    usize bl = (usize)(p - base);
    bool shell = (bl == 2 && !strncmp(base, "sh", 2)) || (bl == 3 && !strncmp(base, "zsh", 3)) ||
                 (bl == 4 && !strncmp(base, "bash", 4));
    const char *rest = NULL;
    if (shell && !strncmp(p, " -lc ", 5)) rest = p + 5;
    else if (shell && !strncmp(p, " -c ", 4)) rest = p + 4;
    if (rest) {
        const char *end = shell_word(rest, out, cap);
        if (end && out[0]) {
            while (*end == ' ' || *end == '\t') end++;
            if (!*end) return;                                  /* the whole rest was that word */
        }
    }
    snprintf(out, cap, "%s", cmd);
}

/* ------------------------------------------------------ shell commands --- */

/* A shell command line read the way the shell reads it, one simple command
 * at a time (`;`, `&&`, `||`, `|`, `&`, a newline, `(` and `)` end one):
 * its words (quotes and backslashes resolved, pieces quoted differently run
 * together), the files its output redirections name, the text it was written
 * as, and the folder the shell is in when it runs. A `cd` or `pushd` with a
 * plain argument is followed and a subshell's ends with its `)`; anything
 * else that moves the shell (`cd` alone, `cd -`, `~`, `$VAR`, `popd`) leaves
 * the folder unknown. A here-document's lines are text, not commands, and a
 * `#` that starts a word comments out the rest of its line. */

#define AGC_SH_WORDS   64
#define AGC_SH_WORD    512
#define AGC_SH_TARGETS 8
#define AGC_SH_DEPTH   4
#define AGC_SH_HEREDOC 4

typedef struct {
    char        words[AGC_SH_WORDS][AGC_SH_WORD];
    i32         nw;
    char        targets[AGC_SH_TARGETS][AGC_SH_WORD];
    i32         nt;
    const char *raw;            /* the command as written, `raw_len` bytes, not NUL-terminated */
    usize       raw_len;
    const char *dir;            /* where it runs, as the caller's folder spells it; NULL = unknown */
} ShCmd;

typedef void (*ShVisit)(void *ctx, const ShCmd *c);

/* One shell word from `p` into `out` (cut to fit), stopping at a blank, a
 * newline, an operator, a redirection or a parenthesis outside quotes. */
static const char *sh_read_word(const char *p, char *out, usize cap) {
    usize o = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != ';' && *p != '|' && *p != '&' &&
           *p != '>' && *p != '<' && *p != '(' && *p != ')') {
        if (*p == '\'') {
            p++;
            while (*p && *p != '\'') { if (o + 1 < cap) out[o++] = *p; p++; }
            if (*p) p++;
        } else if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$' || p[1] == '`')) p++;
                if (o + 1 < cap) out[o++] = *p;
                p++;
            }
            if (*p) p++;
        } else {
            if (*p == '\\' && p[1]) p++;
            if (o + 1 < cap) out[o++] = *p;
            p++;
        }
    }
    out[o] = '\0';
    return p;
}

/* `rel` joined to `dir` ("" = the caller's folder) into `out`; false when it
 * does not fit. An absolute `rel` stands alone. */
static bool sh_join(const char *dir, const char *rel, char *out, usize cap) {
    int w;
    if (rel[0] == '/' || !dir[0]) w = snprintf(out, cap, "%s", rel);
    else                          w = snprintf(out, cap, "%s/%s", dir, rel);
    return w > 0 && (usize)w < cap;
}

/* The index of a simple command's own name, past `VAR=value` words. */
static i32 sh_first(const ShCmd *c) {
    i32 first = 0;
    while (first < c->nw && strchr(c->words[first], '=') && c->words[first][0] != '=') first++;
    return first;
}

static const char *sh_base(const char *word) {
    const char *b = strrchr(word, '/');
    return b ? b + 1 : word;
}

/* Walks `cmd` starting in `dir` ("" = the caller's folder, NULL = unknown),
 * calling `visit` for each simple command. Returns whether the folder the
 * line leaves the shell in is known, and puts it in `dir_out` when given. */
static bool sh_walk(const char *cmd, const char *dir, char *dir_out, usize out_cap, ShVisit visit, void *ctx) {
    if (dir_out && out_cap) dir_out[0] = '\0';
    if (!cmd) return dir != NULL;
    ShCmd *c = malloc(sizeof *c);
    char (*dirs)[AGT_PATH_MAX] = malloc(sizeof(char[AGT_PATH_MAX]) * (AGC_SH_DEPTH + 1));
    if (!c || !dirs) { free(c); free(dirs); return false; }
    bool known[AGC_SH_DEPTH + 1];
    i32 depth = 0, deeper = 0;                 /* subshells past the stack are only counted */
    known[0] = dir && strlen(dir) < AGT_PATH_MAX;
    snprintf(dirs[0], AGT_PATH_MAX, "%s", known[0] ? dir : "");
    char heredoc[AGC_SH_HEREDOC][128];
    bool heredoc_tabs[AGC_SH_HEREDOC];
    i32 nheredoc = 0;
    c->nw = c->nt = 0;
    c->raw = NULL;
    bool want_target = false;
    const char *p = cmd;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        char ch = *p;
        if (ch == '#' && !want_target) {                         /* a comment, to the line's end */
            while (*p && *p != '\n') p++;
            ch = *p;
        }
        if (ch == '&' && p[1] == '>') {                           /* &> and &>> */
            if (!c->raw) c->raw = p;
            p += p[2] == '>' ? 3 : 2;
            want_target = true;
            continue;
        }
        if (!ch || ch == ';' || ch == '|' || ch == '&' || ch == '\n' || ch == '(' || ch == ')') {
            /* the simple command ends: hand it on, then follow its `cd` */
            if (c->nw || c->nt) {
                c->raw_len = c->raw ? (usize)(p - c->raw) : 0;
                c->dir = known[depth] ? dirs[depth] : NULL;
                visit(ctx, c);
                i32 f = sh_first(c);
                const char *name = f < c->nw ? c->words[f] : "";
                if (!strcmp(name, "cd") || !strcmp(name, "pushd")) {
                    const char *arg = f + 1 < c->nw ? c->words[f + 1] : NULL;
                    if (arg && !strcmp(arg, "--")) arg = f + 2 < c->nw ? c->words[f + 2] : NULL;
                    char next[AGT_PATH_MAX];
                    if (known[depth] && arg && arg[0] && arg[0] != '-' && arg[0] != '~' &&
                        !strpbrk(arg, "$`") && sh_join(dirs[depth], arg, next, sizeof next))
                        snprintf(dirs[depth], AGT_PATH_MAX, "%s", next);
                    else
                        known[depth] = false;
                } else if (!strcmp(name, "popd")) {
                    known[depth] = false;
                }
            }
            c->nw = c->nt = 0;
            c->raw = NULL;
            want_target = false;
            if (!ch) break;
            p++;
            if (ch == '(') {
                if (depth < AGC_SH_DEPTH) {
                    depth++;
                    known[depth] = known[depth - 1];
                    memcpy(dirs[depth], dirs[depth - 1], AGT_PATH_MAX);
                } else {
                    deeper++;
                }
            } else if (ch == ')') {
                if (deeper) deeper--;
                else if (depth > 0) depth--;
            } else if (ch == '\n') {
                /* the bodies of the here-documents this line opened */
                for (i32 h = 0; h < nheredoc; h++) {
                    while (*p) {
                        const char *end = strchr(p, '\n');
                        if (!end) end = p + strlen(p);
                        const char *s = p;
                        if (heredoc_tabs[h]) while (*s == '\t') s++;
                        usize dl = strlen(heredoc[h]);
                        bool closes = (usize)(end - s) == dl && !strncmp(s, heredoc[h], dl);
                        p = *end ? end + 1 : end;
                        if (closes) break;
                    }
                }
                nheredoc = 0;
            }
            continue;
        }
        if (!c->raw) c->raw = p;
        /* a redirection: [N]> [N]>> >| [N]< << <<- <<< — `>&N` duplicates */
        const char *q = p;
        while (*q >= '0' && *q <= '9') q++;
        if (*q == '>' || *q == '<') {
            bool out = *q == '>', heredoc_op = false;
            q++;
            if (out) { if (*q == '>' || *q == '|') q++; }
            else if (*q == '<') { q++; if (*q == '<') q++; else heredoc_op = true; }
            if (*q == '&') {
                q++;
                while (*q >= '0' && *q <= '9') q++;
                if (*q == '-') q++;
                p = q;
                continue;
            }
            p = q;
            if (out) { want_target = true; continue; }
            /* an input's word: a file, a here-string, or a here-document's delimiter */
            bool tabs = false;
            if (heredoc_op && *p == '-') { tabs = true; p++; }
            while (*p == ' ' || *p == '\t') p++;
            char w[AGC_SH_WORD];
            p = sh_read_word(p, w, sizeof w);
            if (heredoc_op && w[0] && nheredoc < AGC_SH_HEREDOC && strlen(w) < sizeof heredoc[0]) {
                snprintf(heredoc[nheredoc], sizeof heredoc[0], "%s", w);
                heredoc_tabs[nheredoc] = tabs;
                nheredoc++;
            }
            continue;
        }
        char w[AGC_SH_WORD];
        const char *before = p;
        p = sh_read_word(p, w, sizeof w);
        if (p == before) { p++; continue; }                      /* nothing a word can start with */
        if (want_target) {
            want_target = false;
            if (w[0] && c->nt < AGC_SH_TARGETS) { memcpy(c->targets[c->nt], w, sizeof c->targets[0]); c->nt++; }
            continue;
        }
        if (c->nw < AGC_SH_WORDS) { memcpy(c->words[c->nw], w, sizeof c->words[0]); c->nw++; }
    }
    bool left_known = known[0];
    if (dir_out && out_cap && left_known) snprintf(dir_out, out_cap, "%s", dirs[0]);
    free(c);
    free(dirs);
    return left_known;
}

/* --- the files a line writes, by its words */

typedef struct {
    AgentctlPathFn fn;
    void          *ctx;
    i32            hits;
} ShWrites;

static void sh_writes_one(ShWrites *w, const char *dir, const char *path) {
    if (!path[0] || !strncmp(path, "/dev/", 5)) return;
    char full[AGT_PATH_MAX];
    if (path[0] != '/' && !dir) return;                         /* relative to a folder not known */
    if (!sh_join(dir ? dir : "", path, full, sizeof full)) return;
    w->fn(w->ctx, full);
    w->hits++;
}

static void sh_writes_visit(void *ctx, const ShCmd *c) {
    ShWrites *w = ctx;
    for (i32 t = 0; t < c->nt; t++) sh_writes_one(w, c->dir, c->targets[t]);
    i32 f = sh_first(c);
    if (f >= c->nw) return;
    const char *name = sh_base(c->words[f]);
    /* every file argument of these is written, made or removed */
    if (!strcmp(name, "tee") || !strcmp(name, "touch") || !strcmp(name, "rm") || !strcmp(name, "mv")) {
        bool opts = true;
        for (i32 k = f + 1; k < c->nw; k++) {
            if (opts && !strcmp(c->words[k], "--")) { opts = false; continue; }
            if (opts && c->words[k][0] == '-') continue;
            sh_writes_one(w, c->dir, c->words[k]);
        }
    }
}

i32 agentctl_shell_writes(const char *cmd, const char *dir, AgentctlPathFn fn, void *ctx) {
    if (!fn) return 0;
    ShWrites w = { fn, ctx, 0 };
    sh_walk(cmd, dir, NULL, 0, sh_writes_visit, &w);
    return w.hits;
}

/* --- every word that may name a file */

typedef struct {
    AgentctlPathFn fn;
    void          *ctx;
    i32            n;
} ShNames;

static bool sh_path_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '.' || ch == '_' || ch == '-' || ch == '/' || ch == '+' || ch == '@' ||
           (unsigned char)ch >= 0x80;
}

static void sh_names_visit(void *ctx, const ShCmd *c) {
    ShNames *nm = ctx;
    const char *s = c->raw, *end = c->raw ? c->raw + c->raw_len : NULL;
    while (s && s < end && nm->n < AGC_NAMES_MAX) {
        if (!sh_path_char(*s)) { s++; continue; }
        const char *a = s;
        while (s < end && sh_path_char(*s)) s++;
        /* `~/x`, `$X/y`, `${X}`: not a path Liu can spell */
        if (a > c->raw && (a[-1] == '~' || a[-1] == '$' || a[-1] == '{')) continue;
        usize len = (usize)(s - a);
        while (len > 1 && (a[len - 1] == '.' || a[len - 1] == ',')) len--;
        if (a[0] == '-' || len >= AGC_SH_WORD) continue;
        char word[AGC_SH_WORD], full[AGT_PATH_MAX];
        memcpy(word, a, len);
        word[len] = '\0';
        if (!strcmp(word, ".") || !strcmp(word, "..") || !strcmp(word, "/")) continue;
        if (word[0] != '/' && !c->dir) continue;
        if (!sh_join(c->dir ? c->dir : "", word, full, sizeof full)) continue;
        nm->fn(nm->ctx, full);
        nm->n++;
    }
}

i32 agentctl_command_names(const char *cmd, const char *dir, AgentctlPathFn fn, void *ctx) {
    if (!fn) return 0;
    ShNames nm = { fn, ctx, 0 };
    sh_walk(cmd, dir, NULL, 0, sh_names_visit, &nm);
    return nm.n;
}

/* --- the file names a screen shows */

i32 agentctl_text_names(const char *text, AgentctlPathFn fn, void *ctx) {
    if (!text || !fn) return 0;
    i32 n = 0;
    const char *s = text;
    while (*s && n < AGC_TEXT_NAMES_MAX) {
        if (!sh_path_char(*s)) { s++; continue; }
        const char *a = s;
        while (*s && sh_path_char(*s)) s++;
        usize len = (usize)(s - a);
        while (len > 1 && (a[len - 1] == '.' || a[len - 1] == ',' || a[len - 1] == '-')) len--;
        /* a name with a dot or a slash in it, not a URL's host or a number */
        if (len < 3 || len >= AGC_SH_WORD || a[0] == '-') continue;
        if (a > text && a[-1] == ':' && len > 1 && a[0] == '/' && a[1] == '/') continue;
        if (!memchr(a, '.', len) && !memchr(a, '/', len)) continue;
        bool digits = true;
        for (usize k = 0; k < len && digits; k++) digits = (a[k] >= '0' && a[k] <= '9') || a[k] == '.';
        if (digits) continue;
        char word[AGC_SH_WORD];
        memcpy(word, a, len);
        word[len] = '\0';
        fn(ctx, word);
        n++;
    }
    return n;
}

/* --- a line that only reads */

static void sh_reads_visit(void *ctx, const ShCmd *c) {
    bool *only = ctx;
    if (!*only) return;
    for (i32 t = 0; t < c->nt; t++)
        if (strncmp(c->targets[t], "/dev/", 5) != 0) { *only = false; return; }
    i32 f = sh_first(c);
    if (f >= c->nw) return;
    static const char *const readers[] = {
        "cat", "head", "tail", "less", "more", "grep", "egrep", "fgrep", "rg", "ag", "ls", "wc",
        "stat", "file", "pwd", "which", "type", "echo", "printf", "true", "false", ":", "test", "[",
        "diff", "cmp", "tree", "nl", "du", "df", "cd", "pushd", "popd", "jq", "cut", "tr", "uniq",
        "column", "basename", "dirname", "realpath", "readlink", "date", "uname", "whoami", "id",
        "sleep", "liu-agent", "exit", "hostname", "md5", "shasum", "sha256sum", "od", "xxd",
        "hexdump", "strings",
    };
    const char *name = sh_base(c->words[f]);
    for (usize i = 0; i < sizeof readers / sizeof readers[0]; i++)
        if (!strcmp(name, readers[i])) return;
    if (!strcmp(name, "sed")) {                                  /* unless it edits in place */
        for (i32 k = f + 1; k < c->nw; k++)
            if (!strncmp(c->words[k], "-i", 2) || !strncmp(c->words[k], "--in-place", 10)) { *only = false; return; }
        return;
    }
    if (!strcmp(name, "find")) {                                 /* unless it acts on what it finds */
        for (i32 k = f + 1; k < c->nw; k++)
            if (!strcmp(c->words[k], "-delete") || !strncmp(c->words[k], "-exec", 5) ||
                !strncmp(c->words[k], "-ok", 3) || !strncmp(c->words[k], "-fprint", 7)) { *only = false; return; }
        return;
    }
    if (!strcmp(name, "sort")) {
        for (i32 k = f + 1; k < c->nw; k++)
            if (!strncmp(c->words[k], "-o", 2) || !strncmp(c->words[k], "--output", 8)) { *only = false; return; }
        return;
    }
    if (!strcmp(name, "git") && f + 1 < c->nw) {
        static const char *const looks[] = { "status", "diff", "log", "show", "rev-parse", "ls-files",
                                             "blame", "grep", "describe", "remote", "config", "branch" };
        i32 k = f + 1;
        while (k < c->nw && c->words[k][0] == '-') k += (!strcmp(c->words[k], "-C") || !strcmp(c->words[k], "-c")) ? 2 : 1;
        for (usize i = 0; k < c->nw && i < sizeof looks / sizeof looks[0]; i++)
            if (!strcmp(c->words[k], looks[i])) return;
    }
    *only = false;
}

bool agentctl_command_reads_only(const char *cmd) {
    bool only = true;
    sh_walk(cmd, "", NULL, 0, sh_reads_visit, &only);
    return only;
}

/* ------------------------------------------------- reported file paths --- */

static void stream_file(AgentctlStream *s, const char *path);
static void shell_file_cb(void *ctx, const char *path) { stream_file((AgentctlStream *)ctx, path); }

/* A changed file's path to the caller, cleaned but whole. */
static void stream_file(AgentctlStream *s, const char *path) {
    if (!s->on_file || !path || !path[0]) return;
    char clean[2048];
    usize n = agentctl_sanitize(path, strlen(path), clean, sizeof clean, false);
    if (n > 0 && n < sizeof clean - 1) s->on_file(s->file_ctx, clean, n);
}

static void emit_text_block(AgentctlEmit emit, void *ctx, AgentctlStyle st, const char *text) {
    if (!emit || !text) return;
    usize w = 0;
    char *c = sanitize_dup(text, strlen(text), true, &w);
    if (!c) return;
    /* trailing newlines would leave blank lines in the tab */
    while (w > 0 && (c[w - 1] == '\n' || c[w - 1] == ' ')) c[--w] = '\0';
    if (w) emit(ctx, st, c, w);
    free(c);
}

/* -------------------------------------------------------------- codex --- */

static void codex_item(AgentctlStream *s, const char *phase, const cJSON *item,
                       AgentctlEmit emit, void *ctx) {
    const char *type = jstr(item, "type");
    if (!type) return;
    bool started = !strcmp(phase, "item.started");
    bool updated = !strcmp(phase, "item.updated");
    bool completed = !strcmp(phase, "item.completed");
    char line[512];

    if (!strcmp(type, "agent_message")) {
        if (!completed) return;
        const char *text = jstr(item, "text");
        if (!text) return;
        s->messages++;
        set_final(s, text);
        emit_text_block(emit, ctx, AGS_TEXT, text);
    } else if (!strcmp(type, "reasoning")) {
        if (!completed) return;
        one_line(jstr(item, "text"), line, sizeof line, 160);
        if (line[0]) emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", line);
    } else if (!strcmp(type, "command_execution")) {
        char shown[2048];
        command_shown(jstr(item, "command"), shown, sizeof shown);
        one_line(shown, line, sizeof line, 240);
        if (started) {
            s->commands++;
            emitf(emit, ctx, AGS_TOOL, "\xE2\x96\xB8 $ %s", line);
            if (s->on_command) s->on_command(s->file_ctx, jstr(item, "id"), false, shown, "");
        } else if (completed) {
            double code = 0;
            bool has_code = jnum(item, "exit_code", &code);
            const char *status = jstr(item, "status");
            bool failed = (has_code && code != 0) || (status && !strcmp(status, "failed"));
            if (failed) {
                if (has_code) emitf(emit, ctx, AGS_ERR, "  \xE2\x9C\x97 exit %.0f", code);
                else          emitf(emit, ctx, AGS_ERR, "  \xE2\x9C\x97 failed");
            } else if (status && !strcmp(status, "declined")) {
                emitf(emit, ctx, AGS_ERR, "  \xE2\x9C\x97 not allowed");
            } else {
                emitf(emit, ctx, AGS_OK, "  \xE2\x9C\x93 exit %.0f", has_code ? code : 0.0);
                /* a command that ran: the files it wrote, after its lines;
                 * each runs in the job's folder */
                if (s->on_file) agentctl_shell_writes(shown, "", shell_file_cb, s);
            }
            /* ended, however it ended: what it named may have changed */
            if (s->on_command) s->on_command(s->file_ctx, jstr(item, "id"), true, shown, "");
        }
    } else if (!strcmp(type, "file_change")) {
        if (!completed) return;
        const cJSON *changes = cJSON_GetObjectItemCaseSensitive(item, "changes");
        const cJSON *ch;
        cJSON_ArrayForEach(ch, changes) {
            const char *kind = jstr(ch, "kind");
            one_line(jstr(ch, "path"), line, sizeof line, 240);
            const char *mark = "~";
            if (kind && !strcmp(kind, "add")) mark = "+";
            else if (kind && !strcmp(kind, "delete")) mark = "-";
            s->edits++;
            emitf(emit, ctx, AGS_EDIT, "\xE2\x9C\x8E %s %s", mark, line);
            stream_file(s, jstr(ch, "path"));       /* after its line: what the team hears follows it */
        }
    } else if (!strcmp(type, "mcp_tool_call")) {
        if (!started) return;
        char server[128], tool[128];
        one_line(jstr(item, "server"), server, sizeof server, 60);
        one_line(jstr(item, "tool"), tool, sizeof tool, 60);
        s->tools++;
        emitf(emit, ctx, AGS_TOOL, "\xE2\x9A\x99 %s.%s", server, tool);
    } else if (!strcmp(type, "web_search")) {
        if (!completed) return;
        one_line(jstr(item, "query"), line, sizeof line, 200);
        s->tools++;
        emitf(emit, ctx, AGS_TOOL, "\xE2\x8C\x95 %s", line);
    } else if (!strcmp(type, "todo_list")) {
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(item, "items");
        i32 total = 0, done = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, items) {
            total++;
            const cJSON *c = cJSON_GetObjectItemCaseSensitive(it, "completed");
            if (cJSON_IsTrue(c)) done++;
        }
        if (started) {
            cJSON_ArrayForEach(it, items) {
                one_line(jstr(it, "text"), line, sizeof line, 200);
                emitf(emit, ctx, AGS_DIM, "\xE2\x98\x90 %s", line);
            }
        } else if (updated && total > 0) {
            emitf(emit, ctx, AGS_DIM, "\xE2\x98\x91 %d/%d", done, total);
        }
    } else if (!strcmp(type, "error")) {
        if (!completed) return;
        notice(s, emit, ctx, jstr(item, "message"));
    } else if (completed) {
        one_line(type, line, sizeof line, 60);
        emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", line);
    }
}

static void codex_event(AgentctlStream *s, const cJSON *ev, AgentctlEmit emit, void *ctx) {
    const char *type = jstr(ev, "type");
    if (!type) return;
    if (!strcmp(type, "thread.started")) {
        set_session(s, jstr(ev, "thread_id"));
    } else if (!strcmp(type, "turn.started")) {
        /* the runner draws its own frame line */
    } else if (!strncmp(type, "item.", 5)) {
        codex_item(s, type, cJSON_GetObjectItemCaseSensitive(ev, "item"), emit, ctx);
    } else if (!strcmp(type, "turn.completed")) {
        s->turn_completed = true;
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(ev, "usage");
        double in = 0, cached = 0, out = 0;
        if (cJSON_IsObject(u) && jnum(u, "input_tokens", &in)) {
            jnum(u, "cached_input_tokens", &cached);
            jnum(u, "output_tokens", &out);
            s->tok_in = count_u64(in);
            s->tok_cached = count_u64(cached);
            s->tok_out = count_u64(out);
            char a[16], b[16], c[16];
            count_str(in, a, sizeof a); count_str(cached, b, sizeof b); count_str(out, c, sizeof c);
            if (cached > 0) snprintf(s->usage, sizeof s->usage, "in %s (cached %s) \xC2\xB7 out %s", a, b, c);
            else            snprintf(s->usage, sizeof s->usage, "in %s \xC2\xB7 out %s", a, c);
        }
    } else if (!strcmp(type, "turn.failed")) {
        s->turn_failed = true;
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(ev, "error");
        set_error(s, cJSON_IsObject(e) ? jstr(e, "message") : NULL);
        if (!s->error[0]) snprintf(s->error, sizeof s->error, "the turn failed");
        emitf(emit, ctx, AGS_ERR, "! %s", s->error);
    } else if (!strcmp(type, "error")) {
        /* a notice while the turn goes on; the reason only if nothing better
         * says why a turn did not finish */
        if (!s->error[0]) set_error(s, jstr(ev, "message"));
        notice(s, emit, ctx, jstr(ev, "message"));
    } else {
        char t[96];
        one_line(type, t, sizeof t, 60);
        emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", t);
    }
}

/* ------------------------------------------------------------- claude --- */

/* Claude's tool calls between their call and their result. */
static i32 pend_find(const AgentctlStream *s, const char *id) {
    if (!id || !id[0]) return -1;
    for (u32 i = 0; i < s->npending; i++)
        if (!strcmp(s->pending[i].id, id)) return (i32)i;
    return -1;
}

static void pend_drop(AgentctlStream *s, u32 i) {
    free(s->pending[i].cmd);
    free(s->pending[i].path);
    free(s->pending[i].dir);
    s->pending[i] = s->pending[s->npending - 1];
    memset(&s->pending[s->npending - 1], 0, sizeof s->pending[0]);
    s->npending--;
}

/* False when the call cannot wait for its result (no id, no room): the
 * caller acts on it at once. */
static bool pend_add(AgentctlStream *s, const char *id, const char *cmd, const char *path, const char *dir) {
    if (!id || !id[0] || strlen(id) >= sizeof s->pending[0].id || s->npending >= AGENTCTL_STREAM_PENDING ||
        pend_find(s, id) >= 0)
        return false;
    char *c = cmd ? strdup(cmd) : NULL, *p = path ? strdup(path) : NULL, *d = dir ? strdup(dir) : NULL;
    if ((cmd && !c) || (path && !p) || (dir && !d)) { free(c); free(p); free(d); return false; }
    u32 i = s->npending++;
    snprintf(s->pending[i].id, sizeof s->pending[i].id, "%s", id);
    s->pending[i].cmd = c;
    s->pending[i].path = p;
    s->pending[i].dir = d;
    return true;
}

static void sh_noop_visit(void *ctx, const ShCmd *c) { (void)ctx; (void)c; }

/* Where Claude's shell is after a command that left it in `next` (relative
 * to the job's folder or absolute): a relative folder outside the job's
 * sends it back to the job's, as Claude Code does. */
static void claude_shell_moved(AgentctlStream *s, bool known, const char *next) {
    if (!known) { s->sh_dir_known = false; s->sh_dir[0] = '\0'; return; }
    if (next[0] == '/') { snprintf(s->sh_dir, sizeof s->sh_dir, "%s", next); return; }
    char norm[AGT_PATH_MAX];
    if (!next[0] || !agentctl_team_path("/j", next, strlen(next), norm, sizeof norm) ||
        !agentctl_team_under(norm, "/j")) {
        s->sh_dir[0] = '\0';
        return;
    }
    const char *rel = agentctl_team_rel(norm, "/j");
    snprintf(s->sh_dir, sizeof s->sh_dir, "%s", strcmp(rel, ".") ? rel : "");
}

/* A pending call's result came (`ok`: it worked) or the turn ended without
 * one: an edit counts now, a command's writes and its `cd` are taken now and
 * its end is told. */
static void claude_settle(AgentctlStream *s, u32 k, bool ok, bool result_came) {
    if (s->pending[k].path) {
        if (ok) stream_file(s, s->pending[k].path);
    } else if (s->pending[k].cmd) {
        const char *cmd = s->pending[k].cmd, *dir = s->pending[k].dir;
        if (result_came) {
            char next[1024];
            ShWrites w = { shell_file_cb, s, 0 };
            bool write = ok && s->on_file;
            /* a command that failed is taken to have made its `cd` too: it
             * is mostly what came after it that failed (`cd web && npm test`) */
            bool known = sh_walk(cmd, dir, next, sizeof next, write ? sh_writes_visit : sh_noop_visit, &w);
            claude_shell_moved(s, known, next);
        }
        if (s->on_command) s->on_command(s->file_ctx, s->pending[k].id, true, cmd, dir);
    }
    pend_drop(s, k);
}

static void claude_settle_all(AgentctlStream *s) {
    while (s->npending) claude_settle(s, s->npending - 1, false, false);
}

static void claude_tool_use(AgentctlStream *s, const cJSON *block, AgentctlEmit emit, void *ctx) {
    char name[96], arg[512];
    one_line(jstr(block, "name"), name, sizeof name, 60);
    const cJSON *input = cJSON_GetObjectItemCaseSensitive(block, "input");
    const char *v = NULL;
    if (cJSON_IsObject(input)) {
        if (!(v = jstr(input, "command")) && !(v = jstr(input, "file_path")) &&
            !(v = jstr(input, "path")) && !(v = jstr(input, "pattern")) &&
            !(v = jstr(input, "url")) && !(v = jstr(input, "query")))
            v = jstr(input, "description");
    }
    one_line(v, arg, sizeof arg, 240);
    const char *id = jstr(block, "id");
    bool is_edit = !strcmp(name, "Edit") || !strcmp(name, "Write") || !strcmp(name, "MultiEdit") ||
                   !strcmp(name, "NotebookEdit");
    if (is_edit) {
        s->edits++;
        emitf(emit, ctx, AGS_EDIT, "\xE2\x9C\x8E %s", arg[0] ? arg : name);
        if (cJSON_IsObject(input)) {
            const char *fp = jstr(input, "file_path");
            if (!fp) fp = jstr(input, "notebook_path");
            /* it counts once its tool says it worked */
            if (fp && fp[0] && !pend_add(s, id, NULL, fp, NULL)) stream_file(s, fp);
        }
    } else if (!strcmp(name, "Bash")) {
        s->commands++;
        emitf(emit, ctx, AGS_TOOL, "\xE2\x96\xB8 $ %s", arg);
        const char *cmd = cJSON_IsObject(input) ? jstr(input, "command") : NULL;
        const char *dir = s->sh_dir_known ? s->sh_dir : NULL;
        if (cmd && pend_add(s, id, cmd, NULL, dir)) {
            if (s->on_command) s->on_command(s->file_ctx, id, false, cmd, dir);
        } else if (cmd && s->on_file) {
            agentctl_shell_writes(cmd, dir, shell_file_cb, s);       /* no result to wait for */
        }
    } else {
        s->tools++;
        if (arg[0]) emitf(emit, ctx, AGS_TOOL, "\xE2\x9A\x99 %s %s", name, arg);
        else        emitf(emit, ctx, AGS_TOOL, "\xE2\x9A\x99 %s", name);
    }
}

/* A tool request as a person reads it: the call's own description, else
 * its command or file. */
static void ask_what(const char *tool, const cJSON *req, const cJSON *input, char *out, usize cap) {
    const char *d = jstr(req, "description");
    const char *v = NULL;
    if (cJSON_IsObject(input) && !(v = jstr(input, "command")) && !(v = jstr(input, "file_path")) &&
        !(v = jstr(input, "notebook_path")) && !(v = jstr(input, "path")) && !(v = jstr(input, "url")))
        v = jstr(input, "pattern");
    char a[400], b[400];
    one_line(v, a, sizeof a, 300);
    one_line(d, b, sizeof b, 200);
    char t[96];
    one_line(tool, t, sizeof t, 60);
    if (a[0] && b[0] && strcmp(a, b)) snprintf(out, cap, "%s: %s (%s)", t, a, b);
    else if (a[0])                    snprintf(out, cap, "%s: %s", t, a);
    else if (b[0])                    snprintf(out, cap, "%s: %s", t, b);
    else                              snprintf(out, cap, "%s", t);
}

static void claude_control(AgentctlStream *s, const cJSON *ev, AgentctlEmit emit, void *ctx) {
    const char *type = jstr(ev, "type");
    const char *rid = jstr(ev, "request_id");
    if (!rid || !rid[0] || strlen(rid) > 128) return;
    if (!strcmp(type, "control_cancel_request")) {
        if (s->on_ask_cancel) s->on_ask_cancel(s->file_ctx, rid);
        return;
    }
    const cJSON *req = cJSON_GetObjectItemCaseSensitive(ev, "request");
    const char *sub = cJSON_IsObject(req) ? jstr(req, "subtype") : NULL;
    if (sub && !strcmp(sub, "can_use_tool") && s->on_ask) {
        const char *tool = jstr(req, "tool_name");
        const cJSON *input = cJSON_GetObjectItemCaseSensitive(req, "input");
        char what[700];
        ask_what(tool ? tool : "a tool", req, input, what, sizeof what);
        char *in_json = cJSON_IsObject(input) ? cJSON_PrintUnformatted(input) : NULL;
        char clean[700];
        agentctl_sanitize(what, strlen(what), clean, sizeof clean, false);
        emitf(emit, ctx, AGS_TOOL, "? %s", clean);
        s->on_ask(s->file_ctx, rid, tool ? tool : "", clean, in_json ? in_json : "{}");
        free(in_json);
        return;
    }
    if (s->on_control) s->on_control(s->file_ctx, rid);
}

static void claude_event(AgentctlStream *s, const cJSON *ev, AgentctlEmit emit, void *ctx) {
    const char *type = jstr(ev, "type");
    if (!type) return;
    if (!strcmp(type, "control_request") || !strcmp(type, "control_cancel_request")) {
        claude_control(s, ev, emit, ctx);
        return;
    }
    if (!strcmp(type, "control_response")) return;       /* an answer to the host: none are asked */
    const char *sid = jstr(ev, "session_id");
    if (sid) set_session(s, sid);

    if (!strcmp(type, "rate_limit_event")) {
        /* said only when it limits */
        const cJSON *info = cJSON_GetObjectItemCaseSensitive(ev, "rate_limit_info");
        const char *status = cJSON_IsObject(info) ? jstr(info, "status") : NULL;
        if (status && strcmp(status, "allowed") != 0) {
            char st[64];
            one_line(status, st, sizeof st, 40);
            emitf(emit, ctx, AGS_DIM, "! rate limit: %s", st);
        }
        return;
    }
    if (!strcmp(type, "system")) {
        const char *sub = jstr(ev, "subtype");
        if (sub && !strcmp(sub, "init")) {
            const char *model = jstr(ev, "model");
            if (model) {
                agentctl_sanitize(model, strlen(model), s->model, sizeof s->model, false);
                emitf(emit, ctx, AGS_DIM, "\xC2\xB7 model %s", s->model);
            }
        }
    } else if (!strcmp(type, "assistant")) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(ev, "message");
        const cJSON *content = cJSON_IsObject(msg) ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
        const cJSON *b;
        cJSON_ArrayForEach(b, content) {
            const char *bt = jstr(b, "type");
            if (!bt) continue;
            if (!strcmp(bt, "text")) {
                const char *text = jstr(b, "text");
                if (!text || !text[0]) continue;
                s->messages++;
                set_final(s, text);
                emit_text_block(emit, ctx, AGS_TEXT, text);
            } else if (!strcmp(bt, "tool_use")) {
                claude_tool_use(s, b, emit, ctx);
            } else if (!strcmp(bt, "thinking")) {
                char line[256];
                one_line(jstr(b, "thinking"), line, sizeof line, 160);
                if (line[0]) emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", line);
            }
        }
    } else if (!strcmp(type, "user")) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(ev, "message");
        const cJSON *content = cJSON_IsObject(msg) ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
        const cJSON *b;
        cJSON_ArrayForEach(b, content) {
            const char *bt = jstr(b, "type");
            if (!bt || strcmp(bt, "tool_result")) continue;
            bool failed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "is_error"));
            if (failed) {
                const cJSON *c = cJSON_GetObjectItemCaseSensitive(b, "content");
                const char *text = cJSON_IsString(c) ? c->valuestring : NULL;
                if (!text && cJSON_IsArray(c)) {
                    const cJSON *first = cJSON_GetArrayItem(c, 0);
                    text = first ? jstr(first, "text") : NULL;
                }
                char line[320];
                one_line(text ? text : "tool error", line, sizeof line, 240);
                emitf(emit, ctx, AGS_ERR, "  \xE2\x9C\x97 %s", line);
            }
            i32 k = pend_find(s, jstr(b, "tool_use_id"));
            if (k >= 0) claude_settle(s, (u32)k, !failed, true);
        }
    } else if (!strcmp(type, "result")) {
        claude_settle_all(s);                /* a call the turn ended on gets no result now */
        const char *sub = jstr(ev, "subtype");
        bool is_error = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ev, "is_error")) ||
                        (sub && strcmp(sub, "success") != 0);
        const char *result = jstr(ev, "result");
        if (is_error) {
            s->turn_failed = true;
            set_error(s, result && result[0] ? result : sub);
            if (!s->error[0]) snprintf(s->error, sizeof s->error, "the turn failed");
            emitf(emit, ctx, AGS_ERR, "! %s", s->error);
        } else {
            s->turn_completed = true;
            if (result && result[0]) set_final(s, result);
        }
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(ev, "usage");
        double in = 0, cached = 0, out = 0, cost = 0;
        if (cJSON_IsObject(u) && jnum(u, "input_tokens", &in)) {
            double cw = 0;
            jnum(u, "cache_read_input_tokens", &cached);
            jnum(u, "cache_creation_input_tokens", &cw);
            jnum(u, "output_tokens", &out);
            in += cached + cw;
            s->tok_in = count_u64(in);
            s->tok_cached = count_u64(cached);
            s->tok_out = count_u64(out);
            char a[16], b[16], c[16];
            count_str(in, a, sizeof a); count_str(cached, b, sizeof b); count_str(out, c, sizeof c);
            int w = cached > 0
                ? snprintf(s->usage, sizeof s->usage, "in %s (cached %s) \xC2\xB7 out %s", a, b, c)
                : snprintf(s->usage, sizeof s->usage, "in %s \xC2\xB7 out %s", a, c);
            if (w > 0 && (usize)w < sizeof s->usage && jnum(ev, "total_cost_usd", &cost) && cost > 0)
                snprintf(s->usage + w, sizeof s->usage - (usize)w, " \xC2\xB7 $%.2f", cost);
            s->cost_micro = count_u64(cost * 1000000.0 + 0.5);
        }
    } else {
        char t[96];
        one_line(type, t, sizeof t, 60);
        emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", t);
    }
}

/* ------------------------------------------------------------- gemini --- */

/* Gemini's assistant text in pieces: gathered, then shown whole. */
static void gemini_text_add(AgentctlStream *s, const char *t) {
    if (!t || !t[0]) return;
    usize n = strlen(t);
    if (s->text_len + n + 1 > AGENTCTL_STREAM_FINAL_MAX) return;
    if (s->text_len + n + 1 > s->text_cap) {
        usize cap = s->text_cap ? s->text_cap : 1024;
        while (cap < s->text_len + n + 1) cap *= 2;
        char *nb = realloc(s->text_acc, cap);
        if (!nb) return;
        s->text_acc = nb;
        s->text_cap = cap;
    }
    memcpy(s->text_acc + s->text_len, t, n);
    s->text_len += n;
    s->text_acc[s->text_len] = '\0';
}

static void gemini_text_flush(AgentctlStream *s, AgentctlEmit emit, void *ctx) {
    if (!s->text_len) return;
    s->messages++;
    set_final(s, s->text_acc);
    emit_text_block(emit, ctx, AGS_TEXT, s->text_acc);
    s->text_len = 0;
    s->text_acc[0] = '\0';
}

static bool gemini_is_edit(const char *tool) {
    static const char *const names[] = { "write_file", "replace", "edit", "smart_edit", "write" };
    for (usize i = 0; tool && i < sizeof names / sizeof names[0]; i++) if (!strcmp(tool, names[i])) return true;
    return false;
}

static void gemini_event(AgentctlStream *s, const cJSON *ev, AgentctlEmit emit, void *ctx) {
    const char *type = jstr(ev, "type");
    if (!type) return;
    if (!strcmp(type, "init")) {
        set_session(s, jstr(ev, "session_id"));
        const char *model = jstr(ev, "model");
        if (model) {
            agentctl_sanitize(model, strlen(model), s->model, sizeof s->model, false);
            emitf(emit, ctx, AGS_DIM, "\xC2\xB7 model %s", s->model);
        }
        return;
    }
    if (!strcmp(type, "message")) {
        const char *role = jstr(ev, "role");
        if (!role || strcmp(role, "assistant")) return;           /* the prompt it was given */
        gemini_text_add(s, jstr(ev, "content"));
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ev, "delta"))) gemini_text_flush(s, emit, ctx);
        return;
    }
    gemini_text_flush(s, emit, ctx);
    if (!strcmp(type, "tool_use")) {
        const char *tool = jstr(ev, "tool_name"), *id = jstr(ev, "tool_id");
        const cJSON *in = cJSON_GetObjectItemCaseSensitive(ev, "parameters");
        char name[96], arg[512];
        one_line(tool, name, sizeof name, 60);
        const char *v = NULL;
        if (cJSON_IsObject(in) && !(v = jstr(in, "command")) && !(v = jstr(in, "file_path")) &&
            !(v = jstr(in, "absolute_path")) && !(v = jstr(in, "path")) && !(v = jstr(in, "pattern")))
            v = jstr(in, "query");
        one_line(v, arg, sizeof arg, 240);
        if (tool && !strcmp(tool, "run_shell_command")) {
            s->commands++;
            emitf(emit, ctx, AGS_TOOL, "\xE2\x96\xB8 $ %s", arg);
            const char *cmd = cJSON_IsObject(in) ? jstr(in, "command") : NULL;
            /* each command starts in the job's folder: a `cd` does not carry */
            if (cmd && pend_add(s, id, cmd, NULL, "")) {
                if (s->on_command) s->on_command(s->file_ctx, id, false, cmd, "");
            }
        } else if (gemini_is_edit(tool)) {
            s->edits++;
            emitf(emit, ctx, AGS_EDIT, "\xE2\x9C\x8E %s", arg[0] ? arg : name);
            const char *fp = cJSON_IsObject(in) ? jstr(in, "file_path") : NULL;
            if (!fp && cJSON_IsObject(in)) fp = jstr(in, "absolute_path");
            if (fp && fp[0] && !pend_add(s, id, NULL, fp, NULL)) stream_file(s, fp);
        } else {
            s->tools++;
            if (arg[0]) emitf(emit, ctx, AGS_TOOL, "\xE2\x9A\x99 %s %s", name, arg);
            else        emitf(emit, ctx, AGS_TOOL, "\xE2\x9A\x99 %s", name);
        }
        return;
    }
    if (!strcmp(type, "tool_result")) {
        const char *status = jstr(ev, "status");
        bool failed = status && strcmp(status, "success") != 0;
        if (failed) {
            const cJSON *e = cJSON_GetObjectItemCaseSensitive(ev, "error");
            char line[320];
            one_line(cJSON_IsObject(e) ? jstr(e, "message") : NULL, line, sizeof line, 240);
            emitf(emit, ctx, AGS_ERR, "  \xE2\x9C\x97 %s", line[0] ? line : "tool error");
        }
        i32 k = pend_find(s, jstr(ev, "tool_id"));
        if (k >= 0) {
            claude_settle(s, (u32)k, !failed, true);
            s->sh_dir[0] = '\0';                      /* the next command starts in the folder again */
            s->sh_dir_known = true;
        }
        return;
    }
    if (!strcmp(type, "error")) {
        const char *sev = jstr(ev, "severity");
        if (sev && !strcmp(sev, "error")) {
            set_error(s, jstr(ev, "message"));
            char line[320];
            one_line(jstr(ev, "message"), line, sizeof line, 240);
            emitf(emit, ctx, AGS_ERR, "! %s", line);
        } else {
            notice(s, emit, ctx, jstr(ev, "message"));
        }
        return;
    }
    if (!strcmp(type, "result")) {
        while (s->npending) claude_settle(s, s->npending - 1, false, false);
        const char *status = jstr(ev, "status");
        if (status && !strcmp(status, "success")) {
            s->turn_completed = true;
        } else {
            s->turn_failed = true;
            const cJSON *e = cJSON_GetObjectItemCaseSensitive(ev, "error");
            set_error(s, cJSON_IsObject(e) ? jstr(e, "message") : NULL);
            if (!s->error[0]) snprintf(s->error, sizeof s->error, "the turn failed");
            emitf(emit, ctx, AGS_ERR, "! %s", s->error);
        }
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(ev, "stats");
        double in = 0, cached = 0, out = 0;
        if (cJSON_IsObject(st) && jnum(st, "input_tokens", &in)) {
            jnum(st, "cached", &cached);
            jnum(st, "output_tokens", &out);
            s->tok_in = count_u64(in);
            s->tok_cached = count_u64(cached);
            s->tok_out = count_u64(out);
            char a[16], b[16], c[16];
            count_str(in, a, sizeof a); count_str(cached, b, sizeof b); count_str(out, c, sizeof c);
            if (cached > 0) snprintf(s->usage, sizeof s->usage, "in %s (cached %s) \xC2\xB7 out %s", a, b, c);
            else            snprintf(s->usage, sizeof s->usage, "in %s \xC2\xB7 out %s", a, c);
        }
        return;
    }
    char t[96];
    one_line(type, t, sizeof t, 60);
    emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", t);
}

/* ----------------------------------------------------------- opencode --- */

static bool opencode_is_edit(const char *tool) {
    static const char *const names[] = { "edit", "write", "multiedit", "patch", "apply_patch" };
    for (usize i = 0; tool && i < sizeof names / sizeof names[0]; i++) if (!strcmp(tool, names[i])) return true;
    return false;
}

static void opencode_usage(AgentctlStream *s, const cJSON *part) {
    const cJSON *tk = cJSON_GetObjectItemCaseSensitive(part, "tokens");
    double in = 0, out = 0, reasoning = 0, cread = 0, cwrite = 0, cost = 0;
    if (cJSON_IsObject(tk)) {
        jnum(tk, "input", &in);
        jnum(tk, "output", &out);
        jnum(tk, "reasoning", &reasoning);
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(tk, "cache");
        if (cJSON_IsObject(c)) { jnum(c, "read", &cread); jnum(c, "write", &cwrite); }
    }
    /* one turn is several steps: their counts add up */
    count_add(&s->tok_in, count_u64(in));
    count_add(&s->tok_in, count_u64(cread));
    count_add(&s->tok_in, count_u64(cwrite));
    count_add(&s->tok_cached, count_u64(cread));
    count_add(&s->tok_out, count_u64(out));
    count_add(&s->tok_out, count_u64(reasoning));
    if (jnum(part, "cost", &cost)) count_add(&s->cost_micro, count_u64(cost * 1000000.0 + 0.5));
    char a[16], b[16], c2[16];
    count_str((double)s->tok_in, a, sizeof a);
    count_str((double)s->tok_cached, b, sizeof b);
    count_str((double)s->tok_out, c2, sizeof c2);
    int w = s->tok_cached
        ? snprintf(s->usage, sizeof s->usage, "in %s (cached %s) \xC2\xB7 out %s", a, b, c2)
        : snprintf(s->usage, sizeof s->usage, "in %s \xC2\xB7 out %s", a, c2);
    if (w > 0 && (usize)w < sizeof s->usage && s->cost_micro)
        snprintf(s->usage + w, sizeof s->usage - (usize)w, " \xC2\xB7 $%.2f", (double)s->cost_micro / 1e6);
}

static void opencode_event(AgentctlStream *s, const cJSON *ev, AgentctlEmit emit, void *ctx) {
    const char *type = jstr(ev, "type");
    if (!type) return;
    const char *sid = jstr(ev, "sessionID");
    if (sid) set_session(s, sid);
    const cJSON *part = cJSON_GetObjectItemCaseSensitive(ev, "part");
    if (!strcmp(type, "step_start")) return;
    if (!strcmp(type, "text")) {
        const char *text = cJSON_IsObject(part) ? jstr(part, "text") : NULL;
        if (!text || !text[0]) return;
        s->messages++;
        set_final(s, text);
        emit_text_block(emit, ctx, AGS_TEXT, text);
        return;
    }
    if (!strcmp(type, "reasoning")) {
        char line[256];
        one_line(cJSON_IsObject(part) ? jstr(part, "text") : NULL, line, sizeof line, 160);
        if (line[0]) emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", line);
        return;
    }
    if (!strcmp(type, "tool_use")) {
        if (!cJSON_IsObject(part)) return;
        const char *tool = jstr(part, "tool");
        const char *id = jstr(part, "id");
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(part, "state");
        const cJSON *in = cJSON_IsObject(st) ? cJSON_GetObjectItemCaseSensitive(st, "input") : NULL;
        const char *status = cJSON_IsObject(st) ? jstr(st, "status") : NULL;
        bool failed = status && !strcmp(status, "error");
        char name[96], arg[512];
        one_line(tool, name, sizeof name, 60);
        const char *v = NULL;
        if (cJSON_IsObject(in) && !(v = jstr(in, "command")) && !(v = jstr(in, "filePath")) &&
            !(v = jstr(in, "file_path")) && !(v = jstr(in, "path")) && !(v = jstr(in, "pattern")))
            v = jstr(in, "query");
        one_line(v, arg, sizeof arg, 240);
        /* OpenCode reports a tool once it has run: its line and its result together */
        if (tool && !strcmp(tool, "bash")) {
            s->commands++;
            emitf(emit, ctx, AGS_TOOL, "\xE2\x96\xB8 $ %s", arg);
            const char *cmd = cJSON_IsObject(in) ? jstr(in, "command") : NULL;
            if (s->on_command && cmd) s->on_command(s->file_ctx, id, false, cmd, "");
            if (!failed && cmd && s->on_file) agentctl_shell_writes(cmd, "", shell_file_cb, s);
            if (s->on_command && cmd) s->on_command(s->file_ctx, id, true, cmd, "");
        } else if (opencode_is_edit(tool)) {
            s->edits++;
            emitf(emit, ctx, AGS_EDIT, "\xE2\x9C\x8E %s", arg[0] ? arg : name);
            const char *fp = cJSON_IsObject(in) ? jstr(in, "filePath") : NULL;
            if (!fp && cJSON_IsObject(in)) fp = jstr(in, "file_path");
            if (!failed && fp && fp[0]) stream_file(s, fp);
        } else {
            s->tools++;
            if (arg[0]) emitf(emit, ctx, AGS_TOOL, "\xE2\x9A\x99 %s %s", name, arg);
            else        emitf(emit, ctx, AGS_TOOL, "\xE2\x9A\x99 %s", name);
        }
        if (failed) {
            char line[320];
            one_line(jstr(st, "error"), line, sizeof line, 240);
            emitf(emit, ctx, AGS_ERR, "  \xE2\x9C\x97 %s", line[0] ? line : "tool error");
        }
        return;
    }
    if (!strcmp(type, "step_finish")) {
        if (!cJSON_IsObject(part)) return;
        opencode_usage(s, part);
        const char *reason = jstr(part, "reason");
        /* a step that ended on its tool calls is followed by another */
        if (!reason || strcmp(reason, "tool-calls")) s->turn_completed = true;
        return;
    }
    if (!strcmp(type, "error")) {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(ev, "error");
        const char *msg = cJSON_IsObject(e) ? jstr(e, "message") : NULL;
        const cJSON *data = cJSON_IsObject(e) ? cJSON_GetObjectItemCaseSensitive(e, "data") : NULL;
        if (!msg && cJSON_IsObject(data)) msg = jstr(data, "message");
        if (!msg && cJSON_IsObject(e)) msg = jstr(e, "name");
        s->turn_failed = true;
        set_error(s, msg);
        if (!s->error[0]) snprintf(s->error, sizeof s->error, "the turn failed");
        emitf(emit, ctx, AGS_ERR, "! %s", s->error);
        return;
    }
    char t[96];
    one_line(type, t, sizeof t, 60);
    emitf(emit, ctx, AGS_DIM, "\xC2\xB7 %s", t);
}

/* ----------------------------------------------------- Claude's stdin --- */

char *agentctl_user_message_json(const char *text, usize len) {
    cJSON *root = cJSON_CreateObject();
    cJSON *msg = cJSON_CreateObject();
    char *copy = malloc(len + 1);
    if (!root || !msg || !copy) { cJSON_Delete(root); cJSON_Delete(msg); free(copy); return NULL; }
    memcpy(copy, text ? text : "", len);
    copy[len] = '\0';
    cJSON_AddStringToObject(root, "type", "user");
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", copy);
    free(copy);
    cJSON_AddItemToObject(root, "message", msg);
    cJSON_AddNullToObject(root, "parent_tool_use_id");
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return NULL;
    usize n = strlen(out);
    char *line = realloc(out, n + 2);
    if (!line) { free(out); return NULL; }
    line[n] = '\n';
    line[n + 1] = '\0';
    return line;
}

static char *control_line(cJSON *root) {
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return NULL;
    usize n = strlen(out);
    char *line = realloc(out, n + 2);
    if (!line) { free(out); return NULL; }
    line[n] = '\n';
    line[n + 1] = '\0';
    return line;
}

char *agentctl_control_answer(const char *req_id, bool allow, const char *input, const char *why) {
    cJSON *root = cJSON_CreateObject(), *resp = cJSON_CreateObject(), *res = cJSON_CreateObject();
    if (!root || !resp || !res) { cJSON_Delete(root); cJSON_Delete(resp); cJSON_Delete(res); return NULL; }
    cJSON_AddStringToObject(root, "type", "control_response");
    cJSON_AddStringToObject(resp, "subtype", "success");
    cJSON_AddStringToObject(resp, "request_id", req_id ? req_id : "");
    if (allow) {
        cJSON *in = input ? cJSON_Parse(input) : NULL;
        if (!cJSON_IsObject(in)) { cJSON_Delete(in); cJSON_Delete(root); cJSON_Delete(resp); cJSON_Delete(res); return NULL; }
        cJSON_AddStringToObject(res, "behavior", "allow");
        cJSON_AddItemToObject(res, "updatedInput", in);
    } else {
        cJSON_AddStringToObject(res, "behavior", "deny");
        cJSON_AddStringToObject(res, "message", why && why[0] ? why : "The user did not allow it.");
    }
    cJSON_AddItemToObject(resp, "response", res);
    cJSON_AddItemToObject(root, "response", resp);
    return control_line(root);
}

char *agentctl_control_error(const char *req_id, const char *why) {
    cJSON *root = cJSON_CreateObject(), *resp = cJSON_CreateObject();
    if (!root || !resp) { cJSON_Delete(root); cJSON_Delete(resp); return NULL; }
    cJSON_AddStringToObject(root, "type", "control_response");
    cJSON_AddStringToObject(resp, "subtype", "error");
    cJSON_AddStringToObject(resp, "request_id", req_id ? req_id : "");
    cJSON_AddStringToObject(resp, "error", why && why[0] ? why : "not supported");
    cJSON_AddItemToObject(root, "response", resp);
    return control_line(root);
}

/* -------------------------------------------------------------- lines --- */

void agentctl_stream_init(AgentctlStream *s, i32 agent) {
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->agent = agent;
    s->sh_dir_known = true;
}

void agentctl_stream_free(AgentctlStream *s) {
    if (!s) return;
    while (s->npending) pend_drop(s, s->npending - 1);
    free(s->final_text);
    free(s->line);
    free(s->text_acc);
    memset(s, 0, sizeof *s);
}

void agentctl_stream_line(AgentctlStream *s, const char *line, usize len, AgentctlEmit emit, void *ctx) {
    if (!s || !line) return;
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) len--;
    usize b = 0;
    while (b < len && (line[b] == ' ' || line[b] == '\t')) b++;
    if (b == len) return;
    if (line[b] != '{') {
        /* Not an event: a CLI that printed a plain line on stdout. Shown dim. */
        char tmp[512];
        agentctl_sanitize(line + b, len - b, tmp, sizeof tmp, false);
        if (tmp[0]) emitf(emit, ctx, AGS_DIM, "%s", tmp);
        return;
    }
    cJSON *ev = cJSON_ParseWithLength(line + b, len - b);
    if (!ev) {
        emitf(emit, ctx, AGS_DIM, "\xC2\xB7 (an event that is not JSON)");
        return;
    }
    if (cJSON_IsObject(ev)) {
        switch (s->agent) {
        case AGENTCTL_AGENT_CLAUDE:   claude_event(s, ev, emit, ctx); break;
        case AGENTCTL_AGENT_OPENCODE: opencode_event(s, ev, emit, ctx); break;
        case AGENTCTL_AGENT_GEMINI:   gemini_event(s, ev, emit, ctx); break;
        default:                      codex_event(s, ev, emit, ctx); break;
        }
    }
    cJSON_Delete(ev);
}

static bool line_append(AgentctlStream *s, const u8 *d, usize n) {
    if (s->line_len + n + 1 > s->line_cap) {
        usize cap = s->line_cap ? s->line_cap : 4096;
        while (cap < s->line_len + n + 1) cap *= 2;
        char *nl = realloc(s->line, cap);
        if (!nl) return false;
        s->line = nl;
        s->line_cap = cap;
    }
    memcpy(s->line + s->line_len, d, n);
    s->line_len += n;
    s->line[s->line_len] = '\0';
    return true;
}

void agentctl_stream_feed(AgentctlStream *s, const u8 *data, usize len, AgentctlEmit emit, void *ctx) {
    if (!s || !data) return;
    usize i = 0;
    while (i < len) {
        const u8 *nl = memchr(data + i, '\n', len - i);
        usize piece = nl ? (usize)(nl - (data + i)) : len - i;
        if (!s->line_skipping) {
            if (s->line_len + piece > AGENTCTL_STREAM_LINE_MAX || !line_append(s, data + i, piece)) {
                s->line_skipping = true;
                s->line_len = 0;
            }
        }
        i += piece;
        if (nl) {
            i++;   /* the newline */
            if (s->line_skipping) {
                emitf(emit, ctx, AGS_DIM, "\xC2\xB7 (an event too large to show)");
                s->line_skipping = false;
            } else if (s->line_len) {
                agentctl_stream_line(s, s->line, s->line_len, emit, ctx);
            }
            s->line_len = 0;
        }
    }
}

void agentctl_stream_finish(AgentctlStream *s, AgentctlEmit emit, void *ctx) {
    if (!s) return;
    if (!s->line_skipping && s->line_len) agentctl_stream_line(s, s->line, s->line_len, emit, ctx);
    s->line_len = 0;
    s->line_skipping = false;
    if (s->agent == AGENTCTL_AGENT_GEMINI) gemini_text_flush(s, emit, ctx);
    claude_settle_all(s);                    /* the agent is gone: its commands have ended */
}
