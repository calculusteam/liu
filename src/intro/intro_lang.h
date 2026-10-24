#ifndef LIU_INTRO_LANG_H
#define LIU_INTRO_LANG_H

/* Which language the first-run intro's captions are in.
 *
 * The intro is the one place the app is not English-only: its captions are in
 * the reader's language for every language listed in INTRO_LANG_CODES (in the
 * generated intro_geom.h), with one caption atlas baked per entry
 * (scripts/gen_intro_assets.py). English is entry 0 and is what every other
 * language gets: there is no second choice, and no mixing.
 *
 * Pure C: a tag in, an index out. intro_macos.m asks the system for the tag. */

#include "core/types.h"
#include "intro/intro_geom.h"

/* The index, into INTRO_LANG_CODES, of the language a tag names: "tr",
 * "tr-TR", "tr_TR.UTF-8", "zh-Hans-CN" (BCP 47 or POSIX, any case). Only the
 * primary subtag is looked at — the list has one Chinese, one Spanish, one
 * Arabic — and anything else is 0, English: a language with no captions, an
 * empty tag, NULL. */
i32 intro_lang_for_tag(const char *tag);

/* The code of language `index` ("en" for one out of range). */
const char *intro_lang_code(i32 index);

#endif /* LIU_INTRO_LANG_H */
