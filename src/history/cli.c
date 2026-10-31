/*
 * liu-history — list / show / info / scan agent session files.
 *
 *   liu-history list [--tool=claude|codex|commandcode|…|all] [--limit=N]
 *   liu-history show <id|path> [--format=text|markdown|json] [--roles=user,assistant,tool]
 *   liu-history info <id|path>
 *   liu-history scan
 */
#include "history/event.h"
#include "history/session.h"
#include "history/parser.h"
#include "history/formatter.h"
#include "history/util.h"
#include "history/bridge/bridge.h"
#include "history/bridge/lineage.h"
#include "history/bridge/compatibility.h"
#include "core/memory.h"
#include "core/checked.h"
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

/* --tool=<id>. Ids come from chat_tool_name() via chat_tool_from_agent_id(),
 * so every tool the enum knows is filterable the moment it is added.
 *
 * Returns 0 for an unknown id; callers must reject that rather than pass it
 * on: chat_scan reads a mask of 0 as "scan everything", so the filter would
 * look like it worked while listing every agent. */
static ChatToolMask parse_tools_mask(const char *s) {
    if (!s || !*s) return 0;
    if (strcmp(s, "all") == 0)
        return (CHAT_TOOL_COUNT_ >= 32) ? 0xFFFFFFFFu
                                        : (CHAT_TOOL_BIT(CHAT_TOOL_COUNT_) - 1u);
    ChatTool t = chat_tool_from_agent_id(s);
    return (t == CHAT_TOOL_UNKNOWN) ? 0 : CHAT_TOOL_BIT(t);
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
    /* OpenCode's byte total is not measured — see the comment on the query in
     * scan_opencode(). Printing 0 there would read as "empty session", so the
     * column shows "-" and the message count moves into the ID slot's place
     * via `info`, which already prints event_count. */
    char szcol[16];
    if (chat_tool_is_db_store(m->tool)) snprintf(szcol, sizeof szcol, "%s", "-");
    else snprintf(szcol, sizeof szcol, "%lld", (long long)m->size_bytes);
    printf("%-8s  %-38s  %-45s  %8s  %s\n",
           chat_tool_name(m->tool),
           m->session_id, m->project,
           szcol, ts);
    ctx->printed++;
    return true;
}

static int cmd_list(int argc, char **argv) {
    ChatToolMask tools = 0;
    u32 limit = 20;
    for (int i = 0; i < argc; i++) {
        const char *v;
        if ((v = arg_value(argv[i], "--tool")) != NULL) {
            tools = parse_tools_mask(v);
            if (tools == 0) {
                fprintf(stderr, "liu-history: unknown tool '%s'\n", v);
                return 64;
            }
        }
        else if ((v = arg_value(argv[i], "--limit")) != NULL) {
            i32 parsed;
            if (checked_parse_i32(v, &parsed) && parsed > 0)
                limit = (u32)parsed;
        }
    }
    printf("%-8s  %-38s  %-45s  %8s  %s\n",
           "TOOL", "SESSION_ID", "PROJECT", "BYTES", "MODIFIED");

    Arena *a = chat_scan_arena_borrow();
    if (!a) { fprintf(stderr, "liu-history: arena alloc failed\n"); return 1; }
    ListCtx ctx = { .limit = limit ? limit : 20, .printed = 0 };
    chat_scan(a, tools, list_cb, &ctx);
    return 0;
}

/* ------------------------------------------------------------------------- */

static bool scan_cb_scan(const ChatSessionMeta *m, void *user) {
    (void)user;
    printf("%-8s\t%s\n", chat_tool_name(m->tool), m->path);
    return true;
}

static int cmd_scan(void) {
    chat_scan_with_arena(0, scan_cb_scan, NULL);
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

    Arena *scan_a = chat_scan_arena_borrow();
    if (!scan_a) {
        fprintf(stderr, "liu-history: arena alloc failed\n");
        return 1;
    }
    ChatSessionMeta meta;
    bool found = chat_find_session(scan_a, needle, &meta);
    if (!found) {
        fprintf(stderr, "liu-history: session not found: %s\n", needle);
        return 1;
    }

    HistFormatter f;
    hist_formatter_init(&f, fmt, roles, isatty(fileno(stdout)));
    hist_formatter_begin(&f);

    Arena parse_a = arena_create(MB(16));
    if (!parse_a.base) {
        fprintf(stderr, "liu-history: parse arena alloc failed\n");
        return 1;
    }
    ShowCtx ctx = { .f = &f };
    parser_run_file(&parse_a, meta.tool, meta.path, show_cb, &ctx);
    arena_destroy(&parse_a);

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

    Arena *a = chat_scan_arena_borrow();
    if (!a) {
        fprintf(stderr, "liu-history: arena alloc failed\n");
        return 1;
    }
    ChatSessionMeta meta;
    bool found = chat_find_session(a, needle, &meta);
    if (!found) {
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

    /* A DB-backed tool has no transcript file to parse: meta.path points at
     * the shared SQLite store, so the parse above reads zero events and this
     * row would report an empty session. The scanner already counted the
     * messages in SQL (scan_opencode's COUNT(*) subquery) and the picker
     * shows exactly that number, so fall back to it rather than print a 0
     * that contradicts both. Only ever non-zero for OpenCode today. */
    u32 events = ctx.turns ? ctx.turns : meta.event_count;

    printf("tool:          %s\n",     chat_tool_name(meta.tool));
    printf("session_id:    %s\n",     meta.session_id ? meta.session_id : "");
    printf("project:       %s\n",     meta.project    ? meta.project    : "");
    printf("path:          %s\n",     meta.path       ? meta.path       : "");
    if (chat_tool_is_db_store(meta.tool) && meta.size_bytes == 0)
        printf("size_bytes:    - (not measured for %s)\n", chat_tool_name(meta.tool));
    else
        printf("size_bytes:    %lld\n",   (long long)meta.size_bytes);
    printf("modified:      %s\n",     ts);
    printf("event_count:   %u\n",     events);
    return 0;
}


/* ------------------------------------------------------------------------- */
/* This command calls bridge_migrate(), the same migration path used by the
 * UI, and exposes target capabilities without opening the interface.
 * capabilities reports which targets and source formats are available on this
 * machine. */

static int cmd_capabilities(void) {
    printf("%-14s %-16s %-13s %-10s %s\n",
           "AGENT", "IMPORT MODE", "QUALITY", "VERSION", "AS SOURCE");
    for (i32 t = 1; t < CHAT_TOOL_COUNT_; t++) {
        ChatTool tool = (ChatTool)t;
        const BridgeTarget *tgt = bridge_target_for(tool);
        bool src = bridge_source_supported(tool);
        if (!tgt && !src) continue;

        BridgeImportMode m = bridge_capability(tool);
        char ver[64] = "";
        bridge_compat_installed_version(tool, ver, sizeof ver);
        printf("%-14s %-16s %-13s %-10s %s\n",
               chat_tool_name(tool),
               bridge_import_mode_name(m),
               bridge_import_mode_badge(m),
               ver[0] ? ver : "-",
               src ? "yes" : "no");
    }
    return 0;
}

static int cmd_continue(int argc, char **argv) {
    const char *needle = NULL;
    const char *target = NULL;
    bool dry = false;

    for (int i = 0; i < argc; i++) {
        const char *v;
        if ((v = arg_value(argv[i], "--to")) != NULL)      { target = v; continue; }
        if (!strcmp(argv[i], "--dry-run"))                 { dry = true; continue; }
        if (argv[i][0] != '-' && !needle)                  { needle = argv[i]; continue; }
    }
    if (!needle || !target) {
        fprintf(stderr, "usage: liu-history continue <session-id|path> --to=<agent> [--dry-run]\n");
        return 64;
    }

    ChatTool tt = chat_tool_from_agent_id(target);
    if (tt == CHAT_TOOL_UNKNOWN) {
        fprintf(stderr, "liu-history: unknown target agent '%s'\n", target);
        return 64;
    }

    Arena *scan = chat_scan_arena_borrow();
    ChatSessionMeta meta;
    if (!scan || !chat_find_session(scan, needle, &meta)) {
        fprintf(stderr, "liu-history: no session matching '%s'\n", needle);
        return 66;
    }

    /* --dry-run parses the source and reports the fidelity WITHOUT creating
     * anything. It is the only way to see what a migration would carry before
     * it writes into another application's store, which is exactly the thing
     * a user should be able to look at first. */
    if (dry) {
        Arena a = arena_create(MB(64));
        BridgeHistory h;
        if (!a.base || !bridge_parse_source(&a, &meta, &h)) {
            fprintf(stderr, "liu-history: cannot parse that session\n");
            if (a.base) arena_destroy(&a);
            return 65;
        }
        printf("source      %s  %s\n", chat_tool_name(meta.tool), meta.session_id);
        printf("cwd         %s\n", h.cwd ? h.cwd : "(unknown)");
        printf("title       %s\n", h.title ? h.title : "(none)");
        printf("model       %s\n", h.source_model ? h.source_model : "(unknown)");
        printf("messages    %u\n", h.fidelity.messages);
        printf("tool calls  %u\n", h.fidelity.tool_calls);
        printf("tool results %u\n", h.fidelity.tool_results);
        printf("dropped     %u\n", h.fidelity.events_dropped);
        printf("truncated   %u tool outputs, %u messages\n",
               h.fidelity.tool_outputs_truncated, h.fidelity.messages_truncated);
        printf("fidelity    %s\n", bridge_fidelity_name(bridge_fidelity_of(&h.fidelity)));
        printf("target      %s (%s)\n", chat_tool_name(tt),
               bridge_import_mode_name(bridge_capability(tt)));
        bridge_history_free(&h);
        arena_destroy(&a);
        return 0;
    }

    BridgeImportResult r;
    if (!bridge_migrate(&meta, tt, &r)) {
        fprintf(stderr, "liu-history: %s\n", r.error[0] ? r.error : "migration failed");
        return 65;
    }
    printf("%s session %s\n", bridge_import_mode_name(r.mode), r.session_id);
    if (r.session_path[0]) printf("path      %s\n", r.session_path);
    printf("fidelity  %s\n", bridge_fidelity_name(r.fidelity));
    if (r.warnings) printf("warnings  %u\n", r.warnings);
    if (r.launch_cmd[0]) printf("resume    %s\n", r.launch_cmd);
    return 0;
}

static bool lineage_print_cb(const BridgeLineage *rec, void *user) {
    (void)user;
    char when[32] = "-";
    if (rec->created_at_ms > 0) {
        time_t s = (time_t)(rec->created_at_ms / 1000);
        struct tm tmv;
        localtime_r(&s, &tmv);
        strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tmv);
    }
    printf("%s  %-12s %-24s -> %-12s %-24s  %s\n", when,
           chat_tool_name(rec->source_tool), rec->source_session_id,
           chat_tool_name(rec->target_tool), rec->target_session_id,
           bridge_fidelity_name(rec->fidelity));
    return true;
}

static int cmd_lineage(int argc, char **argv) {
    u32 limit = 50;
    for (int i = 0; i < argc; i++) {
        const char *v = arg_value(argv[i], "--limit");
        if (v) { long n = strtol(v, NULL, 10); if (n > 0) limit = (u32)n; }
    }
    u32 n = bridge_lineage_scan(lineage_print_cb, NULL, limit);
    if (n == 0) printf("(no cross-agent migrations recorded)\n");
    return 0;
}

/* ------------------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  liu-history list [--tool=claude|codex|commandcode|…|all] [--limit=N]\n"
        "  liu-history show <session-id|path> [--format=text|markdown|json]\n"
        "                                      [--roles=user,assistant,tool,system]\n"
        "  liu-history info <session-id|path>\n"
        "  liu-history scan\n"
        "\n"
        "cross-agent session continuation:\n"
        "  liu-history capabilities\n"
        "  liu-history continue <session-id|path> --to=<agent> [--dry-run]\n"
        "  liu-history lineage [--limit=N]\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 64; }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "list")) return cmd_list(argc - 2, argv + 2);
    if (!strcmp(cmd, "show")) return cmd_show(argc - 2, argv + 2);
    if (!strcmp(cmd, "info")) return cmd_info(argc - 2, argv + 2);
    if (!strcmp(cmd, "scan")) return cmd_scan();
    if (!strcmp(cmd, "capabilities")) return cmd_capabilities();
    if (!strcmp(cmd, "continue")) return cmd_continue(argc - 2, argv + 2);
    if (!strcmp(cmd, "lineage"))  return cmd_lineage(argc - 2, argv + 2);
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) { usage(); return 0; }
    usage();
    return 64;
}
