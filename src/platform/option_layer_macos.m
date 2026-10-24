/*
 * Option-layer probing for the active macOS keyboard layout.
 * See option_layer_macos.h for what this answers and why.
 */
#include "platform/option_layer_macos.h"

#import <Carbon/Carbon.h>
#import <Foundation/Foundation.h>

/* The alphanumeric block: every virtual keycode that can carry a character.
 * Function keys, arrows and modifiers are irrelevant — they compose nothing on
 * any layout, so they never reach the Option-as-Alt decision. */
static const u16 OPTION_LAYER_KEYCODES[] = {
     0, 11,  8,  2, 14,  3,  5,  4, 34, 38, 40, 37, 46,   /* a..m */
    45, 31, 35, 12, 15,  1, 17, 32,  9, 13,  7, 16,  6,   /* n..z */
    29, 18, 19, 20, 21, 23, 22, 26, 28, 25,               /* 0..9 */
    24, 27, 33, 30, 41, 39, 43, 47, 44, 42, 50,           /* = - [ ] ; ' , . / \ ` */
    49,                                                    /* space */
};
static const usize OPTION_LAYER_NKEYS =
    sizeof OPTION_LAYER_KEYCODES / sizeof OPTION_LAYER_KEYCODES[0];

AsciiSet option_layer_scan_bare_ascii(const void *layout_data, u32 kbd_type) {
    AsciiSet set = { { 0, 0 } };
    if (!layout_data) return set;
    const UCKeyboardLayout *layout = (const UCKeyboardLayout *)layout_data;

    /* Bare and Shift only. Anything requiring Control is not "typing the
     * character" in the sense this question is about. */
    const UInt32 mod_variants[] = { 0, (shiftKey >> 8) & 0xff };

    for (usize k = 0; k < OPTION_LAYER_NKEYS; k++) {
        for (usize m = 0; m < 2; m++) {
            UInt32 dead_state = 0;
            UniChar buf[8];
            UniCharCount len = 0;
            OSStatus st = UCKeyTranslate(layout, OPTION_LAYER_KEYCODES[k],
                                         kUCKeyActionDown, mod_variants[m],
                                         (UInt32)kbd_type, 0, &dead_state,
                                         sizeof buf / sizeof buf[0], &len, buf);
            /* A dead key produces no character of its own — it waits for the
             * next keystroke — so it does not count as reaching one. */
            if (st != noErr || len != 1 || dead_state != 0) continue;
            if (buf[0] < 128) ascii_set_add(&set, (u32)buf[0]);
        }
    }
    return set;
}

/* Cache keyed on the input source ID, so switching layouts (Cmd+Space) picks
 * up the new table on the next Option chord without any notification wiring. */
static AsciiSet g_bare_ascii;
static NSString *g_cached_source_id = nil;
static bool g_cache_valid = false;

static void option_layer_refresh(void) {
    TISInputSourceRef src = TISCopyCurrentKeyboardLayoutInputSource();
    if (!src) { g_cache_valid = false; return; }

    NSString *source_id = (__bridge NSString *)
        TISGetInputSourceProperty(src, kTISPropertyInputSourceID);
    if (g_cache_valid && source_id && g_cached_source_id &&
        [source_id isEqualToString:g_cached_source_id]) {
        CFRelease(src);
        return;
    }

    CFDataRef data = (CFDataRef)TISGetInputSourceProperty(
        src, kTISPropertyUnicodeKeyLayoutData);
    if (!data) {
        /* IMEs (Japanese, Chinese, Korean input methods) expose no key table.
         * Leave the cache invalid: letters then stay Meta, and the IME's own
         * marked-text path is unaffected either way. */
        g_cache_valid = false;
        CFRelease(src);
        return;
    }

    g_bare_ascii = option_layer_scan_bare_ascii(CFDataGetBytePtr(data),
                                                (u32)LMGetKbdType());
    [g_cached_source_id release];
    g_cached_source_id = [source_id copy];
    g_cache_valid = true;
    CFRelease(src);
}

bool option_layer_needs_option(u32 codepoint) {
    if (codepoint >= 128) return false;
    option_layer_refresh();
    if (!g_cache_valid) return false;
    return !ascii_set_has(&g_bare_ascii, codepoint);
}
