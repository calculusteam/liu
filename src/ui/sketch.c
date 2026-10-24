#include "ui/sketch.h"
#include "ui/sketch_job.h"
#include "ui/ui.h"
#include "ui/chrome_palette.h"
#include "ui/icon.h"
#include "platform/platform.h"
#include "core/mem_budget.h"
#include "core/utf8.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

extern void draw_text_ex(Renderer *,const char *,f32,f32,Color,i32,f32);
static const u32 ink_colors[SKETCH_COLOR_COUNT]={0x171717,0xffffff,0x6b7280,0x92400e,0xef4444,0xf97316,0xf59e0b,0x22c55e,0x0d9488,0x06b6d4,0x2563eb,0x4f46e5,0xa855f7,0xdb2777};
static u32 color_rgb(Color c) {
    return ((u32)(fminf(1,fmaxf(0,c.r))*255+0.5f)<<16)|((u32)(fminf(1,fmaxf(0,c.g))*255+0.5f)<<8)|(u32)(fminf(1,fmaxf(0,c.b))*255+0.5f);
}
static void sketch_update_canvas(AppState *app) {
    SketchDocument *d=app->sketch.document;if(!d)return;
    const Theme *t=app->config.theme?app->config.theme:&THEME_Liu;
    d->has_background=true;d->background_rgb=color_rgb(t->bg);
    SketchLayout g=sketch_layout(app->fb_width,app->fb_height,app->dpi_scale);
    if(!d->stroke_count&&!d->drawing&&g.usable) {
        f32 factor=SKETCH_MAX_DIM/fmaxf(g.paper.w,g.paper.h);
        d->width=(u16)MAX(64,MIN(SKETCH_MAX_DIM,(i32)roundf(g.paper.w*factor)));
        d->height=(u16)MAX(64,MIN(SKETCH_MAX_DIM,(i32)roundf(g.paper.h*factor)));
    }
}
bool sketch_open(AppState *app) {
    if(!app)return false;Sketch *s=&app->sketch;
    if(s->finishing)return true;
    if(!s->document) {
        s->arena=arena_create(sizeof(SketchDocument)+64);
        if(!s->arena.base)return false;
        s->document=arena_calloc(&s->arena,sizeof(SketchDocument));
        if(!s->document){arena_destroy(&s->arena);return false;}
        mem_budget_adjust(MEM_IMAGE_CPU,(isize)s->arena.size);
        s->brush_width=4;
        const Theme *t=app->config.theme?app->config.theme:&THEME_Liu;
        s->color_index=t->bg.r+t->bg.g+t->bg.b<1.5f?1:0;
    }
    /* Prepare before the canvas opens, so the one-time GPU compile does not
     * interrupt the first pointer stroke. Subsequent opens reuse it. */
    renderer_prepare_strokes(&app->renderer);
    app->palette_active=false;
    if(app->settings.open)settings_close(&app->settings);
    if(app->lightbox.open)lightbox_close(app);
    sketch_update_canvas(app);
    s->open=true;s->dirty=true;s->width_dragging=false;s->hover_control=0;s->error[0]=0;return true;
}
void sketch_close(AppState *app) {
    Sketch *s=&app->sketch;if(!s->open||s->finishing)return;
    s->dirty=true;
    s->width_dragging=false;sketch_update_canvas(app);
    sketch_end(s->document);
    if(!sketch_has_content(s->document)){s->open=false;return;}
    SketchExportJob *j=sketch_job_start(s->document);
    if(!j){snprintf(s->error,sizeof s->error,"Could not prepare the sketch. Try closing again.");return;}
    s->job=j;s->finishing=true;s->error[0]=0;
}
void sketch_discard(AppState *app) {
    Sketch *s=&app->sketch;
    if(s->job){sketch_job_cancel(s->job);sketch_job_release(s->job);s->job=NULL;}
    s->finishing=false;s->open=false;s->dirty=true;s->width_dragging=false;sketch_cancel_stroke(s->document);
}
void sketch_free(Sketch *s) {
    if(s->job){sketch_job_cancel(s->job);sketch_job_release(s->job);s->job=NULL;}
    if(s->arena.base){mem_budget_adjust(MEM_IMAGE_CPU,-(isize)s->arena.size);arena_destroy(&s->arena);}
    memset(s,0,sizeof *s);
}
bool sketch_ready(const Sketch *s) {return s&&sketch_job_ready(s->job);}
bool sketch_pump(AppState *app) {
    Sketch *s=&app->sketch;if(!sketch_ready(s))return false;
    SketchExportJob *j=s->job;s->job=NULL;s->finishing=false;s->dirty=true;
    if(j->png&&j->len) {
        if(platform_clipboard_set_png(j->png,j->len)==PLATFORM_CLIP_OK){s->open=false;app_show_toast(app,"Sketch copied to clipboard");}
        else snprintf(s->error,sizeof s->error,"Could not copy the sketch. Try closing again.");
    } else if (!j->error[0]) s->open=false;
    else snprintf(s->error,sizeof s->error,"%s",j->error);
    sketch_job_release(j);return true;
}
static SketchPoint paper_point(SketchLayout g,f32 x,f32 y) {return (SketchPoint){(x-g.paper.x)/g.scale,(y-g.paper.y)/g.scale};}
bool sketch_key(AppState *app,i32 key,u32 mods) {
    Sketch *s=&app->sketch;if(!s->open)return false;
    if(s->finishing)return true;
    if((mods&MOD_ALT)&&key==KEY_ESCAPE){sketch_discard(app);return true;}
    if(key==KEY_ESCAPE||key==KEY_ENTER){sketch_close(app);return true;}
    if((mods&(MOD_SUPER|MOD_CTRL))&&key==KEY_Z){s->dirty|=(mods&MOD_SHIFT)?sketch_redo(s->document):sketch_undo(s->document);return true;}
    if((mods&(MOD_SUPER|MOD_CTRL))&&key==KEY_Y){s->dirty|=sketch_redo(s->document);return true;}
    if(mods&(MOD_SUPER|MOD_CTRL|MOD_ALT))return true;
    SketchTool previous=s->tool;
    if(key==KEY_P)s->tool=SKETCH_PEN;
    else if(key==KEY_L)s->tool=SKETCH_LINE;
    else if(key==KEY_A)s->tool=SKETCH_ARROW;
    else if(key==KEY_R)s->tool=SKETCH_BOX;
    else if(key==KEY_E)s->tool=SKETCH_ERASER;
    s->dirty|=s->tool!=previous;
    return true;
}
static void sketch_set_width(Sketch *s,SketchLayout g,f32 y) {
    if(!g.usable||g.slider_track.h<=0)return;
    f32 t=fminf(1,fmaxf(0,(y-g.slider_track.y)/g.slider_track.h));
    f32 width=roundf(12-11*t);
    s->dirty|=width!=s->brush_width;s->brush_width=width;
}
bool sketch_mouse_down(AppState *app,f32 x,f32 y,i32 button) {
    Sketch *s=&app->sketch;if(!s->open)return false;if(s->finishing||button!=0)return true;
    sketch_update_canvas(app);
    SketchLayout g=sketch_layout_for_document(app->fb_width,app->fb_height,app->dpi_scale,s->document);
    if(g.usable && s->error[0] && settings_rect_contains(g.leave,x,y)){sketch_discard(app);return true;}
    if(settings_rect_contains(g.close,x,y)||settings_rect_contains(g.done,x,y)){sketch_close(app);return true;}
    if(!g.usable)return true;
    for(i32 i=0;i<5;i++)if(settings_rect_contains(g.tools[i],x,y)){s->dirty|=s->tool!=(SketchTool)i;s->tool=(SketchTool)i;return true;}
    for(i32 i=0;i<SKETCH_COLOR_COUNT;i++)if(settings_rect_contains(g.colors[i],x,y)){s->dirty|=s->color_index!=i;s->color_index=i;return true;}
    if(settings_rect_contains(g.slider,x,y)){s->width_dragging=true;sketch_set_width(s,g,y);return true;}
    if(settings_rect_contains(g.undo,x,y)){s->dirty|=sketch_undo(s->document);return true;}
    if(settings_rect_contains(g.redo,x,y)){s->dirty|=sketch_redo(s->document);return true;}
    if(settings_rect_contains(g.clear,x,y)){s->dirty|=sketch_clear(s->document);return true;}
    if(settings_rect_contains(g.paper,x,y)) {
        if(s->tool==SKETCH_ERASER&&!sketch_has_content(s->document))return true;
        f32 width=s->tool==SKETCH_ERASER?fminf(24,s->brush_width*2):s->brush_width;
        if(!sketch_begin(s->document,s->tool,paper_point(g,x,y),width,ink_colors[s->color_index]))
            snprintf(s->error,sizeof s->error,"Sketch limit reached. Clear or undo to continue.");
        else s->error[0]=0;
        s->dirty=true;
    }
    return true;
}
static i32 sketch_control_at(SketchLayout g,f32 x,f32 y) {
    if(!g.usable)return 0;
    for(i32 i=0;i<5;i++)if(settings_rect_contains(g.tools[i],x,y))return i+1;
    for(i32 i=0;i<SKETCH_COLOR_COUNT;i++)if(settings_rect_contains(g.colors[i],x,y))return i+6;
    if(settings_rect_contains(g.slider,x,y))return 20;
    if(settings_rect_contains(g.undo,x,y))return 21;
    if(settings_rect_contains(g.redo,x,y))return 22;
    if(settings_rect_contains(g.clear,x,y))return 23;
    if(settings_rect_contains(g.close,x,y))return 24;
    if(settings_rect_contains(g.done,x,y))return 25;
    return 0;
}
bool sketch_mouse_move(AppState *app,f32 x,f32 y) {
    Sketch *s=&app->sketch;if(!s->open)return false;
    SketchLayout g=sketch_layout_for_document(app->fb_width,app->fb_height,app->dpi_scale,s->document);
    i32 hovered=s->finishing||s->document->drawing?0:sketch_control_at(g,x,y);
    s->dirty|=s->hover_control!=hovered;s->hover_control=hovered;
    if(s->width_dragging&&!s->finishing){sketch_set_width(s,g,y);return true;}
    if(!s->finishing&&s->document->drawing) {
        /* Freehand input outside the drawing area must not be clamped into
         * a trail along its border. Shape endpoints may still clip there. */
        SketchTool active=s->document->strokes[s->document->visible_count-1].tool;
        bool outside=(active==SKETCH_PEN||active==SKETCH_ERASER)&&!settings_rect_contains(g.paper,x,y);
        s->dirty|=g.usable&&!outside?sketch_move(s->document,paper_point(g,x,y)):sketch_end(s->document);
    }
    return true;
}
bool sketch_mouse_up(AppState *app) {if(!app->sketch.open)return false;app->sketch.width_dragging=false;app->sketch.dirty|=sketch_end(app->sketch.document);return true;}

static void label(Renderer *r,const char *text,f32 x,f32 y,Color fg,i32 cells,f32 cw) {draw_text_ex(r,text,x,y,fg,cells,cw);}
static void button(Renderer *r,SettingsRect b,const char *text,Color bg,Color edge,Color fg,f32 dpi,f32 alpha) {
    bg.a=fminf(bg.a,alpha);edge.a=fminf(edge.a,alpha);fg.a=fminf(fg.a,alpha);
    renderer_draw_rrect_bordered(r,b.x,b.y,b.w,b.h,bg,edge,dpi,6*dpi,6*dpi,6*dpi,6*dpi,0,0,0,0);
    renderer_flush_rrects(r);
    i32 max=(i32)((b.w-8*dpi)/(8*dpi));i32 shown=MIN((i32)utf8_len((const u8 *)text,strlen(text)),MAX(0,max));
    label(r,text,b.x+(b.w-shown*8*dpi)*0.5f,b.y+(b.h-16*dpi)*0.5f,fg,MAX(0,max),8*dpi);
}
static void symbol_button(Renderer *r,SettingsRect b,IconKind icon,bool selected,bool hovered,bool enabled,Color fill,Color fg,Color dim,f32 dpi,f32 alpha,bool solid) {
    Color ink=enabled?(selected||hovered?fg:dim):color_lerp(fill,dim,0.4f);
    ink.a=fminf(ink.a,alpha);
    if(selected||(hovered&&enabled)||solid) {
        fill.a*=alpha;
        renderer_draw_rrect_simple(r,b.x,b.y,b.w,b.h,fill,b.w*0.5f);
        renderer_flush_rrects(r);
    }
    icon_draw(r,icon,b.x+(b.w-20*dpi)*0.5f,b.y+(b.h-20*dpi)*0.5f,20*dpi,ink);
}
static const char *control_hint(i32 control) {
    static const char *tools[]={"Pen · P","Line · L","Arrow · A","Box · R","Erase · E"};
    static const char *colors[]={"Black","White","Gray","Brown","Red","Orange","Amber","Green","Teal","Cyan","Blue","Indigo","Purple","Pink"};
    if(control>0&&control<=5)return tools[control-1];
    if(control>=6&&control<20)return colors[control-6];
    if(control==20)return "Stroke width";
#ifdef PLATFORM_MACOS
    if(control==21)return "Undo · ⌘Z";if(control==22)return "Redo · ⇧⌘Z";
#else
    if(control==21)return "Undo · Ctrl+Z";if(control==22)return "Redo · Ctrl+Shift+Z";
#endif
    if(control==23)return "Clear canvas";
    if(control==24)return "Close and copy · Esc";
    if(control==25)return "Finish and copy · Return";
    return "";
}
typedef struct {Renderer *r;SketchLayout layout;bool has_ink;f32 alpha;u32 rgb;
    bool freehand,pending;SketchPoint a,b,normal;f32 width;} Preview;
static SketchPoint preview_normal(SketchPoint a,SketchPoint b) {
    f32 dx=b.x-a.x,dy=b.y-a.y,n=sqrtf(dx*dx+dy*dy);
    return n>.0001f?(SketchPoint){-dy/n,dx/n}:(SketchPoint){0,1};
}
static Color preview_ink(const Preview *p){return (Color){(p->rgb>>16&255)/255.0f,(p->rgb>>8&255)/255.0f,(p->rgb&255)/255.0f,p->alpha};}
static void preview_disc(Preview *p,SketchPoint point,f32 width) {
    f32 diameter=fmaxf(.5f,width*p->layout.scale);
    renderer_draw_rrect_simple(p->r,p->layout.paper.x+point.x*p->layout.scale-diameter*.5f,p->layout.paper.y+point.y*p->layout.scale-diameter*.5f,diameter,diameter,preview_ink(p),diameter*.5f);
}
static void preview_ribbon(Preview *p,SketchPoint end_normal) {
    SketchLayout g=p->layout;
    renderer_draw_stroke_segment(p->r,g.paper.x+p->a.x*g.scale,g.paper.y+p->a.y*g.scale,g.paper.x+p->b.x*g.scale,g.paper.y+p->b.y*g.scale,p->normal.x,p->normal.y,end_normal.x,end_normal.y,p->width*g.scale,preview_ink(p));
}
static void preview_end(void *ctx) {
    Preview *p=ctx;
    if(p->pending){preview_ribbon(p,preview_normal(p->a,p->b));preview_disc(p,p->b,p->width);p->pending=false;}
    renderer_flush_lines(p->r);renderer_flush_rrects(p->r);
}
static void preview_begin(void *ctx,SketchTool tool,f32 width,u32 rgb) {
    Preview *p=ctx;p->rgb=rgb;p->has_ink=true;p->width=width;p->pending=false;
    p->freehand=tool==SKETCH_PEN||tool==SKETCH_ERASER;
}
static void preview_segment(void *ctx,SketchPoint a,SketchPoint b,f32 width,u32 rgb) {
    Preview *p=ctx;SketchLayout g=p->layout;
    if(p->has_ink&&p->rgb!=rgb)preview_end(p);
    p->has_ink=true;p->rgb=rgb;
    bool dot=a.x==b.x&&a.y==b.y;
    if(!p->freehand) {
        if(!dot)renderer_draw_line(p->r,g.paper.x+a.x*g.scale,g.paper.y+a.y*g.scale,g.paper.x+b.x*g.scale,g.paper.y+b.y*g.scale,width*g.scale,preview_ink(p));
        preview_disc(p,a,width);if(!dot)preview_disc(p,b,width);return;
    }
    if(dot){preview_disc(p,a,width);return;}
    SketchPoint normal=preview_normal(a,b);
    if(p->pending) {
        SketchPoint prev=preview_normal(p->a,p->b);
        f32 dotn=prev.x*normal.x+prev.y*normal.y;
        if(dotn<-.75f) {
            preview_ribbon(p,prev);preview_disc(p,p->b,width);
        } else {
            /* Shared miter positions eliminate both gaps and the overlapping
             * per-sample AA rings. Bound the scale at tight reversals. */
            f32 scale=1/fmaxf(.5f,1+dotn);
            SketchPoint join={(prev.x+normal.x)*scale,(prev.y+normal.y)*scale};
            preview_ribbon(p,join);normal=join;
        }
    } else preview_disc(p,a,width);
    p->a=a;p->b=b;p->normal=normal;p->width=width;p->pending=true;
}
void sketch_render(AppState *app) {
    Sketch *s=&app->sketch;f32 scale,alpha,yoff;
    s->dirty=false;
    if(!modal_anim_progress(s->open,&s->open_anim,&s->close_anim,&s->was_open,app->dpi_scale,MODAL_OPEN_DUR_LARGE,&scale,&alpha,&yoff))return;
    (void)scale;(void)yoff;sketch_update_canvas(app);
    Renderer *r=&app->renderer;f32 dpi=app->dpi_scale;
    SketchLayout g=sketch_layout_for_document(app->fb_width,app->fb_height,dpi,s->document);
    const Theme *t=app->config.theme?app->config.theme:&THEME_Liu;
    ChromePalette cp=chrome_palette_for(t);
    Color bg=t->bg;bg.a=alpha;
    Color edge=cp.divider_subtle;edge.a*=alpha;
    Color fg=t->fg;fg.a*=alpha;
    Color dim=color_lerp(t->bg,t->fg,0.64f);dim.a*=alpha;
    Color selected=color_lerp(t->bg,t->fg,0.12f);
    renderer_draw_rect(r,0,0,app->fb_width,app->fb_height,(Color){0,0,0,0.3f*alpha});renderer_flush_rects(r);
    renderer_draw_rrect_bordered(r,g.panel.x,g.panel.y,g.panel.w,g.panel.h,bg,edge,dpi,26*dpi,26*dpi,26*dpi,26*dpi,24*dpi,0.2f*alpha,0,4*dpi);renderer_flush_rrects(r);
    renderer_set_ui_scale(r,8*dpi,16*dpi);
    symbol_button(r,g.close,ICON_CLOSE,false,s->hover_control==24,!s->finishing,selected,fg,dim,dpi,alpha,false);
    Color done_fill=color_lerp(t->bg,t->fg,0.55f),done_ink=chrome_legible_on(done_fill);
    symbol_button(r,g.done,s->finishing?ICON_REFRESH:ICON_CHECK,false,s->hover_control==25,!s->finishing,done_fill,done_ink,done_ink,dpi,alpha,true);
    if(!g.usable) {
        if(g.panel.h>=120*dpi&&g.panel.w>=180*dpi) {
            i32 cells=(i32)((g.panel.w-32*dpi)/(8*dpi));
            label(r,"Make the window larger",g.panel.x+16*dpi,g.panel.y+g.panel.h*0.5f-16*dpi,fg,cells,8*dpi);
            label(r,"to sketch.",g.panel.x+16*dpi,g.panel.y+g.panel.h*0.5f+4*dpi,dim,cells,8*dpi);
        }
        renderer_flush_glyphs(r);renderer_reset_ui_scale(r);return;
    }
    renderer_push_scissor(r,g.paper.x,g.paper.y,g.paper.w,g.paper.h);
    if(s->document){Preview p={.r=r,.layout=g,.alpha=alpha};sketch_visit_paths(s->document,preview_begin,preview_segment,preview_end,&p);renderer_flush_lines(r);renderer_flush_rrects(r);}
    renderer_pop_scissor(r);
    Color toolbar_fill=color_lerp(t->bg,t->fg,0.06f);toolbar_fill.a*=alpha;
    Color toolbar_edge=cp.divider_strong;toolbar_edge.a*=alpha;
    renderer_draw_rrect_bordered(r,g.toolbar.x,g.toolbar.y,g.toolbar.w,g.toolbar.h,toolbar_fill,toolbar_edge,dpi,g.toolbar.h*0.5f,g.toolbar.h*0.5f,g.toolbar.h*0.5f,g.toolbar.h*0.5f,0,0,0,0);renderer_flush_rrects(r);
    const IconKind tools[]={ICON_PENCIL,ICON_LINE,ICON_ARROW_UP_RIGHT,ICON_SQUARE,ICON_ERASER};
    for(i32 i=0;i<5;i++)symbol_button(r,g.tools[i],tools[i],(SketchTool)i==s->tool,s->hover_control==i+1,!s->finishing,selected,fg,dim,dpi,alpha,false);
    bool idle=!s->finishing&&s->document&&!s->document->drawing;
    symbol_button(r,g.undo,ICON_UNDO,false,s->hover_control==21,idle&&s->document->visible_count,selected,fg,dim,dpi,alpha,false);
    symbol_button(r,g.redo,ICON_REDO,false,s->hover_control==22,idle&&s->document->visible_count<s->document->stroke_count,selected,fg,dim,dpi,alpha,false);
    symbol_button(r,g.clear,ICON_TRASH,false,s->hover_control==23,idle&&s->document->visible_count,selected,fg,dim,dpi,alpha,false);
    Color track=color_lerp(t->bg,t->fg,0.3f);track.a*=alpha;
    renderer_draw_rrect_simple(r,g.slider_track.x,g.slider_track.y,g.slider_track.w,g.slider_track.h,track,dpi);
    f32 thumb_y=g.slider_track.y+(12-s->brush_width)/11*g.slider_track.h;
    renderer_draw_rrect_bordered(r,g.slider.x+(g.slider.w-18*dpi)*0.5f,thumb_y-9*dpi,18*dpi,18*dpi,fg,toolbar_edge,dpi,9*dpi,9*dpi,9*dpi,9*dpi,0,0,0,0);renderer_flush_rrects(r);
    Color accent=theme_ui_accent(t);accent.a*=alpha;
    for(i32 i=0;i<SKETCH_COLOR_COUNT;i++) {
        SettingsRect b=g.colors[i];u32 c=ink_colors[i];
        Color ink={(c>>16&255)/255.0f,(c>>8&255)/255.0f,(c&255)/255.0f,alpha};
        if(i==s->color_index)renderer_draw_rrect_bordered(r,b.x-3*dpi,b.y-3*dpi,b.w+6*dpi,b.h+6*dpi,bg,accent,2*dpi,(b.w+6*dpi)*0.5f,(b.w+6*dpi)*0.5f,(b.w+6*dpi)*0.5f,(b.w+6*dpi)*0.5f,0,0,0,0);
        renderer_draw_rrect_bordered(r,b.x,b.y,b.w,b.h,ink,toolbar_edge,dpi,b.w*0.5f,b.w*0.5f,b.w*0.5f,b.w*0.5f,0,0,0,0);
    }
    renderer_flush_rrects(r);
    const char *hint=s->error[0]?s->error:s->finishing?"Preparing your sketch…":control_hint(s->hover_control);
    if(hint[0]) {
        f32 y=g.colors[0].y-30*dpi;
        i32 cells=(i32)((g.panel.w-40*dpi)/(8*dpi));
        if(s->error[0]) {
            y=g.leave.y-22*dpi;
            button(r,g.leave,"Close without copy",cp.surface_neutral,toolbar_edge,dim,dpi,alpha);
        }
        label(r,hint,g.panel.x+20*dpi,y,s->error[0]?cp.btn_destructive_fg:dim,cells,8*dpi);
    }
    renderer_flush_glyphs(r);renderer_reset_ui_scale(r);
}
