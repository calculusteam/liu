/*
 * Liu - a pointer event that comes from another X screen (X11)
 *
 * A display can have several X screens, each with its own root window, and the
 * pointer walks from one to the next. A window of Liu's is on ONE of them, and
 * while a button is held the server keeps sending the pointer's events to the
 * window that took the press - after the pointer has left its screen. Such an
 * event says it is not on the window's screen (the core event's same_screen,
 * an XInput2 event's root), and its coordinates are the other screen's or none:
 * acted on, a drag that crossed over would see the pointer jump to the window's
 * corner.
 *
 *   - a motion from another screen is not Liu's to act on;
 *   - a press from another screen is not either;
 *   - a release always is: the gesture has to end, and it ends where the
 *     pointer left the window.
 *
 * Pure C: no server is needed.
 */
#ifndef LIU_PLATFORM_X11_POINTER_SCREEN_H
#define LIU_PLATFORM_X11_POINTER_SCREEN_H

#include "core/types.h"

typedef enum { X11_PTR_MOTION = 0, X11_PTR_PRESS, X11_PTR_RELEASE } X11PtrKind;

/* Where the pointer last was inside Liu's screen. */
typedef struct { i32 x, y; } X11PtrMemo;

/* Whether the event is to be acted on at all, and the position it is to be
 * acted on at: the event's own when it is on Liu's screen (and remembered),
 * the last one seen there when it is not. */
static inline bool x11_pointer_event(X11PtrMemo *m, X11PtrKind kind, bool same_screen,
                                     i32 *x, i32 *y) {
    if (same_screen) {
        m->x = *x; m->y = *y;
        return true;
    }
    *x = m->x; *y = m->y;
    return kind == X11_PTR_RELEASE;
}

#endif /* LIU_PLATFORM_X11_POINTER_SCREEN_H */
