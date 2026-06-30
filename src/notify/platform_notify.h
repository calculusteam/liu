/*
 * liu-notify - platform TTS interface.
 * Kept separate from src/platform/platform.h so the daemon binary does not pull
 * the main app's Cocoa window / event loop / renderer code.
 */
#ifndef NOTIFY_PLATFORM_NOTIFY_H
#define NOTIFY_PLATFORM_NOTIFY_H

#include "core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the speech subsystem.
 * Returns true on success, false if unavailable (caller may proceed anyway). */
bool tts_init(void);

/* Release synthesizer, cancel any pending utterance. Idempotent. */
void tts_shutdown(void);

/* Enqueue an utterance. Non-blocking: returns as soon as the utterance is handed
 * to the platform speech engine.
 *   text  : UTF-8, validated by caller, NUL-terminated.
 *   voice : NULL = default, or a BCP-47 language tag ("en-US", "tr-TR") or an
 *           AVSpeechSynthesisVoice identifier (macOS). Ignored on stub platforms.
 *   rate  : 0.5..2.0 ; 1.0 = platform default. Clamped internally. */
void tts_speak(const char *text, const char *voice, f32 rate);

/* True while the synthesizer is actively producing audio. */
bool tts_busy(void);

/* Immediately abort any in-flight + queued utterances. */
void tts_cancel(void);

/* Show a native desktop notification (banner / toast) alongside TTS.
 * No-op on platforms without a sensible implementation in v1. */
void platform_notify_desktop(const char *title, const char *body);

/* Play an audio file (mp3/wav/aiff/caf/m4a on macOS; wav/ogg on Linux via
 * paplay/aplay; wav/mp3 on Win32). Returns true when playback was scheduled
 * successfully, false on a missing file, unsupported format, or platform
 * stub. Non-blocking — returns immediately, audio plays asynchronously. */
bool notify_play_sound(const char *path);

/* True while an audio file queued via notify_play_sound is still playing. */
bool notify_sound_busy(void);

/* Cancel any in-flight sound playback. Idempotent. */
void notify_sound_cancel(void);

/* True when the user is actively focused on a Liu window — daemon uses
 * this to drop notifications whose target is already looking at the
 * source. macOS queries NSWorkspace.frontmostApplication; non-macOS
 * stubs return false so other platforms still notify unconditionally. */
bool notify_target_active(void);

#ifdef __cplusplus
}
#endif

#endif /* NOTIFY_PLATFORM_NOTIFY_H */
