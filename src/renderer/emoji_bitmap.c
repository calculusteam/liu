/*
 * Liu — color bitmap font reader (CBDT/CBLC). See emoji_bitmap.h for why this
 * exists.
 *
 * Scope is deliberately narrow: enough of the OpenType tables to answer
 * "give me the RGBA for this codepoint" for the fonts that actually ship on
 * Linux desktops (NotoColorEmoji: cmap format 12, CBLC indexFormat 1,
 * imageFormat 17). Formats 4/6 (cmap) and 18/19 (CBDT) are handled too since
 * they cost a few lines each and other emoji fonts use them.
 */
#include "renderer/emoji_bitmap.h"
#include "stb_image.h"

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* Largest embedded bitmap strike a colour font may carry; Apple's sbix tops
 * out at 160 px, Noto's CBDT at 136. */
#define EMOJI_PNG_MAX_DIM 1024

struct EmojiFont {
    const u8 *data;
    usize     size;

    usize cmap_off, cmap_len;
    usize cblc_off, cblc_len;
    usize cbdt_off, cbdt_len;

    usize cmap_sub;      /* chosen cmap subtable, absolute offset */
    u16   cmap_format;
};

/* ---------------------------------------------------------------- reading
 * Every accessor takes an ABSOLUTE offset and returns 0 when the read would
 * leave the mapping. Callers that must distinguish "zero" from "out of range"
 * validate the offset themselves first via in_range(). */

static bool in_range(const EmojiFont *f, usize off, usize len) {
    return off <= f->size && len <= f->size - off;
}

static u16 rd16(const EmojiFont *f, usize off) {
    if (!in_range(f, off, 2)) return 0;
    return (u16)((f->data[off] << 8) | f->data[off + 1]);
}

static u32 rd32(const EmojiFont *f, usize off) {
    if (!in_range(f, off, 4)) return 0;
    return ((u32)f->data[off] << 24) | ((u32)f->data[off + 1] << 16) |
           ((u32)f->data[off + 2] << 8) | (u32)f->data[off + 3];
}

/* ------------------------------------------------------------------ cmap */

/* Pick the best Unicode subtable. Format 12 first: it is the only one that can
 * address the astral plane, which is where nearly every emoji lives (U+1F300+).
 * A format-4 font can still supply the BMP symbols (☀ ✈ ❤). */
static void cmap_select(EmojiFont *f) {
    f->cmap_sub = 0;
    f->cmap_format = 0;
    if (!f->cmap_off) return;

    u16 n = rd16(f, f->cmap_off + 2);
    usize best = 0;
    u16 best_fmt = 0;

    for (u16 i = 0; i < n; i++) {
        usize rec = f->cmap_off + 4 + (usize)i * 8;
        if (!in_range(f, rec, 8)) break;
        u16 platform = rd16(f, rec);
        u16 encoding = rd16(f, rec + 2);
        u32 sub_off  = rd32(f, rec + 4);
        usize sub = f->cmap_off + sub_off;
        if (!in_range(f, sub, 4)) continue;

        bool unicode = (platform == 0) ||
                       (platform == 3 && (encoding == 1 || encoding == 10));
        if (!unicode) continue;

        u16 fmt = rd16(f, sub);
        if (fmt != 4 && fmt != 6 && fmt != 12) continue;
        /* 12 beats 4 beats 6. */
        u16 rank = (fmt == 12) ? 3 : (fmt == 4) ? 2 : 1;
        u16 best_rank = (best_fmt == 12) ? 3 : (best_fmt == 4) ? 2 : (best_fmt == 6) ? 1 : 0;
        if (rank > best_rank) { best = sub; best_fmt = fmt; }
    }
    f->cmap_sub = best;
    f->cmap_format = best_fmt;
}

static u32 cmap_lookup_12(const EmojiFont *f, usize sub, u32 cp) {
    u32 ngroups = rd32(f, sub + 12);
    usize base = sub + 16;
    /* Groups are sorted by startCharCode — binary search. */
    u32 lo = 0, hi = ngroups;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        usize g = base + (usize)mid * 12;
        if (!in_range(f, g, 12)) return 0;
        u32 start = rd32(f, g), end = rd32(f, g + 4);
        if (cp < start)      hi = mid;
        else if (cp > end)   lo = mid + 1;
        else                 return rd32(f, g + 8) + (cp - start);
    }
    return 0;
}

static u32 cmap_lookup_4(const EmojiFont *f, usize sub, u32 cp) {
    if (cp > 0xFFFF) return 0;
    u16 segx2 = rd16(f, sub + 6);
    if (segx2 < 2) return 0;
    u16 segs = (u16)(segx2 / 2);
    usize ends    = sub + 14;
    usize starts  = ends + segx2 + 2;      /* +2 for reservedPad */
    usize deltas  = starts + segx2;
    usize ranges  = deltas + segx2;

    for (u16 i = 0; i < segs; i++) {
        u16 end = rd16(f, ends + (usize)i * 2);
        if (cp > end) continue;
        u16 start = rd16(f, starts + (usize)i * 2);
        if (cp < start) return 0;
        u16 delta = rd16(f, deltas + (usize)i * 2);
        usize ro_at = ranges + (usize)i * 2;
        u16 range_off = rd16(f, ro_at);
        if (range_off == 0) return (u32)((u16)(cp + delta));
        usize gi = ro_at + range_off + (usize)(cp - start) * 2;
        u16 g = rd16(f, gi);
        if (g == 0) return 0;
        return (u32)((u16)(g + delta));
    }
    return 0;
}

static u32 cmap_lookup_6(const EmojiFont *f, usize sub, u32 cp) {
    u16 first = rd16(f, sub + 6);
    u16 count = rd16(f, sub + 8);
    if (cp < first || cp >= (u32)first + count) return 0;
    return rd16(f, sub + 10 + (usize)(cp - first) * 2);
}

static u32 glyph_for_codepoint(const EmojiFont *f, u32 cp) {
    if (!f->cmap_sub) return 0;
    switch (f->cmap_format) {
    case 12: return cmap_lookup_12(f, f->cmap_sub, cp);
    case 4:  return cmap_lookup_4(f, f->cmap_sub, cp);
    case 6:  return cmap_lookup_6(f, f->cmap_sub, cp);
    default: return 0;
    }
}

/* ------------------------------------------------------------- CBLC/CBDT */

/* Locate the CBDT byte range holding `gid`'s image, and the image format that
 * describes its layout. Returns false when the glyph has no bitmap. */
static bool cblc_find_image(const EmojiFont *f, u32 gid,
                            usize *img_off, usize *img_len, u16 *img_fmt) {
    if (!f->cblc_off) return false;
    u32 num_sizes = rd32(f, f->cblc_off + 4);
    if (num_sizes > 64) num_sizes = 64;      /* sanity cap on a hostile file */

    for (u32 s = 0; s < num_sizes; s++) {
        /* BitmapSize record: 48 bytes. Only the index-subtable array
         * pointer, the glyph range, and the count are needed. */
        usize rec = f->cblc_off + 8 + (usize)s * 48;
        if (!in_range(f, rec, 48)) return false;
        u32 idx_array_off = rd32(f, rec);
        u32 num_subtables = rd32(f, rec + 8);
        u16 start_glyph   = rd16(f, rec + 40);
        u16 end_glyph     = rd16(f, rec + 42);
        if (gid < start_glyph || gid > end_glyph) continue;
        if (num_subtables > 4096) continue;

        usize idx_array = f->cblc_off + idx_array_off;
        for (u32 k = 0; k < num_subtables; k++) {
            usize e = idx_array + (usize)k * 8;
            if (!in_range(f, e, 8)) break;
            u16 first = rd16(f, e);
            u16 last  = rd16(f, e + 2);
            u32 add   = rd32(f, e + 4);
            if (gid < first || gid > last) continue;

            usize st = idx_array + add;
            if (!in_range(f, st, 8)) return false;
            u16 index_format = rd16(f, st);
            u16 image_format = rd16(f, st + 2);
            u32 image_data_off = rd32(f, st + 4);
            u32 rel = gid - first;

            usize start_off = 0, end_off = 0;
            if (index_format == 1) {
                /* uint32 offsets, one per glyph plus a terminator. */
                usize a = st + 8 + (usize)rel * 4;
                if (!in_range(f, a, 8)) return false;
                start_off = rd32(f, a);
                end_off   = rd32(f, a + 4);
            } else if (index_format == 2) {
                /* Constant-size images: imageSize then bigGlyphMetrics. */
                u32 image_size = rd32(f, st + 8);
                if (!image_size) return false;
                start_off = (usize)rel * image_size;
                end_off   = start_off + image_size;
            } else if (index_format == 3) {
                /* uint16 offsets. */
                usize a = st + 8 + (usize)rel * 2;
                if (!in_range(f, a, 4)) return false;
                start_off = rd16(f, a);
                end_off   = rd16(f, a + 2);
            } else {
                return false;   /* formats 4/5 are sparse; no shipping font uses them here */
            }

            if (end_off <= start_off) return false;
            usize off = f->cbdt_off + image_data_off + start_off;
            usize len = end_off - start_off;
            if (!in_range(f, off, len)) return false;

            *img_off = off;
            *img_len = len;
            *img_fmt = image_format;
            return true;
        }
    }
    return false;
}

/* Strip the metrics header so only the PNG payload remains. The three formats
 * differ solely in how many bytes of metrics precede the length field. */
static bool image_png_range(const EmojiFont *f, usize off, usize len, u16 fmt,
                            usize *png_off, usize *png_len) {
    usize header;
    switch (fmt) {
    case 17: header = 5; break;   /* smallGlyphMetrics */
    case 18: header = 8; break;   /* bigGlyphMetrics   */
    case 19: /* raw PNG, no metrics and no length prefix */
        *png_off = off;
        *png_len = len;
        return true;
    default: return false;
    }
    if (len < header + 4) return false;
    u32 data_len = rd32(f, off + header);
    usize start = off + header + 4;
    if (data_len == 0 || data_len > len - header - 4) return false;
    if (!in_range(f, start, data_len)) return false;
    *png_off = start;
    *png_len = data_len;
    return true;
}

/* --------------------------------------------------------------- public */

EmojiFont *emoji_font_open(const char *path) {
    if (!path || !path[0]) return NULL;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    /* Same ceiling font.c puts on an outline face: a font is a file the user
     * pointed at, and a multi-gigabyte one is not mapped just because its
     * extension says .ttf. */
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 12 ||
        (u64)st.st_size > (u64)MB(256)) {
        close(fd);
        return NULL;
    }
    usize size = (usize)st.st_size;
    /* mmap rather than read: NotoColorEmoji is ~10 MB and a session may touch
     * a handful of glyphs, so paging in on demand keeps the resident cost to
     * the bitmaps actually used. */
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return NULL;

    EmojiFont *f = calloc(1, sizeof *f);
    if (!f) { munmap(map, size); return NULL; }
    f->data = (const u8 *)map;
    f->size = size;

    u32 tag = rd32(f, 0);
    usize dir = 12;
    if (tag == 0x74746366u /* 'ttcf' */) {
        /* Collection: take the first face. */
        u32 off = rd32(f, 12);
        if (!in_range(f, off, 12)) { emoji_font_close(f); return NULL; }
        dir = off + 12;
        tag = rd32(f, off);
    }
    if (tag != 0x00010000u && tag != 0x4F54544Fu /* 'OTTO' */ &&
        tag != 0x74727565u /* 'true' */) {
        emoji_font_close(f);
        return NULL;
    }

    u16 num_tables = rd16(f, dir - 8);
    if (num_tables > 512) { emoji_font_close(f); return NULL; }
    for (u16 i = 0; i < num_tables; i++) {
        usize e = dir + (usize)i * 16;
        if (!in_range(f, e, 16)) break;
        u32 t = rd32(f, e);
        u32 off = rd32(f, e + 8);
        u32 len = rd32(f, e + 12);
        if (!in_range(f, off, len)) continue;
        switch (t) {
        case 0x636D6170u: f->cmap_off = off; f->cmap_len = len; break;  /* cmap */
        case 0x43424C43u: f->cblc_off = off; f->cblc_len = len; break;  /* CBLC */
        case 0x43424454u: f->cbdt_off = off; f->cbdt_len = len; break;  /* CBDT */
        default: break;
        }
    }

    /* No color bitmaps: leave the font to the outline path. */
    if (!f->cmap_off || !f->cblc_off || !f->cbdt_off) {
        emoji_font_close(f);
        return NULL;
    }
    cmap_select(f);
    if (!f->cmap_sub) { emoji_font_close(f); return NULL; }
    return f;
}

void emoji_font_close(EmojiFont *f) {
    if (!f) return;
    if (f->data) munmap((void *)f->data, f->size);
    free(f);
}

bool emoji_font_has_glyph(EmojiFont *f, u32 cp) {
    if (!f) return false;
    u32 gid = glyph_for_codepoint(f, cp);
    if (!gid) return false;
    usize off, len; u16 fmt;
    return cblc_find_image(f, gid, &off, &len, &fmt);
}

u8 *emoji_font_glyph_rgba(EmojiFont *f, u32 cp, i32 *w, i32 *h) {
    if (!f || !w || !h) return NULL;
    *w = *h = 0;

    u32 gid = glyph_for_codepoint(f, cp);
    if (!gid) return NULL;

    usize img_off, img_len; u16 img_fmt;
    if (!cblc_find_image(f, gid, &img_off, &img_len, &img_fmt)) return NULL;

    usize png_off, png_len;
    if (!image_png_range(f, img_off, img_len, img_fmt, &png_off, &png_len))
        return NULL;
    if (png_len > (usize)0x7FFFFFFF) return NULL;

    /* Read the header before decoding, as the terminal image path does: stb's
     * own limit is 2^24 per axis, so a PNG declaring 16000x16000 would be
     * allocated in full (1 GB) before any box-fit ran. A colour-emoji strike
     * is at most a few hundred pixels a side. */
    int iw = 0, ih = 0, channels = 0;
    if (!stbi_info_from_memory(f->data + png_off, (int)png_len, &iw, &ih, &channels) ||
        iw <= 0 || ih <= 0 || iw > EMOJI_PNG_MAX_DIM || ih > EMOJI_PNG_MAX_DIM)
        return NULL;
    u8 *rgba = stbi_load_from_memory(f->data + png_off, (int)png_len,
                                     &iw, &ih, &channels, 4);
    if (!rgba) return NULL;
    if (iw <= 0 || ih <= 0) { stbi_image_free(rgba); return NULL; }

    *w = iw;
    *h = ih;
    return rgba;   /* stb allocates with malloc; caller frees with free() */
}
