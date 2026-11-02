/*
 * Liu - the key a chord names on any keyboard layout (X11)
 *
 * Liu's chords (Ctrl+Shift+T, Alt+1, Ctrl+, …) and a pane's Ctrl+<letter>
 * are matched by the key the layout's keysym names (platform_linux.c,
 * map_keysym). On a layout whose keys type something else that name is
 * missing, and the chord would do nothing:
 *
 *   - Cyrillic, Greek, Arabic, Hebrew … : the letter keys type their own
 *     script, so Ctrl+Shift+T is Ctrl+Shift+Т, and Ctrl+C in a shell would
 *     send nothing at all;
 *   - French AZERTY: the top row types & é " ' ( - è _ ç à and the digits
 *     need Shift, so Alt+1 is Alt+&.
 *
 * Desktop applications match such a chord by the key's other symbols, and so
 * does Liu: XKB is asked what the SAME physical key carries — first on the
 * other shift level of the active layout (AZERTY's digits), then in the other
 * layouts the keymap holds (the Latin layout a Russian user switches to). No
 * layout is named anywhere: whatever the keymap says is what is used. A
 * keymap with no Latin layout at all has nothing to offer; there the key's
 * physical position is the last resort (the end of this file).
 *
 * Only the base and Shift levels are read: an AltGr symbol is typed with a
 * modifier that was not pressed. Pure C over the symbols the caller read from
 * XKB (XkbKeycodeToKeysym), so the order can be checked without an X server.
 */
#ifndef LIU_PLATFORM_X11_KEYPOS_H
#define LIU_PLATFORM_X11_KEYPOS_H

#include "core/types.h"

#include <string.h>

#define X11_KEYPOS_GROUPS 4   /* XKB holds at most four layouts */
#define X11_KEYPOS_LEVELS 2   /* base and Shift */

/* The symbols of one physical key: syms[group][level], 0 = none. */
typedef struct {
    u32 syms[X11_KEYPOS_GROUPS][X11_KEYPOS_LEVELS];
    i32 groups;          /* layouts the keymap holds for this key (1..4) */
} X11KeySyms;

/* The first of the key's symbols `known` maps to a key (non-zero): the active
 * layout's levels, then every other layout's in order. 0 when none does. */
static inline u32 x11_key_by_position(const X11KeySyms *k, i32 active,
                                      u32 (*known)(u32 keysym)) {
    i32 groups = k->groups;
    if (groups > X11_KEYPOS_GROUPS) groups = X11_KEYPOS_GROUPS;
    if (groups < 1) return 0;
    if (active < 0 || active >= groups) active = 0;
    for (i32 pass = 0; pass < groups; pass++) {
        i32 g = pass == 0 ? active : (pass <= active ? pass - 1 : pass);
        for (i32 l = 0; l < X11_KEYPOS_LEVELS; l++) {
            u32 s = k->syms[g][l];
            if (!s) continue;
            u32 key = known(s);
            if (key) return key;
        }
    }
    return 0;
}

/* ---- The last resort: where the key SITS -----------------------------------
 *
 * A keymap whose every layout is non-Latin (Russian alone, Greek alone, a
 * Russian + Ukrainian pair ...) carries no symbol that names Ctrl+C, Alt+1 or
 * Ctrl+Shift+T on any key, so the walk above finds nothing. What is left is
 * the key's physical position, and a keycode on a PC server IS a position: the
 * Linux input code of the key plus 8 (the `evdev` keycodes of XKB; `xfree86`
 * has the same main block). <linux/input-event-codes.h> names those positions
 * by their US key-cap (KEY_Q, KEY_W ... KEY_SLASH); that naming is what is used
 * here, as ROWS of physical keys and not as a table of layouts: it says
 * nothing about any layout, and it is read only when the keymap itself has no
 * ASCII letter on any key of any layout (x11_keymap_has_latin), so a German ß
 * key or an AZERTY ) key keeps meaning what its layout says. */

/* Does the server number its keys as Linux input codes + 8? `name` is the
 * keymap's keycodes component (XkbGetNames: "evdev", "evdev+aliases(qwerty)",
 * "xfree86+aliases(qwerty)" ...). Any other server numbers keys its own way and
 * has no position to read. */
static inline bool x11_keycodes_are_pc_positions(const char *name) {
    if (!name) return false;
    return strncmp(name, "evdev", 5) == 0 || strncmp(name, "xfree86", 7) == 0;
}

/* The main block's physical keys, row by row: the Linux input code of the first
 * key of the row and the US key-caps from there, one per consecutive code.
 * (The numbers are <linux/input-event-codes.h>'s; the header is not included,
 * macOS has none.) */
typedef struct { u8 first; const char *caps; } X11KeyRow;
static const X11KeyRow x11_key_rows[] = {
    {  2, "1234567890-=" },   /* KEY_1 .. KEY_9, KEY_0, KEY_MINUS, KEY_EQUAL */
    { 16, "qwertyuiop[]" },   /* KEY_Q .. KEY_P, KEY_LEFTBRACE, KEY_RIGHTBRACE */
    { 30, "asdfghjkl;'`" },   /* KEY_A .. KEY_L, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE */
    { 43, "\\zxcvbnm,./" },   /* KEY_BACKSLASH, KEY_Z .. KEY_M, KEY_COMMA, KEY_DOT, KEY_SLASH */
};
#define X11_KEY_ROWS ((i32)(sizeof x11_key_rows / sizeof x11_key_rows[0]))
#define X11_KEYCODE_OFFSET 8   /* an X keycode is the Linux input code + 8 */

/* The US key-cap of the physical key behind X keycode `xkeycode`, as the keysym
 * that key-cap types (an ASCII keysym is its character); 0 for a key outside
 * the main block. */
static inline u32 x11_position_keysym(u32 xkeycode) {
    if (xkeycode < X11_KEYCODE_OFFSET) return 0;
    u32 code = xkeycode - X11_KEYCODE_OFFSET;
    for (i32 r = 0; r < X11_KEY_ROWS; r++) {
        u32 n = (u32)strlen(x11_key_rows[r].caps);
        if (code >= x11_key_rows[r].first && code < (u32)x11_key_rows[r].first + n)
            return (u32)(u8)x11_key_rows[r].caps[code - x11_key_rows[r].first];
    }
    return 0;
}

/* The symbol the keymap holds for a keycode in a group at a level (0 = none):
 * XkbKeycodeToKeysym, in the platform. */
typedef u32 (*X11SymAt)(void *ctx, u32 xkeycode, i32 group, i32 level);

/* Does the keymap type an ASCII letter anywhere: on any key of the main block,
 * in any of its layouts, at the base or Shift level? A Latin layout does; a
 * Russian or a Greek one alone does not. */
static inline bool x11_keymap_has_latin(X11SymAt sym_at, void *ctx) {
    for (i32 r = 0; r < X11_KEY_ROWS; r++) {
        u32 n = (u32)strlen(x11_key_rows[r].caps);
        for (u32 i = 0; i < n; i++) {
            u32 kc = (u32)x11_key_rows[r].first + i + X11_KEYCODE_OFFSET;
            for (i32 g = 0; g < X11_KEYPOS_GROUPS; g++)
                for (i32 l = 0; l < X11_KEYPOS_LEVELS; l++) {
                    u32 s = sym_at(ctx, kc, g, l);
                    if ((s >= 'a' && s <= 'z') || (s >= 'A' && s <= 'Z')) return true;
                }
        }
    }
    return false;
}

/* The US key-cap of the physical key behind `xkeycode`, for a keymap with no
 * Latin layout at all: the position is all there is to read. 0 when the keymap
 * has an ASCII letter somewhere (the walk above was the answer) or the key is
 * not in the main block. */
static inline u32 x11_physical_cap(u32 xkeycode, X11SymAt sym_at, void *ctx) {
    u32 cap = x11_position_keysym(xkeycode);
    if (!cap) return 0;
    if (x11_keymap_has_latin(sym_at, ctx)) return 0;
    return cap;
}

/* The key a chord names by where its key sits: `known` of that key-cap, 0 for
 * none. */
static inline u32 x11_key_by_physical_position(u32 xkeycode, X11SymAt sym_at, void *ctx,
                                               u32 (*known)(u32 keysym)) {
    u32 cap = x11_physical_cap(xkeycode, sym_at, ctx);
    return cap ? known(cap) : 0;
}

/* The control byte a key-cap makes with Control held, by Xlib's own rule for an
 * ASCII keysym (XLookupString): the byte Ctrl+[ , Ctrl+\ , Ctrl+] and Ctrl+Space
 * type on a Latin layout. A layout with no Latin key types none at all for
 * them (its [ key is a Cyrillic letter), so the key found by position gives it
 * here. -1 when the key-cap makes no control byte. */
static inline i32 x11_ctrl_byte(u32 cap) {
    u32 c = cap;
    if ((c >= '@' && c < 0x7f) || c == ' ') c &= 0x1f;
    else if (c == '2') c = 0x00;
    else if (c >= '3' && c <= '7') c -= '3' - 0x1b;
    else if (c == '8') c = 0x7f;
    else if (c == '/') c = '_' & 0x1f;
    return (c < 0x20 || c == 0x7f) ? (i32)c : -1;
}

#endif /* LIU_PLATFORM_X11_KEYPOS_H */
