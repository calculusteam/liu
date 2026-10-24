/*
 * Liu - multi-agent notify-hook installers (see agent_hooks.h for contract).
 *
 * grok/opencode own their whole config artifact (a dedicated file in the
 * agent's hooks/plugins dir), so install = atomic write, uninstall = unlink.
 * codex shares ~/.codex/config.toml with the user's settings, so Liu's entry
 * is found by what it RUNS (hook_cmd_is_liu — the comment fence around it is
 * decoration that a rewrite of the file may drop) and taken out as whole
 * lines: a line lexer that knows where a table starts, never a full TOML
 * parse, so the user's `notify`/`model`/project trust lines stay intact.
 */
#include "notify/agent_hooks.h"
#include "core/checked.h"
#include "notify/hook_paths.h"
#include "core/atomic_file.h"

#include "cJSON.h"   /* antigravity's hooks.json is a shared JSON file that is merged into */

#include <errno.h>
#include <fcntl.h>
#ifndef PLATFORM_WIN32
#include <pwd.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

#include "notify/hook_scan.h"   /* hook_cmd_is_liu: the one ownership rule */

/* ----------------------------------------------------------------- *
 *  shared helpers                                                    *
 * ----------------------------------------------------------------- */

static const char *home_dir(void) {
    const char *home = getenv("HOME");
#ifndef PLATFORM_WIN32
    if (!home || !*home) {
        struct passwd *pw = getpwuid(geteuid());
        if (pw && pw->pw_dir) home = pw->pw_dir;
    }
#endif
    return (home && *home) ? home : NULL;
}

/* Atomic text write via tmp + rename. */
static bool atomic_write(const char *path, const char *text) {
    if (!path || !text) return false;
    AtomicFileWriter writer;
    if (!atomic_file_begin_personal(&writer, path)) return false;
    usize len = strlen(text);
    if (fwrite(text, 1, len, writer.stream) != len) {
        atomic_file_abort(&writer);
        return false;
    }
    return atomic_file_commit(&writer);
}

/* Read a whole file into a malloc'd NUL-terminated buffer, or NULL. */
static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || sz > 8 * 1024 * 1024) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    buf[got] = '\0';
    return buf;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Where a write to `path` has to land. See agent_hooks.h: a config that is a
 * symbolic link to an existing file is written at the link's TARGET, so the
 * link — and the dotfiles repository it points into — survives the write. */
bool agent_hook_write_path(const char *path, char *out, usize cap, bool *is_link) {
    return atomic_file_write_path(path, out, cap, is_link);
}

/* atomic_write() at the place agent_hook_write_path() names. The temp file and
 * the rename both happen in the TARGET's directory, so a link is never the
 * thing renamed over; when that directory cannot be written the call fails
 * and nothing has changed. `target` is the size of every `path` buffer in
 * this file, not PATH_MAX: a resolved name longer than the paths the writer
 * was sized for is refused rather than handed to it. */
static bool write_through(const char *path, const char *text) {
    char target[1024];
    return agent_hook_write_path(path, target, sizeof target, NULL) &&
           atomic_write(target, text);
}

/* Read a config file Liu splices into, distinguishing "genuinely absent" from
 * "present but unusable". Mirrors claude_hooks.c load_or_empty so a transiently
 * unreadable or oversized config.toml is never silently overwritten:
 *   - returns NULL and sets *absent = true  → no file (ENOENT/empty): ok to create;
 *   - returns NULL and sets *absent = false → present but unreadable/oversized:
 *     caller MUST abort without writing;
 *   - returns the malloc'd contents on success (*absent = false). */
static char *read_text_distinguish(const char *path, bool *absent) {
    *absent = false;
    FILE *f = fopen(path, "rb");
    if (!f) { *absent = (errno == ENOENT); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz == 0) { fclose(f); *absent = true; return NULL; }   /* empty: start fresh */
    if (sz < 0 || sz > 8 * 1024 * 1024) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    buf[got] = '\0';
    return buf;
}

/* Escape a string for embedding inside a double-quoted JSON / JS / TOML
 * value: backslash and double-quote only (paths and CLI flags carry no
 * control bytes). */
static void escape_dq(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && o + 2 < cap; p++) {
        if (*p == '\\' || *p == '"') out[o++] = '\\';
        out[o++] = *p;
    }
    out[o] = '\0';
}

static AgentHookResult result(bool ok, const char *fmt, ...) {
    AgentHookResult r = { .ok = ok, .msg = {0} };
    va_list ap; va_start(ap, fmt);
    vsnprintf(r.msg, sizeof r.msg, fmt, ap);
    va_end(ap);
    return r;
}

/* ----------------------------------------------------------------- *
 *  grok — ~/.grok/hooks/liu-notify.json (Liu's own file)            *
 * ----------------------------------------------------------------- */

static bool grok_path(char *out, size_t cap) {
    const char *home = home_dir();
    if (!home) return false;
    return (size_t)snprintf(out, cap, "%s/" LIU_HOOK_GROK_REL, home) < cap;
}

static AgentHookResult grok_install(const char *bin) {
    char path[1024];
    if (!grok_path(path, sizeof path)) return result(false, "grok: path too long");
    char q[1024], b[1024];
    if (!agent_hook_shell_quote(bin, q, sizeof q)) return result(false, "grok: hook path too long");
    escape_dq(q, b, sizeof b);   /* shell-quoted, then JSON-escaped */
    /* Stop → top-level turn complete; Notification → attention. No
     * SubagentStop (subagent completions must stay silent); --hook makes
     * liu-notify read Grok's stdin JSON and drop any SubagentStop that
     * still arrives. Global ~/.grok/hooks JSON files are "always
     * trusted", so this works the moment the file lands. */
    char json[2048];
    int n = snprintf(json, sizeof json,
        "{\n"
        "  \"hooks\": {\n"
        "    \"Stop\": [\n"
        "      { \"hooks\": [ { \"type\": \"command\",\n"
        "                       \"command\": \"%s send --tool=grok --event=complete --hook\" } ] }\n"
        "    ],\n"
        "    \"Notification\": [\n"
        "      { \"hooks\": [ { \"type\": \"command\",\n"
        "                       \"command\": \"%s send --tool=grok --event=notify --hook\" } ] }\n"
        "    ]\n"
        "  }\n"
        "}\n", b, b);
    /* Never write truncated (invalid) JSON. */
    if (n < 0 || (size_t)n >= sizeof json) return result(false, "grok: hook path too long");
    if (!write_through(path, json)) return result(false, "grok: write failed");
    return result(true, "Grok hook installed (%s)", path);
}

static AgentHookResult grok_uninstall(void) {
    char path[1024];
    if (!grok_path(path, sizeof path)) return result(false, "grok: path too long");
    if (file_exists(path) && unlink(path) != 0 && errno != ENOENT)
        return result(false, "grok: remove failed");
    return result(true, "Grok hook removed");
}

static bool grok_is_installed(void) {
    char path[1024];
    return grok_path(path, sizeof path) && file_exists(path);
}

/* ----------------------------------------------------------------- *
 *  opencode — ~/.config/opencode/plugins/liu-notify.js (own file)   *
 * ----------------------------------------------------------------- */

static bool opencode_path(char *out, size_t cap) {
    const char *home = home_dir();
    if (!home) return false;
    return (size_t)snprintf(out, cap,
        "%s/" LIU_HOOK_OPENCODE_REL, home) < cap;
}

static AgentHookResult opencode_install(const char *bin) {
    char path[1024];
    if (!opencode_path(path, sizeof path)) return result(false, "opencode: path too long");
    char b[1024];
    escape_dq(bin, b, sizeof b);
    /* Plugins in ~/.config/opencode/plugins/ auto-load — no config.json edit, so
     * the user's existing `plugin` array is untouched. session.idle fires per
     * session INCLUDING subagent/Task child sessions (they carry a non-null
     * parentID), so the plugin fetches the session and notifies ONLY when it is
     * the top-level one (no parentID); otherwise a finished subagent would ding.
     */
    char js[2048];
    int n = snprintf(js, sizeof js,
        "// liu-notify plugin (managed by Liu; do not edit).\n"
        "// Notifies on TOP-LEVEL session completion only; subagent/Task child\n"
        "// sessions (non-null parentID) are suppressed.\n"
        "import { spawn } from \"child_process\";\n"
        "const BIN = \"%s\";\n"
        "export default async ({ client }) => ({\n"
        "  event: async ({ event }) => {\n"
        "    if (!event || event.type !== \"session.idle\") return;\n"
        "    try {\n"
        "      const id = event.properties && event.properties.sessionID;\n"
        "      if (id && client && client.session && client.session.get) {\n"
        "        const r = await client.session.get({ path: { id } });\n"
        "        const s = r && (r.data || r);\n"
        "        if (s && s.parentID) return;   /* subagent/child -> silent */\n"
        "      }\n"
        "    } catch (e) { /* lookup failed: fall through and notify */ }\n"
        "    try {\n"
        "      spawn(BIN, [\"send\", \"--tool=opencode\", \"--event=complete\"],\n"
        "            { stdio: \"ignore\", detached: true }).unref();\n"
        "    } catch (e) {}\n"
        "  },\n"
        "});\n", b);
    /* Never write a truncated (broken) plugin file. */
    if (n < 0 || (size_t)n >= sizeof js) return result(false, "opencode: plugin path too long");
    if (!write_through(path, js)) return result(false, "opencode: write failed");
    return result(true, "OpenCode plugin installed (%s)", path);
}

static AgentHookResult opencode_uninstall(void) {
    char path[1024];
    if (!opencode_path(path, sizeof path)) return result(false, "opencode: path too long");
    if (file_exists(path) && unlink(path) != 0 && errno != ENOENT)
        return result(false, "opencode: remove failed");
    return result(true, "OpenCode plugin removed");
}

static bool opencode_is_installed(void) {
    char path[1024];
    return opencode_path(path, sizeof path) && file_exists(path);
}

/* ----------------------------------------------------------------- *
 *  pi, omp — an extension in their agent folder (Liu's own file)    *
 * ----------------------------------------------------------------- */

/* Pi and omp load *.ts files from <agent dir>/extensions/ without changing the
 * user's settings. For Pi, agent_settled means the agent will not continue;
 * agent_end can occur before retries, compaction, or queued messages finish.
 * An aborted run is not a completed turn, and ui_prompt_start indicates that
 * the extension UI is waiting for input. For omp, session_stop signals that
 * the main session stopped after background jobs became idle; it does not
 * apply to task or sub-agent sessions. The handlers return without continuing
 * or blocking. These extension events are handled directly rather than
 * translated to --hook events; headless -p also loads extensions. */
static bool pi_path(char *out, size_t cap) {
    const char *home = home_dir();
    return home && (size_t)snprintf(out, cap, "%s/" LIU_HOOK_PI_REL, home) < cap;
}

static bool omp_path(char *out, size_t cap) {
    const char *home = home_dir();
    return home && (size_t)snprintf(out, cap, "%s/" LIU_HOOK_OMP_REL, home) < cap;
}

static AgentHookResult pi_install(const char *bin) {
    char path[1024];
    if (!pi_path(path, sizeof path)) return result(false, "pi: path too long");
    char b[1024];
    escape_dq(bin, b, sizeof b);
    char ts[3072];
    int n = snprintf(ts, sizeof ts,
        "// liu-notify extension (managed by Liu; do not edit).\n"
        "// Reports a finished turn (agent_settled) and a prompt waiting on you.\n"
        "import { spawn } from \"node:child_process\";\n"
        "const BIN = \"%s\";\n"
        "function send(event: string) {\n"
        "  try {\n"
        "    spawn(BIN, [\"send\", \"--tool=pi\", \"--event=\" + event],\n"
        "          { stdio: \"ignore\", detached: true }).unref();\n"
        "  } catch (e) {}\n"
        "}\n"
        "export default function (pi: any) {\n"
        "  let aborted = false;\n"
        "  pi.on(\"agent_end\", async (event: any) => {\n"
        "    const m = event && event.messages;\n"
        "    const last = Array.isArray(m) && m.length ? m[m.length - 1] : null;\n"
        "    aborted = !!last && last.stopReason === \"aborted\";\n"
        "  });\n"
        "  pi.on(\"agent_settled\", async () => {\n"
        "    if (!aborted) send(\"complete\");\n"
        "    aborted = false;\n"
        "  });\n"
        "  pi.on(\"ui_prompt_start\", async () => { send(\"notify\"); });\n"
        "}\n", b);
    if (n < 0 || (size_t)n >= sizeof ts) return result(false, "pi: extension path too long");
    if (!write_through(path, ts)) return result(false, "pi: write failed");
    return result(true, "Pi extension installed (%s)", path);
}

static AgentHookResult omp_install(const char *bin) {
    char path[1024];
    if (!omp_path(path, sizeof path)) return result(false, "omp: path too long");
    char b[1024];
    escape_dq(bin, b, sizeof b);
    char ts[2048];
    int n = snprintf(ts, sizeof ts,
        "// liu-notify extension (managed by Liu; do not edit).\n"
        "// Reports the main session's stop (never a sub-agent's).\n"
        "import { spawn } from \"node:child_process\";\n"
        "const BIN = \"%s\";\n"
        "export default function (pi: any) {\n"
        "  pi.on(\"session_stop\", async () => {\n"
        "    try {\n"
        "      spawn(BIN, [\"send\", \"--tool=omp\", \"--event=complete\"],\n"
        "            { stdio: \"ignore\", detached: true }).unref();\n"
        "    } catch (e) {}\n"
        "  });\n"
        "}\n", b);
    if (n < 0 || (size_t)n >= sizeof ts) return result(false, "omp: extension path too long");
    if (!write_through(path, ts)) return result(false, "omp: write failed");
    return result(true, "Oh My Pi extension installed (%s)", path);
}

static AgentHookResult pi_uninstall(void) {
    char path[1024];
    if (!pi_path(path, sizeof path)) return result(false, "pi: path too long");
    if (file_exists(path) && unlink(path) != 0 && errno != ENOENT)
        return result(false, "pi: remove failed");
    return result(true, "Pi extension removed");
}

static AgentHookResult omp_uninstall(void) {
    char path[1024];
    if (!omp_path(path, sizeof path)) return result(false, "omp: path too long");
    if (file_exists(path) && unlink(path) != 0 && errno != ENOENT)
        return result(false, "omp: remove failed");
    return result(true, "Oh My Pi extension removed");
}

static bool pi_is_installed(void) {
    char path[1024];
    return pi_path(path, sizeof path) && file_exists(path);
}

static bool omp_is_installed(void) {
    char path[1024];
    return omp_path(path, sizeof path) && file_exists(path);
}

/* ----------------------------------------------------------------- *
 *  codex — one [[hooks.Stop]] entry in ~/.codex/config.toml         *
 * ----------------------------------------------------------------- */

/* The fence Liu draws around its entry. It tells a reader of config.toml
 * whose lines these are and is removed with them, but it is NOT how the
 * entry is found:
 *
 *   - Codex rewrites config.toml itself (trusting a hook adds a [hooks.state]
 *     table), and a rewrite keeps tables and drops comments, so the fence can
 *     disappear while the hook stays;
 *   - cutting raw text from a start marker to an end marker would glue
 *     whatever follows the block onto the line before it
 *     (`notifications = true[mcp_servers.docs]` — a config Codex cannot
 *     read), and with the end marker lost it would cut to the end of the
 *     file.
 *
 * So: an entry is Liu's when the hook in it RUNS liu-notify (hook_cmd_is_liu),
 * wherever it sits and whatever comments surround it, and lines are only ever
 * removed whole. */
#define CODEX_MARK_START "# >>> liu-notify (managed by Liu; do not edit) >>>"
#define CODEX_MARK_END   "# <<< liu-notify <<<"

static bool codex_path(char *out, size_t cap) {
    const char *home = home_dir();
    if (!home) return false;
    return (size_t)snprintf(out, cap, "%s/" LIU_HOOK_CODEX_REL, home) < cap;
}

/* --- config.toml as lines ------------------------------------------------
 *
 * Enough TOML to answer three questions and no more: where does a STATEMENT
 * end (a multi-line string or array is one statement, and a line inside one
 * that happens to read `[[hooks.Stop]]` is text), which table does a header
 * open, and what string does `command = …` assign. Anything this does not
 * understand is kept as an opaque line and never removed. */

typedef enum { TL_BLANK, TL_COMMENT, TL_HEADER, TL_KEYVAL } TomlLineKind;

/* What a table header or a key names, as far as hooks.Stop is concerned. */
typedef enum {
    TT_OTHER = 0,
    TT_HOOKS,           /* hooks */
    TT_STOP,            /* hooks.Stop */
    TT_STOP_HOOK,       /* hooks.Stop.hooks */
    TT_STOP_HOOK_SUB,   /* hooks.Stop.hooks.<more> — part of one hook */
    TT_STOP_SUB,        /* hooks.Stop.<anything else> */
    TT_KEY_STOP,        /* Stop[.<more>] — hooks.Stop when it sits in [hooks] */
    TT_KEY_COMMAND,     /* command */
} TomlTag;

typedef struct {
    const char *s, *e;  /* the statement, line terminator(s) included */
    const char *val;    /* TL_KEYVAL: the first byte after '=' */
    u8   kind;          /* TomlLineKind */
    u8   tag;           /* TomlTag of the header's table / the key */
    bool array;         /* TL_HEADER spelled [[…]] */
    bool marker;        /* TL_COMMENT that is one of the two fence lines */
    bool del;           /* Liu's: left out by codex_render() */
} TomlLine;

typedef struct { TomlLine *v; i32 n; } TomlDoc;

/* 32 bytes a line; a config.toml is a few hundred. The cap only stops a file
 * that is 8 MB of newlines from being answered with a quarter-gigabyte
 * allocation — such a file is refused, not edited. */
#define TOML_MAX_LINES (1 << 18)

/* A dotted key: `hooks.Stop`, `hooks . "Stop"`, 'a'.b. `n` counts every
 * component; the first three are kept, and one too long to be any name looked
 * for here is kept as "" so it can never compare equal to one. */
typedef struct { i32 n; char part[3][16]; } TomlKey;

static const char *toml_key(const char *p, const char *end, TomlKey *k) {
    k->n = 0;
    for (;;) {
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        char name[16] = {0};
        usize len = 0;
        bool fits = true;
        if (p < end && (*p == '"' || *p == '\'')) {
            char q = *p++;
            const char *b = p;
            while (p < end && *p != q && *p != '\n') {
                if (q == '"' && *p == '\\' && p + 1 < end && p[1] != '\n') p++;
                p++;
            }
            if (p >= end || *p != q) return NULL;
            if (q == '"') {
                fits = hook_scan_unescape(b, p, name, sizeof name);
                len = fits ? strlen(name) : 0;
            } else if ((usize)(p - b) < sizeof name) {
                len = (usize)(p - b);
                memcpy(name, b, len);
            } else {
                fits = false;
            }
            p++;
        } else {
            const char *b = p;
            while (p < end && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) p++;
            if (p == b) return NULL;
            if ((usize)(p - b) < sizeof name) {
                len = (usize)(p - b);
                memcpy(name, b, len);
            } else {
                fits = false;
            }
        }
        name[fits ? len : 0] = '\0';
        if (k->n < 3) memcpy(k->part[k->n], name, sizeof name);
        k->n++;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end || *p != '.') return p;
        p++;
    }
}

static TomlTag toml_classify(const TomlKey *k) {
    if (k->n >= 1 && !strcmp(k->part[0], "hooks")) {
        if (k->n == 1) return TT_HOOKS;
        if (strcmp(k->part[1], "Stop") != 0) return TT_OTHER;
        if (k->n == 2) return TT_STOP;
        if (strcmp(k->part[2], "hooks") != 0) return TT_STOP_SUB;
        return k->n == 3 ? TT_STOP_HOOK : TT_STOP_HOOK_SUB;
    }
    if (k->n >= 1 && !strcmp(k->part[0], "Stop")) return TT_KEY_STOP;
    if (k->n == 1 && !strcmp(k->part[0], "command")) return TT_KEY_COMMAND;
    return TT_OTHER;
}

/* Where the value that starts at `p` stops: after the newline that ends the
 * statement. Strings (all four kinds) are skipped whole and a newline inside
 * an open [ or { belongs to the value. */
static const char *toml_value_end(const char *p, const char *end) {
    i32 depth = 0;
    while (p < end) {
        char c = *p;
        if (c == '"' || c == '\'') {
            bool triple = end - p >= 3 && p[1] == c && p[2] == c;
            p += triple ? 3 : 1;
            while (p < end) {
                if (c == '"' && *p == '\\' && p + 1 < end && (triple || p[1] != '\n')) {
                    p += 2;
                    continue;
                }
                if (triple) {
                    if (end - p >= 3 && p[0] == c && p[1] == c && p[2] == c) { p += 3; break; }
                } else {
                    if (*p == c) { p++; break; }
                    if (*p == '\n') break;    /* unterminated: the line ends it */
                }
                p++;
            }
            continue;
        }
        if (c == '#') {
            while (p < end && *p != '\n') p++;
            continue;
        }
        if (c == '[' || c == '{') depth++;
        else if (c == ']' || c == '}') depth--;
        else if (c == '\n' && depth <= 0) return p + 1;
        p++;
    }
    return end;
}

static bool toml_is_marker(const char *s, const char *e) {
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    usize n = (usize)(e - s);
    return (n == sizeof CODEX_MARK_START - 1 && !memcmp(s, CODEX_MARK_START, n)) ||
           (n == sizeof CODEX_MARK_END - 1 && !memcmp(s, CODEX_MARK_END, n));
}

/* Split `text` into statements. False on OOM or past TOML_MAX_LINES; the
 * caller then leaves the file alone. `text` must outlive the document. */
static bool toml_lex(const char *text, TomlDoc *doc) {
    doc->v = NULL;
    doc->n = 0;
    i32 cap = 0;
    const char *end = text + strlen(text);
    for (const char *p = text; p < end; ) {
        if (doc->n == cap) {
            i32 ncap = cap ? cap * 2 : 256;
            TomlLine *nv = cap < TOML_MAX_LINES
                         ? (TomlLine *)realloc(doc->v, (usize)ncap * sizeof *nv) : NULL;
            if (!nv) { free(doc->v); doc->v = NULL; doc->n = 0; return false; }
            doc->v = nv;
            cap = ncap;
        }
        TomlLine *l = &doc->v[doc->n++];
        memset(l, 0, sizeof *l);
        l->s = p;

        const char *q = p;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        const char *nl = memchr(q, '\n', (usize)(end - q));
        const char *eol = nl ? nl : end;
        l->e = nl ? nl + 1 : end;

        if (q == eol || (*q == '\r' && q + 1 == eol)) {
            l->kind = TL_BLANK;
        } else if (*q == '#') {
            l->kind = TL_COMMENT;
            l->marker = toml_is_marker(q, eol);
        } else if (*q == '[') {
            l->kind = TL_HEADER;
            l->array = q + 1 < eol && q[1] == '[';
            TomlKey k;
            const char *after = toml_key(q + (l->array ? 2 : 1), eol, &k);
            if (after && after < eol && *after == ']') l->tag = (u8)toml_classify(&k);
        } else {
            l->kind = TL_KEYVAL;
            TomlKey k;
            const char *after = toml_key(q, eol, &k);
            if (after && after < eol && *after == '=') {
                l->tag = (u8)toml_classify(&k);
                l->val = after + 1;
                l->e = toml_value_end(l->val, end);
            }
        }
        p = l->e;
    }
    return true;
}

/* The string a `key = "…"` statement assigns, decoded — basic, literal, or
 * either multi-line form. False for anything that is not one plain string. */
static bool toml_string_value(const TomlLine *l, char *out, usize cap) {
    const char *p = l->val, *end = l->e;
    if (!p) return false;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || (*p != '"' && *p != '\'')) return false;
    char q = *p;
    bool triple = end - p >= 3 && p[1] == q && p[2] == q;
    const char *b = p + (triple ? 3 : 1);
    if (triple && b < end && *b == '\r') b++;
    if (triple && b < end && *b == '\n') b++;     /* a newline right after the opener is not content */
    const char *c = b;
    for (;;) {
        if (c >= end) return false;
        if (q == '"' && *c == '\\' && c + 1 < end) { c += 2; continue; }
        if (triple ? (end - c >= 3 && c[0] == q && c[1] == q && c[2] == q) : *c == q) break;
        if (!triple && *c == '\n') return false;
        c++;
    }
    const char *tail = c + (triple ? 3 : 1);
    while (tail < end && (*tail == ' ' || *tail == '\t' || *tail == '\r')) tail++;
    if (tail < end && *tail != '\n' && *tail != '#') return false;
    if (q == '"') return hook_scan_unescape(b, c, out, cap);
    usize n = (usize)(c - b);
    if (n >= cap) return false;
    memcpy(out, b, n);
    out[n] = '\0';
    return true;
}

/* --- which lines are Liu's ----------------------------------------------- */

/* Mark the statements and the blank lines of [a, b) through its last
 * statement. Comments stay (they are somebody's words, and only the two fence
 * lines are known to be ours), and so does whatever trails the last
 * statement: a blank line or a comment there introduces the NEXT table. */
static void codex_mark_span(TomlDoc *d, i32 a, i32 b) {
    i32 last = a;
    for (i32 i = a; i < b; i++)
        if (d->v[i].kind == TL_HEADER || d->v[i].kind == TL_KEYVAL) last = i;
    for (i32 i = a; i <= last; i++)
        if (d->v[i].kind != TL_COMMENT) d->v[i].del = true;
}

/* Mark every line of the document that is Liu's; returns how many Liu hooks
 * that is. install, uninstall and "is it installed" all go through here, so
 * the three always agree.
 *
 * A [[hooks.Stop]] entry is its header, its own keys and the sub-tables that
 * follow it. A [[hooks.Stop.hooks]] sub-table whose command is Liu's goes;
 * the entry goes with it only when no other hook is left in it — a hook of
 * the user's in the same entry keeps the entry and loses nothing. The fence
 * lines go wherever they are, and one blank line in front of a removed table
 * goes with it: that is the separator the install put there, and taking
 * exactly one back out is what makes install → uninstall return the file. */
static i32 codex_mark_liu(TomlDoc *d) {
    TomlLine *L = d->v;
    i32 found = 0;
    for (i32 i = 0; i < d->n; i++)
        if (L[i].marker) L[i].del = true;

    for (i32 h = 0; h < d->n; ) {
        if (!(L[h].kind == TL_HEADER && L[h].array && L[h].tag == TT_STOP)) { h++; continue; }
        i32 end = h + 1;                      /* the next header that is not a sub-table of ours */
        while (end < d->n && !(L[end].kind == TL_HEADER && L[end].tag != TT_STOP_HOOK &&
                               L[end].tag != TT_STOP_HOOK_SUB && L[end].tag != TT_STOP_SUB))
            end++;

        bool keeps = false;                   /* something of the user's lives in this entry */
        i32  ours = 0;
        i32  k = h + 1;
        for (; k < end && L[k].kind != TL_HEADER; k++)
            if (L[k].kind == TL_KEYVAL && L[k].tag == TT_HOOKS) keeps = true;   /* hooks = [ … ] inline */
        while (k < end) {
            bool hook = L[k].array && L[k].tag == TT_STOP_HOOK;
            i32 m = k + 1;                    /* one sub-table, with what is nested under it */
            while (m < end && !(L[m].kind == TL_HEADER &&
                                !(hook && L[m].tag == TT_STOP_HOOK_SUB)))
                m++;
            bool liu = false;
            char cmd[HOOK_SCAN_LIT_CAP];
            for (i32 j = k + 1; hook && j < m && L[j].kind != TL_HEADER; j++)
                if (L[j].kind == TL_KEYVAL && L[j].tag == TT_KEY_COMMAND &&
                    toml_string_value(&L[j], cmd, sizeof cmd) && hook_cmd_is_liu(cmd))
                    liu = true;
            if (liu) { ours++; codex_mark_span(d, k, m); }
            else keeps = true;
            k = m;
        }
        found += ours;
        if (ours > 0 && !keeps) codex_mark_span(d, h, end);
        h = end;
    }

    for (i32 i = 0; i < d->n; i++) {
        if (!L[i].del || (i > 0 && L[i - 1].del)) continue;      /* first line of a removed run */
        bool table = false;
        for (i32 j = i; j < d->n && L[j].del; j++)
            if (L[j].kind == TL_HEADER) table = true;
        if (table && i > 0 && L[i - 1].kind == TL_BLANK) L[i - 1].del = true;
    }
    return found;
}

/* The document without its marked lines, with `extra` spare bytes after the
 * text. Lines are copied whole, terminator and all, so nothing that stays can
 * end up sharing a line with its neighbour. NULL on OOM. */
static char *codex_render(const TomlDoc *d, usize extra, usize *len_out) {
    usize len = 0;
    for (i32 i = 0; i < d->n; i++)
        if (!d->v[i].del) len += (usize)(d->v[i].e - d->v[i].s);
    usize cap;
    if (!checked_add_usize(len, extra, &cap) || !checked_add_usize(cap, 1u, &cap)) return NULL;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    usize o = 0;
    for (i32 i = 0; i < d->n; i++) {
        if (d->v[i].del) continue;
        usize n = (usize)(d->v[i].e - d->v[i].s);
        memcpy(out + o, d->v[i].s, n);
        o += n;
    }
    out[o] = '\0';
    if (len_out) *len_out = o;
    return out;
}

/* Why a [[hooks.Stop]] entry cannot be added to what is left of the document,
 * or NULL when it can.
 *
 * TOML lets a table or an array be written ONCE as a value — `hooks = { … }`,
 * `hooks.Stop = [ … ]`, `[hooks]` + `Stop = [ … ]` — and a value is closed: a
 * later [[hooks.Stop]] header may not add to it. Appending one makes the
 * whole file invalid ("Cannot mutate immutable namespace") and Codex would
 * not start. Liu's entry does not fit beside hooks written that way without
 * rewriting them, so it is not installed and the result says why. */
static const char *codex_append_blocker(const TomlDoc *d) {
    bool root = true, in_hooks = false, has_entry = false;
    for (i32 i = 0; i < d->n; i++) {
        const TomlLine *l = &d->v[i];
        if (l->del) continue;
        if (l->kind == TL_HEADER) {
            root = false;
            in_hooks = l->tag == TT_HOOKS;
            if (l->tag == TT_STOP) {
                if (!l->array) return "[hooks.Stop] is a plain table";
                has_entry = true;
            } else if (!has_entry && (l->tag == TT_STOP_HOOK || l->tag == TT_STOP_HOOK_SUB ||
                                      l->tag == TT_STOP_SUB)) {
                return "hooks.Stop is a plain table";
            }
            continue;
        }
        if (l->kind != TL_KEYVAL) continue;
        if (root && l->tag == TT_HOOKS) return "`hooks = …` is a single value";
        if (root && (l->tag == TT_STOP || l->tag == TT_STOP_HOOK ||
                     l->tag == TT_STOP_HOOK_SUB || l->tag == TT_STOP_SUB))
            return "`hooks.Stop = …` is a single value";
        if (in_hooks && l->tag == TT_KEY_STOP) return "`Stop = …` under [hooks] is a single value";
    }
    return NULL;
}

/* The file's own line ending, so a CRLF config does not get LF lines of ours
 * in the middle of it. */
static const char *codex_eol(const char *text) {
    const char *nl = text ? strchr(text, '\n') : NULL;
    return (nl && nl > text && nl[-1] == '\r') ? "\r\n" : "\n";
}

static AgentHookResult codex_install(const char *bin) {
    char path[1024];
    if (!codex_path(path, sizeof path)) return result(false, "codex: path too long");
    char q[1024], b[1024];
    if (!agent_hook_shell_quote(bin, q, sizeof q)) return result(false, "codex: hook path too long");
    escape_dq(q, b, sizeof b);   /* shell-quoted, then TOML basic-string escaped */

    /* Distinguish "no config" (ok to create) from "present but unreadable
     * or oversized": a config that could not be read is never overwritten,
     * or the user's notify/model/trust lines would vanish. */
    bool absent;
    char *existing = read_text_distinguish(path, &absent);
    if (!existing && !absent)
        return result(false, "codex: %s exists but is unreadable (config left untouched)", path);

    /* Every Liu entry comes out first — the fenced block of the last run AND
     * any copy a rewrite of the file left unfenced — so exactly one entry
     * exists afterwards however many there were. */
    TomlDoc doc;
    if (!toml_lex(existing ? existing : "", &doc)) {
        free(existing);
        return result(false, "codex: config.toml is too large to edit (left untouched)");
    }
    codex_mark_liu(&doc);
    const char *blocker = codex_append_blocker(&doc);
    if (blocker) {
        free(doc.v);
        free(existing);
        return result(false, "codex: %s in config.toml, which a [[hooks.Stop]] entry cannot "
                             "extend — hook not installed (config left untouched)", blocker);
    }

    /* Codex Stop hook = a conversation turn finished. --hook lets liu-notify
     * read Codex's stdin JSON and drop any SubagentStop. */
    const char *nl = codex_eol(existing);
    char block[2048];
    int wrote = snprintf(block, sizeof block,
        CODEX_MARK_START "%s"
        "[[hooks.Stop]]%s"
        "[[hooks.Stop.hooks]]%s"
        "type = \"command\"%s"
        "command = \"%s send --tool=codex --event=complete --hook\"%s"
        CODEX_MARK_END "%s",
        nl, nl, nl, nl, b, nl, nl);
    if (wrote < 0 || (usize)wrote >= sizeof block) {
        free(doc.v);
        free(existing);
        return result(false, "codex: hook path too long");
    }

    /* The whole file = what is left of the config + one blank line + Liu's
     * block, in a buffer sized to the config (a real config.toml can be many
     * KB — a fixed buffer would let snprintf silently TRUNCATE the user's
     * settings). The blank line is the one codex_mark_liu() takes back. */
    usize len = 0;
    char *out = codex_render(&doc, (usize)wrote + 8, &len);
    free(doc.v);
    if (!out) { free(existing); return result(false, "codex: out of memory"); }
    if (len > 0 && out[len - 1] != '\n') len += (usize)snprintf(out + len, 4, "%s", nl);
    if (len > 0) len += (usize)snprintf(out + len, 4, "%s", nl);
    memcpy(out + len, block, (usize)wrote + 1);

    /* Nothing to do when the file already reads exactly so — which it does on
     * every launch after the first. Not rewriting it keeps its inode and
     * mtime still for whatever watches the file. */
    bool same = existing && strcmp(existing, out) == 0;
    free(existing);
    bool ok = same || write_through(path, out);
    free(out);
    if (!ok) return result(false, "codex: write failed (config left untouched)");
    /* Codex requires hook trust before a freshly-added command runs. */
    return result(true, "Codex hook installed — run /hooks in Codex once to trust it");
}

static AgentHookResult codex_uninstall(void) {
    char path[1024];
    if (!codex_path(path, sizeof path)) return result(false, "codex: path too long");
    char *existing = read_text(path);
    if (!existing) return result(true, "Codex: nothing to remove");
    TomlDoc doc;
    if (!toml_lex(existing, &doc)) {
        free(existing);
        return result(false, "codex: config.toml is too large to edit (left untouched)");
    }
    codex_mark_liu(&doc);
    bool changed = false;
    for (i32 i = 0; i < doc.n; i++) changed = changed || doc.v[i].del;
    if (!changed) {
        free(doc.v);
        free(existing);
        return result(true, "Codex: no Liu hook");
    }
    char *stripped = codex_render(&doc, 0, NULL);
    free(doc.v);
    free(existing);
    if (!stripped) return result(false, "codex: out of memory");
    bool ok = write_through(path, stripped);
    free(stripped);
    return ok ? result(true, "Codex hook removed")
              : result(false, "codex: write failed (config left untouched)");
}

static bool codex_is_installed(void) {
    char path[1024];
    if (!codex_path(path, sizeof path)) return false;
    char *t = read_text(path);
    if (!t) return false;
    TomlDoc doc;
    bool found = toml_lex(t, &doc) && codex_mark_liu(&doc) > 0;
    free(doc.v);
    free(t);
    return found;
}

/* ----------------------------------------------------------------- *
 *  antigravity — a "liu-notify" named hook in                        *
 *  ~/.gemini/config/hooks.json (the CLI's global customization root) *
 * ----------------------------------------------------------------- */

/* The top-level key Liu owns in the shared hooks.json. Antigravity merges
 * every named hook for the same event, so this key coexists with the user's /
 * other plugins' hooks; install/uninstall/detect touch ONLY this key. */
#define ANTIGRAVITY_HOOK_KEY "liu-notify"

static bool antigravity_path(char *out, size_t cap) {
    const char *home = home_dir();
    if (!home) return false;
    return (size_t)snprintf(out, cap, "%s/" LIU_HOOK_ANTIGRAVITY_REL, home) < cap;
}

/* Parse hooks.json into a cJSON object. Fresh empty object when the file is
 * genuinely absent or empty (safe to create); NULL when present but
 * unreadable/oversized/malformed — caller MUST abort, never overwrite (or the
 * user's other named hooks vanish). Mirrors claude_hooks.c load_or_empty,
 * including its two refusals: the text must END where the JSON value ends
 * (cJSON_Parse stops at the first value, so a write-back would drop whatever
 * followed it), and `*wide_int` reports an integer the write-back would
 * re-spell (hook_json_has_wide_integer) — a caller about to write must refuse
 * on it, one that only reads may pass NULL. */
static cJSON *antigravity_load_or_empty(const char *path, bool *wide_int) {
    if (wide_int) *wide_int = false;
    bool absent;
    char *txt = read_text_distinguish(path, &absent);
    if (!txt) return absent ? cJSON_CreateObject() : NULL;
    cJSON *root = cJSON_ParseWithOpts(txt, NULL, true);
    if (root && wide_int) *wide_int = hook_json_has_wide_integer(txt);
    free(txt);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool antigravity_write(const char *path, cJSON *root) {
    char *txt = cJSON_Print(root);
    if (!txt) return false;
    bool ok = write_through(path, txt);  /* reuse tmp+rename text writer */
    free(txt);
    return ok;
}

/* The one sentence for a hooks.json whose rewrite would change a number. */
#define ANTIGRAVITY_WIDE_INT_MSG \
    "antigravity: %s holds an integer too long to rewrite exactly (left untouched)"

static AgentHookResult antigravity_install(const char *bin) {
    char path[1024];
    if (!antigravity_path(path, sizeof path)) return result(false, "antigravity: path too long");

    bool wide_int;
    cJSON *root = antigravity_load_or_empty(path, &wide_int);
    if (!root)
        return result(false, "antigravity: %s exists but is unreadable/malformed (left untouched)", path);
    if (wide_int) {
        cJSON_Delete(root);
        return result(false, ANTIGRAVITY_WIDE_INT_MSG, path);
    }

    /* Drop any prior Liu block so re-install is idempotent (refreshes a stale
     * dev-path command, clears a leftover after a crash). */
    cJSON_DeleteItemFromObjectCaseSensitive(root, ANTIGRAVITY_HOOK_KEY);

    /* Antigravity's `Stop` event = the agent's execution loop terminated (a
     * top-level turn finished). ONLY Stop is registered — no PostToolUse and no
     * subagent event — so a finished tool or subagent never notifies.
     *
     * --hook is NOT passed here: Antigravity's stdin payload carries no
     * hook-event-name field, so liu-notify's --hook whitelist (which keys off
     * "hook_event_name"/"hookEventName") would read an unidentifiable payload and
     * drop EVERY notification. The Stop-only registration is the subagent guard.
     *
     * The Stop hook may print a JSON `{"decision":"continue"}` on stdout to keep
     * the agent from stopping; liu-notify prints nothing, which Antigravity reads
     * as "allow stop". */
    char q[1024], cmd[1024];
    if (!agent_hook_shell_quote(bin, q, sizeof q)) {
        cJSON_Delete(root);
        return result(false, "antigravity: command path too long");
    }
    int n = snprintf(cmd, sizeof cmd, "%s send --tool=antigravity --event=complete", q);
    if (n < 0 || (size_t)n >= sizeof cmd) {
        cJSON_Delete(root);
        return result(false, "antigravity: command path too long");
    }

    cJSON *blk  = cJSON_CreateObject();
    cJSON *stop = cJSON_AddArrayToObject(blk, "Stop");
    cJSON *h    = cJSON_CreateObject();
    cJSON_AddStringToObject(h, "type", "command");
    cJSON_AddStringToObject(h, "command", cmd);   /* cJSON handles JSON escaping */
    cJSON_AddItemToArray(stop, h);
    cJSON_AddItemToObject(root, ANTIGRAVITY_HOOK_KEY, blk);

    bool ok = antigravity_write(path, root);
    cJSON_Delete(root);
    if (!ok) return result(false, "antigravity: write failed");
    return result(true, "Antigravity hook installed (%s)", path);
}

static AgentHookResult antigravity_uninstall(void) {
    char path[1024];
    if (!antigravity_path(path, sizeof path)) return result(false, "antigravity: path too long");
    if (!file_exists(path)) return result(true, "Antigravity: nothing to remove");

    bool wide_int;
    cJSON *root = antigravity_load_or_empty(path, &wide_int);
    if (!root)
        return result(false, "antigravity: %s unreadable/malformed (left untouched)", path);
    if (!cJSON_GetObjectItemCaseSensitive(root, ANTIGRAVITY_HOOK_KEY)) {
        cJSON_Delete(root);
        return result(true, "Antigravity: no Liu hook");
    }
    if (wide_int) {
        cJSON_Delete(root);
        return result(false, ANTIGRAVITY_WIDE_INT_MSG, path);
    }
    cJSON_DeleteItemFromObjectCaseSensitive(root, ANTIGRAVITY_HOOK_KEY);

    /* If Liu's block was the file's only content, the file is removed so
     * nothing is left behind; otherwise the user's remaining hooks are kept
     * intact. A LINK is never removed, though: the name is the user's (a
     * dotfiles manager put it there), so the emptied document is written
     * through it and the link stays. Unlinking it would leave the linked copy
     * still holding Liu's hook and the agent reading nothing. */
    char target[1024];
    bool is_link = false;
    bool ok = agent_hook_write_path(path, target, sizeof target, &is_link);
    if (ok && cJSON_GetArraySize(root) == 0 && !is_link)
        ok = (unlink(path) == 0 || errno == ENOENT);
    else if (ok)
        ok = antigravity_write(path, root);
    cJSON_Delete(root);
    return ok ? result(true, "Antigravity hook removed") : result(false, "antigravity: write failed");
}

static bool antigravity_is_installed(void) {
    char path[1024];
    if (!antigravity_path(path, sizeof path) || !file_exists(path)) return false;
    cJSON *root = antigravity_load_or_empty(path, NULL);
    if (!root) return false;
    bool found = cJSON_GetObjectItemCaseSensitive(root, ANTIGRAVITY_HOOK_KEY) != NULL;
    cJSON_Delete(root);
    return found;
}

/* ----------------------------------------------------------------- *
 *  hook shim (see agent_hooks.h for why this exists)                 *
 * ----------------------------------------------------------------- */

bool agent_hook_shim_path(char *out, usize cap) {
    const char *home = home_dir();
    if (!home || !out || cap == 0) return false;
    return (usize)snprintf(out, cap, "%s/" LIU_HOOK_SHIM_REL, home) < cap;
}

/* Single-quote `in` for /bin/sh: wrap in '…' and rewrite each embedded quote
 * as '\''. Public because every hook installer writes the same path into a
 * command an agent runs through a shell: unquoted, a home directory with a
 * space would break the hook and one with a shell metacharacter in it would
 * be parsed as syntax. */
bool agent_hook_shell_quote(const char *in, char *out, usize cap) {
    usize j = 0;
    if (cap < 3) return false;
    out[j++] = '\'';
    for (const char *p = in; *p; p++) {
        if (*p == '\'') {
            if (j + 4 >= cap) return false;
            out[j++] = '\''; out[j++] = '\\'; out[j++] = '\''; out[j++] = '\'';
        } else {
            if (j + 2 >= cap) return false;
            out[j++] = *p;
        }
    }
    if (j + 2 >= cap) return false;
    out[j++] = '\'';
    out[j] = '\0';
    return true;
}

AgentHookResult agent_hook_shim_install(const char *notify_bin) {
    if (!notify_bin || !*notify_bin)
        return result(false, "shim: missing liu-notify path");

    /* The shim runs in the AGENT's working directory, not Liu's, so its target
     * has to mean the same thing from anywhere. A relative path is pinned to
     * the file it names now; a bare name (the PATH-lookup fallback, see
     * diag_notify_bin) stays a name and is looked up each time the shim runs. */
    char pinned[PATH_MAX];
    bool by_name = strchr(notify_bin, '/') == NULL;
    if (!by_name && notify_bin[0] != '/' && realpath(notify_bin, pinned))
        notify_bin = pinned;

    char path[1024];
    if (!agent_hook_shim_path(path, sizeof path))
        return result(false, "shim: path too long");

    char q[1152];
    if (!agent_hook_shell_quote(notify_bin, q, sizeof q))
        return result(false, "shim: binary path too long");

    /* `exec` so the agent waits on liu-notify itself and sees its exit status
     * directly — no extra shell frame between them. stdin passes through
     * untouched, which the --hook payload parser depends on.
     *
     * A bare name gets two more lines. `[ -x "$b" ]` on a bare name would
     * look for a file of that name in the agent's working directory, so
     * `command -v` first resolves it through the shell's own PATH lookup; the
     * line after it is for a PATH that leads straight back to this file. */
    char text[2048];
    int n = snprintf(text, sizeof text,
        "#!/bin/sh\n"
        "# liu-notify hook shim — written by Liu, do not edit.\n"
        "# Agents record THIS path, so a Liu update/move/uninstall can never\n"
        "# leave them running a command that no longer exists: when the real\n"
        "# binary is gone this exits 0 in silence instead of failing the hook.\n"
        "b=%s\n"
        "%s"
        "[ -x \"$b\" ] || exit 0\n"
        "exec \"$b\" \"$@\"\n", q,
        by_name ? "b=$(command -v \"$b\" 2>/dev/null) || exit 0\n"
                  "[ \"$b\" = \"$0\" ] && exit 0\n" : "");
    if (n < 0 || (usize)n >= sizeof text)
        return result(false, "shim: render overflow");

    /* Rewrite only on change: agents that fingerprint their hook config (Codex
     * asks the user to re-trust a "new or changed" hook) must not see churn
     * from a launch that changed nothing. */
    char *cur = read_text(path);
    bool same = (cur && strcmp(cur, text) == 0);
    free(cur);
    if (!same && !atomic_write(path, text))
        return result(false, "shim: write failed (%s)", path);

    /* Shared configuration stays private; the shim has to be executable. */
    if (chmod(path, 0755) != 0 && errno != ENOENT)
        return result(false, "shim: chmod failed (%s)", path);

    return result(true, "hook shim ready (%s)", path);
}

/* ----------------------------------------------------------------- *
 *  dispatch                                                          *
 * ----------------------------------------------------------------- */

bool agent_hook_supported(const char *id) {
    if (!id) return false;
    return !strcmp(id, "claude") || !strcmp(id, "grok") ||
           !strcmp(id, "codex")  || !strcmp(id, "opencode") ||
           !strcmp(id, "antigravity") || !strcmp(id, "commandcode") ||
           !strcmp(id, "pi") || !strcmp(id, "omp");
}

AgentHookResult agent_hook_install(const char *id, const char *bin) {
    if (!id || !bin || !*bin) return result(false, "missing agent id / binary");
    if (!strcmp(id, "claude"))      return claude_hooks_install(bin);
    if (!strcmp(id, "grok"))        return grok_install(bin);
    if (!strcmp(id, "opencode"))    return opencode_install(bin);
    if (!strcmp(id, "codex"))       return codex_install(bin);
    if (!strcmp(id, "antigravity")) return antigravity_install(bin);
    if (!strcmp(id, "commandcode")) return commandcode_hooks_install(bin);
    if (!strcmp(id, "pi"))          return pi_install(bin);
    if (!strcmp(id, "omp"))         return omp_install(bin);
    return result(false, "%s: no hook integration", id);
}

/* What the launch does for every agent, as one call (see agent_hooks.h). */
AgentHookResult agent_hook_install_shimmed(const char *id, const char *notify_bin) {
    if (!id || !notify_bin || !*notify_bin) return result(false, "missing agent id / binary");
    if (!agent_hook_supported(id)) return result(false, "%s: no hook integration", id);
    char shim[1024];
    if (agent_hook_shim_install(notify_bin).ok && agent_hook_shim_path(shim, sizeof shim))
        return agent_hook_install(id, shim);
    return agent_hook_install(id, notify_bin);   /* If shim installation fails, record notify_bin directly. */
}

AgentHookResult agent_hook_uninstall(const char *id) {
    if (!id) return result(false, "missing agent id");
    if (!strcmp(id, "claude"))      return claude_hooks_uninstall();
    if (!strcmp(id, "grok"))        return grok_uninstall();
    if (!strcmp(id, "opencode"))    return opencode_uninstall();
    if (!strcmp(id, "codex"))       return codex_uninstall();
    if (!strcmp(id, "antigravity")) return antigravity_uninstall();
    if (!strcmp(id, "commandcode")) return commandcode_hooks_uninstall();
    if (!strcmp(id, "pi"))          return pi_uninstall();
    if (!strcmp(id, "omp"))         return omp_uninstall();
    return result(false, "%s: no hook integration", id);
}

bool agent_hook_installed(const char *id) {
    if (!id) return false;
    if (!strcmp(id, "claude"))      return claude_hooks_installed();
    if (!strcmp(id, "grok"))        return grok_is_installed();
    if (!strcmp(id, "opencode"))    return opencode_is_installed();
    if (!strcmp(id, "codex"))       return codex_is_installed();
    if (!strcmp(id, "antigravity")) return antigravity_is_installed();
    if (!strcmp(id, "commandcode")) return commandcode_hooks_installed();
    if (!strcmp(id, "pi"))          return pi_is_installed();
    if (!strcmp(id, "omp"))         return omp_is_installed();
    return false;
}
