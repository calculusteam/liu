/*
 * Liu — minimal native Mermaid diagram engine for the markdown viewer.
 *
 * v1 supports flowcharts (`graph TD|LR|BT|RL` / `flowchart …`): node shapes
 * [rect] (round) ((circle)) {diamond}, edges --> --- -.-> ==> with optional
 * |labels|. Layered (longest-path) layout + straight clipped edges with
 * arrowheads, drawn with the renderer's rrect/line/glyph batches. Everything
 * else (sequence/class/state/gantt/…) probes as NONE so the caller falls back
 * to rendering the raw fenced source as a code block.
 *
 * Pure functions of (src, len, avail_w, cw, ch) so the markdown layout cache
 * stays coherent.
 */
#ifndef UI_MARKDOWN_MD_DIAGRAM_H
#define UI_MARKDOWN_MD_DIAGRAM_H

#include "core/types.h"
#include "core/config.h"     /* Theme */
#include "renderer/renderer.h"

typedef enum {
    MD_DIAGRAM_NONE = 0,     /* unsupported — caller renders raw code instead */
    MD_DIAGRAM_FLOWCHART
} MdDiagramKind;

/* Cheap classification from the first keyword. */
MdDiagramKind md_diagram_probe(const u8 *src, u32 len);

/* Lay out and report the drawn size in px (0×0 if unsupported/empty). */
void md_diagram_measure(const u8 *src, u32 len, f32 avail_w, f32 cw, f32 ch,
                        f32 *out_w, f32 *out_h);

/* Draw with top-left at (x, y). */
void md_diagram_render(Renderer *r, const Theme *theme, const u8 *src, u32 len,
                       f32 x, f32 y, f32 avail_w, f32 cw, f32 ch);

#endif /* UI_MARKDOWN_MD_DIAGRAM_H */
