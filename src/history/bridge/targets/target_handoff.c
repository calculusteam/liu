/*
 * These adapters are listed in the target picker and create a context
 * handoff when Liu has no verified native-history write path for the target.
 *
 * Cursor transcripts contain tool calls without corresponding tool results,
 * so a migration cannot claim complete tool history. Liu also has no
 * verified session-creation interface for a hand-written Cursor transcript.
 *
 * Copilot CLI and Factory Droid store layouts are not verified for writing.
 * Kiro's store roots are not verified. Amp provides no local import file;
 * its thread option resumes a thread created by Amp.
 *
 * Antigravity stores steps as Protocol Buffers in SQLite. Without a verified
 * writer, a partial record could leave the database unreadable.
 *
 * Keep these targets in context-handoff mode until a compatible store format
 * and resume path have been verified.
 */
#include "history/bridge/targets.h"
#include "history/bridge/handoff.h"

#include <string.h>

/* Each adapter is availability + the launch format. `%s` receives the
 * shell-quoted instruction that points at the briefing document. */
#define HANDOFF_TARGET(sym, tool_enum, binary_name, cmd_format)               \
    static bool sym##_available(void) {                                       \
        return bridge_binary_available(binary_name);                          \
    }                                                                         \
    static bool sym##_full(void) {                                            \
        /* A briefing is never full history, by construction. */              \
        return false;                                                         \
    }                                                                         \
    static bool sym##_import(const BridgeHistory *h, BridgeImportResult *r) { \
        return bridge_handoff_import(h, r, cmd_format);                       \
    }                                                                         \
    const BridgeTarget g_bridge_target_##sym = {                              \
        .tool                  = tool_enum,                                   \
        .mode                  = BRIDGE_IMPORT_CONTEXT_HANDOFF,               \
        .available             = sym##_available,                             \
        .supports_full_history = sym##_full,                                  \
        .import_history        = sym##_import,                                \
        .validate_import       = bridge_handoff_validate,                     \
    }

/* `cursor-agent` is the real Cursor binary. NOT `agent`: the Grok CLI
 * installs that name too (~/.local/bin/agent can be a symlink into
 * ~/.grok/bin), so probing it finds whichever installed itself last.
 * AGENT_CATALOG's Cursor row probes the same unambiguous name, and the two
 * must stay in step. */
HANDOFF_TARGET(cursor,      CHAT_TOOL_CURSOR,      "cursor-agent", "cursor-agent %s");
HANDOFF_TARGET(copilot,     CHAT_TOOL_COPILOT,     "copilot",      "copilot -i %s");
HANDOFF_TARGET(droid,       CHAT_TOOL_DROID,       "droid",        "droid exec %s");
HANDOFF_TARGET(amp,         CHAT_TOOL_AMP,         "amp",          "amp %s");
HANDOFF_TARGET(antigravity, CHAT_TOOL_ANTIGRAVITY, "agy",          "agy -i %s");

/* Two terminal harnesses that take the opening message on their command line,
 * so a briefing can be handed over (Pi and Oh My Pi have writers of their own,
 * target_writers.c).
 *   openclaw    `openclaw chat --message <text>` runs the agent in the TUI
 *               process itself, with no Gateway to start first.
 *   hermes      `hermes chat -q <text>` submits it as the first turn and
 *               leaves the session open on a TTY. */
HANDOFF_TARGET(openclaw,    CHAT_TOOL_OPENCLAW,    "openclaw",     "openclaw chat --message %s");
HANDOFF_TARGET(hermes,      CHAT_TOOL_HERMES,      "hermes",       "hermes chat -q %s");

/* Two more that open their interface with a first message: Mastra Code's
 * `--tui-prompt <text>` sends it, Maki takes it as its positional prompt. */
HANDOFF_TARGET(mastracode,  CHAT_TOOL_MASTRACODE,  "mastracode",   "mastracode --tui-prompt %s");
HANDOFF_TARGET(maki,        CHAT_TOOL_MAKI,        "maki",         "maki %s");

/* Kiro's CLI takes no positional prompt in any released build, so there is
 * nothing to interpolate: the briefing is written and the user is told where
 * it is. A launch command that silently dropped the prompt would be worse
 * than none — it would look like a resume and start an empty session. */
static bool kiro_available(void) { return bridge_binary_available("kiro"); }
static bool kiro_full(void) { return false; }
static bool kiro_import(const BridgeHistory *h, BridgeImportResult *r) {
    return bridge_handoff_import(h, r, NULL);
}
const BridgeTarget g_bridge_target_kiro = {
    .tool                  = CHAT_TOOL_KIRO,
    .mode                  = BRIDGE_IMPORT_CONTEXT_HANDOFF,
    .available             = kiro_available,
    .supports_full_history = kiro_full,
    .import_history        = kiro_import,
    .validate_import       = bridge_handoff_validate,
};
