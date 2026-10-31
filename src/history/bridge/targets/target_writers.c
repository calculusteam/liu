/*
 * liu-history/bridge - the writer-backed target adapters.
 *
 * Eight agents with no import API but a session store Liu can write:
 * Claude Code, Command Code, Qwen Code, Grok, Kimi, the Cline CLI, Pi and
 * Oh My Pi. Each is
 * a thin shell around its writer in writers/, and the shell is where all the
 * safety lives:
 *
 *   1. availability   — binary on PATH
 *   2. version gate   — bridge_capability() has already refused to route here
 *                       unless bridge_compat_writer_ok() passed, so an
 *                       unknown or out-of-range version never reaches a
 *                       writer. It degrades to a handoff instead.
 *   3. a cwd          — every one of these stores is keyed by working
 *                       directory. A session written without one lands
 *                       somewhere the agent's own picker will not look.
 *   4. a FRESH id     — minted here, collision-checked against the store
 *                       before a byte is written. A collision is a hard
 *                       failure: overwriting a real session is the one thing
 *                       this subsystem could do that would destroy a user's
 *                       work, so it is never a merge and never a retry into
 *                       the same path.
 *   5. validation     — the file exists, is non-empty, and re-reads as the
 *                       session the writer claimed to create.
 *
 * The launch commands are the same ones AGENT_CATALOG uses for a native
 * resume, because after a successful write these ARE native sessions.
 */
#include "history/bridge/targets.h"
#include "history/bridge/compatibility.h"
#include "history/bridge/handoff.h"
#include "history/bridge/writers.h"
#include "history/util.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Shared preflight. Returns false (with `r->error` set) when the migration
 * must not proceed to a writer. */
static bool writer_preflight(const BridgeHistory *h, BridgeImportResult *r) {
    r->mode = BRIDGE_IMPORT_INTERNAL_WRITER;
    if (!h->cwd || h->cwd[0] != '/') {
        snprintf(r->error, sizeof r->error,
                 "Source session does not record a working directory, and this "
                 "agent files sessions by directory.");
        return false;
    }
    return true;
}

static bool file_nonempty(const char *path) {
    struct stat st;
    return path && path[0] && lstat(path, &st) == 0 &&
           S_ISREG(st.st_mode) && st.st_size > 0;
}

/* ------------------------------------------------------------- Claude ----- */

static bool claude_available(void) { return bridge_binary_available("claude"); }
static bool yes(void) { return true; }

static bool claude_import(const BridgeHistory *h, BridgeImportResult *r) {
    if (!writer_preflight(h, r)) return false;

    char dir[BRIDGE_PATH_CAP];
    if (!bridge_claude_project_dir(h->cwd, dir, sizeof dir) || !bridge_mkdir_p(dir)) {
        snprintf(r->error, sizeof r->error, "Cannot prepare Claude's project directory.");
        return false;
    }

    char uuid[64];
    char path[BRIDGE_PATH_CAP];
    /* Mint, then check. Three attempts is not superstition about UUID
     * collisions — it covers the real case, which is a stale file left by an
     * earlier crash at the same path. */
    bool have = false;
    for (i32 attempt = 0; attempt < 3 && !have; attempt++) {
        if (!bridge_new_uuid(uuid, sizeof uuid)) break;
        int n = snprintf(path, sizeof path, "%s/%s.jsonl", dir, uuid);
        if (n <= 0 || (usize)n >= sizeof path) break;
        if (!bridge_path_exists(path)) have = true;
    }
    if (!have) {
        snprintf(r->error, sizeof r->error, "Could not allocate a free Claude session id.");
        return false;
    }

    u32 warnings = 0;
    if (!bridge_write_claude_session(h, uuid, path, true, &warnings)) {
        snprintf(r->error, sizeof r->error, "Failed writing the Claude session transcript.");
        return false;
    }

    hist_copy_bounded(r->session_id,   sizeof r->session_id,   uuid);
    hist_copy_bounded(r->session_path, sizeof r->session_path, path);
    r->warnings += warnings;
    snprintf(r->launch_cmd, sizeof r->launch_cmd, "claude --resume %s", uuid);
    return true;
}

/* Validation for the file-backed writers: the transcript exists, is
 * non-empty, and its FIRST line parses as one of the records the writer
 * wrote. That last part is what separates "fwrite returned" from "the agent
 * can read this" without launching the agent — a truncated or mis-nested
 * document fails here rather than in front of the user. */
static bool jsonl_first_line_ok(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char buf[8192];
    bool ok = false;
    if (fgets(buf, sizeof buf, f)) ok = strstr(buf, needle) != NULL;
    fclose(f);
    return ok;
}

static bool claude_validate(const BridgeImportResult *r) {
    if (!file_nonempty(r->session_path)) return false;
    /* Claude resumes by walking the parentUuid chain from a leaf. The chain's
     * root is the only line with "parentUuid":null, so its presence on line 1
     * is exactly the property a broken chain would lose. */
    return jsonl_first_line_ok(r->session_path, "\"parentUuid\":null");
}

const BridgeTarget g_bridge_target_claude = {
    .tool                  = CHAT_TOOL_CLAUDE,
    .mode                  = BRIDGE_IMPORT_INTERNAL_WRITER,
    .available             = claude_available,
    .supports_full_history = yes,
    .import_history        = claude_import,
    .validate_import       = claude_validate,
    .handoff_launch        = "claude %s",
};

/* --------------------------------------------------------- CommandCode --- */

static bool cc_available(void) { return bridge_binary_available("commandcode"); }

static bool cc_import(const BridgeHistory *h, BridgeImportResult *r) {
    if (!writer_preflight(h, r)) return false;

    char dir[BRIDGE_PATH_CAP];
    if (!bridge_commandcode_project_dir(h->cwd, dir, sizeof dir) || !bridge_mkdir_p(dir)) {
        snprintf(r->error, sizeof r->error, "Cannot prepare Command Code's project directory.");
        return false;
    }

    char uuid[64], path[BRIDGE_PATH_CAP];
    bool have = false;
    for (i32 attempt = 0; attempt < 3 && !have; attempt++) {
        if (!bridge_new_uuid(uuid, sizeof uuid)) break;
        int n = snprintf(path, sizeof path, "%s/%s.jsonl", dir, uuid);
        if (n <= 0 || (usize)n >= sizeof path) break;
        if (!bridge_path_exists(path)) have = true;
    }
    if (!have) {
        snprintf(r->error, sizeof r->error, "Could not allocate a free Command Code session id.");
        return false;
    }

    u32 warnings = 0;
    if (!bridge_write_commandcode_session(h, uuid, path, &warnings)) {
        snprintf(r->error, sizeof r->error, "Failed writing the Command Code session.");
        return false;
    }

    hist_copy_bounded(r->session_id,   sizeof r->session_id,   uuid);
    hist_copy_bounded(r->session_path, sizeof r->session_path, path);
    r->warnings += warnings;
    /* --session takes a PATH. Its sibling --resume matches an id or a name
     * but only inside the current directory's project bucket, so resuming
     * from a pane sitting elsewhere would drop into Command Code's own
     * picker instead — the same reasoning AGENT_CATALOG records. */
    snprintf(r->launch_cmd, sizeof r->launch_cmd, "commandcode --session \"%s\"", path);
    return true;
}

static bool cc_validate(const BridgeImportResult *r) {
    if (!file_nonempty(r->session_path)) return false;
    return jsonl_first_line_ok(r->session_path, "\"sessionId\"");
}

const BridgeTarget g_bridge_target_commandcode = {
    .tool                  = CHAT_TOOL_COMMANDCODE,
    .mode                  = BRIDGE_IMPORT_INTERNAL_WRITER,
    .available             = cc_available,
    .supports_full_history = yes,
    .import_history        = cc_import,
    .validate_import       = cc_validate,
};

/* ---------------------------------------------------------------- Qwen ---- */

static bool qwen_available(void) { return bridge_binary_available("qwen"); }

static bool qwen_import(const BridgeHistory *h, BridgeImportResult *r) {
    if (!writer_preflight(h, r)) return false;

    char dir[BRIDGE_PATH_CAP];
    if (!bridge_qwen_chats_dir(h->cwd, dir, sizeof dir) || !bridge_mkdir_p(dir)) {
        snprintf(r->error, sizeof r->error, "Cannot prepare Qwen's chats directory.");
        return false;
    }

    char id[64], path[BRIDGE_PATH_CAP];
    bool have = false;
    for (i32 attempt = 0; attempt < 3 && !have; attempt++) {
        if (!bridge_new_short_id(id, sizeof id)) break;
        int n = snprintf(path, sizeof path, "%s/liu-%s.jsonl", dir, id);
        if (n <= 0 || (usize)n >= sizeof path) break;
        if (!bridge_path_exists(path)) have = true;
    }
    if (!have) {
        snprintf(r->error, sizeof r->error, "Could not allocate a free Qwen session id.");
        return false;
    }

    u32 warnings = 0;
    if (!bridge_write_qwen_session(h, id, path, &warnings)) {
        snprintf(r->error, sizeof r->error, "Failed writing the Qwen session.");
        return false;
    }

    /* `qwen --resume` takes the chat's NAME, which is the file basename
     * without the extension — the prefix is kept so an imported session is
     * identifiable in Qwen's own picker. */
    char name[96];
    snprintf(name, sizeof name, "liu-%s", id);
    hist_copy_bounded(r->session_id,   sizeof r->session_id,   name);
    hist_copy_bounded(r->session_path, sizeof r->session_path, path);
    r->warnings += warnings;
    snprintf(r->launch_cmd, sizeof r->launch_cmd, "qwen --resume %s", name);
    return true;
}

static bool qwen_validate(const BridgeImportResult *r) {
    if (!file_nonempty(r->session_path)) return false;
    return jsonl_first_line_ok(r->session_path, "\"parts\"");
}

const BridgeTarget g_bridge_target_qwen = {
    .tool                  = CHAT_TOOL_QWEN,
    .mode                  = BRIDGE_IMPORT_INTERNAL_WRITER,
    .available             = qwen_available,
    .supports_full_history = yes,
    .import_history        = qwen_import,
    .validate_import       = qwen_validate,
};

/* ---------------------------------------------------------------- Grok ---- */

static bool grok_available(void) {
    /* `grok`, never `agent`. Grok installs BOTH names (~/.local/bin/agent can
     * be a symlink into ~/.grok/bin), and `agent` is also a name Cursor's CLI
     * answers to, so only the unambiguous name is used here. */
    return bridge_binary_available("grok");
}

static bool grok_import(const BridgeHistory *h, BridgeImportResult *r) {
    if (!writer_preflight(h, r)) return false;

    char uuid[64], dir[BRIDGE_PATH_CAP];
    bool have = false;
    for (i32 attempt = 0; attempt < 3 && !have; attempt++) {
        if (!bridge_new_uuid(uuid, sizeof uuid)) break;
        if (!bridge_grok_session_dir(h->cwd, uuid, dir, sizeof dir)) break;
        if (!bridge_path_exists(dir)) have = true;
    }
    if (!have) {
        snprintf(r->error, sizeof r->error, "Could not allocate a free Grok session id.");
        return false;
    }

    u32 warnings = 0;
    if (!bridge_write_grok_session(h, uuid, dir, &warnings)) {
        snprintf(r->error, sizeof r->error, "Failed writing the Grok session directory.");
        return false;
    }

    hist_copy_bounded(r->session_id,   sizeof r->session_id,   uuid);
    hist_copy_bounded(r->session_path, sizeof r->session_path, dir);
    r->warnings += warnings;
    snprintf(r->launch_cmd, sizeof r->launch_cmd, "grok --resume %s", uuid);
    return true;
}

static bool grok_validate(const BridgeImportResult *r) {
    /* A Grok session is a directory, so both halves have to be there: the
     * transcript AND the summary that names its cwd and session_kind. A
     * directory with only one of them lists as a broken row in Grok's own
     * picker. */
    char p[BRIDGE_PATH_CAP];
    int n = snprintf(p, sizeof p, "%s/chat_history.jsonl", r->session_path);
    if (n <= 0 || (usize)n >= sizeof p || !file_nonempty(p)) return false;
    n = snprintf(p, sizeof p, "%s/summary.json", r->session_path);
    if (n <= 0 || (usize)n >= sizeof p) return false;
    return file_nonempty(p);
}

const BridgeTarget g_bridge_target_grok = {
    .tool                  = CHAT_TOOL_XAI,
    .mode                  = BRIDGE_IMPORT_INTERNAL_WRITER,
    .available             = grok_available,
    .supports_full_history = yes,
    .import_history        = grok_import,
    .validate_import       = grok_validate,
};

/* ---------------------------------------------------------------- Kimi ---- */

static bool kimi_available(void) { return bridge_binary_available("kimi"); }

static bool kimi_import(const BridgeHistory *h, BridgeImportResult *r) {
    if (!writer_preflight(h, r)) return false;

    char uuid[64], dir[BRIDGE_PATH_CAP];
    bool have = false;
    for (i32 attempt = 0; attempt < 3 && !have; attempt++) {
        if (!bridge_new_uuid(uuid, sizeof uuid)) break;
        if (!bridge_kimi_session_dir(h->cwd, uuid, dir, sizeof dir)) break;
        if (!bridge_path_exists(dir)) have = true;
    }
    if (!have) {
        snprintf(r->error, sizeof r->error, "Could not allocate a free Kimi session id.");
        return false;
    }

    u32 warnings = 0;
    if (!bridge_write_kimi_session(h, uuid, dir, &warnings)) {
        snprintf(r->error, sizeof r->error, "Failed writing the Kimi session.");
        return false;
    }

    char path[BRIDGE_PATH_CAP];
    snprintf(path, sizeof path, "%s/context.jsonl", dir);
    hist_copy_bounded(r->session_id,   sizeof r->session_id,   uuid);
    hist_copy_bounded(r->session_path, sizeof r->session_path, path);
    r->warnings += warnings;
    snprintf(r->launch_cmd, sizeof r->launch_cmd, "kimi --session %s", uuid);
    return true;
}

static bool kimi_validate(const BridgeImportResult *r) {
    if (!file_nonempty(r->session_path)) return false;
    /* Kimi's Session.list() hides a session whose context.jsonl is empty, so
     * a zero-byte file is not merely unhelpful — it is invisible. */
    return jsonl_first_line_ok(r->session_path, "\"role\"");
}

const BridgeTarget g_bridge_target_kimi = {
    .tool                  = CHAT_TOOL_KIMI,
    .mode                  = BRIDGE_IMPORT_INTERNAL_WRITER,
    .available             = kimi_available,
    .supports_full_history = yes,
    .import_history        = kimi_import,
    .validate_import       = kimi_validate,
};

/* ------------------------------------------------------------ Cline CLI --- */

static bool cline_available(void) { return bridge_binary_available("cline"); }

static bool cline_import(const BridgeHistory *h, BridgeImportResult *r) {
    /* Cline records a cwd but does not FILE by it, so unlike the others a
     * session without one is still findable — no preflight cwd requirement. */
    r->mode = BRIDGE_IMPORT_INTERNAL_WRITER;

    char id[64], dir[BRIDGE_PATH_CAP];
    bool have = false;
    for (i32 attempt = 0; attempt < 3 && !have; attempt++) {
        if (!bridge_cline_new_id(id, sizeof id)) break;
        if (!bridge_cline_session_dir(id, dir, sizeof dir)) break;
        if (!bridge_path_exists(dir)) have = true;
    }
    if (!have) {
        snprintf(r->error, sizeof r->error, "Could not allocate a free Cline session id.");
        return false;
    }

    u32 warnings = 0;
    if (!bridge_write_cline_session(h, id, dir, &warnings)) {
        snprintf(r->error, sizeof r->error, "Failed writing the Cline session.");
        return false;
    }

    char path[BRIDGE_PATH_CAP];
    snprintf(path, sizeof path, "%s/%s.messages.json", dir, id);
    hist_copy_bounded(r->session_id,   sizeof r->session_id,   id);
    hist_copy_bounded(r->session_path, sizeof r->session_path, path);
    r->warnings += warnings;
    snprintf(r->launch_cmd, sizeof r->launch_cmd, "cline --id %s", id);
    return true;
}

static bool cline_validate(const BridgeImportResult *r) {
    if (!file_nonempty(r->session_path)) return false;
    /* The metadata sibling is what `cline --id` reads first; a transcript
     * without it resumes as an unknown session. */
    char meta[BRIDGE_PATH_CAP];
    hist_copy_bounded(meta, sizeof meta, r->session_path);
    char *dot = strstr(meta, ".messages.json");
    if (!dot) return false;
    snprintf(dot, sizeof meta - (usize)(dot - meta), ".json");
    return file_nonempty(meta);
}

const BridgeTarget g_bridge_target_cline_cli = {
    .tool                  = CHAT_TOOL_CLINE_CLI,
    .mode                  = BRIDGE_IMPORT_INTERNAL_WRITER,
    .available             = cline_available,
    .supports_full_history = yes,
    .import_history        = cline_import,
    .validate_import       = cline_validate,
};

/* ------------------------------------------------------ Pi and Oh My Pi --- */

static bool pi_available(void)  { return bridge_binary_available("pi"); }
static bool omp_available(void) { return bridge_binary_available("omp"); }

static bool pi_family_import(ChatTool tool, const BridgeHistory *h, BridgeImportResult *r) {
    const char *bin = tool == CHAT_TOOL_OMP ? "omp" : "pi";
    if (!writer_preflight(h, r)) return false;

    i64 t0 = 0;
    for (usize i = 0; i < h->event_count && t0 <= 0; i++) t0 = h->events[i].timestamp_ms;
    if (t0 <= 0) t0 = bridge_now_ms();
    char uuid[64], dir[BRIDGE_PATH_CAP], path[BRIDGE_PATH_CAP];
    bool have = false;
    for (i32 attempt = 0; attempt < 3 && !have; attempt++) {
        if (!bridge_new_uuid(uuid, sizeof uuid)) break;
        if (!bridge_pi_session_path(tool, h->cwd, uuid, t0, dir, sizeof dir, path, sizeof path)) break;
        if (!bridge_path_exists(path)) have = true;
    }
    if (!have || !bridge_mkdir_p(dir)) {
        snprintf(r->error, sizeof r->error, "Could not allocate a free %s session.",
                 chat_tool_name(tool));
        return false;
    }
    u32 warnings = 0;
    if (!bridge_write_pi_session(h, tool, uuid, path, &warnings)) {
        snprintf(r->error, sizeof r->error, "Failed writing the %s session.", chat_tool_name(tool));
        return false;
    }
    hist_copy_bounded(r->session_id,   sizeof r->session_id,   uuid);
    hist_copy_bounded(r->session_path, sizeof r->session_path, path);
    r->warnings += warnings;
    snprintf(r->launch_cmd, sizeof r->launch_cmd, "%s --session %s", bin, uuid);
    return true;
}

static bool pi_import(const BridgeHistory *h, BridgeImportResult *r) {
    return pi_family_import(CHAT_TOOL_PI, h, r);
}

static bool omp_import(const BridgeHistory *h, BridgeImportResult *r) {
    return pi_family_import(CHAT_TOOL_OMP, h, r);
}

static bool pi_validate(const BridgeImportResult *r) {
    if (!file_nonempty(r->session_path)) return false;
    /* The header is the first line (Oh My Pi's title line before it). */
    return jsonl_first_line_ok(r->session_path, "\"type\":\"session\"") ||
           jsonl_first_line_ok(r->session_path, "\"type\":\"title\"");
}

const BridgeTarget g_bridge_target_pi = {
    .tool                  = CHAT_TOOL_PI,
    .mode                  = BRIDGE_IMPORT_INTERNAL_WRITER,
    .available             = pi_available,
    .supports_full_history = yes,
    .import_history        = pi_import,
    .validate_import       = pi_validate,
    .handoff_launch        = "pi %s",
};

const BridgeTarget g_bridge_target_omp = {
    .tool                  = CHAT_TOOL_OMP,
    .mode                  = BRIDGE_IMPORT_INTERNAL_WRITER,
    .available             = omp_available,
    .supports_full_history = yes,
    .import_history        = omp_import,
    .validate_import       = pi_validate,
    .handoff_launch        = "omp %s",
};
