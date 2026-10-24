#ifndef UI_SKETCH_H
#define UI_SKETCH_H
#include "ui/sketch_model.h"
#include "core/memory.h"
#include "ui/anim.h"
#include "ui/settings_layout.h"
typedef struct AppState AppState;
#define SKETCH_COLOR_COUNT 14
typedef struct {
    SettingsRect panel, paper, toolbar, close, done, tools[5], colors[SKETCH_COLOR_COUNT], undo, redo, clear, slider, slider_track, leave;
    f32 scale;
    bool usable;
} SketchLayout;
typedef struct {
    bool open, was_open, finishing, dirty;
    Arena arena;
    SketchDocument *document;
    SketchTool tool;
    i32 color_index, hover_control;
    f32 brush_width;
    bool width_dragging;
    Anim open_anim, close_anim;
    struct SketchExportJob *job;
    char error[128];
} Sketch;
SketchLayout sketch_layout(f32 sw, f32 sh, f32 dpi);
SketchLayout sketch_layout_for_document(f32 sw,f32 sh,f32 dpi,const SketchDocument *document);
bool sketch_open(AppState *app);
void sketch_close(AppState *app);
void sketch_discard(AppState *app);
void sketch_free(Sketch *s);
bool sketch_pump(AppState *app);
bool sketch_ready(const Sketch *s);
void sketch_render(AppState *app);
bool sketch_key(AppState *app, i32 key, u32 mods);
bool sketch_mouse_down(AppState *app, f32 x, f32 y, i32 button);
bool sketch_mouse_move(AppState *app, f32 x, f32 y);
bool sketch_mouse_up(AppState *app);
#endif
