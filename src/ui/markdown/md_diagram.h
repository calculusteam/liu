/*
 * Liu — minimal native Mermaid diagram engine for the markdown viewer.
 *
 * Four diagram types are drawn natively; everything else is deliberately left
 * to the code-block fallback:
 *
 *   flowchart   `graph TD|TB|LR|BT|RL` / `flowchart …`
 *               shapes [rect] (round) ([stadium]) ((circle)) {diamond}
 *               [[subroutine]] [(cylinder)] {{hexagon}} [/parallelogram/],
 *               links --> --- -.-> -.- ==> === --x --o <--> with labels in
 *               either spelling (`-->|text|`, `-- text -->`, `-. t .->`,
 *               `== t ==>`), `subgraph … end` as a labelled enclosing box,
 *               `direction`, quoted labels and <br/> row breaks.
 *   sequence    `sequenceDiagram` — participant/actor (with `as` aliases),
 *               messages -> --> ->> -->> -x -), activation via +/- and
 *               activate/deactivate, Note left of/right of/over. Grouping
 *               keywords (loop/alt/opt/par/…) are skipped, not drawn.
 *   state       `stateDiagram` / `stateDiagram-v2` — [*] start dot and end
 *               ring, `state "desc" as id`, `A --> B : label`, `direction`.
 *   class       `classDiagram` — name + member compartments from `class X {…}`
 *               and `X : member`, relations <|-- *-- o-- --> ..> ..|> with
 *               optional `: label`; cardinality strings are skipped.
 *
 * Layout is layered (longest-path ranks over a DAG-ified graph, one barycentre
 * pass per rank) for the graph kinds and a lifeline grid for sequences, with
 * straight clipped edges and polyline shape outlines drawn through the
 * renderer's rrect/line/glyph batches. A diagram wider than the column is
 * uniformly scaled down to fit; if fitting would need a scale below 2% the
 * diagram is REFUSED (probes as NONE) rather than clamped, because at that
 * size every shape is sub-pixel and a clamped diagram would still overflow
 * the column it claimed to fit. The source is shown as code instead.
 *
 * Anything else — gantt, pie, erDiagram, journey, mindmap, gitGraph,
 * quadrantChart, prose — probes as NONE, and so does a *recognised* header
 * whose body yields nothing drawable or trips one of the internal caps (128
 * nodes, 256 edges, 24 participants, 128 messages, 16 subgraphs, 2048 lines,
 * 32 KB of source). The caller then renders the raw fenced source as code,
 * which always beats an empty box.
 *
 * Pure functions of (src, len, avail_w, cw, ch) so the markdown layout cache
 * stays coherent. Single-threaded: the parsed model lives in file statics.
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
    MD_DIAGRAM_CLASS
} MdDiagramKind;

/* Classify. Parses the body too: a recognised header over a body that cannot
 * be drawn reports NONE, so the fallback never renders an empty frame. */
MdDiagramKind md_diagram_probe(const u8 *src, u32 len);

/* Lay out and report the drawn size in px (0×0 if unsupported/empty). */
void md_diagram_measure(const u8 *src, u32 len, f32 avail_w, f32 cw, f32 ch,
                        f32 *out_w, f32 *out_h);

/* Draw with top-left at (x, y). */
void md_diagram_render(Renderer *r, const Theme *theme, const u8 *src, u32 len,
                       f32 x, f32 y, f32 avail_w, f32 cw, f32 ch);

#endif /* UI_MARKDOWN_MD_DIAGRAM_H */
