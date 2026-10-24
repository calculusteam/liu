/* Liu — bounds and recursion gate for the stb readers used by font.c. Keep
 * this paired with the length-aware CFF buffer reader in the vendored stb
 * fork. Tables unused by Liu (kerning, GPOS, SVG, names) are container-bounded
 * only.
 */
#ifndef LIU_SFNT_PROBE_H
#define LIU_SFNT_PROBE_H
#include "core/types.h"
#include <limits.h>
#include <stdlib.h>

#define SFNT_TAG_TRUETYPE 0x00010000u
#define SFNT_TAG_TRUE     0x74727565u
#define SFNT_TAG_TYP1     0x74797031u
#define SFNT_TAG_OTTO     0x4F54544Fu
#define SFNT_TAG_TTCF     0x74746366u
static inline u32 sfnt_be32(const u8 *p) {
    return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3];
}
static inline u16 sfnt_be16(const u8 *p) { return (u16)(((u32)p[0]<<8)|p[1]); }
typedef struct { const u8 *p; u32 n; } SfntTable;

static inline bool sfnt_cmap_ok(SfntTable cmap, u32 glyphs) {
    if (cmap.n < 4) return false;
    u32 nr = sfnt_be16(cmap.p+2), selected = 0;
    if (!nr || nr > (cmap.n-4)/8) return false;
    for (u32 i=0; i<nr; i++) {
        const u8 *r=cmap.p+4+i*8;
        u32 platform=sfnt_be16(r), encoding=sfnt_be16(r+2);
        if (platform==0 || (platform==3 && (encoding==1 || encoding==10)))
            selected=sfnt_be32(r+4);
    }
    if (selected < 4+nr*8 || selected > cmap.n || cmap.n-selected < 2) return false;
    const u8 *p=cmap.p+selected;
    u32 left=cmap.n-selected, fmt=sfnt_be16(p), len;
    if (fmt==0 || fmt==6 || fmt==4) {
        if (left<4) return false;
        len=sfnt_be16(p+2);
    } else if (fmt==12 || fmt==13) {
        if (left<16) return false;
        len=sfnt_be32(p+4);
    } else return false; /* stb asserts for unsupported mappings */
    if (len>left) return false;
    if (fmt==0) {
        if (len<6) return false;
        for (u32 i=6; i<len; i++) if (p[i]>=glyphs) return false;
    } else if (fmt==6) {
        if (len<10) return false;
        u32 count=sfnt_be16(p+8);
        if (count>(len-10)/2 || (u32)sfnt_be16(p+6)+count>65536u) return false;
        for (u32 i=0; i<count; i++) if (sfnt_be16(p+10+2*i)>=glyphs) return false;
    } else if (fmt==4) {
        if (len<16) return false;
        u32 twice=sfnt_be16(p+6), n=twice/2;
        if (!n || (twice&1) || n>(len-16)/8) return false;
        u32 power=1, log=0;
        while (power<=n/2) { power*=2; log++; }
        if (sfnt_be16(p+8)!=2*power || sfnt_be16(p+10)!=log ||
            sfnt_be16(p+12)!=2*(n-power)) return false;
        u32 prev=0;
        for (u32 i=0; i<n; i++) {
            u32 end=sfnt_be16(p+14+2*i), start=sfnt_be16(p+16+2*n+2*i);
            u32 delta=sfnt_be16(p+16+4*n+2*i), range=sfnt_be16(p+16+6*n+2*i);
            if (start>end || (i && start<=prev)) return false;
            prev=end;
            if (range) {
                u32 at=16+6*n+2*i+range;
                if ((range&1) || at<16+8*n || at>len || 2*(end-start+1)>len-at) return false;
                /* stb's offset branch returns the raw glyph id. */
                for (u32 j=0; j<=end-start; j++) if (sfnt_be16(p+at+2*j)>=glyphs) return false;
            } else {
                for (u32 c=start; c<=end; c++) if ((u16)(c+delta)>=glyphs) return false;
            }
        }
        if (prev!=65535u) return false;
    } else {
        if (len<16) return false;
        u32 count=sfnt_be32(p+12), prev=0;
        if (count>(len-16)/12 || count>INT_MAX) return false;
        for (u32 i=0; i<count; i++) {
            const u8 *r=p+16+12*i;
            u32 start=sfnt_be32(r), end=sfnt_be32(r+4), gid=sfnt_be32(r+8);
            if (start>end || end>0x10ffffu || (i && start<=prev) || gid>=glyphs) return false;
            if (fmt==12 && end-start>=glyphs-gid) return false;
            prev=end;
        }
    }
    return true;
}

/* Validate every glyph, then memoize the compound DAG's depth and expanded
 * vertex bound. This rejects cycles and exponential component expansion before
 * stb recursively allocates/rasterizes any shape. */
typedef struct { SfntTable glyf, loca; u32 glyphs, width; u8 *state; u32 *cost, *depth; } SfntGlyphs;
static inline u32 sfnt_loca(const SfntGlyphs *g, u32 i) {
    return g->width==4 ? sfnt_be32(g->loca.p+4*i) : 2u*sfnt_be16(g->loca.p+2*i);
}
static inline bool sfnt_glyph_ok(SfntGlyphs *g, u32 id, u32 nesting) {
    if (id>=g->glyphs || nesting>32 || g->state[id]==1) return false;
    if (g->state[id]==2) return nesting+g->depth[id]<=32;
    g->state[id]=1;
    /* Even an empty component consumes a recursive shape visit. Counting
     * vertices alone admits exponentially repeated zero-vertex glyphs. */
    u32 a=sfnt_loca(g,id), z=sfnt_loca(g,id+1), cost=1, depth=0;
    if (a>z || z>g->glyf.n) return false;
    if (a!=z) {
        const u8 *p=g->glyf.p+a;
        u32 len=z-a, at=10;
        if (len<10) return false;
        i32 contours=(i16)sfnt_be16(p);
        if ((i16)sfnt_be16(p+2)>(i16)sfnt_be16(p+6) ||
            (i16)sfnt_be16(p+4)>(i16)sfnt_be16(p+8)) return false;
        if (contours>0) {
            u32 nc=(u32)contours;
            if (nc>(len-10)/2 || len-10-2*nc<2) return false;
            u32 previous=0;
            for (u32 i=0; i<nc; i++) {
                u32 last=sfnt_be16(p+10+2*i);
                if (i && last<=previous) return false;
                previous=last;
            }
            u32 points=previous+1;
            at+=2*nc;
            u32 ins=sfnt_be16(p+at); at+=2;
            if (ins>len-at) return false;
            at+=ins;
            u32 coords=0;
            for (u32 i=0; i<points;) {
                if (at>=len) return false;
                u32 flags=p[at++], run=1;
                if (flags&8) { if (at>=len) return false; run+=(u32)p[at++]; }
                if (run>points-i) return false;
                u32 bytes=((flags&2)?1:((flags&16)?0:2))+((flags&4)?1:((flags&32)?0:2));
                coords+=run*bytes;
                i+=run;
            }
            if (coords>len-at) return false;
            cost=1+points+2*nc;
        } else if (contours<0) {
            u32 flags;
            do {
                if (len-at<4) return false;
                flags=sfnt_be16(p+at);
                u32 child=sfnt_be16(p+at+2); at+=4;
                /* stb does not support point-matching components. */
                if (!(flags&2) || ((flags&8)!=0)+((flags&64)!=0)+((flags&128)!=0)>1) return false;
                u32 extra=(flags&1)?4:2;
                extra+=(flags&8)?2:((flags&64)?4:((flags&128)?8:0));
                if (extra>len-at) return false;
                at+=extra;
                if (!sfnt_glyph_ok(g,child,nesting+1)) return false;
                if (g->cost[child]>1000000u-cost) return false;
                cost+=g->cost[child];
                if (depth<g->depth[child]+1) depth=g->depth[child]+1;
            } while (flags&32);
            if (flags&256) {
                if (len-at<2) return false;
                u32 ins=sfnt_be16(p+at); at+=2;
                if (ins>len-at) return false;
            }
        }
    }
    g->cost[id]=cost; g->depth[id]=depth; g->state[id]=2;
    return nesting+depth<=32;
}
static inline bool sfnt_dir_ok(const u8 *d, usize size, u32 off) {
    if (!d || size>INT_MAX || off>size || size-off<12) return false;
    u32 ver=sfnt_be32(d+off);
    if (ver!=SFNT_TAG_TRUETYPE && ver!=SFNT_TAG_TRUE && ver!=SFNT_TAG_TYP1 && ver!=SFNT_TAG_OTTO) return false;
    u32 n=sfnt_be16(d+off+4);
    usize dir=(usize)off+12;
    if (!n || n>256 || (usize)n*16>size-dir) return false;
    enum { HEAD, MAXP, HHEA, HMTX, CMAP, LOCA, GLYF, CFF, NT };
    static const u32 tags[NT]={0x68656164u,0x6d617870u,0x68686561u,0x686d7478u,
                              0x636d6170u,0x6c6f6361u,0x676c7966u,0x43464620u};
    SfntTable t[NT]={{0}};
    for (u32 i=0; i<n; i++) {
        const u8 *r=d+dir+16*i;
        u32 tag=sfnt_be32(r), a=sfnt_be32(r+8), len=sfnt_be32(r+12);
        if (!a || a>size || len>size-a) return false;
        /* Avoid first/last-tag disagreement with stb. */
        for (u32 j=0; j<i; j++) if (sfnt_be32(d+dir+16*j)==tag) return false;
        for (u32 j=0; j<NT; j++) if (tag==tags[j]) t[j]=(SfntTable){d+a,len};
    }
    if (t[HEAD].n<54 || t[MAXP].n<6 || t[HHEA].n<36 || !t[HMTX].p) return false;
    u32 glyphs=sfnt_be16(t[MAXP].p+4), metrics=sfnt_be16(t[HHEA].p+34);
    if (!glyphs || !metrics || metrics>glyphs || 4*metrics+2*(glyphs-metrics)>t[HMTX].n) return false;
    if (!sfnt_be16(t[HEAD].p+18) || (i16)sfnt_be16(t[HHEA].p+4)<=(i16)sfnt_be16(t[HHEA].p+6)) return false;
    if (!sfnt_cmap_ok(t[CMAP],glyphs)) return false;
    if (!t[GLYF].p) return t[CFF].n>=4 && t[CFF].n<0x40000000u;
    u32 format=sfnt_be16(t[HEAD].p+50);
    if (format>1 || !t[LOCA].p) return false;
    u32 width=format?4:2;
    if ((glyphs+1)*width>t[LOCA].n) return false;
    SfntGlyphs g={t[GLYF],t[LOCA],glyphs,width,NULL,NULL,NULL};
    /* Glyph count is 16-bit; this bounded temporary storage is shared by all
     * recursive checks and freed before either loader sees the font. */
    g.state=(u8*)calloc(glyphs,1);
    g.cost=(u32*)calloc(glyphs,sizeof(u32));
    g.depth=(u32*)calloc(glyphs,sizeof(u32));
    bool ok=g.state && g.cost && g.depth;
    for (u32 i=0; ok && i<glyphs; i++) ok=sfnt_glyph_ok(&g,i,0);
    free(g.state); free(g.cost); free(g.depth);
    return ok;
}
static inline bool sfnt_probe(const u8 *data, usize size, u32 *out_offset) {
    if (!data || size<12 || size>INT_MAX) return false;
    u32 off=0;
    if (sfnt_be32(data)==SFNT_TAG_TTCF) {
        if (size<16) return false;
        u32 ver=sfnt_be32(data+4), n=sfnt_be32(data+8);
        if ((ver!=0x00010000u && ver!=0x00020000u) || !n || n>0x10000u || 12+(usize)n*4>size) return false;
        off=sfnt_be32(data+12);
    }
    if (!sfnt_dir_ok(data,size,off)) return false;
    if (out_offset) *out_offset=off;
    return true;
}
#endif
