/*
 * liu-history — list / show / info / scan agent session files.
 *
 *   liu-history list [--tool=claude|codex|copilot] [--limit=N]
 *   liu-history show <id|path> [--format=text|markdown|json] [--roles=user,assistant,tool]
 *   liu-history info <id|path>
 *   liu-history scan
 */
#include "history/event.h"
#include "history/session.h"
#include "history/parser.h"
#include "history/formatter.h"
#include "history/util.h"
#include "core/memory.h"
#include "core/types.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- */

static const char *arg_value(const char *arg, const char *key) {
    size_t klen = strlen(key);
    if (strncmp(arg, key, klen) != 0) return NULL;
    if (arg[klen] != '=') return NULL;
    return arg + klen + 1;
}

static u32 parse_tools_mask(const char *s) {
    if (!s || !*s) return 0;
    u32 mask = 0;
    if (strcmp(s, "claude") == 0)      mask |= 1u << CHAT_TOOL_CLAUDE;
    if (strcmp(s, "codex") == 0)       mask |= 1u << CHAT_TOOL_CODEX;
    if (strcmp(s, "copilot") == 0)     mask |= 1u << CHAT_TOOL_COPILOT;
    if (strcmp(s, "antigravity") == 0) mask |= 1u << CHAT_TOOL_ANTIGRAVITY;
    if (strcmp(s, "cline") == 0)       mask |= 1u << CHAT_TOOL_CLINE;
    if (strcmp(s, "roo") == 0)         mask |= 1u << CHAT_TOOL_ROO;
    if (strcmp(s, "kilo") == 0)        mask |= 1u << CHAT_TOOL_KILO;
    if (strcmp(s, "amp") == 0)         mask |= 1u << CHAT_TOOL_AMP;
    if (strcmp(s, "kiro") == 0)        mask |= 1u << CHAT_TOOL_KIRO;
    if (strcmp(s, "cursor") == 0)      mask |= 1u << CHAT_TOOL_CURSOR;
    if (strcmp(s, "droid") == 0)       mask |= 1u << CHAT_TOOL_DROID;
    if (strcmp(s, "opencode") == 0)    mask |= 1u << CHAT_TOOL_OPENCODE;
    if (strcmp(s, "kimi") == 0)        mask |= 1u << CHAT_TOOL_KIMI;
    if (strcmp(s, "qwen") == 0)        mask |= 1u << CHAT_TOOL_QWEN;
    if (strcmp(s, "all") == 0)         mask = (CHAT_TOOL_COUNT_ >= 32)
                                              ? 0xFFFFFFFFu
                                              : ((1u << CHAT_TOOL_COUNT_) - 1u);
    return mask;
}

static u32 parse_roles(const char *s) {
    if (!s || !*s) return ROLE_MASK_ALL;
    u32 m = 0;
    const char *p = s;
    while (*p) {
        const char *c = strchr(p, ',');
        size_t n = c ? (size_t)(c - p) : strlen(p);
        if (n == 4 && !strncmp(p, "user", 4))          m |= ROLE_MASK_USER;
        else if (n == 9 && !strncmp(p, "assistant", 9)) m |= ROLE_MASK_ASSISTANT;
        else if (n == 4 && !strncmp(p, "tool", 4))      m |= ROLE_MASK_TOOL_USE | ROLE_MASK_TOOL_RESULT;
        else if (n == 8 && !strncmp(p, "tool_use", 8))  m |= ROLE_MASK_TOOL_USE;
        else if (n == 11 && !strncmp(p, "tool_result", 11)) m |= ROLE_MASK_TOOL_RESULT;
        else if (n == 6 && !strncmp(p, "system", 6))    m |= ROLE_MASK_SYSTEM;
        if (!c) break;
        p = c + 1;
    }
    return m ? m : ROLE_MASK_ALL;
}

static HistFormat parse_format(const char *s) {
    if (!s) return HIST_FMT_TEXT;
    if (strcmp(s, "markdown") == 0 || strcmp(s, "md") == 0) return HIST_FMT_MARKDOWN;
    if (strcmp(s, "json") == 0)                             return HIST_FMT_JSON;
    return HIST_FMT_TEXT;
}

/* ------------------------------------------------------------------------- */

typedef struct {
    u32 limit;
    u32 printed;
} ListCtx;

static bool list_cb(const ChatSessionMeta *m, void *user) {
    ListCtx *ctx = user;
    if (ctx->printed >= ctx->limit) return false;
    char ts[40];
    if (m->last_modified_ms > 0) {
        time_t s = (time_t)(m->last_modified_ms / 1000);
        struct tm tm; gmtime_r(&s, &tm);
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    } else {
        snprintf(ts, sizeof ts, "-");
    }
    printf("%-8s  %-38s  %-45s  %8lld  %s\n",
           chat_tool_name(m->tool),
           m->session_id, m->project,
           (long long)m->size_bytes, ts);
    ctx->printed++;
    return true;
}

static int cmd_list(int argc, char **argv) {
    u32 tools = 0;
    u32 limit = 20;
    for (int i = 0; i < argc; i++) {
        const char *v;
        if ((v = arg_value(argv[i], "--tool")) != NULL)  tools = parse_tools_mask(v);
        else if ((v = arg_value(argv[i], "--limit")) != NULL) limit = (u32)strtoul(v, NULL, 10);
    }
    printf("%-8s  %-38s  %-45s  %8s  %s\n",
           "TOOL", "SESSION_ID", "PROJECT", "BYTES", "MODIFIED");

    Arena a = arena_create(MB(16));
    if (!a.base) { fprintf(stderr, "liu-history: arena alloc failed\n"); return 1; }
    ListCtx ctx = { .limit = limit ? limit : 20, .printed = 0 };
    chat_scan(&a, tools, list_cb, &ctx);
    arena_destroy(&a);
    return 0;
}

/* ------------------------------------------------------------------------- */

static bool scan_cb_scan(const ChatSessionMeta *m, void *user) {
    (void)user;
    printf("%-8s\t%s\n", chat_tool_name(m->tool), m->path);
    return true;
}

static int cmd_scan(void) {
    Arena a = arena_create(MB(16));
    chat_scan(&a, 0, scan_cb_scan, NULL);
    arena_destroy(&a);
    return 0;
}

/* ------------------------------------------------------------------------- */

typedef struct {
    HistFormatter *f;
} ShowCtx;

static bool show_cb(const ChatEvent *e, void *user) {
    ShowCtx *ctx = user;
    hist_formatter_emit(ctx->f, e);
    return true;
}

static int cmd_show(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "liu-history show: missing <session-id|path>\n"); return 64; }
    const char *needle = argv[0];
    HistFormat fmt = HIST_FMT_TEXT;
    u32 roles = ROLE_MASK_ALL;
    for (int i = 1; i < argc; i++) {
        const char *v;
        if ((v = arg_value(argv[i], "--format")) != NULL) fmt = parse_format(v);
        else if ((v = arg_value(argv[i], "--roles")) != NULL) roles = parse_roles(v);
    }

    Arena scan_a = arena_create(MB(16));
    ChatSessionMeta meta;
    bool found = chat_find_session(&scan_a, needle, &meta);
    if (!found) {
        arena_destroy(&scan_a);
        fprintf(stderr, "liu-history: session not found: %s\n", needle);
        return 1;
    }

    HistFormatter f;
    hist_formatter_init(&f, fmt, roles, isatty(fileno(stdout)));
    hist_formatter_begin(&f);

    Arena parse_a = arena_create(MB(16));
    if (!parse_a.base) {
        arena_destroy(&scan_a);
        fprintf(stderr, "liu-history: parse arena alloc failed\n");
        return 1;
    }
    ShowCtx ctx = { .f = &f };
    parser_run_file(&parse_a, meta.tool, meta.path, show_cb, &ctx);
    arena_destroy(&parse_a);
    arena_destroy(&scan_a);

    hist_formatter_finish(&f);
    return 0;
}

/* ------------------------------------------------------------------------- */

typedef struct { u32 turns; } InfoCtx;
static bool info_cb(const ChatEvent *e, void *user) {
    (void)e;
    InfoCtx *ctx = user;
    ctx->turns++;
    return true;
}

static int cmd_info(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "liu-history info: missing <session-id|path>\n"); return 64; }
    const char *needle = argv[0];

    Arena a = arena_create(MB(16));
    ChatSessionMeta meta;
    bool found = chat_find_session(&a, needle, &meta);
    if (!found) {
        arena_destroy(&a);
        fprintf(stderr, "liu-history: session not found: %s\n", needle);
        return 1;
    }

    char ts[40];
    if (meta.last_modified_ms > 0) {
        time_t s = (time_t)(meta.last_modified_ms / 1000);
        struct tm tm; gmtime_r(&s, &tm);
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    } else {
        snprintf(ts, sizeof ts, "-");
    }

    /* Count events lazily — users who just want metadata can get it fast. */
    Arena parse_a = arena_create(MB(8));
    InfoCtx ctx = {0};
    parser_run_file(&parse_a, meta.tool, meta.path, info_cb, &ctx);
    arena_destroy(&parse_a);

    printf("tool:          %s\n",     chat_tool_name(meta.tool));
    printf("session_id:    %s\n",     meta.session_id ? meta.session_id : "");
    printf("project:       %s\n",     meta.project    ? meta.project    : "");
    printf("path:          %s\n",     meta.path       ? meta.path       : "");
    printf("size_bytes:    %lld\n",   (long long)meta.size_bytes);
    printf("modified:      %s\n",     ts);
    printf("event_count:   %u\n",     ctx.turns);
    arena_destroy(&a);
    return 0;
}

/* ------------------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  liu-history list [--tool=claude|codex|copilot] [--limit=N]\n"
        "  liu-history show <session-id|path> [--format=text|markdown|json]\n"
        "                                      [--roles=user,assistant,tool,system]\n"
        "  liu-history info <session-id|path>\n"
        "  liu-history scan\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 64; }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "list")) return cmd_list(argc - 2, argv + 2);
    if (!strcmp(cmd, "show")) return cmd_show(argc - 2, argv + 2);
    if (!strcmp(cmd, "info")) return cmd_info(argc - 2, argv + 2);
    if (!strcmp(cmd, "scan")) return cmd_scan();
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) { usage(); return 0; }
    usage();
    return 64;
}
