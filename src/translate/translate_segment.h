/*
 * Liu - Translate-on-Tab: prompt segmenter.
 *
 * Splits a captured prompt line into TEXT segments (translated) and KEEP
 * segments (preserved in place in their original position): filesystem
 * paths, URLs, @file references and [Image #N]-style attachment
 * placeholders. Translating those tokens corrupts them — a dragged
 * screenshot path must reach the agent byte-for-byte — so the translate
 * state machine in main.c translates each TEXT segment separately and
 * replaces only those TEXT ranges, crossing KEEP/CHIP ranges with arrow keys.
 *
 * Read-only source classification, independent of app state.
 */
#ifndef TRANSLATE_TRANSLATE_SEGMENT_H
#define TRANSLATE_TRANSLATE_SEGMENT_H

#include "core/types.h"

/* Hard cap on segments. Overflow returns -1, never exposes protected tokens
 * to a translation backend. (Sixty-four: a prompt of thirty-two lines taken
 * apart at its line breaks is that many.) */
#define TRANSLATE_MAX_SEGS 64

typedef enum {
    TRANSLATE_SEG_TEXT = 0,   /* natural language — translate */
    TRANSLATE_SEG_KEEP = 1,   /* path / URL — pass through as text */
    /* An attachment the agent already owns, drawn as "[Image #1]". It is NOT
     * text: the agent holds the decoded image behind it, and re-typing the
     * label produces a dead string with no attachment. It must be stepped
     * over rather than rewritten — Claude Code and OpenCode both treat the
     * chip as ONE editing unit, so a single ←/→ crosses it and a single
     * Backspace would destroy it. */
    TRANSLATE_SEG_CHIP = 2,
    /* The line breaks between two lines of a prompt that is taken apart at
     * them (translate_segment_split_lines): the run of blanks and line breaks
     * from one line's last word to the next line's first. Never translated
     * and never handed to a backend — a model's own line break is a blank to
     * the draft — it is typed back as the editor's line-break key, `units`
     * times. Only in a draft that empties its input. */
    TRANSLATE_SEG_BREAK = 3,
} TranslateSegKind;

typedef struct TranslateSeg {
    TranslateSegKind kind;
    i32 off;                  /* byte offset into the source string */
    i32 len;                  /* byte length (never splits a UTF-8 seq) */
    /* Editing units this segment occupies in the agent's input: characters
     * for TEXT/KEEP, and for CHIP one unit for the chip itself plus one per
     * whitespace character it absorbed. This — not the byte or character
     * length — is how many ←/→/Backspace presses cross the segment. */
    i32 units;
} TranslateSeg;

/* Split `text` into at most `cap` segments. Returns the segment count
 * (0 for NULL/empty input, -1 on capacity overflow). TEXT segments are trimmed of surrounding
 * whitespace; KEEP segments absorb the whitespace between themselves and
 * their neighbours so that emitting all segments back-to-back with each
 * TEXT segment replaced by its translation reproduces the original
 * spacing around the kept tokens. Adjacent KEEP tokens merge into one
 * segment. Backslash-escaped bytes (e.g. "\\ " in dropped paths) never
 * break a token. */
i32 translate_segment_split(const char *text, TranslateSeg *out, i32 cap);

/* Most tokens a path is allowed to grow by while the probe below hunts for
 * the real filename. Covers "Screenshot 2026-08-02 at 19.35.57.png" with
 * room to spare, and bounds how far a stray '/' token can reach into prose.
 */
#define TRANSLATE_PATH_MAX_SPACE_TOKENS 8

/* Does `path` (`len` bytes, NOT NUL-terminated) name a file that exists?
 *
 * A path with spaces in it cannot be recognised by tokenising alone, and
 * unescaped ones are exactly what agent CLIs get: liu deliberately drops the
 * backslash escaping for them (they parse their own input and would try to
 * open a filename with literal backslashes in it), so a dragged screenshot
 * arrives as "/…/Screenshot 2026-08-02 at 19.35.57.png" — several tokens,
 * and the ones that read as ordinary words would be translated, corrupting
 * the path. Asking the filesystem is the one test that answers this without
 * guessing.
 *
 * NULL disables the recovery and leaves a pure string classification. */
typedef bool (*TranslatePathProbe)(const char *path, i32 len, void *ud);

i32 translate_segment_split_ex(const char *text, TranslateSeg *out, i32 cap,
                               TranslatePathProbe probe, void *ud);

/* A prompt that is ALL text (translate_segment_split gave one TEXT), taken
 * apart at its line breaks, so that each line is translated by itself and the
 * breaks between them are kept: TEXT for a line with a letter in it, KEEP for
 * one without ("---", "1.", "```": typed back as it is), BREAK for what
 * stands between two lines. A model handed the whole prompt may keep its
 * lines or may not (it can keep a numbered list and join two paragraphs), and
 * its line break is never typed — so the lines are the caller's to keep.
 * Returns the segment count, 1 for a prompt of one line, 0 for no text, -1
 * past `cap`. */
i32 translate_segment_split_lines(const char *text, TranslateSeg *out, i32 cap);

/* Editing units of one segment (see TranslateSeg.units). Exposed so callers
 * can recompute it for a segment they built themselves. */
i32 translate_segment_units(const char *text, const TranslateSeg *sg);

/* True if at least one segment in the list is TEXT (i.e. there is
 * something to translate at all). */
bool translate_segment_has_text(const TranslateSeg *segs, i32 count);

#endif /* TRANSLATE_TRANSLATE_SEGMENT_H */
