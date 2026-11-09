/*
 * Liu — native Mermaid diagram engine, shared by the markdown viewer and the
 * diagrams drawn over an agent's output in a terminal pane.
 *
 * Diagram types drawn natively; everything else is deliberately left to the
 * code-block fallback:
 *
 *   flowchart   `graph TD|TB|LR|BT|RL` / `flowchart …`
 *               shapes [rect] (round) ([stadium]) ((circle)) {diamond}
 *               [[subroutine]] [(cylinder)] {{hexagon}} [/parallelogram/],
 *               links --> --- -.-> -.- ==> === --x --o <--> with labels in
 *               either spelling (`-->|text|`, `-- text -->`, `-. t .->`,
 *               `== t ==>`), `A & B --> C` fan-outs, `:::class` suffixes,
 *               `subgraph id [Title] … end` as a labelled enclosing box that
 *               a link may also start or end at, `direction`, quoted labels,
 *               `<br/>` row breaks and Mermaid's entity codes.
 *   sequence    `sequenceDiagram` — participant/actor (with `as` aliases),
 *               messages -> --> ->> -->> -x --x -) --) <<->>, activation via
 *               +/- and activate/deactivate, Note left of/right of/over,
 *               loop/alt/else/opt/par/and/critical/option/break/rect frames,
 *               autonumber.
 *   state       `stateDiagram` / `stateDiagram-v2` — [*] start dot and end
 *               ring per scope, `state "desc" as id`, `id : desc`,
 *               `A --> B : label`, composite `state X { … }` as a box,
 *               <<choice>> / <<fork>> / <<join>>, `direction`.
 *   class       `classDiagram` — name + member compartments from `class X {…}`
 *               and `X : member`, `~T~` generics, <<stereotype>>, relations
 *               <|-- *-- o-- --> ..> ..|> with optional `: label`.
 *   er          `erDiagram` — entities with attribute blocks, relationships
 *               with crow's-foot ends and a label.
 *   pie         `pie [showData] [title …]` — slices and a legend.
 *
 * Graph kinds are laid out in layers: ranks with every edge two ranks long, a
 * dummy node at each rank a long edge passes (the middle one carries the
 * label, so labels get room of their own), several barycentre sweeps that
 * keep each subgraph contiguous and keep the ordering with the fewest
 * crossings, and cross-axis positions solved as a least-squares fit to each
 * node's neighbours under the separation constraints. Edges are smooth curves
 * through their dummies, clipped to the shape they meet, with filled heads.
 *
 * The parse and the layout run ONCE per (source, cell size): the result is an
 * MdDiagram, a flat list of boxes, stroke segments and glyphs in diagram
 * space, and drawing it at any scale is a walk over that list. Colours are
 * roles resolved at draw time, so a theme change needs no rebuild.
 *
 * Caps — 128 nodes, 256 edges, 24 participants, 128 messages, 16 subgraphs,
 * 2048 lines, 32 KB of source — truncate to NONE rather than draw a diagram
 * that is not the reader's: the caller then shows the source as code.
 *
 * Single-threaded: the parser and layout keep their working state in file
 * statics. Every caller is on the main thread.
 */
#ifndef UI_MARKDOWN_MD_DIAGRAM_H
#define UI_MARKDOWN_MD_DIAGRAM_H

#include "core/types.h"
#include "core/config.h"     /* Theme */
#include "renderer/renderer.h"

typedef enum {
    MD_DIAGRAM_NONE = 0,     /* unsupported — caller renders raw code instead */
    MD_DIAGRAM_FLOWCHART,
    MD_DIAGRAM_SEQUENCE,
    MD_DIAGRAM_STATE,
    MD_DIAGRAM_CLASS,
    MD_DIAGRAM_ER,
    MD_DIAGRAM_PIE,
    MD_DIAGRAM_GANTT,
    MD_DIAGRAM_MINDMAP,
    MD_DIAGRAM_TIMELINE,
    MD_DIAGRAM_GITGRAPH,
    MD_DIAGRAM_XYCHART,
    MD_DIAGRAM_QUADRANT,
    MD_DIAGRAM_JOURNEY
} MdDiagramKind;

/* The language a source is written in. Graphviz DOT and PlantUML are read as
 * the Mermaid of the same diagram and drawn by the same layouts. */
typedef enum {
    MD_DIAGRAM_LANG_MERMAID = 0,
    MD_DIAGRAM_LANG_GRAPHVIZ,
    MD_DIAGRAM_LANG_PLANTUML
} MdDiagramLang;

/* "Mermaid", "Graphviz", "PlantUML". */
const char *md_diagram_lang_name(MdDiagramLang lang);
/* The language a source is written in, from its first statement: Mermaid
 * when it is no other. */
MdDiagramLang md_diagram_lang_detect(const u8 *src, u32 len);

/* Classify. Parses the body too: a recognised header over a body that cannot
 * be drawn reports NONE, so the fallback never renders an empty frame. */
MdDiagramKind md_diagram_probe(const u8 *src, u32 len);

/* What a diagram kind is called, for a caption ("flowchart", "sequence"…). */
const char *md_diagram_kind_name(MdDiagramKind kind);

/* Is `line` (one trimmed source line, not NUL-terminated) a header this
 * engine draws? The terminal's detector asks it of a candidate row. */
bool md_diagram_header_line(const u8 *line, u32 len);

/* ---- built diagrams ---- */

typedef struct MdDiagram MdDiagram;

/* Parse and lay out `src` with text in a `cw` x `ch` cell. NULL when the
 * source is not a drawable diagram or a cap was hit. */
MdDiagram    *md_diagram_create(const u8 *src, u32 len, f32 cw, f32 ch);
/* The same with less room around and between shapes, for a diagram that has
 * to fit the rows it was given; text keeps its size. */
typedef enum { MD_DIAGRAM_READING = 0, MD_DIAGRAM_COMPACT } MdDiagramSpacing;
MdDiagram    *md_diagram_create_ex(const u8 *src, u32 len, f32 cw, f32 ch, MdDiagramSpacing spacing);
void          md_diagram_destroy(MdDiagram *d);
MdDiagramKind md_diagram_kind_of(const MdDiagram *d);
MdDiagramLang md_diagram_lang_of(const MdDiagram *d);
/* Size at scale 1, in pixels, margins included. */
void          md_diagram_extent(const MdDiagram *d, f32 *w, f32 *h);
/* Bytes this diagram holds, for the memory ledger. */
usize         md_diagram_bytes(const MdDiagram *d);
/* How many primitives one draw submits (boxes, stroke segments, glyphs). */
void          md_diagram_counts(const MdDiagram *d, u32 *boxes, u32 *strokes, u32 *glyphs);

/* The colours a diagram is drawn in. */
typedef struct {
    Color surface;           /* what the diagram sits on */
    Color text, text_dim;
    Color node_fill, node_stroke;
    Color edge, chip;
    Color group_fill, group_stroke, group_text;
    Color note_fill, note_stroke;
    Color accent, activation, solid;
    Color slice[8];
    Color soft[8];           /* the slices washed into the surface: fills text sits on */
} MdDiagramInks;

/* Inks for `theme` (NULL: a neutral dark set) on `surface`, with `accent` as
 * the highlight colour. */
void md_diagram_inks(const Theme *theme, Color surface, Color accent, MdDiagramInks *out);

/* Draw with the top-left at (x, y), every length times `scale`, every colour's
 * alpha times `alpha`. Leaves the renderer's UI glyph cell at the diagram's
 * scaled cell: the caller puts its own back. */
void md_diagram_draw(Renderer *r, const MdDiagram *d, const MdDiagramInks *inks,
                     f32 x, f32 y, f32 scale, f32 alpha);

/* ---- the markdown viewer's interface ----
 *
 * Pure functions of (src, len, avail_w, cw, ch): a diagram wider than the
 * column is scaled down to fit, and one that would need a scale below 2% is
 * refused (0x0) because every shape would be sub-pixel. Built diagrams are
 * kept in a small cache keyed by the source's hash and the cell, so a
 * document repainting does not parse again. */

/* Lay out and report the drawn size in px (0×0 if unsupported/empty). */
void md_diagram_measure(const u8 *src, u32 len, f32 avail_w, f32 cw, f32 ch,
                        f32 *out_w, f32 *out_h);

/* Draw with top-left at (x, y), and put the caller's glyph cell back. */
void md_diagram_render(Renderer *r, const Theme *theme, const u8 *src, u32 len,
                       f32 x, f32 y, f32 avail_w, f32 cw, f32 ch);

/* Drop every cached diagram (memory pressure, teardown). */
void md_diagram_cache_clear(void);

#endif /* UI_MARKDOWN_MD_DIAGRAM_H */
