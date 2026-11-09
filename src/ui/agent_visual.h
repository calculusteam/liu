/*
 * Liu — what an agent drew: a diagram, a formula or an SVG picture
 *
 * The agent-output cards (ui/agent_diagram.h) and their large viewer show
 * three kinds of block, told apart by the source alone, as ui/mermaid_scan.h
 * writes it:
 *
 *   - display math: TeX between a `$$` line and another, laid out by the
 *     markdown reader's math engine (ui/markdown/md_math.h);
 *   - an SVG document (`<svg …` / `<?xml …`), drawn to pixels by
 *     ui/svg_raster.h on a thread of its own — nanosvg can take seconds on a
 *     hostile document, so it never runs on the caller's — and its `<text>`
 *     elements, which nanosvg does not draw, laid over the picture in the
 *     terminal's face where their transforms are only moves and scales;
 *   - anything else: a diagram for ui/markdown/md_diagram.h (Mermaid,
 *     Graphviz DOT, PlantUML).
 *
 * Every size is in pixels at the cell the visual was made for, as
 * md_diagram's are, and a draw scales them by `s`. An SVG's CSS pixel is
 * the cell's width over 8.4 (a 14 pt face's advance in points).
 */
#ifndef UI_AGENT_VISUAL_H
#define UI_AGENT_VISUAL_H

#include "core/types.h"
#include "renderer/renderer.h"
#include "ui/markdown/md_diagram.h"

typedef enum { AGV_DIAGRAM = 0, AGV_MATH, AGV_SVG } AgvType;

typedef struct AgentVisual AgentVisual;

/* What a source is: math between `$$` lines, an SVG document, else a diagram. */
AgvType agv_type_of(const char *src, u32 len);

/* NULL when it cannot be drawn: a diagram the engine refuses, TeX that is a
 * whole document or no math at all, an SVG with no `<svg` element. A
 * `compact` visual is a card's: a diagram's compact spacing, an SVG drawn at
 * its own size (the viewer's twice that). */
AgentVisual *agv_create(const char *src, u32 len, f32 cw, f32 ch, bool compact);
/* The same, for a picture never drawn larger than `fit_edge` pixels on its
 * longer side (a card in a window that size): an SVG is rasterised no larger. */
AgentVisual *agv_create_fit(const char *src, u32 len, f32 cw, f32 ch, bool compact, f32 fit_edge);
void    agv_destroy(AgentVisual *v);
AgvType agv_type(const AgentVisual *v);
/* A formula set in text style (limits beside their operator: a sum stands two
 * rows lower) or in display style, the default; the extent follows. Other
 * visuals keep theirs. */
void    agv_set_text_style(AgentVisual *v, bool on);
void    agv_extent(const AgentVisual *v, f32 *w, f32 *h);
usize   agv_bytes(const AgentVisual *v);

/* "Mermaid · flowchart", "Graphviz · flowchart", "LaTeX · formula",
 * "SVG · 200 × 80". */
void agv_title(const AgentVisual *v, char *out, u32 cap);

/* An SVG still being drawn, and one that could not be (with why). */
bool        agv_pending(const AgentVisual *v);
const char *agv_failure(const AgentVisual *v);

/* Draw at (x, y), every size times `s`. */
void agv_draw(Renderer *r, const AgentVisual *v, const MdDiagramInks *inks,
              f32 x, f32 y, f32 s, f32 alpha);

/* The SVG worker calls `wake` when a drawing is done, and the revision moves:
 * the loop paints every window once when it sees it move. */
void agv_set_wake(void (*wake)(void));
u64  agv_revision(void);

/* ---- for the tests ---- */

/* The TeX of a math source, NUL-terminated in `out`: false when it is no
 * formula (a whole document, or no math in it). */
bool agv_math_tex(const char *src, u32 len, char *out, u32 cap, u32 *out_len);

typedef struct {
    f32  x, y;               /* the anchor, in the document's CSS pixels */
    f32  size;               /* font size, CSS pixels */
    u8   anchor;             /* 0 start, 1 middle, 2 end */
    u8   baseline;           /* 0 alphabetic, 1 middle, 2 hanging */
    bool bold;
    u32  rgba;               /* 0: none */
    char text[160];
} AgvText;

/* The document's size in CSS pixels and its text runs, at most `cap`. False
 * when there is no `<svg` element. */
bool agv_svg_layout(const char *src, u32 len, f32 *doc_w, f32 *doc_h,
                    AgvText *texts, u32 cap, u32 *ntexts);

#endif /* UI_AGENT_VISUAL_H */
