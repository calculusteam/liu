/*
 * Liu - Markdown parser.
 *
 * Parses a Markdown document into an MdDoc AST. The
 * caller owns the parse arena and the source byte buffer; both must
 * outlive the returned doc. Slices in the AST point directly into the
 * source — no per-string copies.
 */
#ifndef UI_MARKDOWN_PARSE_H
#define UI_MARKDOWN_PARSE_H

#include <stdatomic.h>
#include "core/types.h"
#include "core/memory.h"
#include "ui/markdown/md_ast.h"

/* Parse `len` bytes of UTF-8 markdown into a doc allocated from `arena`.
 * `source` must remain valid for the lifetime of the returned doc.
 * `base_dir` is captured for image-path resolution; may be NULL. Returns
 * NULL on invalid input, failure to start, or pre-cancellation. Empty input
 * returns a doc with zero blocks. An incomplete doc must not be accepted as
 * a successful prefix: parse_limited marks work/vector/cancellation refusal. */
MdDoc *md_parse(Arena *arena, const u8 *source, usize len, const char *base_dir);

/* The same bounded parser, with cancellation polled inside scans. The flag
 * must remain valid for the call; NULL is allowed. */
MdDoc *md_parse_ex(Arena *arena, const u8 *source, usize len, const char *base_dir,
                    const _Atomic bool *cancel);

/* Convenience accessor for scroll clamping. */
u32 md_block_count(const MdDoc *doc);

#endif /* UI_MARKDOWN_PARSE_H */
