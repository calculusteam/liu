/* Liu — animation timing globals (see anim.h for the inline math). */
#include "ui/anim.h"

/* Global "animation active until" deadline. Each anim_start() bumps this
 * forward; main.c polls anim_global_active() to keep waking the event loop
 * while animations are running. Reading is racy (single thread renders) so
 * no atomic needed — the worst case is one extra frame past the deadline. */
static f64 g_anim_active_until = 0.0;

/* "Soft" animation deadline — like the above, but for continuous low-rate
 * visuals (the agent-working tab accent) that look smooth at ~30 Hz and must
 * NOT pin the loop to FRAME_DT_BURST (250 Hz / panel rate). Kept separate so a
 * background agent streaming for minutes doesn't burn full-rate frames. */
static f64 g_anim_soft_until = 0.0;

void anim_register_until(f64 end_time) {
    if (end_time > g_anim_active_until) g_anim_active_until = end_time;
}

void anim_register_soft_until(f64 end_time) {
    if (end_time > g_anim_soft_until) g_anim_soft_until = end_time;
}

void anim_start(Anim *a, f64 duration_sec) {
    if (!a) return;
    a->start_time = platform_time_sec();
    a->duration   = duration_sec > 0.0 ? duration_sec : 0.001;
    a->active     = true;
    anim_register_until(a->start_time + a->duration);
}

void anim_reset(Anim *a) {
    if (!a) return;
    a->start_time = 0.0;
    a->duration   = 0.0;
    a->active     = false;
}

bool anim_global_active(void) {
    return platform_time_sec() < g_anim_active_until;
}

bool anim_soft_active(void) {
    return platform_time_sec() < g_anim_soft_until;
}
