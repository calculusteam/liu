/*
 * Liu — SVG to pixels. See svg_raster.h.
 */
#include "ui/svg_raster.h"

#include "core/imgproc.h"

/* Declarations only — the implementation is compiled once, in nanosvg_impl.c. */
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* A declared size past this is not a drawing: it is a number somebody typed to
 * see what would happen. Nothing a viewer shows is a hundred million CSS pixels
 * across, and it keeps `scale` and the products below far from overflow. */
#define SVG_MAX_DECLARED_PX 1.0e8f

const char *svg_status_text(SvgStatus s) {
    switch (s) {
    case SVG_OK:             return "ok";
    case SVG_ERR_READ:       return "cannot be read";
    case SVG_ERR_TOO_LARGE:  return "too large to draw";
    case SVG_ERR_NOT_SVG:    return "not an SVG document";
    case SVG_ERR_EMPTY:      return "declares no size";
    case SVG_ERR_MEMORY:     return "too large for its raster";
    case SVG_ERR_TOO_COMPLEX: return "too complex to draw";
    }
    return "unknown error";
}

/* Does the text hold an `<svg` element start anywhere? An SVG may open with an
 * XML declaration, a DOCTYPE and any number of comments, so there is no fixed
 * window to look in; the scan is linear and the input is bounded. */
static bool svg_has_root(const u8 *data, usize len) {
    static const char needle[] = "<svg";
    return memmem(data, len, needle, sizeof needle - 1) != NULL;
}

/* Is the ink mostly dark? The mean luminance of what is drawn, each pixel
 * weighted by its own alpha, under one half. Over STRAIGHT alpha (nanosvg's
 * output, before the premultiply): the renderer's premultiply works in linear
 * light, so a translucent pixel's premultiplied bytes are not its colour. */
static bool svg_ink_is_dark(const u8 *px, usize pixels) {
    u64 lum = 0, alpha = 0;
    for (usize i = 0; i < pixels; i++) {
        const u8 *p = px + i * 4u;
        if (!p[3]) continue;
        lum += (u64)(2126u * p[0] + 7152u * p[1] + 722u * p[2]) * p[3];     /* 0..255 x 10000, by alpha */
        alpha += (u64)p[3];
    }
    if (!alpha) return false;
    return lum < alpha * 255u * 5000u;      /* < 0.5 of the 255 x 10000 scale */
}

/* What drawing `img` at `scale` onto `rows` rows will cost.
 *
 *   work    the sum over raster rows of (edges crossing that row)^2. nanosvg keeps
 *           the edges that cross a scanline in a list it sorts every sub-scanline,
 *           so a row costs the square of its crossings. A fill puts an edge in
 *           every row a segment spans; a stroke puts four (the outline's sides and
 *           ends), and about two more in every row within the line's half width
 *           of it, in proportion to how sharply the path turns there (the arcs
 *           of round joins, the spikes of miter joins, which reach miterLimit
 *           times as far; a smooth curve has none, a zigzag has one at every
 *           vertex, and the ends of a path are caps). Local geometry (a map:
 *           thousands of short outlines) crosses each row a few hundred times and
 *           costs nothing; one path that wanders over the whole canvas tens of
 *           thousands of times costs seconds.
 *   dashes  how many pieces a dashed stroke is cut into, each with its own caps.
 *           That count is the drawing's, not the raster's: the path and the
 *           pattern scale together, so a smaller picture has as many.
 *   edges   how many edges that is: nanosvg keeps those of one shape in ONE array
 *           (emptied between shapes, and between a shape's fill and its stroke),
 *           so the most any shape makes is its memory, and all of them its time.
 *           A round cap or join is `divs` of them — the count comes from a
 *           formula that collapses to infinity once the line is wider than a
 *           float can tell from its own offset (~1e7 px), which is a loop of
 *           INT_MAX edges and gigabytes.
 *
 * An estimate from the control polygons, not a prediction: parallel edges sort
 * for free (a dense hatch is over-counted), curves cross a row more than once.
 * A coordinate or a line width that is not a plausible number of pixels is not
 * counted, it is refused: past a million pixels nanosvg's fixed-point arithmetic
 * overflows, and nothing drawn by a person is there. Returns false when it could
 * not count. */
#define SVG_SECONDS_PER_WORK 5.0e-11
#define SVG_SECONDS_PER_DASH 1.5e-6
#define SVG_SECONDS_PER_EDGE 2.0e-7
#define SVG_MAX_PX           1.0e6          /* a coordinate or a line width, after scaling */
#define SVG_TESS_TOL         0.25f          /* NSVGrasterizer.tessTol, as nsvgCreateRasterizer sets it */

/* nanosvg's own division of a round cap (nsvg__curveDivs), in the same floats, so
 * that the count here is the count it will use. Infinity when its arithmetic breaks down. */
static f64 svg_round_divs(f32 line_width) {
    const f32 r = line_width * 0.5f;
    const f32 da = acosf(r / (r + SVG_TESS_TOL)) * 2.0f;
    if (!(da > 0.0f) || !isfinite(da)) return HUGE_VAL;
    return ceil(3.14159265358979323846 / (f64)da);       /* a half circle: nanosvg passes NSVG_PI */
}

typedef struct { f64 seconds, edges; } SvgCost;

/* A dash that a float cannot step by is a loop that never advances: the position
 * moves by the dash, the sum rounds back to itself, and the walker comes round
 * again. ULP is ~FLT_EPSILON x the coordinate; a period under this many of them
 * is refused (the longest piece of the pattern is at least period / 16). */
#define SVG_DASH_ULPS 64.0
#define SVG_FLT_EPSILON 1.1920929e-7

/* How far a path turns where one segment meets the next, 0 (straight on) to pi
 * (back on itself). pi when either tangent is undefined: the dearer answer. */
static f64 svg_turn(const f32 *prev, const f32 *next) {
    const f64 px = (f64)(prev[6] - prev[4]), py = (f64)(prev[7] - prev[5]);
    const f64 nx = (f64)(next[2] - next[0]), ny = (f64)(next[3] - next[1]);
    if ((px == 0.0 && py == 0.0) || (nx == 0.0 && ny == 0.0)) return 3.14159265358979323846;
    return atan2(fabs(px * ny - py * nx), px * nx + py * ny);
}

static void svg_rows_add(f64 *delta, i32 rows, f64 lo, f64 hi, f64 weight) {
    if (!(hi >= 0.0) || !(lo < (f64)rows)) return;               /* off the canvas, or not a number */
    const i32 a = lo < 0.0 ? 0 : (i32)lo;
    const i32 b = hi > (f64)(rows - 1) ? rows - 1 : (i32)hi;
    delta[a] += weight;
    delta[b + 1] -= weight;
}

static bool svg_estimate(const NSVGimage *img, f32 scale, i32 rows, SvgCost *cost) {
    const SvgCost refused = { HUGE_VAL, HUGE_VAL };
    f64 *delta = (f64 *)calloc((usize)rows + 2u, sizeof *delta);
    if (!delta) return false;
    f64 dashes = 0.0, edges = 0.0, biggest = 0.0;
    for (const NSVGshape *sh = img->shapes; sh; sh = sh->next) {
        if (!(sh->flags & NSVG_FLAGS_VISIBLE)) continue;
        const bool fill = sh->fill.type != NSVG_PAINT_NONE;
        const bool stroke = sh->stroke.type != NSVG_PAINT_NONE && sh->strokeWidth > 0.0f;
        if (!fill && !stroke) continue;
        const f64 weight = (fill ? 1.0 : 0.0) + (stroke ? 4.0 : 0.0);   /* edges per row, over the segment's rows */
        f64 reach = 0.0;                        /* rows past the segment that caps, joins and spikes can reach */
        f64 period = 0.0;
        f64 divs = 0.0;                         /* edges in one round cap or join */
        if (stroke) {
            const f32 lw = sh->strokeWidth * scale;     /* the product the rasterizer takes */
            if (!(lw <= (f32)SVG_MAX_PX)) goto absurd;
            reach = (f64)lw * 0.5;
            if (sh->strokeLineJoin == NSVG_JOIN_MITER) reach *= sh->miterLimit >= 1.0f ? (f64)sh->miterLimit : 1.0;
            if (sh->strokeLineCap == NSVG_CAP_ROUND || sh->strokeLineJoin == NSVG_JOIN_ROUND) {
                divs = svg_round_divs(lw);
                if (!(divs <= 1.0e6)) goto absurd;
            }
            if (sh->strokeDashCount > 0) {
                for (int k = 0; k < sh->strokeDashCount; k++) period += (f64)sh->strokeDashArray[k];
                if (sh->strokeDashCount & 1) period *= 2.0;
                period *= (f64)scale;
                if (period < 1.0e-6) period = 1.0e-6;
            }
        }
        f64 shape_dashes = 0.0, maxabs = 0.0;
        const f64 edges_before = edges;
        for (const NSVGpath *pa = sh->paths; pa; pa = pa->next) {
            edges += 2.0 * divs;                                    /* the two ends of the subpath */
            const f32 *prev = NULL;
            for (int i = 0; i + 3 < pa->npts; i += 3) {
                const f32 *c = pa->pts + 2 * i;           /* four control points: x0 y0 ... x3 y3 */
                f32 lo = c[1], hi = c[1];
                f64 len = 0.0;
                for (int k = 0; k < 8; k++) {
                    const f32 at = fabsf(c[k]) * scale;
                    if (!(at <= (f32)SVG_MAX_PX)) goto absurd;                      /* also NaN */
                    if ((f64)at > maxabs) maxabs = (f64)at;
                }
                for (int k = 1; k < 4; k++) {
                    if (c[2 * k + 1] < lo) lo = c[2 * k + 1];
                    if (c[2 * k + 1] > hi) hi = c[2 * k + 1];
                    len += hypot((f64)(c[2 * k] - c[2 * k - 2]), (f64)(c[2 * k + 1] - c[2 * k - 1]));
                }
                if (period > 0.0) shape_dashes += len * (f64)scale / period;
                /* The start of the path is a cap; every later start is a join. */
                const f64 turn = prev ? svg_turn(prev, c) : 3.14159265358979323846;
                const f64 sharp = turn / 3.14159265358979323846;           /* 0..1 */
                /* A cubic whose control points do not run one way in y can cross a row
                 * three times (y(t) is a cubic): three edges where a line has one. */
                const bool mono = (c[1] <= c[3] && c[3] <= c[5] && c[5] <= c[7]) || (c[1] >= c[3] && c[3] >= c[5] && c[5] >= c[7]);
                const f64 crossings = mono ? 1.0 : 3.0;
                edges += weight * crossings + divs * sharp;
                const f64 r0 = floor((f64)lo * (f64)scale), r1 = ceil((f64)hi * (f64)scale);
                svg_rows_add(delta, rows, r0, r1, weight * crossings);
                if (stroke) svg_rows_add(delta, rows, r0 - reach, r1 + reach, 2.0 * sharp);
                prev = c;
            }
        }
        if (period > 0.0 && period < SVG_DASH_ULPS * SVG_FLT_EPSILON * maxabs) goto absurd;
        dashes += shape_dashes;
        const f64 shape_edges = edges - edges_before + shape_dashes * (4.0 + 2.0 * divs);
        if (shape_edges > biggest) biggest = shape_edges;
    }
    f64 work = 0.0, active = 0.0;
    for (i32 y = 0; y < rows; y++) {
        active += delta[y];
        work += active * active;
    }
    free(delta);
    /* A dash's own cost already holds its edges: they count toward memory only. */
    cost->seconds = work * SVG_SECONDS_PER_WORK + dashes * SVG_SECONDS_PER_DASH + edges * SVG_SECONDS_PER_EDGE;
    cost->edges = biggest;
    return true;

absurd:
    free(delta);
    *cost = refused;
    return true;
}

SvgStatus svg_rasterize(const u8 *data, usize len, const SvgLimits *lim, SvgImage *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!data || !lim || !out || lim->long_edge < 1) return SVG_ERR_READ;
    if (len == 0) return SVG_ERR_NOT_SVG;
    if (len > lim->max_file_bytes) return SVG_ERR_TOO_LARGE;
    if (!svg_has_root(data, len)) return SVG_ERR_NOT_SVG;

    /* nsvgParse() writes into the text it is given (it terminates strings in
     * place), so it gets a copy and the caller's bytes stay as they were. */
    char *copy = (char *)malloc(len + 1u);
    if (!copy) return SVG_ERR_MEMORY;
    memcpy(copy, data, len);
    copy[len] = '\0';
    NSVGimage *img = nsvgParse(copy, "px", 96.0f);
    free(copy);
    if (!img) return SVG_ERR_NOT_SVG;

    const f32 dw = img->width, dh = img->height;
    if (!isfinite(dw) || !isfinite(dh) || dw <= 0.0f || dh <= 0.0f ||
        dw > SVG_MAX_DECLARED_PX || dh > SVG_MAX_DECLARED_PX) {
        nsvgDelete(img);
        return SVG_ERR_EMPTY;
    }

    /* One scale for both axes, so the drawing is not stretched; the longer side
     * lands on `edge` and the shorter one follows the aspect (at least a pixel:
     * a 1000:1 banner is still a banner). A drawing over the time budget is
     * drawn at half the size, and again, before it is refused: the cost falls
     * with the size, and a smaller picture beats none. */
    const f32 longer = dw > dh ? dw : dh;
    i32 edge = lim->long_edge;
    i32 w = 1, h = 1;
    f32 scale = 1.0f;
    SvgCost cost = { 0.0, 0.0 };
    for (;;) {
        scale = (f32)edge / longer;
        w = (i32)lroundf(dw * scale);
        h = (i32)lroundf(dh * scale);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        if (w > edge) w = edge;
        if (h > edge) h = edge;
        if (!svg_estimate(img, scale, h, &cost)) {
            nsvgDelete(img);
            return SVG_ERR_MEMORY;
        }
        if (cost.seconds <= SVG_BUDGET_SECONDS && cost.edges <= SVG_EDGE_BUDGET) break;     /* false for NaN: refused */
        if (edge <= SVG_MIN_LONG_EDGE) {
            nsvgDelete(img);
            return SVG_ERR_TOO_COMPLEX;
        }
        edge = edge / 2 < SVG_MIN_LONG_EDGE ? SVG_MIN_LONG_EDGE : edge / 2;
    }

    usize bytes = 0;
    if (!img_rgba_bytes_checked(w, h, &bytes) || bytes > lim->max_rgba_bytes) {
        nsvgDelete(img);
        return SVG_ERR_MEMORY;
    }
    u8 *rgba = (u8 *)calloc(bytes, 1);
    NSVGrasterizer *rast = rgba ? nsvgCreateRasterizer() : NULL;
    if (!rast) {
        free(rgba);
        nsvgDelete(img);
        return SVG_ERR_MEMORY;
    }
    /* The guarantee under the model: nanosvg stops itself at the deadline, or when
     * one shape makes more edges than SVG_HARD_EDGE_CAP, and says so. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const f64 limit = lim->max_seconds > 0.0 ? lim->max_seconds : SVG_DEADLINE_SECONDS;
    nsvgRasterizerSetLimits(rast, (f64)now.tv_sec + (f64)now.tv_nsec * 1e-9 + limit, SVG_HARD_EDGE_CAP);
    nsvgRasterize(rast, img, 0.0f, 0.0f, scale, rgba, w, h, w * 4);
    const bool stopped = nsvgRasterizerAborted(rast) != 0;
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);
    if (stopped) {
        free(rgba);
        return SVG_ERR_TOO_COMPLEX;
    }

    const bool dark = svg_ink_is_dark(rgba, (usize)w * (usize)h);

    /* nanosvg emits straight alpha; the renderer's image pipeline takes
     * premultiplied (the same step lucide.c and seti_icons.c take). */
    img_premultiply_rgba(rgba, w, h);

    out->rgba = rgba;
    out->w = w;
    out->h = h;
    out->doc_w = dw;
    out->doc_h = dh;
    out->dark_ink = dark;
    out->est_seconds = cost.seconds;
    out->long_edge_used = edge;
    return SVG_OK;
}

SvgStatus svg_rasterize_file(const char *path, const SvgLimits *lim, SvgImage *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!path || !path[0] || !lim || !out) return SVG_ERR_READ;

    /* O_NONBLOCK so that opening a FIFO returns at once; the type is checked
     * on the descriptor, so there is no window between asking and opening. */
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return SVG_ERR_READ;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return SVG_ERR_READ;
    }
    if ((u64)st.st_size > (u64)lim->max_file_bytes) {
        close(fd);
        return SVG_ERR_TOO_LARGE;
    }

    /* Sized by the file, not by the limit: a 2 KB icon does not take 8 MiB. One
     * byte of slack tells a file that GREW since the stat from one that fits. */
    const usize size = (usize)st.st_size;
    u8 *buf = (u8 *)malloc(size + 1u);
    if (!buf) {
        close(fd);
        return SVG_ERR_MEMORY;
    }
    usize got = 0;
    for (;;) {
        ssize_t n = read(fd, buf + got, size + 1u - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (usize)n;
        if (got > size) break;
    }
    close(fd);

    SvgStatus s = got > size ? SVG_ERR_READ : svg_rasterize(buf, got, lim, out);
    free(buf);
    return s;
}
