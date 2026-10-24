/*
 * Liu - terminal mouse reporting (X10, SGR, button/motion tracking)
 */
#include "terminal/terminal.h"
#include "platform/platform.h"
#include <string.h>
#include <stdio.h>

/*
 * Encode mouse event as escape sequence for the remote application.
 * Returns bytes written to out buffer.
 *
 * button: 0=left, 1=middle, 2=right, 3=release, 64=scroll-up, 65=scroll-down
 * col, row: 0-based terminal cell coordinates
 * pressed: true=press, false=release
 */
/* `motion` adds 32 to the button code: the protocol's "moved with this button held". */
static i32 mouse_encode(Terminal *t, i32 button, i32 col, i32 row, bool pressed,
                        bool motion, u32 mods, u8 *out, i32 out_size) {
    if (!t || !out || out_size <= 0 || col < 0 || row < 0 ||
        col == INT32_MAX || row == INT32_MAX) return 0;
    if (!(button == 0 || button == 1 || button == 2 || button == 3 ||
          button == 64 || button == 65)) return 0;
    if (!(t->mode & MODE_MOUSE_TRACKING)) return 0;

    /* Build button value. A release is "button 3" in the X10 form, which has
     * no other way to say it; the SGR form says it with its final `m` and
     * keeps the button's own number, so a program knows WHICH button came up. */
    i32 cb = button;
    if (!pressed && button < 3 && !(t->mode & MODE_MOUSE_SGR)) cb = 3; /* release */
    if (motion) cb += 32;
    if (mods & MOD_SHIFT) cb |= 4;
    if (mods & MOD_ALT)   cb |= 8;
    if (mods & MOD_CTRL)  cb |= 16;

    if (t->mode & MODE_MOUSE_SGR) {
        /* SGR mode: ESC [ < Cb ; Cx ; Cy M/m */
        char type = pressed ? 'M' : 'm';
        i32 n = snprintf((char *)out, (size_t)out_size,
                         "\x1b[<%d;%d;%d%c", cb, col + 1, row + 1, type);
        if (n < 0) return 0;
        return n < out_size ? n : out_size - 1;
    } else {
        /* X10 / normal mode: ESC [ M Cb Cx Cy (limited to 223 cols/rows) */
        if (col > 222 || row > 222) return 0;
        if (out_size < 6) return 0;
        out[0] = '\x1b';
        out[1] = '[';
        out[2] = 'M';
        out[3] = (u8)(32 + cb);
        out[4] = (u8)(32 + col + 1);
        out[5] = (u8)(32 + row + 1);
        return 6;
    }
}

i32 terminal_mouse_encode(Terminal *t, i32 button, i32 col, i32 row,
                           bool pressed, u32 mods, u8 *out, i32 out_size) {
    return mouse_encode(t, button, col, row, pressed, false, mods, out, out_size);
}

i32 terminal_mouse_encode_drag(Terminal *t, i32 button, i32 col, i32 row,
                               u32 mods, u8 *out, i32 out_size) {
    if (!t || !(t->mode & (MODE_MOUSE_MOTION | MODE_MOUSE_ANY))) return 0;
    if (button < 0 || button > 2) return 0;
    return mouse_encode(t, button, col, row, true, true, mods, out, out_size);
}

i32 terminal_mouse_encode_move(Terminal *t, i32 col, i32 row, u32 mods, u8 *out, i32 out_size) {
    if (!t || !(t->mode & MODE_MOUSE_ANY)) return 0;
    /* "No button" is 3, and motion adds 32: Cb 35. */
    return mouse_encode(t, 3, col, row, true, true, mods, out, out_size);
}

/*
 * Encode scroll wheel event.
 */
i32 terminal_mouse_scroll(Terminal *t, i32 col, i32 row, bool up, u32 mods,
                           u8 *out, i32 out_size) {
    i32 button = up ? 64 : 65;
    return terminal_mouse_encode(t, button, col, row, true, mods, out, out_size);
}
