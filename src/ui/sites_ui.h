/*
 * Liu - Sites (dev-server manager) panel — UI layer.
 *
 * Full-screen overlay (Cmd+Shift+S): a site list on the left and the selected
 * site's live log on the right. Drives the backend runtime in AppState.site_mgr
 * and renders each site's headless log Terminal via render_terminal_pane().
 */
#ifndef UI_SITES_UI_H
#define UI_SITES_UI_H

#include "core/types.h"

typedef struct AppState AppState;

/* Reset the panel's view state. Call once at startup. */
void sites_ui_init(AppState *app);

/* Render the overlay (gated internally by its modal animation). */
void render_sites_panel(AppState *app);

/* Input — absolute framebuffer coords. Return true if the event was consumed. */
bool sites_handle_click(AppState *app, f32 mx, f32 my);
bool sites_handle_key(AppState *app, u32 key, u32 mods);
bool sites_handle_char(AppState *app, u32 codepoint);
void sites_handle_scroll(AppState *app, f32 dy, bool precise, f32 mx, f32 my);
void sites_handle_mouse_move(AppState *app, f32 mx, f32 my);

#endif /* UI_SITES_UI_H */
