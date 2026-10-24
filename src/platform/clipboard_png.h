#ifndef PLATFORM_CLIPBOARD_PNG_H
#define PLATFORM_CLIPBOARD_PNG_H
#include "core/types.h"
#include <string.h>
#define PLATFORM_CLIPBOARD_PNG_MAX_BYTES (4u * 1024u * 1024u)
static inline u32 clipboard_png_u32(const u8 *p) { return (u32)p[0]<<24|(u32)p[1]<<16|(u32)p[2]<<8|p[3]; }
/* Internal writers supply complete PNGs; reject malformed/oversized headers
 * before any clipboard ownership change, without decoding on the UI thread. */
static inline bool clipboard_png_header_valid(const u8 *data,usize len) {
    static const u8 magic[8]={137,80,78,71,13,10,26,10};
    if(!data||len<33||len>PLATFORM_CLIPBOARD_PNG_MAX_BYTES||memcmp(data,magic,8)||
       clipboard_png_u32(data+8)!=13||memcmp(data+12,"IHDR",4))return false;
    u32 w=clipboard_png_u32(data+16),h=clipboard_png_u32(data+20);
    return w&&h&&w<=4096&&h<=4096&&(u64)w*h<=4194304&&data[24]==8&&
           (data[25]==2||data[25]==6)&&!data[26]&&!data[27]&&data[28]<=1;
}
#endif
