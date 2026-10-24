/* Shared immutable atlas pages; no renderer pointer or GPU writer is shared.
 * The key includes every source byte, dimensions and MTLDevice identity. */
#ifndef LIU_FONT_SHARED_SEED_H
#define LIU_FONT_SHARED_SEED_H
#import <CommonCrypto/CommonDigest.h>
typedef struct FontSharedSeed {
    struct FontSharedSeed *next;
    id<MTLTexture> texture;
    void *device;
    u8 digest[CC_SHA256_DIGEST_LENGTH];
    i32 width,height,pixel_height;
    u32 refs;
    usize bytes;
} FontSharedSeed;
static pthread_mutex_t s_seed_mu=PTHREAD_MUTEX_INITIALIZER;
static FontSharedSeed *s_seeds;

/* The immutable prewarm does not need growth headroom: dynamic glyphs use a
 * separate page. Include every reserved glyph/ligature rectangle and its
 * sampling gutter, not just the last nonzero ink pixel. Texel coordinates
 * remain unchanged; the shader uses the actual seed texture dimensions. */
static i32 font_shared_seed_height(const FontAtlas *atlas) {
    const char *mode=getenv("LIU_COMPACT_FONT_SEED");
    const FontAtlasPriv *p=fa_priv(atlas);
    if ((mode && mode[0]=='0') || !p) return atlas->atlas_height;
    f64 bottom=(f64)p->pack_y+p->pack_row_h;
    for (usize i=0;i<256;i++) bottom=fmax(bottom,atlas->glyphs[i].ay1);
    for (u32 i=0;i<p->cache_cap;i++)
        if (p->cache[i].valid && p->cache[i].kind==FONT_GLYPH_ALPHA)
            bottom=fmax(bottom,p->cache[i].y1);
    for (u32 i=0;i<p->lig_cap;i++)
        if (p->lig[i].valid && !p->lig[i].negative) bottom=fmax(bottom,p->lig[i].y1);
    if (!isfinite(bottom) || bottom<0 || bottom>=atlas->atlas_height)
        return atlas->atlas_height;
    i32 height=((i32)ceil(bottom)+2+63)&~63;
    return height<atlas->atlas_height ? height : atlas->atlas_height;
}

static void font_shared_seed_release(FontAtlas *atlas) {
    FontSharedSeed *seed=atlas->shared_seed;
    if (!seed) return;
    atlas->shared_seed=NULL;atlas->metal_seed_texture=NULL;
    pthread_mutex_lock(&s_seed_mu);
    if (--seed->refs==0) {
        FontSharedSeed **link=&s_seeds;
        while(*link && *link!=seed)link=&(*link)->next;
        if(*link)*link=seed->next;
        [seed->texture release];
        mem_budget_transient_adjust(MEM_FONT_GPU,-(isize)seed->bytes);
        mem_budget_transient_adjust(MEM_FONT_CPU,-(isize)sizeof(*seed));
        free(seed);
    }
    pthread_mutex_unlock(&s_seed_mu);
}

static bool font_shared_seed_acquire(FontAtlas *atlas,void *device) {
    const char *mode=getenv("LIU_SHARED_FONT_ATLAS");
    if ((mode && mode[0]=='0') || !atlas->atlas_bitmap || atlas->shared_seed) return false;
    usize size;
    if (!checked_mul_usize((usize)atlas->atlas_width,(usize)atlas->atlas_height,&size) || size>UINT_MAX) return false;
    i32 pixel_height=font_shared_seed_height(atlas);
    usize pixel_bytes;
    if (!checked_mul_usize((usize)atlas->atlas_width,(usize)pixel_height,&pixel_bytes)) return false;
    u8 digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(atlas->atlas_bitmap,(CC_LONG)size,digest);
    pthread_mutex_lock(&s_seed_mu);
    FontSharedSeed *seed;
    for(seed=s_seeds;seed;seed=seed->next) {
        if(seed->device==device && seed->width==atlas->atlas_width && seed->height==atlas->atlas_height && seed->pixel_height==pixel_height &&
           memcmp(seed->digest,digest,sizeof digest)==0) break;
    }
    if (!seed) {
        seed=calloc(1,sizeof(*seed));
        if (!seed) { pthread_mutex_unlock(&s_seed_mu);return false; }
        MTLTextureDescriptor *desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:atlas->atlas_width height:pixel_height mipmapped:NO];
        desc.storageMode=MTLStorageModePrivate;desc.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
        seed->texture=[(id<MTLDevice>)device newTextureWithDescriptor:desc];
        if (!seed->texture) { free(seed);pthread_mutex_unlock(&s_seed_mu);return false; }
        renderer_metal_upload_texture((void *)seed->texture,0,0,atlas->atlas_width,pixel_height,atlas->atlas_bitmap,pixel_bytes);
        seed->device=device;seed->width=atlas->atlas_width;seed->height=atlas->atlas_height;
        seed->pixel_height=pixel_height;
        memcpy(seed->digest,digest,sizeof digest);seed->bytes=pixel_bytes;
        seed->next=s_seeds;s_seeds=seed;
        mem_budget_transient_adjust(MEM_FONT_GPU,(isize)pixel_bytes);
        mem_budget_transient_adjust(MEM_FONT_CPU,(isize)sizeof(*seed));
    }
    seed->refs++;
    atlas->shared_seed=seed;atlas->metal_seed_texture=(void *)seed->texture;
    pthread_mutex_unlock(&s_seed_mu);return true;
}
#endif
