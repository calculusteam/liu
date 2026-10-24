/*
 * Liu — effective-settings snapshot (see effective_settings.h).
 */
#include "ui/effective_settings.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* JSON string escape. Paths carry backslashes and quotes on real machines;
 * a raw byte below 0x20 would make the whole file unreadable to the harness. */
static usize es_str(char *out, usize cap, const char *s) {
    usize n = 0;
    if (cap < 3) return 0;
    out[n++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        const char *rep = NULL;
        char buf[8];
        switch (c) {
        case '"':  rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\n': rep = "\\n";  break;
        case '\r': rep = "\\r";  break;
        case '\t': rep = "\\t";  break;
        default:
            if (c < 0x20) { snprintf(buf, sizeof buf, "\\u%04x", c); rep = buf; }
            break;
        }
        usize len = rep ? strlen(rep) : 1;
        if (n + len + 2 > cap) return 0;          /* room for the closing quote */
        if (rep) memcpy(out + n, rep, len); else out[n] = (char)c;
        n += len;
    }
    out[n++] = '"';
    out[n] = '\0';
    return n;
}

/* Append one `"key": value,\n` line; `value` is already JSON. */
static bool es_line(char *out, usize cap, usize *len, const char *key,
                    const char *value, bool last) {
    int w = snprintf(out + *len, cap > *len ? cap - *len : 0,
                     "  \"%s\": %s%s\n", key, value, last ? "" : ",");
    if (w < 0 || (usize)w >= cap - *len) return false;
    *len += (usize)w;
    return true;
}

usize effective_settings_format(const EffectiveSettings *s, char *out, usize cap) {
    if (!s || !out || cap < 4) return 0;
    usize len = 0;
    char v[1200];
    out[0] = '\0';
    if (cap < 3) return 0;
    out[len++] = '{'; out[len++] = '\n'; out[len] = '\0';

#define ES_STR(key, field) \
    do { if (!es_str(v, sizeof v, s->field) || !es_line(out, cap, &len, key, v, false)) return 0; } while (0)
#define ES_NUM(key, fmt, expr) \
    do { snprintf(v, sizeof v, fmt, expr); if (!es_line(out, cap, &len, key, v, false)) return 0; } while (0)
#define ES_BOOL(key, expr) \
    do { if (!es_line(out, cap, &len, key, (expr) ? "true" : "false", false)) return 0; } while (0)

    ES_STR ("font_path", font_path);
    ES_NUM ("font_size", "%.3f", (f64)s->font_size);
    ES_NUM ("font_weight", "%.3f", (f64)s->font_weight);
    ES_NUM ("line_height", "%.3f", (f64)s->line_height);
    ES_BOOL("font_normalize", s->font_normalize);
    ES_STR ("theme", theme);
    ES_NUM ("dpi_scale", "%.4f", (f64)s->dpi_scale);
    ES_NUM ("cell_w_px", "%.3f", (f64)s->cell_w_px);
    ES_NUM ("cell_h_px", "%.3f", (f64)s->cell_h_px);
    ES_NUM ("grid_cols", "%d", s->grid_cols);
    ES_NUM ("grid_rows", "%d", s->grid_rows);
    ES_NUM ("window_w_pt", "%d", s->window_w_pt);
    ES_NUM ("window_h_pt", "%d", s->window_h_pt);
    ES_NUM ("fb_w_px", "%d", s->fb_w_px);
    ES_NUM ("fb_h_px", "%d", s->fb_h_px);
    ES_NUM ("panel_hz", "%.2f", (f64)s->panel_hz);
    ES_BOOL("cursor_blink", s->cursor_blink);
    ES_NUM ("opacity", "%.4f", (f64)s->opacity);
    ES_BOOL("background_blur", s->background_blur);
    ES_NUM ("background_blur_radius", "%.3f", (f64)s->background_blur_radius);
    ES_STR ("backend", backend);
    ES_STR ("build", build);
    ES_BOOL("e2e_probe", s->e2e_probe);
    ES_STR ("config_dir", config_dir);
    /* Last line: no trailing comma, or the file is not JSON. */
    snprintf(v, sizeof v, "%d", s->presents_before);
    if (!es_line(out, cap, &len, "presents_before", v, true)) return 0;
#undef ES_STR
#undef ES_NUM
#undef ES_BOOL

    if (len + 3 > cap) return 0;
    out[len++] = '}'; out[len++] = '\n'; out[len] = '\0';
    return len;
}

bool effective_settings_write(const EffectiveSettings *s, const char *path) {
    if (!s || !path || !path[0]) return false;
    char buf[4096];
    usize n = effective_settings_format(s, buf, sizeof buf);
    if (n == 0) return false;
    char tmp[1100];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return false;
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = fwrite(buf, 1, n, f) == n;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}
