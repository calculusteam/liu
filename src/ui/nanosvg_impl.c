/*
 * Liu — the one translation unit that compiles nanosvg.
 *
 * nanosvg is a single-header library: exactly one file in the program defines
 * the implementation macros, and every other file includes the declarations
 * only. Keeping that file apart from the icon caches lets a target that parses
 * an SVG (the file viewer's SVG view, ui/svg_raster.c) link nanosvg without the
 * Lucide icon cache and its generated data; lucide.c, seti_icons.c,
 * agent_icons.c and svg_raster.c include the headers and link this.
 *
 * NANOSVG_ALL_COLOR_KEYWORDS: every CSS colour name, not the short built-in
 * list. The icon sets only use hex, but a user's SVG says "steelblue".
 */
#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
