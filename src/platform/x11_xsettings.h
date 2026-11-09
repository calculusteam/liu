/*
 * Liu - the XSETTINGS property (X11)
 *
 * A desktop that does not write Xft.dpi into the root window's
 * RESOURCE_MANAGER still publishes its scale: a desktop's settings daemon,
 * or a stand-alone one, owns the manager selection _XSETTINGS_S<screen>
 * and keep every setting in one binary property of the selection owner's
 * window, _XSETTINGS_SETTINGS (the XSETTINGS specification, version 0.5):
 *
 *   CARD8   byte order      0 = LSBFirst, 1 = MSBFirst
 *   3 bytes padding
 *   CARD32  serial
 *   CARD32  number of settings
 *   then each setting:
 *     CARD8   type          0 = integer, 1 = string, 2 = colour
 *     1 byte  padding
 *     CARD16  name length
 *     name, padded to a multiple of 4
 *     CARD32  last-change serial
 *     value   integer: INT32; string: CARD32 length + bytes, padded to 4;
 *             colour: four CARD16 (red, green, blue, alpha)
 *
 * The property is untrusted input from another client, so the reader walks it
 * with every length checked against what is left and stops at the first entry
 * that does not fit: the settings before it stay valid. An entry of an
 * unknown type has no length Liu could skip, so it ends the walk too.
 * Pure C: the format needs no server.
 */
#ifndef LIU_PLATFORM_X11_XSETTINGS_H
#define LIU_PLATFORM_X11_XSETTINGS_H

#include "core/types.h"
#include <string.h>

typedef enum { XSET_INT = 0, XSET_STRING = 1, XSET_COLOR = 2 } XSettingType;

typedef struct {
    const u8 *buf;
    usize     len;
    usize     pos;
    bool      big_endian;
    u32       remaining;    /* settings the header promised and are still to come */
    u32       serial;
} XSettingsReader;

typedef struct {
    XSettingType type;
    const char  *name;      /* not NUL-terminated */
    u32          name_len;
    u32          last_change;
    i32          ival;      /* XSET_INT */
    const u8    *sval;      /* XSET_STRING, not NUL-terminated */
    u32          slen;
    u16          rgba[4];   /* XSET_COLOR */
} XSettingsEntry;

static inline u32 xsettings_u16(const XSettingsReader *r, usize at) {
    const u8 *p = r->buf + at;
    return r->big_endian ? ((u32)p[0] << 8) | p[1] : ((u32)p[1] << 8) | p[0];
}

static inline u32 xsettings_u32(const XSettingsReader *r, usize at) {
    const u8 *p = r->buf + at;
    return r->big_endian
        ? ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]
        : ((u32)p[3] << 24) | ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0];
}

/* Begin a walk. False when the property is too short to hold the header or
 * names a byte order that does not exist. */
static inline bool xsettings_open(XSettingsReader *r, const u8 *buf, usize len) {
    memset(r, 0, sizeof *r);
    if (!buf || len < 12) return false;
    if (buf[0] != 0 && buf[0] != 1) return false;
    r->buf = buf;
    r->len = len;
    r->big_endian = buf[0] == 1;
    r->serial = xsettings_u32(r, 4);
    r->remaining = xsettings_u32(r, 8);
    r->pos = 12;
    return true;
}

/* The next setting; false at the end or at the first entry that is cut short,
 * has an unknown type or says more than the property holds. */
static inline bool xsettings_next(XSettingsReader *r, XSettingsEntry *e) {
    if (!r || !e || !r->buf || r->remaining == 0) return false;
    usize left = r->len - r->pos;
    if (left < 4) return false;

    u32 type = r->buf[r->pos];
    u32 name_len = xsettings_u16(r, r->pos + 2);
    if (type > XSET_COLOR) return false;

    /* type, padding, name length; the name padded; the change serial */
    u64 need = 4 + (u64)((name_len + 3u) & ~3u) + 4;
    if ((u64)left < need) return false;
    usize at = r->pos + 4;
    memset(e, 0, sizeof *e);
    e->type = (XSettingType)type;
    e->name = (const char *)(r->buf + at);
    e->name_len = name_len;
    at += (name_len + 3u) & ~3u;
    e->last_change = xsettings_u32(r, at);
    at += 4;

    left = r->len - at;
    switch (type) {
    case XSET_INT:
        if (left < 4) return false;
        e->ival = (i32)xsettings_u32(r, at);
        at += 4;
        break;
    case XSET_STRING: {
        if (left < 4) return false;
        u32 slen = xsettings_u32(r, at);
        at += 4;
        left -= 4;
        if ((u64)slen + ((4u - (slen & 3u)) & 3u) > (u64)left) return false;
        e->sval = r->buf + at;
        e->slen = slen;
        at += (usize)slen + ((4u - (slen & 3u)) & 3u);
        break;
    }
    default:
        if (left < 8) return false;
        for (int i = 0; i < 4; i++) e->rgba[i] = (u16)xsettings_u16(r, at + 2 * (usize)i);
        at += 8;
        break;
    }
    r->pos = at;
    r->remaining--;
    return true;
}

/* The settings that say how large things are. */
typedef struct {
    bool has_xft_dpi;        i32 xft_dpi_1024;        /* Xft/DPI: dots per inch x 1024 */
    bool has_window_scale;   i32 window_scale;        /* Gdk/WindowScalingFactor */
    bool has_unscaled_dpi;   i32 unscaled_dpi_1024;   /* Gdk/UnscaledDPI: dpi x 1024, before the window scale */
} XSettingsScale;

static inline bool xsettings_name_is(const XSettingsEntry *e, const char *name) {
    usize n = strlen(name);
    return e->name_len == n && memcmp(e->name, name, n) == 0;
}

/* Read the three scale settings out of a whole property. A property that does
 * not parse at all gives an empty answer; one cut short gives what came
 * before the cut. A later entry of the same name replaces an earlier one. */
static inline XSettingsScale xsettings_scale_of(const u8 *buf, usize len) {
    XSettingsScale s;
    memset(&s, 0, sizeof s);
    XSettingsReader r;
    XSettingsEntry e;
    if (!xsettings_open(&r, buf, len)) return s;
    while (xsettings_next(&r, &e)) {
        if (e.type != XSET_INT) continue;
        if (xsettings_name_is(&e, "Xft/DPI")) {
            s.has_xft_dpi = true; s.xft_dpi_1024 = e.ival;
        } else if (xsettings_name_is(&e, "Gdk/WindowScalingFactor")) {
            s.has_window_scale = true; s.window_scale = e.ival;
        } else if (xsettings_name_is(&e, "Gdk/UnscaledDPI")) {
            s.has_unscaled_dpi = true; s.unscaled_dpi_1024 = e.ival;
        }
    }
    return s;
}

#endif /* LIU_PLATFORM_X11_XSETTINGS_H */
