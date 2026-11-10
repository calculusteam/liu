/*
 * Liu — Mermaid blocks in an agent's output
 *
 * An agent asked for a diagram prints a Mermaid source the way its own
 * renderer shows a fenced code block. Claude Code prints the fence's language
 * on a dim row of its own ("mermaid") and the code under it, verbatim, with
 * every row longer than its box word-wrapped by the agent — on the alternate
 * screen, where nothing marks a row as wrapped. Others print the ``` fences,
 * or the code alone, starting at its header row (`flowchart TD`).
 *
 * This module answers, from the cells alone, where such a block is on the
 * screen, whether it is whole, and what its source was: the rows from the
 * header on, the block's left column taken off, and each row the agent
 * wrapped joined back onto its line. A row is a continuation when it starts
 * at the block's own column (the agent's wrap puts it there; a statement of
 * an indented source starts further in) and either the line before it is
 * still open — an unclosed bracket or quote, a trailing link operator — or
 * its first word would not have fitted at the end of the row before it.
 *
 * A block is WHOLE when something after it says so: a closing fence, prose
 * at the block's column, or the agent's own chrome below it after a blank
 * row while the agent is not working. Rows running off the screen, or an
 * open subgraph or frame, leave it unfinished, and an unfinished block is
 * not drawn.
 *
 * A block taller than the pane is never on screen whole: a full-screen agent
 * scrolls its own transcript under a fixed input box. The TAPE (below) keeps
 * the rows of the agent's output the pane has shown, placed by how far each
 * new picture moved them, so the blocks are looked for in what the pane has
 * shown, not only in what it shows now.
 *
 * Pure: rows come in through a callback and every state is the caller's.
 * The caller summarises each row once (mmd_row_summarize), on the same dirty
 * signal its glyph rows are rebuilt on, and asks mmd_find only when a
 * summary changed.
 */
#ifndef UI_MERMAID_SCAN_H
#define UI_MERMAID_SCAN_H

#include "core/types.h"

#define MMD_MAX_BLOCKS 32
#define MMD_MAX_SRC    (32u * 1024u)

typedef enum {
    MMD_ROW_BLANK = 0,
    MMD_ROW_TEXT,
    MMD_ROW_LABEL,       /* `mermaid` / ```mermaid / ~~~mermaid, and the other languages below */
    MMD_ROW_HEADER,      /* a diagram header the engine draws, `$$`, `\[`, `<svg` */
    MMD_ROW_FENCE        /* a closing ``` / ~~~ */
} MmdRowKind;

/* What a block is: a diagram (Mermaid, Graphviz DOT, PlantUML), display
 * math (TeX between `$$` / `\[ \]`, or under a math / latex / tex label), or
 * an SVG document (`<svg` … `</svg>`). A label or header row carries its
 * type in MmdRow.tag, with MMD_TAG_FENCED on a label that opened a fence. */
typedef enum { MMD_TYPE_DIAGRAM = 0, MMD_TYPE_MATH, MMD_TYPE_SVG } MmdType;
#define MMD_TAG_TYPE   0x0Fu
#define MMD_TAG_FENCED 0x10u

typedef struct {
    u64 hash;            /* the row's text and where it starts; 0 when blank */
    i16 first, last;     /* first / last column holding anything, -1 when blank */
    i16 t0, t1;          /* the same without box drawing at either end */
    u8  kind;            /* MmdRowKind */
    u8  first_word;      /* cells of the row's first word, at most 255 */
    u8  rule;            /* blank, but drawn: box drawing and nothing else */
    u8  tag;             /* a label's or header's MmdType, | MMD_TAG_FENCED */
} MmdRow;

/* Codepoints of one row, `cols` of them: 0 for an empty cell or the second
 * half of a wide character, a cluster already reduced to its base. Box
 * drawing at either end (a bordered code block) is not text: a row is
 * classified by what is between it. */
void mmd_row_summarize(const u32 *cp, i32 cols, MmdRow *out);

/* Fill `cp[0..cols)` with row `row`'s codepoints. False when there is none. */
typedef bool (*MmdRowRead)(void *ctx, i32 row, u32 *cp, i32 cols);

typedef enum {
    MMD_END_SCREEN = 0,  /* ran off the rows given: unfinished */
    MMD_END_FENCE,       /* a closing fence */
    MMD_END_PROSE,       /* text at the block's column that is no statement */
    MMD_END_CHROME,      /* something left of the block's column */
    MMD_END_CLOSED       /* its own last statement: `}`, `@enduml`, `$$`, `</svg>`, a blank after math */
} MmdEnd;

typedef struct {
    i32  row0;           /* first row: the label, or the header */
    i32  head;           /* the header row */
    i32  row1;           /* last row of the block */
    i32  col0;           /* the block's left column */
    i32  col1;           /* rightmost column its rows reach */
    bool complete;
    u8   end;            /* MmdEnd */
    u8   type;           /* MmdType */
    u32  src_off, src_len;
    u64  src_hash;       /* of the rebuilt source */
} MmdBlock;

typedef struct {
    const MmdRow *rows;  /* one per row, rows[0] is row 0 */
    i32  nrows, cols;
    MmdRowRead read;
    void *ctx;
    bool busy;           /* the pane's agent is at work */
    /* The rows end where the agent's own rows begin (its input), not where
     * what has been seen ends: a block that runs to the last row ended at
     * its chrome. */
    bool tail_chrome;
    /* Only blocks with a row in [want_lo, want_lo + want_n) are returned;
     * want_n 0 is every block. */
    i32  want_lo, want_n;
} MmdScreen;

/* Find the blocks among the rows, top to bottom, at most `cap`. Each one's
 * source is written into `src` (`src_cap` bytes in all); a block whose source
 * does not fit is left out. A math block's source is its TeX between a `$$`
 * line and another, however it was written; an SVG's is the document; a
 * diagram's is its text from the header on. Statements of the languages that
 * close themselves (DOT's braces, PlantUML's `@enduml`, `</svg>`, math's
 * closing delimiter) are not judged: the block is whole when it closes. */
u32 mmd_find(const MmdScreen *scr, MmdBlock *out, u32 cap, char *src, u32 src_cap);

/* Rows the scans have walked since the process began: a diagnostic count of
 * the work mmd_find did. Main thread only, like mmd_find's callers. */
u64 mmd_rows_walked(void);

/* How far the rows moved between two pictures of the same pane: the `shift`
 * for which cur[y] reads as old[y + shift] over the longest run of rows, and
 * that run, cur[*run0..*run1]. A run counts by its non-blank rows and needs
 * `need` of them; a tie goes to the smaller move. False when no run does. */
bool mmd_row_shift(const MmdRow *old, i32 old_n, const MmdRow *cur, i32 cur_n, i32 need,
                   i32 *shift, i32 *run0, i32 *run1);

/* A row's cells a..b as UTF-8, the way the scan reads them (an empty cell a
 * space, the second half of a wide character nothing), and back: cells from
 * column `col` on, every other cell 0. */
u32  mmd_row_text(const u32 *cp, i32 a, i32 b, char *out, u32 cap);
void mmd_row_cells(const char *text, u32 len, i32 col, u32 *cp, i32 cols);

/* ---- the tape ----
 *
 * The agent's output as the pane has shown it, a row per tape row. Each
 * picture is placed against the one before by mmd_row_shift: the rows that
 * moved together are the output, and the screen's first row is that many
 * tape rows further on. The agent's own rows are never written into it: the
 * input box (from the cursor's row, and the rule over it), and rows that
 * stayed put while the output moved under them. A blank row at the edge of
 * the output is written only where the tape knows nothing yet — it may be
 * the input box's margin over a row the tape holds. A picture that shares
 * too few rows with the one before starts the tape again. */

#define MMD_TAPE_ROWS 512    /* rows kept; a power of two */

typedef struct {
    i64    idx;              /* the tape row this slot holds */
    u32    gen;              /* the tape it was written for; any other is stale */
    MmdRow sum;
    char  *text;             /* cells sum.first..sum.last, mmd_row_text */
    u32    len, cap;
} MmdTapeRow;

typedef struct {
    MmdTapeRow *slot;        /* MMD_TAPE_ROWS, NULL until the first picture */
    u32    gen;              /* 0 until the first picture */
    i32    rows, cols;
    i64    base;             /* the tape row the screen's first row shows */
    i64    end;              /* after the lowest output seen: the agent's rows */
    MmdRow *prev;            /* the picture the tape was last written from */
    i32    fixed_top, fixed_bot;   /* rows that stayed put while the output moved */
    i32    out0, out1;       /* this picture's output rows, [out0, out1) */
    MmdRow *seg;             /* the known run of tape rows around the picture */
    i64    seg0;
    i32    nseg;
    u32   *cp;               /* one row's codepoints */
    u64    restarts;         /* pictures that shared too few rows with the one before */
} MmdTape;

/* Take a picture: `sum` is every screen row's summary, `read` gives a row's
 * cells, `cursor` the row the agent's cursor is on (-1 when it shows none).
 * False when nothing could be kept (no memory). */
bool mmd_tape_take(MmdTape *t, const MmdRow *sum, i32 rows, i32 cols, i32 cursor,
                   MmdRowRead read, void *ctx);
/* The tape as rows to look for blocks in: the known run around the picture,
 * only blocks on screen wanted. Valid until the next take. */
void mmd_tape_screen(MmdTape *t, MmdScreen *scr, bool busy);
/* The screen row of a row of mmd_tape_screen's (above the top or below the
 * bottom for one that does not show). */
static inline i32 mmd_tape_screen_row(const MmdTape *t, i32 row) {
    return (i32)(t->seg0 + row - t->base);
}
void  mmd_tape_free(MmdTape *t);
usize mmd_tape_bytes(const MmdTape *t);

/* Where a block known whole, by the hashes of its rows `known[0..nknown)`,
 * sits among the rows now on screen when only part of it is: the screen row
 * its first row would be on (possibly above the top or below the bottom),
 * and the visible rows that match, in *vis0..*vis1. False when no run of
 * rows touching the screen's top or bottom matches it. */
bool mmd_locate_known(const MmdRow *rows, i32 nrows, const u64 *known, i32 nknown,
                      i32 *row0, i32 *vis0, i32 *vis1);

u64 mmd_hash_bytes(const char *s, u32 n);

#endif /* UI_MERMAID_SCAN_H */
