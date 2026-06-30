/*
 * liu-notify - user configuration.
 *
 * Lives at ~/.config/liu/notify.conf (or $XDG_CONFIG_HOME/liu/notify.conf).
 * Simple key=value file, UTF-8; '#' and blank lines ignored. All keys have
 * sensible built-in defaults, so the file is entirely optional.
 *
 * Intentional non-goals:
 *   - no JSON (keeps parser attack surface minimal)
 *   - no hot-reload (daemon re-reads only on explicit SIGHUP — not v1)
 */
#ifndef NOTIFY_CONFIG_H
#define NOTIFY_CONFIG_H

#include "core/types.h"

#define NOTIFY_SOUND_RULE_CAP 32
#define NOTIFY_SOUND_PATH_CAP 256

/* Sentinel for "match any tool / any event". */
#define NOTIFY_MATCH_ANY 0xFFu

/* Maps a (tool, event) pair to an audio file played in place of TTS. The
 * selector uses NOTIFY_MATCH_ANY wildcards; lookup picks the most specific
 * match (both exact) first, then partial, then the catch-all. */
typedef struct {
    u8   tool_id;   /* NotifyTool or NOTIFY_MATCH_ANY */
    u8   event_id;  /* NotifyEvent or NOTIFY_MATCH_ANY */
    char path[NOTIFY_SOUND_PATH_CAP];
} NotifySoundRule;

typedef struct {
    bool enabled;                       /* global kill-switch */
    char voice[64];                     /* empty = system default */
    f32  rate;                          /* 0.5..2.0 */
    bool desktop_notification;          /* legacy global default applied to
                                         * event_banner[] at load time;
                                         * Settings UI writes per-event only */
    bool tts_fallback;                  /* speak text when no sound rule matches */
    u32  rate_limit_per_sec;            /* token-bucket refill; 0 = keep default */
    u32  rate_burst;                    /* token-bucket max; 0 = keep default */
    f32  dedup_window_sec;              /* duplicate suppression window */

    /* Per-event "show macOS banner alongside the sound" toggle. Indexed by
     * NotifyEvent (start=1 .. complete=5; slot 0 reserved). Notify defaults
     * to true on a fresh install — those events are interrupt-class so a
     * banner is wanted; other events default to sound-only. */
    bool event_banner[6];

    /* Per-event master on/off. When false the daemon suppresses EVERYTHING
     * for that event — no sound, no TTS, no banner. Indexed by NotifyEvent
     * (slot 0 reserved). Defaults to true (every event notifies). This is
     * what the Settings "On/Off" pill drives. */
    bool event_enabled[6];

    NotifySoundRule sounds[NOTIFY_SOUND_RULE_CAP];
    u32             sound_count;

    struct {
        bool claude;
        bool copilot;
        bool codex;
        bool custom;
    } tool_enabled;
} NotifyConfig;

/* Fill `cfg` with built-in defaults. TTS is OFF by default. */
void notify_config_defaults(NotifyConfig *cfg);

/* Resolve a bundled sound shipped under assets/sounds/<name> to an absolute
 * path. Returns false (and clears `out`) when the file can't be located. */
bool notify_bundled_sound_path(const char *name, char *out, usize cap);

/* Seed the four bundled-wav rules (complete/notify/error/stop → any tool) so
 * the default experience plays a shipped sound rather than speaking. Existing
 * exact rules are overwritten; missing files are skipped. */
void notify_config_seed_bundled_sounds(NotifyConfig *cfg);

/* Load config from the user's config path. Missing file is not an error —
 * defaults are kept. Parse errors on a line log a warning and are skipped.
 * Returns true if a config file existed and was parsed, false otherwise. */
bool notify_config_load(NotifyConfig *cfg);

/* Write `cfg` to the user's config path. Creates parent dirs as needed.
 * Atomic via tmp + rename so a partial write can't corrupt the live file.
 * Returns true on success, false on I/O or path-resolution failure. */
bool notify_config_save(const NotifyConfig *cfg);

/* Mutators for the Settings UI. Both ignore unknown (tool, event) pairs.
 *   apply: insert or replace a rule (path != NULL && *path != 0).
 *   remove: delete a rule (matches on tool+event, no-op if absent).
 * Path resolution is up to the caller — store absolute paths. */
void notify_config_apply_sound_rule(NotifyConfig *cfg, u8 tool_id, u8 event_id,
                                    const char *path);
void notify_config_remove_sound_rule(NotifyConfig *cfg, u8 tool_id, u8 event_id);

/* True if the given tool_id is currently enabled in `cfg`. */
bool notify_config_tool_enabled(const NotifyConfig *cfg, u8 tool_id);

/* Resolve a sound file for the given (tool, event). Returns a borrowed
 * pointer into `cfg` or NULL if no rule matches. Most-specific rule wins:
 *   (tool, event) > (any, event) > (tool, any) > (any, any). */
const char *notify_config_sound_for(const NotifyConfig *cfg,
                                    u8 tool_id, u8 event_id);

#endif /* NOTIFY_CONFIG_H */
