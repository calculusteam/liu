/*
 * Copy secret text to a pasteboard without clearing a newer clipboard value. On
 * macOS, mark the item with ConcealedType and clear it after the requested delay
 * only if the pasteboard change count still matches this write. The same ownership
 * check applies at shutdown. Clipboard access runs on the main thread; callers may
 * supply a private pasteboard.
 */
#import <AppKit/AppKit.h>
#include "platform/platform.h"
#include <stdlib.h>
#include <string.h>

static NSPasteboard *g_secret_pb = nil;      /* the pasteboard the last secret went to (retained) */
static NSInteger     g_secret_change = -1;   /* its change count right after that write; -1 = none */

static void secret_clear_if(NSPasteboard *pb, NSInteger change) {
    @autoreleasepool {
        @try {
            if (pb && change >= 0 && pb.changeCount == change) [pb clearContents];
        } @catch (NSException *e) { (void)e; }
        if (g_secret_change == change && g_secret_pb == pb) {
            [g_secret_pb release];
            g_secret_pb = nil;
            g_secret_change = -1;
        }
    }
}

static void secret_atexit(void) { secret_clear_if(g_secret_pb, g_secret_change); }

PlatformClipResult liu_pasteboard_set_secret(NSPasteboard *pb, const char *text, f64 clear_after_sec) {
    if (!text) return PLATFORM_CLIP_BAD_TEXT;
    if (strnlen(text, PLATFORM_CLIPBOARD_MAX_BYTES + 1u) > PLATFORM_CLIPBOARD_MAX_BYTES)
        return PLATFORM_CLIP_TOO_LARGE;
    @autoreleasepool {
        /* stringWithUTF8String returns nil for invalid UTF-8 — refuse before clearContents so
         * the pasteboard keeps what it held. */
        NSString *ns = [NSString stringWithUTF8String:text];
        if (!ns) return PLATFORM_CLIP_BAD_TEXT;
        [pb clearContents];
        if (![pb setString:ns forType:NSPasteboardTypeString]) return PLATFORM_CLIP_FAILED;
        (void)[pb setString:@"concealed" forType:@"org.nspasteboard.ConcealedType"];

        NSInteger mine = pb.changeCount;
        if (g_secret_pb != pb) { [g_secret_pb release]; g_secret_pb = [pb retain]; }
        g_secret_change = mine;
        static bool exit_hooked = false;
        if (!exit_hooked) { exit_hooked = true; atexit(secret_atexit); }
        if (clear_after_sec > 0.0) {
            NSPasteboard *target = [pb retain];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(clear_after_sec * (f64)NSEC_PER_SEC)),
                           dispatch_get_main_queue(), ^{
                secret_clear_if(target, mine);
                [target release];
            });
        }
    }
    return PLATFORM_CLIP_OK;
}

PlatformClipResult platform_clipboard_set_secret(const char *text, f64 clear_after_sec) {
    return liu_pasteboard_set_secret([NSPasteboard generalPasteboard], text, clear_after_sec);
}
