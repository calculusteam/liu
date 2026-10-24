#include "ui/sketch_model.h"
#include <math.h>
#include <string.h>

static bool point_ok(SketchPoint p) { return isfinite(p.x) && isfinite(p.y); }
u32 sketch_background_rgb(const SketchDocument *d) {return d&&d->has_background?d->background_rgb&0xffffff:0xffffff;}
i32 sketch_canvas_width(const SketchDocument *d) {return d&&d->width?d->width:SKETCH_WIDTH;}
i32 sketch_canvas_height(const SketchDocument *d) {return d&&d->height?d->height:SKETCH_HEIGHT;}
static SketchPoint clamp_point(const SketchDocument *d,SketchPoint p) {
    p.x = fminf(sketch_canvas_width(d) - 1, fmaxf(0, p.x));
    p.y = fminf(sketch_canvas_height(d) - 1, fmaxf(0, p.y));
    return p;
}
static void truncate_redo(SketchDocument *d) {
    d->stroke_count = d->visible_count;
    d->point_count = d->visible_count ? d->strokes[d->visible_count - 1].first + d->strokes[d->visible_count - 1].count : 0;
}
bool sketch_begin(SketchDocument *d, SketchTool tool, SketchPoint p, f32 width, u32 rgb) {
    if (!d || d->drawing || !point_ok(p) || !isfinite(width) || width < 1 || width > 24 ||
        tool < SKETCH_PEN || tool > SKETCH_ERASER || d->visible_count >= SKETCH_MAX_STROKES) return false;
    u32 used = d->visible_count ? d->strokes[d->visible_count - 1].first + d->strokes[d->visible_count - 1].count : 0;
    u32 need = (tool == SKETCH_PEN || tool == SKETCH_ERASER) ? 1 : 2;
    if (used > SKETCH_MAX_POINTS - need) return false;
    truncate_redo(d);
    SketchStroke *s = &d->strokes[d->stroke_count++];
    *s = (SketchStroke){d->point_count, need, tool == SKETCH_ERASER ? sketch_background_rgb(d) : rgb & 0xffffff, width, tool};
    p = clamp_point(d,p);
    d->points[d->point_count++] = p;
    if (need == 2) d->points[d->point_count++] = p;
    d->visible_count = d->stroke_count;
    d->drawing = true;
    d->revision++;
    return true;
}
bool sketch_move(SketchDocument *d, SketchPoint p) {
    if (!d || !d->drawing || !point_ok(p)) return false;
    p = clamp_point(d,p);
    SketchStroke *s = &d->strokes[d->visible_count - 1];
    SketchPoint last = d->points[s->first + s->count - 1];
    f32 dx = p.x - last.x, dy = p.y - last.y;
    if (dx * dx + dy * dy < 0.4f) return false;
    if (s->tool == SKETCH_PEN || s->tool == SKETCH_ERASER) {
        if (d->point_count == SKETCH_MAX_POINTS) return false;
        d->points[d->point_count++] = p; s->count++;
    } else d->points[s->first + 1] = p;
    d->revision++;
    return true;
}
bool sketch_end(SketchDocument *d) {
    if (!d || !d->drawing) return false;
    d->drawing = false; d->revision++; return true;
}
void sketch_cancel_stroke(SketchDocument *d) {
    if (!d || !d->drawing) return;
    d->drawing = false; d->visible_count--; truncate_redo(d); d->revision++;
}
bool sketch_undo(SketchDocument *d) {
    if (!d || d->drawing || !d->visible_count) return false;
    d->visible_count--; d->revision++; return true;
}
bool sketch_redo(SketchDocument *d) {
    if (!d || d->drawing || d->visible_count >= d->stroke_count) return false;
    d->visible_count++; d->revision++; return true;
}
bool sketch_clear(SketchDocument *d) {
    if (!d || d->drawing) return false;
    if(d->point_count>=SKETCH_MAX_POINTS-1 || d->visible_count>=SKETCH_MAX_STROKES-1) {
        /* At the hard cap, Clear is also the explicit way to reclaim history.
         * Ordinary Clear remains undoable until that capacity is exhausted. */
        d->point_count=d->stroke_count=d->visible_count=0;d->revision++;return true;
    }
    if(!sketch_has_content(d))return false;
    truncate_redo(d);
    d->strokes[d->stroke_count++] = (SketchStroke){d->point_count, 0, 0xffffff, 1, SKETCH_CLEAR};
    d->visible_count = d->stroke_count; d->revision++; return true;
}
static u32 first_visible(const SketchDocument *d) {
    u32 first = 0;
    for (u32 i = 0; i < d->visible_count; i++) if (d->strokes[i].tool == SKETCH_CLEAR) first = i + 1;
    return first;
}
bool sketch_has_content(const SketchDocument *d) {
    if (!d) return false;
    for (u32 i = first_visible(d); i < d->visible_count; i++)
        if (d->strokes[i].tool != SKETCH_ERASER && d->strokes[i].tool != SKETCH_CLEAR) return true;
    return false;
}
bool sketch_document_valid(const SketchDocument *d) {
    if (!d || d->point_count > SKETCH_MAX_POINTS || d->stroke_count > SKETCH_MAX_STROKES || d->visible_count > d->stroke_count) return false;
    if((d->width||d->height) && (d->width<64||d->height<64||d->width>SKETCH_MAX_DIM||d->height>SKETCH_MAX_DIM))return false;
    if(d->drawing&&(!d->visible_count||d->visible_count!=d->stroke_count||d->strokes[d->visible_count-1].tool==SKETCH_CLEAR))return false;
    u32 expected_first=0;
    for (u32 i = 0; i < d->stroke_count; i++) {
        const SketchStroke *s = &d->strokes[i];
        if (s->tool < SKETCH_PEN || s->tool > SKETCH_CLEAR || !isfinite(s->width) || s->width < 1 || s->width > 24 ||
            s->first > d->point_count || s->count > d->point_count - s->first) return false;
        if(s->first!=expected_first)return false;
        expected_first+=s->count;
        if (s->tool == SKETCH_CLEAR ? s->count != 0 : s->count < 1) return false;
        if ((s->tool == SKETCH_LINE || s->tool == SKETCH_ARROW || s->tool == SKETCH_BOX) && s->count != 2) return false;
    }
    if(expected_first!=d->point_count)return false;
    for (u32 i = 0; i < d->point_count; i++) {
        SketchPoint p = d->points[i];
        if (!point_ok(p) || p.x < 0 || p.x >= sketch_canvas_width(d) || p.y < 0 || p.y >= sketch_canvas_height(d)) return false;
    }
    return true;
}
static SketchPoint midpoint(SketchPoint a,SketchPoint b){return (SketchPoint){(a.x+b.x)*.5f,(a.y+b.y)*.5f};}
static void quadratic(SketchPoint a,SketchPoint control,SketchPoint b,i32 max_steps,f32 width,u32 rgb,SketchSegmentFn fn,void *ctx) {
    f32 cx=b.x-a.x,cy=b.y-a.y,n=cx*cx+cy*cy;
    f32 t=n>.00001f?fminf(1,fmaxf(0,((control.x-a.x)*cx+(control.y-a.y)*cy)/n)):0;
    f32 dx=control.x-a.x-t*cx,dy=control.y-a.y-t*cy;
    f32 flatness=sqrtf(dx*dx+dy*dy);
    i32 steps=MIN(max_steps,MAX(1,(i32)ceilf(sqrtf(flatness*8))));
    SketchPoint last=a;
    for(i32 i=1;i<=steps;i++) {
        f32 t=(f32)i/steps,u=1-t;
        SketchPoint next={u*u*a.x+2*u*t*control.x+t*t*b.x,u*u*a.y+2*u*t*control.y+t*t*b.y};
        fn(ctx,last,next,width,rgb);last=next;
    }
}
void sketch_visit_paths(const SketchDocument *d,SketchPathBeginFn begin,SketchSegmentFn fn,SketchPathEndFn end,void *ctx) {
    if (!d || !fn || !sketch_document_valid(d)) return;
    u32 first=first_visible(d),visible_points=0;
    for(u32 i=first;i<d->visible_count;i++)visible_points+=d->strokes[i].count;
    i32 max_steps=MIN(64,MAX(4,65536/(i32)MAX(1u,visible_points)));
    for (u32 i = first; i < d->visible_count; i++) {
        const SketchStroke *s = &d->strokes[i];
        if (!s->count) continue;
        u32 rgb=s->tool==SKETCH_ERASER?sketch_background_rgb(d):s->rgb;
        if(begin)begin(ctx,s->tool,s->width,rgb);
        SketchPoint a = d->points[s->first], b = d->points[s->first + s->count - 1];
        if (s->tool == SKETCH_BOX) {
            SketchPoint c = {b.x, a.y}, e = {a.x, b.y};
            fn(ctx,a,c,s->width,s->rgb); fn(ctx,c,b,s->width,s->rgb);
            fn(ctx,b,e,s->width,s->rgb); fn(ctx,e,a,s->width,s->rgb);
        } else if (s->tool == SKETCH_PEN || s->tool == SKETCH_ERASER) {
            if(s->count<=2)fn(ctx,a,b,s->width,rgb);
            else {
                /* Midpoint quadratics have matching tangents at joins and
                 * stay inside the input's convex hull. Keep raw points for
                 * undo and use this same path for screen and PNG rendering. */
                SketchPoint last=a;
                for(u32 k=1;k<s->count;k++) {
                    SketchPoint control=d->points[s->first+k-1],next=midpoint(control,d->points[s->first+k]);
                    quadratic(last,control,next,max_steps,s->width,rgb,fn,ctx);last=next;
                }
                quadratic(last,b,b,max_steps,s->width,rgb,fn,ctx);
            }
        } else {
            fn(ctx,a,b,s->width,s->rgb);
            if (s->tool == SKETCH_ARROW) {
                f32 dx = b.x-a.x, dy = b.y-a.y, n = sqrtf(dx*dx+dy*dy);
                if (n > 1) {
                    f32 size = fminf(n*0.4f, 12+s->width*2), ux=dx/n, uy=dy/n;
                    SketchPoint c={b.x-size*ux+size*0.5f*uy,b.y-size*uy-size*0.5f*ux};
                    SketchPoint e={b.x-size*ux-size*0.5f*uy,b.y-size*uy+size*0.5f*ux};
                    fn(ctx,b,c,s->width,s->rgb);fn(ctx,b,e,s->width,s->rgb);
                }
            }
        }
        if(end)end(ctx);
    }
}
void sketch_segments(const SketchDocument *d,SketchSegmentFn fn,void *ctx){sketch_visit_paths(d,NULL,fn,NULL,ctx);}
