#include "ui/browser_ui.h"
#include "ui/ui.h"
#include "ui/pane_zoom.h"
#include "ui/chrome_palette.h"
#include "ui/layout.h"
#include "ui/icon.h"
#include "browser/browser.h"
#include "browser/browser_nav.h"
#include "browser/browser_viewport.h"
#include "browser/browser_device.h"
#include "platform/platform.h"
#include "core/utf8.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

extern void draw_text_ex(Renderer *r, const char *text, f32 x, f32 y,
                         Color fg, i32 max_chars, f32 step);

bool browser_any_metal_overlay(const AppState *app) {
    if (!app) return true;
    if (app->palette_active) return true;
    if (app->search_active) return true;
    if (app->settings.open) return true;
    if (app->sketch.open || !anim_done(&app->sketch.close_anim)) return true;
    if (app->sites.active) return true;
    if (app->key_manager_active) return true;
    if (app->keygen_form_active) return true;
    if (app->cmd_history_active) return true;
    if (app->transcript_viewer_active) return true;
    if (app->agent_picker_active) return true;
    if (app->passphrase_dialog_active) return true;
    if (app->kbi_dialog_active) return true;
    if (app->hostkey_dialog_active) return true;
    if (app->known_hosts_open) return true;
    if (app->port_forward_dialog_active) return true;
    if (app->broadcast_overlay_active) return true;
    if (app->ssh_dialog_active) return true;
    if (app->create_theme_active) return true;
    if (app->vault_unlock_dialog_active) return true;
    if (app->vault_browser_active) return true;
    if (app->close_confirm_active) return true;
    if (app->paste_confirm_active) return true;
    if (app->tab_ctx_menu_active || app->term_ctx_menu_active ||
        app->group_ctx_menu_active || app->fb_ctx_menu_active) return true;
    if (app->tab_rename_active || app->chip_rename_active) return true;
    if (app->drag_over_active) return true;
    /* Three more Metal overlays the page covered: the image lightbox (a
     * full-window scrim, opened from a badge in the pane beside a browser
     * split), the explorer's Rename / New / Delete prompt (centred — wholly
     * under a full-tab page, and its typing went into the address bar), and
     * the sidebar's Customize popover (it opens over the content area). */
    if (app->lightbox.open || app->fb_prompt_active || app->sbc_open) return true;
    /* Pane layout drag: the drop-zone highlight and the ghost of the dragged
     * pane are Metal chrome drawn into the pane area. With the web view on
     * screen the browser pane is the one place that feedback would vanish —
     * both when the browser pane is the one being dragged and when it is the
     * drop target. */
    if (app->pane_drag_active) return true;
    /* The viewport picker is Metal chrome drawn into the page area, and the
     * WKWebView is a native subview sitting ABOVE the Metal layer
     * (addSubview:positioned:NSWindowAbove). Without listing the popover here
     * it renders correctly and is then completely covered by the web view —
     * the button lights up and nothing else appears to happen. Every other
     * Liu overlay is in this list for exactly the same reason. */
    {
        Tab *vt = app_active_tab((AppState *)app);
        if (vt && vt->browser && vt->browser->viewport_menu_open) return true;
    }
    return false;
}

bool browser_should_show(const AppState *app) {
    if (!app || !app->config.browser_enabled) return false;
    Tab *t = app_active_tab((AppState *)app);
    if (!t || !t->browser) return false;
    /* Full-tab browser or terminal tab with embedded browser pane. */
    if (t->kind != TAB_BROWSER && t->browser_pane < 0) return false;
    /* Another pane of the tab is zoomed over the browser's (ui/pane_zoom.h). */
    if (t->kind != TAB_BROWSER && tab_zoom_hides_pane(t, t->browser_pane)) return false;
    if (browser_any_metal_overlay(app)) return false;
    return true;
}

/* Shared scale + chrome height for a pane size (px). Keeps WebView page
 * rect and Metal chrome strip in lockstep. */
static void browser_chrome_scale(f32 w_px, f32 h_px, f32 dpi,
                                 f32 *out_scale, f32 *out_chrome_h) {
    f32 w_pt = w_px / dpi;
    f32 h_pt = h_px / dpi;
    f32 scale = 1.0f;
    if (w_pt < BROWSER_BP_FULL_PT)   scale = 0.96f;
    if (w_pt < BROWSER_BP_MED_PT)    scale = 0.90f;
    if (w_pt < BROWSER_BP_NARROW_PT) scale = 0.84f;
    if (w_pt < BROWSER_BP_TINY_PT)   scale = 0.78f;
    /* Very short pane: shrink chrome further so page still gets room. */
    if (h_pt < 120.0f) scale = fminf(scale, 0.80f);
    if (h_pt < 80.0f)  scale = fminf(scale, 0.72f);

    f32 ch = BROWSER_CHROME_H_PT * scale * dpi;
    f32 max_ch = h_px * 0.38f;
    if (ch > max_ch && max_ch > 18.0f * dpi) ch = max_ch;
    if (ch < 24.0f * dpi) ch = fminf(24.0f * dpi, h_px * 0.45f);
    if (out_scale) *out_scale = scale;
    if (out_chrome_h) *out_chrome_h = ch;
}

void browser_content_rect(const AppState *app, BrowserContentRect *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!app) return;

    f32 dpi = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    f32 ox = app->sidebar_width + app->padding;
    f32 oy = app->tab_bar_height +
             app->config.style.terminal_top_gap * dpi;
    f32 total_w = (f32)app->fb_width - ox - app->padding;
    /* Same content box render_terminal uses (see TERM_GRID_MIN_VPAD_PT). */
    f32 total_h = (f32)app->fb_height - oy - app->status_bar_height -
                  TERM_GRID_MIN_VPAD_PT * dpi;
    if (total_w < 1.0f) total_w = 1.0f;
    if (total_h < 1.0f) total_h = 1.0f;

    /* Split-embedded browser: use that pane's rect. */
    Tab *t = app_active_tab((AppState *)app);
    if (t && t->browser && t->browser_pane >= 0 && t->kind == TAB_TERMINAL) {
        f32 px, py, pw, ph;
        if (app_tab_pane_rect((AppState *)app, t, t->browser_pane,
                              &px, &py, &pw, &ph) && pw > 1.0f && ph > 1.0f) {
            ox = px; oy = py; total_w = pw; total_h = ph;
        }
    }

    out->x_px = ox;
    out->y_px = oy;
    out->w_px = total_w;
    out->h_px = total_h;
    f32 scale = 1.0f;
    browser_chrome_scale(total_w, total_h, dpi, &scale, &out->chrome_h_px);
    out->page_y_px = oy + out->chrome_h_px;
    out->page_h_px = total_h - out->chrome_h_px;
    if (out->page_h_px < 1.0f) out->page_h_px = 1.0f;

    f32 view_h_pt = (f32)app->fb_height / dpi;
    out->x_pt = ox / dpi;
    out->w_pt = total_w / dpi;
    out->h_pt = total_h / dpi;
    out->y_pt = view_h_pt - (oy + total_h) / dpi;

    out->page_x_pt = out->x_pt;
    out->page_w_pt = out->w_pt;
    out->page_h_pt = out->page_h_px / dpi;
    out->page_y_pt = view_h_pt - (out->page_y_px + out->page_h_px) / dpi;
}

/* Progressive disclosure: hide secondary actions so URL never collapses
 * under icons. Priority drop order: attach → ext → pick → forward. */
static void fill_chrome_layout(BrowserTab *b, f32 ox, f32 oy, f32 w, f32 h,
                               f32 dpi) {
    memset(&b->layout, 0, sizeof b->layout);
    f32 scale = 1.0f, ch = 0.0f;
    browser_chrome_scale(w, h, dpi, &scale, &ch);

    f32 pad   = fmaxf(2.0f * dpi, BROWSER_PAD_PT * scale * dpi);
    f32 gap   = fmaxf(0.5f * dpi, BROWSER_BTN_GAP_PT * scale * dpi);
    f32 ugap  = fmaxf(3.0f * dpi, BROWSER_URL_GAP_PT * scale * dpi);
    f32 bs    = BROWSER_BTN_SIZE_PT * scale * dpi;
    f32 uh    = BROWSER_URL_H_PT * scale * dpi;
    if (uh > ch - 4.0f * dpi) uh = ch - 4.0f * dpi;
    if (bs > ch - 4.0f * dpi) bs = ch - 4.0f * dpi;
    if (bs < 14.0f * dpi) bs = 14.0f * dpi;
    if (uh < 12.0f * dpi) uh = 12.0f * dpi;

    /* Glyph is independent of hit-box — keep icons slim (~12pt). */
    f32 icon_s = BROWSER_ICON_SIZE_PT * scale * dpi;
    if (icon_s > bs - 2.0f * dpi) icon_s = bs - 2.0f * dpi;
    if (icon_s < 9.0f * dpi) icon_s = 9.0f * dpi;

    f32 w_pt = w / dpi;
    bool show_back   = true;
    bool show_reload = true;
    bool show_url    = w_pt >= 72.0f;
    bool show_fwd    = w_pt >= BROWSER_BP_TINY_PT;
    bool show_device = w_pt >= BROWSER_BP_NARROW_PT;
    bool show_pick   = w_pt >= BROWSER_BP_NARROW_PT;
    bool show_ext    = w_pt >= BROWSER_BP_NARROW_PT;
    bool show_attach = w_pt >= BROWSER_BP_MED_PT;
    bool show_close  = true;   /* the close button is never dropped */

    /* Recompute after visibility so URL gets remaining space. */
    f32 min_url = (show_url ? 48.0f : 0.0f) * dpi;
    for (i32 pass = 0; pass < 4; pass++) {
        i32 n_left = (show_back ? 1 : 0) + (show_fwd ? 1 : 0) + (show_reload ? 1 : 0);
        i32 n_right = (show_device ? 1 : 0) + (show_pick ? 1 : 0)
                    + (show_attach ? 1 : 0) + (show_ext ? 1 : 0)
                    + (show_close ? 1 : 0);
        f32 left_w  = (f32)n_left * bs + (n_left > 0 ? (f32)(n_left - 1) * gap : 0.0f);
        f32 right_w = (f32)n_right * bs + (n_right > 0 ? (f32)(n_right - 1) * gap : 0.0f);
        f32 gaps    = (n_left > 0 && show_url ? ugap : 0.0f)
                    + (n_right > 0 && show_url ? ugap : 0.0f)
                    + (n_left > 0 && n_right > 0 && !show_url ? ugap : 0.0f);
        f32 url_w = w - 2.0f * pad - left_w - right_w - gaps;
        if (!show_url) url_w = 0.0f;
        if (url_w >= min_url || !show_url) break;
        /* Drop least-critical action until URL fits. */
        if (show_attach)      { show_attach = false; continue; }
        if (show_ext)         { show_ext = false; continue; }
        if (show_device)      { show_device = false; continue; }
        if (show_pick)        { show_pick = false; continue; }
        if (show_fwd)         { show_fwd = false; continue; }
        break;
    }

    f32 by = oy + (ch - bs) * 0.5f;
    f32 uy = oy + (ch - uh) * 0.5f;
    f32 x  = ox + pad;

    b->layout.chrome_h = ch;
    b->layout.btn_y = by;
    b->layout.btn_s = bs;
    b->layout.icon_s = icon_s;
    b->layout.pad = pad;
    b->layout.scale = scale;
    b->layout.url_h = uh;
    b->layout.url_y = uy;
    b->layout.show_back = show_back;
    b->layout.show_fwd = show_fwd;
    b->layout.show_reload = show_reload;
    b->layout.show_url = show_url;
    b->layout.show_device = show_device;
    b->layout.show_pick = show_pick;
    b->layout.show_attach = show_attach;
    b->layout.show_ext = show_ext;
    b->layout.show_close = show_close;

    b->layout.back_x = -1.0f;
    b->layout.fwd_x = -1.0f;
    b->layout.reload_x = -1.0f;
    b->layout.device_x = -1.0f;
    b->layout.pick_x = -1.0f;
    b->layout.attach_x = -1.0f;
    b->layout.ext_x = -1.0f;
    b->layout.close_x = -1.0f;
    b->layout.url_x = ox + pad;
    b->layout.url_w = 0.0f;

    if (show_back)   { b->layout.back_x = x;   x += bs + gap; }
    if (show_fwd)    { b->layout.fwd_x = x;    x += bs + gap; }
    if (show_reload) { b->layout.reload_x = x; x += bs + gap; }
    if ((show_back || show_fwd || show_reload) && show_url)
        x += ugap - gap; /* reload already added gap; replace with url gap */

    f32 right = ox + w - pad;
    b->layout.sep_x = -1.0f;
    b->layout.show_sep = false;
    if (show_close)  {
        right -= bs; b->layout.close_x = right;
        /* Close is dismissive — give it its own space and a hairline so it
         * cannot be hit by aiming at the action next to it. */
        if (show_ext || show_attach || show_pick || show_device) {
            right -= gap;
            b->layout.sep_x = right + gap * 0.5f;
            b->layout.show_sep = true;
            right -= gap;
        } else {
            right -= gap;
        }
    }
    if (show_ext)    { right -= bs; b->layout.ext_x = right; right -= gap; }
    if (show_attach) { right -= bs; b->layout.attach_x = right; right -= gap; }
    if (show_pick)   { right -= bs; b->layout.pick_x = right; right -= gap; }
    if (show_device) { right -= bs; b->layout.device_x = right; right -= gap; }
    if ((show_close || show_ext || show_attach || show_pick || show_device) && show_url)
        right -= (ugap - gap);

    if (show_url) {
        b->layout.url_x = x;
        b->layout.url_w = right - x;
        if (b->layout.url_w < 24.0f * dpi) b->layout.url_w = 24.0f * dpi;
    }
}

/* Compact URL label for narrow fields: strip scheme, ellipsize tail. */
static void browser_format_url_label(const char *url, char *out, usize cap,
                                     i32 max_chars) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (max_chars < 3) max_chars = 3;
    if (!url || !url[0]) {
        snprintf(out, cap, "localhost…");
        return;
    }
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;
    i32 len = (i32)strlen(p);
    if (len <= max_chars) {
        snprintf(out, cap, "%s", p);
        return;
    }
    /* Keep host head + ellipsis. */
    i32 keep = max_chars - 1;
    if (keep < 2) keep = 2;
    if ((usize)keep + 2 > cap) keep = (i32)cap - 2;
    snprintf(out, cap, "%.*s…", keep, p);
}

/* Printable-codepoint count over the first `bytes` bytes. Used to measure the
 * host portion of a URL so the path can be dimmed from the right column. */
static i32 browser_text_cells_n(const char *s, i32 bytes) {
    i32 n = 0, off = 0;
    const u8 *p = (const u8 *)s;
    while (off < bytes && p[off]) {
        u32 cp; u32 c = utf8_decode(p + off, 4, &cp);
        if (c == 0) { off++; continue; }
        off += (i32)c;
        if (cp >= 32) n++;
    }
    return n;
}

/* Printable-codepoint count (cp >= 32) — matches draw_text_ex's advance so the
 * caret column stays glued to the glyph run. */
static i32 browser_text_cells(const char *s) {
    i32 n = 0;
    const u8 *p = (const u8 *)s;
    while (*p) {
        u32 cp; u32 c = utf8_decode(p, 4, &cp);
        if (c == 0) { p++; continue; }
        if (cp >= 32) n++;
        p += c;
    }
    return n;
}


/* One chrome button.
 *
 * Every clickable control goes through ui_hover_amount() — the single hover
 * engine (ui/anim.h) — so the browser bar eases exactly like the rest of Liu
 * instead of hand-rolling its own timing. `danger` tints the hover and glyph
 * red for the close button: it is the one dismissive action in the row and
 * should not look identical to the four beside it.
 *
 * The background rrect is flushed before the glyph, because a geometrically
 * drawn icon (the fallback when an SF symbol is missing) is flat rects, and
 * flat rects flush UNDER rrects — without the flush the fill would swallow
 * its own icon. */
static void draw_browser_btn(AppState *app, Renderer *r, f32 x, f32 y, f32 s,
                             f32 icon_s, IconKind kind, Color fg,
                             Color accent, const ChromePalette *cp,
                             bool active, bool danger) {
    if (x < 0.0f) return;
    f32 rad = s * 0.32f;
    if (rad < 3.0f) rad = 3.0f;

    f32 hov = ui_hover_amount(x, y, s, s, app->hover_x, app->hover_y);
    Color icon_c = fg;

    /* Three states, each painted the way the rest of Liu paints it:
     *   engaged  → a faint ACCENT wash, the app-wide "this control is on"
     *              chip idiom (settings segments, agent chips);
     *   hover    → a neutral 0.12 foreground wash, the same weight
     *              draw_toolbar_button_bg gives the toolbar icons;
     *   close    → the palette's destructive tint, so it stays legible on
     *              light themes. */
    Color fill = fg;
    fill.a = 0.0f;
    if (danger) {
        fill   = cp->btn_destructive_bg;
        fill.a = cp->btn_destructive_bg.a * hov;
    } else if (active) {
        fill   = accent;
        fill.a = 0.18f + 0.08f * hov;
    } else if (hov > 0.001f) {
        fill.a = 0.12f * hov;
    }
    if (fill.a > 0.004f) {
        renderer_draw_rrect_simple(r, x, y, s, s, fill, rad);
        renderer_flush_rrects(r);
    }
    if (danger && hov > 0.001f) {
        icon_c = color_lerp(icon_c, cp->btn_destructive_fg, hov);
    } else if (hov > 0.001f) {
        /* Idle glyphs sit at 0.88 alpha; hover brings them fully up so the
         * feedback reads even where the fill is barely visible. */
        icon_c.a = fminf(1.0f, icon_c.a + 0.12f * hov);
    }

    f32 ix = x + (s - icon_s) * 0.5f;
    f32 iy = y + (s - icon_s) * 0.5f;
    icon_draw(r, kind, ix, iy, icon_s, icon_c);
}

/* The device around a preset's viewport, laid out in a page area given in
 * pixels; the layout itself is in points (y down), the unit the web view is
 * placed in. False for Responsive. */
static bool device_in_page(const BrowserTab *b, f32 x_px, f32 y_px,
                           f32 w_px, f32 h_px, f32 dpi,
                           BrowserDeviceLayout *out) {
    if (!b || b->viewport_idx == BVP_RESPONSIVE || !(dpi > 0.0f)) {
        if (out) memset(out, 0, sizeof *out);
        return false;
    }
    return browser_device_layout(b->viewport_idx, b->viewport_landscape,
                                 x_px / dpi, y_px / dpi, w_px / dpi, h_px / dpi,
                                 out);
}

bool browser_device_now(const AppState *app, BrowserDeviceLayout *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!app || !out) return false;
    const Tab *tab = app_active_tab((AppState *)app);
    if (!tab || !tab->browser) return false;
    if (tab->kind != TAB_BROWSER && tab->browser_pane < 0) return false;
    f32 dpi = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    BrowserContentRect cr;
    browser_content_rect(app, &cr);
    return device_in_page(tab->browser, cr.x_px, cr.page_y_px, cr.w_px,
                          cr.page_h_px, dpi, out);
}

static Color dev_rgb(u32 rgb) {
    return COLOR_RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

static bool dev_is_light(Color c) {
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b > 0.5f;
}

static void dev_shape(Renderer *r, const BrowserDeviceShape *s, f32 dpi,
                      Color fill, Color edge, f32 edge_w,
                      f32 shadow, f32 shadow_a, f32 shadow_dy) {
    if (!(s->w > 0.0f) || !(s->h > 0.0f)) return;
    renderer_draw_rrect_bordered(r, s->x * dpi, s->y * dpi, s->w * dpi, s->h * dpi,
                                 fill, edge, edge_w,
                                 s->r[BDEV_TL] * dpi, s->r[BDEV_TR] * dpi,
                                 s->r[BDEV_BR] * dpi, s->r[BDEV_BL] * dpi,
                                 shadow, shadow_a, 0.0f, shadow_dy);
}

/* Cellular bars, rising left to right. */
static void dev_signal(Renderer *r, const BrowserDeviceShape *s, f32 dpi, Color ink) {
    f32 x = s->x * dpi, y = s->y * dpi, w = s->w * dpi, h = s->h * dpi;
    f32 gap = w * 0.12f, bw = (w - 3.0f * gap) / 4.0f;
    for (i32 i = 0; i < 4; i++) {
        f32 bh = h * (0.40f + 0.20f * (f32)i);
        f32 rr = bw * 0.30f;
        renderer_draw_rrect(r, x + (f32)i * (bw + gap), y + h - bh, bw, bh, ink,
                            rr, rr, rr, rr, 0.0f, 0.0f, 0.0f, 0.0f);
    }
}

/* Wi-Fi: a dot and two arcs opening upward from it. */
static void dev_wifi(Renderer *r, const BrowserDeviceShape *s, f32 dpi, Color ink) {
    f32 w = s->w * dpi, h = s->h * dpi;
    f32 cx = s->x * dpi + w * 0.5f, cy = s->y * dpi + h * 0.92f;
    f32 t = fmaxf(1.0f, h * 0.17f);
    f32 dot = h * 0.30f;
    renderer_draw_rrect(r, cx - dot * 0.5f, cy - dot, dot, dot, ink,
                        dot * 0.5f, dot * 0.5f, dot * 0.5f, dot * 0.5f,
                        0.0f, 0.0f, 0.0f, 0.0f);
    renderer_flush_rrects(r);
    const f32 radii[2] = { h * 0.56f, h * 0.90f };
    for (i32 a = 0; a < 2; a++) {
        f32 rad = radii[a] - t * 0.5f;
        const i32 n = 8;
        for (i32 i = 0; i < n; i++) {
            f32 a0 = -2.36f + 1.57f * (f32)i / (f32)n;
            f32 a1 = -2.36f + 1.57f * (f32)(i + 1) / (f32)n;
            renderer_draw_line(r, cx + rad * cosf(a0), cy + rad * sinf(a0),
                               cx + rad * cosf(a1), cy + rad * sinf(a1), t, ink);
        }
    }
    renderer_flush_lines(r);
}

/* Battery. Lying (iOS): a faint outline 25 x 13 of a 27.3 x 13 box, radius
 * 4.3, its charge 2 in from it, radius 2.5, and a faint terminal 1.33 wide one
 * point to its right. Upright (Android): a solid body with its terminal on
 * top. */
static void dev_battery(Renderer *r, const BrowserDeviceShape *s, f32 dpi, Color ink,
                        bool upright) {
    f32 x = s->x * dpi, y = s->y * dpi, w = s->w * dpi, h = s->h * dpi;
    Color faint = ink; faint.a *= 0.35f;
    Color tip = ink; tip.a *= 0.40f;
    Color none = ink; none.a = 0.0f;
    if (upright) {
        f32 cap_h = h * 0.10f, cap_w = w * 0.45f;
        f32 br = w * 0.22f;
        renderer_draw_rrect(r, x + (w - cap_w) * 0.5f, y, cap_w, cap_h + br, ink,
                            cap_w * 0.2f, cap_w * 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        renderer_draw_rrect(r, x, y + cap_h, w, h - cap_h, ink,
                            br, br, br, br, 0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    f32 u = h / 13.0f;
    f32 bw = w * (25.0f / 27.3f), cap_w = w * (1.33f / 27.3f), cap_h = 4.0f * u;
    f32 line = fmaxf(1.0f, u);
    renderer_draw_rrect_bordered(r, x, y, bw, h, none, faint, line,
                                 4.3f * u, 4.3f * u, 4.3f * u, 4.3f * u, 0.0f, 0.0f, 0.0f, 0.0f);
    f32 in = 2.0f * u, cr = 2.5f * u;
    renderer_draw_rrect(r, x + in, y + in, bw - 2.0f * in, h - 2.0f * in, ink,
                        cr, cr, cr, cr, 0.0f, 0.0f, 0.0f, 0.0f);
    renderer_draw_rrect(r, x + w - cap_w, y + (h - cap_h) * 0.5f, cap_w, cap_h, tip,
                        0.0f, cap_w, cap_w, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
}

/* The time in the terminal's font, for a platform without a system-font
 * rasteriser: its capitals as tall as the box allows. */
static void dev_clock_fallback(Renderer *r, const BrowserDeviceLayout *d, f32 dpi, Color ink) {
    const BrowserDeviceShape *s = &d->clock;
    const char *text = d->clock_text ? d->clock_text : "9:41";
    i32 n = (i32)strlen(text);
    const FontAtlas *f = &r->font;
    if (n <= 0 || !(f->cell_width > 0.0f) || !(f->cell_height > 0.0f) || !(f->cap_height > 0.0f))
        return;
    f32 bw = s->w * dpi, bh = s->h * dpi;
    f32 cw = bh * 0.72f * f->cell_width / f->cap_height;
    if (cw * (f32)n > bw) cw = bw / (f32)n;
    if (cw < 2.0f) return;
    f32 ch = roundf(cw * f->cell_height / f->cell_width);
    renderer_set_ui_scale(r, cw, ch);
    f32 x = s->x * dpi + (d->clock_left ? 0.0f : (bw - (f32)n * cw) * 0.5f);
    f32 y = renderer_ui_cap_center_y(r, s->y * dpi, bh);
    draw_text_ex(r, text, x, y, ink, n, cw);
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r);
}

static u64 dev_mask_gen(const BrowserMaskSpec *s) {
    u64 h = 1469598103934665603ull;
#define DEV_MIX(v) (h = (h ^ (u64)(v)) * 1099511628211ull)
    DEV_MIX(s->kind);
    DEV_MIX((u32)s->w_px);
    DEV_MIX((u32)s->h_px);
    DEV_MIX((u32)(s->size_px * 64.0f));
    DEV_MIX((u32)(s->weight * 1000.0f + 1000.0f));
    for (const char *c = s->text; c && *c; c++) DEV_MIX((u8)*c);
#undef DEV_MIX
    return h | 1u;
}

/* One status bar mark as the platform rasterises it (a system-font line, a
 * system symbol, a fan or a wedge), tinted `ink`, its top-left on whole
 * pixels. `key` names the texture it is kept in. */
static void dev_mask_draw(Renderer *r, const void *key, const BrowserMaskSpec *spec,
                          const BrowserMask *m, f32 x, f32 y, Color ink) {
    renderer_draw_image_tinted(r, key, dev_mask_gen(spec), m->pixels, m->w, m->h,
                               floorf(x + 0.5f), floorf(y + 0.5f), (f32)m->w, (f32)m->h, ink);
}

/* A glyph (system symbol, fan or wedge) filling `s`. */
static bool dev_glyph(Renderer *r, const void *key, u8 kind, const char *name,
                      const BrowserDeviceShape *s, f32 dpi, Color ink) {
    BrowserMaskSpec spec = { kind, name, 0.0f, 0.30f,
                             (i32)lroundf(s->w * dpi), (i32)lroundf(s->h * dpi) };
    BrowserMask m;
    if (spec.w_px < 1 || spec.h_px < 1 || !browser_native_mask(&spec, &m)) return false;
    dev_mask_draw(r, key, &spec, &m, s->x * dpi + (s->w * dpi - (f32)m.w) * 0.5f,
                  s->y * dpi + (s->h * dpi - (f32)m.h) * 0.5f, ink);
    return true;
}

/* A line of text in the system font, its capitals centred on the box's
 * middle: from the box's left edge, centred in it, or ending at its right. */
enum { DEV_TEXT_LEFT, DEV_TEXT_CENTRE, DEV_TEXT_RIGHT };
static bool dev_text(Renderer *r, const void *key, const char *text, f32 size_px,
                     f32 weight, const BrowserDeviceShape *s, i32 align, f32 dpi, Color ink) {
    BrowserMaskSpec spec = { BROWSER_MASK_TEXT, text, size_px, weight, 0, 0 };
    BrowserMask m;
    if (!(size_px >= 4.0f) || !browser_native_mask(&spec, &m)) return false;
    /* The mask keeps a pixel of room on either side of the line. */
    f32 x = align == DEV_TEXT_LEFT  ? s->x * dpi - 1.0f
          : align == DEV_TEXT_RIGHT ? (s->x + s->w) * dpi - (f32)m.w + 1.0f
          : (s->x + s->w * 0.5f) * dpi - (f32)m.w * 0.5f;
    f32 y = (s->y + s->h * 0.5f) * dpi - (m.baseline - m.cap * 0.5f);
    dev_mask_draw(r, key, &spec, &m, x, y, ink);
    return true;
}

/* The textures the status bar's marks are kept in, one each. */
static const char k_dev_clock, k_dev_percent, k_dev_wifi, k_dev_signal;

static void dev_circle(Renderer *r, f32 cx, f32 cy, f32 d, Color c) {
    f32 h = d * 0.5f;
    renderer_draw_rrect(r, cx - h, cy - h, d, d, c, h, h, h, h, 0.0f, 0.0f, 0.0f, 0.0f);
}

/* What shows through the cutout: the front camera's lens and, in a notch,
 * the earpiece over it. Placed in the cutout's upright frame (u across, v
 * down) and turned with it; the glint stays at the top-left, where the light
 * is. */
static void dev_cutout_detail(Renderer *r, const BrowserDeviceLayout *d, f32 dpi) {
    const BrowserDeviceShape *c = &d->cutout;
    if (!(c->w > 0.0f) || !(c->h > 0.0f)) return;
    f32 x = c->x * dpi, y = c->y * dpi, w = c->w * dpi, h = c->h * dpi;
    bool turned = d->cut != BDEV_CUT_HOLE && h > w;
    f32 uw = turned ? h : w, uh = turned ? w : h;
#define DEV_AT(u, v, ox, oy) do { \
        if (turned) { ox = x + (v) * w; oy = y + (1.0f - (u)) * h; } \
        else        { ox = x + (u) * w; oy = y + (v) * h; } } while (0)
    f32 lu = 0.5f, lv = 0.5f, ld = 0.56f * fminf(uw, uh);
    Color ring = COLOR_RGB(0x0c, 0x0f, 0x18), core = COLOR_RGB(0x17, 0x20, 0x36);
    f32 glint = 0.30f;
    if (d->cut == BDEV_CUT_ISLAND) {
        lu = 1.0f - uh * 0.5f / uw;
        ld = 0.40f * uh;
        ring = COLOR_RGB(0x07, 0x08, 0x0d);
        core = COLOR_RGB(0x0f, 0x14, 0x22);
        glint = 0.16f;
    } else if (d->cut == BDEV_CUT_NOTCH) {
        lu = 0.74f;
        ld = 0.34f * uh;
        f32 len = 0.30f * uw, th = fmaxf(1.0f, 0.10f * uh), sx, sy;
        DEV_AT(0.5f, 0.20f, sx, sy);
        f32 sw = turned ? th : len, sh = turned ? len : th, sr = th * 0.5f;
        renderer_draw_rrect(r, sx - sw * 0.5f, sy - sh * 0.5f, sw, sh, COLOR_RGB(0x1d, 0x1e, 0x23),
                            sr, sr, sr, sr, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    if (ld < 1.5f) return;
    f32 cx, cy;
    DEV_AT(lu, lv, cx, cy);
#undef DEV_AT
    dev_circle(r, cx, cy, ld, ring);
    dev_circle(r, cx, cy, ld * 0.56f, core);
    Color g = COLOR_RGB(255, 255, 255);
    g.a = glint;
    dev_circle(r, cx - ld * 0.13f, cy - ld * 0.13f, fmaxf(1.0f, ld * 0.18f), g);
}

/* The system's strips of the screen: painted in the page's colour as a
 * phone's browser paints them, the cutout in them, the clock, icons and home
 * indicator in whichever of black or white reads on it. The clock and the
 * glyphs are the platform's own (the system font and symbols on macOS); where
 * it has none they are drawn here. */
static void draw_device_system(Renderer *r, const BrowserDeviceLayout *d, f32 dpi,
                               bool tint_known, u32 tint) {
    const Color black = COLOR_RGB(0, 0, 0), white = COLOR_RGB(255, 255, 255);
    Color fill = tint_known ? dev_rgb(tint) : black;
    Color ink = dev_is_light(fill) ? black : white;
    Color none = black; none.a = 0.0f;
    dev_shape(r, &d->screen, dpi, fill, none, 0.0f, 0.0f, 0.0f, 0.0f);
    dev_shape(r, &d->cutout, dpi, black, none, 0.0f, 0.0f, 0.0f, 0.0f);
    dev_cutout_detail(r, d, dpi);
    dev_shape(r, &d->indicator, dpi, ink, none, 0.0f, 0.0f, 0.0f, 0.0f);
    if (d->bar.w <= 0.0f) {
        renderer_flush_rrects(r);
        return;
    }
    bool ios = d->sys == BDEV_SYS_IOS;
    if (d->battery.w > 0.0f) dev_battery(r, &d->battery, dpi, ink, !ios);
    renderer_flush_rrects(r);
    if (d->signal.w > 0.0f &&
        !dev_glyph(r, &k_dev_signal, ios ? BROWSER_MASK_SYMBOL : BROWSER_MASK_WEDGE,
                   ios ? "cellularbars" : NULL, &d->signal, dpi, ink)) {
        dev_signal(r, &d->signal, dpi, ink);
        renderer_flush_rrects(r);
    }
    if (d->wifi.w > 0.0f &&
        !dev_glyph(r, &k_dev_wifi, ios ? BROWSER_MASK_SYMBOL : BROWSER_MASK_FAN,
                   ios ? "wifi" : NULL, &d->wifi, dpi, ink))
        dev_wifi(r, &d->wifi, dpi, ink);
    if (d->clock.w > 0.0f &&
        !dev_text(r, &k_dev_clock, d->clock_text, d->clock_pt * dpi, d->clock_weight,
                  &d->clock, d->clock_left ? DEV_TEXT_LEFT : DEV_TEXT_CENTRE, dpi, ink))
        dev_clock_fallback(r, d, dpi, ink);
    if (d->percent.w > 0.0f)
        (void)dev_text(r, &k_dev_percent, "100%", d->percent_pt * dpi, 0.30f,
                       &d->percent, DEV_TEXT_RIGHT, dpi, ink);
}

/* The textures the painted body is kept in, one per band. */
static const char k_dev_art[BROWSER_ART_PARTS];

/* The device's shadow on the pane: a wide, faint one and a tight, darker one
 * where it stands closest. No fill, so only the shadow is laid down; the body
 * painted over it covers the rest. */
static void dev_shadow(Renderer *r, const BrowserDeviceShape *s, f32 dpi, f32 k, f32 weight) {
    if (!(s->w > 0.0f) || !(s->h > 0.0f)) return;
    Color none = COLOR_RGB(0, 0, 0);
    none.a = 0.0f;
    f32 x = s->x * dpi + 1.0f, y = s->y * dpi + 1.0f;
    f32 w = s->w * dpi - 2.0f, h = s->h * dpi - 2.0f;
    f32 rr[4];
    for (i32 i = 0; i < 4; i++) rr[i] = fmaxf(0.0f, s->r[i] * dpi - 1.0f);
    renderer_draw_rrect(r, x, y, w, h, none, rr[BDEV_TL], rr[BDEV_TR], rr[BDEV_BR], rr[BDEV_BL],
                        fmaxf(8.0f * dpi, 36.0f * k * dpi), 0.42f * weight,
                        0.0f, fmaxf(3.0f * dpi, 14.0f * k * dpi));
    renderer_draw_rrect(r, x, y, w, h, none, rr[BDEV_TL], rr[BDEV_TR], rr[BDEV_BR], rr[BDEV_BL],
                        fmaxf(2.0f * dpi, 6.0f * k * dpi), 0.50f * weight,
                        0.0f, fmaxf(1.0f, 2.0f * k * dpi));
}

/* The body as the platform paints it (browser_native_device_art), over its
 * shadow, and the screen's strips on it. False where there is no painter. */
static bool draw_device_painted(Renderer *r, const BrowserDeviceLayout *d, f32 dpi,
                                bool tint_known, u32 tint) {
    BrowserDeviceArt art;
    if (!browser_native_device_art(d, dpi, &art)) return false;
    dev_shadow(r, &d->body, dpi, d->scale, 1.0f);
    if (d->kind == BDEV_LAPTOP || d->kind == BDEV_MONITOR)
        dev_shadow(r, &d->base, dpi, d->scale, 0.8f);
    renderer_flush_rrects(r);
    for (i32 i = 0; i < art.n; i++) {
        const BrowserArtPart *p = &art.part[i];
        renderer_draw_image_cached(r, &k_dev_art[i], art.generation, p->pixels, p->w, p->h,
                                   (f32)p->x, (f32)p->y, (f32)p->w, (f32)p->h);
    }
    draw_device_system(r, d, dpi, tint_known, tint);
    renderer_flush_rrects(r);
    return true;
}

/* The device's body around the screen. The page itself is the native web
 * view over the page rect (rounded by browser_native_set_screen_shape); the
 * strips around it are draw_device_system's. Colours are the hardware's, not
 * the theme's, with an edge that keeps the outline readable on dark and light
 * themes alike. Where the platform paints the body, that is drawn; the flat
 * shapes below are the rest's. */
static void draw_browser_device(Renderer *r, const BrowserDeviceLayout *d, f32 dpi,
                                bool tint_known, u32 tint) {
    if (draw_device_painted(r, d, dpi, tint_known, tint)) return;
    const Color white = COLOR_RGB(255, 255, 255), black = COLOR_RGB(0, 0, 0);
    Color frame = dev_rgb(d->frame_rgb);
    Color glass = dev_rgb(d->glass_rgb);
    Color base  = dev_rgb(d->base_rgb);
    f32 k = d->scale;
    f32 hair = fmaxf(1.0f, 0.6f * k * dpi);
    f32 shadow = fmaxf(6.0f * dpi, 26.0f * k * dpi);
    f32 drop = fmaxf(2.0f * dpi, 8.0f * k * dpi);
    Color frame_edge = dev_is_light(frame) ? color_lerp(frame, black, 0.24f)
                                           : color_lerp(frame, white, 0.30f);
    Color mark = dev_is_light(glass) ? color_lerp(glass, black, 0.22f)
                                     : color_lerp(glass, white, 0.16f);
    Color none = black; none.a = 0.0f;

    /* What it stands on, behind it: the head hangs in front of its neck. */
    dev_shape(r, &d->neck, dpi, color_lerp(base, black, 0.14f), none, 0.0f,
              0.0f, 0.0f, 0.0f);
    if (d->kind == BDEV_MONITOR)
        dev_shape(r, &d->base, dpi, base, color_lerp(base, black, 0.22f), hair,
                  shadow * 0.6f, 0.30f, drop * 0.5f);

    dev_shape(r, &d->body, dpi, frame, frame_edge, hair, shadow, 0.40f, drop);
    dev_shape(r, &d->glass, dpi, glass, color_lerp(glass, white, 0.10f), hair,
              0.0f, 0.0f, 0.0f);
    renderer_flush_rrects(r);
    draw_device_system(r, d, dpi, tint_known, tint);

    for (i32 i = 0; i < d->n_keys; i++)
        dev_shape(r, &d->keys[i], dpi, color_lerp(frame, black, 0.10f), none, 0.0f,
                  0.0f, 0.0f, 0.0f);

    dev_shape(r, &d->speaker, dpi, mark, none, 0.0f, 0.0f, 0.0f, 0.0f);
    dev_shape(r, &d->hinge, dpi, COLOR_RGB(0x26, 0x26, 0x26), none, 0.0f, 0.0f, 0.0f, 0.0f);
    for (i32 i = 0; i < 2; i++)
        dev_shape(r, &d->knuckles[i], dpi, COLOR_RGB(0x8a, 0x8a, 0x86),
                  COLOR_RGB(0x3a, 0x3a, 0x38), hair * 0.5f, 0.0f, 0.0f, 0.0f);
    dev_shape(r, &d->flash, dpi, COLOR_RGB(0xe6, 0xe2, 0xd0), COLOR_RGB(0x2a, 0x2a, 0x28),
              fmaxf(1.0f, 1.2f * k * dpi), 0.0f, 0.0f, 0.0f);
    if (d->lens.w > 0.0f) {
        dev_shape(r, &d->lens, dpi, COLOR_RGB(0x1b, 0x1f, 0x2a),
                  color_lerp(mark, black, 0.2f), hair * 0.5f, 0.0f, 0.0f, 0.0f);
        BrowserDeviceShape glint = d->lens;
        glint.w = glint.h = d->lens.w * 0.28f;
        glint.x += d->lens.w * 0.24f;
        glint.y += d->lens.h * 0.24f;
        for (i32 i = 0; i < 4; i++) glint.r[i] = glint.w * 0.5f;
        Color g = white; g.a = 0.28f;
        dev_shape(r, &glint, dpi, g, none, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    dev_shape(r, &d->home, dpi, glass, mark, fmaxf(1.0f, 1.6f * k * dpi),
              0.0f, 0.0f, 0.0f);

    if (d->kind == BDEV_LAPTOP && d->base.w > 0.0f) {
        dev_shape(r, &d->base, dpi, base, color_lerp(base, black, 0.20f), hair,
                  shadow * 0.7f, 0.35f, drop * 0.6f);
        dev_shape(r, &d->scoop, dpi, color_lerp(base, black, 0.16f), none, 0.0f,
                  0.0f, 0.0f, 0.0f);
    }
    renderer_flush_rrects(r);
}

void browser_ui_render(AppState *app, f32 ox, f32 oy, f32 w, f32 h) {
    if (!app) return;
    Tab *tab = app_active_tab(app);
    if (!tab || !tab->browser) return;
    if (tab->kind != TAB_BROWSER && tab->browser_pane < 0) return;
    BrowserTab *b = tab->browser;
    Renderer *r = &app->renderer;
    const Theme *t = app->config.theme ? app->config.theme : &THEME_Liu;
    ChromePalette cp = chrome_palette_for(t);
    f32 dpi = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;

    /* Window opacity, not a hardcoded 1: only the chrome strip and the margins
     * around a device preset are painted here (the page itself is an opaque
     * WKWebView), and an opaque fill there would switch the backdrop blur off
     * for as long as a browser tab is in front. */
    Color bg = t->bg; bg.a = app->config.opacity;
    renderer_draw_rect(r, ox, oy, w, h, bg);
    renderer_flush_rects(r);

    fill_chrome_layout(b, ox, oy, w, h, dpi);
    f32 ch = b->layout.chrome_h;
    f32 bs = b->layout.btn_s;
    f32 by = b->layout.btn_y;
    f32 icon_s = b->layout.icon_s;

    /* Chrome surface: a floating rounded toolbar, not a full-bleed slab.
     *
     * Inset and given the panel treatment (neutral surface + subtle border, no
     * shadow: it is attached to the page, not floating over it), the bar
     * matches the app's rounded panels, fields, tab pills and menus, and the
     * theme background showing around it separates bar from page more quietly
     * than a rule across the full width.
     *
     * The inset is symmetric top/bottom, so the button row — centred on `ch`
     * by fill_chrome_layout — stays centred on the island too, and every hit
     * box in `b->layout` keeps the geometry it was computed with. */
    Color fg = t->fg;
    f32 isl_inset = fmaxf(2.0f * dpi, 3.0f * b->layout.scale * dpi);
    if (isl_inset > ch * 0.16f) isl_inset = ch * 0.16f;
    f32 isl_x = ox + isl_inset;
    f32 isl_y = oy + isl_inset;
    f32 isl_w = w - 2.0f * isl_inset;
    f32 isl_h = ch - 2.0f * isl_inset;
    /* Clamped against BOTH axes. Height alone is not enough: a pathological
     * nested split can make the island wider than it is tall in the other
     * direction, and a radius past w/2 degenerates the rounded-rect SDF. */
    f32 isl_r = fminf(fminf(isl_h * 0.34f, isl_w * 0.5f),
                      12.0f * b->layout.scale * dpi);
    if (isl_r < 0.0f) isl_r = 0.0f;
    if (isl_w > 2.0f && isl_h > 2.0f) {
        renderer_draw_rrect_bordered(r, isl_x, isl_y, isl_w, isl_h,
                                     cp.surface_neutral, cp.divider_subtle,
                                     fmaxf(1.0f, dpi),
                                     isl_r, isl_r, isl_r, isl_r,
                                     0.0f, 0.0f, 0.0f, 0.0f);
        renderer_flush_rrects(r);
    }
    renderer_flush_rects(r);

    Color accent = theme_ui_accent(t);
    Color muted = fg; muted.a = 0.30f;
    Color normal = fg; normal.a = 0.82f;

    /* Nav cluster. Reload lights up while a load is in flight — same accent
     * "engaged" state as the device/pick toggles, so the stop icon is not the
     * only thing saying the page is busy. */
    if (b->layout.show_back)
        draw_browser_btn(app, r, b->layout.back_x, by, bs, icon_s,
                         ICON_ARROW_LEFT,
                         b->can_go_back ? normal : muted, accent, &cp,
                         false, false);
    if (b->layout.show_fwd)
        draw_browser_btn(app, r, b->layout.fwd_x, by, bs, icon_s,
                         ICON_ARROW_RIGHT,
                         b->can_go_forward ? normal : muted, accent, &cp,
                         false, false);
    if (b->layout.show_reload)
        draw_browser_btn(app, r, b->layout.reload_x, by, bs, icon_s,
                         b->loading ? ICON_STOP : ICON_REFRESH,
                         b->loading ? accent : normal, accent, &cp,
                         b->loading, false);

    /* URL field — pill */
    if (b->layout.show_url && b->layout.url_w > 1.0f) {
        f32 uh = b->layout.url_h;
        f32 uy = b->layout.url_y;
        /* Full pill. The field is the widest element in the bar and a 6pt
         * radius on a 22pt-tall box read as a rounded rectangle inside a
         * rounded bar — two different corner languages stacked. Half the
         * height is the one radius that cannot disagree with the island. */
        f32 ur = uh * 0.5f;
        /* Focus is carried by a hairline border plus a faint accent tint in the
         * fill, so the field is present without being the loudest thing in the
         * bar. The idle field keeps a border too, so it reads as an input
         * before it is clicked. */
        f32 uhov = ui_hover_amount(b->layout.url_x, uy, b->layout.url_w, uh,
                                   app->hover_x, app->hover_y);
        /* Recessed, like every other input in the app: the palette's sunken
         * surface rather than a lerp toward fg, which on a light theme made
         * the field DARKER than the bar it sits in and inverted the depth. */
        Color field = cp.surface_sunken;
        if (b->url_focused) field = color_lerp(field, accent, 0.07f);
        else if (uhov > 0.001f) field = color_lerp(field, fg, 0.03f * uhov);
        field.a = 1.0f;

        Color edge = cp.divider_subtle;
        if (b->url_focused) { edge = accent; edge.a = 0.62f; }
        else if (uhov > 0.001f) edge.a = fminf(1.0f, edge.a + 0.16f * uhov);
        renderer_draw_rrect_bordered(r, b->layout.url_x, uy,
            b->layout.url_w, uh, field, edge, fmaxf(1.0f, dpi),
            ur, ur, ur, ur, 0.0f, 0.0f, 0.0f, 0.0f);
        renderer_flush_rrects(r);
        renderer_flush_rects(r);

        f32 ui_cw = fmaxf(6.0f, 7.5f * b->layout.scale) * dpi;
        f32 ui_ch = fmaxf(11.0f, 14.0f * b->layout.scale) * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);

        const char *raw = b->url_focused ? b->url_edit : b->url;
        char label[BROWSER_URL_CAP];
        f32 text_pad = 8.0f * b->layout.scale * dpi;
        i32 maxc = (i32)((b->layout.url_w - 2.0f * text_pad) / ui_cw);
        if (maxc < 4) maxc = 4;
        f32 text_x = b->layout.url_x + text_pad;
        f32 text_y = uy + (uh - ui_ch) * 0.5f;
        i32 caret_cell = 0;        /* visible cell index for the caret */
        bool draw_caret = false;

        if (b->url_focused) {
            /* Editing. The caret moves with the shared editor
             * (ui/textedit.h), so the window has to follow it in both
             * directions instead of always showing the end. Scroll is kept in
             * characters; the field is monospaced. */
            i32 cells = browser_text_cells(raw);
            i32 caret_cells = (app->te_owner == (const void *)b->url_edit)
                ? textedit_cell_index(raw, app->te_caret) : cells;
            i32 scroll = app->te_scroll;
            if (caret_cells < scroll)             scroll = caret_cells;
            if (caret_cells > scroll + maxc - 1)  scroll = caret_cells - maxc + 1;
            if (scroll > cells) scroll = cells;
            if (scroll < 0)     scroll = 0;
            app->te_scroll = scroll;

            i32 boff = textedit_byte_offset(raw, (i32)strlen(raw), scroll);
            snprintf(label, sizeof label, "%s", raw + boff);
            caret_cell = caret_cells - scroll;
            draw_caret = true;
        } else {
            browser_format_url_label(raw, label, sizeof label, maxc);
        }
        Color ufg = fg;
        if (!raw[0] && !b->url_focused) ufg.a = 0.34f;
        else ufg.a = 0.92f;
        /* Focused + empty shows just the caret, not the placeholder. */
        const char *shown_text = (label[0] || b->url_focused) ? label : "localhost:3000";

        if (!b->url_focused && label[0]) {
            /* Host emphasis: the origin at full strength, the path dimmed —
             * what tells you *where* you are at a glance. Browsers do this for
             * a reason, and a dev preview URL is mostly path. */
            const char *slash = strchr(shown_text, '/');
            i32 host_cells = slash
                ? browser_text_cells_n(shown_text, (i32)(slash - shown_text))
                : maxc;
            if (host_cells > maxc) host_cells = maxc;
            draw_text_ex(r, shown_text, text_x, text_y, ufg, host_cells, ui_cw);
            if (slash && host_cells < maxc) {
                Color path_c = fg; path_c.a = 0.50f;
                draw_text_ex(r, slash, text_x + (f32)host_cells * ui_cw, text_y,
                             path_c, maxc - host_cells, ui_cw);
            }
        } else {
            draw_text_ex(r, shown_text, text_x, text_y, ufg, maxc, ui_cw);
        }
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);

        if (draw_caret) {
            f32 caret_w = fmaxf(1.0f, 1.5f * dpi);
            f32 caret_h = ui_ch;
            f32 cxp = text_x + (f32)caret_cell * ui_cw;
            f32 cyp = uy + (uh - caret_h) * 0.5f;
            /* Smooth blink matching the terminal caret (period 1.1s), or a solid
             * bar when blink is disabled. */
            f32 a = 1.0f;
            if (app->config.cursor_blink) {
                f64 bnow = platform_time_sec();
                f32 phase = (f32)fmod(bnow * (2.0 * 3.14159265 / 1.1),
                                      2.0 * 3.14159265);
                f32 wave  = 0.5f + 0.5f * sinf(phase);
                a = 0.30f + 0.70f * wave;
            }
            Color caret_c = accent; caret_c.a = a;
            renderer_draw_rect(r, cxp, cyp, caret_w, caret_h, caret_c);
            renderer_flush_rects(r);
        }
    } else {
        renderer_flush_rrects(r);
        renderer_flush_rects(r);
    }

    /* Action cluster (right). A hairline before the close button keeps the one
     * dismissive control out of the run of page actions. */
    if (b->layout.show_sep && b->layout.sep_x > 0.0f) {
        /* Theme-derived, and short enough to read as a seam between two
         * clusters rather than a rule chopping the bar in half. */
        Color sep = cp.divider_subtle;
        f32 sep_h = bs * 0.44f;
        renderer_draw_rect(r, b->layout.sep_x, by + (bs - sep_h) * 0.5f,
                           fmaxf(1.0f, dpi), sep_h, sep);
        renderer_flush_rects(r);
    }
    if (b->layout.show_device) {
        /* Lit while a device preset is active, so the pane's letterboxing is
         * never unexplained. */
        bool vp_on = (b->viewport_idx != BVP_RESPONSIVE);
        Color df = (vp_on || b->viewport_menu_open) ? accent : normal;
        draw_browser_btn(app, r, b->layout.device_x, by, bs, icon_s,
                         ICON_DEVICE, df, accent, &cp,
                         vp_on || b->viewport_menu_open, false);
    }
    if (b->layout.show_pick) {
        Color pf = b->pick_mode ? accent : normal;
        draw_browser_btn(app, r, b->layout.pick_x, by, bs, icon_s,
                         ICON_SCOPE, pf, accent, &cp, b->pick_mode, false);
    }
    if (b->layout.show_attach) {
        Color af = b->has_pick ? accent : muted;
        draw_browser_btn(app, r, b->layout.attach_x, by, bs, icon_s,
                         ICON_UP, af, accent, &cp, b->has_pick, false);
    }
    if (b->layout.show_ext)
        draw_browser_btn(app, r, b->layout.ext_x, by, bs, icon_s,
                         ICON_ARROW_UP_RIGHT, normal, accent, &cp, false, false);
    if (b->layout.show_close)
        draw_browser_btn(app, r, b->layout.close_x, by, bs, icon_s,
                         ICON_CLOSE, normal, accent, &cp, false, /*danger=*/true);

    renderer_flush_rects(r);
    renderer_flush_rrects(r);

    /* Loading indicator: an indeterminate sweep along the bar's bottom edge.
     * The stop icon alone says something is happening but not that progress
     * is still being made, so a stalled dev server would look identical to a
     * fast one. Runs on the soft (~30 Hz) redraw cadence, like the tab
     * accent. */
    if (b->loading) {
        f64 now = platform_time_sec();
        f32 lh = fmaxf(2.0f, 2.0f * dpi);
        /* Rides the island's bottom edge, inset past the corner radius so the
         * rail can't poke out of the rounded corners it is supposed to sit
         * inside. Drawn as rrects for the same reason: a square-ended 2px bar
         * inside a 12pt-radius toolbar is the detail that gives a rounded UI
         * away. */
        f32 rail_x = isl_x + isl_r * 0.5f;
        f32 rail_w = isl_w - isl_r;
        /* Flush with the island's inner bottom edge. Any extra inset would
         * lift the rail INTO the URL pill — the field's bottom and the
         * island's inner bottom are only a couple of pixels apart at dpi=2 —
         * and the rail is painted after the field, so its 0.85-alpha head
         * would draw an accent stripe across the URL text. */
        f32 ly = isl_y + isl_h - lh;
        if (b->layout.show_url && b->layout.url_w > 1.0f) {
            f32 url_bot = b->layout.url_y + b->layout.url_h;
            if (ly < url_bot) ly = url_bot;
        }
        if (rail_w > 4.0f * dpi) {
            Color rail = accent; rail.a = 0.14f;
            renderer_draw_rrect_simple(r, rail_x, ly, rail_w, lh, rail, lh * 0.5f);
            /* Head sweeps left to right; width grows then shrinks so it reads
             * as motion rather than a block sliding. */
            f32 ph = (f32)(fmod(now, 1.15) / 1.15);
            f32 span = rail_w * (0.18f + 0.22f * sinf(ph * 3.14159265f));
            f32 lx = rail_x + (rail_w + span) * ph - span;
            f32 x0 = fmaxf(rail_x, lx);
            f32 x1 = fminf(rail_x + rail_w, lx + span);
            if (x1 - x0 > lh) {
                Color headc = accent; headc.a = 0.85f;
                renderer_draw_rrect_simple(r, x0, ly, x1 - x0, lh, headc,
                                           lh * 0.5f);
            }
            renderer_flush_rrects(r);
        }
        anim_register_soft_until(now + 0.1);
    }

    /* The device the preset stands for, around the page: with a preset the
     * WebView is smaller than the pane, and the phone, tablet or laptop it
     * sits in says why. */
    BrowserDeviceLayout dev;
    if (device_in_page(b, ox, oy + ch, w, h - ch, dpi, &dev)) {
        draw_browser_device(r, &dev, dpi, b->page_tint_known, b->page_tint);
        if (dev.cap_room) {
            f32 ui_cw = 6.5f * dpi, ui_ch = 12.0f * dpi;
            renderer_set_ui_scale(r, ui_cw, ui_ch);
            const BrowserViewportPreset *p = browser_viewport_get(b->viewport_idx);
            /* The screen's size, and the page's when the system keeps part of
             * the screen. */
            char size[48], pct[16] = "";
            if (dev.vw != dev.sw || dev.vh != dev.sh)
                snprintf(size, sizeof size, "%d x %d, page %d x %d",
                         dev.sw, dev.sh, dev.vw, dev.vh);
            else
                snprintf(size, sizeof size, "%d x %d", dev.sw, dev.sh);
            if (dev.scale < 0.999f)
                snprintf(pct, sizeof pct, "  (%d%%)", (i32)(dev.scale * 100.0f + 0.5f));
            char cap[128];
            snprintf(cap, sizeof cap, "%s  %s%s", p ? p->name : "", size, pct);
            i32 clen = (i32)strlen(cap);
            f32 cap_w = (f32)clen * ui_cw;
            f32 cx = (dev.ext_x + dev.ext_w * 0.5f) * dpi - cap_w * 0.5f;
            if (cx + cap_w > ox + w) cx = ox + w - cap_w;
            if (cx < ox) cx = ox;
            i32 maxc = (i32)(w / ui_cw);
            if (clen > maxc) clen = maxc;
            Color cc = fg; cc.a = 0.50f;
            draw_text_ex(r, cap, cx, dev.cap_y * dpi, cc, clen, ui_cw);
            renderer_flush_glyphs(r);
            renderer_reset_ui_scale(r);
        }
    }

    /* Status under chrome only when there's vertical room. */
    if (b->status[0] && h > ch + 18.0f * dpi) {
        f32 ui_cw = 7.0f * dpi;
        f32 ui_ch = 13.0f * dpi;
        renderer_set_ui_scale(r, ui_cw, ui_ch);
        Color st = fg; st.a = 0.45f;
        i32 maxc = (i32)((w - 16.0f * dpi) / ui_cw);
        if (maxc < 8) maxc = 8;
        draw_text_ex(r, b->status, ox + 8.0f * dpi,
                     oy + ch + 3.0f * dpi, st, maxc, ui_cw);
        renderer_flush_glyphs(r);
        renderer_reset_ui_scale(r);
    }
}

void browser_apply_visibility_and_frame(AppState *app, PlatformWindow *window) {
    if (!app) return;
    bool show = browser_should_show(app);
    BrowserContentRect cr;
    browser_content_rect(app, &cr);

    /* The file viewer/editor panel is Metal chrome laid over the right edge of
     * the window, so it overlaps whatever is there — the terminal is simply
     * painted over, but the WebView is a native subview ABOVE the Metal layer
     * and would instead cover the panel (and eat its clicks). Clip the page to
     * what the panel leaves free; the geometry mirrors the hit-tester's viewer
     * block in hittest.c, including the resize grab strip outside its left
     * edge — clicks there must reach Liu to start a resize drag. */
    f32 dpi = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    bool clipped = (app->filebrowser.view_mode != FVIEW_NONE);
    f32 clip_w_pt = 0.0f, clip_h_pt = (f32)app->fb_height / dpi;
    if (clipped) {
        f32 vw_px = app->viewer_width > 0.0f
                  ? app->viewer_width
                  : (f32)app->fb_width * VIEWER_WIDTH_RATIO;
        f32 free_px = (f32)app->fb_width - vw_px - VIEWER_RESIZE_GRAB_PT * dpi;
        if (free_px < 0.0f) free_px = 0.0f;
        clip_w_pt = free_px / dpi;
    }

    for (i32 i = 0; i < app->tab_count; i++) {
        Tab *t = &app->tabs[i];
        /* Full-tab browser or terminal tab with an embedded browser pane. */
        if (!t->browser || !t->browser->native) continue;
        if (t->kind != TAB_BROWSER && t->browser_pane < 0) continue;
        bool is_active = (i == app->active_tab) && show;
        browser_native_set_visible(t->browser->native, is_active);
        /* Cmd+drag reaches the Metal view only while there is a pane to move,
         * i.e. the browser lives in a split. A full-tab browser keeps Cmd+click
         * for the page. */
        browser_native_set_cmd_passthrough(t->browser->native,
            is_active && t->browser_pane >= 0 && t->split != SPLIT_NONE);
        browser_native_set_clip_pt(t->browser->native, clipped,
                                   0.0f, 0.0f, clip_w_pt, clip_h_pt);
        if (is_active) {
            /* A device preset puts the page on the device's screen, below
             * its status bar and above its home indicator, at the emulated
             * viewport scaled to fit; the device around it is painted by the
             * Metal chrome from the same layout. */
            BrowserDeviceLayout dev;
            if (device_in_page(t->browser, cr.x_px, cr.page_y_px, cr.w_px,
                               cr.page_h_px, dpi, &dev)) {
                t->browser->viewport_scale = dev.scale;
                /* The layout is y down from the window's top; AppKit's
                 * points run up from its bottom. */
                f32 view_h_pt = (f32)app->fb_height / dpi;
                f32 py = view_h_pt - (dev.page.y + dev.page.h);
                browser_native_set_viewport_pt(t->browser->native,
                    dev.page.x, py, dev.page.w, dev.page.h, dev.vw, dev.vh);
                BrowserScreenShape shape;
                for (i32 c = 0; c < 4; c++) shape.r[c] = dev.page.r[c];
                browser_native_set_screen_shape(t->browser->native, &shape);
            } else {
                t->browser->viewport_scale = 1.0f;
                browser_native_set_frame_pt(t->browser->native,
                    cr.page_x_pt, cr.page_y_pt, cr.page_w_pt, cr.page_h_pt);
                browser_native_set_screen_shape(t->browser->native, NULL);
            }
        }
    }
    (void)window;
}

void app_browser_pick_arrived(AppState *app, BrowserTab *browser,
                              const BrowserPick *pick) {
    if (!app || !browser || !pick) return;
    browser->last_pick = *pick;
    browser->has_pick = true;
    browser->pick_mode = false;
    /* Auto-attach the pick to the focused agent. */
    app_browser_inject_pick(app, pick,
        browser->site_path[0] ? browser->site_path : NULL, false);
}

bool app_browser_tick(AppState *app) {
    if (!app) return false;
    const Theme *theme = app->config.theme ? app->config.theme : &THEME_Liu;
    bool dirty = false;
    for (i32 i = 0; i < app->tab_count; i++) {
        Tab *t = &app->tabs[i];
        if (!t->browser || !t->browser->native) continue;
        if (t->kind != TAB_BROWSER && t->browser_pane < 0) continue;

        /* Theme drift: a switch in Settings has no notification path down to
         * the WebView, so compare names here (one strcmp per browser tab per
         * wake) and re-push. The native side re-renders whichever internal
         * page is on screen; a live site is untouched. */
        if (strcmp(t->browser->themed_as, theme->name) != 0)
            app_browser_send_page_theme(app, t->browser);

        /* Poll is O(1) when idle (revision early-out) and returns whether any
         * state was copied. */
        bool changed = browser_native_poll(t->browser->native, t->browser);

        /* Drain pending Select Element picks. */
        if (t->browser->pending_pick) {
            BrowserPick pick;
            if (browser_native_take_pick(t->browser->native, &pick)) {
                app_browser_pick_arrived(app, t->browser, &pick);
                dirty = true;
            } else {
                /* cancel */
                t->browser->pick_mode = false;
                dirty = true;
            }
        }

        /* Drain a pending navigation from clicking a Recents start-page entry. */
        if (t->browser->pending_nav) {
            char nav_url[BROWSER_URL_CAP];
            char norm[BROWSER_URL_CAP];
            if (browser_native_take_nav(t->browser->native, nav_url, sizeof nav_url) &&
                browser_nav_normalize(nav_url, norm, sizeof norm) &&
                browser_nav_is_allowed_with_list(norm, &app->config.browser_allowlist)) {
                browser_native_navigate(t->browser->native, norm);
                snprintf(t->browser->url, sizeof t->browser->url, "%s", norm);
                app_browser_push_recent(app, norm);
                dirty = true;
            }
            t->browser->pending_nav = false;
        }

        /* Drain a pending "Ask Agent" from the connection-error page. */
        if (t->browser->pending_ask) {
            char err_url[BROWSER_URL_CAP];
            char err_reason[256];
            i32  err_code = 0;
            if (browser_native_take_ask(t->browser->native, err_url, sizeof err_url,
                                        err_reason, sizeof err_reason, &err_code)) {
                app_browser_inject_error(app, err_url, err_reason, err_code,
                    t->browser->site_path[0] ? t->browser->site_path : NULL);
                dirty = true;
            }
            t->browser->pending_ask = false;
        }

        if (changed) {
            dirty = true;
            if (t->browser->title[0] && !t->title_locked && !t->custom_title[0]) {
                snprintf(t->title, sizeof t->title, "%s", t->browser->title);
            }
        }
    }
    return dirty;
}

/* ---------------------------------------------------------------------------
 * Viewport picker popover
 *
 * Its own layout rather than the generic context-menu chrome: every row has to
 * carry three things at once — device name, CSS resolution and pixel ratio —
 * and the eye should be able to scan the resolution column on its own. So the
 * name is left-aligned, the "393 × 852" is a right-aligned monospaced column,
 * and the "@3x" sits after it as a dim chip. Group headers (Phone / Tablet /
 * Laptop) break the list up; the active row gets an accent wash plus a check.
 * ------------------------------------------------------------------------- */

#define BVP_MENU_W_PT      248.0f
#define BVP_MENU_ROW_PT     26.0f
#define BVP_MENU_HDR_PT     20.0f
#define BVP_MENU_PAD_PT      8.0f
#define BVP_MENU_FOOT_PT    30.0f
#define BVP_MENU_CELL_PT     7.0f   /* the menu's text cell */
#define BVP_MENU_PADX_PT    12.0f
#define BVP_MENU_DPR_GAP_PT  6.0f   /* between "853 x 1280" and "@1.5x" */
#define BVP_MENU_NAME_GAP_PT 14.0f  /* the least room between a name and its size */

/* A preset row's right-hand labels, "853 x 1280" and "@1.5x". */
static void bvp_row_labels(const BrowserViewportPreset *p, bool landscape,
                           char *res, usize res_cap, char *dprs, usize dpr_cap) {
    i32 vw = p->w, vh = p->h;
    if (landscape) { i32 sw = vw; vw = vh; vh = sw; }
    snprintf(res, res_cap, "%d x %d", vw, vh);
    f32 fr = p->dpr;
    if (fr == (f32)(i32)fr) snprintf(dprs, dpr_cap, "@%dx", (i32)fr);
    else                    snprintf(dprs, dpr_cap, "@%.1fx", (double)fr);
}

static i32 bvp_cols(const char *s) {
    return (i32)utf8_display_cols((const u8 *)s, strlen(s));
}

/* The popover's width: its longest row with the least gap, never narrower
 * than BVP_MENU_W_PT, and no wider than the pane once past that. A row that
 * still does not fit has its name cut where the size column starts. */
static f32 bvp_menu_w(const BrowserContentRect *cr, f32 dpi) {
    i32 cols = 0;
    i32 n = browser_viewport_count();
    for (i32 i = 0; i < n; i++) {
        const BrowserViewportPreset *p = browser_viewport_get(i);
        if (!p || i == BVP_RESPONSIVE) continue;
        char res[32], dprs[12];
        bvp_row_labels(p, false, res, sizeof res, dprs, sizeof dprs);
        i32 c = bvp_cols(p->name) + bvp_cols(res) + bvp_cols(dprs);
        if (c > cols) cols = c;
    }
    f32 want = (2.0f * BVP_MENU_PADX_PT + (f32)cols * BVP_MENU_CELL_PT +
                BVP_MENU_DPR_GAP_PT + BVP_MENU_NAME_GAP_PT) * dpi;
    f32 least = BVP_MENU_W_PT * dpi;
    if (want < least) want = least;
    f32 room = cr->w_px - 8.0f * dpi;
    if (want > room) want = room > least ? room : least;
    return want;
}

/* Row geometry for the popover. `out_y` receives the top of row `idx` (in the
 * preset table's own indexing) and the function returns the menu's total
 * height; passing idx < 0 just measures. Group headers are inserted before the
 * first preset of each group, so the two must agree — one function computes
 * both, and the renderer and hit-test both call it. */
static f32 bvp_menu_layout(f32 dpi, i32 want_idx, f32 *out_y) {
    f32 row = BVP_MENU_ROW_PT * dpi;
    f32 hdr = BVP_MENU_HDR_PT * dpi;
    f32 y = BVP_MENU_PAD_PT * dpi;
    i32 last_group = -1;
    i32 n = browser_viewport_count();
    for (i32 i = 0; i < n; i++) {
        const BrowserViewportPreset *p = browser_viewport_get(i);
        if (!p) continue;
        if (i != BVP_RESPONSIVE && p->group != last_group) {
            last_group = p->group;
            y += hdr;
        }
        if (i == want_idx) { if (out_y) *out_y = y; }
        y += row;
    }
    return y + BVP_MENU_PAD_PT * dpi;
}

/* On-screen panel height: the scrollable list capped to what the pane can
 * hold, plus the fixed footer. With ~80 device presets the full list is far
 * taller than any pane, so the popover scrolls instead of being clipped to
 * whatever happened to fit. */
static f32 bvp_menu_panel_h(f32 dpi, f32 avail_h) {
    f32 foot = BVP_MENU_FOOT_PT * dpi;
    f32 content = bvp_menu_layout(dpi, -1, NULL);
    f32 max_list = avail_h - foot - 8.0f * dpi;
    if (max_list < BVP_MENU_ROW_PT * dpi * 3.0f)
        max_list = BVP_MENU_ROW_PT * dpi * 3.0f;
    f32 list = content < max_list ? content : max_list;
    return list + foot;
}

static f32 bvp_menu_max_scroll(f32 dpi, f32 avail_h) {
    f32 content = bvp_menu_layout(dpi, -1, NULL);
    f32 list = bvp_menu_panel_h(dpi, avail_h) - BVP_MENU_FOOT_PT * dpi;
    f32 max_s = content - list;
    return max_s > 0.0f ? max_s : 0.0f;
}

/* Preset index at a point inside the popover, or -1. Mirrors bvp_menu_layout. */
static i32 bvp_menu_hit(f32 dpi, f32 menu_x, f32 menu_y, f32 menu_w,
                        f32 list_h, f32 scroll, f32 mx, f32 my) {
    if (mx < menu_x || mx >= menu_x + menu_w) return -1;
    if (my < menu_y || my >= menu_y + list_h) return -1;   /* footer / outside */
    f32 row = BVP_MENU_ROW_PT * dpi;
    i32 n = browser_viewport_count();
    for (i32 i = 0; i < n; i++) {
        f32 ry = 0.0f;
        (void)bvp_menu_layout(dpi, i, &ry);
        ry -= scroll;
        if (my >= menu_y + ry && my < menu_y + ry + row) return i;
    }
    return -1;
}

/* Where the popover is anchored: under the device button, right-aligned to it
 * but kept inside the pane. */
static void bvp_menu_origin(const BrowserTab *b, const BrowserContentRect *cr,
                            f32 dpi, f32 *out_x, f32 *out_y) {
    f32 mw = bvp_menu_w(cr, dpi);
    f32 x = b->layout.device_x + b->layout.btn_s - mw;
    if (x < cr->x_px + 4.0f * dpi) x = cr->x_px + 4.0f * dpi;
    if (x + mw > cr->x_px + cr->w_px - 4.0f * dpi)
        x = cr->x_px + cr->w_px - 4.0f * dpi - mw;
    *out_x = x;
    *out_y = cr->y_px + cr->chrome_h_px + 4.0f * dpi;
}

/* Vertical space the popover may occupy inside the pane. */
static f32 bvp_menu_avail_h(const BrowserContentRect *cr, f32 dpi) {
    f32 avail = cr->h_px - cr->chrome_h_px - 8.0f * dpi;
    return avail > 0.0f ? avail : 0.0f;
}

void browser_render_viewport_menu(AppState *app, Renderer *r) {
    if (!app) return;
    Tab *tab = app_active_tab(app);
    if (!tab || !tab->browser || !tab->browser->viewport_menu_open) return;
    if (tab->kind != TAB_BROWSER && tab->browser_pane < 0) return;
    BrowserTab *b = tab->browser;

    const Theme *t = app->config.theme ? app->config.theme : &THEME_Liu;
    ChromePalette cp = chrome_palette_for(t);
    f32 dpi = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;

    BrowserContentRect cr;
    browser_content_rect(app, &cr);

    f32 mx, my;
    bvp_menu_origin(b, &cr, dpi, &mx, &my);
    f32 mw = bvp_menu_w(&cr, dpi);
    f32 avail = bvp_menu_avail_h(&cr, dpi);
    f32 mh = bvp_menu_panel_h(dpi, avail);
    f32 list_h = mh - BVP_MENU_FOOT_PT * dpi;
    f32 max_scroll = bvp_menu_max_scroll(dpi, avail);
    if (b->viewport_menu_scroll > max_scroll) b->viewport_menu_scroll = max_scroll;
    if (b->viewport_menu_scroll < 0.0f)       b->viewport_menu_scroll = 0.0f;
    f32 scroll = b->viewport_menu_scroll;

    Color panel = cp.surface_raised; panel.a = 1.0f;
    Color border = cp.divider_strong; border.a = fmaxf(border.a, 0.75f);
    renderer_draw_rrect_bordered(r, mx, my, mw, mh, panel, border,
                                 fmaxf(1.0f, dpi), 12.0f * dpi, 12.0f * dpi,
                                 12.0f * dpi, 12.0f * dpi,
                                 18.0f * dpi, 0.34f, 0.0f, 2.0f * dpi);
    renderer_flush_rrects(r);

    f32 ui_cw = BVP_MENU_CELL_PT * dpi;
    f32 ui_ch = 13.0f * dpi;
    renderer_set_ui_scale(r, ui_cw, ui_ch);

    Color accent  = theme_ui_accent(t);
    Color fg      = t->fg;
    Color dim     = fg; dim.a = 0.52f;
    Color faint   = fg; faint.a = 0.36f;
    f32 row = BVP_MENU_ROW_PT * dpi;
    f32 hdr = BVP_MENU_HDR_PT * dpi;
    f32 padx = BVP_MENU_PADX_PT * dpi;

    /* The list scrolls; the footer does not. */
    renderer_flush_rects(r);
    renderer_push_scissor(r, mx, my, mw, list_h);

    i32 last_group = -1;
    i32 n = browser_viewport_count();
    for (i32 i = 0; i < n; i++) {
        const BrowserViewportPreset *p = browser_viewport_get(i);
        if (!p) continue;
        f32 ry = 0.0f;
        (void)bvp_menu_layout(dpi, i, &ry);
        ry -= scroll;
        /* Skip rows fully outside the viewport — with ~80 devices this is the
         * difference between 80 row draws and the dozen actually on screen. */
        if (ry + row < 0.0f || ry > list_h) {
            if (i != BVP_RESPONSIVE) last_group = p->group;
            continue;
        }

        if (i != BVP_RESPONSIVE && p->group != last_group) {
            last_group = p->group;
            const char *gname = browser_viewport_group_name(p->group);
            draw_text_ex(r, gname, mx + padx, my + ry - hdr + 5.0f * dpi,
                         faint, (i32)strlen(gname), ui_cw);
        }

        bool active = (i == b->viewport_idx);
        bool hover  = (i == b->viewport_menu_hover);
        if (active || hover) {
            Color wash = accent;
            wash.a = active ? 0.20f : 0.10f;
            renderer_draw_rect(r, mx + 4.0f * dpi, my + ry,
                               mw - 8.0f * dpi, row, wash);
        }

        Color name_c = active ? accent : fg;
        f32 ty = my + ry + (row - ui_ch) * 0.5f;
        f32 name_end = mx + mw - padx;

        if (i != BVP_RESPONSIVE) {
            /* Resolution is right-aligned so the column scans vertically —
             * the whole reason this menu isn't a plain list of names. */
            char res[32], dprs[12];
            bvp_row_labels(p, b->viewport_landscape, res, sizeof res,
                           dprs, sizeof dprs);
            i32 dlen = (i32)strlen(dprs);
            f32 dx = mx + mw - padx - (f32)bvp_cols(dprs) * ui_cw;
            draw_text_ex(r, dprs, dx, ty, faint, dlen, ui_cw);
            i32 rlen = (i32)strlen(res);
            f32 rx = dx - BVP_MENU_DPR_GAP_PT * dpi - (f32)bvp_cols(res) * ui_cw;
            draw_text_ex(r, res, rx, ty, dim, rlen, ui_cw);
            name_end = rx - BVP_MENU_NAME_GAP_PT * dpi;
        }

        /* A name longer than its room ends in an ellipsis before the size
         * column instead of running under it. */
        i32 fit = (i32)floorf((name_end - (mx + padx)) / ui_cw);
        i32 ncols = bvp_cols(p->name);
        if (ncols <= fit) {
            draw_text_ex(r, p->name, mx + padx, ty, name_c,
                         (i32)strlen(p->name), ui_cw);
        } else if (fit >= 2) {
            draw_text_ex(r, p->name, mx + padx, ty, name_c, fit - 1, ui_cw);
            draw_text_ex(r, "\xE2\x80\xA6", mx + padx + (f32)(fit - 1) * ui_cw,
                         ty, name_c, 1, ui_cw);
        }
    }

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    renderer_pop_scissor(r);

    /* Scrollbar — only while there is something to scroll. */
    if (max_scroll > 0.5f) {
        f32 content = bvp_menu_layout(dpi, -1, NULL);
        f32 track_x = mx + mw - 4.0f * dpi;
        f32 thumb_h = list_h * (list_h / content);
        if (thumb_h < 18.0f * dpi) thumb_h = 18.0f * dpi;
        f32 thumb_y = my + (list_h - thumb_h) * (scroll / max_scroll);
        Color thumb = fg; thumb.a = 0.22f;
        renderer_draw_rect(r, track_x, thumb_y, 2.5f * dpi, thumb_h, thumb);
        renderer_flush_rects(r);
    }

    /* Footer: orientation toggle + the scale the fit actually landed on, so a
     * shrunken desktop preset says so instead of silently lying. */
    f32 fy = my + mh - BVP_MENU_FOOT_PT * dpi;
    Color divider = cp.divider_subtle;
    renderer_draw_rect(r, mx + 4.0f * dpi, fy, mw - 8.0f * dpi,
                       fmaxf(1.0f, dpi), divider);
    char foot[64];
    if (b->viewport_idx == BVP_RESPONSIVE) {
        snprintf(foot, sizeof foot, "Fills the pane");
    } else {
        snprintf(foot, sizeof foot, "%s  -  Fit %d%%",
                 browser_viewport_is_landscape(b->viewport_idx,
                                               b->viewport_landscape)
                     ? "Landscape" : "Portrait",
                 (i32)(b->viewport_scale * 100.0f + 0.5f));
    }
    draw_text_ex(r, foot, mx + padx, fy + (BVP_MENU_FOOT_PT * dpi - ui_ch) * 0.5f,
                 dim, (i32)strlen(foot), ui_cw);
    const char *hint = "R rotate";
    i32 hlen = (i32)strlen(hint);
    draw_text_ex(r, hint, mx + mw - padx - (f32)hlen * ui_cw,
                 fy + (BVP_MENU_FOOT_PT * dpi - ui_ch) * 0.5f,
                 faint, hlen, ui_cw);

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r);
}

bool browser_viewport_menu_click(AppState *app, f32 mx, f32 my) {
    if (!app) return false;
    Tab *tab = app_active_tab(app);
    if (!tab || !tab->browser || !tab->browser->viewport_menu_open) return false;
    BrowserTab *b = tab->browser;
    f32 dpi = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    BrowserContentRect cr;
    browser_content_rect(app, &cr);
    f32 menu_x, menu_y;
    bvp_menu_origin(b, &cr, dpi, &menu_x, &menu_y);

    f32 avail = bvp_menu_avail_h(&cr, dpi);
    f32 mw = bvp_menu_w(&cr, dpi);
    f32 mh = bvp_menu_panel_h(dpi, avail);
    f32 list_h = mh - BVP_MENU_FOOT_PT * dpi;
    i32 idx = bvp_menu_hit(dpi, menu_x, menu_y, mw, list_h,
                           b->viewport_menu_scroll, mx, my);
    if (idx >= 0) {
        /* Re-picking the active preset rotates it — the orientation toggle
         * where the hand already is, instead of a separate control. */
        if (idx == b->viewport_idx && idx != BVP_RESPONSIVE)
            b->viewport_landscape = !b->viewport_landscape;
        else
            b->viewport_idx = idx;
        b->viewport_menu_open = false;
        return true;
    }
    /* Any click inside the popover but not on a row is swallowed; outside
     * closes it. */
    bool inside = (mx >= menu_x && mx < menu_x + mw &&
                   my >= menu_y && my < menu_y + mh);
    if (!inside) b->viewport_menu_open = false;
    return true;
}

bool browser_viewport_menu_scroll(AppState *app, f32 mx, f32 my,
                                  f32 dy, bool precise) {
    if (!app) return false;
    Tab *tab = app_active_tab(app);
    if (!tab || !tab->browser || !tab->browser->viewport_menu_open) return false;
    BrowserTab *b = tab->browser;
    f32 dpi = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    BrowserContentRect cr;
    browser_content_rect(app, &cr);
    f32 menu_x, menu_y;
    bvp_menu_origin(b, &cr, dpi, &menu_x, &menu_y);
    f32 avail = bvp_menu_avail_h(&cr, dpi);
    f32 mw = bvp_menu_w(&cr, dpi);
    f32 mh = bvp_menu_panel_h(dpi, avail);
    if (mx < menu_x || mx >= menu_x + mw || my < menu_y || my >= menu_y + mh)
        return false;

    /* Trackpad deltas are pixels, wheel deltas are notches — treating the
     * former as the latter scrolls absurdly fast (see the palette / transcript
     * wheel handlers for the same rule). */
    f32 step = precise ? dy : dy * BVP_MENU_ROW_PT * dpi;
    b->viewport_menu_scroll -= step;
    f32 max_scroll = bvp_menu_max_scroll(dpi, avail);
    if (b->viewport_menu_scroll > max_scroll) b->viewport_menu_scroll = max_scroll;
    if (b->viewport_menu_scroll < 0.0f)       b->viewport_menu_scroll = 0.0f;
    return true;
}

bool browser_viewport_menu_hover(AppState *app, f32 mx, f32 my) {
    if (!app) return false;
    Tab *tab = app_active_tab(app);
    if (!tab || !tab->browser || !tab->browser->viewport_menu_open) return false;
    BrowserTab *b = tab->browser;
    f32 dpi = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    BrowserContentRect cr;
    browser_content_rect(app, &cr);
    f32 menu_x, menu_y;
    bvp_menu_origin(b, &cr, dpi, &menu_x, &menu_y);
    f32 avail = bvp_menu_avail_h(&cr, dpi);
    f32 list_h = bvp_menu_panel_h(dpi, avail) - BVP_MENU_FOOT_PT * dpi;
    i32 idx = bvp_menu_hit(dpi, menu_x, menu_y, bvp_menu_w(&cr, dpi), list_h,
                           b->viewport_menu_scroll, mx, my);
    if (idx != b->viewport_menu_hover) {
        b->viewport_menu_hover = idx;
        return true;
    }
    return false;
}

bool browser_viewport_menu_row_center(const AppState *app, i32 idx,
                                      f32 *cx, f32 *cy) {
    if (!app) return false;
    Tab *tab = app_active_tab((AppState *)app);
    if (!tab || !tab->browser || !tab->browser->viewport_menu_open) return false;
    if (idx < 0 || idx >= browser_viewport_count()) return false;
    const BrowserTab *b = tab->browser;
    f32 dpi = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    BrowserContentRect cr;
    browser_content_rect(app, &cr);
    f32 menu_x, menu_y;
    bvp_menu_origin(b, &cr, dpi, &menu_x, &menu_y);
    f32 avail = bvp_menu_avail_h(&cr, dpi);
    f32 list_h = bvp_menu_panel_h(dpi, avail) - BVP_MENU_FOOT_PT * dpi;
    f32 ry = 0.0f;
    (void)bvp_menu_layout(dpi, idx, &ry);
    ry -= b->viewport_menu_scroll;
    f32 row = BVP_MENU_ROW_PT * dpi;
    if (ry < 0.0f || ry + row > list_h) return false;
    if (cx) *cx = menu_x + bvp_menu_w(&cr, dpi) * 0.5f;
    if (cy) *cy = menu_y + ry + row * 0.5f;
    return true;
}

bool browser_viewport_menu_rect(const AppState *app, f32 *x, f32 *y,
                                f32 *w, f32 *h) {
    if (!app) return false;
    Tab *tab = app_active_tab((AppState *)app);
    if (!tab || !tab->browser || !tab->browser->viewport_menu_open) return false;
    f32 dpi = app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f;
    BrowserContentRect cr;
    browser_content_rect(app, &cr);
    f32 mx, my;
    bvp_menu_origin(tab->browser, &cr, dpi, &mx, &my);
    if (x) *x = mx;
    if (y) *y = my;
    if (w) *w = bvp_menu_w(&cr, dpi);
    if (h) *h = bvp_menu_panel_h(dpi, bvp_menu_avail_h(&cr, dpi));
    return true;
}

i32 browser_chrome_hit(const AppState *app, f32 mx, f32 my) {
    if (!app) return BROWSER_HIT_NONE;
    Tab *tab = app_active_tab((AppState *)app);
    if (!tab || !tab->browser) return BROWSER_HIT_NONE;
    if (tab->kind != TAB_BROWSER && tab->browser_pane < 0) return BROWSER_HIT_NONE;
    BrowserTab *b = tab->browser;
    BrowserContentRect cr;
    browser_content_rect(app, &cr);
    if (my < cr.y_px || my >= cr.y_px + cr.chrome_h_px) return BROWSER_HIT_NONE;
    if (mx < cr.x_px || mx >= cr.x_px + cr.w_px) return BROWSER_HIT_NONE;

    fill_chrome_layout(b, cr.x_px, cr.y_px, cr.w_px, cr.h_px, app->dpi_scale);
    f32 bs = b->layout.btn_s;
    f32 by = b->layout.btn_y;
    #define HIT_BTN(show, x) \
        ((show) && (x) >= 0.0f && mx >= (x) && mx < (x) + bs && \
         my >= by && my < by + bs)
    if (HIT_BTN(b->layout.show_back,   b->layout.back_x))   return BROWSER_HIT_BACK;
    if (HIT_BTN(b->layout.show_fwd,    b->layout.fwd_x))    return BROWSER_HIT_FWD;
    if (HIT_BTN(b->layout.show_reload, b->layout.reload_x)) return BROWSER_HIT_RELOAD;
    if (HIT_BTN(b->layout.show_device, b->layout.device_x)) return BROWSER_HIT_DEVICE;
    if (HIT_BTN(b->layout.show_pick,   b->layout.pick_x))   return BROWSER_HIT_PICK;
    if (HIT_BTN(b->layout.show_attach, b->layout.attach_x)) return BROWSER_HIT_ATTACH;
    if (HIT_BTN(b->layout.show_ext,    b->layout.ext_x))    return BROWSER_HIT_EXTERNAL;
    if (HIT_BTN(b->layout.show_close,  b->layout.close_x))  return BROWSER_HIT_CLOSE;
    #undef HIT_BTN
    if (b->layout.show_url && b->layout.url_w > 0.0f &&
        mx >= b->layout.url_x && mx < b->layout.url_x + b->layout.url_w &&
        my >= b->layout.url_y && my < b->layout.url_y + b->layout.url_h)
        return BROWSER_HIT_URL;
    return BROWSER_HIT_NONE;
}
