/*
 * A `![[Other Note]]` embed renders another note inline. Each embedded
 * document is parsed into its own arena and retained until its modification
 * time changes. The arena owns the file bytes and AST, so dropping the cache
 * entry frees both.
 */
#ifndef UI_MARKDOWN_MD_EMBED_H
#define UI_MARKDOWN_MD_EMBED_H

#include "core/types.h"
#include "ui/markdown/md_ast.h"

typedef struct MdEmbedCache MdEmbedCache;

MdEmbedCache *md_embed_cache_create(void);
void          md_embed_cache_destroy(MdEmbedCache *c);

/* Where a transclusion may read FROM. An embed target is OPENED first and the
 * canonical name of what was opened (the kernel's own, off the descriptor, so
 * a symlink cannot point out and cannot be swapped between the check and the
 * read either) must sit under one of the roots given here — the notes Vault,
 * the folder the explorer is showing, the host document's own folder — and
 * must carry a note extension. Without a root nothing is embedded, so
 * `![[/Users/me/.ssh/id_ed25519]]` in a README from a cloned repo cannot
 * render that file into the page. NULL/empty roots are skipped. */
#define MD_EMBED_MAX_ROOTS 4
void md_embed_cache_set_roots(MdEmbedCache *c, const char *const *roots, u32 count);

/* Returns whether the canonical absolute `resolved` path is inside one of
 * `roots` and has a note extension. */
bool md_embed_path_allowed(const char *resolved, const char *const *roots, u32 count);

/* Start a render pass. Entries not touched during a pass become eviction
 * candidates, so a document that stops embedding a note releases it. */
void md_embed_cache_begin_pass(MdEmbedCache *c);
/* UI caches load on a bounded shared worker. pump() stages results without
 * changing displayed AST pointers; begin_pass adopts them for a new frame. */
void md_embed_cache_enable_async(MdEmbedCache *c, void (*wake)(void));
bool md_embed_cache_completion_ready(const MdEmbedCache *c);
bool md_embed_cache_pump(MdEmbedCache *c);
u64 md_embed_cache_revision(const MdEmbedCache *c);

/* Resolve `target` (a wikilink path, no anchor) against `base_dir` and return
 * its parsed document, or NULL when it cannot be read. `out_path` receives the
 * resolved absolute-ish path for display; pass NULL if not needed. */
const MdDoc *md_embed_cache_get(MdEmbedCache *c, const char *base_dir,
                                const u8 *target, u32 target_len,
                                char *out_path, usize out_path_cap);

/* Why a get() returned NULL. The reader prints a different line for each:
 * "not found" invites a fix to the link, "too large" says the note is there
 * and the embed was refused. */
typedef enum {
    MD_EMBED_OK = 0,
    MD_EMBED_NOT_FOUND,     /* unreadable, outside every root, or not a note */
    MD_EMBED_TOO_LARGE,     /* past MD_EMBED_MAX_BYTES, or its parse did not fit
                             * the largest arena the cache will give one embed */
    MD_EMBED_LOADING,
} MdEmbedStatus;
const MdDoc *md_embed_cache_get_ex(MdEmbedCache *c, const char *base_dir,
                                   const u8 *target, u32 target_len,
                                   char *out_path, usize out_path_cap,
                                   MdEmbedStatus *out_status);

/* What the cache holds right now: the parse arenas' USED bytes (what is
 * resident — the reservation is demand-paged and up to ~20x larger) and the
 * reservation itself. For the ledger (MEM_EDITOR, beside the host
 * document's own arena, which is charged the same way). */
usize md_embed_cache_used_bytes(const MdEmbedCache *c);
usize md_embed_cache_reserved_bytes(const MdEmbedCache *c);

/* Set the clock used for cache recheck intervals; pass `NULL` to restore the
 * monotonic clock. Served notes are re-statted at `MD_EMBED_RECHECK_SEC`;
 * missing notes are retried at `MD_EMBED_MISSING_SEC`. */
#define MD_EMBED_RECHECK_SEC 0.5
#define MD_EMBED_MISSING_SEC 1.0
void md_embed_set_clock(f64 (*now_sec)(void));

/* Select the slice of `doc->blocks` an anchor refers to.
 *   - no anchor           → the whole document
 *   - "#Heading"          → that heading and everything under it, stopping at
 *                           the next heading of the same or higher level
 *   - "^block-id"         → the single block carrying that id
 * Returns false when the anchor names nothing in the document. */
bool md_embed_section(const MdDoc *doc, const u8 *anchor, u32 anchor_len,
                      bool anchor_is_block, u32 *out_first, u32 *out_count);

#endif /* UI_MARKDOWN_MD_EMBED_H */
