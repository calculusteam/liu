/*
 * Liu — minimal native TeX-math layout/renderer for the markdown viewer.
 *
 * A self-contained TeX *subset* engine: no external deps, no JS. Lays a
 * formula into a TeX-style box tree (glyph / hlist / fraction / script / sqrt /
 * kern) over an internal scratch arena, then emits glyphs + rules through the
 * renderer. Unicode codepoints (Greek, operators) rasterize on demand via the
 * font atlas, so no math font is required.
 *
 * Supported v1: ^ _ (incl. nested + combined x_i^2), { } grouping, \frac,
 * \sqrt (and \sqrt[n]), Greek letters, common operators/relations, \,\;\quad
 * spacing, upright function names (\sin \cos \log …). Unknown \cmd renders as
 * its literal name so nothing is silently dropped. Out of scope: matrices,
 * alignment, stacked big-operator limits, auto-sized delimiters, accents.
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
