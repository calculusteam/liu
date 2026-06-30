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
i32 terminal_mouse_encode(Terminal *t, i32 button, i32 col, i32 row,
                           bool pressed, u32 mods, u8 *out, i32 out_size) {
    if (!(t->mode & (MODE_MOUSE_BTN | MODE_MOUSE_MOTION))) return 0;

    /* Build button value */
    i32 cb = button;
    if (!pressed && button < 3) cb = 3; /* release */
    if (mods & MOD_SHIFT) cb |= 4;
    if (mods & MOD_ALT)   cb |= 8;
    if (mods & MOD_CTRL)  cb |= 16;

    if (t->mode & MODE_MOUSE_SGR) {
        /* SGR mode: ESC [ < Cb ; Cx ; Cy M/m */
        char type = pressed ? 'M' : 'm';
        i32 n = snprintf((char *)out, (size_t)out_size,
                         "\x1b[<%d;%d;%d%c", cb, col + 1, row + 1, type);
        return n;
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

/*
 * Encode scroll wheel event.
 */
i32 terminal_mouse_scroll(Terminal *t, i32 col, i32 row, bool up, u32 mods,
                           u8 *out, i32 out_size) {
    i32 button = up ? 64 : 65;
    return terminal_mouse_encode(t, button, col, row, true, mods, out, out_size);
}
