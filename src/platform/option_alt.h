/*
 * These pure helpers map NSEvent modifier state to Option-as-Alt behavior.
 * The keyboard event path runs through AppKit, while the mapping rules can be
 * evaluated independently.
 */
#ifndef PLATFORM_OPTION_ALT_H
#define PLATFORM_OPTION_ALT_H

#include "core/types.h"

/* Is the Option key held on this event acting as Alt/Meta?
 *
 * `mode` is an OptionAsAlt (0 = off, 1 = left, 2 = right, 3 = both).
 * `left_down` / `right_down` come from the device-dependent modifier bits.
 * Synthesized events (accessibility tools, osascript) often report neither
 * side; treating those as the configured side keeps Alt working for them
 * rather than silently dropping the modifier. */
static inline bool option_alt_side_active(i32 mode, bool left_down, bool right_down) {
    if (mode <= 0 || mode > 3) return false;
    if (mode == 3) return true;
    if (!left_down && !right_down) return true;
    return (mode == 1) ? left_down : right_down;
}

/* Printable ASCII that is not a letter: punctuation, symbols, digits, space. */
static inline bool option_alt_is_ascii_text(u32 c) {
    if (c < 0x20 || c >= 0x7f) return false;
    bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return !letter;
}

/* Should this keystroke be delivered as text (composed character) rather than
 * as Alt+key?
 *
 * `dead_key`      — the chord composed nothing yet (accent waiting for input).
 * `composed_char` — [characters] differs from [charactersIgnoringModifiers],
 *                   i.e. the Option layer produced an actual character.
 * `c0`            — first code point of the composed string (0 if none).
 *
 * Off the Alt side, anything the layout composes is text — plain macOS
 * behaviour, and the only way some layouts can reach parts of their alphabet.
 *
 * On the Alt side the rule splits by what the chord composes, because macOS
 * layouts use the Option layer for two unrelated jobs (of the 251 layouts
 * macOS ships, 222 put some ASCII there):
 *
 *   - Punctuation layer — Turkish, German, French, Nordic, Spanish and most
 *     other non-US Latin layouts park ASCII punctuation on Option:
 *     '@' on 133 layouts, '{' on 163, '[' on 145, '|' on 139, '#' on 113.
 *     A terminal where '~', '|' or '{' cannot be typed is unusable, and these
 *     never collide with Meta, whose bindings are all letters. Text wins.
 *
 *   - Latin-access layer — Cyrillic (Bulgarian, Serbian, Macedonian), Korean,
 *     Kana, Cangjie and Zhuyin map the WHOLE Latin alphabet onto Option, so
 *     Option+B composes 'b'. Meta bindings are all letters, so a blanket
 *     "ASCII wins" would silently delete every one of them for those users.
 *
 * Letters therefore ask one more question, answered per layout by the caller:
 * is this letter reachable WITHOUT Option? If it is (Turkish Option+I types
 * 'i', but 'i' also has its own key), Meta wins and the user loses nothing.
 * If Option is the only way to reach it, text wins — otherwise the character
 * would be untypable.
 *
 * That distinction matters most on the partial layouts: Sámi, Turkmen, Azeri
 * and Akan put only a few letters (Q W X Z) behind Option, so those type while
 * Alt+B/F/D keep working. Of the 251 layouts macOS ships, 31 have letters that
 * are reachable only through Option.
 *
 * Digits and space count as text, not letters: layouts that expose them via
 * Option (Kana, Lithuanian QWERTY) mean them literally, and no Meta binding
 * uses them. */
static inline bool option_alt_text_wins(bool alt_side, bool dead_key,
                                        bool composed_char, u32 c0,
                                        bool letter_needs_option) {
    if (!alt_side) return dead_key || composed_char;
    if (!composed_char) return false;
    if (option_alt_is_ascii_text(c0)) return true;
    bool ascii_letter = (c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z');
    return ascii_letter && letter_needs_option;
}

#endif /* PLATFORM_OPTION_ALT_H */
