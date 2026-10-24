/*
 * Liu — SVG to pixels, for the file viewer's SVG view.
 *
 * One function in, one RGBA buffer out, and nothing else: no globals, no UI, no
 * renderer. The viewer's decode worker calls it off the main thread.
 *
 * What is drawn is what nanosvg draws (the copy in third_party/nanosvg): paths,
 * rects, circles, ellipses, lines, polylines, polygons, groups, transforms,
 * solid and gradient fills, strokes and dashes, opacity, the simple class rules
 * of a <style> block, every CSS colour name. What it does not: <text> (nothing
 * is drawn for it), <use>, clip paths, masks, filters, <image>, <foreignObject>.
 * A document that leans on those still draws — less of it. The viewer keeps the
 * source one toggle away.
 *
 * An SVG is untrusted text from wherever the user's folders came from. The
 * rasteriser never opens a second file and never touches the network: it has no
 * element that reads one (<image> and <use> are not among those it handles), and
 * this module hands it a buffer, not a path. What is bounded is the cost of a
 * hostile one — its size (SvgLimits.max_file_bytes), the pixels it may produce
 * (long_edge, max_rgba_bytes) and the numbers it declares (a width of 1e30 is
 * "empty", not a 30-order-of-magnitude scale).
 */
#ifndef UI_SVG_RASTER_H
#define UI_SVG_RASTER_H

#include "core/types.h"

typedef struct {
    usize max_file_bytes;   /* a document over this is refused, unread past it */
    i32   long_edge;        /* the raster's longer side in pixels; the other follows the aspect */
    usize max_rgba_bytes;   /* the raster may not take more than this */
    f64   max_seconds;      /* the rasteriser is stopped after this long (0: SVG_DEADLINE_SECONDS) */
} SvgLimits;

/* The file-size limit bounds the SVG input passed to NanoSVG. Larger
 * documents can require excessive parsing time. */
#define SVG_DEFAULT_MAX_FILE_BYTES ((usize)8 * 1024 * 1024)

/* nanosvg offers no way to interrupt it, and a document can make it run for
 * minutes or take gigabytes. Two layers stand between that and the viewer.
 *
 * The first is the ESTIMATE below, which spares the time and picks a size. It
 * is a model, and a geometry it was not calibrated against can get past it (a
 * dash too short for a float to step by; curves that cross a row three times;
 * round caps on a line too wide for a float). The second is the guarantee, a
 * Liu edit in nanosvg: its loops ask a deadline and an edge cap, and give up
 * (SVG_ERR_TOO_COMPLEX). SVG_DEADLINE_SECONDS is twice the budget; a drawing
 * the model passed can take at most that long, whatever it is. What follows
 * is the model.
 *
 * nanosvg's cost is the square of the edges crossing a scanline (a few
 * thousand overlapping strokes) and a dashed stroke is cut into one piece per
 * dash. Both are estimated from the parsed geometry before anything is drawn,
 * as seconds of CPU, together with the edges it will make; a drawing over
 * either budget is drawn at half the size, and again, down to
 * SVG_MIN_LONG_EDGE, and then refused. A line width or coordinate beyond a
 * million pixels is refused outright (a ROUND cap on a line that wide is a
 * loop of billions of edges). The estimate is approximate; the deadline is
 * what bounds the worst case. */
#define SVG_BUDGET_SECONDS 3.0
#define SVG_EDGE_BUDGET    6.0e6        /* edges of the biggest shape, ~20 bytes each: ~120 MB */
#define SVG_MIN_LONG_EDGE  512
#define SVG_DEADLINE_SECONDS 6.0
#define SVG_HARD_EDGE_CAP  12000000     /* edges of one shape at once, the model's budget twice over: ~240 MB */

typedef enum {
    SVG_OK = 0,
    SVG_ERR_READ,           /* cannot be opened or read, or is not a regular file */
    SVG_ERR_TOO_LARGE,      /* over max_file_bytes */
    SVG_ERR_NOT_SVG,        /* no <svg element, or nanosvg refused it */
    SVG_ERR_EMPTY,          /* parsed, but no usable size: none declared and no shapes to size it by, or not a finite number */
    SVG_ERR_MEMORY,         /* the raster would not fit max_rgba_bytes, or allocation failed */
    SVG_ERR_TOO_COMPLEX,    /* over the budget even at SVG_MIN_LONG_EDGE (not attempted), or stopped at the deadline or the edge cap */
} SvgStatus;

typedef struct {
    u8   *rgba;             /* premultiplied, w*h*4, malloc'd: the caller free()s it */
    i32   w, h;
    f32   doc_w, doc_h;     /* the document's own size in CSS px, before scaling */
    /* The ink is mostly dark (alpha-weighted mean luminance under one half, taken
     * from the colours as drawn): on a dark panel it needs a light backing to be
     * seen at all. False for a blank drawing. */
    bool  dark_ink;
    /* The cost estimated BEFORE drawing, in seconds, and the long edge actually
     * used — less than asked when the drawing was too heavy to draw at that. */
    f64   est_seconds;
    i32   long_edge_used;
} SvgImage;

/* Rasterise `len` bytes of SVG at `lim->long_edge` pixels on the longer side
 * (up as well as down: it is a vector). `data` is not modified. On anything but
 * SVG_OK `out` is zeroed. */
SvgStatus svg_rasterize(const u8 *data, usize len, const SvgLimits *lim, SvgImage *out);

/* The same from a file: a regular file only (a FIFO or device is refused, not
 * waited on), read no further than max_file_bytes + 1. */
SvgStatus svg_rasterize_file(const char *path, const SvgLimits *lim, SvgImage *out);

/* A few words for the notice that says why a document could not be drawn. */
const char *svg_status_text(SvgStatus s);

#endif /* UI_SVG_RASTER_H */
