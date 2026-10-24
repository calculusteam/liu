/*
 * macOS Secure Event Input routes key events only to the active process that
 * holds it. Liu enables it only while active: when a Liu password field has
 * focus; when the focused pane has tty echo disabled and line mode is active
 * (`secure_input_auto`); or when manual Secure Keyboard Entry is enabled. The
 * manual setting is off by default and also blocks text expanders while Liu
 * is frontmost. The toolbar chip indicates when Secure Event Input is held.
 */
#ifndef UI_SECURE_INPUT_H
#define UI_SECURE_INPUT_H

#include "core/types.h"

typedef struct {
    bool app_active;        /* Liu is the active app and this window has the keyboard */
    bool manual;            /* the user's switch */
    bool automatic;         /* take it by itself while a pane reads a password */
    bool pane_password;     /* the focused pane's tty: echo off, line mode */
    bool field_password;    /* one of Liu's own password fields has the keyboard */
} SecureInputFacts;

typedef enum {
    SECURE_INPUT_OFF = 0,
    SECURE_INPUT_PASSWORD,  /* held for as long as a password is being typed */
    SECURE_INPUT_MANUAL,    /* Held while manual Secure Keyboard Entry is enabled. */
} SecureInputWhy;

static inline SecureInputWhy secure_input_wanted(const SecureInputFacts *f) {
    if (!f || !f->app_active) return SECURE_INPUT_OFF;
    if (f->field_password) return SECURE_INPUT_PASSWORD;
    if (f->automatic && f->pane_password) return SECURE_INPUT_PASSWORD;
    if (f->manual) return SECURE_INPUT_MANUAL;
    return SECURE_INPUT_OFF;
}

/* What the chip says, and what a click on it explains. */
static inline const char *secure_input_label(void) { return "Secure input"; }
static inline const char *secure_input_explain(SecureInputWhy why) {
    switch (why) {
        case SECURE_INPUT_PASSWORD:
            return "Secure input is on while a password is typed: no other app can read your keys";
        case SECURE_INPUT_MANUAL:
            return "Secure keyboard entry is on: no other app can read your keys (Cmd+K to turn it off)";
        default: return "";
    }
}

#endif /* UI_SECURE_INPUT_H */
