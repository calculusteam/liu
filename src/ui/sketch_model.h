#ifndef UI_SKETCH_MODEL_H
#define UI_SKETCH_MODEL_H
#include "core/types.h"
#include <stdatomic.h>
#define SKETCH_WIDTH 1024
#define SKETCH_HEIGHT 640
#define SKETCH_MAX_DIM 1024
#define SKETCH_MAX_POINTS 16384
#define SKETCH_MAX_STROKES 512

typedef enum { SKETCH_PEN, SKETCH_LINE, SKETCH_ARROW, SKETCH_BOX, SKETCH_ERASER, SKETCH_CLEAR } SketchTool;
typedef struct { f32 x, y; } SketchPoint;
typedef struct { u32 first, count, rgb; f32 width; SketchTool tool; } SketchStroke;
typedef struct {
    SketchPoint points[SKETCH_MAX_POINTS];
    SketchStroke strokes[SKETCH_MAX_STROKES];
    u32 point_count, stroke_count, visible_count;
    bool drawing;
    u64 revision;
    u16 width, height; /* 0/0 uses the original 1024 × 640 document size. */
    bool has_background;
    u32 background_rgb;
} SketchDocument;
u32 sketch_background_rgb(const SketchDocument *d);
i32 sketch_canvas_width(const SketchDocument *d);
i32 sketch_canvas_height(const SketchDocument *d);
typedef void (*SketchSegmentFn)(void *ctx, SketchPoint a, SketchPoint b, f32 width, u32 rgb);
typedef void (*SketchPathBeginFn)(void *ctx,SketchTool tool,f32 width,u32 rgb);
typedef void (*SketchPathEndFn)(void *ctx);
bool sketch_begin(SketchDocument *d, SketchTool tool, SketchPoint p, f32 width, u32 rgb);
bool sketch_move(SketchDocument *d, SketchPoint p);
bool sketch_end(SketchDocument *d);
void sketch_cancel_stroke(SketchDocument *d);
bool sketch_undo(SketchDocument *d);
bool sketch_redo(SketchDocument *d);
bool sketch_clear(SketchDocument *d);
bool sketch_has_content(const SketchDocument *d);
bool sketch_document_valid(const SketchDocument *d);
void sketch_segments(const SketchDocument *d, SketchSegmentFn fn, void *ctx);
void sketch_visit_paths(const SketchDocument *d,SketchPathBeginFn begin,SketchSegmentFn segment,SketchPathEndFn end,void *ctx);
/* Worker-only export. Bounded dimensions and raster work, no I/O.
 * A blank image succeeds with NULL/0 output and leaves the clipboard alone. */
bool sketch_export_png(const SketchDocument *d, const _Atomic bool *cancel,
                       u8 **png, usize *len, char *error, usize error_cap);
#endif
