/*
 * liu-history/bridge - the target adapter registry.
 *
 * One extern per adapter, and nothing else. bridge.c holds the array; this
 * header exists so adding an agent is one file plus one line there, and so
 * no adapter can see any other adapter.
 */
#ifndef HISTORY_BRIDGE_TARGETS_H
#define HISTORY_BRIDGE_TARGETS_H

#include "history/bridge/bridge.h"

extern const BridgeTarget g_bridge_target_codex;        /* thread/inject_items */
extern const BridgeTarget g_bridge_target_opencode;     /* opencode import     */

/* Version-gated writers into other agents' session stores. */
extern const BridgeTarget g_bridge_target_claude;
extern const BridgeTarget g_bridge_target_commandcode;
extern const BridgeTarget g_bridge_target_qwen;
extern const BridgeTarget g_bridge_target_grok;
extern const BridgeTarget g_bridge_target_kimi;
extern const BridgeTarget g_bridge_target_cline_cli;

/* These targets offer transcript handoff; the UI labels the action as
 * handoff rather than resume. */
extern const BridgeTarget g_bridge_target_cursor;
extern const BridgeTarget g_bridge_target_copilot;
extern const BridgeTarget g_bridge_target_droid;
extern const BridgeTarget g_bridge_target_kiro;
extern const BridgeTarget g_bridge_target_amp;
extern const BridgeTarget g_bridge_target_antigravity;
extern const BridgeTarget g_bridge_target_pi;
extern const BridgeTarget g_bridge_target_omp;
extern const BridgeTarget g_bridge_target_openclaw;
extern const BridgeTarget g_bridge_target_hermes;

#endif /* HISTORY_BRIDGE_TARGETS_H */
