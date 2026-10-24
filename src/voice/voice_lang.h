/*
 * Liu - the languages Whisper can transcribe, with names to show a human.
 *
 * The CODES already exist, in `whisper_vocab.c`'s g_lang_codes — but that table
 * is in the model's own token order (index 0 is <|en|>, and a language token is
 * sot+1+index), which is load-bearing there and useless for a picker. This one
 * carries the same set sorted by display name, which is the only order a list
 * of a hundred languages can be read in.
 *
 * The two are kept apart rather than bolting names onto the decoder's table:
 * that array's ORDER is part of the model format and must never change for a UI
 * reason. Both tables must hold exactly the same set of codes, so a language
 * cannot be added to one and forgotten in the other.
 */
#ifndef VOICE_VOICE_LANG_H
#define VOICE_VOICE_LANG_H

#include "core/types.h"

i32         voice_lang_count(void);

/* ISO code ("tr") and display name ("Turkish") for a display-order index.
 * Return "" when the index is out of range. */
const char *voice_lang_code(i32 index);
const char *voice_lang_name(i32 index);

/* Display name for a code, or NULL when the code is not one Whisper knows.
 * An empty code means auto-detect and also returns NULL — callers label that
 * themselves, since "Auto" is a UI concept and not a language. */
const char *voice_lang_name_for_code(const char *code);

/* Display index of `code`, or -1. */
i32         voice_lang_index_for_code(const char *code);

#endif /* VOICE_VOICE_LANG_H */
