/*
 * Liu — when the editor's selection is published as X11's PRIMARY.
 *
 * A selection made in the code editor is what a middle click pastes, in Liu and
 * in every other X application. It is published once per change, when no mouse
 * button is down, so a drag is not published at every move. "A change" is a
 * different document, a different text (the edit generation) or a different
 * pair of ends; this folds those into one number the caller compares with the
 * last it published.
 *
 * Pure.
 */
#ifndef UI_EDITOR_PRIMARY_H
#define UI_EDITOR_PRIMARY_H

#include "core/types.h"

static inline u64 editor_primary_sig(const void *doc, u32 edit_gen, i32 anchor_line, i32 anchor_col,
                                     i32 caret_line, i32 caret_col) {
    u64 h = (u64)(usize)doc * 0x9E3779B97F4A7C15ull;
    h ^= (u64)edit_gen * 0xC2B2AE3D27D4EB4Full;
    h = (h << 13) | (h >> 51);
    h ^= (u64)(u32)anchor_line * 0x165667B19E3779F9ull;
    h = (h << 13) | (h >> 51);
    h ^= (u64)(u32)anchor_col * 0x27D4EB2F165667C5ull;
    h = (h << 13) | (h >> 51);
    h ^= (u64)(u32)caret_line * 0x9E3779B97F4A7C15ull;
    h = (h << 13) | (h >> 51);
    h ^= (u64)(u32)caret_col * 0xC2B2AE3D27D4EB4Full;
    return h;
}

#endif /* UI_EDITOR_PRIMARY_H */
