/* Shared notification measurements and theme colours. No persistent state. */
#ifndef UI_NOTIFICATION_H
#define UI_NOTIFICATION_H

#include "ui/layout.h"
#include "ui/chrome_palette.h"
#include "ui/status_colors.h"
#include "core/utf8.h"
#include <math.h>
#include <string.h>

#define NOTIFICATION_MAX_LINES 3
#define NOTIFICATION_TEXT_CAP 256

typedef struct {
    char lines[NOTIFICATION_MAX_LINES][NOTIFICATION_TEXT_CAP];
    i32 count;
} NotificationText;

/* What a notice is about. The mark, the tile's tint and how long it stays all
 * follow from it; a notice that says nothing of its own is INFO. */
typedef enum {
    NOTIFY_INFO = 0,
    NOTIFY_SUCCESS,
    NOTIFY_WARNING,
    NOTIFY_ERROR,
    NOTIFY_KIND_COUNT
} NotificationKind;

typedef struct {
    Color body, border, fg, muted, accent, badge;
    f32 shadow_alpha;
} NotificationStyle;

static inline bool notification_has(const char *text, const char *needle) {
    usize n = strlen(needle);
    for (const char *p = text; *p; p++) {
        usize i = 0;
        while (i < n && p[i]) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (a != b) break;
            i++;
        }
        if (i == n) return true;
    }
    return false;
}

/* The kind of a message from the way the app words it. Two hundred call sites
 * say "could not …", "… failed", "… limit reached" and "… copied" in the same
 * few phrasings, so the kind is read off the phrase instead of being passed by
 * each of them; a caller that knows better says so through
 * app_show_toast_kind(). The order is the precedence: a failure outranks a
 * success word in the same sentence ("Note created, but it could not be
 * opened"), and "already" is a statement, never a result. A message that
 * matches nothing is INFO, which is also what it looked like before. */
static inline NotificationKind notification_kind_from_text(const char *text) {
    static const char *const error[] = {
        "failed", "could not", "couldn't", "unable to", "invalid", "error",
        "not found", "no longer", "timed out", "out of memory", "refused",
        "denied", "no permission", "unsupported", "cannot", "not valid", NULL };
    static const char *const warning[] = {
        "limit reached", "too small", "too long", "too many", "stayed awake",
        "cancelled", "canceled", "stopped", "busy", "not compiled",
        "not downloaded", "not installed", "is off", "nothing to",
        "no sleeping", "not enough", "no folder history", "no speech",
        "is full", "is waiting", "first", "can't", "not in a group",
        "is empty", "no element", "unlock vault", "not ready", "prompt kept",
        "unsaved",   /* before the success list: "unsaved" holds "saved" */
        NULL };
    static const char *const success[] = {
        "unlocked", "copied", "created", "ready", "saved", "done",
        "finished", "opened", "added", "continued in", "downloaded", NULL };
    if (!text || !*text) return NOTIFY_INFO;
    for (i32 i = 0; error[i]; i++)
        if (notification_has(text, error[i])) return NOTIFY_ERROR;
    for (i32 i = 0; warning[i]; i++)
        if (notification_has(text, warning[i])) return NOTIFY_WARNING;
    if (notification_has(text, "already")) return NOTIFY_INFO;
    for (i32 i = 0; success[i]; i++)
        if (notification_has(text, success[i])) return NOTIFY_SUCCESS;
    return NOTIFY_INFO;
}

/* How long a notice of this kind stays, hover aside. A failure or a refusal
 * carries a reason the reader has to get through; a confirmation is read at a
 * glance. */
static inline f64 notification_hold_s(NotificationKind kind) {
    switch (kind) {
        case NOTIFY_ERROR:   return 7.0;
        case NOTIFY_WARNING: return 5.0;
        case NOTIFY_SUCCESS: return 3.0;
        default:             return NOTIFICATION_HOLD_S;
    }
}

/* The colour a kind speaks in: the theme's accent for INFO, its own green,
 * yellow and red otherwise (ui/status_colors.h). */
static inline Color notification_kind_color(const Theme *t, NotificationKind kind) {
    switch (kind) {
        case NOTIFY_SUCCESS: return status_color_safe(t);
        case NOTIFY_WARNING: return status_color_warn(t);
        case NOTIFY_ERROR:   return status_color_danger(t);
        default:             return theme_ui_accent(t);
    }
}

static inline Color notification_blend(Color a, Color b, f32 amount) {
    return (Color){a.r + (b.r - a.r) * amount,
                   a.g + (b.g - a.g) * amount,
                   a.b + (b.b - a.b) * amount, 1.0f};
}

static inline NotificationStyle notification_style_for(const Theme *t,
                                                        ChromePalette cp,
                                                        NotificationKind kind) {
    NotificationStyle s;
    /* Nearly opaque so terminal text behind the card cannot muddy its copy. */
    s.body = cp.surface_neutral;
    s.body.a = 0.98f;
    s.fg = cp.btn_secondary_fg;
    s.muted = notification_blend(s.body, s.fg, 0.68f);
    s.border = notification_blend(s.body, s.fg, cp.is_light ? 0.18f : 0.16f);
    s.accent = notification_kind_color(t, kind);
    /* Keep the icon legible even for a very pale user-supplied accent. A
     * status colour is blended less: pulled far toward the text colour a red
     * stops reading as one. */
    f32 pull = kind == NOTIFY_INFO ? (cp.is_light ? 0.36f : 0.18f)
                                   : (cp.is_light ? 0.22f : 0.06f);
    s.accent = notification_blend(s.accent, s.fg, pull);
    s.badge = notification_blend(s.body, s.accent, 0.14f);
    s.shadow_alpha = cp.is_light ? 0.12f : 0.24f;
    return s;
}

static inline NotificationStyle notification_style(const Theme *t,
                                                     ChromePalette cp) {
    return notification_style_for(t, cp, NOTIFY_INFO);
}

/* Match draw_text_ex's grapheme advances, including two-cell emoji/CJK. */
static inline i32 notification_columns(const char *text) {
    const u8 *p = (const u8 *)(text ? text : "");
    const u8 *end = p + strlen((const char *)p);
    i32 cols = 0;
    while (p < end) {
        u32 cp; i32 width;
        u32 n = utf8_next_grapheme(p, (usize)(end - p), &cp, &width);
        if (!n) { p++; continue; }
        cols += cp < 32 ? 1 : (width == 2 ? 2 : 1);
        p += n;
    }
    return cols;
}

/* Word wrapping with whole graphemes. The last visible line gets an ellipsis
 * only when content remains; long paths can break without a space. */
static inline NotificationText notification_wrap(const char *text,
                                                  i32 max_cols, i32 max_lines) {
    NotificationText out = {0};
    if (!text || !*text || max_cols < 1 || max_lines < 1) return out;
    if (max_lines > NOTIFICATION_MAX_LINES) max_lines = NOTIFICATION_MAX_LINES;
    const u8 *p = (const u8 *)text;
    const u8 *end = p + strlen(text);
    while (p < end && out.count < max_lines) {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\t')) p++;
        if (p == end) break;
        char *line = out.lines[out.count++];
        usize bytes = 0, space_bytes = 0;
        const u8 *after_space = NULL;
        i32 cols = 0;
        bool overflow = false;
        while (p < end && *p != '\n') {
            u32 cp; i32 width;
            u32 n = utf8_next_grapheme(p, (usize)(end - p), &cp, &width);
            if (!n) { p++; continue; }
            i32 advance = cp < 32 ? 1 : (width == 2 ? 2 : 1);
            if (cols + advance > max_cols || bytes + n + 4 >= NOTIFICATION_TEXT_CAP) {
                overflow = true;
                break;
            }
            if (*p == ' ' || cp < 32) {
                space_bytes = bytes;
                after_space = p + n;
                line[bytes++] = ' ';
            } else {
                memcpy(line + bytes, p, n);
                bytes += n;
            }
            cols += advance;
            p += n;
        }
        if (overflow && after_space && out.count < max_lines &&
            p < end && *p != ' ' && *p != '\t') {
            bytes = space_bytes;
            p = after_space;
        }
        while (bytes && line[bytes - 1] == ' ') bytes--;
        line[bytes] = '\0';
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\t')) p++;
        if ((out.count == max_lines && p < end) || (overflow && bytes == 0)) {
            /* Re-walk instead of deleting bytes: never cut an emoji cluster. */
            const u8 *q = (const u8 *)line;
            const u8 *line_end = q + bytes;
            usize keep = 0;
            cols = 0;
            while (q < line_end) {
                u32 cp; i32 width;
                u32 n = utf8_next_grapheme(q, (usize)(line_end - q), &cp, &width);
                if (!n) break;
                i32 advance = width == 2 ? 2 : 1;
                if (cols + advance > max_cols - 1) break;
                cols += advance; keep += n; q += n;
            }
            while (keep && line[keep - 1] == ' ') keep--;
            memcpy(line + keep, "\xe2\x80\xa6", 4);
            break;
        }
    }
    return out;
}

typedef struct {
    f32 x, y, w, h, text_x, text_y, cw, ch;
    NotificationText text;
} NotificationLayout;

static inline NotificationLayout notification_toast_layout(const char *message,
                    f32 fb_width, f32 tab_bar_height, f32 dpi) {
    NotificationLayout l = {0};
    if (dpi <= 0.0f) dpi = 1.0f;
    f32 pad = NOTIFICATION_PAD_PT * dpi;
    f32 leading = (NOTIFICATION_TILE_PT + NOTIFICATION_GAP_PT) * dpi;
    f32 room = fb_width - 2.0f * NOTIFICATION_EDGE_PT * dpi;
    if (room < 2.0f * pad + leading + 2.0f * 8.0f * dpi) return l;
    l.cw = 8.0f * dpi; l.ch = 16.0f * dpi;
    f32 need = 2.0f * pad + leading + notification_columns(message) * l.cw;
    l.w = roundf(fminf(room, fminf(NOTIFICATION_MAX_W_PT * dpi,
                      fmaxf(NOTIFICATION_MIN_W_PT * dpi, need))));
    i32 cols = (i32)((l.w - 2.0f * pad - leading) / l.cw);
    l.text = notification_wrap(message, cols, NOTIFICATION_MAX_LINES);
    if (!l.text.count) return (NotificationLayout){0};
    f32 text_h = l.text.count * l.ch +
                 (l.text.count - 1) * NOTIFICATION_LINE_GAP_PT * dpi;
    l.h = roundf(2.0f * pad + fmaxf(NOTIFICATION_TILE_PT * dpi, text_h));
    l.x = roundf(fb_width - NOTIFICATION_EDGE_PT * dpi - l.w);
    l.y = roundf(tab_bar_height + NOTIFICATION_TOP_PT * dpi);
    l.text_x = l.x + pad + leading;
    l.text_y = l.y + roundf((l.h - text_h) * 0.5f);
    return l;
}

#endif /* UI_NOTIFICATION_H */
