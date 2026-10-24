/*
 * Liu — minimal native TeX-math layout/renderer for the markdown viewer.
 *
 * A self-contained TeX *subset* engine: no external deps, no JS. Lays a
 * formula into a TeX-style box tree (glyph / hlist / fraction / script / sqrt /
 * fence / accent / limits / matrix / kern) over an internal scratch arena, then
 * emits glyphs + rules through the renderer. Unicode codepoints (Greek,
 * operators) rasterize on demand via the font atlas, so no math font is
 * required.
 *
 * Supported: ^ _ (incl. nested + combined x_i^2), { } grouping, \frac, \sqrt
 * (and \sqrt[n]), Greek letters, common operators/relations, \,\;\quad spacing,
 * upright function names (\sin \cos \log …), plus:
 *   - grid environments — \begin{X} … \end{X} for matrix, pmatrix, bmatrix,
 *     Bmatrix, vmatrix, Vmatrix, smallmatrix, cases, array, aligned/align,
 *     split and gather. Cells split on '&', rows on '\\'; columns take the
 *     width of their widest cell and share one baseline per row.
 *   - \left … \right auto-sized delimiters, including the invisible "." forms,
 *     nested. The delimiter grows with the content it encloses.
 *   - accents — \hat \widehat \check \tilde \widetilde \bar \vec \dot \ddot
 *     \breve \acute \grave \mathring, and \overline, which is a rule spanning
 *     the nucleus. They compose with scripts (\hat{x}^2).
 *   - big operators — \sum \prod \coprod \bigcup \bigcap \bigvee \bigwedge
 *     \bigoplus … and \lim-style upright names. In display mode the operator
 *     is drawn larger and its scripts stack above/below it; \int and \oint keep
 *     side scripts even in display mode, per TeX convention. Inline mode always
 *     uses side scripts.
 * Unknown \cmd renders as its literal name so nothing is silently dropped.
 *
 * Out of scope: the \over / \atop primitives, per-column alignment specs (the
 * \begin{array} spec is skipped and columns centre), horizontal stretching of
 * wide accents and \substack, macro definition (\newcommand), and text/style
 * switches (\text, \mathrm, \mathbb — the name prints and its argument is laid
 * out as ordinary math).
 *
 * Bounded by construction: grids truncate at 64 rows x 32 columns, nesting at
 * 64 levels, and the scratch arena is fixed — malformed input can only truncate
 * a formula, never overrun, hang or crash.
 *
 * Both calls are pure functions of (tex, len, cw, ch, display) so the markdown
 * block layout cache stays coherent.
 */
#ifndef UI_MARKDOWN_MD_MATH_H
#define UI_MARKDOWN_MD_MATH_H

#include "core/types.h"
#include "renderer/renderer.h"

/* Measure a formula. Fills width / ascent (above baseline) / descent (below)
 * in pixels. Total vertical extent == ascent + descent. */
void md_math_measure(const u8 *tex, u32 len, f32 cw, f32 ch, bool display,
                     f32 *out_w, f32 *out_ascent, f32 *out_descent);

/* Render with the formula's baseline at (x, baseline_y), left edge at x. */
void md_math_render(Renderer *r, const u8 *tex, u32 len,
                    f32 x, f32 baseline_y, f32 cw, f32 ch, bool display, Color fg);

#endif /* UI_MARKDOWN_MD_MATH_H */
