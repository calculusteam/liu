/*
 * Per-target version ranges for session-store adapters. Limit read/write
 * operations to formats the adapter supports; targets without a verified
 * writer remain handoff-only.
 */
#include "history/bridge/compatibility.h"
#include "history/bridge/bridge.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static const BridgeCompatibility kCompat[] = {
    /* tool                 binary        min        max        native  writer  version_arg */

    /* Native import targets use their supported APIs: Codex uses its session
     * import over app-server (thread/inject_items where a release has none);
     * OpenCode uses opencode import on 1.x or opencode session import on 2.x
     * with an export document. This table does not set maximum versions for
     * these targets. */
    { CHAT_TOOL_CODEX,       "codex",       "0.40.0",  NULL,     true,  false, "--version" },
    { CHAT_TOOL_OPENCODE,    "opencode",    "0.5.0",   NULL,     true,  false, "--version" },

    /* These targets have no import API, so writes are limited to
     * supported version ranges and refuse unknown major versions rather
     * than guessing. */
    { CHAT_TOOL_CLAUDE,      "claude",      "1.0.0",   "3.0.0",  false, true,  "--version" },
    { CHAT_TOOL_COMMANDCODE, "commandcode", "1.0.0",   "2.0.0",  false, true,  "--version" },
    { CHAT_TOOL_XAI,         "grok",        "0.5.0",   "2.0.0",  false, true,  "--version" },
    { CHAT_TOOL_KIMI,        "kimi",        "1.0.0",   "2.0.0",  false, true,  "--version" },
    { CHAT_TOOL_CLINE_CLI,   "cline",       "2.0.0",   "4.0.0",  false, true,  "--version" },

    /* Qwen stores chat messages as JSONL under ~/.qwen/projects/<slug>/chats.
     * Its writer is limited to releases whose layout the scanner supports;
     * the UI identifies the writer as unverified. */
    { CHAT_TOOL_QWEN,        "qwen",        "0.1.0",   "2.0.0",  false, true,  "--version" },

    /* Pi and Oh My Pi document their session file (docs/session-format.md),
     * checked against pi 1.3 and omp 18.11. */
    { CHAT_TOOL_PI,          "pi",          "1.0.0",   "2.0.0",  false, true,  "--version" },
    { CHAT_TOOL_OMP,         "omp",         "18.0.0",  "19.0.0", false, true,  "--version" },

    /* No verified writer. Present in the table so the UI can say WHY:
     * available, understood as a source, but nothing safe to write. */
    { CHAT_TOOL_CURSOR,      "cursor-agent", NULL,     NULL,     false, false, "--version" },
    { CHAT_TOOL_COPILOT,     "copilot",     NULL,      NULL,     false, false, "--version" },
    { CHAT_TOOL_DROID,       "droid",       NULL,      NULL,     false, false, "--version" },
    { CHAT_TOOL_KIRO,        "kiro",        NULL,      NULL,     false, false, "--version" },
    { CHAT_TOOL_AMP,         "amp",         NULL,      NULL,     false, false, "--version" },
    { CHAT_TOOL_ANTIGRAVITY, "agy",         NULL,      NULL,     false, false, "--version" },
    { CHAT_TOOL_OPENCLAW,    "openclaw",    NULL,      NULL,     false, false, "--version" },
    { CHAT_TOOL_HERMES,      "hermes",      NULL,      NULL,     false, false, "--version" },
    { CHAT_TOOL_DEVIN,       "devin",       NULL,      NULL,     false, false, "--version" },
    { CHAT_TOOL_QODER,       "qodercli",    NULL,      NULL,     false, false, "--version" },
    { CHAT_TOOL_LETTA,       "letta",       NULL,      NULL,     false, false, "--version" },
    { CHAT_TOOL_MASTRACODE,  "mastracode",  NULL,      NULL,     false, false, "--version" },
    { CHAT_TOOL_MAKI,        "maki",        NULL,      NULL,     false, false, "--version" },
    { CHAT_TOOL_MUSE,        "muse",        NULL,      NULL,     false, false, "--version" },
    { CHAT_TOOL_PRIME,       "prime-agent", NULL,      NULL,     false, false, "--version" },
};

#define COMPAT_COUNT ((usize)(sizeof kCompat / sizeof kCompat[0]))

const BridgeCompatibility *bridge_compat_for(ChatTool tool) {
    for (usize i = 0; i < COMPAT_COUNT; i++)
        if (kCompat[i].tool == tool) return &kCompat[i];
    return NULL;
}

/* ------------------------------------------------------------- versions -- */

i32 bridge_version_cmp(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    for (;;) {
        /* A missing component reads as 0, so "1.2" == "1.2.0" and
         * "1.2" < "1.2.1". */
        long va = 0, vb = 0;
        while (*a >= '0' && *a <= '9') { va = va * 10 + (*a++ - '0'); if (va > 1000000) va = 1000000; }
        while (*b >= '0' && *b <= '9') { vb = vb * 10 + (*b++ - '0'); if (vb > 1000000) vb = 1000000; }
        if (va != vb) return va < vb ? -1 : 1;

        /* A prerelease tail ENDS the version. The first version of this
         * skipped forward to the next '.' instead, which quietly turned
         * "0.149.1-rc.2" into a FOUR-component 0.149.1.2 and made it compare
         * GREATER than the release 0.149.1 — a prerelease reading as newer
         * than the thing it precedes is exactly backwards, and it would let
         * an unreleased build slip past a max-version ceiling. So: a
         * component is over at the first non-digit, and unless that character
         * is a '.' the whole version is over. */
        bool more_a = (*a == '.');
        bool more_b = (*b == '.');
        if (more_a) a++;
        if (more_b) b++;
        if (!more_a && !more_b) return 0;
        if (!more_a) a = "";   /* remaining components read as 0 */
        if (!more_b) b = "";
    }
}

bool bridge_version_extract(const char *text, char *out, usize cap) {
    if (!text || !out || cap < 2) return false;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9') continue;
        /* Only accept a token that is actually dotted: agent banners are full
         * of bare numbers ("Codex CLI 2 tools loaded") and taking the first
         * digit run would read one of those as the version. */
        const char *s = p;
        usize digits = 0, dots = 0, n = 0;
        while (p[n] && (isdigit((unsigned char)p[n]) || p[n] == '.')) {
            if (p[n] == '.') dots++; else digits++;
            n++;
        }
        if (dots >= 1 && digits >= 2 && n + 1 < cap) {
            /* Trim a trailing dot ("1.2." from "version 1.2."). */
            while (n > 0 && s[n - 1] == '.') n--;
            memcpy(out, s, n);
            out[n] = '\0';
            return true;
        }
        p += n ? n - 1 : 0;
    }
    return false;
}

/* Per-process version cache. A probe is a process spawn and the picker asks
 * about every installed agent on every open; without this, opening the
 * "Continue with…" list would fork a dozen CLIs.
 *
 * The picker never waits for a probe (bridge_compat_version_peek): a dozen
 * CLIs asked one after another on the main thread would hold the window for
 * seconds, up to the probe's timeout each for a slow one. One worker asks them
 * in turn; whoever must have the answer (the migration itself, the liu-history
 * CLI) waits for the probe in flight instead of starting a second. `g_vmu`
 * guards every field. */
typedef struct {
    ChatTool tool;
    bool     probed;
    bool     found;
    bool     in_flight;
    bool     wanted;     /* asked for by a peek, not yet probed */
    char     version[64];
} VersionCacheEntry;

static VersionCacheEntry g_vcache[COMPAT_COUNT];
static bool              g_vcache_ready;
static pthread_mutex_t   g_vmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    g_vcv = PTHREAD_COND_INITIALIZER;
static u32               g_vgen;
static bool              g_vworker_live;
static void            (*g_vwake)(void);

static VersionCacheEntry *vcache_slot(ChatTool tool) {
    if (!g_vcache_ready) {
        for (usize i = 0; i < COMPAT_COUNT; i++) g_vcache[i].tool = kCompat[i].tool;
        g_vcache_ready = true;
    }
    for (usize i = 0; i < COMPAT_COUNT; i++)
        if (g_vcache[i].tool == tool) return &g_vcache[i];
    return NULL;
}

void bridge_compat_reset_cache(void) {
    pthread_mutex_lock(&g_vmu);
    for (usize i = 0; i < COMPAT_COUNT; i++) {
        if (g_vcache[i].in_flight) continue;   /* its result lands when it is done */
        g_vcache[i].probed = false;
        g_vcache[i].found  = false;
        g_vcache[i].wanted = false;
        g_vcache[i].version[0] = '\0';
    }
    g_vgen++;
    pthread_mutex_unlock(&g_vmu);
}

/* Ask the binary. A process spawn: never under g_vmu. */
static bool compat_probe(const BridgeCompatibility *c, char *version, usize cap) {
    version[0] = '\0';
    if (!bridge_binary_available(c->binary)) return false;
    char buf[4096];
    const char *argv[] = { c->binary, c->version_arg ? c->version_arg : "--version", NULL };
    /* 10 s: a first run of a node-wrapped CLI on a cold FS cache is slow,
     * and a probe that times out reads as "not installed" — which would
     * silently demote a perfectly good target to handoff. */
    i32 rc = bridge_run_capture(argv, buf, sizeof buf, 10);
    /* Several of these print the version and exit non-zero (or print an
     * upgrade banner first), so the exit status is not the test — the
     * presence of a dotted version in the output is. */
    return rc >= 0 && bridge_version_extract(buf, version, cap);
}

/* Record a finished probe (g_vmu held). */
static void vcache_store(VersionCacheEntry *slot, bool found, const char *version) {
    slot->probed    = true;
    slot->found     = found;
    slot->in_flight = false;
    slot->wanted    = false;
    snprintf(slot->version, sizeof slot->version, "%s", found ? version : "");
    g_vgen++;
    pthread_cond_broadcast(&g_vcv);
}

static void *compat_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_vmu);
        usize pick = COMPAT_COUNT;
        for (usize i = 0; i < COMPAT_COUNT && pick == COMPAT_COUNT; i++)
            if (g_vcache[i].wanted && !g_vcache[i].probed && !g_vcache[i].in_flight) pick = i;
        if (pick == COMPAT_COUNT) {
            g_vworker_live = false;
            pthread_mutex_unlock(&g_vmu);
            return NULL;
        }
        g_vcache[pick].in_flight = true;
        pthread_mutex_unlock(&g_vmu);

        char version[64];
        bool found = compat_probe(&kCompat[pick], version, sizeof version);

        pthread_mutex_lock(&g_vmu);
        vcache_store(&g_vcache[pick], found, version);
        void (*wake)(void) = g_vwake;
        pthread_mutex_unlock(&g_vmu);
        if (wake) wake();
    }
}

void bridge_compat_set_wake(void (*wake)(void)) {
    pthread_mutex_lock(&g_vmu);
    g_vwake = wake;
    pthread_mutex_unlock(&g_vmu);
}

u32 bridge_compat_generation(void) {
    pthread_mutex_lock(&g_vmu);
    u32 gen = g_vgen;
    pthread_mutex_unlock(&g_vmu);
    return gen;
}

BridgeVersionState bridge_compat_version_peek(ChatTool tool, char *out, usize cap) {
    if (out && cap) out[0] = '\0';
    const BridgeCompatibility *c = bridge_compat_for(tool);
    if (!c || !c->binary) return BRIDGE_VERSION_MISSING;

    pthread_mutex_lock(&g_vmu);
    VersionCacheEntry *slot = vcache_slot(tool);
    if (!slot) { pthread_mutex_unlock(&g_vmu); return BRIDGE_VERSION_MISSING; }
    if (slot->probed) {
        BridgeVersionState st = slot->found ? BRIDGE_VERSION_FOUND : BRIDGE_VERSION_MISSING;
        if (slot->found && out && cap) snprintf(out, cap, "%s", slot->version);
        pthread_mutex_unlock(&g_vmu);
        return st;
    }
    slot->wanted = true;
    if (!g_vworker_live) {
        pthread_t tid;
        pthread_attr_t attr;
        g_vworker_live = pthread_attr_init(&attr) == 0;
        if (g_vworker_live) {
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            /* No worker: the probe waits for whoever must know (the
             * migration asks bridge_compat_installed_version). */
            if (pthread_create(&tid, &attr, compat_worker, NULL) != 0) g_vworker_live = false;
            pthread_attr_destroy(&attr);
        }
    }
    pthread_mutex_unlock(&g_vmu);
    return BRIDGE_VERSION_PENDING;
}

bool bridge_compat_installed_version(ChatTool tool, char *out, usize cap) {
    if (out && cap) out[0] = '\0';
    const BridgeCompatibility *c = bridge_compat_for(tool);
    if (!c || !c->binary) return false;

    pthread_mutex_lock(&g_vmu);
    VersionCacheEntry *slot = vcache_slot(tool);
    if (!slot) { pthread_mutex_unlock(&g_vmu); return false; }
    /* A probe already running is waited for, not repeated. */
    while (slot->in_flight) pthread_cond_wait(&g_vcv, &g_vmu);
    if (slot->probed) {
        bool found = slot->found;
        if (found && out && cap) snprintf(out, cap, "%s", slot->version);
        pthread_mutex_unlock(&g_vmu);
        return found;
    }
    slot->in_flight = true;
    pthread_mutex_unlock(&g_vmu);

    char version[64];
    bool found = compat_probe(c, version, sizeof version);

    pthread_mutex_lock(&g_vmu);
    vcache_store(slot, found, version);
    pthread_mutex_unlock(&g_vmu);
    if (found && out && cap) snprintf(out, cap, "%s", version);
    return found;
}

/* The version gate, for a version known (`have`) or not. */
static bool compat_gate(const BridgeCompatibility *c, bool have, const char *v) {
    if (!have) {
        /* Version unknown. For a native-import target that is still fine —
         * the agent validates the payload itself and a rejection is a clean
         * error. For a WRITER it is not: writing another app's private store
         * on a version that could not be identified is exactly the "try and
         * see" this gate exists to prevent. */
        return c->supports_native_import;
    }
    if (c->min_version && bridge_version_cmp(v, c->min_version) < 0) return false;
    /* Max bound is EXCLUSIVE: "3.0.0" means "every 2.x is fine, 3.0 is not",
     * which is the shape a major-version ceiling actually has. */
    if (c->max_version && bridge_version_cmp(v, c->max_version) >= 0) return false;
    return true;
}

bool bridge_compat_writer_ok_peek(ChatTool tool, bool *pending) {
    if (pending) *pending = false;
    const BridgeCompatibility *c = bridge_compat_for(tool);
    if (!c) return false;
    if (!c->supports_writer && !c->supports_native_import) return false;
    char v[64];
    BridgeVersionState st = bridge_compat_version_peek(tool, v, sizeof v);
    if (st == BRIDGE_VERSION_PENDING) {
        if (pending) *pending = true;
        return true;
    }
    return compat_gate(c, st == BRIDGE_VERSION_FOUND, v);
}

bool bridge_compat_writer_ok(ChatTool tool, char *out_version, usize cap) {
    if (out_version && cap) out_version[0] = '\0';
    const BridgeCompatibility *c = bridge_compat_for(tool);
    if (!c) return false;
    if (!c->supports_writer && !c->supports_native_import) return false;

    char v[64];
    bool have = bridge_compat_installed_version(tool, v, sizeof v);
    if (have && out_version && cap) snprintf(out_version, cap, "%s", v);
    return compat_gate(c, have, v);
}
