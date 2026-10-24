#ifndef LIU_INTRO_RENDER_H
#define LIU_INTRO_RENDER_H

/* Draws one IntroFrame. Everything on the overlay — the dark, the dithered
 * field, the cursor, the mark, the name, the frame that becomes the window —
 * is one fragment shader, so an offscreen render is the same picture as the
 * live one. Objective-C only (macOS). */

#ifdef __OBJC__

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "intro/intro_timeline.h"

typedef struct IntroRenderer IntroRenderer;

/* The overlay's layer must use this format; output is premultiplied alpha. */
#define INTRO_PIXEL_FORMAT MTLPixelFormatBGRA8Unorm

/* `lang` is the captions' language, an index into INTRO_LANG_CODES
 * (intro_lang.h); one out of range is English. NULL when the shader does not
 * compile or a texture cannot be made; the caller then simply shows the
 * window. */
IntroRenderer *intro_renderer_create(id<MTLDevice> device, i32 lang);
void intro_renderer_destroy(IntroRenderer *r);

/* Presents one frame into `layer`. Safe to call from a display-link thread.
 * False when the layer had no drawable to give. */
bool intro_renderer_draw(IntroRenderer *r, const IntroFrame *f, const IntroLayout *l,
                         f64 time, CAMetalLayer *layer);

/* The frame as premultiplied BGRA, view size x scale pixels, rendered
 * offscreen. malloc'd; the caller frees. */
u8 *intro_renderer_image(IntroRenderer *r, const IntroFrame *f, const IntroLayout *l,
                         f64 time, i32 *out_w, i32 *out_h);

#endif /* __OBJC__ */
#endif /* LIU_INTRO_RENDER_H */
