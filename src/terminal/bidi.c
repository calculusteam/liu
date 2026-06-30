/*
 * Liu — Bidirectional text (BiDi) implementation
 * Simplified UAX #9 (Unicode BiDi Algorithm) for terminal rendering.
 *
 * This implements a practical subset of the full UBA:
 *   - Character classification for Hebrew, Arabic, Latin, numbers
 *   - Base direction detection (first strong character)
 *   - Embedding level resolution (simplified: 0 = LTR, 1 = RTL)
 *   - Visual reordering by reversing RTL runs
 *   - Basic Arabic presentation form shaping
 *
 * Explicit embedding controls (LRE, RLE, PDF, LRI, RLI, etc.) are NOT
 * supported — terminals rarely encounter them.
 */
#include "terminal/bidi.h"
#include <string.h>
#include <stdlib.h>

/* =========================================================================
 * Character classification
 * ========================================================================= */

BidiType bidi_char_type(u32 cp) {
    /* ASCII fast path */
    if (cp < 0x0080) {
        if (cp >= 'A' && cp <= 'Z') return BIDI_L;
        if (cp >= 'a' && cp <= 'z') return BIDI_L;
        if (cp >= '0' && cp <= '9') return BIDI_EN;
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') return BIDI_WS;
        if (cp == '+' || cp == '-') return BIDI_ES;
        if (cp == ',' || cp == '.' || cp == ':' || cp == '/') return BIDI_CS;
        if (cp == '$' || cp == '%' || cp == '#') return BIDI_ET;
        /* Other ASCII punctuation/symbols */
        return BIDI_ON;
    }

    /* Latin Extended / IPA / Spacing Modifier / Combining Diacritical */
    if (cp >= 0x0080 && cp <= 0x036F) {
        /* Combining diacritical marks (U+0300-U+036F) are non-spacing marks.
         * The outer range must reach 0x036F or this NSM check is dead code. */
        if (cp >= 0x0300 && cp <= 0x036F) return BIDI_NSM;
        return BIDI_L;
    }

    /* Greek and Coptic */
    if (cp >= 0x0370 && cp <= 0x03FF) return BIDI_L;

    /* Cyrillic */
    if (cp >= 0x0400 && cp <= 0x04FF) return BIDI_L;

    /* Hebrew */
    if (cp >= 0x0590 && cp <= 0x05FF) {
        /* Hebrew combining marks */
        if (cp >= 0x0591 && cp <= 0x05BD) return BIDI_NSM;
        if (cp == 0x05BF) return BIDI_NSM;
        if (cp >= 0x05C1 && cp <= 0x05C2) return BIDI_NSM;
        if (cp >= 0x05C4 && cp <= 0x05C5) return BIDI_NSM;
        if (cp == 0x05C7) return BIDI_NSM;
        return BIDI_R;
    }

    /* Arabic */
    if (cp >= 0x0600 && cp <= 0x06FF) {
        /* Arabic-Indic digits */
        if (cp >= 0x0660 && cp <= 0x0669) return BIDI_AN;
        /* Extended Arabic-Indic digits */
        if (cp >= 0x06F0 && cp <= 0x06F9) return BIDI_EN;
        /* Arabic combining marks */
        if (cp >= 0x064B && cp <= 0x065F) return BIDI_NSM;
        if (cp == 0x0670) return BIDI_NSM;
        if (cp >= 0x06D6 && cp <= 0x06DC) return BIDI_NSM;
        if (cp >= 0x06DF && cp <= 0x06E4) return BIDI_NSM;
        if (cp >= 0x06E7 && cp <= 0x06E8) return BIDI_NSM;
        if (cp >= 0x06EA && cp <= 0x06ED) return BIDI_NSM;
        /* Format characters */
        if (cp >= 0x0600 && cp <= 0x0605) return BIDI_AN;
        if (cp == 0x060C || cp == 0x060D) return BIDI_CS;
        return BIDI_AL;
    }

    /* Syriac */
    if (cp >= 0x0700 && cp <= 0x074F) return BIDI_AL;

    /* Arabic Supplement */
    if (cp >= 0x0750 && cp <= 0x077F) return BIDI_AL;

    /* Thaana */
    if (cp >= 0x0780 && cp <= 0x07BF) return BIDI_AL;

    /* NKo */
    if (cp >= 0x07C0 && cp <= 0x07FF) return BIDI_R;

    /* Samaritan */
    if (cp >= 0x0800 && cp <= 0x083F) return BIDI_R;

    /* Mandaic */
    if (cp >= 0x0840 && cp <= 0x085F) return BIDI_AL;

    /* Arabic Extended-A */
    if (cp >= 0x08A0 && cp <= 0x08FF) return BIDI_AL;

    /* Devanagari, Bengali, etc. (Indic scripts) */
    if (cp >= 0x0900 && cp <= 0x0DFF) return BIDI_L;

    /* Thai, Lao */
    if (cp >= 0x0E00 && cp <= 0x0EFF) return BIDI_L;

    /* Georgian */
    if (cp >= 0x10A0 && cp <= 0x10FF) return BIDI_L;

    /* Hangul Jamo */
    if (cp >= 0x1100 && cp <= 0x11FF) return BIDI_L;

    /* General punctuation */
    if (cp >= 0x2000 && cp <= 0x200A) return BIDI_WS;
    if (cp >= 0x200B && cp <= 0x200D) return BIDI_BN;   /* ZWSP, ZWNJ, ZWJ */
    if (cp == 0x200E) return BIDI_L;   /* LRM */
    if (cp == 0x200F) return BIDI_R;   /* RLM */

    /* Explicit embedding/override/isolate controls */
    if (cp >= 0x202A && cp <= 0x202E) return BIDI_BN;
    if (cp >= 0x2066 && cp <= 0x2069) return BIDI_BN;

    /* Currency symbols */
    if (cp >= 0x20A0 && cp <= 0x20CF) return BIDI_ET;

    /* Letterlike symbols */
    if (cp >= 0x2100 && cp <= 0x214F) return BIDI_L;

    /* Number forms */
    if (cp >= 0x2150 && cp <= 0x218F) return BIDI_EN;

    /* Arrows, math operators, box drawing, etc. */
    if (cp >= 0x2190 && cp <= 0x27FF) return BIDI_ON;

    /* CJK */
    if (cp >= 0x2E80 && cp <= 0x9FFF) return BIDI_L;
    if (cp >= 0xF900 && cp <= 0xFAFF) return BIDI_L;

    /* Hangul syllables */
    if (cp >= 0xAC00 && cp <= 0xD7AF) return BIDI_L;

    /* Arabic Presentation Forms-A */
    if (cp >= 0xFB1D && cp <= 0xFB4F) {
        if (cp == 0xFB1D) return BIDI_R;  /* Hebrew */
        if (cp >= 0xFB1E && cp <= 0xFB1E) return BIDI_NSM;
        if (cp >= 0xFB1F && cp <= 0xFB4F) return BIDI_R;  /* Hebrew ligatures */
    }

    /* Arabic Presentation Forms-A (continued) */
    if (cp >= 0xFB50 && cp <= 0xFDFF) return BIDI_AL;

    /* Arabic Presentation Forms-B */
    if (cp >= 0xFE70 && cp <= 0xFEFF) return BIDI_AL;

    /* Fullwidth/Halfwidth forms */
    if (cp >= 0xFF01 && cp <= 0xFF60) return BIDI_L;  /* Fullwidth Latin */
    if (cp >= 0xFF61 && cp <= 0xFFDC) return BIDI_L;  /* Halfwidth Katakana/Hangul */

    /* SMP: Supplementary scripts */
    if (cp >= 0x10000 && cp <= 0x1007F) return BIDI_L;  /* Linear B */
    if (cp >= 0x10800 && cp <= 0x1083F) return BIDI_R;  /* Cypriot */
    if (cp >= 0x10840 && cp <= 0x1085F) return BIDI_R;  /* Imperial Aramaic */
    if (cp >= 0x10900 && cp <= 0x1091F) return BIDI_R;  /* Phoenician */
    if (cp >= 0x10920 && cp <= 0x1093F) return BIDI_R;  /* Lydian */
    if (cp >= 0x10A00 && cp <= 0x10A5F) return BIDI_R;  /* Kharoshthi */
    if (cp >= 0x10B00 && cp <= 0x10B3F) return BIDI_R;  /* Avestan */

    /* Emoji (mostly neutral) */
    if (cp >= 0x1F300 && cp <= 0x1F9FF) return BIDI_ON;

    /* CJK Unified Ideographs Extension B+ */
    if (cp >= 0x20000 && cp <= 0x2FA1F) return BIDI_L;

    /* Default: treat as left-to-right */
    return BIDI_L;
}

/* =========================================================================
 * Quick RTL check
 * ========================================================================= */

bool bidi_line_has_rtl(const Cell *cells, i32 cols) {
    if (!cells) return false;
    for (i32 i = 0; i < cols; i++) {
        u32 cp = cells[i].codepoint;
        /* Fast reject: ASCII is never RTL */
        if (cp < 0x0590) continue;
        BidiType bt = bidi_char_type(cp);
        if (bt == BIDI_R || bt == BIDI_AL) return true;
    }
    return false;
}

/* =========================================================================
 * Base direction detection
 * ========================================================================= */

BidiDir bidi_base_direction(const Cell *cells, i32 cols) {
    if (!cells) return BIDI_DIR_LTR;
    for (i32 i = 0; i < cols; i++) {
        BidiType bt = bidi_char_type(cells[i].codepoint);
        if (bt == BIDI_L) return BIDI_DIR_LTR;
        if (bt == BIDI_R || bt == BIDI_AL) return BIDI_DIR_RTL;
    }
    return BIDI_DIR_LTR;
}

/* =========================================================================
 * Simplified BiDi reordering (UAX #9)
 *
 * Algorithm overview:
 *   1. Classify each character
 *   2. Determine base (paragraph) level: 0 (LTR) or 1 (RTL)
 *   3. Assign embedding levels using simplified rules:
 *      - Strong L characters get even (LTR) level
 *      - Strong R/AL characters get odd (RTL) level
 *      - Weak/neutral characters inherit from context
 *   4. Resolve weak types (numbers, separators)
 *   5. Resolve neutrals
 *   6. Reverse RTL runs in place
 * ========================================================================= */

/* Stack-allocated work buffers — avoid malloc in the hot path.
 * Max terminal width is typically 500 cols; 1024 covers all cases. */
#define BIDI_MAX_COLS 1024

void bidi_reorder_line(const Cell *cells, i32 cols, Cell *visual,
                       i32 *logical_to_visual) {
    if (!cells || !visual || cols <= 0) return;

    /* Clamp to our stack buffer limit */
    if (cols > BIDI_MAX_COLS) cols = BIDI_MAX_COLS;

    /* Work arrays on stack */
    BidiType types[BIDI_MAX_COLS];
    BidiType resolved[BIDI_MAX_COLS];
    u8       levels[BIDI_MAX_COLS];
    i32      order[BIDI_MAX_COLS];

    /* Step 1: Classify all characters */
    for (i32 i = 0; i < cols; i++) {
        types[i] = bidi_char_type(cells[i].codepoint);
        resolved[i] = types[i];
    }

    /* Step 2: Determine base direction */
    u8 base_level = 0;
    for (i32 i = 0; i < cols; i++) {
        if (types[i] == BIDI_L) { base_level = 0; break; }
        if (types[i] == BIDI_R || types[i] == BIDI_AL) { base_level = 1; break; }
    }

    /* Step 3: Assign initial embedding levels */
    for (i32 i = 0; i < cols; i++) {
        switch (types[i]) {
        case BIDI_L:
            levels[i] = base_level & ~1u;  /* even = LTR: 0 if base=0, 0 if base=1 -> force even */
            /* Actually: L in RTL paragraph should be level 2 (next even after base=1) */
            if (base_level == 1) levels[i] = 2;
            else levels[i] = 0;
            break;
        case BIDI_R:
            levels[i] = (base_level == 0) ? 1 : 1;
            break;
        case BIDI_AL:
            levels[i] = (base_level == 0) ? 1 : 1;
            break;
        default:
            levels[i] = base_level;
            break;
        }
    }

    /* Step 4: Resolve weak types */
    /* Rule W1: NSM gets type of preceding character */
    for (i32 i = 0; i < cols; i++) {
        if (resolved[i] == BIDI_NSM) {
            resolved[i] = (i > 0) ? resolved[i - 1] : BIDI_ON;
            levels[i] = (i > 0) ? levels[i - 1] : base_level;
        }
    }

    /* Rule W2: EN after AL becomes AN */
    {
        BidiType last_strong = (base_level == 1) ? BIDI_R : BIDI_L;
        for (i32 i = 0; i < cols; i++) {
            if (resolved[i] == BIDI_L || resolved[i] == BIDI_R)
                last_strong = resolved[i];
            else if (resolved[i] == BIDI_AL)
                last_strong = BIDI_AL;
            else if (resolved[i] == BIDI_EN && last_strong == BIDI_AL)
                resolved[i] = BIDI_AN;
        }
    }

    /* Rule W3: AL becomes R */
    for (i32 i = 0; i < cols; i++) {
        if (resolved[i] == BIDI_AL) resolved[i] = BIDI_R;
    }

    /* Rule W4: ES between two EN becomes EN; CS between matching becomes that type */
    for (i32 i = 1; i < cols - 1; i++) {
        if (resolved[i] == BIDI_ES &&
            resolved[i - 1] == BIDI_EN && resolved[i + 1] == BIDI_EN) {
            resolved[i] = BIDI_EN;
        }
        if (resolved[i] == BIDI_CS) {
            if (resolved[i - 1] == BIDI_EN && resolved[i + 1] == BIDI_EN)
                resolved[i] = BIDI_EN;
            else if (resolved[i - 1] == BIDI_AN && resolved[i + 1] == BIDI_AN)
                resolved[i] = BIDI_AN;
        }
    }

    /* Rule W5: ET adjacent to EN becomes EN */
    for (i32 i = 0; i < cols; i++) {
        if (resolved[i] == BIDI_ET) {
            bool near_en = false;
            if (i > 0 && resolved[i - 1] == BIDI_EN) near_en = true;
            if (i < cols - 1 && resolved[i + 1] == BIDI_EN) near_en = true;
            if (near_en) resolved[i] = BIDI_EN;
        }
    }
    /* Extended ET propagation: chain of ET next to EN all become EN */
    for (i32 i = 0; i < cols; i++) {
        if (resolved[i] == BIDI_ET) {
            /* Scan backwards for EN */
            for (i32 j = i - 1; j >= 0; j--) {
                if (resolved[j] == BIDI_EN) { resolved[i] = BIDI_EN; break; }
                if (resolved[j] != BIDI_ET) break;
            }
        }
    }
    for (i32 i = cols - 1; i >= 0; i--) {
        if (resolved[i] == BIDI_ET) {
            for (i32 j = i + 1; j < cols; j++) {
                if (resolved[j] == BIDI_EN) { resolved[i] = BIDI_EN; break; }
                if (resolved[j] != BIDI_ET) break;
            }
        }
    }

    /* Rule W6: Remaining separators and terminators become ON */
    for (i32 i = 0; i < cols; i++) {
        if (resolved[i] == BIDI_ES || resolved[i] == BIDI_ET || resolved[i] == BIDI_CS)
            resolved[i] = BIDI_ON;
    }

    /* Rule W7: EN after L becomes L */
    {
        BidiType last_strong = (base_level == 0) ? BIDI_L : BIDI_R;
        for (i32 i = 0; i < cols; i++) {
            if (resolved[i] == BIDI_L || resolved[i] == BIDI_R)
                last_strong = resolved[i];
            else if (resolved[i] == BIDI_EN && last_strong == BIDI_L)
                resolved[i] = BIDI_L;
        }
    }

    /* Step 5: Resolve neutrals and weak isolates
     * Rules N1/N2: neutrals between same-direction strong types adopt that direction;
     * otherwise adopt the base direction */
    for (i32 i = 0; i < cols; i++) {
        if (resolved[i] == BIDI_ON || resolved[i] == BIDI_WS ||
            resolved[i] == BIDI_BN) {
            /* Find preceding strong type */
            BidiType prev_strong = (base_level == 0) ? BIDI_L : BIDI_R;
            for (i32 j = i - 1; j >= 0; j--) {
                if (resolved[j] == BIDI_L || resolved[j] == BIDI_R ||
                    resolved[j] == BIDI_EN || resolved[j] == BIDI_AN) {
                    prev_strong = (resolved[j] == BIDI_EN || resolved[j] == BIDI_AN)
                        ? BIDI_R : resolved[j];
                    break;
                }
            }

            /* Find following strong type */
            BidiType next_strong = (base_level == 0) ? BIDI_L : BIDI_R;
            for (i32 j = i + 1; j < cols; j++) {
                if (resolved[j] == BIDI_L || resolved[j] == BIDI_R ||
                    resolved[j] == BIDI_EN || resolved[j] == BIDI_AN) {
                    next_strong = (resolved[j] == BIDI_EN || resolved[j] == BIDI_AN)
                        ? BIDI_R : resolved[j];
                    break;
                }
            }

            /* N1: if both sides agree, adopt their direction */
            if (prev_strong == next_strong) {
                resolved[i] = prev_strong;
            } else {
                /* N2: adopt paragraph direction */
                resolved[i] = (base_level == 0) ? BIDI_L : BIDI_R;
            }
        }
    }

    /* Step 5b: Final level assignment from resolved types */
    for (i32 i = 0; i < cols; i++) {
        if (resolved[i] == BIDI_L) {
            /* L in RTL context: next higher even level */
            levels[i] = (base_level == 1) ? 2 : 0;
        } else if (resolved[i] == BIDI_R) {
            /* R: next higher odd level */
            levels[i] = (base_level == 0) ? 1 : 1;
        } else if (resolved[i] == BIDI_AN || resolved[i] == BIDI_EN) {
            /* Numbers in RTL context: level 2 */
            levels[i] = (base_level == 0) ? 2 : 2;
            /* But EN in LTR stays at 0 (it was resolved to L by W7 if applicable) */
            if (resolved[i] == BIDI_EN && base_level == 0) levels[i] = 0;
        }
    }

    /* Step 6: Build initial logical order */
    for (i32 i = 0; i < cols; i++) {
        order[i] = i;
    }

    /* Step 7: Reverse runs at each level, from max down to base+1
     * Find maximum embedding level */
    u8 max_level = base_level;
    for (i32 i = 0; i < cols; i++) {
        if (levels[i] > max_level) max_level = levels[i];
    }

    /* Reverse sequences of characters at each level from max_level down */
    for (u8 lev = max_level; lev > 0; lev--) {
        i32 i = 0;
        while (i < cols) {
            /* Find start of a run at >= this level */
            while (i < cols && levels[i] < lev) i++;
            if (i >= cols) break;

            i32 run_start = i;
            while (i < cols && levels[i] >= lev) i++;
            i32 run_end = i - 1;

            /* Reverse the run in the order array */
            while (run_start < run_end) {
                i32 tmp = order[run_start];
                order[run_start] = order[run_end];
                order[run_end] = tmp;
                run_start++;
                run_end--;
            }
        }
    }

    /* Step 8: Produce visual output */
    for (i32 vis = 0; vis < cols; vis++) {
        visual[vis] = cells[order[vis]];
    }

    /* Step 9: Build logical-to-visual mapping if requested */
    if (logical_to_visual) {
        for (i32 vis = 0; vis < cols; vis++) {
            logical_to_visual[order[vis]] = vis;
        }
    }
}

/* =========================================================================
 * Arabic shaping — map base Arabic characters to presentation forms
 *
 * Arabic characters have four forms based on their position in a word:
 *   - Isolated (standalone)
 *   - Initial  (beginning of word)
 *   - Medial   (middle of word)
 *   - Final    (end of word)
 *
 * The presentation forms are in the Unicode Arabic Presentation Forms-B
 * block (U+FE70-U+FEFF).
 * ========================================================================= */

/* Joining type: does this character join with neighbors? */
typedef enum {
    JOIN_NONE,          /* Non-joining */
    JOIN_RIGHT,         /* Right-joining (joins to preceding char on the right) */
    JOIN_DUAL,          /* Dual-joining (joins on both sides) */
    JOIN_CAUSING,       /* Join-causing (e.g., ZWJ) */
    JOIN_TRANSPARENT,   /* Transparent (combining marks) */
} JoinType;

static JoinType arabic_join_type(u32 cp) {
    /* Alef family: right-joining only */
    if (cp == 0x0627 || cp == 0x0622 || cp == 0x0623 || cp == 0x0625 ||
        cp == 0x0671 || cp == 0x0672 || cp == 0x0673)
        return JOIN_RIGHT;

    /* Teh Marbuta, Heh, Dal, Thal, Reh, Zain, Waw: right-joining */
    if (cp == 0x0629 || cp == 0x062F || cp == 0x0630 || cp == 0x0631 ||
        cp == 0x0632 || cp == 0x0648)
        return JOIN_RIGHT;

    /* Most Arabic letters are dual-joining */
    if (cp >= 0x0628 && cp <= 0x064A) {
        /* Filter out non-joining marks/diacritics */
        if (cp >= 0x064B && cp <= 0x065F) return JOIN_TRANSPARENT;
        /* Already handled right-joining above, rest are dual */
        return JOIN_DUAL;
    }

    /* Zero-width joiner */
    if (cp == 0x200D) return JOIN_CAUSING;

    /* Combining marks are transparent */
    if (cp >= 0x0610 && cp <= 0x061A) return JOIN_TRANSPARENT;
    if (cp >= 0x064B && cp <= 0x065F) return JOIN_TRANSPARENT;
    if (cp == 0x0670) return JOIN_TRANSPARENT;

    return JOIN_NONE;
}

/* Presentation form lookup table for common Arabic letters.
 * Format: { base_codepoint, isolated, final, initial, medial }
 * 0 means "no form available" (use base codepoint). */
typedef struct {
    u32 base;
    u32 isolated;
    u32 final_;
    u32 initial;
    u32 medial;
} ArabicForm;

static const ArabicForm arabic_forms[] = {
    /* Hamza */        { 0x0621, 0xFE80, 0,      0,      0      },
    /* Alef Madda */   { 0x0622, 0xFE81, 0xFE82, 0,      0      },
    /* Alef Hamza^ */  { 0x0623, 0xFE83, 0xFE84, 0,      0      },
    /* Waw Hamza */    { 0x0624, 0xFE85, 0xFE86, 0,      0      },
    /* Alef Hamzav */  { 0x0625, 0xFE87, 0xFE88, 0,      0      },
    /* Yeh Hamza */    { 0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C },
    /* Alef */         { 0x0627, 0xFE8D, 0xFE8E, 0,      0      },
    /* Beh */          { 0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92 },
    /* Teh Marbuta */  { 0x0629, 0xFE93, 0xFE94, 0,      0      },
    /* Teh */          { 0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98 },
    /* Theh */         { 0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C },
    /* Jeem */         { 0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0 },
    /* Hah */          { 0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4 },
    /* Khah */         { 0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8 },
    /* Dal */          { 0x062F, 0xFEA9, 0xFEAA, 0,      0      },
    /* Thal */         { 0x0630, 0xFEAB, 0xFEAC, 0,      0      },
    /* Reh */          { 0x0631, 0xFEAD, 0xFEAE, 0,      0      },
    /* Zain */         { 0x0632, 0xFEAF, 0xFEB0, 0,      0      },
    /* Seen */         { 0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4 },
    /* Sheen */        { 0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8 },
    /* Sad */          { 0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC },
    /* Dad */          { 0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0 },
    /* Tah */          { 0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4 },
    /* Zah */          { 0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8 },
    /* Ain */          { 0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC },
    /* Ghain */        { 0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0 },
    /* Feh */          { 0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4 },
    /* Qaf */          { 0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8 },
    /* Kaf */          { 0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC },
    /* Lam */          { 0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0 },
    /* Meem */         { 0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4 },
    /* Noon */         { 0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8 },
    /* Heh */          { 0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC },
    /* Waw */          { 0x0648, 0xFEED, 0xFEEE, 0,      0      },
    /* Alef Maksura */ { 0x0649, 0xFEEF, 0xFEF0, 0,      0      },
    /* Yeh */          { 0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4 },
};

#define ARABIC_FORM_COUNT (sizeof(arabic_forms) / sizeof(arabic_forms[0]))

static const ArabicForm *find_arabic_form(u32 cp) {
    for (usize i = 0; i < ARABIC_FORM_COUNT; i++) {
        if (arabic_forms[i].base == cp) return &arabic_forms[i];
    }
    return NULL;
}

static bool is_arabic_letter(u32 cp) {
    return (cp >= 0x0621 && cp <= 0x064A) ||
           (cp >= 0xFB50 && cp <= 0xFDFF) ||
           (cp >= 0xFE70 && cp <= 0xFEFF);
}

void bidi_shape_arabic(Cell *visual, i32 cols) {
    if (!visual || cols <= 0) return;

    for (i32 i = 0; i < cols; i++) {
        u32 cp = visual[i].codepoint;
        if (!is_arabic_letter(cp)) continue;

        const ArabicForm *form = find_arabic_form(cp);
        if (!form) continue;

        JoinType my_join = arabic_join_type(cp);
        if (my_join == JOIN_NONE) continue;

        /* Determine joining context.
         * In visual order (after BiDi reordering), Arabic text reads right-to-left.
         * The "previous" character in logical Arabic reading is to the RIGHT
         * in visual order (i+1), and "next" is to the LEFT (i-1).
         *
         * However, since we've already reordered, the visual array has RTL text
         * reversed. So for a visually ordered array, the character that was
         * logically before in Arabic is at i+1 and logically after is at i-1. */
        bool joins_prev = false;  /* joins to right in visual = i+1 */
        bool joins_next = false;  /* joins to left in visual = i-1 */

        /* Check right neighbor (i+1): can we join to the preceding Arabic char? */
        if (i + 1 < cols) {
            JoinType rjt = arabic_join_type(visual[i + 1].codepoint);
            if (rjt == JOIN_DUAL || rjt == JOIN_CAUSING ||
                rjt == JOIN_RIGHT)
                joins_prev = true;
        }

        /* Check left neighbor (i-1): can we join to the following Arabic char? */
        if (i - 1 >= 0) {
            JoinType ljt = arabic_join_type(visual[i - 1].codepoint);
            if (ljt == JOIN_DUAL || ljt == JOIN_CAUSING)
                joins_next = true;
        }

        /* Select form */
        u32 shaped = cp;
        if (my_join == JOIN_DUAL) {
            if (joins_prev && joins_next && form->medial)
                shaped = form->medial;
            else if (joins_prev && form->final_)
                shaped = form->final_;
            else if (joins_next && form->initial)
                shaped = form->initial;
            else if (form->isolated)
                shaped = form->isolated;
        } else if (my_join == JOIN_RIGHT) {
            if (joins_prev && form->final_)
                shaped = form->final_;
            else if (form->isolated)
                shaped = form->isolated;
        }

        visual[i].codepoint = shaped;
    }
}
