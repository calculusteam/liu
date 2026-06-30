/*
 * Liu - Translate-on-Tab: prompt segmenter.
 *
 * Splits a captured prompt line into TEXT segments (translated) and KEEP
 * segments (emitted verbatim, in their original position): filesystem
 * paths, URLs, @file references and [Image #N]-style attachment
 * placeholders. Translating those tokens corrupts them — a dragged
 * screenshot path must reach the agent byte-for-byte — so the translate
 * state machine in main.c translates each TEXT segment separately and
 * stitches the results around the KEEP segments.
 *
 * Pure functions over the input string; no allocation, no app state.
 */
#ifndef TRANSLATE_TRANSLATE_SEGMENT_H
#define TRANSLATE_TRANSLATE_SEGMENT_H

#include "core/types.h"

/* Hard cap on segments per prompt line. A prompt rarely carries more than
 * a handful of attachments; overflow folds the remainder into one final
 * TEXT segment rather than dropping anything. */
#define TRANSLATE_MAX_SEGS 24

typedef enum {
    TRANSLATE_SEG_TEXT = 0,   /* natural language — translate */
    TRANSLATE_SEG_KEEP = 1,   /* path / URL / placeholder — pass through */
} TranslateSegKind;

typedef struct TranslateSeg {
    TranslateSegKind kind;
    i32 off;                  /* byte offset into the source string */
    i32 len;                  /* byte length (never splits a UTF-8 seq) */
} TranslateSeg;

/* Split `text` into at most `cap` segments. Returns the segment count
 * (0 for NULL/empty input). TEXT segments are trimmed of surrounding
 * whitespace; KEEP segments absorb the whitespace between themselves and
 * their neighbours so that emitting all segments back-to-back with each
 * TEXT segment replaced by its translation reproduces the original
 * spacing around the kept tokens. Adjacent KEEP tokens merge into one
 * segment. Backslash-escaped bytes (e.g. "\\ " in dropped paths) never
 * break a token. */
i32 translate_segment_split(const char *text, TranslateSeg *out, i32 cap);

/* True if at least one segment in the list is TEXT (i.e. there is
 * something to translate at all). */
bool translate_segment_has_text(const TranslateSeg *segs, i32 count);

#endif /* TRANSLATE_TRANSLATE_SEGMENT_H */
