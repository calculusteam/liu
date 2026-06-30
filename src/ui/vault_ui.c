/*
 * Liu — Smart Vault browser overlay
 *
 * A fullscreen-ish modal that lists encrypted secrets (metadata only) on
 * the left, with a detail pane on the right. The value is masked by
 * default; "Show (30s)" reveals it for a bounded window. Edit / Delete
 * actions live in the detail pane. A compact "New Entry" button opens
 * the same detail pane in create mode.
 *
 * Lifecycle:
 *   - `app_vault_browser_open` requires the vault to be unlocked.
 *   - `app_vault_browser_close` zeroes any revealed plaintext buffers.
 *   - Every touch of the list / detail / edit form bumps
 *     `vault_touch_activity` so auto-lock doesn't fire while the user
 *     is actively managing secrets.
 */
#include "ui/ui.h"
#include "ui/layout.h"
#include "ui/chrome_palette.h"
#include "platform/platform.h"
#include "vault/vault.h"
#include "vault/crypto.h"
#include "renderer/renderer.h"
#include "core/keybind.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define VB_MAX_ENTRIES 256

/* Shared (file-scope) buffer of current vault entries. Rebuilt lazily
 * whenever the browser opens or the underlying list mutates. */
static VaultSecret g_vb_entries[VB_MAX_ENTRIES];
static i32         g_vb_count = 0;

/* Reveal plaintext buffer. Held only while the user has Show enabled;
 * zeroed on toggle-off, auto-timer expiry, or overlay close. */
static u8   *g_vb_reveal_buf = NULL;
static usize g_vb_reveal_len = 0;

/* Auto-hide window in seconds for reveal. */
#define VB_REVEAL_TIMEOUT_SEC 30.0

extern void draw_text_ex(Renderer *r, const char *text, f32 x, f32 y,
                         Color fg, i32 max_chars, f32 step);
extern void app_show_toast(AppState *app, const char *message);

/* =========================================================================
 * Helpers
 * ========================================================================= */

static const char *kind_label(VaultSecretKind k) {
    switch (k) {
        case VAULT_SECRET_PASSWORD:    return "Password";
        case VAULT_SECRET_PASSPHRASE:  return "Passphrase";
        case VAULT_SECRET_PRIVATE_KEY: return "Private Key";
        case VAULT_SECRET_ENV_VAR:     return "Env Var";
        case VAULT_SECRET_NOTE:        return "Note";
    }
    return "?";
}

static bool filter_match(const VaultSecret *s, const char *q, i32 qlen) {
    if (qlen <= 0) return true;
    /* Simple substring match on label + env_name (case-insensitive). */
    const char *fields[] = { s->label, s->env_name, s->host_id, NULL };
    for (i32 i = 0; fields[i]; i++) {
        const char *f = fields[i];
        if (!f || !*f) continue;
        /* naive loop — secrets count is small, perf not a concern */
        usize flen = strlen(f);
        for (usize j = 0; j + (usize)qlen <= flen; j++) {
            bool ok = true;
            for (i32 k = 0; k < qlen; k++) {
                char a = f[j + (usize)k];
                char b = q[k];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { ok = false; break; }
            }
            if (ok) return true;
        }
    }
    return false;
}

static void reveal_clear(AppState *app) {
    if (g_vb_reveal_buf) {
        vault_secret_release(g_vb_reveal_buf, g_vb_reveal_len);
        g_vb_reveal_buf = NULL;
        g_vb_reveal_len = 0;
    }
    app->vault_browser_reveal = false;
    app->vault_browser_reveal_until_ts = 0;
}

static void rebuild_list(AppState *app) {
    if (!app || !app->vault) { g_vb_count = 0; return; }
    i32 kind = app->vault_browser_kind_filter;
    g_vb_count = vault_secret_list(app->vault, kind, NULL,
                                   g_vb_entries, VB_MAX_ENTRIES);
}

static i32 visible_index_to_entry(AppState *app, i32 vis) {
    /* Map visible-row index (after filter) to g_vb_entries index. */
    i32 seen = 0;
    for (i32 i = 0; i < g_vb_count; i++) {
        if (filter_match(&g_vb_entries[i], app->vault_browser_filter,
                         app->vault_browser_filter_len)) {
            if (seen == vis) return i;
            seen++;
        }
    }
    return -1;
}

static i32 visible_count(AppState *app) {
    i32 n = 0;
    for (i32 i = 0; i < g_vb_count; i++) {
        if (filter_match(&g_vb_entries[i], app->vault_browser_filter,
                         app->vault_browser_filter_len)) n++;
    }
    return n;
}

/* =========================================================================
 * Open / Close
 * ========================================================================= */

void app_vault_browser_open(AppState *app) {
    if (!app || !app->vault) return;
    if (!vault_is_unlocked(app->vault)) {
        app_vault_open_unlock(app, ACT_VAULT_BROWSER);
        return;
    }
    app->vault_browser_active = true;
    app->vault_browser_selected = 0;
    app->vault_browser_scroll = 0;
    app->vault_browser_kind_filter = -1;
    app->vault_browser_filter[0] = '\0';
    app->vault_browser_filter_len = 0;
    app->vault_browser_editing = false;
    app->vault_browser_edit_is_new = false;
    reveal_clear(app);
    rebuild_list(app);
    vault_touch_activity(app->vault);
}

void app_vault_browser_close(AppState *app) {
    if (!app) return;
    reveal_clear(app);
    app->vault_browser_active = false;
    app->vault_browser_editing = false;
    /* Scrub the edit form value — it may contain plaintext. */
    crypto_secure_zero(app->vault_browser_edit_value,
                       sizeof app->vault_browser_edit_value);
    crypto_secure_zero(app->vault_browser_edit_label,
                       sizeof app->vault_browser_edit_label);
    crypto_secure_zero(app->vault_browser_edit_env_name,
                       sizeof app->vault_browser_edit_env_name);
    crypto_secure_zero(app->vault_browser_edit_host_id,
                       sizeof app->vault_browser_edit_host_id);
    crypto_secure_zero(app->vault_browser_edit_secret_id,
                       sizeof app->vault_browser_edit_secret_id);
}

/* =========================================================================
 * Edit form helpers
 * ========================================================================= */

static void start_edit_new(AppState *app) {
    app->vault_browser_editing = true;
    app->vault_browser_edit_is_new = true;
    app->vault_browser_edit_kind = VAULT_SECRET_PASSWORD;
    app->vault_browser_edit_field = 0;  /* label */
    app->vault_browser_edit_label[0] = '\0';
    app->vault_browser_edit_value[0] = '\0';
    app->vault_browser_edit_env_name[0] = '\0';
    app->vault_browser_edit_host_id[0] = '\0';
    app->vault_browser_edit_secret_id[0] = '\0';
}

static void start_edit_update(AppState *app, const VaultSecret *meta) {
    app->vault_browser_editing = true;
    app->vault_browser_edit_is_new = false;
    app->vault_browser_edit_kind = meta->kind;
    app->vault_browser_edit_field = 1;   /* start in value field on update */
    snprintf(app->vault_browser_edit_label, sizeof app->vault_browser_edit_label,
             "%s", meta->label);
    app->vault_browser_edit_value[0] = '\0';
    snprintf(app->vault_browser_edit_env_name,
             sizeof app->vault_browser_edit_env_name, "%s", meta->env_name);
    snprintf(app->vault_browser_edit_host_id,
             sizeof app->vault_browser_edit_host_id, "%s", meta->host_id);
    snprintf(app->vault_browser_edit_secret_id,
             sizeof app->vault_browser_edit_secret_id, "%s", meta->id);
}

static bool submit_edit(AppState *app) {
    Vault *v = app->vault;
    if (!v) return false;
    const char *label = app->vault_browser_edit_label;
    const char *value = app->vault_browser_edit_value;
    VaultSecretKind kind = (VaultSecretKind)app->vault_browser_edit_kind;

    if (!label[0]) {
        snprintf(app->vault_browser_status, sizeof app->vault_browser_status,
                 "Error: label required");
        app->vault_browser_status_ts = platform_time_sec();
        return false;
    }
    if (!value[0]) {
        snprintf(app->vault_browser_status, sizeof app->vault_browser_status,
                 "Error: value empty");
        app->vault_browser_status_ts = platform_time_sec();
        return false;
    }

    bool ok;
    if (app->vault_browser_edit_is_new) {
        char new_id[33];
        const char *scope = (kind == VAULT_SECRET_ENV_VAR)
            ? (app->vault_browser_edit_host_id[0] ? "host" : "global") : NULL;
        ok = vault_secret_create(v, kind, label,
                                 app->vault_browser_edit_host_id,
                                 (const u8 *)value, strlen(value),
                                 app->vault_browser_edit_env_name,
                                 scope, NULL, NULL, new_id);
    } else {
        ok = vault_secret_update(v, app->vault_browser_edit_secret_id,
                                 (const u8 *)value, strlen(value));
    }
    crypto_secure_zero(app->vault_browser_edit_value,
                       sizeof app->vault_browser_edit_value);
    if (ok) {
        app->vault_browser_editing = false;
        snprintf(app->vault_browser_status, sizeof app->vault_browser_status,
                 app->vault_browser_edit_is_new ? "Created" : "Updated");
        rebuild_list(app);
    } else {
        snprintf(app->vault_browser_status, sizeof app->vault_browser_status,
                 "Error: save failed");
    }
    app->vault_browser_status_ts = platform_time_sec();
    vault_touch_activity(v);
    return ok;
}

/* =========================================================================
 * Render
 * ========================================================================= */

void app_vault_browser_render(AppState *app) {
    if (!app || !app->vault_browser_active) return;

    /* Reveal auto-hide */
    if (g_vb_reveal_buf &&
        app->vault_browser_reveal_until_ts > 0 &&
        platform_time_sec() > app->vault_browser_reveal_until_ts) {
        reveal_clear(app);
    }

    Renderer *r = &app->renderer;
    f32 dpi = app->dpi_scale;
    f32 ui_cw = 8.0f * dpi, ui_ch = 16.0f * dpi;
    renderer_set_ui_scale(r, ui_cw, ui_ch);

    const Theme *t = app->config.theme;
    if (!t) t = &THEME_DARK;

    /* Overlay: dim full frame, then a centered panel. */
    renderer_draw_rect(r, 0, 0, (f32)app->fb_width, (f32)app->fb_height,
                       (Color){0, 0, 0, 0.55f});

    f32 panel_w = 760 * dpi;
    if (panel_w > (f32)app->fb_width - 40 * dpi) panel_w = (f32)app->fb_width - 40 * dpi;
    f32 panel_h = (f32)app->fb_height * 0.82f;
    f32 px = ((f32)app->fb_width - panel_w) / 2;
    f32 py = ((f32)app->fb_height - panel_h) / 2;

    /* Border + soft drop shadow + rounded panel body. Same modal pattern as
     * the agent picker / settings overlay. */
    f32 panel_radius = 12.0f * dpi;
    Color panel_bd = t->border; panel_bd.a = fmaxf(panel_bd.a, 0.7f);
    renderer_draw_rrect(r,
        px - 1.0f * dpi, py - 1.0f * dpi,
        panel_w + 2.0f * dpi, panel_h + 2.0f * dpi,
        panel_bd,
        panel_radius + 1.0f, panel_radius + 1.0f,
        panel_radius + 1.0f, panel_radius + 1.0f,
        24.0f * dpi, 0.45f, 0.0f, 12.0f * dpi);
    renderer_draw_rrect_simple(r, px, py, panel_w, panel_h, t->tab_inactive_bg, panel_radius);

    /* Title bar — top corners follow the panel radius, bottom flat. */
    f32 title_h = 34 * dpi;
    renderer_draw_rrect(r, px, py, panel_w, title_h, t->tab_active_bg,
        panel_radius, panel_radius, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f);
    renderer_flush_rects(r);
    renderer_flush_rrects(r);

    f32 title_y = py + (title_h - ui_ch) / 2;
    draw_text_ex(r, "Smart Vault", px + 12, title_y, t->fg, 16, ui_cw);
    /* Lock indicator (top right) */
    draw_text_ex(r, "Esc to close", px + panel_w - 18 * ui_cw, title_y,
                 t->sidebar_fg, 16, ui_cw);

    /* Split: list left, detail right. */
    f32 list_w = 280 * dpi;
    f32 list_x = px;
    f32 list_y = py + title_h;
    f32 list_h = panel_h - title_h;
    f32 detail_x = px + list_w;
    f32 detail_w = panel_w - list_w;

    /* Divider */
    renderer_draw_rect(r, detail_x, list_y, 1, list_h, t->tab_bg);

    /* Filter box at top of list — rounded with accent focus ring (always
     * focused while the vault browser is open). */
    f32 filter_h = 26 * dpi;
    Color ring = t->ansi[4]; ring.a = 0.45f;
    renderer_draw_rrect_simple(r,
        list_x + 6 * dpi, list_y + 6 * dpi,
        list_w - 12 * dpi, filter_h + 4 * dpi,
        ring, 7.0f * dpi);
    renderer_draw_rrect_simple(r,
        list_x + 8 * dpi, list_y + 8 * dpi,
        list_w - 16 * dpi, filter_h,
        t->sidebar_bg, 5.0f * dpi);
    renderer_flush_rects(r);
    renderer_flush_rrects(r);
    {
        const char *prefix = "/ ";
        draw_text_ex(r, prefix, list_x + 14 * dpi,
                     list_y + 8 * dpi + (filter_h - ui_ch) / 2,
                     t->sidebar_fg, 2, ui_cw);
        draw_text_ex(r, app->vault_browser_filter,
                     list_x + 14 * dpi + 2 * ui_cw,
                     list_y + 8 * dpi + (filter_h - ui_ch) / 2,
                     t->fg, 40, ui_cw);
    }

    /* Kind filter chips */
    f32 chip_y = list_y + 8 * dpi + filter_h + 6 * dpi;
    const char *chip_names[] = {"All", "Pw", "Pp", "Key", "Env", "Note"};
    i32 chip_kinds[] = {-1, 0, 1, 2, 3, 4};
    f32 chip_x = list_x + 8 * dpi;
    f32 chip_h = ui_ch + 6 * dpi;
    for (i32 i = 0; i < 6; i++) {
        bool active = app->vault_browser_kind_filter == chip_kinds[i];
        f32 w = (2 + (f32)strlen(chip_names[i])) * ui_cw;
        renderer_draw_rrect_simple(r, chip_x, chip_y, w, chip_h,
                                   active ? t->ansi[4] : t->tab_bg,
                                   chip_h * 0.5f);
        renderer_flush_rrects(r);
        draw_text_ex(r, chip_names[i], chip_x + ui_cw,
                     chip_y + (chip_h - ui_ch) / 2,
                     active ? (Color){1,1,1,1} : t->sidebar_fg, 10, ui_cw);
        chip_x += w + 4 * dpi;
    }

    /* List rows */
    f32 rows_y0 = chip_y + chip_h + 6 * dpi;
    f32 row_h = 28 * dpi;
    i32 vis = visible_count(app);
    i32 max_rows = (i32)((list_y + list_h - rows_y0) / row_h);
    if (max_rows < 0) max_rows = 0;

    if (app->vault_browser_selected < 0) app->vault_browser_selected = 0;
    if (app->vault_browser_selected >= vis) app->vault_browser_selected = vis - 1;
    if (app->vault_browser_scroll > app->vault_browser_selected)
        app->vault_browser_scroll = app->vault_browser_selected;
    if (app->vault_browser_scroll > vis - max_rows)
        app->vault_browser_scroll = vis - max_rows;
    if (app->vault_browser_scroll < 0) app->vault_browser_scroll = 0;
    if (app->vault_browser_selected - app->vault_browser_scroll >= max_rows)
        app->vault_browser_scroll = app->vault_browser_selected - max_rows + 1;

    i32 shown_count = 0;
    i32 seen = 0;
    for (i32 i = 0; i < g_vb_count && shown_count < max_rows; i++) {
        if (!filter_match(&g_vb_entries[i], app->vault_browser_filter,
                          app->vault_browser_filter_len)) continue;
        if (seen < app->vault_browser_scroll) { seen++; continue; }
        bool selected = seen == app->vault_browser_selected;
        f32 ry = rows_y0 + (f32)shown_count * row_h;
        Color sel_clr = theme_ui_accent(t);
        if (selected) {
            renderer_draw_rrect_simple(r, list_x + 4 * dpi, ry, list_w - 8 * dpi,
                               row_h, sel_clr, 6 * dpi);
        }
        renderer_flush_rrects(r);
        Color sel_leg = chrome_legible_on(sel_clr);
        Color name_fg = selected ? sel_leg : t->fg;
        Color sub_fg  = selected ? (Color){sel_leg.r, sel_leg.g, sel_leg.b, 0.85f}
                                 : t->sidebar_fg;

        const VaultSecret *s = &g_vb_entries[i];
        draw_text_ex(r, s->label, list_x + 12 * dpi, ry + 4 * dpi, name_fg,
                     32, ui_cw);
        char sub[96];
        if (s->kind == VAULT_SECRET_ENV_VAR) {
            snprintf(sub, sizeof sub, "%s  %s",
                     kind_label(s->kind), s->env_name);
        } else {
            snprintf(sub, sizeof sub, "%s  %s",
                     kind_label(s->kind),
                     s->host_id[0] ? s->host_id : "global");
        }
        draw_text_ex(r, sub, list_x + 12 * dpi, ry + 4 * dpi + ui_ch,
                     sub_fg, 48, ui_cw * 0.75f);
        seen++;
        shown_count++;
    }

    /* "+ New" button at bottom-left */
    f32 new_btn_y = list_y + list_h - 36 * dpi;
    f32 new_btn_w = list_w - 16 * dpi;
    {
        Color new_bd = theme_ui_accent(t); new_bd.a = fmaxf(new_bd.a, 0.70f);
        renderer_draw_rrect_bordered(r, list_x + 8 * dpi, new_btn_y, new_btn_w,
                           28 * dpi, t->tab_bg, new_bd, 1.5f * dpi,
                           8 * dpi, 8 * dpi, 8 * dpi, 8 * dpi, 0, 0, 0, 0);
    }
    renderer_flush_rrects(r);
    draw_text_ex(r, "+  New Entry",
                 list_x + 8 * dpi + (new_btn_w - 12 * ui_cw) / 2,
                 new_btn_y + (28 * dpi - ui_ch) / 2,
                 t->fg, 16, ui_cw);

    /* ----- Detail pane ----- */
    f32 dx = detail_x + 16 * dpi;
    f32 dy = list_y + 16 * dpi;

    if (app->vault_browser_editing) {
        /* Edit form */
        draw_text_ex(r, app->vault_browser_edit_is_new
                           ? "New Entry" : "Edit Entry",
                     dx, dy, t->fg, 16, ui_cw);
        dy += ui_ch + 12 * dpi;

        /* Kind cycler */
        draw_text_ex(r, "Kind:", dx, dy, t->sidebar_fg, 6, ui_cw);
        renderer_draw_rrect_simple(r, dx + 8 * ui_cw, dy - 2 * dpi, 14 * ui_cw,
                           ui_ch + 6 * dpi, t->tab_bg, 6 * dpi);
        renderer_flush_rrects(r);
        draw_text_ex(r, kind_label((VaultSecretKind)app->vault_browser_edit_kind),
                     dx + 8 * ui_cw + ui_cw, dy, t->fg, 16, ui_cw);
        dy += ui_ch + 12 * dpi;

        /* Label field */
        struct { const char *lbl; char *buf; usize cap; i32 field_id; bool mask; } fields[] = {
            {"Label:",    app->vault_browser_edit_label,
             sizeof app->vault_browser_edit_label, 0, false},
            {"Value:",    app->vault_browser_edit_value,
             sizeof app->vault_browser_edit_value, 1, true},
            {"Env Name:", app->vault_browser_edit_env_name,
             sizeof app->vault_browser_edit_env_name, 2, false},
            {"Host ID:",  app->vault_browser_edit_host_id,
             sizeof app->vault_browser_edit_host_id, 3, false},
        };
        i32 nfields = 4;
        if ((VaultSecretKind)app->vault_browser_edit_kind != VAULT_SECRET_ENV_VAR) {
            /* Hide env name field when kind isn't env var */
            fields[2] = fields[3];
            nfields = 3;
        }
        for (i32 i = 0; i < nfields; i++) {
            draw_text_ex(r, fields[i].lbl, dx, dy, t->sidebar_fg, 16, ui_cw);
            f32 fx = dx + 12 * ui_cw;
            f32 fw = detail_w - 16 * ui_cw;
            if (fw > 40 * ui_cw) fw = 40 * ui_cw;
            bool _vf_focus = (app->vault_browser_edit_field == fields[i].field_id);
            Color _vf_bd = _vf_focus ? theme_ui_accent(t) : t->border;
            _vf_bd.a = fmaxf(_vf_bd.a, _vf_focus ? 0.90f : 0.45f);
            renderer_draw_rrect_bordered(r, fx, dy - 2 * dpi, fw, ui_ch + 6 * dpi,
                               t->sidebar_bg, _vf_bd, 1.5f * dpi,
                               6 * dpi, 6 * dpi, 6 * dpi, 6 * dpi, 0, 0, 0, 0);
            renderer_flush_rrects(r);
            if (fields[i].mask) {
                char masked[96];
                i32 flen = (i32)strlen(fields[i].buf);
                i32 show = flen < (i32)sizeof masked - 1 ? flen : (i32)sizeof masked - 1;
                for (i32 k = 0; k < show; k++) masked[k] = '*';
                masked[show] = '\0';
                draw_text_ex(r, masked, fx + 6 * dpi, dy,
                             t->fg, 40, ui_cw);
            } else {
                draw_text_ex(r, fields[i].buf, fx + 6 * dpi, dy,
                             t->fg, 40, ui_cw);
            }
            dy += ui_ch + 12 * dpi;
        }

        dy += 8 * dpi;
        /* Save / Cancel buttons — rounded pills; Save filled with the UI accent. */
        f32 btn_w = 10 * ui_cw;
        Color save_bg = theme_ui_accent(t);
        renderer_draw_rrect_simple(r, dx, dy, btn_w, ui_ch + 8 * dpi, save_bg, 7 * dpi);
        renderer_draw_rrect_simple(r, dx + btn_w + ui_cw, dy, btn_w, ui_ch + 8 * dpi,
                           t->tab_bg, 7 * dpi);
        renderer_flush_rrects(r);
        draw_text_ex(r, "Save (Ctrl+S)", dx + ui_cw, dy + 4 * dpi,
                     chrome_legible_on(save_bg), 16, ui_cw);
        draw_text_ex(r, "Cancel (Esc)", dx + btn_w + 2 * ui_cw,
                     dy + 4 * dpi, t->sidebar_fg, 16, ui_cw);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
        return;
    }

    /* View mode — show selected entry's metadata. */
    i32 idx = visible_index_to_entry(app, app->vault_browser_selected);
    if (idx < 0) {
        draw_text_ex(r, "No entries yet. Click + New Entry.", dx, dy,
                     t->sidebar_fg, 48, ui_cw);
        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
        return;
    }
    const VaultSecret *s = &g_vb_entries[idx];

    draw_text_ex(r, s->label, dx, dy, t->fg, 32, ui_cw);
    dy += ui_ch + 6 * dpi;
    draw_text_ex(r, kind_label(s->kind), dx, dy, t->sidebar_fg, 24, ui_cw);
    dy += ui_ch + 12 * dpi;

    /* Value row — masked unless revealed */
    draw_text_ex(r, "Value:", dx, dy, t->sidebar_fg, 6, ui_cw);
    f32 val_x = dx + 7 * ui_cw;
    f32 val_w = detail_w - 9 * ui_cw;
    renderer_draw_rrect_simple(r, val_x, dy - 2 * dpi, val_w, ui_ch + 6 * dpi,
                       t->sidebar_bg, 6 * dpi);
    renderer_flush_rrects(r);
    if (app->vault_browser_reveal && g_vb_reveal_buf) {
        draw_text_ex(r, (const char *)g_vb_reveal_buf, val_x + 6 * dpi, dy,
                     t->fg, 64, ui_cw);
    } else {
        draw_text_ex(r, "••••••••••••", val_x + 6 * dpi, dy,
                     t->sidebar_fg, 16, ui_cw);
    }
    dy += ui_ch + 12 * dpi;

    /* Metadata */
    char buf[128];
    if (s->env_name[0]) {
        snprintf(buf, sizeof buf, "$%s", s->env_name);
        draw_text_ex(r, "Env:", dx, dy, t->sidebar_fg, 6, ui_cw);
        draw_text_ex(r, buf, dx + 7 * ui_cw, dy, t->fg, 64, ui_cw);
        dy += ui_ch + 6 * dpi;
    }
    if (s->host_id[0]) {
        draw_text_ex(r, "Host:", dx, dy, t->sidebar_fg, 6, ui_cw);
        draw_text_ex(r, s->host_id, dx + 7 * ui_cw, dy, t->fg, 40, ui_cw);
        dy += ui_ch + 6 * dpi;
    }
    if (s->access_count > 0) {
        snprintf(buf, sizeof buf, "%lld uses", (long long)s->access_count);
        draw_text_ex(r, "Accessed:", dx, dy, t->sidebar_fg, 10, ui_cw);
        draw_text_ex(r, buf, dx + 11 * ui_cw, dy, t->fg, 24, ui_cw);
        dy += ui_ch + 6 * dpi;
    }

    /* Action buttons — rounded pills (Show = UI accent, Delete = destructive). */
    dy += 12 * dpi;
    f32 bw = 14 * ui_cw;
    f32 bh = ui_ch + 8 * dpi;
    const char *show_lbl = app->vault_browser_reveal ? "Hide" : "Show (30s)";
    Color show_bg = theme_ui_accent(t);
    ChromePalette _cp_vd = chrome_palette_for(t);
    Color _del_bg = _cp_vd.btn_destructive_fg; _del_bg.a = 1.0f;
    f32 show_x = dx;
    f32 copy_x = show_x + bw + ui_cw;
    f32 edit_x = copy_x + bw + ui_cw;
    f32 del_x  = edit_x + bw + ui_cw;
    renderer_draw_rrect_simple(r, show_x, dy, bw, bh, show_bg, 7 * dpi);
    renderer_draw_rrect_simple(r, copy_x, dy, bw, bh, t->tab_bg, 7 * dpi);
    renderer_draw_rrect_simple(r, edit_x, dy, bw, bh, t->tab_bg, 7 * dpi);
    renderer_draw_rrect_simple(r, del_x,  dy, bw, bh, _del_bg, 7 * dpi);
    renderer_flush_rrects(r);
    draw_text_ex(r, show_lbl, show_x + ui_cw, dy + 4 * dpi,
                 chrome_legible_on(show_bg), 16, ui_cw);
    draw_text_ex(r, "Copy", copy_x + ui_cw, dy + 4 * dpi, t->fg, 16, ui_cw);
    draw_text_ex(r, "Edit", edit_x + ui_cw, dy + 4 * dpi, t->fg, 16, ui_cw);
    draw_text_ex(r, "Delete", del_x + ui_cw, dy + 4 * dpi,
                 chrome_legible_on(_del_bg), 16, ui_cw);

    /* Status line */
    if (app->vault_browser_status[0] &&
        platform_time_sec() - app->vault_browser_status_ts < 3.0) {
        dy += bh + 12 * dpi;
        draw_text_ex(r, app->vault_browser_status, dx, dy, t->fg, 64, ui_cw);
    }

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r);
}

/* =========================================================================
 * Input
 * ========================================================================= */

static void toggle_reveal(AppState *app) {
    if (!app->vault || !vault_is_unlocked(app->vault)) return;
    if (app->vault_browser_reveal) {
        reveal_clear(app);
        return;
    }
    i32 idx = visible_index_to_entry(app, app->vault_browser_selected);
    if (idx < 0) return;
    const VaultSecret *s = &g_vb_entries[idx];
    usize pt_len = 0;
    u8 *pt = vault_secret_reveal(app->vault, s->id, &pt_len);
    if (!pt) {
        snprintf(app->vault_browser_status, sizeof app->vault_browser_status,
                 "Error: reveal failed");
        app->vault_browser_status_ts = platform_time_sec();
        return;
    }
    reveal_clear(app);
    g_vb_reveal_buf = pt;
    g_vb_reveal_len = pt_len;
    app->vault_browser_reveal = true;
    app->vault_browser_reveal_until_ts = platform_time_sec()
        + VB_REVEAL_TIMEOUT_SEC;
}

static void copy_selected(AppState *app) {
    i32 idx = visible_index_to_entry(app, app->vault_browser_selected);
    if (idx < 0) return;
    const VaultSecret *s = &g_vb_entries[idx];
    usize pt_len = 0;
    u8 *pt = vault_secret_reveal(app->vault, s->id, &pt_len);
    if (!pt) {
        snprintf(app->vault_browser_status, sizeof app->vault_browser_status,
                 "Error: copy failed");
        app->vault_browser_status_ts = platform_time_sec();
        return;
    }
    platform_clipboard_set((const char *)pt);
    /* The clipboard keeps its own copy; scrub ours immediately. */
    vault_secret_release(pt, pt_len);
    snprintf(app->vault_browser_status, sizeof app->vault_browser_status,
             "Copied to clipboard");
    app->vault_browser_status_ts = platform_time_sec();
}

static void delete_selected(AppState *app) {
    i32 idx = visible_index_to_entry(app, app->vault_browser_selected);
    if (idx < 0) return;
    const char *id = g_vb_entries[idx].id;
    if (vault_secret_delete(app->vault, id)) {
        reveal_clear(app);
        snprintf(app->vault_browser_status, sizeof app->vault_browser_status,
                 "Deleted");
        app->vault_browser_status_ts = platform_time_sec();
        rebuild_list(app);
        if (app->vault_browser_selected >= visible_count(app))
            app->vault_browser_selected = visible_count(app) - 1;
    }
}

bool app_vault_browser_handle_key(AppState *app, u32 key, u32 mods) {
    if (!app || !app->vault_browser_active) return false;

    /* Filter backspace first when in search mode (not editing a form). */
    if (!app->vault_browser_editing) {
        if (key == KEY_BACKSPACE) {
            if (app->vault_browser_filter_len > 0) {
                app->vault_browser_filter_len--;
                app->vault_browser_filter[app->vault_browser_filter_len] = '\0';
                vault_touch_activity(app->vault);
            }
            return true;
        }
        if (key == KEY_UP) {
            if (app->vault_browser_selected > 0)
                app->vault_browser_selected--;
            vault_touch_activity(app->vault);
            return true;
        }
        if (key == KEY_DOWN) {
            if (app->vault_browser_selected < visible_count(app) - 1)
                app->vault_browser_selected++;
            vault_touch_activity(app->vault);
            return true;
        }
        if (key == KEY_ENTER || key == KEY_SPACE) {
            toggle_reveal(app);
            return true;
        }
        if (key == KEY_DELETE) {
            delete_selected(app);
            return true;
        }
        if (key == KEY_TAB) {
            /* Cycle kind filter: -1 → 0 → 1 → … → 4 → -1 */
            i32 k = app->vault_browser_kind_filter;
            k = (k + 2) % 6 - 1;  /* -1..4 */
            app->vault_browser_kind_filter = k;
            app->vault_browser_selected = 0;
            app->vault_browser_scroll = 0;
            rebuild_list(app);
            vault_touch_activity(app->vault);
            return true;
        }
        /* Ctrl/Cmd+C → copy */
        if ((mods & (MOD_CTRL | MOD_SUPER)) && (key == KEY_C)) {
            copy_selected(app);
            return true;
        }
        /* Ctrl/Cmd+N → new entry */
        if ((mods & (MOD_CTRL | MOD_SUPER)) && (key == KEY_N)) {
            start_edit_new(app);
            return true;
        }
        /* Ctrl/Cmd+E → edit */
        if ((mods & (MOD_CTRL | MOD_SUPER)) && (key == KEY_E)) {
            i32 idx = visible_index_to_entry(app, app->vault_browser_selected);
            if (idx >= 0) start_edit_update(app, &g_vb_entries[idx]);
            return true;
        }
        return false;
    }

    /* Editing a form */
    if (key == KEY_BACKSPACE) {
        char *buf = NULL;
        if (app->vault_browser_edit_field == 0) buf = app->vault_browser_edit_label;
        else if (app->vault_browser_edit_field == 1) buf = app->vault_browser_edit_value;
        else if (app->vault_browser_edit_field == 2) buf = app->vault_browser_edit_env_name;
        else if (app->vault_browser_edit_field == 3) buf = app->vault_browser_edit_host_id;
        if (buf) {
            i32 len = (i32)strlen(buf);
            if (len > 0) buf[len - 1] = '\0';
            vault_touch_activity(app->vault);
        }
        return true;
    }
    if (key == KEY_TAB) {
        /* Tab must walk the *visible* field ids, not 0..maxf-1. When the kind
         * isn't an env var the env-name field (id 2) is hidden and the render
         * shows {label=0, value=1, host_id=3} (see the fields[] remap above),
         * so cycling % 3 would land on the hidden id 2 and skip Host ID (id 3)
         * entirely — making Host ID unreachable and routing its chars wrong. */
        static const i32 order_env[]  = {0, 1, 2, 3};
        static const i32 order_plain[] = {0, 1, 3};
        bool is_env = ((VaultSecretKind)app->vault_browser_edit_kind
                       == VAULT_SECRET_ENV_VAR);
        const i32 *order = is_env ? order_env : order_plain;
        i32 norder = is_env ? 4 : 3;
        i32 cur = 0;
        for (i32 i = 0; i < norder; i++) {
            if (order[i] == app->vault_browser_edit_field) { cur = i; break; }
        }
        app->vault_browser_edit_field = order[(cur + 1) % norder];
        return true;
    }
    /* Cmd/Ctrl+S → save */
    if ((mods & (MOD_CTRL | MOD_SUPER)) && key == KEY_S) {
        submit_edit(app);
        return true;
    }
    /* Ctrl/Cmd+K → cycle kind when creating */
    if (app->vault_browser_edit_is_new &&
        (mods & (MOD_CTRL | MOD_SUPER)) && key == KEY_K) {
        app->vault_browser_edit_kind = (app->vault_browser_edit_kind + 1) % 5;
        return true;
    }
    return false;
}

bool app_vault_browser_handle_char(AppState *app, u32 codepoint) {
    if (!app || !app->vault_browser_active) return false;
    if (codepoint < 32 || codepoint >= 127) return false;

    if (!app->vault_browser_editing) {
        /* Append to filter */
        if (app->vault_browser_filter_len <
            (i32)sizeof app->vault_browser_filter - 1) {
            app->vault_browser_filter[app->vault_browser_filter_len++] = (char)codepoint;
            app->vault_browser_filter[app->vault_browser_filter_len] = '\0';
            app->vault_browser_selected = 0;
            app->vault_browser_scroll = 0;
            vault_touch_activity(app->vault);
        }
        return true;
    }

    /* Editing — route into the focused field. */
    char *buf = NULL;
    usize cap = 0;
    switch (app->vault_browser_edit_field) {
    case 0: buf = app->vault_browser_edit_label;
            cap = sizeof app->vault_browser_edit_label; break;
    case 1: buf = app->vault_browser_edit_value;
            cap = sizeof app->vault_browser_edit_value; break;
    case 2: buf = app->vault_browser_edit_env_name;
            cap = sizeof app->vault_browser_edit_env_name; break;
    case 3: buf = app->vault_browser_edit_host_id;
            cap = sizeof app->vault_browser_edit_host_id; break;
    default: return false;
    }
    if (!buf) return false;
    usize len = strlen(buf);
    if (len < cap - 1) {
        buf[len] = (char)codepoint;
        buf[len + 1] = '\0';
        vault_touch_activity(app->vault);
    }
    return true;
}
