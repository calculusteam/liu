/* One preparation pass, on its caller's thread. Direct-mapped entries only
 * skip repeated requests; collisions still call the real font API. */
#ifndef LIU_FONT_PREPARE_H
#define LIU_FONT_PREPARE_H
#include "renderer/renderer.h"
typedef struct {
    u32 glyphs[1024];
    struct { u32 key; i32 width; } ligatures[512];
} FontPrepareBatch;
static inline void font_prepare_batch_glyph(FontPrepareBatch *batch,FontAtlas *font,u32 cp) {
    u32 slot=(cp*2654435761u)&1023u;
    if(cp && batch->glyphs[slot]==cp)return;
    batch->glyphs[slot]=cp;
    FontGlyphRect rect;(void)font_get_glyph_rect(font,cp,&rect);
}
static inline i32 font_prepare_batch_ligature(FontPrepareBatch *batch,FontAtlas *font,const u32 *cp,i32 count) {
    if(count<2 || count>4)return font_prepare_ligature(font,cp,count);
    u32 key=(u32)count<<28;
    for(i32 i=0;i<count;i++) {
        if(cp[i]>127)return font_prepare_ligature(font,cp,count);
        key|=cp[i]<<(i*7);
    }
    u32 slot=(key*2654435761u)>>23;
    if(batch->ligatures[slot].key==key)return batch->ligatures[slot].width;
    i32 width=font_prepare_ligature(font,cp,count);
    batch->ligatures[slot].key=key;batch->ligatures[slot].width=width;return width;
}
/* The same for a candidate taken from a run: a miss is shaped through
 * font_check_ligature_run, which skips what the run's own shaping left alone. */
static inline i32 font_prepare_batch_ligature_run(FontPrepareBatch *batch,FontAtlas *font,FontLigatureRun *run,i32 at,i32 count) {
    const u32 *cp=run->cps+at;
    if(count<2 || count>4)return font_prepare_ligature_run(font,run,at,count);
    u32 key=(u32)count<<28;
    for(i32 i=0;i<count;i++) {
        if(cp[i]>127)return font_prepare_ligature_run(font,run,at,count);
        key|=cp[i]<<(i*7);
    }
    u32 slot=(key*2654435761u)>>23;
    if(batch->ligatures[slot].key==key)return batch->ligatures[slot].width;
    i32 width=font_prepare_ligature_run(font,run,at,count);
    batch->ligatures[slot].key=key;batch->ligatures[slot].width=width;return width;
}
#endif
