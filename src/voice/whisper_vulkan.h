/*
 * Vulkan compute backend for Whisper on Linux and Windows systems with
 * NVIDIA, AMD, or Intel GPUs.
 *
 * The engine dispatches through the same interface as the Metal backend.
 * Vulkan is loaded at runtime with dlopen and its entry points are resolved
 * dynamically, so the binary has no libvulkan link dependency and falls back
 * when a loader, device, or compute queue is unavailable. Compiled only when
 * LIU_WHISPER_VULKAN is defined.
 */
#ifndef VOICE_WHISPER_VULKAN_H
#define VOICE_WHISPER_VULKAN_H

#include "core/types.h"
#include "voice/whisper_model.h"

typedef struct WhisperVulkan WhisperVulkan;

/* True if a Vulkan loader, a physical device and a compute queue are all
 * present. Cheap enough to call before deciding to try. */
bool whisper_vulkan_available(void);

/* Build a Vulkan backend over a resolved weight table. The weights are copied
 * into device buffers here, so `w` need not outlive the call. Returns NULL on
 * any failure with a reason in `err`. */
WhisperVulkan *whisper_vulkan_create(const WhisperWeights *w, char *err, usize err_cap);
void           whisper_vulkan_free(WhisperVulkan *v);

/* Same contract as the CPU and Metal backends. */
bool whisper_vulkan_encode(WhisperVulkan *v, const f32 *mel);
bool whisper_vulkan_decode(WhisperVulkan *v, const i32 *tokens, i32 n, i32 n_past,
                           f32 *logits);
void whisper_vulkan_decode_reset(WhisperVulkan *v);

/* Human-readable device name, for the "which backend am I on" readout. */
const char *whisper_vulkan_device_name(const WhisperVulkan *v);

#endif /* VOICE_WHISPER_VULKAN_H */
