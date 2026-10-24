#include "ui/sketch.h"
#include "ui/layout.h"
#include <math.h>

SketchLayout sketch_layout(f32 sw,f32 sh,f32 dpi) {
    SketchLayout g={0};
    if(dpi<=0 || !isfinite(dpi))dpi=1;
    if(!isfinite(sw)||sw<1)sw=1;if(!isfinite(sh)||sh<1)sh=1;
    f32 panel_w=sw*SKETCH_PANEL_RATIO,panel_h=sh*SKETCH_PANEL_RATIO;
    g.panel=(SettingsRect){(sw-panel_w)*.5f,(sh-panel_h)*.5f,panel_w,panel_h};
    f32 w=g.panel.w,h=g.panel.h,pad=fminf(SKETCH_PANEL_PAD_PT*dpi,w*0.08f);
    f32 size=fminf(SKETCH_BUTTON_SIZE_PT*dpi,fminf(w-2*pad,h*0.25f));
    f32 gap=SKETCH_BUTTON_GAP_PT*dpi,step=size+gap;
    g.close=(SettingsRect){g.panel.x+pad,g.panel.y+pad,size,size};
    g.done=(SettingsRect){g.panel.x+w-pad-size,g.panel.y+h-pad-size,size,size};
    g.paper=(SettingsRect){g.panel.x+w*0.5f,g.panel.y+h*0.5f,0,0};
    if(w<300*dpi||h<280*dpi)return g;
    f32 toolbar_w=5*size+4*gap+2*SKETCH_TOOLBAR_PAD_PT*dpi;
    f32 toolbar_h=size+2*SKETCH_TOOLBAR_PAD_PT*dpi;
    f32 toolbar_y=g.panel.y+pad+(w<520*dpi?step:0);
    g.toolbar=(SettingsRect){g.panel.x+(w-toolbar_w)*0.5f,toolbar_y,toolbar_w,toolbar_h};
    for(i32 i=0;i<5;i++)g.tools[i]=(SettingsRect){g.toolbar.x+SKETCH_TOOLBAR_PAD_PT*dpi+i*step,g.toolbar.y+SKETCH_TOOLBAR_PAD_PT*dpi,size,size};
    g.undo=(SettingsRect){g.panel.x+w-pad-(3*size+2*gap),g.panel.y+pad,size,size};
    g.redo=g.undo;g.redo.x+=step;g.clear=g.redo;g.clear.x+=step;
    f32 color_size=SKETCH_COLOR_SIZE_PT*dpi,color_step=color_size+gap;
    f32 available=w-2*pad-size-gap;
    i32 columns=MIN(SKETCH_COLOR_COUNT,MAX(1,(i32)((available+gap)/color_step)));
    i32 rows=(SKETCH_COLOR_COUNT+columns-1)/columns;
    f32 palette_w=columns*color_size+(columns-1)*gap;
    f32 palette_x=g.panel.x+fminf((w-palette_w)*0.5f,w-pad-size-gap-palette_w);
    f32 palette_y=g.done.y+(size-color_size)*0.5f-(rows-1)*color_step;
    for(i32 i=0;i<SKETCH_COLOR_COUNT;i++) {
        i32 row=i/columns,count=MIN(columns,SKETCH_COLOR_COUNT-row*columns);
        g.colors[i]=(SettingsRect){palette_x+(columns-count)*color_step*0.5f+(i%columns)*color_step,palette_y+row*color_step,color_size,color_size};
    }
    f32 top=g.toolbar.y+g.toolbar.h+16*dpi,bottom=palette_y-16*dpi;
    if(bottom-top<60*dpi) {
        SettingsRect panel=g.panel,close=g.close,done=g.done;
        g=(SketchLayout){.panel=panel,.close=close,.done=done,.paper={panel.x+w*0.5f,panel.y+h*0.5f,0,0}};
        return g;
    }
    g.usable=true;
    g.paper=(SettingsRect){g.panel.x+64*dpi,top,w-84*dpi,bottom-top};
    g.scale=fminf(g.paper.w/SKETCH_WIDTH,g.paper.h/SKETCH_HEIGHT);
    f32 track_h=fminf(SKETCH_SLIDER_H_PT*dpi,g.paper.h*0.75f);
    g.slider=(SettingsRect){g.panel.x+16*dpi,g.paper.y+(g.paper.h-track_h)*0.5f-10*dpi,SKETCH_SLIDER_W_PT*dpi,track_h+20*dpi};
    g.slider_track=(SettingsRect){g.slider.x+g.slider.w*0.5f-dpi,g.slider.y+10*dpi,2*dpi,track_h};
    g.leave=(SettingsRect){g.panel.x+pad,palette_y-38*dpi,176*dpi,28*dpi};
    return g;
}
SketchLayout sketch_layout_for_document(f32 sw,f32 sh,f32 dpi,const SketchDocument *d) {
    SketchLayout g=sketch_layout(sw,sh,dpi);
    if(!g.usable)return g;
    f32 width=(f32)sketch_canvas_width(d),height=(f32)sketch_canvas_height(d);
    g.scale=fminf(g.paper.w/width,g.paper.h/height);
    f32 w=width*g.scale,h=height*g.scale;
    g.paper=(SettingsRect){g.paper.x+(g.paper.w-w)*0.5f,g.paper.y+(g.paper.h-h)*0.5f,w,h};
    return g;
}
