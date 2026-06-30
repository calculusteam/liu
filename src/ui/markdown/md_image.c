/*
 * Liu - Markdown image cache implementation.
 */
#include "ui/markdown/md_image.h"
#include "stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Total RGBA-byte budget across all live cache entries. When exceeded after an
 * insert, the LRU tail is evicted (rgba freed, entry kept and marked failed so
 * we don't immediately re-load the same large image). 32 MiB is enough for a
 * typical README's worth of inline screenshots without unbounded growth. */
#define MD_IMAGE_CACHE_BUDGET ((usize)32 * 1024 * 1024)
/* Hard cap on entry count so adversarial / auto-generated markdown
 * with thousands of distinct image refs can't grow bucket-chain
 * metadata (struct + strdup'd abs_path, ~150 B–2 KB each) without
 * bound. Pixels are already capped by MD_IMAGE_CACHE_BUDGET; this
 * caps the *metadata* tail that evicted entries leave behind. */
#define MD_IMAGE_CACHE_MAX_ENTRIES 2048

/* Oversized inline images are box-filter downscaled at decode so the GPU never
 * minifies a multi-thousand-px screenshot down to the <=480pt display size with
 * a single bilinear tap (which shimmers on scroll), and so the cache/texture
 * footprint tracks display size rather than source size. 1536 px keeps a sharp
 * margin above the 480pt display cap even at 3x backing scale. */
#define MD_IMAGE_MAX_DIM 1536

/* Process-global, monotonic id stamped on every successful rgba load. Used as
 * the GPU image-cache generation: MdImageEntry structs are freed and their
 * addresses malloc-reused across document switches, so keying the renderer's
 * pointer-addressed image cache by the struct address alone (at a constant
 * generation) let a recycled address alias a previous document's texture.
 * A unique generation per load makes the renderer re-upload instead. */
static u64 g_md_image_serial = 0;

/* Premultiply straight-alpha RGBA in place, in the stored sRGB byte domain, to
 * match the renderer's premultiplied-alpha blend contract (image pipeline blend
 * is (ONE, 1-srcA) and the shader emits the texel rgb directly). Without this,
 * transparent-PNG edges show dark/light fringes. Opaque texels (a==255) are
 * left bit-exact, so fully-opaque images are unchanged. */
static void md_premultiply_rgba(u8 *px, i32 w, i32 h) {
    i64 n = (i64)w * h;
    for (i64 i = 0; i < n; i++) {
        u8 *p = px + i * 4;
        u8 a = p[3];
        if (a == 255) continue;
        p[0] = (u8)((p[0] * a + 127) / 255);
        p[1] = (u8)((p[1] * a + 127) / 255);
        p[2] = (u8)((p[2] * a + 127) / 255);
    }
}

/* Area-average (box filter) RGBA downscale src(sw x sh) -> dst(dw x dh). Each
 * source pixel is read exactly once, so cost is O(sw*sh). The caller has
 * already premultiplied, so averaging is alpha-correct (transparent texels
 * don't bleed RGB into neighbours). */
static void md_box_downscale_rgba(const u8 *src, i32 sw, i32 sh,
                                  u8 *dst, i32 dw, i32 dh) {
    for (i32 dy = 0; dy < dh; dy++) {
        i32 sy0 = (i32)((i64)dy * sh / dh);
        i32 sy1 = (i32)((i64)(dy + 1) * sh / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (i32 dx = 0; dx < dw; dx++) {
            i32 sx0 = (i32)((i64)dx * sw / dw);
            i32 sx1 = (i32)((i64)(dx + 1) * sw / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            u32 r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (i32 yy = sy0; yy < sy1; yy++) {
                const u8 *row = src + ((i64)yy * sw + sx0) * 4;
                for (i32 xx = sx0; xx < sx1; xx++) {
                    r += row[0]; g += row[1]; b += row[2]; a += row[3];
                    row += 4; cnt++;
                }
            }
            u8 *d = dst + ((i64)dy * dw + dx) * 4;
            d[0] = (u8)(r / cnt); d[1] = (u8)(g / cnt);
            d[2] = (u8)(b / cnt); d[3] = (u8)(a / cnt);
        }
    }
}

/* LRU helpers: head = most-recently-used, tail = eviction victim. */
static void lru_unlink(MdImageCache *c, MdImageEntry *e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else if (c->lru_head == e) c->lru_head = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else if (c->lru_tail == e) c->lru_tail = e->lru_prev;
    e->lru_prev = e->lru_next = NULL;
}

static void lru_push_head(MdImageCache *c, MdImageEntry *e) {
    e->lru_prev = NULL;
    e->lru_next = c->lru_head;
    if (c->lru_head) c->lru_head->lru_prev = e;
    c->lru_head = e;
    if (!c->lru_tail) c->lru_tail = e;
}

/* FNV-1a 32-bit hash for ASCII paths. */
static u32 fnv1a(const char *s) {
    u32 h = 0x811c9dc5u;
    for (; *s; s++) {
        h ^= (u8)*s;
        h *= 0x01000193u;
    }
    return h;
}

/* Concat base_dir + "/" + ref → absolute-ish path. ref may already be
 * absolute (starts with '/') in which case base_dir is ignored. ref is a
 * length-bounded slice (ref_len bytes — not NUL-terminated). */
static void resolve_path(const char *base_dir, const char *ref, u32 ref_len,
                         char *out, usize out_sz) {
    if (ref_len == 0) {
        if (out_sz) out[0] = '\0';
        return;
    }
    if (ref[0] == '/') {
        /* absolute; cap by ref_len */
        usize n = ref_len < out_sz - 1 ? ref_len : out_sz - 1;
        memcpy(out, ref, n);
        out[n] = '\0';
        return;
    }
    if (!base_dir || !*base_dir) {
        usize n = ref_len < out_sz - 1 ? ref_len : out_sz - 1;
        memcpy(out, ref, n);
        out[n] = '\0';
        return;
    }
    snprintf(out, out_sz, "%s/%.*s", base_dir, (i32)ref_len, ref);
}

MdImageCache *md_image_cache_create(void) {
    MdImageCache *c = calloc(1, sizeof(*c));
    return c;
}

void md_image_cache_destroy(MdImageCache *c) {
    if (!c) return;
    for (i32 i = 0; i < (i32)(sizeof c->buckets / sizeof c->buckets[0]); i++) {
        MdImageEntry *e = c->buckets[i];
        while (e) {
            MdImageEntry *next = e->next;
            free(e->abs_path);
            if (e->rgba) stbi_image_free(e->rgba);
            free(e);
            e = next;
        }
    }
    free(c);
}

const MdImageEntry *md_image_cache_get(MdImageCache *c, const char *base_dir,
                                       const char *ref, u32 ref_len) {
    if (!c) return NULL;
    char abs_path[2048];
    resolve_path(base_dir, ref, ref_len, abs_path, sizeof abs_path);

    u32 h = fnv1a(abs_path);
    u32 bi = h % (u32)(sizeof c->buckets / sizeof c->buckets[0]);
    for (MdImageEntry *e = c->buckets[bi]; e; e = e->next) {
        if (strcmp(e->abs_path, abs_path) == 0) {
            /* Hit: bump to LRU head if currently linked. Failed/evicted
             * entries aren't in the LRU list (no rgba) — leave them be. */
            if (e->rgba && (e->lru_prev || e->lru_next || c->lru_head == e)) {
                lru_unlink(c, e);
                lru_push_head(c, e);
            }
            return e;
        }
    }

    /* Miss: load. Refuse new entries once the metadata cap is hit;
     * caller sees NULL and renders a placeholder. Without this, an
     * auto-generated .md with thousands of distinct image refs
     * grows bucket-chain metadata unboundedly within one viewer
     * session — pixel budget alone won't catch it because evicted
     * entries keep their struct + abs_path in the bucket. */
    if (c->entry_count >= MD_IMAGE_CACHE_MAX_ENTRIES) return NULL;

    MdImageEntry *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->abs_path = strdup(abs_path);
    if (!e->abs_path) { free(e); return NULL; }

    int iw = 0, ih = 0, ich = 0;
    u8 *px = stbi_load(abs_path, &iw, &ih, &ich, 4);
    if (px && iw > 0 && ih > 0) {
        /* Reject absurdly large images (256 megapixels) so a malicious .md
         * can't OOM the viewer. */
        if ((i64)iw * (i64)ih > (i64)256 * 1024 * 1024) {
            stbi_image_free(px);
            e->failed = true;
        } else {
            /* Premultiply (only when the source carried alpha) then box-
             * downscale oversized images before caching/upload. Premultiply
             * first so the downscale averages premultiplied texels. */
            if (ich == 4) md_premultiply_rgba(px, iw, ih);
            i32 longest = iw > ih ? iw : ih;
            if (longest > MD_IMAGE_MAX_DIM) {
                f32 s = (f32)MD_IMAGE_MAX_DIM / (f32)longest;
                i32 nw = (i32)((f32)iw * s + 0.5f); if (nw < 1) nw = 1;
                i32 nh = (i32)((f32)ih * s + 0.5f); if (nh < 1) nh = 1;
                u8 *down = (u8 *)malloc((usize)nw * (usize)nh * 4u);
                if (down) {
                    md_box_downscale_rgba(px, iw, ih, down, nw, nh);
                    /* STBI_FREE defaults to free(), so the malloc'd `down`
                     * buffer is later freed the same way as a stbi buffer. */
                    stbi_image_free(px);
                    px = down; iw = nw; ih = nh;
                }
            }
            e->rgba  = px;
            e->generation = ++g_md_image_serial;
            e->w     = iw;
            e->h     = ih;
            e->bytes = (usize)iw * (usize)ih * 4u;
            c->total_bytes += e->bytes;
            lru_push_head(c, e);
        }
    } else {
        e->failed = true;
    }

    e->next = c->buckets[bi];
    c->buckets[bi] = e;
    c->entry_count++;

    /* Enforce byte budget by evicting LRU tails. We keep the bucket entry
     * (marked failed) so a subsequent get() short-circuits instead of
     * thrashing stbi_load on the same oversized image. */
    /* `c->lru_tail != e` keeps the just-inserted entry alive even when it
     * alone exceeds the budget — otherwise a single large image would be
     * freed + marked failed on the very call that decoded it, rendering blank
     * forever (failed is sticky). */
    while (c->total_bytes > MD_IMAGE_CACHE_BUDGET && c->lru_tail && c->lru_tail != e) {
        MdImageEntry *victim = c->lru_tail;
        lru_unlink(c, victim);
        if (victim->rgba) {
            stbi_image_free(victim->rgba);
            victim->rgba = NULL;
        }
        c->total_bytes -= victim->bytes;
        victim->bytes  = 0;
        victim->failed = true;
    }
    return e;
}
