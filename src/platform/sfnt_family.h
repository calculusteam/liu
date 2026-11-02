/*
 * Liu - the family name a font file carries (Linux screen readers)
 *
 * AT-SPI's family-name text attribute names the face the pane is drawn in.
 * The font file's own `name` table says it ("JetBrainsMono Nerd Font Mono"),
 * where the file's name does not ("JetBrainsMonoNerdFontMono-Regular.ttf").
 *
 * sfnt_name_family reads a `name` table that is already in memory, bounded
 * by the length it is given: the typographic family (name ID 16) before the
 * legacy one (ID 1), English (US) before any other language, a Windows
 * Unicode record (UTF-16BE) before a Macintosh Roman one. sfnt_family_file
 * finds the table in a font file with a few positioned reads, never the whole
 * file: it is opened non-blocking and must be a regular file, the table
 * directory is bounded, and so is the table.
 *
 * Pure C: it needs no accessibility bus and no font loader.
 */
#ifndef LIU_PLATFORM_SFNT_FAMILY_H
#define LIU_PLATFORM_SFNT_FAMILY_H

#include "core/types.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SFNT_FAMILY_MAX_TABLES   256u        /* a table directory beyond this is refused */
#define SFNT_FAMILY_MAX_NAME_TAB (512u * 1024u)

static inline u32 sfnt_family_be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static inline u32 sfnt_family_be16(const u8 *p) { return ((u32)p[0] << 8) | p[1]; }

/* A name record's string as UTF-8 into `out`: UTF-16BE (Windows) or Mac Roman
 * (only its ASCII half; a byte above that is dropped). False when nothing
 * printable came out. */
static inline bool sfnt_family_string(const u8 *s, usize n, bool utf16, char *out, usize cap) {
    usize o = 0;
    if (cap == 0) return false;
    if (utf16) {
        for (usize i = 0; i + 1 < n; i += 2) {
            u32 cp = sfnt_family_be16(s + i);
            if (cp >= 0xD800u && cp < 0xDC00u && i + 3 < n) {      /* a surrogate pair */
                u32 lo = sfnt_family_be16(s + i + 2);
                if (lo >= 0xDC00u && lo < 0xE000u) {
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    i += 2;
                }
            }
            if (cp < 0x20u || cp == 0x7Fu || (cp >= 0xD800u && cp < 0xE000u)) continue;
            usize need = cp < 0x80u ? 1 : cp < 0x800u ? 2 : cp < 0x10000u ? 3 : 4;
            if (o + need >= cap) break;
            if (need == 1) out[o++] = (char)cp;
            else if (need == 2) { out[o++] = (char)(0xC0u | (cp >> 6)); out[o++] = (char)(0x80u | (cp & 0x3Fu)); }
            else if (need == 3) {
                out[o++] = (char)(0xE0u | (cp >> 12)); out[o++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                out[o++] = (char)(0x80u | (cp & 0x3Fu));
            } else {
                out[o++] = (char)(0xF0u | (cp >> 18)); out[o++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
                out[o++] = (char)(0x80u | ((cp >> 6) & 0x3Fu)); out[o++] = (char)(0x80u | (cp & 0x3Fu));
            }
        }
    } else {
        for (usize i = 0; i < n && o + 1 < cap; i++)
            if (s[i] >= 0x20u && s[i] < 0x7Fu) out[o++] = (char)s[i];
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = '\0';
    return o > 0;
}

/* The family name in the `name` table at tab[0..n). False when it holds none
 * (or the table is malformed); `out` is then empty. */
static inline bool sfnt_name_family(const u8 *tab, usize n, char *out, usize cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!tab || n < 6) return false;
    u32 count = sfnt_family_be16(tab + 2), str_off = sfnt_family_be16(tab + 4);
    if (count > (n - 6) / 12 || str_off > n) return false;
    /* Candidates, best first. Platform 3 / language 0x409 outranks every
     * other language, any Windows record outranks a Macintosh one, and
     * the typographic family outranks the legacy one within each. */
    i32 best = -1;
    u32 best_rank = 0;
    for (u32 i = 0; i < count; i++) {
        const u8 *r = tab + 6 + (usize)i * 12;
        u32 plat = sfnt_family_be16(r), enc = sfnt_family_be16(r + 2);
        u32 lang = sfnt_family_be16(r + 4), id = sfnt_family_be16(r + 6);
        u32 len = sfnt_family_be16(r + 8), off = sfnt_family_be16(r + 10);
        if ((id != 16u && id != 1u) || len == 0) continue;
        if (off > n - str_off || len > n - str_off - off) continue;
        bool win = plat == 3u && (enc == 1u || enc == 10u);
        bool mac = plat == 1u && enc == 0u && lang == 0u;
        if (!win && !mac) continue;
        u32 rank = 1u + (win ? 4u : 0u) + ((win && lang == 0x409u) || mac ? 2u : 0u) + (id == 16u ? 1u : 0u);
        if (rank > best_rank) { best_rank = rank; best = (i32)i; }
    }
    if (best < 0) return false;
    const u8 *r = tab + 6 + (usize)best * 12;
    u32 plat = sfnt_family_be16(r), len = sfnt_family_be16(r + 8), off = sfnt_family_be16(r + 10);
    return sfnt_family_string(tab + str_off + off, len, plat == 3u, out, cap);
}

/* A positioned read of exactly n bytes. */
static inline bool sfnt_family_pread(int fd, void *buf, usize n, u64 at) {
    usize got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (char *)buf + got, n - got, (off_t)(at + got));
        if (r <= 0) return false;
        got += (usize)r;
    }
    return true;
}

/* The family name of the font file at `path` (the first font of a
 * collection). Bounded: a regular file, at most SFNT_FAMILY_MAX_TABLES
 * directory entries, a name table of at most SFNT_FAMILY_MAX_NAME_TAB
 * bytes that lies inside the file. False and an empty `out` otherwise. */
static inline bool sfnt_family_file(const char *path, char *out, usize cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!path || !path[0]) return false;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = false;
    struct stat st;
    u8 hdr[12], first[4];
    u8 *dir = NULL, *tab = NULL;
    u64 size = 0, base = 0;
    u32 tag, ntab, noff = 0, nlen = 0;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 12) goto done;
    size = (u64)st.st_size;
    if (!sfnt_family_pread(fd, hdr, sizeof(hdr), 0)) goto done;
    if (sfnt_family_be32(hdr) == 0x74746366u) {                 /* "ttcf": a collection */
        if (sfnt_family_be32(hdr + 8) == 0 || !sfnt_family_pread(fd, first, sizeof(first), 12)) goto done;
        base = sfnt_family_be32(first);
        if (base > size - 12 || !sfnt_family_pread(fd, hdr, sizeof(hdr), base)) goto done;
    }
    tag = sfnt_family_be32(hdr);
    if (tag != 0x00010000u && tag != 0x4F54544Fu && tag != 0x74727565u) goto done;
    ntab = sfnt_family_be16(hdr + 4);
    if (ntab == 0 || ntab > SFNT_FAMILY_MAX_TABLES || base + 12 + (u64)ntab * 16 > size) goto done;
    dir = (u8 *)malloc((usize)ntab * 16);
    if (!dir || !sfnt_family_pread(fd, dir, (usize)ntab * 16, base + 12)) goto done;
    for (u32 i = 0; i < ntab; i++) {
        const u8 *e = dir + (usize)i * 16;
        if (memcmp(e, "name", 4) == 0) { noff = sfnt_family_be32(e + 8); nlen = sfnt_family_be32(e + 12); break; }
    }
    if (nlen < 6 || nlen > SFNT_FAMILY_MAX_NAME_TAB || (u64)noff + nlen > size) goto done;
    tab = (u8 *)malloc(nlen);
    if (!tab || !sfnt_family_pread(fd, tab, nlen, noff)) goto done;
    ok = sfnt_name_family(tab, nlen, out, cap);
done:
    free(tab);
    free(dir);
    close(fd);
    if (!ok) out[0] = '\0';
    return ok;
}

#endif /* LIU_PLATFORM_SFNT_FAMILY_H */
