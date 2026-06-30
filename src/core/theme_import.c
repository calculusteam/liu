/*
 * Liu - Theme import (theme color-file and YAML parsers)
 */
#include "core/theme_import.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>
#include <dirent.h>

/* Global user theme storage */
Theme g_user_themes[MAX_USER_THEMES];
i32   g_user_theme_count = 0;

/* =========================================================================
 * Helpers
 * ========================================================================= */

static const char *theme_user_dir_internal(void) {
    static char dir[1024] = {0};
    if (dir[0]) return dir;

    snprintf(dir, sizeof(dir), "%s/themes", config_user_dir());
    return dir;
}

const char *theme_user_dir(void) {
    return theme_user_dir_internal();
}

/* Derive UI chrome colors from terminal bg */
static void theme_derive_chrome(Theme *t) {
    f32 br = t->bg.r, bg = t->bg.g, bb = t->bg.b;
    bool is_dark = (br + bg + bb) / 3.0f < 0.5f;

    if (is_dark) {
        /* Dark theme: chrome is slightly lighter than bg */
        t->tab_bg         = (Color){ br * 0.8f, bg * 0.8f, bb * 0.8f, 1.0f };
        t->tab_active_bg  = (Color){ br * 1.5f + 0.05f, bg * 1.5f + 0.05f, bb * 1.5f + 0.05f, 1.0f };
        t->tab_active_fg  = t->fg;
        t->tab_inactive_bg = (Color){ br * 1.1f, bg * 1.1f, bb * 1.1f, 1.0f };
        t->tab_inactive_fg = (Color){ t->fg.r * 0.6f, t->fg.g * 0.6f, t->fg.b * 0.6f, 1.0f };
        t->sidebar_bg     = (Color){ br * 0.7f, bg * 0.7f, bb * 0.7f, 1.0f };
        t->sidebar_fg     = (Color){ t->fg.r * 0.85f, t->fg.g * 0.85f, t->fg.b * 0.85f, 1.0f };
        t->sidebar_hover  = (Color){ br * 1.3f, bg * 1.3f, bb * 1.3f, 1.0f };
        t->sidebar_active = (Color){ br * 1.6f + 0.05f, bg * 1.6f + 0.05f, bb * 1.6f + 0.1f, 1.0f };
        t->border         = (Color){ br * 1.5f + 0.02f, bg * 1.5f + 0.02f, bb * 1.5f + 0.02f, 1.0f };
        t->scrollbar      = (Color){ br * 1.2f, bg * 1.2f, bb * 1.2f, 1.0f };
        t->scrollbar_thumb = (Color){ br * 2.5f + 0.1f, bg * 2.5f + 0.1f, bb * 2.5f + 0.1f, 1.0f };
        t->status_bg      = (Color){ br * 1.1f, bg * 1.1f, bb * 1.1f, 1.0f };
        t->status_fg      = (Color){ t->fg.r * 0.65f, t->fg.g * 0.65f, t->fg.b * 0.65f, 1.0f };
    } else {
        /* Light theme: chrome is slightly darker than bg */
        t->tab_bg         = (Color){ br * 0.93f, bg * 0.93f, bb * 0.93f, 1.0f };
        t->tab_active_bg  = t->bg;
        t->tab_active_fg  = t->fg;
        t->tab_inactive_bg = (Color){ br * 0.90f, bg * 0.90f, bb * 0.90f, 1.0f };
        t->tab_inactive_fg = (Color){ t->fg.r * 0.6f + 0.2f, t->fg.g * 0.6f + 0.2f, t->fg.b * 0.6f + 0.2f, 1.0f };
        t->sidebar_bg     = (Color){ br * 0.95f, bg * 0.95f, bb * 0.95f, 1.0f };
        t->sidebar_fg     = (Color){ t->fg.r * 0.9f, t->fg.g * 0.9f, t->fg.b * 0.9f, 1.0f };
        t->sidebar_hover  = (Color){ br * 0.88f, bg * 0.88f, bb * 0.88f, 1.0f };
        t->sidebar_active = (Color){ 0.70f, 0.82f, 0.95f, 1.0f };
        t->border         = (Color){ br * 0.82f, bg * 0.82f, bb * 0.82f, 1.0f };
        t->scrollbar      = (Color){ br * 0.90f, bg * 0.90f, bb * 0.90f, 1.0f };
        t->scrollbar_thumb = (Color){ br * 0.70f, bg * 0.70f, bb * 0.70f, 1.0f };
        t->status_bg      = (Color){ br * 0.93f, bg * 0.93f, bb * 0.93f, 1.0f };
        t->status_fg      = (Color){ t->fg.r * 0.5f + 0.2f, t->fg.g * 0.5f + 0.2f, t->fg.b * 0.5f + 0.2f, 1.0f };
    }

    /* Derived tab text is fg-on-(bg+delta); fine for most palettes but cheap
     * to harden against edge cases. */
    theme_enforce_tab_text_legibility(t);
}

/* Clamp color component to [0,1] */
static f32 clampf(f32 v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Parse hex color string "#RRGGBB" or "0xRRGGBB" to Color */
static bool parse_hex_color(const char *s, Color *out) {
    /* Skip whitespace and quotes */
    while (*s && (isspace((unsigned char)*s) || *s == '\'' || *s == '"')) s++;
    if (*s == '#') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;

    u32 val = 0;
    i32 digits = 0;
    for (i32 i = 0; i < 6 && s[i]; i++) {
        char c = s[i];
        u32 nibble;
        if (c >= '0' && c <= '9') nibble = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') nibble = (u32)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') nibble = (u32)(c - 'A') + 10;
        else break;
        val = (val << 4) | nibble;
        digits++;
    }
    if (digits != 6) return false;

    out->r = (f32)((val >> 16) & 0xFF) / 255.0f;
    out->g = (f32)((val >>  8) & 0xFF) / 255.0f;
    out->b = (f32)((val      ) & 0xFF) / 255.0f;
    out->a = 1.0f;
    return true;
}

/* =========================================================================
 * Theme color-file parser (XML plist)
 * ========================================================================= */

/* Find next occurrence of needle in haystack, return pointer or NULL */
static const char *find_str(const char *haystack, const char *needle) {
    return strstr(haystack, needle);
}

/* Extract <real> value after a <key>Component Name</key> pattern.
 * Bounded to `end` so we never pull a <real> from a following color entry
 * when the current key has no value of its own. */
static f32 extract_real_after(const char *pos, const char *end) {
    const char *real = find_str(pos, "<real>");
    if (!real || (end && real >= end)) return 0.0f;
    return (f32)atof(real + 6);
}

/* Parse iTerm color dict: expects pos pointing just past the color key.
 * Looks for Red/Green/Blue Component <real> values. */
static bool parse_iterm_color_dict(const char *pos, const char *end, Color *out) {
    /* Find the <dict> that follows */
    const char *dict_start = find_str(pos, "<dict>");
    if (!dict_start || dict_start >= end) return false;

    const char *dict_end = find_str(dict_start, "</dict>");
    if (!dict_end || dict_end >= end) return false;

    out->r = 0; out->g = 0; out->b = 0; out->a = 1.0f;

    const char *p = dict_start;
    while (p < dict_end) {
        const char *key = find_str(p, "<key>");
        if (!key || key >= dict_end) break;
        const char *key_end = find_str(key, "</key>");
        if (!key_end || key_end >= dict_end) break;

        /* Extract key name */
        const char *kname = key + 5;
        i32 klen = (i32)(key_end - kname);

        /* Bound the <real> search to the current dict so a missing value
         * doesn't grab the next color entry's component. */
        f32 val = extract_real_after(key_end, dict_end);

        if (klen >= 13 && strncmp(kname, "Red Component", 13) == 0) {
            out->r = clampf(val);
        } else if (klen >= 15 && strncmp(kname, "Green Component", 15) == 0) {
            out->g = clampf(val);
        } else if (klen >= 14 && strncmp(kname, "Blue Component", 14) == 0) {
            out->b = clampf(val);
        } else if (klen >= 15 && strncmp(kname, "Alpha Component", 15) == 0) {
            out->a = clampf(val);
        }

        p = key_end + 6;
    }
    return true;
}

bool theme_import_itermcolors(const char *filepath, Theme *out) {
    FILE *f = fopen(filepath, "r");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);

    char *data = (char *)malloc((usize)sz + 1);
    if (!data) { fclose(f); return false; }
    /* Bail on a short read so we never parse an uninitialized tail. */
    if (fread(data, 1, (usize)sz, f) != (usize)sz) {
        free(data); fclose(f); return false;
    }
    data[sz] = '\0';
    fclose(f);

    memset(out, 0, sizeof(*out));

    /* Extract theme name from filename */
    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;
    snprintf(out->name, sizeof(out->name), "%s", base);
    /* Remove .itermcolors extension */
    char *dot = strrchr(out->name, '.');
    if (dot) *dot = '\0';

    /* Default colors in case some are missing */
    out->fg = (Color){ 0.9f, 0.9f, 0.9f, 1.0f };
    out->bg = (Color){ 0.1f, 0.1f, 0.1f, 1.0f };
    out->cursor = (Color){ 0.8f, 0.8f, 0.8f, 1.0f };
    out->selection = (Color){ 0.3f, 0.4f, 0.6f, 0.6f };

    const char *end = data + sz;
    const char *p = data;

    /* iTerm key names for ANSI colors */
    static const char *ansi_keys[] = {
        "Ansi 0 Color", "Ansi 1 Color", "Ansi 2 Color", "Ansi 3 Color",
        "Ansi 4 Color", "Ansi 5 Color", "Ansi 6 Color", "Ansi 7 Color",
        "Ansi 8 Color", "Ansi 9 Color", "Ansi 10 Color", "Ansi 11 Color",
        "Ansi 12 Color", "Ansi 13 Color", "Ansi 14 Color", "Ansi 15 Color",
    };

    while (p < end) {
        const char *key = find_str(p, "<key>");
        if (!key) break;
        const char *key_end = find_str(key, "</key>");
        if (!key_end) break;

        const char *kname = key + 5;
        i32 klen = (i32)(key_end - kname);

        /* Check ANSI colors */
        for (i32 i = 0; i < 16; i++) {
            if (klen == (i32)strlen(ansi_keys[i]) &&
                strncmp(kname, ansi_keys[i], (usize)klen) == 0) {
                parse_iterm_color_dict(key_end, end, &out->ansi[i]);
            }
        }

        /* Named colors */
        if (klen == 16 && strncmp(kname, "Foreground Color", 16) == 0) {
            parse_iterm_color_dict(key_end, end, &out->fg);
        } else if (klen == 16 && strncmp(kname, "Background Color", 16) == 0) {
            parse_iterm_color_dict(key_end, end, &out->bg);
        } else if (klen == 12 && strncmp(kname, "Cursor Color", 12) == 0) {
            parse_iterm_color_dict(key_end, end, &out->cursor);
        } else if (klen == 15 && strncmp(kname, "Selection Color", 15) == 0) {
            /* "Selection Color" is 15 bytes; the old klen==20 guard never
             * matched, so the selection color silently fell back to default. */
            Color sel;
            if (parse_iterm_color_dict(key_end, end, &sel)) {
                out->selection = sel;
                out->selection.a = 0.6f; /* ensure partial transparency */
            }
        } else if (klen == 19 && strncmp(kname, "Selected Text Color", 19) == 0) {
            /* Ignore, use foreground */
        }

        p = key_end + 6;
    }

    /* Derive UI chrome colors */
    theme_derive_chrome(out);

    free(data);
    return true;
}

/* =========================================================================
 * YAML theme parser
 * ========================================================================= */

/* Skip whitespace, return pointer to first non-whitespace char */
static const char *skip_ws(const char *s) {
    while (*s && isspace((unsigned char)*s) && *s != '\n') s++;
    return s;
}

/* Get the indentation level of a line */
static i32 get_indent(const char *line) {
    i32 n = 0;
    while (line[n] == ' ') n++;
    return n;
}

/* Find the value after "key:" on a line, return pointer to value or NULL */
static const char *yaml_get_value(const char *line, const char *key) {
    const char *s = skip_ws(line);
    usize klen = strlen(key);
    if (strncmp(s, key, klen) == 0 && s[klen] == ':') {
        return skip_ws(s + klen + 1);
    }
    return NULL;
}

bool theme_import_alacritty(const char *filepath, Theme *out) {
    FILE *f = fopen(filepath, "r");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);

    char *data = (char *)malloc((usize)sz + 1);
    if (!data) { fclose(f); return false; }
    /* Bail on a short read so we never parse an uninitialized tail. */
    if (fread(data, 1, (usize)sz, f) != (usize)sz) {
        free(data); fclose(f); return false;
    }
    data[sz] = '\0';
    fclose(f);

    memset(out, 0, sizeof(*out));

    /* Extract theme name from filename */
    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;
    snprintf(out->name, sizeof(out->name), "%s", base);
    /* Remove extension */
    char *dot = strrchr(out->name, '.');
    if (dot) *dot = '\0';

    /* Default colors */
    out->fg = (Color){ 0.9f, 0.9f, 0.9f, 1.0f };
    out->bg = (Color){ 0.1f, 0.1f, 0.1f, 1.0f };
    out->cursor = (Color){ 0.8f, 0.8f, 0.8f, 1.0f };
    out->selection = (Color){ 0.3f, 0.4f, 0.6f, 0.6f };

    /* Parse line by line to find color sections */
    typedef enum {
        SEC_NONE = 0,
        SEC_COLORS,
        SEC_PRIMARY,
        SEC_NORMAL,
        SEC_BRIGHT,
        SEC_CURSOR_SEC,
        SEC_SELECTION_SEC,
    } Section;

    Section section = SEC_NONE;
    i32 colors_indent = -1;
    i32 sub_indent = -1;

    /* ANSI color names in Alacritty order */
    static const char *color_names[] = {
        "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"
    };

    char *line = data;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        i32 indent = get_indent(line);
        const char *trimmed = skip_ws(line);

        /* Skip empty lines and comments */
        if (*trimmed == '\0' || *trimmed == '#') {
            line = nl ? nl + 1 : NULL;
            continue;
        }

        /* Top-level: find "colors:" */
        if (indent == 0 || (colors_indent < 0 && strncmp(trimmed, "colors:", 7) == 0)) {
            if (strncmp(trimmed, "colors:", 7) == 0) {
                section = SEC_COLORS;
                colors_indent = indent;
                sub_indent = -1;
            } else if (section != SEC_NONE && indent <= colors_indent) {
                section = SEC_NONE; /* left colors block */
            }
            line = nl ? nl + 1 : NULL;
            continue;
        }

        /* Inside colors: block */
        if (section >= SEC_COLORS) {
            if (strncmp(trimmed, "primary:", 8) == 0) {
                section = SEC_PRIMARY; sub_indent = indent;
            } else if (strncmp(trimmed, "normal:", 7) == 0) {
                section = SEC_NORMAL; sub_indent = indent;
            } else if (strncmp(trimmed, "bright:", 7) == 0) {
                section = SEC_BRIGHT; sub_indent = indent;
            } else if (strncmp(trimmed, "cursor:", 7) == 0) {
                section = SEC_CURSOR_SEC; sub_indent = indent;
            } else if (strncmp(trimmed, "selection:", 10) == 0) {
                section = SEC_SELECTION_SEC; sub_indent = indent;
            } else if (indent > colors_indent && sub_indent >= 0 && indent > sub_indent) {
                /* We are inside a subsection, parse key: value */
                const char *val;

                if (section == SEC_PRIMARY) {
                    val = yaml_get_value(trimmed, "background");
                    if (val) parse_hex_color(val, &out->bg);
                    val = yaml_get_value(trimmed, "foreground");
                    if (val) parse_hex_color(val, &out->fg);
                } else if (section == SEC_NORMAL || section == SEC_BRIGHT) {
                    i32 offset = (section == SEC_BRIGHT) ? 8 : 0;
                    for (i32 i = 0; i < 8; i++) {
                        val = yaml_get_value(trimmed, color_names[i]);
                        if (val) parse_hex_color(val, &out->ansi[offset + i]);
                    }
                } else if (section == SEC_CURSOR_SEC) {
                    val = yaml_get_value(trimmed, "cursor");
                    if (val) parse_hex_color(val, &out->cursor);
                    val = yaml_get_value(trimmed, "text");
                    if (val) { /* cursor text color, not directly mapped */ }
                } else if (section == SEC_SELECTION_SEC) {
                    val = yaml_get_value(trimmed, "background");
                    if (val) {
                        parse_hex_color(val, &out->selection);
                        out->selection.a = 0.6f;
                    }
                }
            } else if (indent <= (sub_indent >= 0 ? sub_indent : colors_indent + 1)) {
                /* Possibly a new subsection at the same level */
                sub_indent = -1;
                /* Re-check this line for section headers */
                if (strncmp(trimmed, "primary:", 8) == 0) {
                    section = SEC_PRIMARY; sub_indent = indent;
                } else if (strncmp(trimmed, "normal:", 7) == 0) {
                    section = SEC_NORMAL; sub_indent = indent;
                } else if (strncmp(trimmed, "bright:", 7) == 0) {
                    section = SEC_BRIGHT; sub_indent = indent;
                } else if (strncmp(trimmed, "cursor:", 7) == 0) {
                    section = SEC_CURSOR_SEC; sub_indent = indent;
                } else if (strncmp(trimmed, "selection:", 10) == 0) {
                    section = SEC_SELECTION_SEC; sub_indent = indent;
                }
            }
        }

        line = nl ? nl + 1 : NULL;
    }

    /* Derive UI chrome colors */
    theme_derive_chrome(out);

    free(data);
    return true;
}

/* =========================================================================
 * Auto-detect and import
 * ========================================================================= */

bool theme_import_file(const char *filepath, Theme *out) {
    if (!filepath || !out) return false;

    /* Detect by extension */
    const char *ext = strrchr(filepath, '.');
    if (ext) {
        if (strcmp(ext, ".itermcolors") == 0) {
            return theme_import_itermcolors(filepath, out);
        }
        if (strcmp(ext, ".yml") == 0 || strcmp(ext, ".yaml") == 0 ||
            strcmp(ext, ".toml") == 0) {
            return theme_import_alacritty(filepath, out);
        }
    }

    /* Try iTerm first (XML detection) */
    FILE *f = fopen(filepath, "r");
    if (!f) return false;
    char header[256];
    usize n = fread(header, 1, sizeof(header) - 1, f);
    header[n] = '\0';
    fclose(f);

    if (strstr(header, "<?xml") || strstr(header, "<plist") || strstr(header, "<dict>")) {
        return theme_import_itermcolors(filepath, out);
    }

    /* Fall back to Alacritty YAML */
    return theme_import_alacritty(filepath, out);
}

/* =========================================================================
 * Save/load user themes
 * ========================================================================= */

static void write_color_json(FILE *f, const char *name, Color c, bool last) {
    fprintf(f, "    \"%s\": [%.4f, %.4f, %.4f, %.4f]%s\n",
            name, c.r, c.g, c.b, c.a, last ? "" : ",");
}

bool theme_save_user(const Theme *theme) {
    const char *dir = theme_user_dir();

    /* Create parent ~/.config/Liu/ first, then the themes dir itself. */
    char parent[1024];
    snprintf(parent, sizeof(parent), "%s", dir);
    char *sl = strrchr(parent, '/');
    if (sl) { *sl = '\0'; mkdir(parent, 0755); }
    mkdir(dir, 0755);

    char path[1280];
    snprintf(path, sizeof(path), "%s/%s.json", dir, theme->name);

    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "{\n");
    fprintf(f, "    \"name\": \"%s\",\n", theme->name);
    write_color_json(f, "fg", theme->fg, false);
    write_color_json(f, "bg", theme->bg, false);
    write_color_json(f, "cursor", theme->cursor, false);
    write_color_json(f, "selection", theme->selection, false);
    for (i32 i = 0; i < 16; i++) {
        char key[16];
        snprintf(key, sizeof(key), "ansi_%d", i);
        write_color_json(f, key, theme->ansi[i], false);
    }
    write_color_json(f, "tab_bg", theme->tab_bg, false);
    write_color_json(f, "tab_active_bg", theme->tab_active_bg, false);
    write_color_json(f, "tab_active_fg", theme->tab_active_fg, false);
    write_color_json(f, "tab_inactive_bg", theme->tab_inactive_bg, false);
    write_color_json(f, "tab_inactive_fg", theme->tab_inactive_fg, false);
    write_color_json(f, "sidebar_bg", theme->sidebar_bg, false);
    write_color_json(f, "sidebar_fg", theme->sidebar_fg, false);
    write_color_json(f, "sidebar_hover", theme->sidebar_hover, false);
    write_color_json(f, "sidebar_active", theme->sidebar_active, false);
    write_color_json(f, "border", theme->border, false);
    write_color_json(f, "scrollbar", theme->scrollbar, false);
    write_color_json(f, "scrollbar_thumb", theme->scrollbar_thumb, false);
    write_color_json(f, "status_bg", theme->status_bg, false);
    /* status_fg used to be the trailing key; with optional style
     * overrides it can no longer claim that slot unconditionally. We
     * always emit the override block even when zero — JSON parsers are
     * tolerant of "extra" zero fields, and it keeps the writer simple. */
    write_color_json(f, "status_fg", theme->status_fg, false);
    /* Optional UI chrome accent. Zero RGBA means "use ansi_12 at
     * runtime" — we still emit it so the export round-trips cleanly. */
    write_color_json(f, "ui_accent", theme->ui_accent, false);
    fprintf(f, "    \"opacity\": %.4f,\n",
            theme->opacity_override);
    fprintf(f, "    \"cursor_style\": %d,\n",
            (int)theme->cursor_style_override);
    fprintf(f, "    \"cursor_blink\": %d,\n",
            (int)theme->cursor_blink_override);
    fprintf(f, "    \"bold_is_bright\": %d\n",
            (int)theme->bold_is_bright_override);
    fprintf(f, "}\n");
    fclose(f);
    return true;
}

/* Parse a color JSON array [r, g, b, a] */
static bool parse_color_array(const char *line, const char *key, Color *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(line, search);
    if (!pos) return false;

    pos = strchr(pos, '[');
    if (!pos) return false;
    pos++;

    f32 vals[4] = {0, 0, 0, 1.0f};
    for (i32 i = 0; i < 4; i++) {
        while (*pos && (isspace((unsigned char)*pos) || *pos == ',')) pos++;
        if (!*pos || *pos == ']') break;
        vals[i] = (f32)atof(pos);
        while (*pos && *pos != ',' && *pos != ']') pos++;
    }

    out->r = vals[0]; out->g = vals[1]; out->b = vals[2]; out->a = vals[3];
    return true;
}

/* =========================================================================
 * Tab-text legibility guard
 *
 * A user/AI theme JSON can omit tab_active_fg / tab_inactive_fg (the zero-init
 * parse leaves them {0,0,0,0}) or pick a colour that collides with the tab
 * background — either way the tab title goes invisible. Guarantee both colours
 * are present and clear the contrast floors the renderer needs, mirroring
 * THEMING.md's "Required color-pair contrast" and the actual paint surfaces in
 * src/ui/ui.c render_toolbar():
 *   active   text sits on the pill  max(tab_active_bg, bg+0.06)  → ≥ 4.5:1
 *   inactive text sits on the strip  bg                          → ≥ 3.0:1
 *
 * Kept self-contained (no dependency on ui/chrome_palette.h) so the core layer
 * stays below the ui layer; the sRGB-luminance maths matches chrome_palette.c.
 * ========================================================================= */
static f32 theme_lin_channel(f32 c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
static f32 theme_color_luminance(Color c) {
    return 0.2126f * theme_lin_channel(c.r)
         + 0.7152f * theme_lin_channel(c.g)
         + 0.0722f * theme_lin_channel(c.b);
}
static f32 theme_contrast(Color a, Color b) {
    f32 la = theme_color_luminance(a), lb = theme_color_luminance(b);
    f32 hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05f) / (lo + 0.05f);
}
static Color theme_legible_on(Color bg) {
    return theme_color_luminance(bg) < 0.179f
        ? (Color){ 1.0f, 1.0f, 1.0f, 1.0f }
        : (Color){ 0.0f, 0.0f, 0.0f, 1.0f };
}

/* Replace `*fg` with a legible colour when it is missing (omitted key →
 * alpha 0) or fails the contrast floor against `bg`. `dim` mutes the result
 * toward `bg` for inactive tabs, but only while it still clears the floor. */
static void theme_fix_tab_fg(Color *fg, Color bg, f32 floor, bool dim) {
    bool missing = fg->a < 0.5f;   /* omitted key → zero-init → a == 0 */
    if (!missing && theme_contrast(*fg, bg) >= floor) return;

    Color leg = theme_legible_on(bg);
    if (dim) {
        Color muted = { leg.r * 0.7f + bg.r * 0.3f,
                        leg.g * 0.7f + bg.g * 0.3f,
                        leg.b * 0.7f + bg.b * 0.3f, 1.0f };
        *fg = (theme_contrast(muted, bg) >= floor) ? muted : leg;
    } else {
        *fg = leg;
    }
}

void theme_enforce_tab_text_legibility(Theme *t) {
    if (!t) return;
    Color active_bg = {
        fmaxf(t->tab_active_bg.r, t->bg.r + 0.06f),
        fmaxf(t->tab_active_bg.g, t->bg.g + 0.06f),
        fmaxf(t->tab_active_bg.b, t->bg.b + 0.06f),
        1.0f
    };
    theme_fix_tab_fg(&t->tab_active_fg,   active_bg, 4.5f, false);
    theme_fix_tab_fg(&t->tab_inactive_fg, t->bg,     3.0f, true);
}

static bool load_user_theme_json(const char *filepath, Theme *out) {
    FILE *f = fopen(filepath, "r");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 256 * 1024) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);

    char *data = (char *)malloc((usize)sz + 1);
    if (!data) { fclose(f); return false; }
    /* Bail on a short read so we never parse an uninitialized tail. */
    if (fread(data, 1, (usize)sz, f) != (usize)sz) {
        free(data); fclose(f); return false;
    }
    data[sz] = '\0';
    fclose(f);

    memset(out, 0, sizeof(*out));

    /* Extract name */
    const char *name_key = strstr(data, "\"name\"");
    if (name_key) {
        const char *colon = strchr(name_key + 6, ':');
        if (colon) {
            const char *q1 = strchr(colon, '"');
            if (q1) {
                q1++;
                const char *q2 = strchr(q1, '"');
                if (q2) {
                    i32 nlen = (i32)(q2 - q1);
                    if (nlen > 63) nlen = 63;
                    memcpy(out->name, q1, (usize)nlen);
                    out->name[nlen] = '\0';
                }
            }
        }
    }

    if (out->name[0] == '\0') {
        /* Fallback: use filename */
        const char *base = strrchr(filepath, '/');
        base = base ? base + 1 : filepath;
        snprintf(out->name, sizeof(out->name), "%s", base);
        char *dot = strrchr(out->name, '.');
        if (dot) *dot = '\0';
    }

    /* Parse colors */
    parse_color_array(data, "fg", &out->fg);
    parse_color_array(data, "bg", &out->bg);
    parse_color_array(data, "cursor", &out->cursor);
    parse_color_array(data, "selection", &out->selection);

    for (i32 i = 0; i < 16; i++) {
        char key[16];
        snprintf(key, sizeof(key), "ansi_%d", i);
        parse_color_array(data, key, &out->ansi[i]);
    }

    parse_color_array(data, "tab_bg", &out->tab_bg);
    parse_color_array(data, "tab_active_bg", &out->tab_active_bg);
    parse_color_array(data, "tab_active_fg", &out->tab_active_fg);
    parse_color_array(data, "tab_inactive_bg", &out->tab_inactive_bg);
    parse_color_array(data, "tab_inactive_fg", &out->tab_inactive_fg);
    parse_color_array(data, "sidebar_bg", &out->sidebar_bg);
    parse_color_array(data, "sidebar_fg", &out->sidebar_fg);
    parse_color_array(data, "sidebar_hover", &out->sidebar_hover);
    parse_color_array(data, "sidebar_active", &out->sidebar_active);
    parse_color_array(data, "border", &out->border);
    parse_color_array(data, "scrollbar", &out->scrollbar);
    parse_color_array(data, "scrollbar_thumb", &out->scrollbar_thumb);
    parse_color_array(data, "status_bg", &out->status_bg);
    parse_color_array(data, "status_fg", &out->status_fg);
    /* Optional UI chrome accent. Missing key leaves alpha at 0, which
     * theme_ui_accent() reads as "fall back to ansi[12]". */
    parse_color_array(data, "ui_accent", &out->ui_accent);

    /* Tab title text is never allowed to vanish — fill/repair tab_active_fg and
     * tab_inactive_fg if the JSON omitted them (zero-init) or chose a colour
     * that collides with the tab background. */
    theme_enforce_tab_text_legibility(out);

    /* Optional style overrides — parse a single scalar after the key.
     * Missing keys leave the field at zero (= no override). */
    {
        struct { const char *k; bool is_int; void *out; } scalars[] = {
            { "opacity",         false, &out->opacity_override         },
            { "cursor_style",    true,  &out->cursor_style_override    },
            { "cursor_blink",    true,  &out->cursor_blink_override    },
            { "bold_is_bright",  true,  &out->bold_is_bright_override  },
        };
        for (i32 i = 0; i < (i32)(sizeof scalars / sizeof scalars[0]); i++) {
            char search[64];
            snprintf(search, sizeof(search), "\"%s\"", scalars[i].k);
            /* Walk every match until we find one that's an actual top-level
             * key (preceded by a JSON structural char, followed by `:` and
             * a scalar). Without this, a theme name like "My opacity test"
             * or any string containing the key would match. */
            const char *cursor = data;
            const char *pos = NULL;
            for (;;) {
                const char *m = strstr(cursor, search);
                if (!m) break;
                bool prev_ok = (m == data) || m[-1] == '\n' || m[-1] == ',' ||
                               m[-1] == '{' || m[-1] == ' ' || m[-1] == '\t';
                if (prev_ok) {
                    const char *colon = strchr(m, ':');
                    if (colon && colon - m < 32) {
                        const char *p = colon + 1;
                        while (*p && isspace((unsigned char)*p)) p++;
                        /* Skip array values — those are color keys (e.g.
                         * the user theme has both `"opacity"` and a string
                         * containing it, but never `"opacity": [...]`). */
                        if (*p && *p != '[' && *p != '"' && *p != '{') {
                            pos = p;
                            break;
                        }
                    }
                }
                cursor = m + strlen(search);
            }
            if (!pos) continue;
            if (scalars[i].is_int) {
                char *endp = NULL;
                long v = strtol(pos, &endp, 10);
                if (endp != pos) *(i8 *)scalars[i].out = (i8)v;
            } else {
                char *endp = NULL;
                double v = strtod(pos, &endp);
                if (endp != pos) *(f32 *)scalars[i].out = (f32)v;
            }
        }
    }

    free(data);
    return true;
}

void theme_load_user_themes(void) {
    g_user_theme_count = 0;
    const char *dir = theme_user_dir();

    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_user_theme_count < MAX_USER_THEMES) {
        const char *ext = strrchr(ent->d_name, '.');
        if (!ext || strcmp(ext, ".json") != 0) continue;

        char path[1280];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        if (load_user_theme_json(path, &g_user_themes[g_user_theme_count])) {
            g_user_theme_count++;
        }
    }
    closedir(d);
}

bool theme_delete_user(const char *name) {
    if (!name || !*name) return false;

    /* Only operate on themes that are present in the loaded user-themes
     * cache. Built-ins live in static const storage and are not in
     * g_user_themes, so this naturally refuses to delete them. */
    bool found = false;
    for (i32 i = 0; i < g_user_theme_count; i++) {
        if (strcmp(g_user_themes[i].name, name) == 0) { found = true; break; }
    }
    if (!found) return false;

    const char *dir = theme_user_dir();

    /* Primary: delete the canonical "<name>.json" path that theme_save_user
     * writes. If the user installed the theme by hand under a different
     * filename, walk the dir and match by parsed JSON "name" field as a
     * fallback so we still find it. */
    char path[1280];
    snprintf(path, sizeof(path), "%s/%s.json", dir, name);
    bool removed = (remove(path) == 0);

    if (!removed) {
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                const char *ext = strrchr(ent->d_name, '.');
                if (!ext || strcmp(ext, ".json") != 0) continue;
                char candidate[1280];
                snprintf(candidate, sizeof(candidate), "%s/%s", dir, ent->d_name);
                Theme probe = {0};
                if (load_user_theme_json(candidate, &probe) &&
                    strcmp(probe.name, name) == 0) {
                    removed = (remove(candidate) == 0);
                    break;
                }
            }
            closedir(d);
        }
    }

    /* Refresh the in-memory cache regardless — even if removal failed we
     * want the live state to reflect what's actually on disk. */
    theme_load_user_themes();
    return removed;
}
