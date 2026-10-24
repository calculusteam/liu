/* Liu — animation timing globals (see anim.h for the inline math). */
#include "ui/anim.h"
#include <math.h>

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

/* Salt folded into every slot key — see anim_set_hover_scope(). A rect alone is
 * window-local, so without the salt two same-size windows would collide on every
 * control: the background window (painted first) would eat the frame's dt and
 * then release the slot the focused window is still fading in. 0 = never set =
 * single window. */
static u32 g_hover_scope = 0;

/* ---- WHOSE animation (see anim.h) ---------------------------------------
 *
 * One deadline pair per window, keyed by the hover scope, plus an UNSCOPED
 * pair that answers for everyone. The two deadlines above are not scoped:
 * they answer "wake the loop", which is one loop and one question.
 *
 * Eight slots because that is LIU_MAX_WINDOWS. The number is not load-bearing
 * — running out falls back to the unscoped pair — so this file does not need
 * to include main.c's cap to stay correct. */
#define ANIM_SCOPE_SLOTS 8
static struct {
    u32 scope;                /* 0 = free */
    f64 until, soft_until;
} g_anim_scopes[ANIM_SCOPE_SLOTS];
/* Registrations that could not be attributed: no scope was ever set, or every
 * slot was taken by a window still animating. Answering true for every window
 * is the DEGRADED case on purpose — an extra repaint, never a missed one. */
static f64 g_anim_unscoped_until = 0.0, g_anim_unscoped_soft = 0.0;

/* The slot for the current scope, reusing one whose window has finished. NULL
 * means "could not attribute", and the caller then widens to unscoped. */
static i32 anim_scope_slot(f64 now) {
    if (g_hover_scope == 0) return -1;
    i32 free_slot = -1;
    for (i32 i = 0; i < ANIM_SCOPE_SLOTS; i++) {
        if (g_anim_scopes[i].scope == g_hover_scope) return i;
        if (free_slot >= 0) continue;
        if (g_anim_scopes[i].scope == 0 ||
            (now >= g_anim_scopes[i].until && now >= g_anim_scopes[i].soft_until))
            free_slot = i;
    }
    if (free_slot >= 0) {
        g_anim_scopes[free_slot].scope = g_hover_scope;
        g_anim_scopes[free_slot].until = 0.0;
        g_anim_scopes[free_slot].soft_until = 0.0;
    }
    return free_slot;
}

void anim_register_until(f64 end_time) {
    if (!isfinite(end_time)) return;
    if (end_time > g_anim_active_until) g_anim_active_until = end_time;
    i32 i = anim_scope_slot(platform_time_sec());
    if (i < 0) {
        if (end_time > g_anim_unscoped_until) g_anim_unscoped_until = end_time;
    } else if (end_time > g_anim_scopes[i].until) {
        g_anim_scopes[i].until = end_time;
    }
}

void anim_register_soft_until(f64 end_time) {
    if (!isfinite(end_time)) return;
    if (end_time > g_anim_soft_until) g_anim_soft_until = end_time;
    i32 i = anim_scope_slot(platform_time_sec());
    if (i < 0) {
        if (end_time > g_anim_unscoped_soft) g_anim_unscoped_soft = end_time;
    } else if (end_time > g_anim_scopes[i].soft_until) {
        g_anim_scopes[i].soft_until = end_time;
    }
}

void anim_start(Anim *a, f64 duration_sec) {
    if (!a) return;
    a->start_time = platform_time_sec();
    a->duration   = isfinite(duration_sec) && duration_sec > 0.0
                  ? duration_sec : 0.001;
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

/* Hashed the same way anim_set_hover_scope hashes it, so the two agree without
 * either having to hold the pointer. */
static u32 anim_scope_of(const void *owner);

static bool anim_for(const void *owner, bool soft) {
    f64 now = platform_time_sec();
    if (now < (soft ? g_anim_unscoped_soft : g_anim_unscoped_until)) return true;
    u32 sc = anim_scope_of(owner);
    if (sc == 0) return false;
    for (i32 i = 0; i < ANIM_SCOPE_SLOTS; i++) {
        if (g_anim_scopes[i].scope != sc) continue;
        return now < (soft ? g_anim_scopes[i].soft_until : g_anim_scopes[i].until);
    }
    return false;
}

bool anim_active_for(const void *owner)      { return anim_for(owner, false); }
bool anim_soft_active_for(const void *owner) { return anim_for(owner, true);  }

/* Auto-hiding overlay scrollbars — see the header note in anim.h. */
f32 ui_autohide_alpha(f64 last_active, bool active_now) {
    if (active_now) return 1.0f;
    if (last_active <= 0.0) return 0.0f;   /* never stamped — stay hidden */
    f64 now   = platform_time_sec();
    f64 since = now - last_active;
    if (since < 0.0) since = 0.0;
    if (since < UI_AUTOHIDE_HOLD_S) {
        anim_register_soft_until(last_active + UI_AUTOHIDE_HOLD_S + 0.05);
        return 1.0f;
    }
    f64 t = (since - UI_AUTOHIDE_HOLD_S) / UI_AUTOHIDE_FADE_S;
    if (t >= 1.0) return 0.0f;
    anim_register_until(now + 0.05);
    /* Quadratic decay, same reasoning as the modal close in anim.h: linear
     * alpha reads as "steady, then a snap" once it crosses the visibility
     * threshold in the last couple of frames. */
    f32 inv = 1.0f - (f32)t;
    return inv * inv;
}

/* -------------------------------------------------------------------------
 * Universal hover easing (see anim.h).
 *
 * A tiny fixed pool of slots, each tracking one element's eased hover value.
 * Slots are keyed by a hash of the element's on-screen rect, so the same
 * button keeps its slot frame-to-frame while it stays put. Only elements the
 * pointer has actually touched (amt > 0) hold a slot; everything else returns
 * 0 with no allocation. At most a couple of elements are ever mid-fade at
 * once (the one being left + the one being entered), so 32 slots is generous.
 */
#define HOVER_SLOTS   32
/* Asymmetric on purpose. Hover-in is a response and wants to feel immediate;
 * hover-out is cleanup and wants to be gone before the eye follows the
 * pointer. Equal timings made a fast sweep across a row leave three or four
 * controls visibly lit behind the cursor, which read as lag rather than as
 * feedback. */
#define HOVER_FADE_IN_S   0.085  /* seconds for a full 0→1 ramp */
#define HOVER_FADE_OUT_S  0.055  /* seconds for a full 1→0 ramp */

static struct {
    u32 id;
    f32 amt;
    f64 t;
    bool used;
} g_hover[HOVER_SLOTS];


/* ---- Pointer-damage registry (see anim.h) --------------------------------
 *
 * One entry per control ui_hover_amount() was asked about during the last
 * paint of that control's window, holding the rect and whether the pointer was
 * inside it AT THAT PAINT. Sized well past any real frame: the busiest surface
 * in the tree (Settings' Appearance tab, every theme card and its delete
 * button) asks about a few dozen. Overflow is not a correctness problem — the
 * registry reports itself full and the test answers "damaged", i.e. the loop
 * falls back to repainting on every motion. */
#define HOVER_REG_MAX 512

typedef struct {
    f32  x, y, w, h;
    u32  scope;
    bool hovered;
} HoverRect;

static HoverRect g_hover_reg[HOVER_REG_MAX];
static i32       g_hover_reg_n   = 0;
static bool      g_hover_reg_full = false;

/* Entries are appended by whichever window is painting, so the array holds a
 * mix of scopes. A frame begin drops just this scope's rows and compacts the
 * rest down — O(n) once per window per paint over an array that is a few
 * hundred entries at worst. */
void ui_hover_frame_begin(void) {
    i32 out = 0;
    for (i32 i = 0; i < g_hover_reg_n; i++) {
        if (g_hover_reg[i].scope == g_hover_scope) continue;
        if (out != i) g_hover_reg[out] = g_hover_reg[i];
        out++;
    }
    g_hover_reg_n = out;
    g_hover_reg_full = false;
}

static bool g_hover_reg_paused = false;

void ui_hover_record_pause(bool paused) { g_hover_reg_paused = paused; }

static void hover_reg_record(f32 x, f32 y, f32 w, f32 h, bool hovered) {
    if (g_hover_reg_paused) return;
    if (g_hover_reg_n >= HOVER_REG_MAX) { g_hover_reg_full = true; return; }
    HoverRect *e = &g_hover_reg[g_hover_reg_n++];
    e->x = x; e->y = y; e->w = w; e->h = h;
    e->scope = g_hover_scope;
    e->hovered = hovered;
}

bool ui_hover_hit(f32 x, f32 y, f32 w, f32 h, f32 px, f32 py) {
    bool hovered = (px >= x && px < x + w && py >= y && py < y + h);
    hover_reg_record(x, y, w, h, hovered);
    return hovered;
}

bool ui_hover_pointer_damage(f32 px, f32 py) {
    if (g_hover_reg_full) return true;
    i32 seen = 0;
    for (i32 i = 0; i < g_hover_reg_n; i++) {
        const HoverRect *e = &g_hover_reg[i];
        if (e->scope != g_hover_scope) continue;
        seen++;
        bool now = (px >= e->x && px < e->x + e->w &&
                    py >= e->y && py < e->y + e->h);
        if (now != e->hovered) return true;
    }
    /* Nothing recorded for this window yet — it has not painted since the
     * scope was set, so nothing is known. Repaint. */
    return seen == 0;
}

/* Pointer identity only — never dereferenced. splitmix64's finaliser, so two
 * AppStates a few KB apart don't land in adjacent key space. NULL keeps the
 * 0 sentinel ("never set"), which both the slot pool and the animation
 * attribution read as "single window / cannot attribute". */
static u32 anim_scope_of(const void *owner) {
    if (!owner) return 0;
    u64 v = (u64)(usize)owner;
    v ^= v >> 33; v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33; v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    u32 h = (u32)v;
    return h ? h : 1u;               /* 0 is the free/never-set sentinel */
}

void anim_set_hover_scope(const void *owner) {
    g_hover_scope = anim_scope_of(owner);
}

static u32 hover_id(f32 x, f32 y, f32 w, f32 h) {
    /* Quantise to whole pixels; fold w/h in lightly so stacked same-origin
     * controls don't collide. Rounding keeps sub-pixel jitter from churning
     * the key each frame. */
    u32 ix = (u32)(x + 0.5f);
    u32 iy = (u32)(y + 0.5f);
    u32 iw = (u32)(w + 0.5f);
    u32 ih = (u32)(h + 0.5f);
    u32 id = (ix * 73856093u) ^ (iy * 19349663u)
           ^ (iw * 83492791u) ^ (ih * 2971215073u)
           ^ g_hover_scope;
    return id ? id : 1u;   /* 0 is the "free" sentinel */
}

f32 ui_hover_amount(f32 x, f32 y, f32 w, f32 h, f32 px, f32 py) {
    bool hovered = (px >= x && px < x + w && py >= y && py < y + h);
    u32 id = hover_id(x, y, w, h);
    f64 now = platform_time_sec();

    /* Record BEFORE the un-hovered early-out below: "the pointer just entered
     * a control that has no slot yet" is exactly the transition the pointer
     * gate has to catch, and that control returns from that early-out. */
    hover_reg_record(x, y, w, h, hovered);

    /* Locate this element's slot; remember a free/evictable one in passing. */
    i32 slot = -1, freei = -1, weakest = 0;
    for (i32 i = 0; i < HOVER_SLOTS; i++) {
        if (g_hover[i].used && g_hover[i].id == id) { slot = i; break; }
        if (!g_hover[i].used && freei < 0) freei = i;
        if (g_hover[i].amt < g_hover[weakest].amt) weakest = i;
    }
    if (slot < 0) {
        if (!hovered) return 0.0f;          /* nothing to animate — skip alloc */
        slot = (freei >= 0) ? freei : weakest;
        g_hover[slot].id  = id;
        g_hover[slot].amt = 0.0f;
        g_hover[slot].t   = now;
        g_hover[slot].used = true;
    }

    f64 dt = now - g_hover[slot].t;
    g_hover[slot].t = now;
    if (dt < 0.0) dt = 0.0; else if (dt > 0.1) dt = 0.1;   /* clamp long gaps */
    f32 target = hovered ? 1.0f : 0.0f;
    f32 step = (f32)(dt / (hovered ? HOVER_FADE_IN_S : HOVER_FADE_OUT_S));
    if (g_hover[slot].amt < target) {
        g_hover[slot].amt += step;
        if (g_hover[slot].amt > target) g_hover[slot].amt = target;
    } else if (g_hover[slot].amt > target) {
        g_hover[slot].amt -= step;
        if (g_hover[slot].amt < target) g_hover[slot].amt = target;
    }

    f32 v = g_hover[slot].amt;
    if (v != target) {
        /* Still moving — keep the loop awake so the fade plays through even if
         * the pointer holds still. Full rate: hover is brief and wants to feel
         * crisp.
         *
         * The test is "has not arrived yet", NOT "is strictly between 0 and 1",
         * and the difference is one frame at each END of the fade — the two
         * frames that matter most. A slot is born with amt = 0 on the frame the
         * pointer enters, and that frame's dt is 0 (it was just stamped), so a
         * `0 < v < 1` test would register NOTHING on it, and the fade-in would
         * need a second frame that nothing asked for. The same hole sits at v
         * == 1 when the pointer leaves. Pointer motion that cannot change
         * anything paints no frame (ui_hover_pointer_damage), so no other frame
         * is coming. */
        anim_register_until(now + 0.05);
    }
    if (!hovered && v <= 0.0008f) {
        g_hover[slot].used = false;         /* fully faded out — release */
        g_hover[slot].id   = 0;
        return 0.0f;
    }
    return v * v * (3.0f - 2.0f * v);        /* smoothstep for a softer curve */
}
