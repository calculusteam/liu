/*
 * sftp_ink.h — the SFTP tab's colours, taken from the theme's own palette.
 *
 * A row's name is coloured the way `ls --color` colours it, in the theme's
 * ANSI colours (Liu's theme says it outright: blue "is what `ls` prints for
 * directories"): a folder blue, a link cyan, an executable green, an archive
 * red, an image, a video or a sound magenta; any other file keeps the text
 * colour. The Permissions column colours its letters as a long listing is
 * usually coloured — r yellow, w red, x / s / t green, the type letter d blue
 * and l cyan — and leaves '-' faint. The two sides wear their own hue: this
 * machine the theme's blue, the host green while it is connected.
 *
 * A theme's ANSI colours are made for its terminal background, and some are
 * not legible as text there (a light theme's yellow): every ink is moved to
 * stand SFTP_INK_MIN_CONTRAST off the surface it is drawn on, darker on a
 * light surface and lighter on a dark one, its hue kept.
 *
 * Pure (with chrome_palette.c).
 */
#ifndef LIU_UI_SFTP_INK_H
#define LIU_UI_SFTP_INK_H

#include "core/types.h"
#include "ui/chrome_palette.h"
#include <string.h>
#include <strings.h>

#define SFTP_INK_MIN_CONTRAST 4.5f   /* WCAG AA for text */

typedef enum {
    SFTP_INK_PLAIN = 0,   /* the text colour */
    SFTP_INK_DIR,
    SFTP_INK_LINK,
    SFTP_INK_EXEC,
    SFTP_INK_ARCHIVE,
    SFTP_INK_MEDIA,
    SFTP_INK_COUNT
} SftpInk;

/* The ANSI colour an ink is (Theme.ansi index), or -1 for the text colour. */
static inline i32 sftp_ink_ansi(SftpInk k) {
    switch (k) {
    case SFTP_INK_DIR:     return 4;   /* blue    */
    case SFTP_INK_LINK:    return 6;   /* cyan    */
    case SFTP_INK_EXEC:    return 2;   /* green   */
    case SFTP_INK_ARCHIVE: return 1;   /* red     */
    case SFTP_INK_MEDIA:   return 5;   /* magenta */
    default:               return -1;
    }
}

static inline bool sftp_ext_in(const char *ext, const char *const *list) {
    for (i32 i = 0; list[i]; i++) if (strcasecmp(ext, list[i]) == 0) return true;
    return false;
}

/* What a row is, for its colour. A link is a link even when it names a folder
 * (the list enters it as one); the executable bit counts only once it is
 * known (a local row stats lazily). Hidden rows are the caller's: they stay
 * dim whatever they are. */
static inline SftpInk sftp_entry_ink(const char *name, bool is_dir, bool is_link,
                                     u32 perms, bool perms_known) {
    static const char *const archive[] = {
        "zip", "tar", "gz", "tgz", "bz2", "tbz", "tbz2", "xz", "txz", "zst", "tzst", "lz",
        "lzma", "lz4", "7z", "rar", "z", "jar", "war", "deb", "rpm", "apk", "cab", "cpio", NULL };
    static const char *const media[] = {
        "png", "jpg", "jpeg", "gif", "bmp", "webp", "svg", "ico", "icns", "tif", "tiff", "heic",
        "heif", "avif", "psd", "mp4", "mov", "mkv", "avi", "webm", "m4v", "mp3", "wav", "flac",
        "ogg", "oga", "m4a", "aac", "opus", NULL };
    if (is_link) return SFTP_INK_LINK;
    if (is_dir) return SFTP_INK_DIR;
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (dot && dot != name && dot[1]) {
        if (sftp_ext_in(dot + 1, archive)) return SFTP_INK_ARCHIVE;
        if (sftp_ext_in(dot + 1, media)) return SFTP_INK_MEDIA;
    }
    if (perms_known && (perms & 0111u)) return SFTP_INK_EXEC;
    return SFTP_INK_PLAIN;
}

/* A permission string's letter (sftp_perm_string: "drwxr-xr-x"), as an ANSI
 * index, or -1 for faint. `at` 0 is the type letter. */
static inline i32 sftp_perm_char_ansi(char c, i32 at) {
    if (at == 0) return c == 'd' ? 4 : c == 'l' ? 6 : -1;
    switch (c) {
    case 'r': return 3;                                  /* yellow */
    case 'w': return 1;                                  /* red    */
    case 'x': case 's': case 't': case 'S': case 'T':
              return 2;                                  /* green  */
    default:  return -1;
    }
}

static inline f32 sftp_contrast(f32 la, f32 lb) {
    return la > lb ? (la + 0.05f) / (lb + 0.05f) : (lb + 0.05f) / (la + 0.05f);
}

/* `c` moved just far enough to stand `min` off `surface`: scaled toward
 * black on a light surface, mixed toward white on a dark one (both keep the
 * hue), by bisection — the least change that qualifies. A colour that already
 * qualifies comes back as it is. */
static inline Color sftp_ink_on(Color c, Color surface, f32 min) {
    c.a = 1.0f;
    f32 ls = chrome_luminance(surface);
    if (sftp_contrast(chrome_luminance(c), ls) >= min) return c;
    bool light = ls >= 0.179f;          /* where black and white read alike */
    f32 lo = 0.0f, hi = 1.0f;           /* how far it is moved */
    for (i32 i = 0; i < 14; i++) {
        f32 k = (lo + hi) * 0.5f;
        Color m = light ? (Color){ c.r * (1.0f - k), c.g * (1.0f - k), c.b * (1.0f - k), 1.0f }
                        : (Color){ c.r + (1.0f - c.r) * k, c.g + (1.0f - c.g) * k,
                                   c.b + (1.0f - c.b) * k, 1.0f };
        if (sftp_contrast(chrome_luminance(m), ls) >= min) hi = k; else lo = k;
    }
    f32 k = hi;
    return light ? (Color){ c.r * (1.0f - k), c.g * (1.0f - k), c.b * (1.0f - k), 1.0f }
                 : (Color){ c.r + (1.0f - c.r) * k, c.g + (1.0f - c.g) * k, c.b + (1.0f - c.b) * k, 1.0f };
}

#endif /* LIU_UI_SFTP_INK_H */
