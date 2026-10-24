/*
 * Runtime workload for exercising window, tab, split-pane, session, and
 * renderer lifecycles through the application's normal code paths.
 */
#ifndef UI_APP_SOAK_H
#define UI_APP_SOAK_H

#include "core/types.h"

typedef struct AppState AppState;

/* Callbacks that expose application operations to this module. */
struct PlatformWindow;

typedef struct {
    /* Cmd+N / Cmd+W, exactly. `open` returns the new window's index or -1. */
    i32  (*window_open)(void);
    void (*window_close)(i32 index);
    i32  (*window_count)(void);
    AppState *(*window_state)(i32 index);
    /* Calls the application's idle-sleep scan so window sleep behavior
     * follows the normal policy. */
    void (*sleep_scan)(AppState *app, f64 now_sec);
    /* Invokes the font reload path used by Cmd+= and Cmd+-; the callback has
     * access to PlatformWindow. */
    void (*font_reload)(AppState *app);
    /* send_input uses app_send_input() so input updates the view position,
     * session activity, last-keystroke timestamp, PTY, and broadcast
     * targets. The translate shadow and prompt capture are separate
     * event-handler steps and are not called here. */
    void (*send_input)(AppState *app, const u8 *data, i32 len);
    /* The PlatformWindow behind a window index. app_new_browser_tab needs one
     * and app_soak.c cannot reach g_liu_wins[], which is main.c's table. */
    struct PlatformWindow *(*window_handle)(i32 index);
} AppSoakHost;

/* Returns and resets the time accumulated inside workload operations. Time
 * spent in epoch transitions and teardown is tracked separately. */
f64 app_soak_workload_seconds_take(void);

/* Returns whether LIU_SOAK is enabled. The environment value is read
 * once. */
bool app_soak_active(void);

/* Returns the scenario name, or an empty string. main.c uses it when
 * selecting the frame timeout. */
const char *app_soak_scenario(void);

/* Called once, after the first window exists. */
void app_soak_begin(const AppSoakHost *host);

/* Called once per main-loop iteration after events and session polling and
 * before rendering. Returns false when the workload has finished. */
bool app_soak_tick(void);

i32  app_soak_exit_code(void);

/* Returns whether the loop should force a frame after each operation. Active
 * scenarios request frames so renderer state is updated; idle leaves frames
 * unscheduled to preserve idle behavior. */
bool app_soak_wants_redraw(void);
/* Which window app_soak_wants_redraw() refers to; -1 = every window. */
i32 app_soak_redraw_window(void);

#endif /* UI_APP_SOAK_H */
