/* A tiny PNG encoder using stored DEFLATE blocks. No codec dependency, global
 * settings, files, or main-thread work. The output keeps the document background. */
#include "ui/sketch_model.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define SKETCH_RASTER_WORK_MAX 32000000u

typedef struct {
    u8 *rgb,*mask;const _Atomic bool *cancel;u32 work;bool failed;i32 width,height;
    i32 x0,y0,x1,y1;f32 ink[3],decode[256];u8 encode[4096];
} Raster;
static bool cancelled(const Raster *r){return r->cancel&&atomic_load_explicit(r->cancel,memory_order_relaxed);}
static void raster_begin(void *ctx,SketchTool tool,f32 width,u32 color) {
    (void)tool;(void)width;Raster *r=ctx;
    r->x0=r->width;r->y0=r->height;r->x1=r->y1=-1;
    r->ink[0]=r->decode[(color>>16)&255];r->ink[1]=r->decode[(color>>8)&255];r->ink[2]=r->decode[color&255];
}
static void coverage_segment(Raster *r,SketchPoint a,SketchPoint b,f32 radius) {
    i32 x0=MAX(0,(i32)floorf(fminf(a.x,b.x)-radius-1)),x1=MIN(r->width-1,(i32)ceilf(fmaxf(a.x,b.x)+radius+1));
    i32 y0=MAX(0,(i32)floorf(fminf(a.y,b.y)-radius-1)),y1=MIN(r->height-1,(i32)ceilf(fmaxf(a.y,b.y)+radius+1));
    r->x0=MIN(r->x0,x0);r->y0=MIN(r->y0,y0);r->x1=MAX(r->x1,x1);r->y1=MAX(r->y1,y1);
    f32 dx=b.x-a.x,dy=b.y-a.y,n=dx*dx+dy*dy;
    for(i32 y=y0;y<=y1;y++) {
        if(cancelled(r)){r->failed=true;return;}
        for(i32 x=x0;x<=x1;x++) {
            if(++r->work>SKETCH_RASTER_WORK_MAX){r->failed=true;return;}
            f32 t=n>.00001f?fminf(1,fmaxf(0,((x+.5f-a.x)*dx+(y+.5f-a.y)*dy)/n)):0;
            f32 vx=x+.5f-a.x-t*dx,vy=y+.5f-a.y-t*dy,distance=sqrtf(vx*vx+vy*vy);
            f32 aa=distance>.0001f?fmaxf(.75f,(fabsf(vx)+fabsf(vy))/distance):1;
            f32 v=fminf(1,fmaxf(0,(distance-radius)/aa+.5f));
            u8 coverage=(u8)roundf((1-v*v*(3-2*v))*255);
            usize index=(usize)y*r->width+x;
            r->mask[index]=MAX(r->mask[index],coverage);
        }
    }
}
static void raster_segment(void *ctx,SketchPoint a,SketchPoint b,f32 width,u32 rgb) {
    (void)rgb;Raster *r=ctx;if(r->failed||cancelled(r)){r->failed=true;return;}
    f32 dx=b.x-a.x,dy=b.y-a.y;
    i32 chunks=MAX(1,(i32)ceilf(sqrtf(dx*dx+dy*dy)/16));
    for(i32 i=0;i<chunks;i++) {
        f32 t=(f32)i/chunks,u=(f32)(i+1)/chunks;
        coverage_segment(r,(SketchPoint){a.x+dx*t,a.y+dy*t},(SketchPoint){a.x+dx*u,a.y+dy*u},width*.5f);
        if(r->failed)return;
    }
}
static void raster_end(void *ctx) {
    Raster *r=ctx;if(r->failed)return;
    for(i32 y=r->y0;y<=r->y1;y++) {
        if(cancelled(r)){r->failed=true;return;}
        for(i32 x=r->x0;x<=r->x1;x++) {
            if(++r->work>SKETCH_RASTER_WORK_MAX){r->failed=true;return;}
            usize i=(usize)y*r->width+x;u8 coverage=r->mask[i];r->mask[i]=0;
            if(!coverage)continue;
            f32 alpha=coverage/255.0f;u8 *p=r->rgb+i*3;
            for(i32 k=0;k<3;k++) {
                f32 value=r->decode[p[k]]*(1-alpha)+r->ink[k]*alpha;
                p[k]=r->encode[MIN(4095,MAX(0,(i32)roundf(value*4095)))];
            }
        }
    }
}
static void raster_tables(Raster *r) {
    for(i32 i=0;i<256;i++){f32 c=i/255.0f;r->decode[i]=c<=.04045f?c/12.92f:powf((c+.055f)/1.055f,2.4f);}
    for(i32 i=0;i<4096;i++){f32 c=i/4095.0f,v=c<=.0031308f?c*12.92f:1.055f*powf(c,1/2.4f)-.055f;r->encode[i]=(u8)roundf(fminf(1,fmaxf(0,v))*255);}
}
static void be32(u8 *p,u32 n){p[0]=(u8)(n>>24);p[1]=(u8)(n>>16);p[2]=(u8)(n>>8);p[3]=(u8)n;}
static u32 crc32_png(const u8 *p,usize n) {
    u32 crc=0xffffffff;
    for(usize i=0;i<n;i++){crc^=p[i];for(u32 k=0;k<8;k++)crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));}
    return crc^0xffffffff;
}
static usize chunk(u8 *out,const char tag[4],const u8 *data,u32 n) {
    be32(out,n);memcpy(out+4,tag,4);if(n)memcpy(out+8,data,n);
    be32(out+8+n,crc32_png(out+4,(usize)n+4));return (usize)n+12;
}
bool sketch_export_png(const SketchDocument *d,const _Atomic bool *cancel,u8 **png,usize *len,char *error,usize error_cap) {
    if(!png||!len)return false;*png=NULL;*len=0;
    if(error&&error_cap)snprintf(error,error_cap,"Could not prepare the sketch");
    if(!sketch_document_valid(d))return false;
    i32 width=sketch_canvas_width(d),height=sketch_canvas_height(d);
    if(width<64||height<64||width>SKETCH_MAX_DIM||height>SKETCH_MAX_DIM)return false;
    u32 background=sketch_background_rgb(d);
    const u8 paper[3]={(u8)(background>>16),(u8)(background>>8),(u8)background};
    usize bytes=(usize)width*height*3;
    u8 *rgb=malloc(bytes);if(!rgb)return false;
    for(usize i=0;i<bytes;i+=3)memcpy(rgb+i,paper,3);
    u8 *mask=calloc((usize)width*height,1);if(!mask){free(rgb);return false;}
    Raster r={.rgb=rgb,.mask=mask,.cancel=cancel,.width=width,.height=height};
    raster_tables(&r);sketch_visit_paths(d,raster_begin,raster_segment,raster_end,&r);free(mask);
    if(r.failed||cancelled(&r)) {
        if(error&&error_cap)snprintf(error,error_cap,"Sketch is too complex to copy. Undo a few strokes and try again.");
        free(rgb);return false;
    }
    bool blank=true;
    for(usize i=0;i<bytes;i++)if(rgb[i]!=paper[i%3]){blank=false;break;}
    if(blank){free(rgb);if(error&&error_cap)error[0]=0;return true;}
    usize row=(usize)width*3+1, raw_len=row*height;
    u8 *raw=malloc(raw_len);if(!raw){free(rgb);return false;}
    for(i32 y=0;y<height;y++){raw[(usize)y*row]=0;memcpy(raw+(usize)y*row+1,rgb+(usize)y*width*3,row-1);}
    free(rgb);
    usize blocks=(raw_len+65534)/65535, zlen=2+blocks*5+raw_len+4;
    u8 *z=malloc(zlen),*result=malloc(zlen+57);
    if(!z||!result){free(raw);free(z);free(result);return false;}
    z[0]=0x78;z[1]=0x01;usize pos=2,offset=0;u32 aa=1,bb=0;
    while(offset<raw_len) {
        if(cancelled(&r)){free(raw);free(z);free(result);return false;}
        u32 n=(u32)MIN(raw_len-offset,(usize)65535);
        z[pos++]=offset+n==raw_len?1:0;
        z[pos++]=(u8)n;z[pos++]=(u8)(n>>8);u32 nn=n^0xffff;
        z[pos++]=(u8)nn;z[pos++]=(u8)(nn>>8);memcpy(z+pos,raw+offset,n);pos+=n;
        for(u32 i=0;i<n;i++){aa=(aa+raw[offset+i])%65521;bb=(bb+aa)%65521;}
        offset+=n;
    }
    be32(z+pos,(bb<<16)|aa);pos+=4;free(raw);
    static const u8 magic[8]={137,80,78,71,13,10,26,10};memcpy(result,magic,8);
    u8 ihdr[13]={0};be32(ihdr,(u32)width);be32(ihdr+4,(u32)height);ihdr[8]=8;ihdr[9]=2;
    usize total=8;total+=chunk(result+total,"IHDR",ihdr,13);total+=chunk(result+total,"IDAT",z,(u32)pos);total+=chunk(result+total,"IEND",NULL,0);free(z);
    if(cancelled(&r)){free(result);return false;}
    *png=result;*len=total;if(error&&error_cap)error[0]=0;return true;
}
