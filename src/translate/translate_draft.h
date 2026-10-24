/* Streaming prompt edit: erase TEXT first, keep paths/attachments in place,
 * then insert validated token bytes. Retain source and sent-byte counts so
 * failures can undo only Liu's own edits without re-creating attachment
 * labels. */
#ifndef TRANSLATE_DRAFT_H
#define TRANSLATE_DRAFT_H
#include "translate/translate_segment.h"

#define TRANSLATE_DRAFT_SOURCE_CAP 4096
#define TRANSLATE_DRAFT_OUTPUT_CAP 16384
#define TRANSLATE_EDIT_CAP (2 * TRANSLATE_MAX_SEGS + 3)

typedef struct {
    char source[TRANSLATE_DRAFT_SOURCE_CAP];
    TranslateSeg segs[TRANSLATE_MAX_SEGS];
    i32 count;
    char output[TRANSLATE_DRAFT_OUTPUT_CAP];
    i32 used, current;
    i32 off[TRANSLATE_MAX_SEGS], len[TRANSLATE_MAX_SEGS];
    i32 sent[TRANSLATE_MAX_SEGS], cursor_seg;
    u8 pending_utf8[4];
    i32 pending_len;
    bool failed, pending_space;
    bool begun, complete;
    /* The prompt is ONE run of text and its editor can be emptied without
     * counting (set by the caller after init): the erase is "Backspace and
     * Delete, more of each than there are characters", so nothing rests on
     * the count — a blank no cell shows, a character the program never took
     * or a cluster it counts differently cannot leave a piece of the prompt
     * behind. The blanks and line breaks around the text go with it.
     *
     * Such a draft may be taken apart at its line breaks
     * (translate_segment_split_lines): what stands between two lines of
     * translation is then typed back — a BREAK as the editor's line-break
     * key, a line without words as itself — instead of stepped over. */
    bool whole;
    /* The editor has a key that takes the caret to the end of its LINE (set by
     * the caller after init; Ctrl+E in Claude Code). The caret is then taken
     * to the end of the input by that key and one Right, once a line — a
     * handful of keys — instead of one Right, or one Delete, for every
     * character. Hundreds of keys that are escape sequences, in one burst, are
     * read by the program in pieces, and one cut at the end of a piece can be
     * taken apart, typing the "3~" of a Delete into the prompt. Backspace is
     * one byte and cannot be. */
    bool line_end_key;
} TranslateDraft;

/* Lines past the count the caret is walked over on its way to the end, and
 * forward deletes sent there: each does nothing at the end of an input. */
#define TRANSLATE_END_SLACK 4
#define TRANSLATE_END_DELETES 16

/* Keys past the count in such an erase: every one of them is a key that does
 * nothing in an input that is already empty. */
#define TRANSLATE_WHOLE_SLACK 64

typedef enum {
    TRANSLATE_EDIT_RIGHT, TRANSLATE_EDIT_LEFT,
    TRANSLATE_EDIT_ERASE, TRANSLATE_EDIT_TEXT, TRANSLATE_EDIT_SOURCE,
    TRANSLATE_EDIT_DELETE,         /* forward delete, `whole` drafts only */
    TRANSLATE_EDIT_NEWLINE,        /* the editor's line-break key, `whole` drafts only */
    TRANSLATE_EDIT_TO_END          /* the line-end key and one Right, `count` times (line_end_key) */
} TranslateEditKind;
typedef struct {
    TranslateEditKind kind;
    i32 count; /* keys, or bytes for TEXT */
    i32 off;   /* byte offset in output (TEXT) or source (SOURCE) */
} TranslateEdit;

bool translate_draft_init(TranslateDraft *d, const char *source,
                          const TranslateSeg *segs, i32 count);
/* Sequential segments; arbitrary UTF-8 chunk boundaries are supported.
 * CR/LF/TAB become spaces. Other controls invalidate the whole draft. */
bool translate_draft_append(TranslateDraft *d, i32 seg, const char *s, i32 len);
/* Begin returns 0 on invalid/stale input. Mark begun only after the complete
 * plan has been accepted by the PTY queue. No translation is needed yet. */
i32 translate_draft_begin_plan(const TranslateDraft *d, const char *current_source,
                                TranslateEdit *edits, i32 cap);
/* Delta returns -1 on invalid input, 0 if there is no complete UTF-8 output
 * yet. seg == count finishes by crossing trailing protected ranges. */
i32 translate_draft_delta_plan(const TranslateDraft *d, i32 seg,
                                TranslateEdit *edits, i32 cap);
void translate_draft_delta_applied(TranslateDraft *d, i32 seg);
bool translate_draft_finish_segment(const TranslateDraft *d, i32 seg);
/* Valid even after a backend/UTF-8 failure. Only accepted output is erased;
 * original TEXT is pasted back and KEEP/CHIP ranges are crossed in place. */
i32 translate_draft_restore_plan(const TranslateDraft *d, TranslateEdit *edits, i32 cap);
/* Visible text implied by accepted edits; false if the destination is short. */
bool translate_draft_visible(const TranslateDraft *d, char *out, usize cap);
#endif
