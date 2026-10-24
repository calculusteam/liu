/* Resolve physical storage after immutable draw geometry is known.
 * Logical viewport/grid geometry is preserved; unknown work keeps full size. */
typedef struct {
    const u8 *bytes;usize length;
} ActiveBinding;

static void active_extent(f32 *bottom,f32 x,f32 y,f32 w,f32 h,MTLScissorRect clip) {
    if(!isfinite(x)||!isfinite(y)||!isfinite(w)||!isfinite(h)||w<0||h<0){*bottom=INFINITY;return;}
    f32 right=fminf(x+w,(f32)(clip.x+clip.width));
    f32 lower=fminf(y+h,(f32)(clip.y+clip.height));
    if(right>fmaxf(x,(f32)clip.x) && lower>fmaxf(y,(f32)clip.y))
        *bottom=fmaxf(*bottom,lower);
}

static NSUInteger active_target_height(Renderer *r,RendererMetal *m,LiuMetalPacket *p,id<MTLTexture> old) {
    NSUInteger full=(NSUInteger)r->screen_height;
    if(r->clear_color[3]!=1 || r->bg_texture || liu_dev_env("LIU_DUMP_FRAME"))return full;
    bool main_pass=false,known=true,saw_clear=false;id pipe=nil;
    ActiveBinding bindings[3]={{0}};
    MTLScissorRect clip={0,0,(NSUInteger)r->screen_width,full};f32 bottom=0;
    for(usize k=0;k<p->count && known;k++) {
        MetalRecord *c=&p->commands[k];
        if(c->kind==MR_RENDER) {
            main_pass=((MTLRenderPassDescriptor *)c->a).colorAttachments[0].texture==old;
            if(main_pass) {
                MTLRenderPassColorAttachmentDescriptor *color=((MTLRenderPassDescriptor *)c->a).colorAttachments[0];
                if(color.loadAction==MTLLoadActionClear) {
                    if(color.clearColor.alpha!=1 || color.clearColor.red!=r->clear_color[0] ||
                       color.clearColor.green!=r->clear_color[1] || color.clearColor.blue!=r->clear_color[2])return full;
                    saw_clear=true;
                } else if(!saw_clear || color.loadAction!=MTLLoadActionLoad)return full;
            }
            pipe=nil;memset(bindings,0,sizeof(bindings));
            clip=(MTLScissorRect){0,0,(NSUInteger)r->screen_width,full};
        }
        if((c->kind==MR_TEXTURE_TEXTURE || c->kind==MR_TEXTURE_BUFFER || c->kind==MR_BUFFER_TEXTURE) &&
            (c->a==old || c->b==old))return full;
        if(!main_pass)continue;
        switch(c->kind) {
        case MR_PIPELINE:pipe=c->a;break;
        case MR_VIEWPORT:
            if(c->u.viewport.originX!=0 || c->u.viewport.originY!=0 ||
               c->u.viewport.width!=r->screen_width || c->u.viewport.height!=r->screen_height)return full;
            break;
        case MR_SCISSOR:clip=c->u.scissor;break;
        case MR_VERTEX_BYTES:
            if(c->u.binding.index<3) {
                usize offset=c->u.binding.offset,n=c->u.binding.length;
                if(offset>p->used || n>p->used-offset){known=false;break;}
                bindings[c->u.binding.index]=(ActiveBinding){p->bytes+offset,n};
            }
            break;
        case MR_VERTEX_BUFFER:
            if(c->u.binding.index<3) {
                id<MTLBuffer> b=c->a;usize offset=c->u.binding.offset;
                if(!b.contents || offset>b.length){known=false;break;}
                bindings[c->u.binding.index]=(ActiveBinding){(const u8 *)b.contents+offset,b.length-offset};
            }
            break;
        case MR_DRAW: {
            usize n=c->u.draw.instances,stride=0,bytes=0;
            bool rect=pipe==m->rectPipeline,glyph=pipe==m->glyphPipeline;
            bool rounded=pipe==m->rrectPipeline || (m->rrectPipelineKeepAlpha && pipe==m->rrectPipelineKeepAlpha);
            /* Bounds are valid only for the renderer's unit geometry. A new
             * pipeline caller can bind a larger quad without changing instances. */
            bool strip=glyph && c->u.draw.type==MTLPrimitiveTypeTriangleStrip;
            const void *quad=strip?(const void *)s_glyph_quad:(const void *)s_quad;
            usize quad_bytes=strip?sizeof(s_glyph_quad):sizeof(s_quad);
            if(c->u.draw.start!=0 || c->u.draw.count!=(strip?4:6) ||
               c->u.draw.type!=(strip?MTLPrimitiveTypeTriangleStrip:MTLPrimitiveTypeTriangle) ||
               !bindings[0].bytes || bindings[0].length<quad_bytes ||
               memcmp(bindings[0].bytes,quad,quad_bytes)){known=false;break;}
            stride=rect?sizeof(RectInstance):glyph?sizeof(GlyphInstance):rounded?sizeof(RRectInstance):0;
            if(!stride || !checked_mul_usize(n,stride,&bytes) || !bindings[1].bytes || bytes>bindings[1].length ||
               !bindings[2].bytes || bindings[2].length!=sizeof(MetalUniforms)){known=false;break;}
            MetalUniforms u;memcpy(&u,bindings[2].bytes,sizeof(u));
            for(u32 i=0;i<16;i++)if(!isfinite(u.projection[i]))known=false;
            if(!known)break;
            if(!isfinite(u.cell_w) || u.cell_w<0 || !isfinite(u.cell_h) || u.cell_h<0 ||
               fabsf(u.projection[0]-2.0f/r->screen_width)>1e-6f ||
               fabsf(u.projection[5]+2.0f/r->screen_height)>1e-6f ||
               u.projection[1]!=0 || u.projection[4]!=0 || u.projection[3]!=0 ||
               u.projection[7]!=0 || u.projection[11]!=0 || u.projection[15]!=1){known=false;break;}
            /* Retained terminal rows store pane-local coordinates. Their
             * origin is carried by the per-draw projection translation. */
            f32 tx=(u.projection[12]+1)*(f32)r->screen_width*.5f;
            f32 ty=(1-u.projection[13])*(f32)r->screen_height*.5f;
            for(usize i=0;i<n;i++) {
                if(rect) {
                    RectInstance v;memcpy(&v,bindings[1].bytes+i*stride,stride);
                    if(v.a==0 || (v.a==1 && v.r==r->clear_color[0] && v.g==r->clear_color[1] && v.b==r->clear_color[2]))continue;
                    active_extent(&bottom,v.x+tx,v.y+ty,v.w,v.h,clip);
                } else if(glyph) {
                    GlyphInstance v;memcpy(&v,bindings[1].bytes+i*stride,stride);
                    if(v.fg[3])active_extent(&bottom,v.x+tx,v.y+ty,u.cell_w*(v.w_cells?v.w_cells:1),u.cell_h,clip);
                } else {
                    RRectInstance v;memcpy(&v,bindings[1].bytes+i*stride,stride);
                    f32 pad=v.shadow_size+2;
                    f32 left=fmaxf(0,pad-fmaxf(0,v.shadow_offset_x));
                    f32 right=pad+fmaxf(0,v.shadow_offset_x);
                    f32 top=fmaxf(0,pad-fmaxf(0,v.shadow_offset_y));
                    f32 lower=pad+fmaxf(0,v.shadow_offset_y);
                    active_extent(&bottom,v.x+tx-left,v.y+ty-top,v.w+left+right,v.h+top+lower,clip);
                }
                if(bottom+2>=(f32)full*.75f)return full;
            }
            break;
        }
        default:break;
        }
    }
    if(!known || !saw_clear || !isfinite(bottom))return full;
    NSUInteger rows=(NSUInteger)ceilf(bottom+2);rows=(rows+15)&~(NSUInteger)15;
    if(rows<16)rows=16;
    if(rows>=full || rows>full*3/4 || (full-rows)*(usize)r->screen_width*4<1024*1024)return full;
    return rows;
}

static bool active_target_finish(Renderer *r,RendererMetal *m) {
    if(!m->frameTargetPlanned || !m->surfacePresenter || !m->currentDrawable ||
       ![(id)m->currentCommandBuffer isKindOfClass:[LiuMetalRecordedBuffer class]])return true;
    LiuMetalPacket *p=((LiuMetalRecordedBuffer *)m->currentCommandBuffer)->packet;
    LiuSurfaceDrawable *d=(LiuSurfaceDrawable *)m->currentDrawable;
    id<MTLTexture> old=[d.texture retain];NSUInteger rows=active_target_height(r,m,p,old);
    bool resized=old.width!=(NSUInteger)r->screen_width || old.height!=rows;
    f64 t0=platform_time_sec();
    if(![m->surfacePresenter resolve:d width:r->screen_width height:rows]){[old release];return false;}
    bool main_pass=false,empty=false;
    for(usize k=0;k<p->count;k++) {
        MetalRecord *c=&p->commands[k];
        if(c->kind==MR_RENDER) {
            MTLRenderPassDescriptor *desc=c->a;
            main_pass=desc.colorAttachments[0].texture==old;empty=false;
            if(main_pass)desc.colorAttachments[0].texture=d.texture;
        }
        if(c->kind==MR_TEXTURE_TEXTURE || c->kind==MR_TEXTURE_BUFFER || c->kind==MR_BUFFER_TEXTURE) {
            if(c->a==old){[d.texture retain];[c->a release];c->a=d.texture;}
            if(c->b==old){[d.texture retain];[c->b release];c->b=d.texture;}
        }
        if(!main_pass)continue;
        if(c->kind==MR_SCISSOR) {
            NSUInteger w=d.texture.width,h=d.texture.height;
            NSUInteger x=c->u.scissor.x,y=c->u.scissor.y;
            empty=x>=w || y>=h || !c->u.scissor.width || !c->u.scissor.height;
            if(empty)c->u.scissor=(MTLScissorRect){0,0,1,1};
            else {
                if(c->u.scissor.width>w-x)c->u.scissor.width=w-x;
                if(c->u.scissor.height>h-y)c->u.scissor.height=h-y;
            }
        }
        if(c->kind==MR_DRAW && empty)c->kind=MR_NOP;
    }
    if(liu_dev_env("LIU_ACTIVE_PROBE_LOG")) {
        static f64 last;f64 now=platform_time_sec();
        if(now-last>1){last=now;fprintf(stderr,"ACTIVE_TARGET logical=%dx%d physical=%lu resize=%d replace_us=%.1f\n",
            r->screen_width,r->screen_height,(unsigned long)rows,resized,(now-t0)*1e6);}
    }
    [old release];return true;
}
