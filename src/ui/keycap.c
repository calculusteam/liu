/*
 * Liu — keyboard shortcut indicators (see keycap.h).
 */
#include "ui/keycap.h"
#include "core/utf8.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* The modifier order a keyboard is pressed in and the HIG prints them in. */
enum { KC_CTRL = 0, KC_ALT, KC_SHIFT, KC_SUPER, KC_MOD_COUNT };

static const char *const kModGlyph[KC_MOD_COUNT] = { "\xe2\x8c\x83", "\xe2\x8c\xa5", "\xe2\x87\xa7", "\xe2\x8c\x98" }; /* ⌃ ⌥ ⇧ ⌘ */
static const char *const kModWord[KC_MOD_COUNT]  = { "Ctrl", "Alt", "Shift", "Super" };

bool keycap_platform_glyphs(void) {
#ifdef PLATFORM_MACOS
    return true;
#else
    return false;
#endif
}

/* One token of the accelerator: a run between '+', ' ' or '-' separators, or
 * a single modifier glyph inside a glyph run ("⌘⇧M" has no separators). */
static i32 kc_modifier_of(const char *tok, usize n) {
    struct { const char *s; i32 m; } names[] = {
        { "ctrl", KC_CTRL }, { "control", KC_CTRL }, { "\xe2\x8c\x83", KC_CTRL },
        { "alt", KC_ALT }, { "option", KC_ALT }, { "opt", KC_ALT }, { "\xe2\x8c\xa5", KC_ALT },
        { "shift", KC_SHIFT }, { "\xe2\x87\xa7", KC_SHIFT },
        { "cmd", KC_SUPER }, { "command", KC_SUPER }, { "super", KC_SUPER },
        { "meta", KC_SUPER }, { "win", KC_SUPER }, { "\xe2\x8c\x98", KC_SUPER },
    };
    for (usize i = 0; i < sizeof names / sizeof names[0]; i++) {
        usize l = strlen(names[i].s);
        if (l != n) continue;
        bool eq = true;
        for (usize k = 0; k < n; k++)
            if (tolower((unsigned char)tok[k]) != tolower((unsigned char)names[i].s[k])) { eq = false; break; }
        if (eq) return names[i].m;
    }
    return -1;
}

/* A key name → what its cap shows. Arrows become arrows, the long words
 * become the short ones a keycap has room for, a letter is upper-cased. */
static void kc_key_label(const char *tok, usize n, char *out, usize cap) {
    struct { const char *from; const char *to; } map[] = {
        { "left", "\xe2\x86\x90" }, { "right", "\xe2\x86\x92" },
        { "up", "\xe2\x86\x91" },   { "down", "\xe2\x86\x93" },
        { "return", "Enter" }, { "enter", "Enter" }, { "\xe2\x86\xa9", "Enter" },
        { "escape", "Esc" }, { "esc", "Esc" }, { "\xe2\x8e\x8b", "Esc" },
        { "delete", "Del" }, { "del", "Del" }, { "forwarddelete", "Del" },
        { "backspace", "\xe2\x8c\xab" }, { "\xe2\x8c\xab", "\xe2\x8c\xab" },
        { "tab", "Tab" }, { "space", "Space" }, { "spacebar", "Space" },
        { "pageup", "PgUp" }, { "pagedown", "PgDn" }, { "home", "Home" }, { "end", "End" },
        { "plus", "+" }, { "minus", "-" }, { "equal", "=" }, { "equals", "=" },
        { "comma", "," }, { "period", "." }, { "slash", "/" }, { "backslash", "\\" },
        { "grave", "`" }, { "backtick", "`" }, { "semicolon", ";" }, { "quote", "'" },
        { "bracketleft", "[" }, { "bracketright", "]" },
    };
    for (usize i = 0; i < sizeof map / sizeof map[0]; i++) {
        usize l = strlen(map[i].from);
        if (l != n) continue;
        bool eq = true;
        for (usize k = 0; k < n; k++)
            if (tolower((unsigned char)tok[k]) != tolower((unsigned char)map[i].from[k])) { eq = false; break; }
        if (eq) { snprintf(out, cap, "%s", map[i].to); return; }
    }
    if (n >= cap) n = cap - 1;
    memcpy(out, tok, n);
    out[n] = '\0';
    /* A single ASCII letter is the key's own legend: upper case. "F2", "PgUp"
     * and the rest keep their spelling. */
    if (n == 1 && isalpha((unsigned char)out[0])) out[0] = (char)toupper((unsigned char)out[0]);
}

i32 keycap_split(const char *accel, KeycapKey *out, i32 max, bool glyph_mods) {
    if (!accel || !out || max <= 0) return 0;
    bool mods[KC_MOD_COUNT] = {0};
    char key[16] = {0};
    const char *p = accel;
    while (*p) {
        while (*p == ' ' || *p == '+' || *p == '\t') p++;
        if (!*p) break;
        /* A lone '+' or '-' between separators is a key; in the middle of a
         * word ("Cmd+-") it is the key that follows the separator. */
        const char *start = p;
        usize n;
        u32 cp;
        u32 adv = utf8_decode((const u8 *)p, 4, &cp);
        if (adv == 0) { p++; continue; }
        if (cp >= 0x2300 && cp <= 0x23FF) {
            /* A technical glyph (⌘ ⌥ ⌃ ⌫ ⎋ …): one token on its own, so "⌘⇧M"
             * splits without separators. */
            n = adv;
            p += adv;
        } else if (cp == 0x21E7 || (cp >= 0x2190 && cp <= 0x21FF)) {
            n = adv;                    /* ⇧, arrows, ↩ */
            p += adv;
        } else if (*p == '+' || *p == '-') {
            n = 1;
            p++;
        } else {
            while (*p && *p != ' ' && *p != '+' && *p != '\t') {
                u32 c2;
                u32 a2 = utf8_decode((const u8 *)p, 4, &c2);
                if (a2 == 0) { p++; continue; }
                /* A glyph modifier glued to a word ends the word. */
                if ((c2 >= 0x2300 && c2 <= 0x23FF) || c2 == 0x21E7 || (c2 >= 0x2190 && c2 <= 0x21FF)) break;
                p += a2;
            }
            n = (usize)(p - start);
            /* "Cmd-T": a dash inside a word separates only when both sides
             * are non-empty and the left is a modifier. */
            const char *dash = memchr(start, '-', n);
            if (dash && dash > start && (usize)(dash - start) < n - 1 &&
                kc_modifier_of(start, (usize)(dash - start)) >= 0) {
                n = (usize)(dash - start);
                p = dash + 1;
            }
        }
        i32 m = kc_modifier_of(start, n);
        if (m >= 0) { mods[m] = true; continue; }
        /* The key. A second key word replaces the first — the last one is
         * the one the binding names. */
        kc_key_label(start, n, key, sizeof key);
    }
    i32 count = 0;
    for (i32 m = 0; m < KC_MOD_COUNT && count < max; m++) {
        if (!mods[m]) continue;
        snprintf(out[count].text, sizeof out[count].text, "%s",
                 glyph_mods ? kModGlyph[m] : kModWord[m]);
        out[count].modifier = true;
        count++;
    }
    if (key[0] && count < max) {
        snprintf(out[count].text, sizeof out[count].text, "%s", key);
        out[count].modifier = false;
        count++;
    }
    return count;
}

/* ---- geometry ------------------------------------------------------------ */

static f32 kc_gap(f32 ui_cw) { return floorf(ui_cw * 0.5f + 0.5f); }

static usize kc_cols(const KeycapKey *k) {
    return utf8_display_cols((const u8 *)k->text, strlen(k->text));
}

f32 keycap_row_width(const KeycapKey *keys, i32 n, f32 ui_cw) {
    f32 w = 0.0f;
    for (i32 i = 0; i < n; i++) {
        if (i) w += kc_gap(ui_cw);
        w += (f32)kc_cols(&keys[i]) * ui_cw;
    }
    return w;
}

/* ---- paint ---------------------------------------------------------------- */

/* draw_text_ex lives in ui.c; declared here the way the other chrome modules
 * declare it, rather than pulling ui.h into a leaf module. */
extern void draw_text_ex(Renderer *r, const char *text, f32 x, f32 y, Color fg,
                         i32 max_chars, f32 step);

f32 keycap_draw(Renderer *r, const KeycapKey *keys, i32 n, f32 x, f32 text_y,
                bool align_right, f32 ui_cw, Color base) {
    if (!r || !keys || n <= 0) return 0.0f;
    f32 row_w = keycap_row_width(keys, n, ui_cw);
    f32 cx = align_right ? x - row_w : x;
    for (i32 i = 0; i < n; i++) {
        usize cols = kc_cols(&keys[i]);
        /* The key is what the eye is looking for; the modifiers qualify it. */
        Color fg = base;
        if (keys[i].modifier) fg.a *= KEYCAP_MOD_ALPHA;
        draw_text_ex(r, keys[i].text, floorf(cx + 0.5f), text_y, fg, (i32)cols + 4, ui_cw);
        cx += (f32)cols * ui_cw + kc_gap(ui_cw);
    }
    return row_w;
}

f32 keycap_draw_accel(Renderer *r, const char *accel, f32 x, f32 text_y,
                      bool align_right, f32 ui_cw, Color base) {
    KeycapKey keys[KEYCAP_MAX_KEYS];
    i32 n = keycap_split(accel, keys, KEYCAP_MAX_KEYS, keycap_platform_glyphs());
    if (n == 0) return 0.0f;
    return keycap_draw(r, keys, n, x, text_y, align_right, ui_cw, base);
}

f32 keycap_accel_width(const char *accel, f32 ui_cw) {
    KeycapKey keys[KEYCAP_MAX_KEYS];
    i32 n = keycap_split(accel, keys, KEYCAP_MAX_KEYS, keycap_platform_glyphs());
    return n ? keycap_row_width(keys, n, ui_cw) : 0.0f;
}
