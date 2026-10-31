/*
 * Liu - macOS platform layer (Cocoa + Metal/NSOpenGL)
 */
#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <AVFoundation/AVFoundation.h>   /* AVAudioRecorder + mic permission */
#ifdef USE_METAL
    #import <Metal/Metal.h>
    #import <QuartzCore/CAMetalLayer.h>
#else
    #import <OpenGL/gl3.h>
#endif
#include "platform/platform.h"
#include "renderer/frame_pacing.h"   /* frame_pacing_min_gap */
#include "platform/clipboard_png.h"
#include "platform/open_url_policy.h"
#include "platform/event_queue.h"
#import <CoreVideo/CoreVideo.h>
#include "platform/watch_hold.h"
#include "platform/option_alt.h"
#include "platform/option_layer_macos.h"
#include "ui/layout.h"
#include "core/crashlog.h"

/* The Theme menu is built from the live theme registry. Declared here rather
 * than by including core/config.h: that header typedefs `Style`, which AppKit
 * also defines, and the redefinition breaks the whole translation unit. One
 * prototype is cheaper than untangling that. */
const char **theme_list_names(i32 *count);
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <dispatch/dispatch.h>

static void platform_memory_pressure_start(void);
#include <math.h>           /* fmax/fmin in liu_center_traffic_lights */
#include <stdarg.h>         /* platform_blur_debug_note */

/* =========================================================================
 * Internal state
 * ========================================================================= */

static struct {
    bool initialized;
    f64  time_base;
    mach_timebase_info_data_t timebase_info;
} g_platform = {0};

/* Event queue. The overflow policy — which events may be folded, which must
 * never be dropped, and what a critical event displaces when the queue is full
 * — lives in platform/event_queue.h so both backends share exactly one
 * implementation of it. */
static PlatformEventQueue g_evq;

/* --- Window registry -----------------------------------------------------
 *
 * Every event carries the window it came from. Attributing an event after the
 * fact is impossible, so the source is captured at the only point where AppKit
 * still knows it.
 *
 * For INPUT events that point is -[NSApp sendEvent:]: every key, mouse and
 * scroll event Liu pushes originates inside a view method called from there,
 * and the NSEvent being dispatched names its window. Stashing the resolved
 * PlatformWindow around that call lets push_event() stamp every input site
 * without touching one of them. Delegate callbacks (resize, focus, occlusion)
 * fire outside sendEvent: but hold the NSWindow themselves, so those stamp
 * explicitly — see push_event_for().
 *
 * NULL stays meaningful: menu commands belong to no particular window and the
 * app routes them to whichever one is key. */
#define MAX_PLATFORM_WINDOWS 16
static PlatformWindow *g_windows[MAX_PLATFORM_WINDOWS];
static i32             g_window_count = 0;
static PlatformWindow *g_event_source = NULL;   /* set around sendEvent: */

static NSWindow *platform_window_nswindow(PlatformWindow *w);
/* Defined below the struct; the delegate above it can only reach the window
 * through accessors like this one. */
static void platform_window_mark_link_rebind(PlatformWindow *w);
static void platform_window_display_link_teardown(PlatformWindow *w);
static void link_install_display_observers(void);
/* Defined with the fd-watch machinery further down; the display link's
 * callback needs the same wake-up. */
static void watch_post_wake_event_ex(bool allow_defer);
static void watch_post_wake_event(void);

static PlatformWindow *platform_window_for_ns(NSWindow *ns) {
    if (!ns) return NULL;
    for (i32 i = 0; i < g_window_count; i++) {
        if (g_windows[i] && platform_window_nswindow(g_windows[i]) == ns)
            return g_windows[i];
    }
    return NULL;
}

static void platform_window_register(PlatformWindow *w) {
    if (!w || g_window_count >= MAX_PLATFORM_WINDOWS) return;
    g_windows[g_window_count++] = w;
}

/* True while `w` is still a live, registered window.
 *
 * Animation completion blocks (quake slide, ~0.2 s) routinely outlive the
 * window they were started for — a window can be closed mid-slide — and the
 * block captures a raw `PlatformWindow *`, so touching any field of it there is
 * a use-after-free. Scanning the registry compares POINTERS and never
 * dereferences, so it is safe to call on an already-freed one. */
static bool platform_window_alive(PlatformWindow *w) {
    if (!w) return false;
    for (i32 i = 0; i < g_window_count; i++) {
        if (g_windows[i] == w) return true;
    }
    return false;
}

static void platform_window_unregister(PlatformWindow *w) {
    for (i32 i = 0; i < g_window_count; i++) {
        if (g_windows[i] != w) continue;
        g_windows[i] = g_windows[--g_window_count];
        g_windows[g_window_count] = NULL;
        break;
    }
    /* Any queued event still naming this window would hand the app a dangling
     * pointer after the window is freed. Cheaper to blank them than to make
     * every consumer re-validate. */
    platform_event_queue_forget_window(&g_evq, w);
    if (g_event_source == w) g_event_source = NULL;
}

/* OptionAsAlt: 0 = off, 1 = left, 2 = right, 3 = both. */
static i32 g_option_as_alt = 0;

/* Device-dependent modifier bits (IOKit's NX_DEVICELALTKEYMASK /
 * NX_DEVICERALTKEYMASK). NSEventModifierFlagOption only says "some Option is
 * down"; these say which one, and they are the only way to tell the two
 * physical keys apart from an NSEvent. */
#define LIU_MOD_LEFT_ALT  0x000020UL
#define LIU_MOD_RIGHT_ALT 0x000040UL

/* Should THIS keystroke's Option be Meta rather than a compose modifier?
 * Rules live in option_alt.h so they can be tested without a window server. */
static bool option_side_is_alt(NSEventModifierFlags flags) {
    return option_alt_side_active(g_option_as_alt,
                                  (flags & LIU_MOD_LEFT_ALT)  != 0,
                                  (flags & LIU_MOD_RIGHT_ALT) != 0);
}
static id g_app_delegate = nil;
static void platform_destroy_owned_ui(void);
/* Per window — the toast NSView hangs off PlatformWindow, not off the file. */
static void platform_toast_destroy_for(PlatformWindow *w);

/* Distinct (name, size, colour) symbol rasters held at once. The app draws
 * ~17 icon kinds, several at 2-3 sizes and 2-3 tints, so 16 thrashed the cache
 * and re-rasterized on every frame. 64 comfortably covers the working set. */
#define MAX_SYMBOL_CACHE 64
typedef struct {
    char name[64];
    i32  pixel_size;
    u32  rgba;
    u8  *pixels;
    i32  width;
    i32  height;
    u64  last_use;   /* monotonic tick for LRU eviction */
    bool valid;
} SymbolCacheEntry;
static SymbolCacheEntry g_symbol_cache[MAX_SYMBOL_CACHE];
static u64 g_symbol_use_tick = 0;

static void push_event(PlatformEvent e) {
    if (e.timestamp <= 0) {
        NSEvent *source = [NSApp currentEvent];
        e.timestamp = source && (e.type == EVENT_KEY_DOWN || e.type == EVENT_CHAR_INPUT)
                    ? source.timestamp - g_platform.time_base : platform_time_sec();
    }
    /* Stamp BEFORE the queue sees it: the overflow path folds an event into
     * another of the same type, and folding a scroll from one window into a
     * queued scroll from another would silently scroll the wrong window. */
    if (!e.window) e.window = g_event_source;
    platform_event_queue_push(&g_evq, e);
}

/* Push an event on behalf of a specific NSWindow — for the delegate callbacks,
 * which run outside sendEvent: and so are not covered by g_event_source. */
static void push_event_for(NSWindow *ns, PlatformEvent e) {
    e.window = platform_window_for_ns(ns);
    push_event(e);
}

/* =========================================================================
 * Quake mode state
 *
 * ONLY the tuning lives here. Everything that describes a particular window in
 * quake mode — enabled/visible/animating and the frame/style/level to restore —
 * is per window and lives in PlatformWindow.
 *
 * Shared state cannot describe two windows: one window's enabled flag would
 * stop another from entering quake mode, disabling one would restore the
 * other's saved frame, style mask and level onto it, and a closed quake window
 * would leave `enabled` set, so main.c's EVENT_BLUR → toggle route would slide
 * another window off-screen on the next focus loss.
 * ========================================================================= */

static struct {
    f32      height_ratio;
    f32      anim_duration;
} g_quake = {0};

/* =========================================================================
 * Global hotkey state
 * ========================================================================= */

static struct {
    GlobalHotkeyCallback callback;
    void *userdata;
    u32   key;
    u32   mods;
    id    global_monitor;
    id    local_monitor;
} g_global_hotkey = {0};

/* =========================================================================
 * AppDelegate
 * ========================================================================= */

/* Whether this app is the frontmost one, mirrored off AppKit's activation
 * notifications so it can be read from a thread. `[NSApp isActive]` is
 * main-thread-only, and the notify server, which needs this answer, runs on
 * its own thread (notify_server_start). Written only by the two delegate
 * callbacks below, both of which AppKit fires on the main thread. */
static atomic_bool g_app_frontmost = false;

/* Set by platform_set_quit_guard; asked in applicationShouldTerminate:. */
static bool (*g_quit_guard)(void);

@interface SSHAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation SSHAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    /* LIU_NO_ACTIVATE=1: an instance started for automation or measurement
     * must not take the keyboard or cover the screen. It comes up BEHIND
     * every other window and is never made key (pair it with
     * LIU_FRAME_PROBE_VIS=1, or the occlusion gate skips every paint). */
    if (!liu_dev_env("LIU_NO_ACTIVATE")) [NSApp activateIgnoringOtherApps:YES];
    /* Refresh the registered Services so Finder can expose Liu's declared
     * Services actions in the current session. */
    [NSApp setServicesProvider:self];
    NSUpdateDynamicServices();
    atomic_store_explicit(&g_app_frontmost, [NSApp isActive], memory_order_release);
}
- (void)applicationDidBecomeActive:(NSNotification *)n {
    (void)n;
    atomic_store_explicit(&g_app_frontmost, true, memory_order_release);
}
- (void)applicationDidResignActive:(NSNotification *)n {
    (void)n;
    atomic_store_explicit(&g_app_frontmost, false, memory_order_release);
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}
/* Cmd+Q is `terminate:`, which goes straight to exit(), so the app is asked
 * first (platform_set_quit_guard): a document with unsaved edits must not be
 * lost with the process without a word. */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;
    if (g_quit_guard && !g_quit_guard()) return NSTerminateCancel;
    return NSTerminateNow;
}
/* AppKit's saved window state is an archive in ~/Library/Saved Application
 * State that any process of the user's can write; decoded without secure
 * coding it is a way to get an object of the writer's choosing built inside
 * this process. Liu restores its own sessions and asks AppKit to restore
 * nothing (the windows are not restorable, below), but the opt-in is what
 * makes the decoder refuse. */
- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)app {
    (void)app;
    return YES;
}

/* Menu action handlers — push events to the queue */
- (void)menuNewTab:(id)sender      { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_NEW_TAB}); }
- (void)menuNewWindow:(id)sender   { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_NEW_WINDOW}); }
- (void)menuCloseTab:(id)sender    { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_CLOSE_TAB}); }
- (void)menuSettings:(id)sender    { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_SETTINGS}); }
- (void)menuSecureKeyboard:(id)sender { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_SECURE_KEYBOARD}); }
- (void)menuSSHConnect:(id)sender  { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_SSH_CONNECT}); }
- (void)menuImportSSH:(id)sender   { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_IMPORT_SSH}); }
- (void)menuMoshConnect:(id)sender { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_MOSH_CONNECT}); }
- (void)menuTelnetConnect:(id)sender { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_TELNET_CONNECT}); }
- (void)menuSerialConnect:(id)sender { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_SERIAL_CONNECT}); }
- (void)menuPortForwarding:(id)sender { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_PORT_FORWARDING}); }
- (void)menuSSHKeys:(id)sender { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_SSH_KEYS}); }
- (void)menuKnownHosts:(id)sender { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_KNOWN_HOSTS}); }
- (void)menuToggleSidebar:(id)sender { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_TOGGLE_SIDEBAR}); }
- (void)menuFontBigger:(id)sender  { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_FONT_BIGGER}); }
- (void)menuFontSmaller:(id)sender { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_FONT_SMALLER}); }
- (void)menuFontReset:(id)sender   { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_FONT_RESET}); }
- (void)menuFind:(id)sender        { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_FIND}); }
/* Finder Services provider — the two entries declared under NSServices in
 * Info.plist. macOS delivers the right-clicked items on a pasteboard; a folder
 * is used as-is and a file contributes its parent directory, which is what
 * "open a terminal HERE" means for both.
 *
 * Registered on NSApp below, so a running Liu handles the service in place; if
 * none is running, launchd starts one and AppKit replays the message once the
 * provider is set. */
- (void)liuServiceOpenFolder:(NSPasteboard *)pb newWindow:(BOOL)newWindow {
    NSArray<NSURL *> *urls =
        [pb readObjectsForClasses:@[[NSURL class]]
                          options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    if (urls.count == 0) return;

    /* Ring buffer for the same reason EVENT_DROP_FILE keeps one: the event
     * queue outlives this autorelease pool, so the C string has to. */
    static char svc_paths[8][1024];
    static i32  svc_idx = 0;

    for (NSURL *url in urls) {
        NSNumber *isDir = nil;
        [url getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:NULL];
        NSString *dir = [isDir boolValue] ? [url path]
                                          : [[url URLByDeletingLastPathComponent] path];
        const char *path = [dir UTF8String];
        if (!path || !*path) continue;
        /* A path the ring cannot hold is refused, not cut: a cut path is
         * another folder. */
        if (strlen(path) >= sizeof(svc_paths[0])) continue;
        i32 slot = svc_idx++ & 7;
        snprintf(svc_paths[slot], sizeof(svc_paths[slot]), "%s", path);
        push_event((PlatformEvent){
            .type = EVENT_MENU_OPEN_PATH,
            .open_path = { .path = svc_paths[slot], .new_window = (bool)newWindow }
        });
        /* One terminal per invocation. Selecting a dozen folders and getting a
         * dozen windows is a footgun, and Finder's own "New Terminal at Folder"
         * behaves the same way. */
        break;
    }
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)openLiuWindowHere:(NSPasteboard *)pb
                 userData:(NSString *)userData
                    error:(NSString **)error {
    (void)userData; (void)error;
    [self liuServiceOpenFolder:pb newWindow:YES];
}

- (void)openLiuTabHere:(NSPasteboard *)pb
              userData:(NSString *)userData
                 error:(NSString **)error {
    (void)userData; (void)error;
    [self liuServiceOpenFolder:pb newWindow:NO];
}

- (void)menuSelectTheme:(NSMenuItem *)sender {
    static char theme_buf[64];
    snprintf(theme_buf, sizeof(theme_buf), "%s", [[sender representedObject] UTF8String]);
    push_event((PlatformEvent){.type = EVENT_MENU_THEME, .theme.name = theme_buf});
}
@end

/* =========================================================================
 * Live-resize render callback
 *
 * Set by main.c. Fired from the live-resize NSTimer at ~60 Hz so the window
 * keeps repainting while AppKit owns the runloop in event-tracking mode and
 * the usual main loop is starved.
 *
 * The REGISTRATION is global (one renderer entry point) but the DISPATCH is
 * per window: the timer belongs to one view, and a window-less callback would
 * repaint whatever window main.c happened to hold, leaving the window whose
 * edge is dragged stale for the whole drag.
 * ========================================================================= */
static PlatformRenderCallback g_render_cb = NULL;
static void                  *g_render_user = NULL;
static PlatformTitlebarZoomQuery g_zoom_query = NULL;
static void                     *g_zoom_user  = NULL;

/* Fire the live-resize hook for the window `view` is currently in.
 *
 * Bails when the view has no window, or the window is not (or no longer) in the
 * registry: an NSTimer added to the runloop retains its target, so a view whose
 * window is being torn down can still tick, and handing main.c a freed
 * PlatformWindow there would crash. */
static void liu_fire_render_cb(NSView *view) {
    if (!g_render_cb || !view) return;
    PlatformWindow *w = platform_window_for_ns([view window]);
    if (!w) return;
    g_render_cb(w, g_render_user);
}

/* =========================================================================
 * Window delegate
 * ========================================================================= */

@interface SSHWindowDelegate : NSObject <NSWindowDelegate>
@property (assign) bool shouldClose;
@end

/* Vertically center the macOS traffic-light buttons within the *painted* tab
 * strip (TOOLBAR_HEIGHT_PT), not AppKit's shorter native titlebar container —
 * otherwise the lights sit above the tab pills. The buttons live in AppKit's
 * titlebar superview, whose top edge coincides with the window top, so a
 * button's vertical center sits (superH - origin.y - btnH/2) points below the
 * top. Solving for the strip's center (stripH/2 from the top) gives the
 * origin.y below. Everything here is in points (AppKit frames are unscaled),
 * matching TOOLBAR_HEIGHT_PT. */
/* Painted tab-strip height in points (AppKit units), kept in sync with the UI
 * by platform_window_set_titlebar_height so the lights track the real strip —
 * which is TOOLBAR_HEIGHT_PT normally, but a shorter macOS drag strip (~30)
 * when the tab bar and toolbar icons are both hidden.
 *
 * It lives in PlatformWindow, NOT in a file-static: two windows with different
 * chrome settings overwrote each other on every layout pass, so the non-key
 * window's lights ended up centred on the OTHER window's strip. The accessors
 * are defined after `struct PlatformWindow` and forward-declared here because
 * the chrome helpers below run above it. `0` (calloc) means "never told",
 * which falls back to the common case. */
static void liu_layout_window_chrome(NSWindow *win);
static CGFloat liu_strip_pt_for_window(PlatformWindow *w);
static CGFloat liu_strip_pt_for_ns(NSWindow *ns);
static void    liu_set_strip_pt_for_window(PlatformWindow *w, CGFloat pt);

/* Window corner radius in points (the macOS 11+ frame radius). The blur layer
 * is clipped to it, so it has to match what AppKit draws or the two disagree
 * by a pixel around the arc. */
#define LIU_CORNER_RADIUS 10.0

/* Keep the corner mask and the blur layer sized to the window. Called on every
 * resize, which is also how fullscreen transitions arrive. */
static void liu_layout_window_chrome(NSWindow *win) {
    if (!win) return;
    NSView *cv = [win contentView];
    if (!cv || !cv.layer) return;
    bool fs = ([win styleMask] & NSWindowStyleMaskFullScreen) != 0;
    cv.wantsLayer = YES;
    /* Actions off: this runs from windowDidResize, where CoreAnimation's
     * default implicit animation would ease the corner radius and the blur
     * layer's frame toward the new bounds over a quarter second — the blur
     * visibly trailing the window edge through a live resize. */
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    /* EXPERIMENT (LIU_CORNER_MASK=0): cornerRadius + masksToBounds on a layer
     * with sublayers forces CoreAnimation to render the subtree offscreen and
     * mask it. Measuring what that costs against a translucent window. */
    static int s_corner_mask = -1;
    if (s_corner_mask < 0) {
        const char *e = getenv("LIU_CORNER_MASK");
        s_corner_mask = (e && e[0] == '0') ? 0 : 1;
    }
    cv.layer.cornerRadius  = (fs || !s_corner_mask) ? 0.0 : LIU_CORNER_RADIUS;
    cv.layer.masksToBounds = s_corner_mask ? YES : NO;
    /* The backdrop is always sublayer 0 (inserted below the Metal layer). */
    if (cv.layer.sublayers.count > 0) {
        CALayer *first = cv.layer.sublayers[0];
        Class bd = NSClassFromString(@"CABackdropLayer");
        if (bd && [first isKindOfClass:bd]) first.frame = cv.bounds;
    }
    [CATransaction commit];
}

static void liu_center_traffic_lights(NSWindow *win) {
    if (!win) return;
    /* In native fullscreen the buttons live in AppKit's auto-hiding overlay;
     * leave their placement to the system. */
    if (win.styleMask & NSWindowStyleMaskFullScreen) return;
    NSButton *close = [win standardWindowButton:NSWindowCloseButton];
    NSButton *mini  = [win standardWindowButton:NSWindowMiniaturizeButton];
    NSButton *zoom  = [win standardWindowButton:NSWindowZoomButton];
    if (!close || !close.superview) return;
    CGFloat superH = close.superview.bounds.size.height;
    CGFloat btnH   = close.frame.size.height;
    /* This window's own strip — the buttons being moved belong to `win`. */
    CGFloat stripH = liu_strip_pt_for_ns(win);
    CGFloat y      = superH - btnH * 0.5 - stripH * 0.5;
    /* Clamp inside the container; fmin first so a (pathologically) tiny
     * container still floors at 0 rather than going negative. */
    y = fmax(0.0, fmin(y, superH - btnH));
    NSButton *btns[3] = { close, mini, zoom };
    for (int i = 0; i < 3; i++) {
        if (!btns[i]) continue;
        NSRect f = btns[i].frame;
        f.origin.y = y;
        btns[i].frame = f;
    }
}

void platform_window_set_titlebar_height(PlatformWindow *w, f32 points) {
    if (!w || points <= 0.0f) return;
    CGFloat v = (CGFloat)points;
    if (v == liu_strip_pt_for_window(w)) return;
    liu_set_strip_pt_for_window(w, v);
    /* Re-center for the new strip height immediately (e.g. the user toggled the
     * tab bar off) — on THIS window's buttons, not [NSApp mainWindow]'s, which
     * is a different window whenever a background window re-lays out. */
    liu_center_traffic_lights(platform_window_nswindow(w));
}

/* Deprecated window-less bridge — see platform.h. Aims at the key window so the
 * single-window behaviour is unchanged; delete once src/ui/ui.c passes its
 * host_window. */
void platform_set_titlebar_height(f32 points) {
    PlatformWindow *w = platform_key_window();
    if (!w && g_window_count > 0) w = g_windows[0];
    platform_window_set_titlebar_height(w, points);
}

/* Bind a CABackdropLayer to the window server's behind-window capture, the way
 * AppKit binds NSVisualEffectView's own backdrop.
 *
 * A freshly allocated CABackdropLayer has `groupName = nil`,
 * `groupNamespace = "owningContext"` and `windowServerAware = NO`. It still
 * blurs — CoreAnimation attaches it to a default group — but that attachment is
 * implicit and tied to the window's activation state: it is torn down and
 * re-established as the window loses and regains key, and the blur visibly
 * drops out for those frames. Nothing in Liu's own layer state changes when
 * that happens, so no diagnostic that watches it can see it.
 *
 * The values are read off a live NSVisualEffectView:
 *
 *     groupName        NSCGSWindowBehindWindowCaptureBackdropGroup
 *     groupNamespace   hostingNamespacedContext
 *     windowServerAware YES
 *
 * Naming the group explicitly is what makes the capture a registered,
 * persistent behind-window feed rather than an inferred one.
 *
 * NOT copied from AppKit: `scale = 0.125` (it downsamples the capture 8x before
 * blurring, which changes what a given gaussian radius looks like — a separate
 * decision, and a performance one), and the `sdrNormalize` / `colorSaturate`
 * filters, which are the MATERIAL. The material is exactly what Liu does not
 * want; see platform_window_set_blur.
 *
 * Every key here is private. Each is probed with respondsToSelector: before
 * use, so a macOS that renames one degrades to the implicit binding instead of
 * raising NSUndefinedKeyException — which, with Liu's uncaught handler, would
 * be fatal. */
static void liu_backdrop_bind_capture(CALayer *bd) {
    if (!bd) return;
    if ([bd respondsToSelector:@selector(setGroupName:)])
        [bd setValue:@"NSCGSWindowBehindWindowCaptureBackdropGroup"
              forKey:@"groupName"];
    if ([bd respondsToSelector:@selector(setGroupNamespace:)])
        [bd setValue:@"hostingNamespacedContext" forKey:@"groupNamespace"];
    if ([bd respondsToSelector:@selector(setWindowServerAware:)])
        [bd setValue:@YES forKey:@"windowServerAware"];
}

/* The one window the blur probe samples (the most recently created). The
 * delegate callbacks below get an NSWindow, not the PlatformWindow, and the
 * probe needs the backdrop layer and the Metal layer that hang off
 * PlatformWindow. */
static PlatformWindow *g_blur_probe_window = NULL;
static void blur_probe(const char *tag) {
    if (g_blur_probe_window) platform_blur_debug_sample(g_blur_probe_window, tag);
}

@implementation SSHWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    self.shouldClose = YES;
    PlatformEvent e = { .type = EVENT_CLOSE };
    push_event_for(sender, e);
    return NO; /* Liu handles closing itself */
}
- (void)windowDidResize:(NSNotification *)notification {
    NSWindow *win = notification.object;
    liu_layout_window_chrome(win);
    NSRect frame = [[win contentView] frame];
    PlatformEvent e = {
        .type = EVENT_RESIZE,
        .resize = { .width = (i32)frame.size.width, .height = (i32)frame.size.height }
    };
    push_event_for(win, e);
    liu_center_traffic_lights(win);
    blur_probe("resize");
}
- (void)windowDidBecomeKey:(NSNotification *)n {
    liu_center_traffic_lights(n.object);
    push_event_for(n.object, (PlatformEvent){ .type = EVENT_FOCUS, .focus.focused = true });
    blur_probe("becomeKey");
}
- (void)windowDidChangeOcclusionState:(NSNotification *)n {
    /* The render path skips frames while the window is covered; without this
     * wake-up the loop could sit in platform_wait_events for up to a second
     * after the window is revealed, showing stale pixels. */
    push_event_for(n.object, (PlatformEvent){ .type = EVENT_VISIBILITY });
    blur_probe("occlusion");
}
- (void)windowDidResignKey:(NSNotification *)n {
    push_event_for(n.object, (PlatformEvent){ .type = EVENT_BLUR, .focus.focused = false });
    blur_probe("resignKey");
}
/* Probe-only hooks. AppKit re-composites the window for each of these, and any
 * of them could be what disturbs the backdrop; the log records which one fired
 * next to a blink. */
- (void)windowDidMove:(NSNotification *)n            { (void)n; blur_probe("move"); }
- (void)windowDidChangeScreen:(NSNotification *)n {
    /* The window moved to another display, so the display link is now paced by
     * the wrong panel — a 120 Hz link driving a 60 Hz screen produces two
     * wake-ups per vblank, and the reverse starves it. Re-bind on the next
     * sync with the display the window is currently on. */
    platform_window_mark_link_rebind(platform_window_for_ns(n.object));
    blur_probe("screen");
}
- (void)windowWillEnterFullScreen:(NSNotification *)n{ (void)n; blur_probe("fsEnter"); }
- (void)windowDidEnterFullScreen:(NSNotification *)n { (void)n; blur_probe("fsEntered"); }
- (void)windowWillExitFullScreen:(NSNotification *)n { (void)n; blur_probe("fsExit"); }
- (void)windowDidExitFullScreen:(NSNotification *)n  { (void)n; blur_probe("fsExited"); }
- (void)windowDidMiniaturize:(NSNotification *)n     { (void)n; blur_probe("mini"); }
- (void)windowDidDeminiaturize:(NSNotification *)n   { (void)n; blur_probe("demini"); }
- (void)windowDidExpose:(NSNotification *)n          { (void)n; blur_probe("expose"); }
- (void)windowDidChangeBackingProperties:(NSNotification *)n {
    NSWindow *win = n.object;
    CGFloat newScale = [win backingScaleFactor];
#ifdef USE_METAL
    /* AppKit fires this when the window's backing scale changes (Retina ↔
     * 1× external display, a system display-resolution change that flips the
     * HiDPI factor, …) but does NOT also re-fire setFrameSize: when the frame
     * in points is unchanged. The Metal layer's contentsScale and drawableSize
     * were sized to the OLD scale by the previous setFrameSize:, so without
     * refreshing them here the renderer keeps drawing into a buffer of the
     * old pixel count while the font atlas (rebuilt at the new dpi_scale by
     * main.c's EVENT_DPI_CHANGE handler) and every other UI dimension move to
     * the new scale — glyphs come out garbled / wrong-size. Update the layer
     * eagerly on the AppKit thread before the event is dispatched so the next
     * nextDrawable call sees the new pixel size. */
    NSView *cv = [win contentView];
    if (cv && [cv.layer isKindOfClass:[CAMetalLayer class]]) {
        CAMetalLayer *layer = (CAMetalLayer *)cv.layer;
        layer.contentsScale = newScale;
        layer.drawableSize = [cv convertSizeToBacking:cv.bounds.size];
    }
#endif
    push_event_for(win, (PlatformEvent){
        .type = EVENT_DPI_CHANGE,
        .dpi = { .scale = (f32)newScale }
    });
    blur_probe("backing");
}
@end

/* =========================================================================
 * Custom NSView for input
 * ========================================================================= */

/* Owns one remote promise context after the drag session starts. AppKit may
 * discard an NSFilePromiseProvider without ever requesting its bytes (cancelled
 * drag, rejected destination), so tying cleanup to the provider's userInfo
 * object prevents both leaked Session references and late UAFs. */
@interface LiuFilePromiseBox : NSObject {
@private
    void *_ctx;
    bool (*_provider)(void *, const char *);
    void (*_cleanup)(void *);
}
- (id)initWithContext:(void *)ctx
             provider:(bool (*)(void *, const char *))provider
              cleanup:(void (*)(void *))cleanup;
- (BOOL)writeToPath:(const char *)path;
@end

@implementation LiuFilePromiseBox
- (id)initWithContext:(void *)ctx
             provider:(bool (*)(void *, const char *))provider
              cleanup:(void (*)(void *))cleanup {
    self = [super init];
    if (self) {
        _ctx = ctx;
        _provider = provider;
        _cleanup = cleanup;
    }
    return self;
}

- (BOOL)writeToPath:(const char *)path {
    void *ctx = NULL;
    bool (*provider)(void *, const char *) = NULL;
    void (*cleanup)(void *) = NULL;
    @synchronized (self) {
        if (!_ctx) return NO;
        ctx = _ctx;
        provider = _provider;
        cleanup = _cleanup;
        _ctx = NULL;
    }
    bool ok = provider && provider(ctx, path);
    if (cleanup) cleanup(ctx);
    return ok;
}

- (void)dealloc {
    void *ctx = NULL;
    void (*cleanup)(void *) = NULL;
    @synchronized (self) {
        ctx = _ctx;
        cleanup = _cleanup;
        _ctx = NULL;
    }
    if (ctx && cleanup) cleanup(ctx);
    [super dealloc];
}
@end

#ifdef USE_METAL
@interface SSHView : NSView <NSTextInputClient, NSDraggingDestination, NSDraggingSource, NSFilePromiseProviderDelegate> {
@public
    NSEvent *_lastMouseEvent;           /* most recent mouse event, used to
                                         * seed beginDraggingSessionWithItems */
    NSOperationQueue *_promiseQueue;    /* serial queue for SFTP downloads */
    NSTimer *_liveResizeTimer;          /* drives redraw during window-edge drag */
    NSEventModifierFlags _lastModifierFlags;
}
@end

@implementation SSHView

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return YES; }

/* The window uses NSWindowStyleMaskFullSizeContentView with a transparent
 * titlebar so Liu's tab bar paints into the title-bar strip. AppKit's
 * default treats that strip as window-drag, so dragging a tab would move
 * the window instead. Returning NO from this override makes the entire
 * content view non-draggable; window dragging still works on the legitimate
 * empty areas of the title bar (right of the resource monitor) because
 * AppKit falls back to its own title-bar machinery there. */
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) {
        [self registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    }
    if (!_promiseQueue) {
        _promiseQueue = [[NSOperationQueue alloc] init];
        _promiseQueue.maxConcurrentOperationCount = 1;
    }
}

- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer {
    CAMetalLayer *layer = [CAMetalLayer layer];
    /* Top-left gravity prevents stretching old contents during resize */
    layer.contentsGravity = kCAGravityTopLeft;
    /* Auto-resize layer with view */
    layer.needsDisplayOnBoundsChange = YES;
    return layer;
}
- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    CAMetalLayer *layer = (CAMetalLayer *)self.layer;
    layer.contentsScale = self.window.backingScaleFactor;
    /* Update both bounds AND drawable size atomically */
    CGSize backing = [self convertSizeToBacking:self.bounds.size];
    layer.drawableSize = backing;
    /* AppKit resizes views outside -[NSApp sendEvent:] (autoresize on a window
     * frame change, live-resize tracking), so g_event_source is NULL here and
     * an unstamped resize would be applied to whichever window happens to be
     * key. Name the source explicitly. */
    push_event_for(self.window, (PlatformEvent){
        .type = EVENT_RESIZE,
        .resize = { .width = (i32)newSize.width, .height = (i32)newSize.height }
    });
}

/* Live resize: ensure clean rendering during drag.
 *
 * AppKit pumps the runloop in NSEventTrackingRunLoopMode while the user is
 * dragging the window edge, so the main loop (which renders) doesn't get
 * to run until the drag ends, and the newly-exposed pixels in the larger
 * drawable would otherwise show as black. An NSTimer registered on the
 * common runloop modes calls the platform render callback every frame
 * while live-resize is active. */
- (void)viewWillStartLiveResize {
    [super viewWillStartLiveResize];
    CAMetalLayer *layer = (CAMetalLayer *)self.layer;
    /* Synchronized present so the new drawable is committed to the screen
     * inside the same CA transaction AppKit uses to grow the window — this
     * avoids a one-frame mismatch between layer size and rendered content. */
    layer.presentsWithTransaction = YES;

    if (!_liveResizeTimer) {
        _liveResizeTimer = [NSTimer timerWithTimeInterval:1.0 / 60.0
                                                   target:self
                                                 selector:@selector(liveResizeTick:)
                                                 userInfo:nil
                                                  repeats:YES];
        /* common modes: fires in both default + event-tracking, so the
         * timer keeps ticking while the user is mid-drag. */
        [[NSRunLoop currentRunLoop] addTimer:_liveResizeTimer forMode:NSRunLoopCommonModes];
    }
}

- (void)viewDidEndLiveResize {
    [super viewDidEndLiveResize];
    CAMetalLayer *layer = (CAMetalLayer *)self.layer;
    layer.presentsWithTransaction = NO;

    if (_liveResizeTimer) {
        [_liveResizeTimer invalidate];
        _liveResizeTimer = nil;
    }
    /* Final tick guarantees the post-resize frame matches the final size. */
    liu_fire_render_cb(self);
}

- (void)liveResizeTick:(NSTimer *)t {
    (void)t;
    /* Render THIS view's window — the timer belongs to one view, and a
     * window-less callback repainted whichever window main.c held. */
    liu_fire_render_cb(self);
}
#else
@interface SSHView : NSOpenGLView <NSTextInputClient, NSDraggingDestination, NSDraggingSource, NSFilePromiseProviderDelegate> {
@public
    NSEvent *_lastMouseEvent;           /* shared mouse-drag seed */
    NSOperationQueue *_promiseQueue;    /* serial queue for file promises */
    NSTimer *_liveResizeTimer;
    NSEventModifierFlags _lastModifierFlags;
}
@end

@implementation SSHView

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) {
        [self registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    }
    if (!_promiseQueue) {
        _promiseQueue = [[NSOperationQueue alloc] init];
        _promiseQueue.maxConcurrentOperationCount = 1;
    }
}

/* Live resize: update OpenGL viewport immediately during drag */
- (void)reshape {
    [super reshape];
    [[self openGLContext] makeCurrentContext];
    [[self openGLContext] update];
    /* Push resize event so main loop re-renders with correct size. Stamped:
     * -reshape runs outside sendEvent:, so g_event_source is NULL here. */
    NSRect frame = [self bounds];
    push_event_for(self.window, (PlatformEvent){
        .type = EVENT_RESIZE,
        .resize = { .width = (i32)frame.size.width, .height = (i32)frame.size.height }
    });
}

- (void)update {
    [super update];
    [[self openGLContext] update];
}

/* See the SSHView (Metal) variant for the rationale; the same on the OpenGL
 * path: drive an NSTimer in common runloop modes so rendering continues
 * while the user is dragging the window edge. */
- (void)viewWillStartLiveResize {
    [super viewWillStartLiveResize];
    if (!_liveResizeTimer) {
        _liveResizeTimer = [NSTimer timerWithTimeInterval:1.0 / 60.0
                                                   target:self
                                                 selector:@selector(liveResizeTick:)
                                                 userInfo:nil
                                                  repeats:YES];
        [[NSRunLoop currentRunLoop] addTimer:_liveResizeTimer forMode:NSRunLoopCommonModes];
    }
}

- (void)viewDidEndLiveResize {
    [super viewDidEndLiveResize];
    if (_liveResizeTimer) {
        [_liveResizeTimer invalidate];
        _liveResizeTimer = nil;
    }
    liu_fire_render_cb(self);
}

- (void)liveResizeTick:(NSTimer *)t {
    (void)t;
    /* See the Metal variant: render this view's own window. */
    liu_fire_render_cb(self);
}
#endif

/* Map macOS keycode → our KeyCode */
static KeyCode map_key(unsigned short kc) {
    switch (kc) {
        /* Modifiers */
        case 0x38: return KEY_LSHIFT;
        case 0x3C: return KEY_RSHIFT;
        case 0x3B: return KEY_LCTRL;
        case 0x3E: return KEY_RCTRL;
        case 0x3A: return KEY_LALT;
        case 0x3D: return KEY_RALT;
        case 0x37: return KEY_LSUPER;
        case 0x36: return KEY_RSUPER;
        /* Special keys */
        case 0x24: return KEY_ENTER;
        case 0x30: return KEY_TAB;
        case 0x33: return KEY_BACKSPACE;
        case 0x35: return KEY_ESCAPE;
        case 0x75: return KEY_DELETE;
        case 0x31: return KEY_SPACE;
        /* Navigation */
        case 0x7E: return KEY_UP;
        case 0x7D: return KEY_DOWN;
        case 0x7B: return KEY_LEFT;
        case 0x7C: return KEY_RIGHT;
        case 0x73: return KEY_HOME;
        case 0x77: return KEY_END;
        case 0x74: return KEY_PAGE_UP;
        case 0x79: return KEY_PAGE_DOWN;
        /* Function keys */
        case 0x7A: return KEY_F1;  case 0x78: return KEY_F2;
        case 0x63: return KEY_F3;  case 0x76: return KEY_F4;
        case 0x60: return KEY_F5;  case 0x61: return KEY_F6;
        case 0x62: return KEY_F7;  case 0x64: return KEY_F8;
        case 0x65: return KEY_F9;  case 0x6D: return KEY_F10;
        case 0x67: return KEY_F11; case 0x6F: return KEY_F12;
        /* Letters (QWERTY layout) */
        case 0x00: return KEY_A;  case 0x0B: return KEY_B;
        case 0x08: return KEY_C;  case 0x02: return KEY_D;
        case 0x0E: return KEY_E;  case 0x03: return KEY_F;
        case 0x05: return KEY_G;  case 0x04: return KEY_H;
        case 0x22: return KEY_I;  case 0x26: return KEY_J;
        case 0x28: return KEY_K;  case 0x25: return KEY_L;
        case 0x2E: return KEY_M;  case 0x2D: return KEY_N;
        case 0x1F: return KEY_O;  case 0x23: return KEY_P;
        case 0x0C: return KEY_Q;  case 0x0F: return KEY_R;
        case 0x01: return KEY_S;  case 0x11: return KEY_T;
        case 0x20: return KEY_U;  case 0x09: return KEY_V;
        case 0x0D: return KEY_W;  case 0x07: return KEY_X;
        case 0x10: return KEY_Y;  case 0x06: return KEY_Z;
        /* Digits */
        case 0x1D: return KEY_0;  case 0x12: return KEY_1;
        case 0x13: return KEY_2;  case 0x14: return KEY_3;
        case 0x15: return KEY_4;  case 0x17: return KEY_5;
        case 0x16: return KEY_6;  case 0x1A: return KEY_7;
        case 0x1C: return KEY_8;  case 0x19: return KEY_9;
        /* Punctuation used by chrome shortcuts (Cmd+[ / Cmd+] / Cmd+.). */
        case 0x21: return KEY_LEFT_BRACKET;
        case 0x1E: return KEY_RIGHT_BRACKET;
        case 0x2F: return KEY_PERIOD;
        case 0x2B: return KEY_COMMA;
        case 0x2C: return KEY_SLASH;          /* kVK_ANSI_Slash */
        default: return KEY_UNKNOWN;
    }
}

static u32 get_mods(NSEventModifierFlags flags) {
    u32 m = MOD_NONE;
    if (flags & NSEventModifierFlagShift)   m |= MOD_SHIFT;
    if (flags & NSEventModifierFlagControl) m |= MOD_CTRL;
    if (flags & NSEventModifierFlagOption)  m |= MOD_ALT;
    if (flags & NSEventModifierFlagCommand) m |= MOD_SUPER;
    return m;
}

static u32 mod_mask_for_key(KeyCode key) {
    switch (key) {
        case KEY_LSHIFT: case KEY_RSHIFT: return MOD_SHIFT;
        case KEY_LCTRL:  case KEY_RCTRL:  return MOD_CTRL;
        case KEY_LALT:   case KEY_RALT:   return MOD_ALT;
        case KEY_LSUPER: case KEY_RSUPER: return MOD_SUPER;
        default: return 0;
    }
}

static bool should_route_cmd_shortcut(KeyCode kc, u32 mods) {
    if (!(mods & MOD_SUPER) || kc == KEY_UNKNOWN) return false;

    switch (kc) {
        case KEY_A:
        case KEY_B:
        case KEY_C:
        case KEY_D:
        case KEY_F:
        case KEY_G:
        case KEY_I:
        case KEY_K:
        case KEY_S:
        case KEY_T:
        case KEY_V:
        case KEY_W:
        /* Text-editing shortcuts liu resolves against the shell's prompt line
         * (cut, undo, start/end of line) — routed here so no responder in the
         * chain gets a chance to claim them first. */
        case KEY_X:
        case KEY_Z:
        case KEY_LEFT:
        case KEY_RIGHT:
        case KEY_UP:
        case KEY_DOWN:
        case KEY_0:
        case KEY_1:
        case KEY_2:
        case KEY_3:
        case KEY_4:
        case KEY_5:
        case KEY_6:
        case KEY_7:
        case KEY_8:
        case KEY_9:
            return true;
        default:
            return false;
    }
}

- (BOOL)performKeyEquivalent:(NSEvent *)event {
    if (event.type == NSEventTypeKeyDown) {
        KeyCode kc = map_key(event.keyCode);
        u32 mods = get_mods(event.modifierFlags);
        if (should_route_cmd_shortcut(kc, mods)) {
            push_event((PlatformEvent){
                .type = EVENT_KEY_DOWN,
                .key = { .key = kc, .mods = mods, .repeat = event.isARepeat }
            });
            return YES;
        }
    }
    return [super performKeyEquivalent:event];
}

- (void)keyDown:(NSEvent *)event {
    KeyCode kc = map_key(event.keyCode);
    u32 mods = get_mods(event.modifierFlags);

    /*
     * Ctrl+key: macOS text input system doesn't produce insertText for control chars.
     * CHAR_INPUT is emitted directly for Ctrl+A..Z so the terminal gets ^A..^Z. Do
     * this before KEY_DOWN so terminal_key_input doesn't also emit a CSI-u sequence
     * for the same physical key.
     */
    if ((mods & MOD_CTRL) && !(mods & MOD_SUPER)) {
        NSString *chars = [event charactersIgnoringModifiers];
        if (chars.length > 0) {
            unichar ch = [chars characterAtIndex:0];
            if (ch >= 'a' && ch <= 'z') {
                push_event((PlatformEvent){
                    .type = EVENT_CHAR_INPUT,
                    .char_input = { .codepoint = (u32)ch, .mods = mods }
                });
                return; /* Don't pass to text input system */
            }
        }
    }

    /* Treat an Option composition as text when it produces printable ASCII or
     * a letter the active layout cannot type without Option. The text input
     * system already delivers the composed character, so suppress the matching
     * key event to avoid duplicate input. Empty compositions remain Alt key
     * events, preserving dead-key composition and Alt-based word motions. */
    if ((mods & MOD_ALT) && !(mods & MOD_SUPER)) {
        NSString *composed = [event characters];
        NSString *bare     = [event charactersIgnoringModifiers];
        unichar c0 = composed.length > 0 ? [composed characterAtIndex:0] : 0;
        bool dead_key = (composed.length == 0);
        bool composed_char = (composed.length > 0 && bare.length > 0 &&
                              c0 < 0xF700 && ![composed isEqualToString:bare]);
        bool alt_side = option_side_is_alt(event.modifierFlags);
        /* Only asked for ASCII letters, and only on the Alt side — the layout
         * scan is skipped for every other chord. */
        bool letter_needs_option =
            alt_side && composed_char && option_layer_needs_option((u32)c0);
        if (option_alt_text_wins(alt_side, dead_key, composed_char, (u32)c0,
                                 letter_needs_option)) {
            [self interpretKeyEvents:@[event]];
            return;
        }
    }

    push_event((PlatformEvent){
        .type = EVENT_KEY_DOWN,
        .key = { .key = kc, .mods = mods, .repeat = event.isARepeat }
    });

    /* Cmd shortcuts — don't pass to text input, handle in main loop */
    if (mods & MOD_SUPER) {
        return;
    }

    /* Option-as-Alt: when enabled, Option+key sends the unmodified character
     * with ALT modifier instead of macOS special characters (like ∫ for Opt+B).
     * This lets the terminal receive ESC+key (Alt+key). */
    if ((mods & MOD_ALT) && option_side_is_alt(event.modifierFlags)) {
        NSString *chars = [event charactersIgnoringModifiers];
        if (chars.length > 0) {
            unichar ch = [chars characterAtIndex:0];
            /* Skip macOS function key codepoints (arrows, F-keys, etc. 0xF700+).
             * These are handled via KEY_DOWN event, not CHAR_INPUT. */
            if (ch < 0xF700) {
                push_event((PlatformEvent){
                    .type = EVENT_CHAR_INPUT,
                    .char_input = { .codepoint = (u32)ch, .mods = mods }
                });
                return;
            }
        }
    }

    /* Regular keys — let the input manager produce insertText */
    [self interpretKeyEvents:@[event]];
}

- (void)keyUp:(NSEvent *)event {
    KeyCode kc = map_key(event.keyCode);
    push_event((PlatformEvent){
        .type = EVENT_KEY_UP,
        .key = { .key = kc, .mods = get_mods(event.modifierFlags), .repeat = false }
    });
}

/*
 * Mouse coordinate helper: convert window-space point to framebuffer pixel
 * with Y flipped (top=0 in the renderer's coordinate system). Uses
 * convertPointToBacking for correct Retina scaling.
 */
- (void)pushMouseEvent:(EventType)type button:(i32)btn event:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    /* Convert point coordinates to backing (framebuffer) pixels */
    NSPoint bp = [self convertPointToBacking:p];
    NSRect fb = [self convertRectToBacking:self.bounds];
    i32 mx = (i32)bp.x;
    i32 my = (i32)(fb.size.height - bp.y); /* flip Y: bottom-up → top-down */
    /* The pointer on the SCREEN, taken now, against the frame this event's
     * window-local point was made for. A tab dragged as its own window moves the
     * window these events arrive in; by the time main.c gets to one, x/y are
     * relative to a frame that no longer exists (several events are drained per
     * loop turn, and the first one already moved the window). */
    bool has_screen = false;
    f32 scr_x = 0.0f, scr_y = 0.0f;
    NSWindow *ew = event.window ? event.window : self.window;
    NSScreen *main_screen = [[NSScreen screens] firstObject];
    if (ew && main_screen) {
        NSPoint sp = [ew convertPointToScreen:event.locationInWindow];
        NSRect  sf = [main_screen frame];
        scr_x = (f32)sp.x;
        scr_y = (f32)(sf.origin.y + sf.size.height - sp.y);
        has_screen = true;
    }
    push_event((PlatformEvent){
        .type = type,
        .mouse = { .button = btn, .x = mx, .y = my, .mods = get_mods(event.modifierFlags),
                   .has_screen = has_screen, .sx = scr_x, .sy = scr_y }
    });
}

- (void)mouseDown:(NSEvent *)event {
    /* Save the event so platform_begin_file_drag can seed a drag session
     * from a later callback without waiting for the next mouseDragged.
     *
     * ARC is not enabled in this target: the ivar is a raw pointer, so it is
     * retained/released manually. Cocoa autoreleases the dispatched NSEvent
     * as soon as the mouse handler returns; without the retain the pointer
     * dangles into dead memory and any subsequent objc_msgSend (e.g. [seed
     * locationInWindow] in platform_begin_file_drag) crashes. */
    [event retain];
    [_lastMouseEvent release];
    _lastMouseEvent = event;
    /* Always send click event — hit testing in main.c decides what to do */
    [self pushMouseEvent:EVENT_MOUSE_DOWN button:0 event:event];

    /* Double-click on the title bar = zoom (macOS convention) — but ONLY on an
     * empty, non-interactive part of the strip. Without the zoom-query guard a
     * fast double-click on a tab's × (close) would zoom the window by accident. */
    if (event.clickCount == 2) {
        NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
        NSPoint bp = [self convertPointToBacking:p];
        NSRect fb = [self convertRectToBacking:self.bounds];
        f32 fb_x = (f32)bp.x;
        f32 fb_y = (f32)(fb.size.height - bp.y);
        if (fb_y < TOOLBAR_HEIGHT_PT * (f32)[self.window backingScaleFactor]) {
            /* Hit-test against the window that was clicked, so a double-click
             * in one window's titlebar runs that window's layout. */
            PlatformWindow *pw = platform_window_for_ns(self.window);
            bool zoomable = !g_zoom_query || !pw ||
                            g_zoom_query(pw, g_zoom_user, fb_x, fb_y);
            if (zoomable) [self.window performZoom:nil];
        }
    }
}

- (void)mouseDragged:(NSEvent *)event {
    [event retain];
    [_lastMouseEvent release];
    _lastMouseEvent = event;
    [self pushMouseEvent:EVENT_MOUSE_MOVE button:0 event:event];
}
- (void)mouseUp:(NSEvent *)event {
    [self pushMouseEvent:EVENT_MOUSE_UP button:0 event:event];
}
- (void)rightMouseDown:(NSEvent *)event {
    [self pushMouseEvent:EVENT_MOUSE_DOWN button:1 event:event];
}
- (void)rightMouseUp:(NSEvent *)event {
    [self pushMouseEvent:EVENT_MOUSE_UP button:1 event:event];
}

- (void)mouseMoved:(NSEvent *)event {
    [self pushMouseEvent:EVENT_MOUSE_MOVE button:-1 event:event];
}
/* mouseDragged is handled above with window drag support */
- (void)rightMouseDragged:(NSEvent *)event {
    [self pushMouseEvent:EVENT_MOUSE_MOVE button:1 event:event];
}

- (void)flagsChanged:(NSEvent *)event {
    KeyCode key = map_key(event.keyCode);
    u32 mods = get_mods(event.modifierFlags);
    u32 mask = mod_mask_for_key(key);
    if (mask) {
        bool down = (mods & mask) != 0;
        push_event((PlatformEvent){
            .type = down ? EVENT_KEY_DOWN : EVENT_KEY_UP,
            .key = { .key = key, .mods = mods, .repeat = false }
        });
    }
    _lastModifierFlags = event.modifierFlags;

    /* Emit a synthetic mouse move with updated mods so URL hover clears on Cmd release */
    [self pushMouseEvent:EVENT_MOUSE_MOVE button:-1 event:event];
}

- (void)scrollWheel:(NSEvent *)event {
    [self pushMouseEvent:EVENT_MOUSE_MOVE button:-1 event:event];
    push_event((PlatformEvent){
        .type = EVENT_SCROLL,
        .scroll = { .dx = (f32)event.scrollingDeltaX,
                    .dy = (f32)event.scrollingDeltaY,
                    .mods = get_mods(event.modifierFlags),
                    .precise = event.hasPreciseScrollingDeltas,
                    .momentum = (event.momentumPhase != NSEventPhaseNone) }
    });
}

/* NSTextInputClient — for proper text input */
- (void)insertText:(id)string replacementRange:(NSRange)range {
    (void)range;
    NSString *str = ([string isKindOfClass:[NSAttributedString class]])
        ? [(NSAttributedString *)string string] : (NSString *)string;
    for (NSUInteger i = 0; i < str.length; i++) {
        unichar ch = [str characterAtIndex:i];
        u32 cp = (u32)ch;
        /* Astral (non-BMP, > U+FFFF) chars arrive as a UTF-16 surrogate pair.
         * Combine the high+low surrogate into the full code point and skip the
         * trailing unit; otherwise two lone surrogates would be emitted and
         * corrupt the UTF-8 written to the PTY. BMP chars take the (u32)ch path
         * unchanged. */
        if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < str.length) {
            unichar lo = [str characterAtIndex:i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + (((u32)ch - 0xD800) << 10) + ((u32)lo - 0xDC00);
                i++;
            }
        }
        /* Text from the input manager is FINAL, composed output: the modifiers
         * that produced it (Shift, and Option on layouts where it composes a
         * character — e.g. Turkish Option+Q = '@', Option+8 = '[') are already
         * baked into `ch`. Reporting mods=0 here is essential: otherwise the
         * Option flag rides along and terminal_char_input() mistakes the
         * composed ASCII char for Alt+<key> (Meta) and prepends ESC, sending
         * "ESC @" for Option+Q instead of "@". Chords the user wants as Meta
         * take the Option-as-Alt path in keyDown, which emits the raw key with
         * MOD_ALT *before* interpretKeyEvents and never reaches here. */
        push_event((PlatformEvent){
            .type = EVENT_CHAR_INPUT,
            .char_input = { .codepoint = cp, .mods = 0 }
        });
    }
}

/* Respond to macOS menu Copy/Paste/Select All — push as KEY_DOWN so keybinds handle them */
- (void)copy:(id)sender {
    (void)sender;
    push_event((PlatformEvent){
        .type = EVENT_KEY_DOWN,
        .key = { .key = KEY_C, .mods = MOD_SUPER, .repeat = false }
    });
}
- (void)paste:(id)sender {
    (void)sender;
    push_event((PlatformEvent){
        .type = EVENT_KEY_DOWN,
        .key = { .key = KEY_V, .mods = MOD_SUPER, .repeat = false }
    });
}
- (void)selectAll:(id)sender {
    (void)sender;
    push_event((PlatformEvent){
        .type = EVENT_KEY_DOWN,
        .key = { .key = KEY_A, .mods = MOD_SUPER, .repeat = false }
    });
}

- (void)doCommandBySelector:(SEL)selector {
    /* Swallow system command selectors that aren't handled */
    (void)selector;
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    (void)string; (void)selectedRange; (void)replacementRange;
}
- (void)unmarkText {}
- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }
- (NSRange)markedRange { return NSMakeRange(NSNotFound, 0); }
- (BOOL)hasMarkedText { return NO; }
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    (void)range; (void)actualRange; return nil;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point { (void)point; return 0; }
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    (void)range; (void)actualRange; return NSZeroRect;
}
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText { return @[]; }

/* =========================================================================
 * NSDraggingDestination -- drag & drop file upload
 * ========================================================================= */

/* Convert an NSDraggingInfo's window-space location to backing (framebuffer)
 * pixels, top-down — matching the coordinate space of mouse events. */
- (void)draggingPoint:(id<NSDraggingInfo>)sender x:(i32 *)ox y:(i32 *)oy {
    NSPoint p  = [self convertPoint:[sender draggingLocation] fromView:nil];
    NSPoint bp = [self convertPointToBacking:p];
    NSRect  fb = [self convertRectToBacking:self.bounds];
    if (ox) *ox = (i32)bp.x;
    if (oy) *oy = (i32)(fb.size.height - bp.y);
}

/* Every push below names self.window explicitly.
 *
 * The NSDraggingDestination callbacks are delivered by the drag manager, NOT
 * from -[NSApp sendEvent:], so g_event_source is NULL throughout a drag, and
 * macOS does not make a window key merely because a drag hovers it. Unstamped,
 * a file dropped on one window while another is focused would paste its path
 * into the focused one. */
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    NSPasteboard *pb = [sender draggingPasteboard];
    if ([pb canReadObjectForClasses:@[[NSURL class]]
                            options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}]) {
        i32 dx = 0, dy = 0;
        [self draggingPoint:sender x:&dx y:&dy];
        push_event_for(self.window,
            (PlatformEvent){ .type = EVENT_DRAG_ENTER, .drag = { .x = dx, .y = dy } });
        return NSDragOperationCopy;
    }
    return NSDragOperationNone;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    NSPasteboard *pb = [sender draggingPasteboard];
    if ([pb canReadObjectForClasses:@[[NSURL class]]
                            options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}]) {
        /* Track the live hover position so a drop can target the pane under
         * the cursor (macOS doesn't deliver mouse-move events during a drag). */
        i32 dx = 0, dy = 0;
        [self draggingPoint:sender x:&dx y:&dy];
        push_event_for(self.window,
            (PlatformEvent){ .type = EVENT_DRAG_ENTER, .drag = { .x = dx, .y = dy } });
        return NSDragOperationCopy;
    }
    return NSDragOperationNone;
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
    (void)sender;
    push_event_for(self.window, (PlatformEvent){ .type = EVENT_DRAG_EXIT });
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSPasteboard *pb = [sender draggingPasteboard];
    NSArray<NSURL *> *urls = [pb readObjectsForClasses:@[[NSURL class]]
                                               options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    if (!urls || urls.count == 0) return NO;

    /* Push a DROP_FILE event for each dropped file.
     * Path strings are copied into static ring buffer to survive event queue. */
    static char drop_paths[16][1024];
    static i32 drop_idx = 0;

    push_event_for(self.window,
        (PlatformEvent){ .type = EVENT_DRAG_EXIT }); /* clear overlay */

    /* Drop location (framebuffer px, top-down) so the consumer can target the
     * pane under the cursor rather than the focused one. */
    i32 drop_x = 0, drop_y = 0;
    [self draggingPoint:sender x:&drop_x y:&drop_y];

    /* The ring has 16 slots and the events are consumed a frame later, so
     * one drop delivers at most a ring's worth (more would overwrite the
     * first ones before they are read), and a path longer than a slot is
     * skipped (cut, it would name a DIFFERENT path). */
    i32 delivered = 0;
    for (NSURL *url in urls) {
        if (delivered >= 16) break;
        const char *path = [[url path] UTF8String];
        if (!path) continue;
        i32 slot = drop_idx++ & 15; /* ring buffer of 16 slots */
        int pn = snprintf(drop_paths[slot], sizeof(drop_paths[slot]), "%s", path);
        if (pn < 0 || (usize)pn >= sizeof(drop_paths[slot])) { drop_idx--; continue; }
        delivered++;
        push_event_for(self.window, (PlatformEvent){
            .type = EVENT_DROP_FILE,
            .drop = { .path = drop_paths[slot], .x = drop_x, .y = drop_y }
        });
    }
    return YES;
}

/* ----------------------------------------------------------------------- *
 *  NSDraggingSource — required so a session Liu starts is accepted by Finder
 * ----------------------------------------------------------------------- */

- (NSDragOperation)draggingSession:(NSDraggingSession *)session
          sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session;
    /* Finder copy = drag out to a folder; within-app doesn't reach this. */
    return (context == NSDraggingContextOutsideApplication)
             ? NSDragOperationCopy
             : NSDragOperationNone;
}

/* ----------------------------------------------------------------------- *
 *  NSFilePromiseProviderDelegate — deferred filename + async writer
 * ----------------------------------------------------------------------- */

- (NSString *)filePromiseProvider:(NSFilePromiseProvider *)filePromiseProvider
                  fileNameForType:(NSString *)fileType {
    (void)fileType;
    NSDictionary *info = (NSDictionary *)filePromiseProvider.userInfo;
    NSString *name = info[@"name"];
    return name ? name : @"dropped";
}

- (NSOperationQueue *)operationQueueForFilePromiseProvider:(NSFilePromiseProvider *)filePromiseProvider {
    (void)filePromiseProvider;
    return _promiseQueue;
}

- (void)filePromiseProvider:(NSFilePromiseProvider *)filePromiseProvider
          writePromiseToURL:(NSURL *)url
          completionHandler:(void (^)(NSError * _Nullable))completionHandler
{
    NSDictionary *info = (NSDictionary *)filePromiseProvider.userInfo;
    LiuFilePromiseBox *box = info[@"box"];

    const char *path = [[url path] fileSystemRepresentation];
    if (!box || !path || ![box writeToPath:path]) {
        completionHandler([NSError errorWithDomain:NSCocoaErrorDomain
                                              code:NSFileWriteUnknownError
                                          userInfo:nil]);
        return;
    }
    completionHandler(nil);
}

- (void)dealloc {
    [_lastMouseEvent release];
    [_promiseQueue release];
    [super dealloc];
}

@end

/* =========================================================================
 * PlatformWindow
 * ========================================================================= */

/* =========================================================================
 * Native Tab Bar View — renders tabs using Cocoa text, pixel-perfect
 * ========================================================================= */

@interface LiuTabBarView : NSView
@property (nonatomic, strong) NSMutableArray<NSString *> *tabTitles;
@property (nonatomic, strong) NSMutableArray<NSNumber *> *tabIsSSH;
@property (nonatomic) NSInteger activeTab;
@property (nonatomic) NSInteger hoverTab;
@property (nonatomic) NSInteger hoverClose;
@end

@implementation LiuTabBarView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        _tabTitles = [NSMutableArray new];
        _tabIsSSH = [NSMutableArray new];
        _activeTab = 0;
        _hoverTab = -1;
        _hoverClose = -1;
        NSTrackingArea *ta = [[NSTrackingArea alloc] initWithRect:self.bounds
            options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
            owner:self userInfo:nil];
        [self addTrackingArea:ta];
        [ta release];
    }
    return self;
}

- (void)dealloc {
    [_tabTitles release];
    [_tabIsSSH release];
    [super dealloc];
}

- (BOOL)isFlipped { return YES; }
- (BOOL)wantsLayer { return YES; }

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    NSRect bounds = self.bounds;

    /* Background */
    [[NSColor colorWithRed:0.12 green:0.12 blue:0.14 alpha:1.0] setFill];
    NSRectFill(bounds);

    /* Bottom border */
    [[NSColor colorWithRed:0.2 green:0.2 blue:0.22 alpha:0.4] setFill];
    NSRectFill(NSMakeRect(0, bounds.size.height - 0.5, bounds.size.width, 0.5));

    /* Tab layout */
    CGFloat tabW = 150, tabH = bounds.size.height - 6, tabY = 3, tabGap = 2;
    CGFloat startX = 6; /* after sidebar toggle area handled by OpenGL */

    NSDictionary *activeAttrs = @{
        NSFontAttributeName: [NSFont systemFontOfSize:12 weight:NSFontWeightMedium],
        NSForegroundColorAttributeName: [NSColor colorWithRed:0.95 green:0.95 blue:0.97 alpha:1.0]
    };
    NSDictionary *inactiveAttrs = @{
        NSFontAttributeName: [NSFont systemFontOfSize:12 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: [NSColor colorWithRed:0.55 green:0.55 blue:0.58 alpha:1.0]
    };
    /* closeAttrs reserved for future close-button rendering */

    for (NSInteger i = 0; i < (NSInteger)self.tabTitles.count; i++) {
        CGFloat tx = startX + i * (tabW + tabGap);
        BOOL active = (i == self.activeTab);

        /* Tab background */
        if (active) {
            NSBezierPath *bg = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(tx, tabY, tabW, tabH)
                                xRadius:4 yRadius:4];
            [[NSColor colorWithRed:0.18 green:0.18 blue:0.22 alpha:1.0] setFill];
            [bg fill];

            /* Bottom accent */
            [[NSColor colorWithRed:0.35 green:0.55 blue:0.85 alpha:1.0] setFill];
            NSRectFill(NSMakeRect(tx, tabY + tabH - 2, tabW, 2));
        } else if (i == self.hoverTab) {
            NSBezierPath *bg = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(tx, tabY, tabW, tabH)
                                xRadius:4 yRadius:4];
            [[NSColor colorWithRed:0.15 green:0.15 blue:0.18 alpha:1.0] setFill];
            [bg fill];
        }

        /* Session dot */
        BOOL isSSH = (i < (NSInteger)self.tabIsSSH.count) ? self.tabIsSSH[i].boolValue : NO;
        NSColor *dotColor = isSSH
            ? [NSColor colorWithRed:0.3 green:0.75 blue:0.4 alpha:1.0]
            : [NSColor colorWithRed:0.4 green:0.55 blue:0.75 alpha:1.0];
        NSRect dotRect = NSMakeRect(tx + 10, tabY + tabH/2 - 2.5, 5, 5);
        NSBezierPath *dot = [NSBezierPath bezierPathWithOvalInRect:dotRect];
        [dotColor setFill];
        [dot fill];

        /* Title */
        NSString *title = self.tabTitles[i];
        NSDictionary *attrs = active ? activeAttrs : inactiveAttrs;
        NSSize titleSize = [title sizeWithAttributes:attrs];
        CGFloat maxTitleW = tabW - 45; /* room for dot + close */
        NSRect titleRect = NSMakeRect(tx + 22, tabY + (tabH - titleSize.height)/2,
                                       maxTitleW, titleSize.height);
        [title drawWithRect:titleRect options:NSStringDrawingTruncatesLastVisibleLine|NSStringDrawingUsesLineFragmentOrigin
                 attributes:attrs];

        /* Close button */
        CGFloat closeX = tx + tabW - 22;
        CGFloat closeY = tabY + (tabH - 12) / 2;
        NSColor *closeFg = (i == self.hoverClose)
            ? [NSColor colorWithRed:0.9 green:0.3 blue:0.3 alpha:1.0]
            : [NSColor colorWithRed:0.5 green:0.5 blue:0.53 alpha:active ? 0.6 : 0.3];
        if (i == self.hoverClose) {
            [[NSColor colorWithRed:0.8 green:0.2 blue:0.2 alpha:0.15] setFill];
            NSRectFill(NSMakeRect(closeX - 3, closeY - 1, 18, 14));
        }
        [@"x" drawAtPoint:NSMakePoint(closeX + 2, closeY - 1)
            withAttributes:@{
                NSFontAttributeName: [NSFont systemFontOfSize:11],
                NSForegroundColorAttributeName: closeFg
            }];
    }

    /* New tab (+) button */
    CGFloat plusX = startX + self.tabTitles.count * (tabW + tabGap) + 8;
    CGFloat plusY = bounds.size.height / 2;
    [[NSColor colorWithRed:0.5 green:0.5 blue:0.53 alpha:1.0] setFill];
    NSRectFill(NSMakeRect(plusX, plusY - 0.5, 10, 1));
    NSRectFill(NSMakeRect(plusX + 4.5, plusY - 5, 1, 10));
}

- (void)mouseMoved:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    CGFloat tabW = 150, tabGap = 2, startX = 6;
    self.hoverTab = -1;
    self.hoverClose = -1;
    for (NSInteger i = 0; i < (NSInteger)self.tabTitles.count; i++) {
        CGFloat tx = startX + i * (tabW + tabGap);
        if (p.x >= tx && p.x < tx + tabW && p.y >= 3 && p.y < self.bounds.size.height - 3) {
            self.hoverTab = i;
            if (p.x >= tx + tabW - 25) self.hoverClose = i;
            break;
        }
    }
    [self setNeedsDisplay:YES];
}

- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    CGFloat tabW = 150, tabGap = 2, startX = 6;

    for (NSInteger i = 0; i < (NSInteger)self.tabTitles.count; i++) {
        CGFloat tx = startX + i * (tabW + tabGap);
        if (p.x >= tx && p.x < tx + tabW) {
            if (p.x >= tx + tabW - 25) {
                /* Close button clicked — push event */
                push_event((PlatformEvent){ .type = EVENT_MENU_CLOSE_TAB, .key.key = (KeyCode)i });
            } else {
                /* Tab clicked — push tab switch as key event with index */
                PlatformEvent e = { .type = EVENT_KEY_DOWN };
                e.key.key = KEY_1 + (KeyCode)i;
                e.key.mods = MOD_SUPER;
                push_event(e);
            }
            return;
        }
    }

    /* New tab (+) button */
    CGFloat plusX = startX + self.tabTitles.count * (tabW + tabGap) + 4;
    if (p.x >= plusX && p.x < plusX + 20) {
        push_event((PlatformEvent){ .type = EVENT_MENU_NEW_TAB });
    }
}

@end

/* Declared down at the toast implementation; the window owns one. */
@class LiuNativeToastView;

struct PlatformWindow {
    NSWindow            *ns_window;
    SSHView             *view;
    SSHWindowDelegate   *delegate;
    /* Display link — the panel's own cadence. See platform.h for the three
     * rules; the counter is written from a CoreVideo thread, hence _Atomic. */
    CVDisplayLinkRef     display_link;
    CGDirectDisplayID    link_display;
    _Atomic uint64_t     vsync_ticks;
    _Atomic uint64_t     link_last_host_time;
    _Atomic uint64_t     link_period_ticks;
    _Atomic bool         link_rebind;   /* set by windowDidChangeScreen: */
    /* The last four gaps between callbacks (host ticks) — the watchdog period
     * is their MINIMUM, see frame_pacing_min_gap. */
    _Atomic uint64_t     link_gaps[4];
    _Atomic uint32_t     link_gap_n;
    /* When the link was last started (host ticks; 0 = not running): what its
     * silence is measured from until the first tick. */
    _Atomic uint64_t     link_started_host;
    /* Set when the displays were reconfigured or the machine woke: the link is
     * torn down and bound again even though the display id may read the same. */
    _Atomic bool         link_refresh;
    f64                  link_restart_after;   /* main thread: no restart before this */
#ifdef USE_METAL
    id<MTLDevice>       mtl_device;
    CAMetalLayer       *mtl_layer;
    NSView             *container;   /* rounds the corners; owns `view` */
    CALayer            *backdrop;    /* CABackdropLayer: the blur, under `view` */
    id<MTLCommandQueue> mtl_queue;
#else
    NSOpenGLContext    *gl_context;
#endif
    /* Last values pushed to the window server, so a repeated apply is a real
     * no-op. Re-assigning the backdrop's `filters` array rebuilds the
     * CoreAnimation filter and the blur visibly blinks while it re-renders —
     * and the appearance apply runs on every settings change, every slider
     * move during a drag, and (before the self-write guard in main.c) every
     * config save. `*_valid` distinguishes "never applied" from "applied 0". */
    bool               blur_applied_valid;
    f32                blur_applied;
    bool               opaque_applied_valid;
    bool               opaque_applied;

    /* Painted tab-strip height in points; 0 = never told, use the default.
     * Per window because two windows can paint different-height strips (tab
     * bar shown vs hidden) — see liu_strip_pt_for_window. */
    CGFloat            titlebar_strip_pt;

    /* Quake mode, per window. Only the tuning (height ratio, animation
     * duration) is global; the saved chrome to restore and the enabled /
     * visible / animating flags describe THIS window and nothing else. */
    bool               quake_enabled;
    bool               quake_visible;
    bool               quake_animating;
    NSRect             quake_saved_frame;  /* frame before quake was enabled */
    NSUInteger         quake_saved_style;  /* style mask before quake */
    NSInteger          quake_saved_level;  /* window level before quake */

    /* Floating native toast (shown above a WKWebView, which Metal cannot
     * cover). One per window, so an idle frame in one window cannot hide
     * another window's toast, and destroying a window removes only its own. */
    LiuNativeToastView *toast[PLATFORM_TOAST_SLOTS];

    /* Accessibility text bridge (platform_a11y.h). `a11y_cache` is the last
     * snapshot the view answered from (a LiuA11ySnap, retained); the rest is
     * the VoiceOver-only change notifier's state. */
    PlatformA11yProviderFn a11y_fn;
    void              *a11y_user;
    id                 a11y_cache;
    u64                a11y_posted_value, a11y_posted_sel;
    f64                a11y_last_post;
    bool               a11y_post_scheduled;

    bool               should_close;
};

/* Registry helper — defined after the struct so the forward declaration up by
 * the event queue can stay above it. */
static NSWindow *platform_window_nswindow(PlatformWindow *w) {
    return w ? w->ns_window : nil;
}

/* Painted tab-strip accessors. Zero means "the UI never set it", which is the
 * state a freshly calloc'd window is in, so fall back to the common case. */
static CGFloat liu_strip_pt_for_window(PlatformWindow *w) {
    return (w && w->titlebar_strip_pt > 0.0) ? w->titlebar_strip_pt
                                             : (CGFloat)TOOLBAR_HEIGHT_PT;
}
static CGFloat liu_strip_pt_for_ns(NSWindow *ns) {
    return liu_strip_pt_for_window(platform_window_for_ns(ns));
}
static void liu_set_strip_pt_for_window(PlatformWindow *w, CGFloat pt) {
    if (w) w->titlebar_strip_pt = pt;
}

/* =========================================================================
 * Accessibility — the terminal view as a read-only text area (VoiceOver)
 *
 * The view answers NSAccessibility's text-area methods from a snapshot the
 * app's provider builds (platform_a11y.h: UTF-16 indices, one line per screen
 * row, the cell geometry). Nothing here runs unless an assistive app asks, and
 * the change notifier below is gated on VoiceOver being on, cached and kept
 * current by KVO — so an idle Liu with VoiceOver off pays one bool test per
 * PRESENTED frame and nothing per idle second.
 * ========================================================================= */

/* A snapshot the view can answer many questions from: VoiceOver asks for the
 * value, then the line of the insertion point, then that line's range, then
 * its frame, within a few milliseconds. The tables are COPIED out of the
 * provider's buffers (which live only until its next call) and `s` is re-aimed
 * at the copies, so the pure helpers in platform_a11y.h run on it unchanged. */
@interface LiuA11ySnap : NSObject {
@public
    NSString        *text;
    NSString        *selText;
    NSString        *label;
    NSData          *lineStart, *lineTextEnd, *lineEndCol, *col16;
    PlatformA11yText s;
    f64              builtAt;
}
@end

@implementation LiuA11ySnap
- (void)dealloc {
    [text release];
    [selText release];
    [label release];
    [lineStart release];
    [lineTextEnd release];
    [lineEndCol release];
    [col16 release];
    [super dealloc];
}
@end

/* How long a snapshot may be answered from without asking the provider
 * whether anything moved, and how long regardless (an erase that writes no
 * codepoint is invisible to the generation counter). */
#define LIU_A11Y_CACHE_MAX_SEC   0.25
/* The notifier's rate cap: at most this many change posts a second per
 * window. A flooding pane repaints 60 times a second; VoiceOver re-reads the
 * whole value on each post, so more than a few is noise, not information. */
#define LIU_A11Y_POST_INTERVAL   0.25

static bool g_a11y_vo_on = false;
static id   g_a11y_vo_observer = nil;

@interface LiuVoiceOverObserver : NSObject
@end
@implementation LiuVoiceOverObserver
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                        change:(NSDictionary *)change context:(void *)context {
    (void)keyPath; (void)object; (void)change; (void)context;
    bool on = [[NSWorkspace sharedWorkspace] isVoiceOverEnabled];
    if ([NSThread isMainThread]) g_a11y_vo_on = on;
    else dispatch_async(dispatch_get_main_queue(), ^{ g_a11y_vo_on = on; });
}
@end

/* Read once, then follow the KVO notification — never polled per frame. */
static void liu_a11y_watch_voiceover(void) {
    if (g_a11y_vo_observer) return;
    g_a11y_vo_observer = [[LiuVoiceOverObserver alloc] init];
    NSWorkspace *ws = [NSWorkspace sharedWorkspace];
    [ws addObserver:g_a11y_vo_observer forKeyPath:@"voiceOverEnabled"
            options:NSKeyValueObservingOptionNew context:NULL];
    g_a11y_vo_on = [ws isVoiceOverEnabled];
}

void platform_window_set_a11y_provider(PlatformWindow *w,
                                       PlatformA11yProviderFn fn, void *user) {
    if (!w) return;
    w->a11y_fn   = fn;
    w->a11y_user = user;
    [w->a11y_cache release];
    w->a11y_cache = nil;
    w->a11y_posted_value = w->a11y_posted_sel = 0;
    if (fn) liu_a11y_watch_voiceover();
}

static void liu_a11y_window_teardown(PlatformWindow *w) {
    if (!w) return;
    w->a11y_fn = NULL;
    w->a11y_user = NULL;
    [w->a11y_cache release];
    w->a11y_cache = nil;
}

static LiuA11ySnap *liu_a11y_empty_snap(NSString *label) {
    LiuA11ySnap *sn = [[[LiuA11ySnap alloc] init] autorelease];
    sn->text  = [@"" retain];
    sn->label = [(label ? label : @"Terminal") retain];
    return sn;
}

/* The snapshot for window `w`, rebuilt only when the provider's generations
 * moved or the cache aged out. Never nil. */
static LiuA11ySnap *liu_a11y_snapshot(PlatformWindow *w) {
    if (!w || !w->a11y_fn) return liu_a11y_empty_snap(nil);
    f64 now = platform_time_sec();
    LiuA11ySnap *cached = (LiuA11ySnap *)w->a11y_cache;
    if (cached && now - cached->builtAt < LIU_A11Y_CACHE_MAX_SEC) {
        PlatformA11yText g;
        memset(&g, 0, sizeof(g));
        w->a11y_fn(w, w->a11y_user, &g, true);
        if (g.gen_value == cached->s.gen_value &&
            g.gen_selection == cached->s.gen_selection)
            return cached;
    }

    PlatformA11yText s;
    memset(&s, 0, sizeof(s));
    bool ok = w->a11y_fn(w, w->a11y_user, &s, false);
    NSString *label = s.label ? [NSString stringWithUTF8String:s.label] : nil;
    LiuA11ySnap *sn = nil;
    if (ok && s.text && s.line_count > 0 && s.line_start16) {
        NSString *text = [[[NSString alloc] initWithBytes:s.text length:s.text_bytes
                                                 encoding:NSUTF8StringEncoding] autorelease];
        /* The tables are in UTF-16 units computed on the C side; the string
         * must agree or every index below is off. It always should (the
         * builder replaces anything NSString would reject) — this is the
         * belt, and it degrades to an empty area rather than to wrong text. */
        if (text && (u32)text.length == s.length16) {
            sn = [[[LiuA11ySnap alloc] init] autorelease];
            sn->text        = [text retain];
            sn->lineStart   = [[NSData alloc] initWithBytes:s.line_start16
                                                     length:(NSUInteger)(s.line_count + 1) * sizeof(u32)];
            sn->lineTextEnd = [[NSData alloc] initWithBytes:s.line_text_end16
                                                     length:(NSUInteger)s.line_count * sizeof(u32)];
            sn->lineEndCol  = [[NSData alloc] initWithBytes:s.line_end_col
                                                     length:(NSUInteger)s.line_count * sizeof(u16)];
            sn->col16       = [[NSData alloc] initWithBytes:s.col16
                                                     length:(NSUInteger)s.length16 * sizeof(u16)];
            if (s.sel_text && s.sel_text_bytes)
                sn->selText = [[NSString alloc] initWithBytes:s.sel_text length:s.sel_text_bytes
                                                     encoding:NSUTF8StringEncoding];
            sn->s = s;
            sn->s.text            = NULL;
            sn->s.sel_text        = NULL;
            sn->s.label           = NULL;
            sn->s.line_start16    = (const u32 *)sn->lineStart.bytes;
            sn->s.line_text_end16 = (const u32 *)sn->lineTextEnd.bytes;
            sn->s.line_end_col    = (const u16 *)sn->lineEndCol.bytes;
            sn->s.col16           = (const u16 *)sn->col16.bytes;
        }
    }
    if (!sn) {
        sn = liu_a11y_empty_snap(label);
        sn->s.gen_value = s.gen_value;
        sn->s.gen_selection = s.gen_selection;
    } else {
        sn->label = [(label ? label : @"Terminal") retain];
    }
    sn->builtAt = now;
    [w->a11y_cache release];
    w->a11y_cache = [sn retain];
    return sn;
}

/* Ask the provider whether anything moved since the last post, and post. */
static void liu_a11y_post_changes(PlatformWindow *w) {
    if (!w || !w->a11y_fn || !w->view) return;
    PlatformA11yText g;
    memset(&g, 0, sizeof(g));
    w->a11y_fn(w, w->a11y_user, &g, true);
    bool value_moved = g.gen_value != w->a11y_posted_value;
    bool sel_moved   = g.gen_selection != w->a11y_posted_sel;
    if (!value_moved && !sel_moved) return;
    w->a11y_posted_value = g.gen_value;
    w->a11y_posted_sel   = g.gen_selection;
    w->a11y_last_post    = platform_time_sec();
    if (value_moved)
        NSAccessibilityPostNotification(w->view, NSAccessibilityValueChangedNotification);
    if (sel_moved)
        NSAccessibilityPostNotification(w->view, NSAccessibilitySelectedTextChangedNotification);
}

/* Called for every PRESENTED frame (platform_window_swap_buffers). Frames are
 * what the terminal changing already produces — output, a scroll, a selection
 * drag — so this adds no wake source and an idle window, which presents
 * nothing, never gets here. With VoiceOver off it is one bool test. Posts are
 * capped at one per LIU_A11Y_POST_INTERVAL; a change inside the cap gets ONE
 * trailing check when the interval ends (a main-queue block, VoiceOver-only,
 * that posts an accessibility notification and pushes no event, so the main
 * loop does not wake to paint for it). */
static void liu_a11y_frame_presented(PlatformWindow *w) {
    if (!g_a11y_vo_on || !w || !w->a11y_fn || !w->view) return;
    if (![w->ns_window isVisible]) return;
    f64 now = platform_time_sec();
    f64 wait = w->a11y_last_post + LIU_A11Y_POST_INTERVAL - now;
    if (wait <= 0.0) { liu_a11y_post_changes(w); return; }
    if (w->a11y_post_scheduled) return;
    PlatformA11yText g;
    memset(&g, 0, sizeof(g));
    w->a11y_fn(w, w->a11y_user, &g, true);
    if (g.gen_value == w->a11y_posted_value && g.gen_selection == w->a11y_posted_sel)
        return;
    w->a11y_post_scheduled = true;
    SSHView *view = w->view;     /* retained by the block copy (MRC) */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(wait * 1e9)),
                   dispatch_get_main_queue(), ^{
        /* Re-resolve: the window may have closed while this waited. */
        PlatformWindow *pw = platform_window_for_ns([view window]);
        if (!pw || pw->view != view) return;
        pw->a11y_post_scheduled = false;
        if (g_a11y_vo_on) liu_a11y_post_changes(pw);
    });
}

/* Cells (top-left-origin view points) → screen rect. */
static NSRect liu_a11y_cells_to_screen(NSView *v, const PlatformA11yText *s,
                                       i32 c0, i32 r0, i32 c1, i32 r1) {
    if (!s->has_geometry || !v.window) return NSZeroRect;
    CGFloat x = s->grid_x + (CGFloat)c0 * s->cell_w;
    CGFloat y = s->grid_y + (CGFloat)r0 * s->cell_h;
    CGFloat w = (CGFloat)(c1 - c0) * s->cell_w;
    CGFloat h = (CGFloat)(r1 - r0 + 1) * s->cell_h;
    if (w < 0) w = 0;
    NSRect r = NSMakeRect(x, v.isFlipped ? y : v.bounds.size.height - y - h, w, h);
    return [v.window convertRectToScreen:[v convertRect:r toView:nil]];
}

static NSRange liu_a11y_clamp(const LiuA11ySnap *sn, NSRange r) {
    NSUInteger n = sn->text.length;
    if (r.location > n) r.location = n;
    if (r.length > n - r.location) r.length = n - r.location;
    return r;
}

/* Both SSHView flavours (Metal and OpenGL) share this: a category applies to
 * whichever one this build compiled. Read-only — typing already reaches the
 * terminal through NSTextInputClient, and nothing here takes first responder
 * or touches key handling. */
@implementation SSHView (LiuAccessibility)

- (LiuA11ySnap *)liuA11ySnap {
    return liu_a11y_snapshot(platform_window_for_ns(self.window));
}

- (BOOL)isAccessibilityElement { return YES; }
- (NSAccessibilityRole)accessibilityRole { return NSAccessibilityTextAreaRole; }
- (NSString *)accessibilityLabel { return [self liuA11ySnap]->label; }
- (id)accessibilityValue { return [self liuA11ySnap]->text; }
- (NSInteger)accessibilityNumberOfCharacters {
    return (NSInteger)[self liuA11ySnap]->text.length;
}
- (NSRange)accessibilityVisibleCharacterRange {
    return NSMakeRange(0, [self liuA11ySnap]->text.length);
}

/* The selection when there is one; otherwise an empty range at the cursor,
 * which is how a text area tells VoiceOver where the insertion point is. */
- (NSRange)accessibilitySelectedTextRange {
    LiuA11ySnap *sn = [self liuA11ySnap];
    if (sn->s.has_selection)
        return liu_a11y_clamp(sn, NSMakeRange(sn->s.sel_start16, sn->s.sel_len16));
    if (sn->s.has_cursor) return liu_a11y_clamp(sn, NSMakeRange(sn->s.cursor16, 0));
    return NSMakeRange(0, 0);
}
- (NSArray<NSValue *> *)accessibilitySelectedTextRanges {
    return @[ [NSValue valueWithRange:[self accessibilitySelectedTextRange]] ];
}
/* What Cmd+C would copy — including any part scrolled off screen, and a
 * rectangle as its block of columns — rather than the on-screen span. */
- (NSString *)accessibilitySelectedText {
    LiuA11ySnap *sn = [self liuA11ySnap];
    if (!sn->s.has_selection) return @"";
    if (sn->selText) return sn->selText;
    return [sn->text substringWithRange:[self accessibilitySelectedTextRange]];
}
- (NSInteger)accessibilityInsertionPointLineNumber {
    LiuA11ySnap *sn = [self liuA11ySnap];
    if (sn->s.line_count <= 0) return 0;
    NSRange r = [self accessibilitySelectedTextRange];
    return a11y_line_for_index(&sn->s, (u32)r.location);
}

- (NSInteger)accessibilityLineForIndex:(NSInteger)index {
    LiuA11ySnap *sn = [self liuA11ySnap];
    if (sn->s.line_count <= 0 || index < 0) return 0;
    return a11y_line_for_index(&sn->s, (u32)index);
}
- (NSRange)accessibilityRangeForLine:(NSInteger)line {
    LiuA11ySnap *sn = [self liuA11ySnap];
    u32 loc = 0, len = 0;
    if (line < 0 || !a11y_range_for_line(&sn->s, (i32)line, &loc, &len))
        return NSMakeRange(NSNotFound, 0);
    return NSMakeRange(loc, len);
}
- (NSString *)accessibilityStringForRange:(NSRange)range {
    LiuA11ySnap *sn = [self liuA11ySnap];
    return [sn->text substringWithRange:liu_a11y_clamp(sn, range)];
}
- (NSAttributedString *)accessibilityAttributedStringForRange:(NSRange)range {
    return [[[NSAttributedString alloc]
             initWithString:[self accessibilityStringForRange:range]] autorelease];
}
/* A composed character: a surrogate pair or a whole cluster is one unit to
 * VoiceOver's character navigation, never half an emoji. */
- (NSRange)accessibilityRangeForIndex:(NSInteger)index {
    LiuA11ySnap *sn = [self liuA11ySnap];
    if (index < 0 || (NSUInteger)index >= sn->text.length) return NSMakeRange(NSNotFound, 0);
    return [sn->text rangeOfComposedCharacterSequenceAtIndex:(NSUInteger)index];
}
/* No styled runs are exposed; a line is the unit a style range covers. */
- (NSRange)accessibilityStyleRangeForIndex:(NSInteger)index {
    return [self accessibilityRangeForLine:[self accessibilityLineForIndex:index]];
}
- (NSRect)accessibilityFrameForRange:(NSRange)range {
    LiuA11ySnap *sn = [self liuA11ySnap];
    if (sn->s.line_count <= 0) return NSZeroRect;
    range = liu_a11y_clamp(sn, range);
    i32 c0, r0, c1, r1;
    a11y_cells_for_range(&sn->s, (u32)range.location, (u32)range.length, &c0, &r0, &c1, &r1);
    return liu_a11y_cells_to_screen(self, &sn->s, c0, r0, c1, r1);
}
- (NSRange)accessibilityRangeForPosition:(NSPoint)point {
    LiuA11ySnap *sn = [self liuA11ySnap];
    if (sn->s.line_count <= 0 || !sn->s.has_geometry || !self.window ||
        sn->s.cell_w <= 0 || sn->s.cell_h <= 0)
        return NSMakeRange(NSNotFound, 0);
    NSPoint wp = [self.window convertPointFromScreen:point];
    NSPoint vp = [self convertPoint:wp fromView:nil];
    CGFloat top_y = self.isFlipped ? vp.y : self.bounds.size.height - vp.y;
    i32 col = (i32)floor((vp.x - sn->s.grid_x) / sn->s.cell_w);
    i32 row = (i32)floor((top_y - sn->s.grid_y) / sn->s.cell_h);
    u32 idx = a11y_index_for_cell(&sn->s, col, row);
    if (idx >= sn->text.length) return NSMakeRange(sn->text.length, 0);
    return [sn->text rangeOfComposedCharacterSequenceAtIndex:idx];
}

/* Terminal text is what the program drew; an assistive app cannot set it,
 * and moving the selection or the caret through here would bypass the
 * terminal's own selection model. */
- (BOOL)isAccessibilitySelectorAllowed:(SEL)selector {
    if (selector == @selector(setAccessibilityValue:) ||
        selector == @selector(setAccessibilitySelectedText:) ||
        selector == @selector(setAccessibilitySelectedTextRange:) ||
        selector == @selector(setAccessibilitySelectedTextRanges:) ||
        selector == @selector(setAccessibilityVisibleCharacterRange:))
        return NO;
    return [super isAccessibilitySelectorAllowed:selector];
}

@end


/* =========================================================================
 * Implementation
 * ========================================================================= */

/* Funnel an uncaught Objective-C exception (e.g. an invalid window title) into
 * the crash log before the default handler abort()s. Liu pumps its own event
 * loop (nextEventMatchingMask + sendEvent), so exceptions thrown inside AppKit
 * propagate out to that loop and terminate the app through this handler. */
static void liu_uncaught_exception_handler(NSException *exc) {
    @autoreleasepool {
        const char *name   = exc.name ? exc.name.UTF8String : "NSException";
        const char *reason = exc.reason ? exc.reason.UTF8String : NULL;
        NSMutableString *bt = [NSMutableString string];
        for (NSString *frame in exc.callStackSymbols) {
            [bt appendString:frame];
            [bt appendString:@"\n"];
        }
        crashlog_record_exception(name, reason, bt.UTF8String);
    }
}

/* The app menu's "Secure Keyboard Entry" item, kept for its check mark
 * (platform_menu_set_secure_keyboard). The menu bar lives as long as the app. */
static NSMenuItem *g_secure_menu_item = nil;

bool platform_init(void) {
    if (g_platform.initialized) return true;
    NSSetUncaughtExceptionHandler(liu_uncaught_exception_handler);
    @autoreleasepool {
        [NSApplication sharedApplication];
        g_app_delegate = [[SSHAppDelegate alloc] init];
        [NSApp setDelegate:g_app_delegate];

        /* ---- Menu Bar ---- */
        NSMenu *menuBar = [[NSMenu alloc] init];

        /* App menu (Liu) */
        NSMenuItem *appMenuItem = [[NSMenuItem alloc] initWithTitle:@"Liu" action:nil keyEquivalent:@""];
        NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"Liu"];
        [appMenu addItemWithTitle:@"About Liu" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:@"Settings..." action:@selector(menuSettings:) keyEquivalent:@","];
        [appMenu addItem:[NSMenuItem separatorItem]];
        /* Where Terminal keeps it, and with no key of its own: it is a switch
         * that is set once. */
        g_secure_menu_item = [appMenu addItemWithTitle:@"Secure Keyboard Entry"
                                               action:@selector(menuSecureKeyboard:) keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:@"Hide Liu" action:@selector(hide:) keyEquivalent:@"h"];
        NSMenuItem *hideOthers = [appMenu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
        [hideOthers setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
        [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:@"Quit Liu" action:@selector(terminate:) keyEquivalent:@"q"];
        [appMenuItem setSubmenu:appMenu];
        [menuBar addItem:appMenuItem];
        [appMenu release];
        [appMenuItem release];

        /* Shell menu */
        NSMenuItem *shellMenuItem = [[NSMenuItem alloc] init];
        NSMenu *shellMenu = [[NSMenu alloc] initWithTitle:@"Shell"];
        [shellMenu addItemWithTitle:@"New Tab" action:@selector(menuNewTab:) keyEquivalent:@"t"];
        [shellMenu addItemWithTitle:@"New Window" action:@selector(menuNewWindow:) keyEquivalent:@"n"];
        [shellMenu addItem:[NSMenuItem separatorItem]];
        [shellMenu addItemWithTitle:@"Close Tab" action:@selector(menuCloseTab:) keyEquivalent:@"w"];
        [shellMenu addItem:[NSMenuItem separatorItem]];
        [shellMenuItem setSubmenu:shellMenu];
        [menuBar addItem:shellMenuItem];
        [shellMenu release];
        [shellMenuItem release];

        /* Remote menu */
        NSMenuItem *remoteMenuItem = [[NSMenuItem alloc] init];
        NSMenu *remoteMenu = [[NSMenu alloc] initWithTitle:@"Remote"];
        [remoteMenu addItemWithTitle:@"SSH Connect..." action:@selector(menuSSHConnect:) keyEquivalent:@""];
        [remoteMenu addItemWithTitle:@"Mosh Connect..." action:@selector(menuMoshConnect:) keyEquivalent:@""];
        [remoteMenu addItemWithTitle:@"Telnet Connect..." action:@selector(menuTelnetConnect:) keyEquivalent:@""];
        [remoteMenu addItemWithTitle:@"Serial Connect..." action:@selector(menuSerialConnect:) keyEquivalent:@""];
        [remoteMenu addItem:[NSMenuItem separatorItem]];
        [remoteMenu addItemWithTitle:@"Import SSH Config" action:@selector(menuImportSSH:) keyEquivalent:@""];
        [remoteMenu addItemWithTitle:@"SSH Keys" action:@selector(menuSSHKeys:) keyEquivalent:@""];
        [remoteMenu addItemWithTitle:@"Known Hosts" action:@selector(menuKnownHosts:) keyEquivalent:@""];
        [remoteMenu addItemWithTitle:@"Port Forwarding" action:@selector(menuPortForwarding:) keyEquivalent:@""];
        [remoteMenuItem setSubmenu:remoteMenu];
        [menuBar addItem:remoteMenuItem];
        [remoteMenu release];
        [remoteMenuItem release];

        /* Edit menu */
        NSMenuItem *editMenuItem = [[NSMenuItem alloc] init];
        NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
        [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
        [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
        [editMenu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
        [editMenu addItem:[NSMenuItem separatorItem]];
        [editMenu addItemWithTitle:@"Find..." action:@selector(menuFind:) keyEquivalent:@"f"];
        [editMenuItem setSubmenu:editMenu];
        [menuBar addItem:editMenuItem];
        [editMenu release];
        [editMenuItem release];

        /* View menu */
        NSMenuItem *viewMenuItem = [[NSMenuItem alloc] init];
        NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
        [viewMenu addItemWithTitle:@"Toggle Sidebar" action:@selector(menuToggleSidebar:) keyEquivalent:@"b"];
        [viewMenu addItem:[NSMenuItem separatorItem]];
        [viewMenu addItemWithTitle:@"Bigger Font" action:@selector(menuFontBigger:) keyEquivalent:@"+"];
        [viewMenu addItemWithTitle:@"Smaller Font" action:@selector(menuFontSmaller:) keyEquivalent:@"-"];
        [viewMenu addItemWithTitle:@"Reset Font Size" action:@selector(menuFontReset:) keyEquivalent:@"0"];
        [viewMenu addItem:[NSMenuItem separatorItem]];
        [viewMenu addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
        [[viewMenu itemWithTitle:@"Enter Full Screen"] setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagControl];
        [viewMenuItem setSubmenu:viewMenu];
        [menuBar addItem:viewMenuItem];
        [viewMenu release];
        [viewMenuItem release];

        /* Theme menu */
        NSMenuItem *themeMenuItem = [[NSMenuItem alloc] init];
        NSMenu *themeMenu = [[NSMenu alloc] initWithTitle:@"Theme"];
        /* Built from the live registry, not a second hand-kept list, so the
         * menu offers exactly the themes that are installed. */
        i32 theme_count = 0;
        const char **theme_names = theme_list_names(&theme_count);
        for (i32 ti = 0; ti < theme_count; ti++) {
            NSString *name = [NSString stringWithUTF8String:theme_names[ti]];
            if (!name) continue;
            NSMenuItem *item = [themeMenu addItemWithTitle:name action:@selector(menuSelectTheme:) keyEquivalent:@""];
            [item setRepresentedObject:name];
        }
        [themeMenuItem setSubmenu:themeMenu];
        [menuBar addItem:themeMenuItem];
        [themeMenu release];
        [themeMenuItem release];

        /* Window menu */
        NSMenuItem *windowMenuItem = [[NSMenuItem alloc] init];
        NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
        [windowMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
        [windowMenu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
        [windowMenu addItem:[NSMenuItem separatorItem]];
        [windowMenu addItemWithTitle:@"Bring All to Front" action:@selector(arrangeInFront:) keyEquivalent:@""];
        [windowMenuItem setSubmenu:windowMenu];
        [menuBar addItem:windowMenuItem];
        [NSApp setWindowsMenu:windowMenu];
        [windowMenu release];
        [windowMenuItem release];

        [NSApp setMainMenu:menuBar];
        [menuBar release];

        [NSApp finishLaunching];
    }

    mach_timebase_info(&g_platform.timebase_info);
    g_platform.time_base = (f64)mach_absolute_time() *
        g_platform.timebase_info.numer / g_platform.timebase_info.denom / 1e9;
    g_platform.initialized = true;
    platform_memory_pressure_start();
    return true;
}

void platform_shutdown(void) {
    platform_event_queue_clear(&g_evq);
    platform_destroy_owned_ui();
    [NSApp setWindowsMenu:nil];
    [NSApp setMainMenu:nil];
    [NSApp setDelegate:nil];
    [g_app_delegate release];
    g_app_delegate = nil;
    for (i32 i = 0; i < MAX_SYMBOL_CACHE; i++) {
        if (g_symbol_cache[i].pixels) {
            free(g_symbol_cache[i].pixels);
            g_symbol_cache[i].pixels = NULL;
        }
        g_symbol_cache[i].valid = false;
    }
    g_platform.initialized = false;
}

#ifdef USE_METAL
/* The device a window draws with. On a Mac with two GPUs the default device
 * is the discrete one, and holding it keeps that GPU powered for as long as
 * the window lives; a terminal's frames are light, so the integrated GPU
 * (low power, driving displays, built in) is taken when there is one. A Mac
 * with one GPU gets the default. Retained, like MTLCreateSystemDefaultDevice. */
static id<MTLDevice> liu_window_metal_device(void) {
#if defined(__x86_64__)
    NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
    id<MTLDevice> pick = nil;
    if (all.count > 1) {
        for (id<MTLDevice> d in all) {
            if (d.isLowPower && !d.isHeadless && !d.isRemovable) { pick = [d retain]; break; }
        }
    }
    [all release];
    if (pick) return pick;
#endif
    return MTLCreateSystemDefaultDevice();
}
#endif

PlatformWindow *platform_window_create(const PlatformWindowConfig *cfg) {
    @autoreleasepool {
        PlatformWindow *w = calloc(1, sizeof(PlatformWindow));
        if (!w) return NULL;

        NSRect rect = NSMakeRect(0, 0, cfg->width, cfg->height);
        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable |
                           NSWindowStyleMaskFullSizeContentView;
        if (cfg->resizable) style |= NSWindowStyleMaskResizable;

        w->ns_window = [[NSWindow alloc] initWithContentRect:rect
            styleMask:style
            backing:NSBackingStoreBuffered
            defer:NO];
        [w->ns_window setReleasedWhenClosed:NO];
        /* sessions.json is what brings a window back; AppKit keeps no archive
         * of this one to decode at the next launch. */
        [w->ns_window setRestorable:NO];

        /* Transparent title bar — the content extends into the title bar area */
        [w->ns_window setTitlebarAppearsTransparent:YES];
        [w->ns_window setTitleVisibility:NSWindowTitleHidden];
        NSString *initTitle = cfg->title ? [NSString stringWithUTF8String:cfg->title] : nil;
        [w->ns_window setTitle:(initTitle ? initTitle : @"")];
        [w->ns_window center];
        /* LIU_WINDOW_RECT=x,y,w,h — place the window explicitly, in points from
         * the bottom-left of the main screen, without stealing focus. A window
         * used for measurement must be genuinely on screen (the occlusion gate
         * skips paints for a covered one, so render/present read zero). Debug
         * only; unset = the centred default. */
        {   const char *_wr = liu_dev_env("LIU_WINDOW_RECT");
            double _x, _y, _w, _h;
            if (_wr && sscanf(_wr, "%lf,%lf,%lf,%lf", &_x, &_y, &_w, &_h) == 4 &&
                _w > 100.0 && _h > 100.0) {
                [w->ns_window setFrame:NSMakeRect(_x, _y, _w, _h) display:NO];
            }
        }
        [w->ns_window setAcceptsMouseMovedEvents:YES];

        w->delegate = [[SSHWindowDelegate alloc] init];
        [w->ns_window setDelegate:w->delegate];

#ifdef USE_METAL
        w->view = [[SSHView alloc] initWithFrame:rect];
        [w->view setWantsLayer:YES];
        /* Redraw immediately on bounds change — prevents stale content during resize */
        w->view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
        w->view.layerContentsPlacement = NSViewLayerContentsPlacementTopLeft;
        /* The Metal view lives inside a plain container whose layer carries the
         * corner radius and does the clipping.
         *
         * Two things need that container. A CAMetalLayer's own cornerRadius
         * does not clip its contents (those are the presented drawable), so the
         * radius has to live one level up. And the blur below has to be a
         * SIBLING under the Metal view: AppKit draws subviews above their
         * parent's layer, so a blur placed under a view whose own layer is the
         * Metal layer would cover the whole terminal. */
        w->container = [[NSView alloc] initWithFrame:rect];
        [w->container setWantsLayer:YES];
        {   const char *_cm = getenv("LIU_CORNER_MASK");
            w->container.layer.masksToBounds = (_cm && _cm[0]=='0') ? NO : YES; }
        w->container.layer.backgroundColor = [[NSColor clearColor] CGColor];
        [w->ns_window setContentView:w->container];
        w->view.frame = [w->container bounds];
        w->view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [w->container addSubview:w->view];

        /* Backdrop blur.
         *
         * CABackdropLayer is the piece inside NSVisualEffectView that actually
         * samples what is behind the window; the grey wash people associate
         * with that class comes from the MATERIAL layered on top of it, not
         * from the backdrop. Using the backdrop alone, with nothing but a
         * gaussian filter, gives the blur without the tint, as a layer in
         * Liu's own tree and therefore clipped by the rounded container.
         *
         * Private class, resolved by name: if it ever disappears the layer is
         * simply never created and blur does nothing. */
        {
            Class bd_cls = NSClassFromString(@"CABackdropLayer");
            if (bd_cls) {
                w->backdrop = [[bd_cls alloc] init];
                if (w->backdrop) {
                    w->backdrop.frame = [w->container bounds];
                    w->backdrop.autoresizingMask =
                        kCALayerWidthSizable | kCALayerHeightSizable;
                    w->backdrop.hidden = YES;
                    liu_backdrop_bind_capture(w->backdrop);
                    [w->container.layer insertSublayer:w->backdrop atIndex:0];
                }
            }
        }

        [w->ns_window makeFirstResponder:w->view];
        liu_layout_window_chrome(w->ns_window);
        g_blur_probe_window = w;
        platform_window_register(w);

        w->mtl_device = liu_window_metal_device();
        w->mtl_layer = (CAMetalLayer *)w->view.layer;
        w->mtl_layer.device = w->mtl_device;
        w->mtl_layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
        /* TRIPLE-buffered. With two drawables and vsync, the CPU cannot start
         * frame N+1 until the compositor releases the drawable it is still
         * scanning out, so the loop spends the whole refresh period parked in
         * -nextDrawable, unable to read input or drain a PTY. Time spent there
         * under double buffering is the ceiling, not headroom under it. The
         * first frame that runs long (a translucent window costs several times
         * the GPU time of an opaque one, see app_apply_window_background) then
         * has nothing to absorb it, drops to the next vblank, and the frame rate
         * cascades down.
         *
         * The third drawable costs one more IOSurface per window.
         *
         * The frame ring and its semaphore stay at METAL_FRAME_BUFFER_COUNT (3):
         * the ring slot is only written after nextDrawable returns, so a layer
         * handing out fewer drawables than the ring has slots just paces the
         * loop earlier — it cannot let a frame overwrite a slot the GPU is still
         * reading. LIU_DRAWABLES overrides the count (2 = double buffering). */
        w->mtl_layer.maximumDrawableCount = getenv("LIU_DRAWABLES") ? (NSUInteger)atoi(getenv("LIU_DRAWABLES")) : 3;
        /* framebufferOnly = YES keeps CAMetalLayer's lossless framebuffer
         * compression on the drawables (terminal frames are dominated by
         * flat-color runs, the best case for it) and avoids the per-drawable
         * memory overhead of NO. The only consumer that requires NO is the
         * backdrop-blur capture blit (renderer_blur_capture), which currently
         * has no call site and self-guards against framebufferOnly drawables
         * (falls back to a flat tint). Wiring up blur panels would need NO
         * here. */
        /* LIU_DUMP_FRAME asks the renderer to copy a finished frame out of the
         * drawable (renderer_debug_dump_frame). A framebufferOnly drawable
         * cannot be a blit source, so the diagnostic has to cost the
         * compression to exist at all — hence env-gated rather than always on. */
        w->mtl_layer.framebufferOnly =
            (liu_dev_env("LIU_DUMP_FRAME") || liu_dev_env("LIU_ATLAS_DEMO") ||
             liu_dev_env("LIU_SEL_DEMO_DUMP") || liu_dev_env("LIU_PANEGHOST_DEMO_DUMP") ||
             liu_dev_env("LIU_EDITFOCUS_DEMO_DUMP") ||
             liu_dev_env("LIU_FBMENU_DEMO_DUMP") || liu_dev_env("LIU_VOICE_DEMO_DUMP")) ? NO : YES;
        /* EXPERIMENT (LIU_LAYER_CSPACE=1): give the layer an explicit sRGB
         * colorspace so the compositor has no colour match to perform. */
        if (getenv("LIU_LAYER_CSPACE")) {
            CGColorSpaceRef _cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            if (_cs) { w->mtl_layer.colorspace = _cs; CGColorSpaceRelease(_cs); }
        }
        w->mtl_layer.contentsScale = [w->ns_window backingScaleFactor];
        w->mtl_layer.drawableSize = [w->view convertSizeToBacking:w->view.bounds.size];
        /* Top-left gravity prevents content stretching during resize */
        w->mtl_layer.contentsGravity = kCAGravityTopLeft;
        w->mtl_queue = [w->mtl_device newCommandQueue];
        w->mtl_layer.displaySyncEnabled = cfg->vsync;
        /* Default opaque: prevents the macOS title bar vibrancy effect from
         * bleeding into the tab bar area when the app runs at full opacity. */
        w->mtl_layer.opaque = YES;
        [w->ns_window setOpaque:YES];
        [w->ns_window setBackgroundColor:[NSColor blackColor]];
#else
        /* OpenGL pixel format — 4.1 Core Profile.
         *
         * 4.1 rather than 3.2 because of the shaders, which are shared with the
         * GLX backend and all declare `#version 330 core` with explicit
         * `layout(location = N)` attribute bindings. A 3.2 core context caps
         * GLSL at 1.50 and rejects both.
         *
         * There is no 3.3 to ask for: Apple's profiles go 3.2 straight to 4.1,
         * which is why the version here differs from the GLX side's 3.3
         * request. 4.1 is therefore the lowest profile on this platform that
         * accepts the shaders, and it also brings the two features the renderer
         * needs past 3.2 into core rather than leaving them to extensions:
         * instanced arrays (glVertexAttribDivisor) and sampler objects
         * (renderer_flush_glyphs' nearest/linear split).
         *
         * 4.1 is also Apple's ceiling (the OpenGL stack was frozen there and
         * deprecated in 10.14), so nothing above it is available to ask for. */
        NSOpenGLPixelFormatAttribute attrs[] = {
            NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
            NSOpenGLPFAColorSize,     24,
            NSOpenGLPFAAlphaSize,     8,
            NSOpenGLPFADepthSize,     0,
            NSOpenGLPFAStencilSize,   0,
            NSOpenGLPFADoubleBuffer,
            NSOpenGLPFAAccelerated,
            0
        };
        NSOpenGLPixelFormat *fmt = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];

        w->view = [[SSHView alloc] initWithFrame:rect pixelFormat:fmt];
        [fmt release];
        /* The renderer stores sRGB bytes with GL_FRAMEBUFFER_SRGB. Tag the
         * window accordingly so the compositor can match the display profile. */
        w->ns_window.colorSpace = [NSColorSpace sRGBColorSpace];
        [w->view setWantsBestResolutionOpenGLSurface:YES];
        [w->ns_window setContentView:w->view];
        [w->ns_window makeFirstResponder:w->view];

        w->gl_context = [w->view openGLContext];
        [w->gl_context makeCurrentContext];

        if (cfg->vsync) {
            GLint swapInterval = 1;
            [w->gl_context setValues:&swapInterval forParameter:NSOpenGLContextParameterSwapInterval];
        }
        /* Accessibility and native event routing resolve NSWindow through the
         * shared registry, just as the Metal view does. Register before showing
         * the window so its first delegate callbacks can find their owner. */
        platform_window_register(w);
#endif

        /* Before it is ordered in, so not one frame of it is seen: whoever
         * asked for this (the first-run intro) brings the alpha back. */
        if (cfg->start_transparent) [w->ns_window setAlphaValue:0.0];
        /* LIU_WINDOW_UNSEEN=1 — a window for automation that nobody is to look
         * at: wholly transparent, without a shadow, and no click of the user's
         * lands on it. "Behind every other window" (LIU_NO_ACTIVATE) still shows
         * wherever the desktop does, and a window cannot be put off the screen
         * instead: AppKit pulls it back when it is ordered in (below). The app
         * draws as ever (the frame a dump writes is its own); only the
         * compositor leaves it out. */
        if (liu_dev_env("LIU_WINDOW_UNSEEN")) {
            [w->ns_window setAlphaValue:0.0];
            [w->ns_window setHasShadow:NO];
            [w->ns_window setIgnoresMouseEvents:YES];
        }
        /* Also before it is ordered in: a window born under the pointer (a tab
         * torn off mid-drag) must not be seen centred for a frame first. */
        if (cfg->has_origin) {
            NSScreen *main_screen = [[NSScreen screens] firstObject];
            if (main_screen) {
                NSRect sf = [main_screen frame];
                NSRect wf = [w->ns_window frame];
                CGFloat ns_y = sf.origin.y + sf.size.height
                             - (CGFloat)cfg->origin_y - wf.size.height;
                [w->ns_window setFrameOrigin:NSMakePoint((CGFloat)cfg->origin_x, ns_y)];
            }
        }
        if (liu_dev_env("LIU_NO_ACTIVATE")) [w->ns_window orderBack:nil];
        else                           [w->ns_window makeKeyAndOrderFront:nil];
        /* ...and once more after: ordering a window in for the first time lets
         * AppKit pull it wholly onto the screen. That is the right thing for a
         * window opened from a menu and the wrong one here: the pointer is
         * holding this window by its strip, and the strip would be moved away
         * from it. A plain frame set is not constrained. */
        if (cfg->has_origin) {
            NSScreen *main_screen = [[NSScreen screens] firstObject];
            if (main_screen) {
                NSRect sf = [main_screen frame];
                NSRect wf = [w->ns_window frame];
                CGFloat ns_y = sf.origin.y + sf.size.height
                             - (CGFloat)cfg->origin_y - wf.size.height;
                [w->ns_window setFrameOrigin:NSMakePoint((CGFloat)cfg->origin_x, ns_y)];
            }
        }
        return w;
    }
}

void platform_window_destroy(PlatformWindow *w) {
    if (!w) return;
    /* The quake hotkey names the window it toggles: a closed one is dropped
     * with its monitors, or Ctrl+` reached freed memory and stayed swallowed
     * app-wide. */
    if (g_global_hotkey.userdata == (void *)w) {
        @autoreleasepool {
            if (g_global_hotkey.global_monitor) [NSEvent removeMonitor:g_global_hotkey.global_monitor];
            if (g_global_hotkey.local_monitor)  [NSEvent removeMonitor:g_global_hotkey.local_monitor];
        }
        g_global_hotkey.global_monitor = nil;
        g_global_hotkey.local_monitor = nil;
        g_global_hotkey.callback = NULL;
        g_global_hotkey.userdata = NULL;
    }
    /* Before anything else frees the window: the CoreVideo callback holds `w`
     * and runs on its own thread, so it has to be stopped while `w` is still
     * whole. */
    platform_window_display_link_teardown(w);
    platform_window_unregister(w);
    liu_a11y_window_teardown(w);
    if (g_blur_probe_window == w) g_blur_probe_window = NULL;
    @autoreleasepool {
        /* FIRST, before anything can pump the runloop: the live-resize timer is
         * scheduled in NSRunLoopCommonModes with target:self, which means the
         * RUNLOOP holds a strong reference to the view. Releasing the view
         * below therefore does not stop the timer — it keeps the view alive and
         * keeps firing the render hook at 60 Hz forever, now into a destroyed
         * window. -viewDidEndLiveResize is the only other place that
         * invalidates, and a window closed mid-drag never gets one. */
        if (w->view && w->view->_liveResizeTimer) {
            [w->view->_liveResizeTimer invalidate];
            w->view->_liveResizeTimer = nil;
        }
        /* Only THIS window's toast. Destroying the global one tore another
         * window's toast out of its superview and deallocated it. */
        platform_toast_destroy_for(w);
        /* Quake state dies with the window instead of latching "enabled"
         * forever — an in-flight slide animation checks the registry
         * (platform_window_alive) before touching these again. */
        w->quake_enabled = w->quake_visible = w->quake_animating = false;
        [w->ns_window setDelegate:nil];
        [w->ns_window setContentView:nil];
#ifdef USE_METAL
        [w->mtl_queue release];
        w->mtl_queue = nil;
        [w->mtl_device release];
        w->mtl_device = nil;
#endif
        [w->view release];
        w->view = nil;
#ifdef USE_METAL
        /* Both only exist in the Metal build — the container is what carries
         * the corner radius around the CAMetalLayer, and the backdrop is the
         * blur beneath it. Released after `view`, not with the queue/device
         * above, because the container OWNS `view`. */
        [w->backdrop release];
        w->backdrop = nil;
        [w->container release];
        w->container = nil;
#endif
        [w->delegate release];
        w->delegate = nil;
        [w->ns_window close];
        [w->ns_window release];
        w->ns_window = nil;
        free(w);
    }
}

void platform_window_set_title(PlatformWindow *w, const char *title) {
    @autoreleasepool {
        /* +stringWithUTF8String: returns nil on a NULL pointer OR invalid
         * UTF-8, and -[NSWindow setTitle:] throws on nil. An OSC 0/2 title from
         * untrusted output (e.g. `cat`-ing a binary file injects arbitrary
         * bytes) is exactly that case, so fall back to an empty string. */
        NSString *ns = title ? [NSString stringWithUTF8String:title] : nil;
        [w->ns_window setTitle:(ns ? ns : @"")];
        /* setTitle: makes AppKit re-lay the titlebar, which snaps the standard
         * window buttons back to their default (too-high) origin, visible as
         * the traffic lights jumping up when the active tab changes. Re-apply
         * the centering: once now, and once on the next runloop tick because
         * the reset lands on a later layout pass. */
        NSWindow *win = w->ns_window;
        liu_center_traffic_lights(win);
        dispatch_async(dispatch_get_main_queue(), ^{ liu_center_traffic_lights(win); });
        /* setTitle: makes AppKit re-lay the window's theme frame, so it is a
         * candidate for disturbing the backdrop — and it fires on its own
         * schedule (agent chat titles, cwd changes), which matches "the blur
         * goes by itself". Logged so the timeline can rule it in or out. */
        platform_blur_debug_sample(w, "setTitle");
    }
}

/* The window's opaque state, in one place and applied at most once per real
 * change. Both entry points below want exactly this, and both are called on
 * every appearance apply — repeating a setOpaque:/setBackgroundColor: pair the
 * window already has costs a window-server round trip and makes the compositor
 * re-evaluate the window's backing, which the backdrop blur is sampled from. */
static void liu_window_apply_opaque(PlatformWindow *w, bool opaque) {
    if (!w || !w->ns_window) return;
    if (w->opaque_applied_valid && w->opaque_applied == opaque) {
        platform_blur_debug_note("set_opaque(%d) skipped (already applied)", opaque);
        return;
    }
    platform_blur_debug_note("set_opaque(%d) applying", opaque);
    w->opaque_applied_valid = true;
    w->opaque_applied = opaque;
    [w->ns_window setOpaque:(opaque ? YES : NO)];
    [w->ns_window setBackgroundColor:(opaque ? [NSColor blackColor]
                                             : [NSColor clearColor])];
#ifdef USE_METAL
    w->mtl_layer.opaque = opaque ? YES : NO;
#endif
}

bool platform_window_set_content_opacity(PlatformWindow *w, f32 opacity) {
#ifndef USE_METAL
    /* The layer opacity IS the CAMetalLayer's. An NSOpenGLContext draws through
     * the view's surface with no layer of ours to address, so this backend
     * cannot honour the request — and `false` is the documented "not supported"
     * answer the caller already handles by falling back to the clear alpha. */
    (void)w; (void)opacity;
    return false;
#else
    if (!w || !w->mtl_layer) return false;
    @autoreleasepool {
        if (opacity < 0.0f) opacity = 0.0f;
        if (opacity > 1.0f) opacity = 1.0f;
        if (fabsf(w->mtl_layer.opacity - opacity) > 0.001f) {
            /* Implicit CA animations would ramp the opacity over 0.25s on every
             * assignment — harmless once, but this runs per frame, so the layer
             * would spend its life chasing a value it already has. */
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            w->mtl_layer.opacity = opacity;
            [CATransaction commit];
        }
    }
    return true;
#endif
}

void platform_window_set_transparent(PlatformWindow *w, bool transparent) {
    @autoreleasepool {
        liu_window_apply_opaque(w, !transparent);

        /* NO NSVisualEffectView here. AppKit draws SUBVIEWS ON TOP OF their
         * parent's layer, so an effect view added here would cover the entire
         * terminal: lowering opacity would blank the window to the material's flat
         * grey, and that material also reads as an unrequested blur.
         *
         * Blur comes solely from platform_window_set_blur, a backdrop layer under
         * the Metal view that puts nothing in front of the content, and only when
         * blur is turned on. */
    }
}

/* =========================================================================
 * Blur diagnostics — see platform.h for the contract.
 * ========================================================================= */

typedef struct {
    bool    valid;
    bool    has_backdrop;
    bool    hidden;
    int     filter_count;
    double  filter_radius;
    int     layer_index;      /* backdrop's position among the container's sublayers */
    int     sublayer_count;
    double  frame[4];
    double  bounds[4];        /* the container's, for the frame comparison */
    bool    win_opaque;
    bool    layer_opaque;
    double  win_alpha;
    bool    occluded;
    bool    key;
    bool    visible;
    bool    miniaturized;
    bool    fullscreen;
    bool    masks;
    double  corner_radius;
    double  backing_scale;
    void   *screen;
    /* Capture binding — the properties liu_backdrop_bind_capture() sets. If the
     * blur ever drops again, these say whether the binding itself came undone. */
    bool    bd_enabled;
    bool    bd_ws_aware;
    bool    bd_has_group;
    double  bd_scale;
} BlurSnapshot;

static BlurSnapshot g_blur_prev;
static f64          g_blur_t0;
static int          g_blur_enabled = -1;   /* -1 = not yet read from the env */

bool platform_blur_debug_enabled(void) {
    if (g_blur_enabled < 0) {
        const char *e = liu_dev_env("LIU_BLUR_DEBUG");
        g_blur_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
        if (g_blur_enabled) {
            g_blur_t0 = platform_time_sec();
            fprintf(stderr, "[blur] probe on — fields log only when they change\n");
        }
    }
    return g_blur_enabled == 1;
}

void platform_blur_debug_note(const char *fmt, ...) {
    if (!platform_blur_debug_enabled()) return;
    fprintf(stderr, "[blur] %7.3f ", platform_time_sec() - g_blur_t0);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void platform_blur_debug_sample(PlatformWindow *w, const char *tag) {
    if (!platform_blur_debug_enabled()) return;
    if (!w || !w->ns_window) return;
    if (!tag) tag = "?";

    BlurSnapshot s;
    memset(&s, 0, sizeof s);
    s.valid = true;
#ifdef USE_METAL
    NSView *cv = [w->ns_window contentView];
    s.has_backdrop = (w->backdrop != nil);
    s.layer_index = -1;
    if (cv && cv.layer) {
        s.sublayer_count = (int)cv.layer.sublayers.count;
        for (int i = 0; i < s.sublayer_count; i++) {
            if (cv.layer.sublayers[(NSUInteger)i] == w->backdrop) { s.layer_index = i; break; }
        }
        NSRect b = cv.bounds;
        s.bounds[0] = b.origin.x; s.bounds[1] = b.origin.y;
        s.bounds[2] = b.size.width; s.bounds[3] = b.size.height;
        s.masks = cv.layer.masksToBounds;
        s.corner_radius = cv.layer.cornerRadius;
    }
    if (w->backdrop) {
        s.hidden = w->backdrop.hidden;
        s.filter_count = (int)[w->backdrop.filters count];
        if (s.filter_count > 0) {
            id f = w->backdrop.filters[0];
            id v = [f valueForKey:@"inputRadius"];
            s.filter_radius = [v respondsToSelector:@selector(doubleValue)]
                            ? [v doubleValue] : -1.0;
        }
        CGRect fr = w->backdrop.frame;
        s.frame[0] = fr.origin.x; s.frame[1] = fr.origin.y;
        s.frame[2] = fr.size.width; s.frame[3] = fr.size.height;

        /* Private keys — valueForKey: raises on an unknown one, and Liu's
         * uncaught handler turns that into a crash, so ask first. */
        if ([w->backdrop respondsToSelector:@selector(isEnabled)])
            s.bd_enabled = [[w->backdrop valueForKey:@"enabled"] boolValue];
        if ([w->backdrop respondsToSelector:@selector(windowServerAware)])
            s.bd_ws_aware = [[w->backdrop valueForKey:@"windowServerAware"] boolValue];
        if ([w->backdrop respondsToSelector:@selector(groupName)])
            s.bd_has_group = [w->backdrop valueForKey:@"groupName"] != nil;
        if ([w->backdrop respondsToSelector:@selector(scale)])
            s.bd_scale = [[w->backdrop valueForKey:@"scale"] doubleValue];
    }
    s.layer_opaque = w->mtl_layer ? w->mtl_layer.opaque : false;
#endif
    s.win_opaque   = [w->ns_window isOpaque];
    s.win_alpha    = [w->ns_window alphaValue];
    s.occluded     = ([w->ns_window occlusionState] & NSWindowOcclusionStateVisible) == 0;
    s.key          = [w->ns_window isKeyWindow];
    s.visible      = [w->ns_window isVisible];
    s.miniaturized = [w->ns_window isMiniaturized];
    s.fullscreen   = ([w->ns_window styleMask] & NSWindowStyleMaskFullScreen) != 0;
    s.backing_scale = [w->ns_window backingScaleFactor];
    s.screen       = (__bridge void *)[w->ns_window screen];

    BlurSnapshot *p = &g_blur_prev;
    if (!p->valid) {
        platform_blur_debug_note(
            "[%s] baseline backdrop=%d hidden=%d filters=%d radius=%.1f "
            "idx=%d/%d frame=%.0fx%.0f bounds=%.0fx%.0f winOpaque=%d "
            "layerOpaque=%d alpha=%.2f occluded=%d key=%d fs=%d masks=%d "
            "corner=%.1f scale=%.1f | bdEnabled=%d wsAware=%d group=%d bdScale=%.3f",
            tag, s.has_backdrop, s.hidden, s.filter_count, s.filter_radius,
            s.layer_index, s.sublayer_count, s.frame[2], s.frame[3],
            s.bounds[2], s.bounds[3], s.win_opaque, s.layer_opaque,
            s.win_alpha, s.occluded, s.key, s.fullscreen, s.masks,
            s.corner_radius, s.backing_scale,
            s.bd_enabled, s.bd_ws_aware, s.bd_has_group, s.bd_scale);
        *p = s;
        return;
    }

    /* Only the deltas. A per-frame call on a quiet window prints nothing. */
    char buf[1024];
    int n = 0;
    #define DIFF_I(field, name) \
        if (p->field != s.field && n < (int)sizeof buf - 64) \
            n += snprintf(buf + n, sizeof buf - (usize)n, " %s %d->%d", name, \
                          (int)p->field, (int)s.field)
    #define DIFF_F(field, name) \
        if (fabs(p->field - s.field) > 0.01 && n < (int)sizeof buf - 64) \
            n += snprintf(buf + n, sizeof buf - (usize)n, " %s %.2f->%.2f", name, \
                          p->field, s.field)

    DIFF_I(has_backdrop, "backdrop");
    DIFF_I(hidden,       "hidden");
    DIFF_I(filter_count, "filters");
    DIFF_F(filter_radius,"radius");
    DIFF_I(layer_index,  "layerIdx");
    DIFF_I(sublayer_count, "sublayers");
    DIFF_I(win_opaque,   "winOpaque");
    DIFF_I(layer_opaque, "layerOpaque");
    DIFF_F(win_alpha,    "winAlpha");
    DIFF_I(occluded,     "occluded");
    DIFF_I(key,          "key");
    DIFF_I(visible,      "visible");
    DIFF_I(miniaturized, "mini");
    DIFF_I(fullscreen,   "fullscreen");
    DIFF_I(masks,        "masks");
    DIFF_F(corner_radius,"corner");
    DIFF_F(backing_scale,"scale");
    DIFF_I(bd_enabled,   "bdEnabled");
    DIFF_I(bd_ws_aware,  "wsAware");
    DIFF_I(bd_has_group, "bdGroup");
    DIFF_F(bd_scale,     "bdScale");
    if (p->screen != s.screen && n < (int)sizeof buf - 64)
        n += snprintf(buf + n, sizeof buf - (usize)n, " screen %p->%p",
                      p->screen, s.screen);
    /* The blur is drawn from this rect; a stale one shows unblurred edges. */
    if ((fabs(p->frame[2] - s.frame[2]) > 0.5 ||
         fabs(p->frame[3] - s.frame[3]) > 0.5) && n < (int)sizeof buf - 64)
        n += snprintf(buf + n, sizeof buf - (usize)n, " frame %.0fx%.0f->%.0fx%.0f",
                      p->frame[2], p->frame[3], s.frame[2], s.frame[3]);
    /* Layer frame vs container bounds: a mismatch means part of the window is
     * not being blurred at all, which reads exactly like "the blur went away". */
    bool was_fit = fabs(p->frame[2] - p->bounds[2]) < 1.0 &&
                   fabs(p->frame[3] - p->bounds[3]) < 1.0;
    bool now_fit = fabs(s.frame[2] - s.bounds[2]) < 1.0 &&
                   fabs(s.frame[3] - s.bounds[3]) < 1.0;
    if (was_fit != now_fit && n < (int)sizeof buf - 64)
        n += snprintf(buf + n, sizeof buf - (usize)n, " coversWindow %d->%d",
                      was_fit, now_fit);

    #undef DIFF_I
    #undef DIFF_F

    if (n > 0) platform_blur_debug_note("[%s]%s", tag, buf);
    *p = s;
}

PlatformWindow *platform_key_window(void) {
    @autoreleasepool {
        PlatformWindow *w = platform_window_for_ns([NSApp keyWindow]);
        if (!w) w = platform_window_for_ns([NSApp mainWindow]);
        return w;
    }
}

#ifndef USE_METAL
/* No backdrop blur in the OpenGL build, and that is structural rather than a
 * gap waiting to be filled. The whole arrangement below hangs off the Metal
 * layer: the backdrop is a sublayer of the container that rounds the
 * CAMetalLayer, and AppKit draws subviews above their parent's layer, so an
 * NSOpenGLContext — which has no layer of ours to sit under — has nowhere to
 * put it that is not on top of the terminal. `blur_is_exact` answering false
 * is what tells the UI to stop offering a radius it cannot render. */
bool platform_window_blur_is_exact(void) { return false; }
void platform_window_set_blur(PlatformWindow *w, f32 radius) { (void)w; (void)radius; }
#else

bool platform_window_blur_is_exact(void) {
    /* A gaussian radius in points, not a material preset. */
    return NSClassFromString(@"CABackdropLayer") != nil &&
           NSClassFromString(@"CAFilter") != nil;
}

/* Does the backdrop still LOOK the way the memo claims it was left?
 *
 * The memo records what was last ASKED for, not what survived. Anything that
 * hides the layer, drops the filter or reorders the container's sublayers (a
 * style-mask change, an AppKit relayout, a subview inserted below) leaves the
 * memo asserting a blur that is no longer on screen, and from then on every
 * apply would short-circuit and the blur would never come back.
 *
 * All of these are local CALayer property reads: no window-server round trip,
 * so it is cheap enough to run on every apply. */
static bool liu_blur_state_intact(PlatformWindow *w, f32 radius) {
    if (!w->backdrop) return false;                   /* layer still there */
    BOOL want_hidden = (radius < 0.5f) ? YES : NO;
    if (w->backdrop.hidden != want_hidden) return false;
    /* Exactly the one gaussian installed (set to nil when blur is off). */
    NSUInteger want_filters = want_hidden ? 0u : 1u;
    if ([w->backdrop.filters count] != want_filters) return false;
    /* Still the BOTTOM sublayer: anything below it draws behind the blur, and
     * anything that displaced it puts the blur over the terminal. */
    NSView *cv = [w->ns_window contentView];
    if (!cv || !cv.layer || cv.layer.sublayers.count == 0) return false;
    if (cv.layer.sublayers[0] != w->backdrop) return false;
    return true;
}

/* Backdrop blur — a real gaussian radius, drawn by CoreAnimation.
 *
 * The window server can do this too (CGSSetWindowBackgroundBlurRadius), but it
 * blurs the window's frame RECTANGLE: the rounded corners sit inside that
 * rectangle, so a square patch of blur would show through them. That region
 * belongs to the server, and nothing on the client's side (layer masks,
 * container clipping, invalidateShadow, even handing it a rounded shape) moves
 * it.
 *
 * Doing the blur as a layer puts the clipping in Liu's hands: the backdrop is
 * a sublayer of the rounded container, so CoreAnimation clips it with
 * everything else. The filter is a plain gaussian with no material on top, so
 * the colours underneath are untouched, which NSVisualEffectView's material
 * does not allow.
 *
 * Both CABackdropLayer and CAFilter are private, resolved by name. If either
 * is missing the blur silently does nothing; the terminal is unaffected.
 */
void platform_window_set_blur(PlatformWindow *w, f32 radius) {
    if (!w || !w->ns_window) return;
    @autoreleasepool {
        if (radius < 0.0f)  radius = 0.0f;
        if (radius > 64.0f) radius = 64.0f;

        /* Already showing exactly this blur AND the layer still agrees: leave
         * it alone. Handing CoreAnimation a fresh CAFilter for the same radius
         * is not free — the backdrop drops its rendered content and
         * re-captures, which is the blink users see when an unrelated setting
         * is saved.
         *
         * The state check is what keeps that memo from becoming a one-way
         * door: without it, a single disturbance of the layer is PERMANENT,
         * because every later apply matches the memo and returns. */
        if (w->blur_applied_valid && w->blur_applied == radius &&
            liu_blur_state_intact(w, radius)) {
            platform_blur_debug_note("set_blur(%.1f) skipped (already applied)",
                                     (double)radius);
            return;
        }
        platform_blur_debug_note("set_blur(%.1f) applying (was %s%.1f)",
                                 (double)radius,
                                 w->blur_applied_valid ? "" : "unset:",
                                 (double)w->blur_applied);

        liu_layout_window_chrome(w->ns_window);
        if (!w->backdrop) return;
        /* Put the backdrop back at the bottom if anything displaced it.
         * Detecting that in liu_blur_state_intact() is not enough: without a
         * repair the state never converges and every apply rebuilds the filter,
         * so the blur would blink on every apply. -insertSublayer:atIndex:
         * MOVES an existing sublayer (it is removed from its superlayer first),
         * so this cannot duplicate it. */
        {
            NSView *cv = [w->ns_window contentView];
            if (cv && cv.layer &&
                (cv.layer.sublayers.count == 0 ||
                 cv.layer.sublayers[0] != w->backdrop)) {
                [CATransaction begin];
                [CATransaction setDisableActions:YES];
                [cv.layer insertSublayer:w->backdrop atIndex:0];
                [CATransaction commit];
            }
        }
        w->blur_applied_valid = true;
        w->blur_applied = radius;

        if (radius < 0.5f) {
            w->backdrop.hidden = YES;
            w->backdrop.filters = nil;
            /* Sample here too, not just on the apply path below: turning blur
             * OFF is the one state change the probe would otherwise attribute
             * to whatever ran next. */
            platform_blur_debug_sample(w, "set_blur:off");
            return;
        }

        Class filter_cls = NSClassFromString(@"CAFilter");
        if (!filter_cls) { w->backdrop.hidden = YES; return; }
        id blur = [filter_cls performSelector:@selector(filterWithName:)
                                   withObject:@"gaussianBlur"];
        if (!blur) { w->backdrop.hidden = YES; return; }
        [blur setValue:@(radius) forKey:@"inputRadius"];
        /* Without this the filter samples past its own bounds and the edges of
         * the window fade out instead of staying solid. */
        [blur setValue:@YES forKey:@"inputNormalizeEdges"];

        /* Layer property changes animate by default; a blur that eases in over
         * a quarter second every time the setting is touched reads as lag. */
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        w->backdrop.filters = @[blur];
        w->backdrop.hidden  = NO;
        [CATransaction commit];
        platform_blur_debug_sample(w, "set_blur");
    }
}
#endif  /* USE_METAL — backdrop blur */

/* There is no platform_window_set_opacity() here on purpose. It set the exact
 * same flag as platform_window_set_transparent() — the window's `opaque`,
 * because the translucency is RENDERED (the clear's alpha), never
 * NSWindow.alphaValue — and its last caller went away when
 * app_apply_window_background() started pushing the state once. Keeping a
 * second entry point only invites someone to "restore the ordering" and get
 * two window-server writes plus a compositor re-evaluation per apply, which
 * the backdrop blur is sampled from. Use platform_window_set_transparent. */

void platform_window_set_vsync(PlatformWindow *w, bool enabled) {
    if (!w) return;
#ifdef USE_METAL
    if (w->mtl_layer) {
        w->mtl_layer.displaySyncEnabled = enabled ? YES : NO;
    }
#else
    if (w->gl_context) {
        GLint interval = enabled ? 1 : 0;
        [w->gl_context setValues:&interval forParameter:NSOpenGLContextParameterSwapInterval];
    }
#endif
}

void platform_window_set_presents_with_transaction(PlatformWindow *w, bool enabled) {
    if (!w) return;
#ifdef USE_METAL
    if (w->mtl_layer) {
        w->mtl_layer.presentsWithTransaction = enabled ? YES : NO;
    }
#else
    (void)w; (void)enabled;
#endif
}

void platform_window_get_size(PlatformWindow *w, i32 *width, i32 *height) {
    NSRect frame = [[w->ns_window contentView] frame];
    *width  = (i32)frame.size.width;
    *height = (i32)frame.size.height;
}

void platform_window_get_framebuffer_size(PlatformWindow *w, i32 *width, i32 *height) {
    NSRect fb = [w->view convertRectToBacking:[w->view bounds]];
    *width  = (i32)fb.size.width;
    *height = (i32)fb.size.height;
}

f32 platform_window_get_dpi_scale(PlatformWindow *w) {
    return (f32)[w->ns_window backingScaleFactor];
}

/* ===========================================================================
 * Display link — follow the panel instead of modelling it
 *
 * The one that is easy to get wrong is the
 * first: this callback runs on a CoreVideo thread and MUST NOT draw. It bumps
 * a counter and posts the same wake-up the fd watches use, so the frame is
 * still built on the main thread.
 * ======================================================================== */

static CGDirectDisplayID window_display_id(PlatformWindow *w) {
    if (!w || !w->ns_window) return 0;
    NSScreen *screen = [w->ns_window screen];
    if (!screen) return 0;
    NSNumber *num = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
    return num ? (CGDirectDisplayID)[num unsignedIntValue] : 0;
}

/* Host time after which the demo link stops ticking (0 = never). */
static _Atomic uint64_t g_link_freeze_host;
static bool             g_link_freeze_stays;   /* "N!": a restart does not cure it */

static void link_forget_cadence(PlatformWindow *w) {
    atomic_store_explicit(&w->link_last_host_time, 0, memory_order_release);
    atomic_store_explicit(&w->link_period_ticks, 0, memory_order_release);
    for (i32 i = 0; i < 4; i++)
        atomic_store_explicit(&w->link_gaps[i], 0, memory_order_relaxed);
    /* A link re-bound while RUNNING has no last tick any more, so its silence
     * would be counted from whenever it was started — minutes ago — and read
     * as a dead link on the very next sync. Count it from here instead. */
    if (atomic_load_explicit(&w->link_started_host, memory_order_acquire))
        atomic_store_explicit(&w->link_started_host, mach_absolute_time(), memory_order_release);
}

static CVReturn liu_display_link_cb(CVDisplayLinkRef link,
                                    const CVTimeStamp *now,
                                    const CVTimeStamp *out,
                                    CVOptionFlags in_flags,
                                    CVOptionFlags *out_flags,
                                    void *ud) {
    (void)link; (void)out; (void)in_flags; (void)out_flags;
    PlatformWindow *w = (PlatformWindow *)ud;
    if (!w) return kCVReturnSuccess;
    /* LIU_LINK_FREEZE_DEMO: the link stays "running" and stops ticking — the
     * state a long-lived window can be left in and a fresh one never is. */
    u64 freeze = atomic_load_explicit(&g_link_freeze_host, memory_order_acquire);
    if (freeze && mach_absolute_time() >= freeze) return kCVReturnSuccess;
    if (now && now->hostTime) {
        u64 previous = atomic_exchange_explicit(&w->link_last_host_time,
                                                 now->hostTime, memory_order_relaxed);
        if (previous && now->hostTime > previous) {
            u32 slot = atomic_fetch_add_explicit(&w->link_gap_n, 1, memory_order_relaxed) & 3u;
            atomic_store_explicit(&w->link_gaps[slot], now->hostTime - previous,
                                  memory_order_relaxed);
            u64 gaps[4];
            for (i32 i = 0; i < 4; i++)
                gaps[i] = atomic_load_explicit(&w->link_gaps[i], memory_order_relaxed);
            atomic_store_explicit(&w->link_period_ticks, frame_pacing_min_gap(gaps, 4),
                                  memory_order_release);
        }
    }
    atomic_fetch_add_explicit(&w->vsync_ticks, 1, memory_order_release);
    /* Wake the loop, do not draw. Deliberately NOT platform_wake_event_loop():
     * that counts as an fd fire and would make every vblank run a session
     * poll. A vblank says "you may present", not "there are bytes". */
    watch_post_wake_event();
    return kCVReturnSuccess;
}

static void platform_window_mark_link_rebind(PlatformWindow *w) {
    if (w) atomic_store_explicit(&w->link_rebind, true, memory_order_release);
}

void platform_window_display_link_sync(PlatformWindow *w, bool visible,
                                       bool focused, bool wants_frames) {
    if (!w || !w->ns_window) return;

    if (!w->display_link) {
        CVDisplayLinkRef link = NULL;
        /* A locked session can have no active displays; that is a temporary
         * failure, not an error: the window stays on the software grid and the
         * next sync retries. The platform logs and falls back the same way. */
        if (CVDisplayLinkCreateWithActiveCGDisplays(&link) != kCVReturnSuccess || !link)
            return;
        if (CVDisplayLinkSetOutputCallback(link, liu_display_link_cb, w) != kCVReturnSuccess) {
            CVDisplayLinkRelease(link);
            return;
        }
        w->display_link  = link;
        w->link_display  = 0;
        atomic_store_explicit(&w->link_rebind, true, memory_order_release);
        link_install_display_observers();
        const char *fz = liu_dev_env("LIU_LINK_FREEZE_DEMO");
        if (fz && fz[0] && !atomic_load_explicit(&g_link_freeze_host, memory_order_acquire)) {
            f64 after = strtod(fz, NULL);
            if (after >= 0.5 && after <= 600.0) {
                g_link_freeze_stays = strchr(fz, '!') != NULL;
                u64 ticks = (u64)(after * 1e9 * g_platform.timebase_info.denom /
                                  g_platform.timebase_info.numer);
                atomic_store_explicit(&g_link_freeze_host, mach_absolute_time() + ticks,
                                      memory_order_release);
            }
        }
    }

    bool running_now = CVDisplayLinkIsRunning(w->display_link) != 0;

    /* The displays changed under the link (a monitor plugged or unplugged, a
     * resolution or scaling change, the lid, a wake from sleep). The id the
     * window reports is often the SAME afterwards, so "re-bind when the id
     * differs" never fired for any of these — and nothing else looked. Tear
     * the binding down and make it again. */
    if (atomic_exchange_explicit(&w->link_refresh, false, memory_order_acq_rel)) {
        if (running_now) CVDisplayLinkStop(w->display_link);
        atomic_store_explicit(&w->link_started_host, 0, memory_order_release);
        w->link_display = 0;
        atomic_store_explicit(&w->link_rebind, true, memory_order_release);
        running_now = false;
        if (liu_dev_env("LIU_FRAME_PROBE")) fprintf(stderr, "[link] refresh (displays changed or wake)\n");
    }

    /* A link that is "running" and has said nothing for a second is not going
     * to: stop it and bind it again. The paint gate stopped waiting for it long
     * before this (frame_pacing_link_live), so the restart is about getting
     * the panel's own cadence back, not about unblocking frames. At most once
     * every five seconds. */
    if (running_now && visible && focused && wants_frames) {
        f64 quiet = platform_window_display_link_silence_sec(w);
        f64 t = platform_time_sec();
        if (quiet > 1.0 && t >= w->link_restart_after) {
            CVDisplayLinkStop(w->display_link);
            atomic_store_explicit(&w->link_started_host, 0, memory_order_release);
            w->link_display = 0;
            atomic_store_explicit(&w->link_rebind, true, memory_order_release);
            w->link_restart_after = t + 5.0;
            if (!g_link_freeze_stays)
                atomic_store_explicit(&g_link_freeze_host, 0, memory_order_release);
            if (liu_dev_env("LIU_FRAME_PROBE"))
                fprintf(stderr, "[link] restart (silent for %.1f s)\n", quiet);
        }
    }

    /* The request is CLEARED ONLY WHEN THE REBIND ACTUALLY HAPPENED.
     *
     * A window_display_id() of 0 (a locked session, or a window mid-move
     * between screens) or a transient CVDisplayLinkSetCurrentCGDisplay failure
     * must not consume the request: the link would stay pinned to the PREVIOUS
     * display's cadence with nothing left to ask again until the window moved a
     * second time. Both the flag and this sync run on the main thread, so the
     * load/store pair cannot lose a concurrent set; the atomics are for
     * windowDidChangeScreen:, which is on the same thread but reached through
     * AppKit. */
    if (atomic_load_explicit(&w->link_rebind, memory_order_acquire)) {
        CGDirectDisplayID id = window_display_id(w);
        if (id == 0) {
            /* No display to bind to yet. Keep the request; this runs once per
             * frame, so the next sync retries. */
        } else if (id == w->link_display) {
            atomic_store_explicit(&w->link_rebind, false, memory_order_release);
        } else if (CVDisplayLinkSetCurrentCGDisplay(w->display_link, id) ==
                   kCVReturnSuccess) {
            w->link_display = id;
            link_forget_cadence(w);
            atomic_store_explicit(&w->link_rebind, false, memory_order_release);
        }
        /* else: a transient CV failure — the request stands. */
    }

    /* Visible AND focused, plus `wants_frames`, because unlike a renderer with
     * a frame-level damage gate and a render thread,
     * so a link ticking at an idle window would wake the MAIN loop 60-120
     * times a second to produce nothing. See platform.h. */
    bool want = visible && focused && wants_frames;
    bool running = CVDisplayLinkIsRunning(w->display_link) != 0;
    if (want && !running) {
        /* The gap across a stop is not a period, and silence is counted from
         * here until the first tick arrives. */
        atomic_store_explicit(&w->link_last_host_time, 0, memory_order_release);
        atomic_store_explicit(&w->link_started_host, mach_absolute_time(), memory_order_release);
        CVDisplayLinkStart(w->display_link);
        if (liu_dev_env("LIU_FRAME_PROBE")) {
            /* The period reads 0 until the link has actually run once, so this
             * prints "?" on the first start rather than inf. */
            double per = CVDisplayLinkGetActualOutputVideoRefreshPeriod(w->display_link);
            if (per > 0.0)
                fprintf(stderr, "[link] start display=%u actual=%.1f Hz\n",
                        (unsigned)w->link_display, 1.0 / per);
            else
                fprintf(stderr, "[link] start display=%u actual=? (not run yet)\n",
                        (unsigned)w->link_display);
        }
    } else if (!want && running) {
        CVDisplayLinkStop(w->display_link);
        atomic_store_explicit(&w->link_started_host, 0, memory_order_release);
        if (liu_dev_env("LIU_FRAME_PROBE"))
            fprintf(stderr, "[link] stop (vis=%d foc=%d want=%d)\n",
                    (int)visible, (int)focused, (int)wants_frames);
    }
}

bool platform_window_display_link_running(PlatformWindow *w) {
    if (!w || !w->display_link) return false;
    return CVDisplayLinkIsRunning(w->display_link) != 0;
}

u64 platform_window_vsync_ticks(PlatformWindow *w) {
    if (!w) return 0;
    return (u64)atomic_load_explicit(&w->vsync_ticks, memory_order_acquire);
}

f64 platform_window_display_link_period_sec(PlatformWindow *w) {
    if (!w) return 0.0;
    u64 ticks = atomic_load_explicit(&w->link_period_ticks, memory_order_acquire);
    if (!ticks) return 0.0;
    f64 seconds = (f64)ticks * g_platform.timebase_info.numer /
                  g_platform.timebase_info.denom * 1e-9;
    /* A suspended process can have an arbitrarily long callback gap. A dead
     * link still needs a bounded watchdog instead of waiting for that gap. */
    return seconds >= 0.001 && seconds <= 0.1 ? seconds : 0.0;
}

f64 platform_window_display_link_silence_sec(PlatformWindow *w) {
    if (!w || !w->display_link) return 0.0;
    u64 started = atomic_load_explicit(&w->link_started_host, memory_order_acquire);
    if (!started) return 0.0;                       /* not running */
    u64 last = atomic_load_explicit(&w->link_last_host_time, memory_order_acquire);
    u64 ref = last > started ? last : started;
    u64 now = mach_absolute_time();
    if (now <= ref) return 0.0;
    return (f64)(now - ref) * g_platform.timebase_info.numer /
           g_platform.timebase_info.denom * 1e-9;
}

/* Every window's link is refreshed on the next sync. Main thread (both
 * notifications are delivered on the main queue). */
static void link_refresh_all(void) {
    for (i32 i = 0; i < g_window_count; i++)
        if (g_windows[i])
            atomic_store_explicit(&g_windows[i]->link_refresh, true, memory_order_release);
}

static void link_install_display_observers(void) {
    static bool installed = false;
    if (installed) return;
    installed = true;
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSApplicationDidChangeScreenParametersNotification
                    object:nil queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *n) { (void)n; link_refresh_all(); }];
    [[[NSWorkspace sharedWorkspace] notificationCenter]
        addObserverForName:NSWorkspaceDidWakeNotification
                    object:nil queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *n) { (void)n; link_refresh_all(); }];
    [[[NSWorkspace sharedWorkspace] notificationCenter]
        addObserverForName:NSWorkspaceScreensDidWakeNotification
                    object:nil queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *n) { (void)n; link_refresh_all(); }];
}

static void platform_window_display_link_teardown(PlatformWindow *w) {
    if (!w || !w->display_link) return;
    /* Stop before release: the callback holds `w`, and CVDisplayLinkRelease
     * does not wait for an in-flight callback on its own. */
    CVDisplayLinkStop(w->display_link);
    CVDisplayLinkRelease(w->display_link);
    w->display_link = NULL;
}

f32 platform_window_max_refresh_hz(PlatformWindow *w) {
    if (!w || !w->ns_window) return 60.0f;
    @autoreleasepool {
        NSScreen *screen = [w->ns_window screen];
        if (!screen) screen = [NSScreen mainScreen];
        if (screen) {
            if (@available(macOS 12.0, *)) {
                NSInteger hz = [screen maximumFramesPerSecond];
                if (hz > 0) return (f32)hz;
            }
        }
    }
    return 60.0f;
}

void platform_window_snap(PlatformWindow *w, WindowSnap pos) {
    if (!w || !w->ns_window) return;
    @autoreleasepool {
        NSScreen *screen = [w->ns_window screen];
        if (!screen) screen = [NSScreen mainScreen];
        NSRect vf = [screen visibleFrame];
        NSRect r;
        switch (pos) {
        case WIN_SNAP_LEFT_HALF:
            r = NSMakeRect(vf.origin.x, vf.origin.y,
                           vf.size.width / 2, vf.size.height);
            break;
        case WIN_SNAP_RIGHT_HALF:
            r = NSMakeRect(vf.origin.x + vf.size.width / 2, vf.origin.y,
                           vf.size.width / 2, vf.size.height);
            break;
        case WIN_SNAP_TOP_HALF:
            r = NSMakeRect(vf.origin.x, vf.origin.y + vf.size.height / 2,
                           vf.size.width, vf.size.height / 2);
            break;
        case WIN_SNAP_BOTTOM_HALF:
            r = NSMakeRect(vf.origin.x, vf.origin.y,
                           vf.size.width, vf.size.height / 2);
            break;
        case WIN_SNAP_FULL:
            r = vf;
            break;
        default:
            r = vf;
            break;
            break;
        case WIN_SNAP_CENTER: {
            NSRect cur = [w->ns_window frame];
            r = NSMakeRect(vf.origin.x + (vf.size.width - cur.size.width) / 2,
                           vf.origin.y + (vf.size.height - cur.size.height) / 2,
                           cur.size.width, cur.size.height);
            break;
        }
        case WIN_SNAP_TOP_LEFT:
            r = NSMakeRect(vf.origin.x, vf.origin.y + vf.size.height / 2,
                           vf.size.width / 2, vf.size.height / 2);
            break;
        case WIN_SNAP_TOP_RIGHT:
            r = NSMakeRect(vf.origin.x + vf.size.width / 2,
                           vf.origin.y + vf.size.height / 2,
                           vf.size.width / 2, vf.size.height / 2);
            break;
        case WIN_SNAP_BOTTOM_LEFT:
            r = NSMakeRect(vf.origin.x, vf.origin.y,
                           vf.size.width / 2, vf.size.height / 2);
            break;
        case WIN_SNAP_BOTTOM_RIGHT:
            r = NSMakeRect(vf.origin.x + vf.size.width / 2, vf.origin.y,
                           vf.size.width / 2, vf.size.height / 2);
            break;
        }
        [w->ns_window setFrame:r display:YES animate:YES];
    }
}

void platform_begin_window_drag(PlatformWindow *w) {
    if (!w || !w->ns_window || !w->view) return;
    @autoreleasepool {
        if (!w->view->_lastMouseEvent) return;
        /* Runs AppKit's own modal drag loop and returns when the mouse comes
         * up — which makes this the exact, and only, moment the drag ends.
         * There is no "window drag finished" notification to hook instead. */
        [w->ns_window performWindowDragWithEvent:w->view->_lastMouseEvent];

        /* Dropped onto another Liu window's tab strip? Then the user is asking
         * for this window to become a tab over there. Judged by where the
         * POINTER ended up, not by frame overlap: two windows overlap all the
         * time without it meaning anything, while the pointer is where the
         * user was actually aiming. */
        NSPoint p = [NSEvent mouseLocation];
        /* ...and by what is VISIBLY under it. Taking the first Liu window
         * whose strip rectangle contains the point would ignore stacking
         * order: a window let go over the place where another Liu window's
         * strip happens to be, hidden behind this one, behind a browser or
         * behind anything, would hand all of its tabs to a window the user
         * cannot see. The window server knows the order; ask it for the window
         * directly under the dragged one. */
        NSInteger self_num = [w->ns_window windowNumber];
        NSInteger num = [NSWindow windowNumberAtPoint:p belowWindowWithWindowNumber:0];
        if (num == self_num)
            num = [NSWindow windowNumberAtPoint:p belowWindowWithWindowNumber:self_num];
        if (num <= 0 || num == self_num) return;
        for (i32 i = 0; i < g_window_count; i++) {
            PlatformWindow *other = g_windows[i];
            if (!other || other == w || !other->ns_window) continue;
            if ((NSInteger)[other->ns_window windowNumber] != num) continue;
            if (![other->ns_window isVisible]) break;
            if ([other->ns_window styleMask] & NSWindowStyleMaskFullScreen) break;
            NSRect f = [other->ns_window frame];
            /* The TARGET's strip, not a global one: the drop band is the tab
             * strip the user is aiming at, and it is a different height when
             * that window hides its tab bar. */
            CGFloat strip = liu_strip_pt_for_window(other);
            NSRect bar = NSMakeRect(f.origin.x, f.origin.y + f.size.height - strip,
                                    f.size.width, strip);
            if (!NSPointInRect(p, bar)) break;
            PlatformEvent e = { .type = EVENT_WINDOW_MERGE };
            e.window = w;
            e.merge.target = other;
            push_event(e);
            break;
        }
    }
}

void platform_window_set_position(PlatformWindow *w, f32 x, f32 y) {
    if (!w || !w->ns_window) return;
    @autoreleasepool {
        /* AppKit's screen origin is the BOTTOM-left of the main display and y
         * grows upward; the app's is the top-left with y growing down. */
        NSScreen *main_screen = [[NSScreen screens] firstObject];
        if (!main_screen) return;
        NSRect sf = [main_screen frame];
        NSRect wf = [w->ns_window frame];
        CGFloat ns_y = sf.origin.y + sf.size.height - (CGFloat)y - wf.size.height;
        [w->ns_window setFrameOrigin:NSMakePoint((CGFloat)x, ns_y)];
    }
}

void platform_window_get_position(PlatformWindow *w, f32 *x, f32 *y) {
    if (x) *x = 0; if (y) *y = 0;
    if (!w || !w->ns_window) return;
    @autoreleasepool {
        NSScreen *main_screen = [[NSScreen screens] firstObject];
        if (!main_screen) return;
        NSRect sf = [main_screen frame];
        NSRect wf = [w->ns_window frame];
        if (x) *x = (f32)wf.origin.x;
        if (y) *y = (f32)(sf.origin.y + sf.size.height - wf.origin.y - wf.size.height);
    }
}

/* Window-local framebuffer pixels <-> screen points. The local half mirrors
 * -[SSHView pushMouseEvent:] step for step (view point -> backing pixels, Y
 * flipped against the backing height), so a point taken from a mouse event and
 * converted here lands exactly where the pointer is. The screen half is the
 * top-origin space platform_window_get_position() reports. */
bool platform_window_local_to_screen(PlatformWindow *w, f32 fx, f32 fy,
                                     f32 *sx, f32 *sy) {
    if (sx) *sx = 0;
    if (sy) *sy = 0;
    if (!w || !w->ns_window || !w->view) return false;
    @autoreleasepool {
        NSScreen *main_screen = [[NSScreen screens] firstObject];
        if (!main_screen) return false;
        NSView *v = w->view;
        NSRect fb = [v convertRectToBacking:v.bounds];
        NSPoint vp = [v convertPointFromBacking:NSMakePoint((CGFloat)fx,
                                                            fb.size.height - (CGFloat)fy)];
        NSPoint sp = [w->ns_window convertPointToScreen:[v convertPoint:vp toView:nil]];
        NSRect sf = [main_screen frame];
        if (sx) *sx = (f32)sp.x;
        if (sy) *sy = (f32)(sf.origin.y + sf.size.height - sp.y);
        return true;
    }
}

bool platform_window_screen_to_local(PlatformWindow *w, f32 sx, f32 sy,
                                     f32 *fx, f32 *fy) {
    if (fx) *fx = 0;
    if (fy) *fy = 0;
    if (!w || !w->ns_window || !w->view) return false;
    @autoreleasepool {
        NSScreen *main_screen = [[NSScreen screens] firstObject];
        if (!main_screen) return false;
        NSView *v = w->view;
        NSRect sf = [main_screen frame];
        NSPoint sp = NSMakePoint((CGFloat)sx,
                                 sf.origin.y + sf.size.height - (CGFloat)sy);
        NSPoint vp = [v convertPoint:[w->ns_window convertPointFromScreen:sp]
                            fromView:nil];
        NSPoint bp = [v convertPointToBacking:vp];
        NSRect fb = [v convertRectToBacking:v.bounds];
        if (fx) *fx = (f32)bp.x;
        if (fy) *fy = (f32)(fb.size.height - bp.y);
        return true;
    }
}

PlatformWindow *platform_window_at_screen_point_below(f32 sx, f32 sy,
                                                      PlatformWindow *through) {
    @autoreleasepool {
        NSScreen *main_screen = [[NSScreen screens] firstObject];
        if (!main_screen) return NULL;
        NSRect sf = [main_screen frame];
        NSPoint sp = NSMakePoint((CGFloat)sx,
                                 sf.origin.y + sf.size.height - (CGFloat)sy);
        /* Asked of the window server rather than answered from Liu's own
         * frames: it knows the stacking order across applications, so another
         * app's window sitting over a Liu window reads as "not Liu's" instead
         * of as a drop. */
        NSInteger num = [NSWindow windowNumberAtPoint:sp belowWindowWithWindowNumber:0];
        /* `through` on top: ask again for what is directly under it. Only that
         * one window is looked past — whatever comes next is the answer, ours
         * or not. */
        if (num > 0 && through && through->ns_window &&
            (NSInteger)[through->ns_window windowNumber] == num)
            num = [NSWindow windowNumberAtPoint:sp belowWindowWithWindowNumber:num];
        if (num <= 0) return NULL;
        for (i32 i = 0; i < g_window_count; i++) {
            PlatformWindow *w = g_windows[i];
            if (w && w != through && w->ns_window &&
                (NSInteger)[w->ns_window windowNumber] == num)
                return w;
        }
        return NULL;
    }
}

PlatformWindow *platform_window_at_screen_point(f32 sx, f32 sy) {
    return platform_window_at_screen_point_below(sx, sy, NULL);
}

bool platform_screen_visible_frame_at(f32 sx, f32 sy,
                                      f32 *x, f32 *y, f32 *w, f32 *h) {
    if (x) *x = 0; if (y) *y = 0; if (w) *w = 0; if (h) *h = 0;
    @autoreleasepool {
        NSArray<NSScreen *> *screens = [NSScreen screens];
        NSScreen *main_screen = [screens firstObject];
        if (!main_screen) return false;
        NSRect sf = [main_screen frame];
        NSPoint sp = NSMakePoint((CGFloat)sx,
                                 sf.origin.y + sf.size.height - (CGFloat)sy);
        /* The screen the point is on; failing that, the nearest one — a
         * pointer can sit in the dead corner between two displays of
         * different heights. */
        NSScreen *best = nil;
        CGFloat best_d = 0;
        for (NSScreen *sc in screens) {
            NSRect f = [sc frame];
            if (NSPointInRect(sp, f)) { best = sc; break; }
            CGFloat dx = sp.x < NSMinX(f) ? NSMinX(f) - sp.x
                       : sp.x > NSMaxX(f) ? sp.x - NSMaxX(f) : 0;
            CGFloat dy = sp.y < NSMinY(f) ? NSMinY(f) - sp.y
                       : sp.y > NSMaxY(f) ? sp.y - NSMaxY(f) : 0;
            CGFloat d = dx * dx + dy * dy;
            if (!best || d < best_d) { best = sc; best_d = d; }
        }
        if (!best) return false;
        NSRect vf = [best visibleFrame];
        if (x) *x = (f32)vf.origin.x;
        if (y) *y = (f32)(sf.origin.y + sf.size.height - vf.origin.y - vf.size.height);
        if (w) *w = (f32)vf.size.width;
        if (h) *h = (f32)vf.size.height;
        return true;
    }
}

void platform_window_set_drag_alpha(PlatformWindow *w, f32 alpha) {
    if (!w || !w->ns_window) return;
    if (liu_dev_env("LIU_WINDOW_UNSEEN")) return;      /* it stays unseen */
    @autoreleasepool {
        CGFloat a = alpha < 0.0f ? 0.0 : alpha > 1.0f ? 1.0 : (CGFloat)alpha;
        if ([w->ns_window alphaValue] != a) [w->ns_window setAlphaValue:a];
    }
}

bool platform_mouse_primary_down(void) {
    return ([NSEvent pressedMouseButtons] & 1u) != 0;
}

void platform_window_activate(PlatformWindow *w) {
    if (!w || !w->ns_window) return;
    @autoreleasepool {
        if ([w->ns_window isMiniaturized]) [w->ns_window deminiaturize:nil];
        [w->ns_window makeKeyAndOrderFront:nil];
    }
}

bool platform_window_should_close(PlatformWindow *w) {
    return w->delegate.shouldClose;
}

void platform_window_cancel_close(PlatformWindow *w) {
    if (w && w->delegate) w->delegate.shouldClose = NO;
}

void platform_window_perform_close(PlatformWindow *w) {
    if (!w || !w->ns_window || !w->delegate) return;
    /* What performClose: reaches on a click (and nothing else: the delegate
     * answers NO and closes nothing itself) — without the button, which a
     * window with a custom title bar may not have where AppKit looks. */
    @autoreleasepool { (void)[w->delegate windowShouldClose:w->ns_window]; }
}

bool platform_window_is_fullscreen(PlatformWindow *w) {
    if (!w || !w->ns_window) return false;
    return ([w->ns_window styleMask] & NSWindowStyleMaskFullScreen) != 0;
}

f32 platform_traffic_light_right_pt(PlatformWindow *w) {
    if (!w || !w->ns_window) return 0.0f;
    /* Fullscreen hands the buttons to AppKit's auto-hiding overlay; their
     * frames there say nothing about the toolbar Liu paints. */
    if ([w->ns_window styleMask] & NSWindowStyleMaskFullScreen) return 0.0f;
    NSWindowButton kinds[3] = { NSWindowCloseButton, NSWindowMiniaturizeButton,
                                NSWindowZoomButton };
    CGFloat right = 0.0;
    for (int i = 0; i < 3; i++) {
        NSButton *b = [w->ns_window standardWindowButton:kinds[i]];
        if (!b || b.isHidden || !b.superview) continue;
        NSRect f = b.frame;
        CGFloat r = f.origin.x + f.size.width;
        if (r > right) right = r;
    }
    return (f32)right;
}

bool platform_window_visible(PlatformWindow *w) {
    if (!w || !w->ns_window) return true;
    /* Occlusion notifications may lag the Dock transition. A minimized
     * window cannot become a paint target through a stale visible bit. */
    if ([w->ns_window isMiniaturized]) return false;
    /* NSWindowOcclusionStateVisible means "at least partly on screen and not
     * covered". AppKit clears it for minimised windows, windows on another
     * space, and windows fully behind others — every case where drawing is
     * wasted work. Not gated on key/main: a visible but unfocused window still
     * has to show live output. */
    return ([w->ns_window occlusionState] & NSWindowOcclusionStateVisible) != 0;
}

void platform_window_set_minimized(PlatformWindow *w, bool minimized) {
    if (!w || !w->ns_window) return;
    @autoreleasepool {
        if (minimized) { if (![w->ns_window isMiniaturized]) [w->ns_window miniaturize:nil]; }
        else if ([w->ns_window isMiniaturized]) [w->ns_window deminiaturize:nil];
    }
}

bool platform_window_is_minimized(PlatformWindow *w) {
    return w && w->ns_window && [w->ns_window isMiniaturized];
}

void platform_window_swap_buffers(PlatformWindow *w) {
#ifdef USE_METAL
    /* Metal presents in renderer_end_frame */
#else
    [w->gl_context flushBuffer];
#endif
    /* A presented frame is the one signal that the picture changed; the
     * accessibility notifier rides on it (one bool test with VoiceOver off). */
    liu_a11y_frame_presented(w);
}

void platform_make_current(PlatformWindow *w) {
#ifdef USE_METAL
    (void)w; /* no-op for Metal */
#else
    [w->gl_context makeCurrentContext];
#endif
}

bool platform_begin_file_drag(PlatformWindow *w,
                              const PlatformFilePromise *items, i32 count)
{
#ifndef USE_METAL
    (void)w; (void)items; (void)count;
    return false;    /* OpenGL path skipped for V1 */
#else
    if (!w || !w->view || !items || count <= 0) return false;
    SSHView *view = w->view;
    NSEvent *seed = view->_lastMouseEvent;
    if (!seed) return false;

    @autoreleasepool {
        NSMutableArray<NSDraggingItem *> *drag_items =
            [NSMutableArray arrayWithCapacity:(NSUInteger)count];
        NSPoint origin = [view convertPoint:seed.locationInWindow fromView:nil];
        bool consumed_remote = false;

        for (i32 i = 0; i < count; i++) {
            const PlatformFilePromise *it = &items[i];

            id<NSPasteboardWriting> writer = nil;
            if (it->is_remote) {
                if (!it->ctx || !it->provider) continue;
                /* NSFilePromiseProvider with the view as delegate.
                 * Attach ctx + provider fn via userInfo. */
                NSString *type = (NSString *)UTTypeData.identifier;  /* generic file promise */
                /* Try to resolve a better UTI from the filename extension. */
                NSString *name = [NSString stringWithUTF8String:it->display_name];
                if (!name) name = @"dropped";
                NSString *ext = [name pathExtension];
                if (ext.length > 0) {
                    UTType *tt = [UTType typeWithFilenameExtension:ext];
                    if (tt && tt.identifier) type = tt.identifier;
                }
                NSFilePromiseProvider *prov =
                    [[NSFilePromiseProvider alloc] initWithFileType:type
                                                           delegate:view];
                LiuFilePromiseBox *box = [[LiuFilePromiseBox alloc]
                    initWithContext:it->ctx provider:it->provider
                    cleanup:it->cleanup];
                if (!box) {
                    if (it->cleanup) it->cleanup(it->ctx);
                    consumed_remote = true;
                    [prov release];
                    continue;
                }
                consumed_remote = true;
                prov.userInfo = @{
                    @"name": name,
                    @"box": box,
                };
                [box release];
                writer = prov;
            } else {
                /* Local item — use a file URL directly. */
                NSString *path = [[NSFileManager defaultManager]
                    stringWithFileSystemRepresentation:it->local_path
                    length:strlen(it->local_path)];
                if (!path) continue;
                NSURL *url = [NSURL fileURLWithPath:path];
                NSPasteboardItem *pi = [[NSPasteboardItem alloc] init];
                [pi setString:[url absoluteString] forType:NSPasteboardTypeFileURL];
                writer = pi;
            }

            NSDraggingItem *di = [[NSDraggingItem alloc] initWithPasteboardWriter:writer];
            if (!di) {
                [writer release];
                continue;
            }
            /* Stack subsequent items 8 pt below each other so Finder's
             * multi-file badge renders a legible pile. */
            NSRect frame = NSMakeRect(origin.x - 8, origin.y - 8 - i*8, 28, 28);
            [di setDraggingFrame:frame contents:nil];
            [drag_items addObject:di];
            [di release];
            [writer release];
        }

        /* A remote ctx is consumed once wrapped (or explicitly cleaned after
         * an allocation failure). Return true in that rare all-OOM case so
         * the caller does not attempt a second cleanup. */
        if (drag_items.count == 0) return consumed_remote;

        [view beginDraggingSessionWithItems:drag_items
                                      event:seed
                                     source:view];
    }
    return true;
#endif
}

/* =========================================================================
 * Events
 * ========================================================================= */

/* LIU_MASK_ALTERNATE=<seconds>: flip the rounded-corner mask on the container
 * every N seconds and log the edge, so one visible window can compare both
 * states. Alternating inside one process keeps other GPU clients on the
 * machine from skewing a compositor-cost comparison. */
static void liu_mask_alternate_tick(void) {
    static double period = -1.0;
    static double next_t = 0.0;
    static int    on     = 1;
    if (period < 0.0) {
        const char *e = getenv("LIU_MASK_ALTERNATE");
        period = e ? atof(e) : 0.0;
        if (period < 0.5) period = 0.0;
    }
    if (period == 0.0) return;
    double now = (double)platform_time_sec();
    if (next_t == 0.0) { next_t = now + period; return; }
    if (now < next_t) return;
    next_t = now + period;
    on = !on;
    for (i32 i = 0; i < g_window_count; i++) {
        NSWindow *win = g_windows[i] ? g_windows[i]->ns_window : nil;
        NSView   *cv  = win ? [win contentView] : nil;
        if (!cv || !cv.layer) continue;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        cv.layer.cornerRadius  = on ? LIU_CORNER_RADIUS : 0.0;
        cv.layer.masksToBounds = on ? YES : NO;
        [CATransaction commit];
    }
    fprintf(stderr, "[mask] %s\n", on ? "ON" : "OFF");
    fflush(stderr);
}

void platform_poll_events(void) {
    liu_mask_alternate_tick();
    @autoreleasepool {
        for (i32 dispatched = 0; dispatched < 64 && g_evq.count < 128 && !g_evq.overflow_head; dispatched++) {
            /* NSRunLoopCommonModes is a set marker, not a real mode — passing
             * it here makes CFRunLoopRunSpecific reject the call. Default
             * mode is correct; live-resize handling is driven by the timer
             * registered on common modes in SSHView's start-live-resize hook,
             * not by trying to dequeue from tracking mode here. */
            NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                untilDate:nil
                inMode:NSDefaultRunLoopMode
                dequeue:YES];
            if (!event) break;
            /* Capture the source window for push_event() — see the window
             * registry banner. Restored (not just cleared) so a nested pump
             * cannot strand the outer one. */
            PlatformWindow *prev_src = g_event_source;
            g_event_source = platform_window_for_ns([event window]);
            [NSApp sendEvent:event];
            g_event_source = prev_src;
        }
    }
}

/* platform_set_wake_now_handler. Both MAIN THREAD ONLY: the flag is up only
 * while the loop is parked in the wait below, between its turns, which is the
 * one moment the app's state is whole and no AppKit call of the app's is on
 * the stack (a modal panel or a resize's tracking loop runs from sendEvent,
 * after the flag is down). */
static void (*g_wake_now_handler)(void);
static bool g_in_loop_wait;

void platform_set_wake_now_handler(void (*fn)(void)) { g_wake_now_handler = fn; }

void platform_wait_events(f64 timeout_sec) {
    if (g_evq.count || g_evq.overflow_head) return;
    @autoreleasepool {
        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:timeout_sec];
        g_in_loop_wait = true;
        NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
            untilDate:until
            inMode:NSDefaultRunLoopMode
            dequeue:YES];
        g_in_loop_wait = false;
        if (event) {
            PlatformWindow *prev_src = g_event_source;
            g_event_source = platform_window_for_ns([event window]);
            [NSApp sendEvent:event];
            g_event_source = prev_src;
        }
    }
}

/* Storage for the live-resize render callback lives near the top of the
 * file (search for "static PlatformRenderCallback g_render_cb"). */
void platform_set_render_callback(PlatformRenderCallback cb, void *user) {
    g_render_cb = cb;
    g_render_user = user;
}

void platform_set_titlebar_zoom_query(PlatformTitlebarZoomQuery fn, void *user) {
    g_zoom_query = fn;
    g_zoom_user = user;
}

/* =========================================================================
 * Socket read-ready watches
 *
 * fd-indexed table of dispatch_source_t + a "suspended" flag. When the
 * kernel marks an fd readable, the source fires once and the handler
 * dispatch_suspend()s it so the global queue doesn't spin (EV_DISPATCH
 * plus an unread fd = immediate re-fire = 100 % CPU). The handler also
 * schedules a coalesced wake NSEvent on the main queue.
 *
 * The main loop calls platform_resume_watches() AFTER draining session
 * I/O — resuming reads any pending data into libssh2 / the terminal,
 * then the source becomes eligible to fire again only on genuinely new
 * arrivals. Resuming before the drain would re-fire at once on data that
 * is already queued.
 * ========================================================================= */

#define WATCH_MAX_FD 2048
static dispatch_source_t g_watch_sources[WATCH_MAX_FD] = {0};
static _Atomic int       g_watch_suspended[WATCH_MAX_FD] = {0};
static pthread_mutex_t   g_watch_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool      g_wake_pending = false;
/* Incremented from any thread when an fd watch fires; decremented (zeroed)
 * by platform_take_fd_fire_count() on the main thread. Used to skip the
 * full session-poll pass on idle frames where no fd became readable. */
static _Atomic int       g_fd_fire_count = 0;
/* Monotonic total of fire events ever observed. Never reset. The resume
 * path uses it to detect "nothing fired since last resume" without racing
 * with platform_take_fd_fire_count(), which zeros the per-frame counter. */
static _Atomic uint64_t  g_fd_fire_total = 0;

static _Atomic uint64_t g_last_take_ns = 0;
static u64 mach_absolute_time_ns(void) {
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
/* Resolved once, on first use from any thread: getenv is not thread-safe
 * against a concurrent setenv, and none happens after startup. */
static i64 wake_quantum_ns(void) {
    static _Atomic i64 v = -1;
    i64 c = atomic_load_explicit(&v, memory_order_acquire);
    if (c >= 0) return c;
    const char *e = getenv("LIU_WAKE_QUANTUM_US");
    i64 us = (e && *e) ? atoll(e) : 1000;
    if (us < 0) us = 0;
    if (us > 50000) us = 50000;
    atomic_store_explicit(&v, us * 1000, memory_order_release);
    return us * 1000;
}

/* postEvent:atStart: supports secondary threads. Wake the event queue
 * directly rather than waiting for an intermediate main-queue block. */
static void post_wake_event_now(void) {
    @autoreleasepool {
        NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                      location:NSZeroPoint
                                 modifierFlags:0
                                     timestamp:0
                                  windowNumber:0
                                       context:nil
                                       subtype:0
                                         data1:0
                                         data2:0];
        if (e) [NSApp postEvent:e atStart:NO];
    }
}

/* Ring wakes only (see platform_wake_event_loop): a wake that lands within one
 * quantum of the loop's last drain is posted at the END of the quantum rather
 * than at once, so a flooding producer costs the loop one AppKit event round
 * trip per quantum instead of one per read, and the producers get the core
 * back. The added latency is bounded by the quantum and applies to PTY OUTPUT
 * only: keyboard/mouse are real NSEvents, and dispatch-source watches keep
 * their immediate post because suspension already bounds them to one fire per
 * iteration and a deferred fire would cap a watch-backed pty at kernel-queue /
 * quantum. LIU_WAKE_QUANTUM_US selects it; 0 is the immediate post. Returns
 * true when the wake was deferred (the caller must then not post). */
static bool wake_defer_within_quantum(void) {
    i64 q = wake_quantum_ns();
    if (q > 0) {
        u64 now = mach_absolute_time_ns();
        u64 last = atomic_load_explicit(&g_last_take_ns, memory_order_acquire);
        i64 since = (i64)(now - last);
        if (since >= 0 && since < q) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, q - since),
                           dispatch_get_main_queue(), ^{
                post_wake_event_now();
            });
            return true;
        }
    }
    return false;
}

static void watch_post_wake_event_ex(bool allow_defer) {
    /* Coalesce wake-ups: one NSEvent per MAIN-LOOP ITERATION, not one per
     * producer notification. The flag is armed here and released only by
     * platform_take_fd_fire_count(), the point where the loop actually
     * consumes the wake — NOT inside the block.
     *
     * The block runs inside -nextEventMatchingMask:, i.e. inside
     * platform_poll_events()'s drain loop, and a pty_reader thread calls
     * platform_wake_event_loop() once per read(), which for a flooding child
     * is every few dozen bytes. Cleared as the block ran, the flag would be
     * re-armed within microseconds: every pass of the drain loop would find
     * a freshly posted wake event and keep dequeueing until a gap in the
     * flood. The fd-watch path is bounded by construction (it suspends its
     * dispatch source on fire and resumes it in platform_resume_watches(),
     * one fire per iteration); this gives the ring path the same bound.
     *
     * No wake is lost: the ring's head is published before the wake, and
     * platform_take_fd_fire_count() runs every iteration ahead of the drain,
     * so a wake coalesced while the flag is armed is answered by the very
     * drain that clears it, and one arriving after the clear posts anew. The
     * one bound this trades is a wake event swallowed by a nested run loop
     * (menu tracking, live resize): the ring is then drained on the next
     * ordinary iteration, at most one timeout-ladder step later. */
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &g_wake_pending, &expected, true,
            memory_order_acquire, memory_order_relaxed)) {
        return;
    }
    if (allow_defer && wake_defer_within_quantum()) return;
#ifdef LIU_E2E_PROBE
    /* Experimental control, in LIU_E2E_PROBE builds only; the normal
     * application keeps the queued wake policy. */
    if (allow_defer && getenv("LIU_E2E_DIRECT_WAKE")) {
        post_wake_event_now();
        return;
    }
#endif
    dispatch_async(dispatch_get_main_queue(), ^{ post_wake_event_now(); });
}

static void watch_post_wake_event(void) { watch_post_wake_event_ex(false); }

void platform_watch_socket(int fd) {
    if (fd < 0 || fd >= WATCH_MAX_FD) return;
    pthread_mutex_lock(&g_watch_lock);
    if (g_watch_sources[fd]) { pthread_mutex_unlock(&g_watch_lock); return; }
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    __block dispatch_source_t src = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, q);
    if (!src) { pthread_mutex_unlock(&g_watch_lock); return; }
    _Atomic int *susp = &g_watch_suspended[fd];
    dispatch_source_set_event_handler(src, ^{
        /* Cancellation may overlap a handler already running. The fd slot
         * can name a replacement by then, so retirement and the suspend
         * transition share the table lock. Only this source may mark its
         * current slot suspended; an obsolete handler owes no new wake. */
        pthread_mutex_lock(&g_watch_lock);
        bool current = g_watch_sources[fd] == src;
        if (current) {
            dispatch_suspend(src);
            atomic_store_explicit(susp, 1, memory_order_release);
        }
        pthread_mutex_unlock(&g_watch_lock);
        if (current) {
            atomic_fetch_add_explicit(&g_fd_fire_count, 1, memory_order_release);
            atomic_fetch_add_explicit(&g_fd_fire_total, 1, memory_order_release);
            watch_post_wake_event();
        }
    });
    atomic_store_explicit(susp, 0, memory_order_relaxed);
    g_watch_sources[fd] = src;
    dispatch_resume(src);
    pthread_mutex_unlock(&g_watch_lock);
}

void platform_unwatch_socket(int fd) {
    if (fd < 0 || fd >= WATCH_MAX_FD) return;
    pthread_mutex_lock(&g_watch_lock);
    dispatch_source_t src = g_watch_sources[fd];
    g_watch_sources[fd] = NULL;
    /* A suspended source refuses dispatch_source_cancel until resumed;
     * balance the suspend count so cancel actually takes effect. */
    int was_suspended = atomic_exchange_explicit(&g_watch_suspended[fd], 0,
                                                 memory_order_acq_rel);
    pthread_mutex_unlock(&g_watch_lock);
    if (src) {
        if (was_suspended) dispatch_resume(src);
        dispatch_source_cancel(src);
        /* MRC (no ARC): dispatch_source_create returned a +1 object stored in
         * g_watch_sources[]. cancel stops it firing but does NOT release it;
         * without this every watch→unwatch (each session open/close) leaks the
         * source + its captured handler block. Safe to release right after
         * cancel — dispatch retains the source while a handler runs. */
        dispatch_release(src);
    }
}

void platform_resume_watches(void) {
    /* Called once per frame after the main loop has drained session I/O.
     * Re-arms each suspended source so it can fire on the next data
     * arrival. Sources that weren't fired this frame are left alone
     * (suspended flag == 0).
     *
     * Fast path: if no watch handler has fired since the last resume, the
     * 2048-slot table is guaranteed to hold no suspended sources, so skip
     * the lock + scan entirely and keep idle frames free of that work. */
    static _Atomic uint64_t last_seen_fire_total = 0;
    uint64_t total = atomic_load_explicit(&g_fd_fire_total, memory_order_acquire);
    uint64_t seen = atomic_exchange_explicit(&last_seen_fire_total, total,
                                              memory_order_acq_rel);
    if (!watch_hold_pass_begin(total != seen)) return;
    bool skipped = false;
    pthread_mutex_lock(&g_watch_lock);
    for (int i = 0; i < WATCH_MAX_FD; i++) {
        if (!g_watch_sources[i]) continue;
        /* Backpressure: this pane spent its whole read budget last frame, so
         * resuming now would only re-fire the source immediately. Leave it
         * suspended for one cycle — main.c drains it anyway, see
         * AppState.poll_saturated_panes. The `continue` sits BEFORE the
         * exchange, so the suspended flag stays set and the resume is
         * deferred rather than lost; libdispatch's suspend/resume balance is
         * untouched. */
        if (watch_hold_is_set(i)) { skipped = true; continue; }
        int was = atomic_exchange_explicit(&g_watch_suspended[i], 0,
                                            memory_order_acq_rel);
        if (was) dispatch_resume(g_watch_sources[i]);
    }
    pthread_mutex_unlock(&g_watch_lock);
    /* Expire every hold and record whether anything was left disarmed. That
     * recorded bit is what makes the NEXT pass run even when nothing fired:
     * without it the fast path above returns early and a skipped source stays
     * suspended for ever. */
    watch_hold_pass_end(skipped);
}

/* See platform.h: monotonic, read by observers that subtract their own last
 * snapshot. */
static _Atomic unsigned long long g_wake_total = 0;

u64 platform_wake_count(void) {
    return (u64)atomic_load_explicit(&g_wake_total, memory_order_relaxed);
}

/* ---- OS memory pressure (see platform.h) ------------------------------
 *
 * The dispatch source fires on a utility queue whenever the kernel's
 * memorystatus level changes to WARN or CRITICAL (and again on later
 * transitions). The handler does two things and nothing else: raises the
 * recorded level, and wakes the loop so the level is taken THIS iteration
 * rather than at the next input. No free, no GPU wait, no walk of the
 * windows here — the queue is not the main thread and none of the caches
 * are safe to touch from it. */
static _Atomic int g_mem_pressure_level = 0;
static dispatch_source_t g_mem_pressure_source = NULL;

void platform_memory_pressure_inject(PlatformMemPressure level) {
    int want = (int)level, have = atomic_load_explicit(&g_mem_pressure_level, memory_order_relaxed);
    while (want > have &&
           !atomic_compare_exchange_weak_explicit(&g_mem_pressure_level, &have, want,
                                                  memory_order_acq_rel, memory_order_relaxed)) {}
    platform_wake_event_loop();
}

static void platform_memory_pressure_start(void) {
    if (g_mem_pressure_source) return;
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
        DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL, q);
    if (!src) return;
    dispatch_source_set_event_handler(src, ^{
        unsigned long data = dispatch_source_get_data(src);
        if (data & DISPATCH_MEMORYPRESSURE_CRITICAL)
            platform_memory_pressure_inject(PLATFORM_MEM_PRESSURE_CRITICAL);
        else if (data & DISPATCH_MEMORYPRESSURE_WARN)
            platform_memory_pressure_inject(PLATFORM_MEM_PRESSURE_WARN);
    });
    dispatch_resume(src);
    g_mem_pressure_source = src;
}

PlatformMemPressure platform_memory_pressure_take(void) {
    int level = atomic_exchange_explicit(&g_mem_pressure_level, 0, memory_order_acq_rel);
    return (PlatformMemPressure)level;
}

/* ---- "at work" (see platform.h, core/activity_hold.h) ------------------
 *
 * What each activity option does to a covered window whose pane is printing:
 *
 *   no activity                                napped: every thread at 4
 *   UserInitiatedAllowingIdleSystemSleep       not napped, but often clamped
 *                                              to utility (20), below every
 *                                              default-priority program on
 *                                              the machine
 *   ... | LatencyCritical                      46 (readers at 37)
 *   Background | LatencyCritical               napped all the same
 *
 * So both halves are needed. "AllowingIdleSystemSleep" stays: the Mac may
 * still go to sleep when the user leaves it — a build in a pane is not a
 * reason to keep a laptop awake on battery. The token is retained manually
 * (this file is not ARC). */
static id<NSObject> g_user_activity = nil;

void platform_set_user_activity(bool active) {
    if (active == (g_user_activity != nil)) return;
    NSProcessInfo *pi = [NSProcessInfo processInfo];
    if (active) {
        NSActivityOptions opts = NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical;
        g_user_activity = [[pi beginActivityWithOptions:opts
                                                 reason:@"A program is running in a terminal pane"] retain];
    } else {
        [pi endActivity:g_user_activity];
        [g_user_activity release];
        g_user_activity = nil;
    }
}

bool platform_user_activity_held(void) { return g_user_activity != nil; }

/* ---- Secure Event Input (see platform.h, ui/secure_input.h) -------------
 *
 * The system counts Enable against Disable per process; the flag keeps this
 * file's own count at one. If the process dies holding it, the system lets it
 * go by itself. */
static bool g_secure_input = false;

/* Carbon's own header cannot be imported in this file: its `EventType`
 * collides with platform.h's. These two calls are all that is wanted from it
 * (HIToolbox, in the Carbon framework the app already links; OSStatus is a
 * 32-bit int). */
extern int EnableSecureEventInput(void);
extern int DisableSecureEventInput(void);

void platform_secure_input_set(bool on) {
    if (on == g_secure_input) return;
    if (on) {
        const char *no_activate = liu_dev_env("LIU_NO_ACTIVATE");
        if (no_activate && no_activate[0] && no_activate[0] != '0') return;
        EnableSecureEventInput();
    } else {
        DisableSecureEventInput();
    }
    g_secure_input = on;
}

bool platform_secure_input_held(void) { return g_secure_input; }

/* The check mark of the app menu's "Secure Keyboard Entry" item: the user's
 * SWITCH, not whether the system holds secure input right now — that is what
 * the toolbar's chip says. */
void platform_menu_set_secure_keyboard(bool on) {
    if (!g_secure_menu_item) return;
    NSControlStateValue want = on ? NSControlStateValueOn : NSControlStateValueOff;
    if (g_secure_menu_item.state != want) g_secure_menu_item.state = want;
}

bool platform_menu_secure_keyboard_checked(void) {
    return g_secure_menu_item && g_secure_menu_item.state == NSControlStateValueOn;
}

void platform_system_locale(char *out, usize cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    @autoreleasepool {
        NSLocale *loc = [NSLocale currentLocale];
        NSString *lang = [loc objectForKey:NSLocaleLanguageCode];
        NSString *country = [loc objectForKey:NSLocaleCountryCode];
        if (lang.length && country.length)
            snprintf(out, cap, "%s_%s", lang.UTF8String, country.UTF8String);
    }
}

void platform_set_quit_guard(bool (*allow)(void)) { g_quit_guard = allow; }

void platform_wake_event_loop(void) {
    atomic_fetch_add_explicit(&g_wake_total, 1, memory_order_relaxed);
    /* Counted as an fd fire so the main loop's needs_session_poll gate lets the
     * drain run — a ring IS session I/O, even though no descriptor became
     * readable. g_fd_fire_total is deliberately NOT bumped: it exists solely so
     * platform_resume_watches() can skip its 2048-slot scan when no dispatch
     * source suspended itself, and none did here. */
    atomic_fetch_add_explicit(&g_fd_fire_count, 1, memory_order_release);
    watch_post_wake_event_ex(true);
}

static _Atomic bool g_wake_now_mark = false;
bool platform_take_wake_now(void) {
    return atomic_exchange_explicit(&g_wake_now_mark, false, memory_order_acq_rel);
}

/* The post itself is watch_post_wake_event's, with the quantum skipped. Its
 * main-queue block is the first thing the main thread runs once it is awake,
 * well before the NSEvent it posts comes back out of -nextEventMatchingMask:
 * (posting that event from the reader thread instead gains nothing: AppKit's
 * return grows by what the hop saves). So when the loop is parked in its wait,
 * the block runs the app's early drain first, and the answer is queued before
 * AppKit hands the wake back. */
void platform_wake_event_loop_now(void) {
    atomic_store_explicit(&g_wake_now_mark, true, memory_order_release);
    atomic_fetch_add_explicit(&g_wake_total, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_fd_fire_count, 1, memory_order_release);
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &g_wake_pending, &expected, true,
            memory_order_acquire, memory_order_relaxed)) {
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_in_loop_wait && g_wake_now_handler) {
            g_in_loop_wait = false;     /* nothing in it may start a second one */
            g_wake_now_handler();
            g_in_loop_wait = true;
        }
        post_wake_event_now();
    });
}

#ifdef USE_METAL
void platform_wake_render_loop(void) {
    watch_post_wake_event();
}
#endif

i32 platform_take_fd_fire_count(void) {
    /* The loop is consuming this iteration's wake: re-arm the coalescer so
     * the next reader wake posts a fresh event. Cleared BEFORE the count is
     * taken, so a wake that lands between the two is counted by this
     * iteration's drain AND posts its own event (a harmless extra turn),
     * never the other way round. See watch_post_wake_event(). */
    atomic_store_explicit(&g_wake_pending, false, memory_order_release);
    atomic_store_explicit(&g_last_take_ns, mach_absolute_time_ns(),
                          memory_order_release);
    return (i32)atomic_exchange_explicit(&g_fd_fire_count, 0,
                                          memory_order_acq_rel);
}

bool platform_next_event(PlatformEvent *event) {
    return platform_event_queue_pop(&g_evq, event);
}

void platform_event_queue_stats(i32 *high_water, u32 *coalesced, u32 *evicted,
                                u32 *dropped_lowpri, u32 *dropped_critical) {
    if (high_water)       *high_water       = g_evq.high_water;
    if (coalesced)        *coalesced        = g_evq.coalesced;
    if (evicted)          *evicted          = g_evq.evicted_lowpri;
    if (dropped_lowpri)   *dropped_lowpri   = g_evq.dropped_lowpri;
    if (dropped_critical) *dropped_critical = g_evq.dropped_critical;
}

/* =========================================================================
 * Clipboard
 * ========================================================================= */

/* NSString's -UTF8String pointer remains valid only while the NSString
 * exists. Copy it into a persistent buffer before the autorelease pool
 * drains; each call replaces the previous buffer contents, so callers must
 * consume the value before the next call. */
static char   *g_clip_buf = NULL;
static size_t  g_clip_cap = 0;
#define MAC_CLIPBOARD_MAX_BYTES PLATFORM_CLIPBOARD_MAX_BYTES
/* Capacity kept resident between reads. Below this the block is small enough
 * that handing it back would only churn the allocator; above it, one 16 MB
 * paste would otherwise pin 16 MB for the life of the process. */
#define MAC_CLIPBOARD_KEEP_BYTES (64u * 1024u)

/* Give an oversized buffer back to the allocator once the clipboard has gone
 * back to being small. Called only at the TOP of a read, before the new value
 * is built: at that point the previous result is about to be overwritten
 * anyway, so the pointer handed out last call dies exactly when it already
 * died (the grow path's realloc could already move it). Shrinking after the
 * memcpy instead would invalidate the pointer just returned. A failed shrink
 * is harmless — the larger block stays. */
static void mac_clipboard_trim(usize need) {
    if (g_clip_cap <= (usize)MAC_CLIPBOARD_KEEP_BYTES) return;  /* nothing worth freeing */
    if (need > g_clip_cap / 4u) return;                          /* still roughly sized right */
    usize target = (need < (usize)MAC_CLIPBOARD_KEEP_BYTES)
                       ? (usize)MAC_CLIPBOARD_KEEP_BYTES : need;
    if (target >= g_clip_cap) return;
    char *p = realloc(g_clip_buf, target);
    if (!p) return;
    g_clip_buf = p;
    g_clip_cap = target;
}

/* Under automation the app uses a real, process-private AppKit
 * pasteboard, so the general clipboard is never read or cleared.
 * Shipping builds contain no environment override. */
static NSPasteboard *mac_app_pasteboard(void) {
#ifdef LIU_DEV_HOOKS
    if (getenv("LIU_E2E_SCRIPT")) {
        static NSPasteboard *test_board;
        if (!test_board) test_board = [[NSPasteboard pasteboardWithUniqueName] retain];
        return test_board;
    }
#endif
    return [NSPasteboard generalPasteboard];
}

const char *platform_clipboard_get(void) {
    @autoreleasepool {
        NSPasteboard *pb = mac_app_pasteboard();
        NSString *str = [pb stringForType:NSPasteboardTypeString];
        const char *utf8 = str ? [str UTF8String] : NULL;
        if (!utf8) {
            mac_clipboard_trim(1);
            if (g_clip_buf) g_clip_buf[0] = '\0';
            return g_clip_buf ? g_clip_buf : "";
        }
        size_t len = strnlen(utf8, MAC_CLIPBOARD_MAX_BYTES + 1u);
        if (len > MAC_CLIPBOARD_MAX_BYTES) return "";
        mac_clipboard_trim(len + 1);
        if (len + 1 > g_clip_cap) {
            size_t new_cap = len + 1;
            if (new_cap < 1024) new_cap = 1024;
            char *p = realloc(g_clip_buf, new_cap);
            if (!p) return "";
            g_clip_buf = p;
            g_clip_cap = new_cap;
        }
        memcpy(g_clip_buf, utf8, len + 1);
        return g_clip_buf;
    }
}

PlatformClipResult platform_clipboard_set(const char *text) {
    if (!text) return PLATFORM_CLIP_BAD_TEXT;
    if (strnlen(text, MAC_CLIPBOARD_MAX_BYTES + 1u) > MAC_CLIPBOARD_MAX_BYTES)
        return PLATFORM_CLIP_TOO_LARGE;
    @autoreleasepool {
        /* stringWithUTF8String returns nil on invalid UTF-8 — don't clobber
         * the pasteboard with a nil setString (crashes on some macOS builds).
         * Both refusals are decided before clearContents, so the pasteboard
         * keeps what it held. */
        NSString *ns = [NSString stringWithUTF8String:text];
        if (!ns) return PLATFORM_CLIP_BAD_TEXT;
        NSPasteboard *pb = mac_app_pasteboard();
        [pb clearContents];
        if (![pb setString:ns forType:NSPasteboardTypeString])
            return PLATFORM_CLIP_FAILED;
    }
    return PLATFORM_CLIP_OK;
}

PlatformClipResult platform_clipboard_set_png(const u8 *png, usize length) {
    if (!clipboard_png_header_valid(png, length))
        return length > PLATFORM_CLIPBOARD_PNG_MAX_BYTES ? PLATFORM_CLIP_TOO_LARGE : PLATFORM_CLIP_BAD_TEXT;
    @autoreleasepool {
        NSData *data = [NSData dataWithBytes:png length:length];
        if (!data) return PLATFORM_CLIP_FAILED;
        NSPasteboard *pb = mac_app_pasteboard();
#ifdef LIU_E2E_PROBE
        /* Use the configured isolated pasteboard for generated image content
         * so the operation does not replace the user's clipboard. */
        const char *test_name=getenv("LIU_E2E_PNG_PASTEBOARD");
        if(test_name) {
            if(strncmp(test_name,"liu-sketch-test-",16)!=0 || strlen(test_name)>120)return PLATFORM_CLIP_FAILED;
            NSString *name=[NSString stringWithUTF8String:test_name];
            if(!name)return PLATFORM_CLIP_FAILED;
            pb=[NSPasteboard pasteboardWithName:name];
            if(!pb)return PLATFORM_CLIP_FAILED;
        }
#endif
        [pb clearContents];
        return [pb setData:data forType:NSPasteboardTypePNG] ? PLATFORM_CLIP_OK : PLATFORM_CLIP_FAILED;
    }
}

void platform_set_option_as_alt(i32 mode) {
    g_option_as_alt = (mode < 0 || mode > 3) ? 0 : mode;
}

/* =========================================================================
 * URL opening
 * ========================================================================= */

/* A local path reached through a link: open it only when opening cannot run
 * it, otherwise reveal it in Finder. The rules live in open_url_policy.h. */
static void macos_open_local_path(NSString *link_path) {
    const char *cpath = link_path.fileSystemRepresentation;
    if (!cpath || !cpath[0]) return;
    /* Judged, opened and revealed as what it resolves to: a symbolic link's
     * own name said nothing of what the opener would run. */
    char real[PATH_MAX];
    struct stat st;
    if (!liu_open_path_resolve(cpath, real, sizeof real, &st)) return;
    cpath = real;
    NSString *path = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:real
                                                                                 length:strlen(real)];
    if (!path) return;
    bool bundle = S_ISDIR(st.st_mode) &&
                  [[NSWorkspace sharedWorkspace] isFilePackageAtPath:path];
    switch (liu_open_path_verdict(cpath, &st, bundle)) {
    case LIU_OPEN_PATH_OPEN:
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]];
        break;
    case LIU_OPEN_PATH_REVEAL:
        [[NSWorkspace sharedWorkspace] selectFile:path inFileViewerRootedAtPath:@""];
        break;
    case LIU_OPEN_PATH_REFUSE:
    default:
        break;
    }
}

bool platform_trash_supported(void) { return true; }

/* NSFileManager's own Trash move: Finder's "Put Back" works on the result,
 * and a volume with no Trash (most network shares) answers NO instead of
 * deleting. The item itself is moved — a symbolic link, not its target. */
bool platform_trash_path(const char *path) {
    if (!path || !path[0]) return false;
    @autoreleasepool {
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *p = [fm stringWithFileSystemRepresentation:path length:strlen(path)];
        if (!p) return false;
        NSError *err = nil;
        return [fm trashItemAtURL:[NSURL fileURLWithPath:p]
                 resultingItemURL:nil error:&err] ? true : false;
    }
}

void platform_fatal_alert(const char *title, const char *detail) {
    fprintf(stderr, "liu: %s\n", (title && title[0]) ? title : "cannot continue");
    if (detail && detail[0]) fprintf(stderr, "liu: %s\n", detail);
    if (liu_dev_env("LIU_NO_ACTIVATE")) return;
    @autoreleasepool {
        NSString *t = (title && title[0]) ? [NSString stringWithUTF8String:title] : nil;
        NSString *d = (detail && detail[0]) ? [NSString stringWithUTF8String:detail] : nil;
        if (!t) t = @"Liu cannot continue";
        if (!d) d = @"No further detail is available.";

        NSAlert *alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleCritical;
        alert.messageText = t;
        alert.informativeText = d;
        [alert addButtonWithTitle:@"Quit"];
        [alert addButtonWithTitle:@"Copy Details"];
        /* The first-run intro sits one level above the menu bar and takes
         * every click; an alert under it could not be answered. */
        [[alert window] setLevel:kCGMainMenuWindowLevel + 2];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp activateIgnoringOtherApps:YES];
        if ([alert runModal] == NSAlertSecondButtonReturn) {
            NSPasteboard *pb = mac_app_pasteboard();
            [pb clearContents];
            [pb setString:[NSString stringWithFormat:@"%@\n%@", t, d]
                  forType:NSPasteboardTypeString];
        }
        [alert release];
    }
}

void platform_open_url(const char *url) {
    if (!url || !url[0]) return;
    @autoreleasepool {
        NSString *str = [NSString stringWithUTF8String:url];
        if (!str) return;
        /* Distinguish a real URL from a filesystem path. NSURL URLWithString
         * silently produces a half-baked NSURL for bare paths (no scheme),
         * which NSWorkspace then refuses to open, so a markdown link like
         * "./other.md" would do nothing. fileURLWithPath handles escaping and
         * file:// construction.
         *
         * Everything here is attacker-influenced text that a click turns into
         * an OS action, so it goes through open_url_policy.h first: only the
         * allow-listed schemes reach NSWorkspace, and a file target is opened
         * only when opening it cannot execute it. */
        char scheme[32];
        bool has_scheme = liu_url_scheme(url, scheme, sizeof scheme) &&
                          ([str rangeOfString:@"://"].location != NSNotFound ||
                           strcmp(scheme, "mailto") == 0 || strcmp(scheme, "tel") == 0);
        if (!has_scheme) {
            macos_open_local_path(str);
            return;
        }
        if (!liu_open_url_scheme_allowed(scheme)) {
            fprintf(stderr, "liu: refusing to open URL with scheme '%s'\n", scheme);
            return;
        }
        NSURL *nsurl = [NSURL URLWithString:str];
        if (!nsurl) return;
        if (nsurl.isFileURL) {
            if (nsurl.path) macos_open_local_path(nsurl.path);
            return;
        }
        [[NSWorkspace sharedWorkspace] openURL:nsurl];
    }
}

void platform_open_path(const char *path) {
    if (!path || !path[0]) return;
    @autoreleasepool {
        NSString *p = [NSString stringWithUTF8String:path];
        if (!p) return;
        NSTask *task = [[[NSTask alloc] init] autorelease];
        task.launchPath = @"/usr/bin/open";
        task.arguments = @[@"-e", p];
        @try {
            [task launch];
        } @catch (NSException *e) {
            (void)e;
        }
    }
}

/* =========================================================================
 * Native view + key injection (browser WKWebView)
 * ========================================================================= */

void *platform_window_native_view(PlatformWindow *w) {
    if (!w || !w->view) return NULL;
    return (__bridge void *)w->view;
}

/* -------------------------------------------------------------------------
 * Floating toast above WKWebView (Metal toast would render underneath)
 * ------------------------------------------------------------------------- */
@interface LiuNativeToastView : NSView
@property (nonatomic, strong) NSTextField *label;
@property (nonatomic, strong) NSColor *accentColor;
@property (nonatomic, strong) NSColor *badgeColor;
@property (nonatomic, assign) i32 kind;                   /* a NotificationKind */
@property (nonatomic, copy)   NSString *loadedFontPath;   /* cache key: path */
@property (nonatomic, assign) CGFloat    loadedFontPx;    /* cache key: size */
@end

@implementation LiuNativeToastView
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    self.wantsLayer = YES;
    /* masksToBounds must stay NO so the drop shadow (below) can cast outside
     * the bounds — the rounded fill + border still render from the layer's
     * cornerRadius/borderWidth without clipping. Mirrors draw_panel_chrome:
     * a rounded body with a soft shadow, so the browser toast reads identically
     * to the Metal toast used everywhere else. */
    self.layer.masksToBounds = NO;
    self.layer.cornerRadius = NOTIFICATION_RADIUS_PT;
    self.layer.borderWidth = 1.0;
    self.layer.shadowColor = [[NSColor blackColor] CGColor];
    self.layer.shadowOpacity = 0.24f;
    self.layer.shadowRadius = 12.0;
    self.layer.shadowOffset = NSMakeSize(0.0, -4.0);   /* cast downward */
    NSTextField *tf = [[NSTextField alloc] initWithFrame:NSZeroRect];
    tf.editable = NO;
    tf.selectable = NO;
    tf.bezeled = NO;
    tf.drawsBackground = NO;
    tf.bordered = NO;
    tf.alignment = NSTextAlignmentLeft;
    tf.lineBreakMode = NSLineBreakByClipping;
    tf.maximumNumberOfLines = 3;
    tf.cell.wraps = YES;
    tf.cell.scrollable = NO;
    tf.font = [NSFont monospacedSystemFontOfSize:12.0 weight:NSFontWeightMedium];
    tf.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [self addSubview:tf];
    self.label = tf;
    [tf release];
    self.loadedFontPath = @"";
    self.loadedFontPx = 0;
    return self;
}
/* Load and cache the app's terminal font (the same TTF the Metal toast renders
 * with) so the browser toast uses identical glyphs. Falls back to a monospaced
 * system font when the file is missing/unreadable. */
- (void)applyFontPath:(const char *)path px:(CGFloat)px {
    NSString *p = path && path[0] ? [NSString stringWithUTF8String:path] : @"";
    if (px < 6.0) px = 6.0;
    if ([p isEqualToString:self.loadedFontPath] && px == self.loadedFontPx) return;
    self.loadedFontPath = p;
    self.loadedFontPx = px;

    NSFont *font = nil;
    if (p.length) {
        NSData *data = [NSData dataWithContentsOfFile:p];
        if (data) {
            CGDataProviderRef prov =
                CGDataProviderCreateWithCFData((CFDataRef)data);
            if (prov) {
                CGFontRef cg = CGFontCreateWithDataProvider(prov);
                CGDataProviderRelease(prov);
                if (cg) {
                    CTFontRef ct = CTFontCreateWithGraphicsFont(cg, px, NULL, NULL);
                    CGFontRelease(cg);
                    /* MRC file: balance the +1 from Create with autorelease; the
                     * text field retains its font when assigned below. */
                    if (ct) font = [(NSFont *)ct autorelease];
                }
            }
        }
    }
    self.label.font = font ? font
        : [NSFont monospacedSystemFontOfSize:px weight:NSFontWeightMedium];
}
- (void)layout {
    [super layout];
    NSRect b = self.bounds;
    NSUInteger lines = [[self.label.stringValue componentsSeparatedByString:@"\n"] count];
    CGFloat textH = lines * 16.0 + (lines - 1) * NOTIFICATION_LINE_GAP_PT;
    CGFloat x = NOTIFICATION_PAD_PT + NOTIFICATION_TILE_PT + NOTIFICATION_GAP_PT;
    self.label.frame = NSMakeRect(x - 2.0, floor((b.size.height - textH) * 0.5),
                                 b.size.width - x - NOTIFICATION_PAD_PT + 4.0, textH);
}
- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];
    CGFloat pad = NOTIFICATION_PAD_PT, tile = NOTIFICATION_TILE_PT;
    CGFloat y = floor((self.bounds.size.height - tile) * 0.5);
    [self.badgeColor setFill];
    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(pad, y, tile, tile)
                                    xRadius:NOTIFICATION_BADGE_RADIUS_PT yRadius:NOTIFICATION_BADGE_RADIUS_PT] fill];
    /* The GPU notice's marks are Lucide's 24-unit art at the icon size; the
     * same coordinates here, scaled into the tile's centre, so the browser
     * notice carries the identical mark with no icon-font dependency. Lucide's
     * y runs down, AppKit's up. */
    CGFloat s = NOTIFICATION_ICON_PT / 24.0;
    CGFloat ox = pad + (tile - NOTIFICATION_ICON_PT) * 0.5;
    CGFloat oy = y + (tile - NOTIFICATION_ICON_PT) * 0.5;
    #define LP(X, Y) NSMakePoint(ox + (X) * s, oy + (24.0 - (Y)) * s)
    [self.accentColor setStroke];
    [self.accentColor setFill];
    NSBezierPath *p = [NSBezierPath bezierPath];
    p.lineWidth = 2.0 * s;
    p.lineCapStyle = NSLineCapStyleRound;
    p.lineJoinStyle = NSLineJoinStyleRound;
    switch (self.kind) {
    case 1:   /* success: a check */
        [p moveToPoint:LP(20, 6)]; [p lineToPoint:LP(9, 17)]; [p lineToPoint:LP(4, 12)];
        [p stroke];
        break;
    case 2:   /* warning: a triangle, a bar and a dot */
        [p moveToPoint:LP(12, 3.5)]; [p lineToPoint:LP(21.4, 19.5)];
        [p lineToPoint:LP(2.6, 19.5)]; [p closePath];
        [p moveToPoint:LP(12, 9)]; [p lineToPoint:LP(12, 13)];
        [p moveToPoint:LP(12, 17)]; [p lineToPoint:LP(12.01, 17)];
        [p stroke];
        break;
    case 3:   /* error: a ring and a cross */
        [p appendBezierPathWithOvalInRect:NSMakeRect(ox + 2 * s, oy + 2 * s, 20 * s, 20 * s)];
        [p moveToPoint:LP(15, 9)]; [p lineToPoint:LP(9, 15)];
        [p moveToPoint:LP(9, 9)];  [p lineToPoint:LP(15, 15)];
        [p stroke];
        break;
    default:  /* info: a ring, a bar and a dot */
        [p appendBezierPathWithOvalInRect:NSMakeRect(ox + 2 * s, oy + 2 * s, 20 * s, 20 * s)];
        [p moveToPoint:LP(12, 16)]; [p lineToPoint:LP(12, 12)];
        [p moveToPoint:LP(12, 8)];  [p lineToPoint:LP(12.01, 8)];
        [p stroke];
        break;
    }
    #undef LP
}
- (BOOL)acceptsFirstResponder { return NO; }
- (NSView *)hitTest:(NSPoint)point {
    /* Pass clicks through — toast is informational only. */
    (void)point;
    return nil;
}
- (void)dealloc {
    [_label release];
    [_accentColor release];
    [_badgeColor release];
    [_loadedFontPath release];
    [super dealloc];
}
@end

/* One toast per window (PlatformWindow.toast), never a file-static.
 *
 * Retain balance: alloc (+1) / addSubview (+1 by the superview) /
 * removeFromSuperview (-1) / release (-1) → 0, so this is the only place the
 * view is freed and it is safe to call on a window that never showed one. */
static void platform_toast_destroy_for(PlatformWindow *w) {
    if (!w) return;
    for (i32 i = 0; i < PLATFORM_TOAST_SLOTS; i++) {
        if (!w->toast[i]) continue;
        [w->toast[i] removeFromSuperview];
        [w->toast[i] release];
        w->toast[i] = nil;
    }
}

void platform_toast_sync(PlatformWindow *w, i32 slot, i32 kind, const char *message,
                         f32 x_pt, f32 y_pt, f32 w_pt, f32 h_pt, f32 alpha,
                         f32 bg_r, f32 bg_g, f32 bg_b,
                         f32 fg_r, f32 fg_g, f32 fg_b,
                         f32 bd_r, f32 bd_g, f32 bd_b,
                         f32 accent_r, f32 accent_g, f32 accent_b,
                         f32 shadow_alpha,
                         const char *font_path, f32 font_px) {
    if (!w || !w->view || slot < 0 || slot >= PLATFORM_TOAST_SLOTS) return;
    @autoreleasepool {
        LiuNativeToastView *toast = w->toast[slot];
        if (!message || !message[0] || alpha <= 0.02f || w_pt < 8.0f || h_pt < 8.0f) {
            if (toast) { toast.hidden = YES; toast.alphaValue = 0.0; }
            return;
        }
        NSView *parent = w->view;
        if (!toast) {
            toast = [[LiuNativeToastView alloc] initWithFrame:NSZeroRect];
            w->toast[slot] = toast;
        }
        /* Re-added unconditionally so it stays above any WKWebView the browser
         * put back this frame; -addSubview:positioned: on the current superview
         * just re-orders. */
        [parent addSubview:toast positioned:NSWindowAbove relativeTo:nil];

        toast.frame = NSMakeRect(x_pt, y_pt, w_pt, h_pt);
        toast.hidden = NO;
        toast.alphaValue = (CGFloat)alpha;
        toast.layer.backgroundColor =
            [[NSColor colorWithSRGBRed:bg_r green:bg_g blue:bg_b alpha:0.98]
                CGColor];
        /* Border uses the theme border colour (like draw_panel_chrome), so it
         * matches the Metal notice exactly. */
        toast.layer.borderColor =
            [[NSColor colorWithSRGBRed:bd_r green:bd_g blue:bd_b alpha:1.0]
                CGColor];
        toast.layer.shadowOpacity = shadow_alpha;
        toast.accentColor = [NSColor colorWithSRGBRed:accent_r green:accent_g blue:accent_b alpha:1.0];
        toast.badgeColor = [NSColor colorWithSRGBRed:bg_r + (accent_r - bg_r) * 0.14
                                            green:bg_g + (accent_g - bg_g) * 0.14
                                             blue:bg_b + (accent_b - bg_b) * 0.14 alpha:1.0];
        toast.kind = kind;
        [toast applyFontPath:font_path px:(CGFloat)font_px];
        NSString *value = [NSString stringWithUTF8String:message];
        NSMutableParagraphStyle *paragraph = [[[NSMutableParagraphStyle alloc] init] autorelease];
        paragraph.minimumLineHeight = 16.0;
        paragraph.maximumLineHeight = 16.0;
        paragraph.lineSpacing = NOTIFICATION_LINE_GAP_PT;
        paragraph.lineBreakMode = NSLineBreakByClipping;
        toast.label.attributedStringValue = [[[NSAttributedString alloc] initWithString:value ? value : @""
            attributes:@{NSFontAttributeName:toast.label.font,
                         NSForegroundColorAttributeName:[NSColor colorWithSRGBRed:fg_r green:fg_g blue:fg_b alpha:1.0],
                         NSParagraphStyleAttributeName:paragraph}] autorelease];
        toast.label.alphaValue = 1.0;
        [toast setNeedsLayout:YES];
        [toast setNeedsDisplay:YES];
    }
}

void platform_toast_trim(PlatformWindow *w, i32 keep) {
    /* Only `w`'s toast is hidden, so an idle frame in one window cannot blank
     * another window's toast. */
    if (!w) return;
    if (keep < 0) keep = 0;
    @autoreleasepool {
        for (i32 i = keep; i < PLATFORM_TOAST_SLOTS; i++) {
            if (!w->toast[i]) continue;
            w->toast[i].hidden = YES;
            w->toast[i].alphaValue = 0.0;
        }
    }
}

void platform_toast_hide(PlatformWindow *w) {
    platform_toast_trim(w, 0);
}

void platform_push_key_down(i32 key, u32 mods, bool repeat) {
    push_event((PlatformEvent){
        .type = EVENT_KEY_DOWN,
        .key = { .key = (KeyCode)key, .mods = mods, .repeat = repeat }
    });
}

bool platform_is_liu_chrome_cmd_key(i32 key, u32 mods) {
    if (!(mods & MOD_SUPER) || key == KEY_UNKNOWN) return false;
    /* K14 chrome subset — page-focused WebView must not steal these. */
    switch (key) {
        case KEY_K:
        case KEY_W:
        case KEY_T:
        /* Browser chrome shortcuts: reload (+Shift hard, +Opt reset), focus
         * the URL bar, stop, and history navigation. A web page has no claim
         * on any of these, and without forwarding they only worked while the
         * chrome — not the page — had focus. */
        case KEY_R:
        case KEY_L:
        case KEY_PERIOD:
        case KEY_LEFT_BRACKET:
        case KEY_RIGHT_BRACKET:
        case KEY_1: case KEY_2: case KEY_3: case KEY_4: case KEY_5:
        case KEY_6: case KEY_7: case KEY_8: case KEY_9:
            return true;
        case KEY_S:
            /* Sites manager is Cmd+Shift+S; bare Cmd+S stays with WebView. */
            return (mods & MOD_SHIFT) != 0;
        case KEY_UP:
        case KEY_DOWN:
            return true;
        /* With Option the arrows take the keyboard to the pane beside (and
         * with Ctrl as well nudge the divider); Cmd+Left / Right alone stay
         * the page's text-field moves. */
        case KEY_LEFT:
        case KEY_RIGHT:
            return (mods & MOD_ALT) != 0;
        /* Cmd+Shift+Enter fills the tab with the focused pane. */
        case KEY_ENTER:
            return (mods & MOD_SHIFT) != 0;
        default:
            return false;
    }
}

i32 platform_map_nsevent_key(void *nsevent) {
    if (!nsevent) return KEY_UNKNOWN;
    NSEvent *e = (__bridge NSEvent *)nsevent;
    return (i32)map_key(e.keyCode);
}

u32 platform_map_nsevent_mods(void *nsevent) {
    if (!nsevent) return MOD_NONE;
    NSEvent *e = (__bridge NSEvent *)nsevent;
    return get_mods(e.modifierFlags);
}

/* =========================================================================
 * Frame-scoped autorelease pool
 *
 * An explicit NSAutoreleasePool rather than an @autoreleasepool block: the main
 * loop lives in main.c (plain C11) and has to open the pool in one place and
 * close it in another, which the block form cannot express. This file is
 * compiled MRC (see the retain/release pairs throughout), where the pool object
 * is the supported public API for exactly this pattern — the runtime's
 * objc_autoreleasePoolPush/Pop are not declared in the SDK headers.
 *
 * Pools nest as a stack, so the inner @autoreleasepool blocks used elsewhere in
 * this file drain normally inside a frame.
 * ========================================================================= */

void *platform_autorelease_pool_push(void) {
    return [[NSAutoreleasePool alloc] init];
}

void platform_autorelease_pool_pop(void *pool) {
    if (pool) [(NSAutoreleasePool *)pool drain];
}

/* =========================================================================
 * Time
 * ========================================================================= */

f64 platform_time_sec(void) {
    u64 now = mach_absolute_time();
    f64 sec = (f64)now * g_platform.timebase_info.numer / g_platform.timebase_info.denom / 1e9;
    return sec - g_platform.time_base;
}

f64 platform_time_origin(void) { return g_platform.time_base; }

void platform_sleep_ms(u32 ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* =========================================================================
 * GL loader — macOS links OpenGL.framework, symbols are already available
 * ========================================================================= */

bool platform_gl_load(void) {
#ifdef USE_METAL
    /* No GL loading needed for Metal */
    return true;
#else
    /* On macOS with NSOpenGL, GL functions are linked directly.
     * No dlsym/wglGetProcAddress needed for GL 3.2 core profile. */
    return true;
#endif
}

void platform_update_tabs(PlatformWindow *w, const NativeTab *tabs, i32 count, i32 active) {
    (void)w; (void)tabs; (void)count; (void)active;
    /* Tab rendering handled by OpenGL */
}

f32 platform_tab_bar_height(PlatformWindow *w) {
    (void)w;
    return 28.0f; /* matches the accessory view height */
}

void platform_set_cursor(CursorType type) {
    @autoreleasepool {
        switch (type) {
        case CURSOR_TEXT:     [[NSCursor IBeamCursor] set]; break;
        case CURSOR_RESIZE_H: [[NSCursor resizeLeftRightCursor] set]; break;
        case CURSOR_RESIZE_V: [[NSCursor resizeUpDownCursor] set]; break;
        case CURSOR_POINTER:  [[NSCursor pointingHandCursor] set]; break;
        default:              [[NSCursor arrowCursor] set]; break;
        }
    }
}

bool platform_get_system_symbol_rgba(const char *name, i32 pixel_size,
                                     u8 r, u8 g, u8 b, u8 a,
                                     const u8 **pixels, i32 *width, i32 *height) {
    if (!name || !name[0] || pixel_size <= 0 || pixel_size > 4096 ||
        !pixels || !width || !height) return false;

    u32 rgba = ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | (u32)a;
    for (i32 i = 0; i < MAX_SYMBOL_CACHE; i++) {
        SymbolCacheEntry *entry = &g_symbol_cache[i];
        if (!entry->valid) continue;
        if (entry->pixel_size == pixel_size &&
            entry->rgba == rgba &&
            strcmp(entry->name, name) == 0) {
            entry->last_use = ++g_symbol_use_tick;
            *pixels = entry->pixels;
            *width = entry->width;
            *height = entry->height;
            return true;
        }
    }

    @autoreleasepool {
        if (!@available(macOS 11.0, *)) return false;

        NSString *symbol_name = [NSString stringWithUTF8String:name];
        if (!symbol_name) return false;

        NSImage *image = [NSImage imageWithSystemSymbolName:symbol_name accessibilityDescription:nil];
        if (!image) return false;

        /* Rasterization pipeline:
         *
         *   1. A CGBitmapContext with an explicit sRGB color space, so the
         *      pixel buffer matches the MTLPixelFormatRGBA8Unorm_sRGB
         *      texture. A calibrated-RGB bitmap would have its gamma decoded
         *      as if it were sRGB, darkening edges and softening the AA.
         *
         *   2. Drawn at native pixel_size, with no 2× supersample: SF Symbols
         *      are vector, and CoreGraphics' high-quality AA at the target
         *      size is sharper than a bilinear box-filter from a 2× source.
         *
         *   3. NSFontWeightRegular: Semibold is too heavy for the small
         *      toolbar icons (strokes run into one another), and Regular
         *      matches the SF Symbol weight used in macOS menu bars / Finder. */
        NSInteger side = (NSInteger)pixel_size;
        if (side < 4) side = 4;
        CGFloat pt_size = (CGFloat)side;

        NSImageSymbolConfiguration *size_cfg =
            [NSImageSymbolConfiguration configurationWithPointSize:pt_size
                                                            weight:NSFontWeightRegular
                                                             scale:NSImageSymbolScaleMedium];
        NSImage *configured = [image imageWithSymbolConfiguration:size_cfg];
        if (configured) image = configured;

        if (@available(macOS 13.0, *)) {
            NSImageSymbolConfiguration *mono_cfg = [NSImageSymbolConfiguration configurationPreferringMonochrome];
            NSImage *mono = [image imageWithSymbolConfiguration:mono_cfg];
            if (mono) image = mono;
        }

        /* sRGB CGBitmapContext, premultiplied alpha. Matches the upload
         * format consumed by metal_image_texture so no gamma surprises. */
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        if (!cs) return false;
        usize bpr = (usize)side * 4;
        u8 *raw = calloc((usize)side * bpr, 1);
        if (!raw) { CGColorSpaceRelease(cs); return false; }
        CGContextRef cg = CGBitmapContextCreate(raw, (usize)side, (usize)side, 8,
            bpr, cs,
            (CGBitmapInfo)(kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big));
        CGColorSpaceRelease(cs);
        if (!cg) { free(raw); return false; }

        CGContextSetShouldAntialias(cg, YES);
        CGContextSetAllowsAntialiasing(cg, YES);
        CGContextSetInterpolationQuality(cg, kCGInterpolationHigh);
        /* Subpixel positioning is great for body text but works AGAINST
         * a small, single-icon raster — symbols read crisper when their
         * stroke endpoints align to integer pixels. */
        CGContextSetShouldSubpixelPositionFonts(cg, NO);
        CGContextSetShouldSubpixelQuantizeFonts(cg, NO);

        NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithCGContext:cg flipped:NO];
        if (!ctx) {
            CGContextRelease(cg);
            free(raw);
            return false;
        }
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:ctx];
        /* Preserve the symbol's natural aspect ratio inside the square canvas:
         * SF Symbols like arrow.left/right are wider than tall, and drawing them
         * into a full side×side rect stretches them. Fit-and-center instead. */
        NSSize isz = image.size;
        NSRect dstRect = NSMakeRect(0, 0, side, side);
        if (isz.width > 0.0 && isz.height > 0.0) {
            CGFloat aspect = isz.width / isz.height;
            CGFloat dw = side, dh = side;
            if (aspect >= 1.0) dh = (CGFloat)side / aspect;
            else               dw = (CGFloat)side * aspect;
            dstRect = NSMakeRect(((CGFloat)side - dw) * 0.5,
                                 ((CGFloat)side - dh) * 0.5, dw, dh);
        }
        [image drawInRect:dstRect
                 fromRect:NSZeroRect
                operation:NSCompositingOperationSourceOver
                 fraction:1.0
           respectFlipped:NO
                    hints:@{ NSImageHintInterpolation: @(NSImageInterpolationHigh) }];
        [NSGraphicsContext restoreGraphicsState];
        CGContextRelease(cg);

        usize total = (usize)side * (usize)side * 4;
        u8 *dst = calloc(total, 1);   /* zeroed → rows shifted out stay transparent */
        if (!dst) { free(raw); return false; }

        /* Vertically center the glyph by its visible alpha content. SF Symbol
         * canvases carry baseline-relative ascender/descender margins, so a
         * geometric draw can leave a symbol (notably xmark) sitting high in the
         * bitmap. Scan the alpha bounding rows and shift so the visible glyph is
         * centered — callers (e.g. the tab close ×) then get a centered icon. */
        i32 amin = (i32)side, amax = -1;
        for (i32 yy = 0; yy < (i32)side; yy++) {
            const u8 *srow = raw + (usize)yy * bpr;
            for (i32 xx = 0; xx < (i32)side; xx++) {
                if (srow[(usize)xx * 4 + 3] > 8) {
                    if (yy < amin) amin = yy;
                    if (yy > amax) amax = yy;
                    break;
                }
            }
        }
        i32 vshift = (amax >= amin) ? ((i32)side / 2 - (amin + amax) / 2) : 0;

        /* Tinting: SF Symbols come back as monochrome with the shape's
         * anti-aliased coverage in the alpha channel. Premultiplied storage
         * lets the requested RGB be painted pre-multiplied by that
         * coverage. The vshift re-centers the visible glyph vertically. */
        for (i32 yy = 0; yy < (i32)side; yy++) {
            i32 sy = yy - vshift;
            if (sy < 0 || sy >= (i32)side) continue;   /* dst already transparent */
            const u8 *srow = raw + (usize)sy * bpr;
            u8       *drow = dst + (usize)yy * bpr;
            for (i32 xx = 0; xx < (i32)side; xx++) {
                usize o = (usize)xx * 4;
                u8 cov = srow[o + 3];                      /* coverage 0..255 */
                u32 ea = ((u32)cov * (u32)a) / 255U;       /* effective alpha */
                drow[o + 0] = (u8)(((u32)r * ea) / 255U);  /* premul R */
                drow[o + 1] = (u8)(((u32)g * ea) / 255U);  /* premul G */
                drow[o + 2] = (u8)(((u32)b * ea) / 255U);  /* premul B */
                drow[o + 3] = (u8)ea;
            }
        }
        free(raw);

        /* Prefer an empty slot; otherwise evict the least-recently-used entry
         * (NOT always slot 0 — that thrashed the working set and, combined with
         * malloc reusing freed addresses, let the GPU image cache alias a
         * reused pixel buffer to a stale texture). */
        i32 slot = -1;
        u64 oldest = ~0ULL;
        for (i32 i = 0; i < MAX_SYMBOL_CACHE; i++) {
            if (!g_symbol_cache[i].valid) { slot = i; break; }
            if (g_symbol_cache[i].last_use < oldest) {
                oldest = g_symbol_cache[i].last_use;
                slot = i;
            }
        }
        if (slot < 0) slot = 0;

        SymbolCacheEntry *entry = &g_symbol_cache[slot];
        if (entry->pixels) free(entry->pixels);
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->name, sizeof(entry->name), "%s", name);
        entry->pixel_size = pixel_size;
        entry->rgba = rgba;
        entry->pixels = dst;
        entry->width = (i32)side;
        entry->height = (i32)side;
        entry->last_use = ++g_symbol_use_tick;
        entry->valid = true;

        *pixels = entry->pixels;
        *width = entry->width;
        *height = entry->height;
        return true;
    }
}

/* =========================================================================
 * Global hotkey registration
 * ========================================================================= */

/* Map macOS keycode for backtick (grave accent) */
#define MACOS_KEYCODE_GRAVE 0x32

void platform_register_global_hotkey(u32 key, u32 mods, GlobalHotkeyCallback cb, void *userdata) {
    @autoreleasepool {
        /* Remove existing monitor if any */
        if (g_global_hotkey.global_monitor) {
            [NSEvent removeMonitor:g_global_hotkey.global_monitor];
            g_global_hotkey.global_monitor = nil;
        }
        if (g_global_hotkey.local_monitor) {
            [NSEvent removeMonitor:g_global_hotkey.local_monitor];
            g_global_hotkey.local_monitor = nil;
        }

        g_global_hotkey.callback = cb;
        g_global_hotkey.userdata = userdata;
        g_global_hotkey.key = key;
        g_global_hotkey.mods = mods;

        /*
         * Use addGlobalMonitorForEventsMatchingMask for system-wide hotkey detection.
         * This monitors key events even when the app is not focused.
         * Also add a local monitor for when the app IS focused.
         */
        NSEventMask mask = NSEventMaskKeyDown;

        /* Global monitor (app not focused) */
        g_global_hotkey.global_monitor = [NSEvent addGlobalMonitorForEventsMatchingMask:mask
            handler:^(NSEvent *event) {
                u32 emods = get_mods(event.modifierFlags);
                KeyCode ekc = map_key(event.keyCode);

                /* Check for backtick specifically (keyCode 0x32) */
                bool key_match = (ekc == g_global_hotkey.key) ||
                    (g_global_hotkey.key == KEY_UNKNOWN && event.keyCode == MACOS_KEYCODE_GRAVE);

                if (key_match && emods == g_global_hotkey.mods) {
                    if (g_global_hotkey.callback) {
                        dispatch_async(dispatch_get_main_queue(), ^{
                            g_global_hotkey.callback(g_global_hotkey.userdata);
                        });
                    }
                }
            }];

        /* Also register a local monitor so hotkey works when app is focused */
        g_global_hotkey.local_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
            handler:^NSEvent *(NSEvent *event) {
                u32 emods = get_mods(event.modifierFlags);
                KeyCode ekc = map_key(event.keyCode);

                bool key_match = (ekc == g_global_hotkey.key) ||
                    (g_global_hotkey.key == KEY_UNKNOWN && event.keyCode == MACOS_KEYCODE_GRAVE);

                if (key_match && emods == g_global_hotkey.mods) {
                    /* The quake window's hotkey (its only user): with quake
                     * off the key is the pane's again — Ctrl+` is NUL, which
                     * an editor sets a mark with. */
                    PlatformWindow *hw = (PlatformWindow *)g_global_hotkey.userdata;
                    if (!platform_window_alive(hw) || !hw->quake_enabled) return event;
                    if (g_global_hotkey.callback) {
                        g_global_hotkey.callback(g_global_hotkey.userdata);
                    }
                    return nil; /* consume event */
                }
                return event;
            }];
    }
}

/* =========================================================================
 * Quake mode
 * ========================================================================= */

void platform_set_quake_params(f32 height_ratio, f32 anim_duration) {
    if (height_ratio > 0.0f) g_quake.height_ratio = height_ratio;
    if (anim_duration > 0.0f) g_quake.anim_duration = anim_duration;
}

void platform_set_quake_mode(PlatformWindow *w, bool enable) {
    if (!w || !w->ns_window) return;
    @autoreleasepool {
        /* Compared against THIS window's flag, so a second window can enter
         * quake mode while another one is in it. */
        if (enable == w->quake_enabled) return;
        NSWindow *win = w->ns_window;

        if (enable) {
            /* Save current window state — on the window it belongs to. */
            w->quake_saved_frame = [win frame];
            w->quake_saved_style = [win styleMask];
            w->quake_saved_level = [win level];
            w->quake_enabled = true;
            w->quake_visible = true;

            if (g_quake.height_ratio <= 0.0f) g_quake.height_ratio = 0.4f;
            if (g_quake.anim_duration <= 0.0f) g_quake.anim_duration = 0.2f;

            /* Configure window for quake mode */
            [win setStyleMask:NSWindowStyleMaskBorderless |
                              NSWindowStyleMaskFullSizeContentView];
            [win setLevel:NSFloatingWindowLevel];
            [win setHasShadow:YES];
            [win setOpaque:YES];
            /* Written behind liu_window_apply_opaque's back, so drop its memo:
             * otherwise the next appearance apply sees "already translucent",
             * skips the write, and quake mode stays opaque for good. */
            w->opaque_applied_valid = false;

            /* Set frame to full width, specified height, top of screen */
            NSScreen *screen = [win screen];
            if (!screen) screen = [NSScreen mainScreen];
            NSRect screenFrame = [screen visibleFrame];
            f32 quake_h = screenFrame.size.height * g_quake.height_ratio;

            NSRect quakeFrame = NSMakeRect(
                screenFrame.origin.x,
                screenFrame.origin.y + screenFrame.size.height - quake_h,
                screenFrame.size.width,
                quake_h
            );

            /* Animate slide down */
            NSRect offscreenFrame = quakeFrame;
            offscreenFrame.origin.y = screenFrame.origin.y + screenFrame.size.height;
            [win setFrame:offscreenFrame display:NO];
            [win makeKeyAndOrderFront:nil];

            w->quake_animating = true;
            [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
                ctx.duration = g_quake.anim_duration;
                ctx.timingFunction = [CAMediaTimingFunction functionWithName:
                    kCAMediaTimingFunctionEaseOut];
                [[win animator] setFrame:quakeFrame display:YES];
            } completionHandler:^{
                /* Runs ~0.2 s later: the window may have been destroyed since.
                 * The block captured a raw pointer, so ask the registry before
                 * dereferencing it. (`win` itself is retained by the block.) */
                if (!platform_window_alive(w)) return;
                w->quake_animating = false;
                PlatformEvent e = {
                    .type = EVENT_RESIZE,
                    .resize = {
                        .width = (i32)quakeFrame.size.width,
                        .height = (i32)quakeFrame.size.height
                    }
                };
                /* Animation completions run outside sendEvent: — stamp the
                 * source or the resize lands on whatever window is key. */
                e.window = w;
                push_event(e);
            }];

            [NSApp activateIgnoringOtherApps:YES];
        } else {
            /* Restore original window state */
            w->quake_enabled = false;
            w->quake_visible = false;

            [win setStyleMask:w->quake_saved_style];
            [win setLevel:w->quake_saved_level];
            /* AppKit resets `opaque` and `backgroundColor` when the style mask
             * changes, so the opaque memo is now describing a window state that
             * no longer exists: it would claim "translucent already applied"
             * over an opaque window and skip every later write, losing
             * translucency AND blur permanently. Same reason the enable branch
             * above invalidates it. */
            w->opaque_applied_valid = false;
            [win setFrame:w->quake_saved_frame display:YES animate:YES];
            [win setTitlebarAppearsTransparent:YES];
            [win setTitleVisibility:NSWindowTitleHidden];
        }
    }
}

void platform_toggle_quake_window(PlatformWindow *w) {
    @autoreleasepool {
        /* The hotkey's block runs later, on the main queue: its window may be
         * gone by then — compared, never dereferenced, until it is known. */
        if (!platform_window_alive(w)) return;
        /* This window's own state. main.c routes EVENT_BLUR here, so answering
         * from a global slid window 2 off-screen using window 1's settings. */
        if (!w || !w->ns_window) return;
        if (!w->quake_enabled) return;
        if (w->quake_animating) return;
        NSWindow *win = w->ns_window;

        NSScreen *screen = [win screen];
        if (!screen) screen = [NSScreen mainScreen];
        NSRect screenFrame = [screen visibleFrame];
        f32 quake_h = screenFrame.size.height * g_quake.height_ratio;

        if (w->quake_visible) {
            /* Slide up and hide */
            w->quake_animating = true;
            NSRect offscreenFrame = NSMakeRect(
                screenFrame.origin.x,
                screenFrame.origin.y + screenFrame.size.height,
                screenFrame.size.width,
                quake_h
            );
            [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
                ctx.duration = g_quake.anim_duration;
                ctx.timingFunction = [CAMediaTimingFunction functionWithName:
                    kCAMediaTimingFunctionEaseIn];
                [[win animator] setFrame:offscreenFrame display:YES];
            } completionHandler:^{
                /* The window can be closed mid-slide; `w` is a raw pointer. */
                if (!platform_window_alive(w)) return;
                w->quake_animating = false;
                w->quake_visible = false;
                [win orderOut:nil];
            }];
        } else {
            /* Slide down and show */
            NSRect offscreenFrame = NSMakeRect(
                screenFrame.origin.x,
                screenFrame.origin.y + screenFrame.size.height,
                screenFrame.size.width,
                quake_h
            );
            [win setFrame:offscreenFrame display:NO];
            [win makeKeyAndOrderFront:nil];

            NSRect quakeFrame = NSMakeRect(
                screenFrame.origin.x,
                screenFrame.origin.y + screenFrame.size.height - quake_h,
                screenFrame.size.width,
                quake_h
            );

            w->quake_animating = true;
            w->quake_visible = true;
            [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
                ctx.duration = g_quake.anim_duration;
                ctx.timingFunction = [CAMediaTimingFunction functionWithName:
                    kCAMediaTimingFunctionEaseOut];
                [[win animator] setFrame:quakeFrame display:YES];
            } completionHandler:^{
                if (!platform_window_alive(w)) return;
                w->quake_animating = false;
                PlatformEvent e = {
                    .type = EVENT_RESIZE,
                    .resize = {
                        .width = (i32)quakeFrame.size.width,
                        .height = (i32)quakeFrame.size.height
                    }
                };
                /* Outside sendEvent: — stamp the source explicitly. */
                e.window = w;
                push_event(e);
            }];

            [NSApp activateIgnoringOtherApps:YES];
        }
    }
}

bool platform_is_quake_mode(PlatformWindow *w) {
    /* Answers for `w`. */
    return w && w->quake_enabled;
}

/* =========================================================================
 * GPU device accessors (Metal / OpenGL)
 * ========================================================================= */

void *platform_get_gpu_device(PlatformWindow *w) {
#ifdef USE_METAL
    return (__bridge void *)w->mtl_device;
#else
    (void)w; return NULL;
#endif
}

void *platform_get_gpu_layer(PlatformWindow *w) {
#ifdef USE_METAL
    return (__bridge void *)w->mtl_layer;
#else
    (void)w; return NULL;
#endif
}

void *platform_get_gpu_queue(PlatformWindow *w) {
#ifdef USE_METAL
    return (__bridge void *)w->mtl_queue;
#else
    (void)w; return NULL;
#endif
}

/* Native settings panel built with NSPanel, NSToolbar, and Auto Layout. It
 * presents Appearance, Typography, and Behavior sections with native
 * picklists, sliders, color controls, a vibrancy backdrop, and a sticky
 * Done/Reset row. The platform_show_settings(...) C entrypoint remains
 * unchanged. */

static NSPanel *g_settings_panel = nil;
static SettingsFontCallback  g_on_font = NULL;
static SettingsThemeCallback g_on_theme = NULL;
static SettingsValueCallback g_on_value = NULL;

@interface LiuSettingsController : NSObject <NSToolbarDelegate, NSWindowDelegate>
@property (nonatomic, strong) NSPanel *panel;
@property (nonatomic, strong) NSMutableArray<NSString *> *fontNames;
@property (nonatomic, strong) NSMutableArray<NSString *> *fontPaths;
@property (nonatomic, strong) NSMutableArray *fontInstalled;
@property (nonatomic, strong) NSMutableArray<NSString *> *themeNames;
@property (nonatomic, copy)   NSString *currentFont;
@property (nonatomic, copy)   NSString *currentTheme;
@property (nonatomic) f32 fontSize;
@property (nonatomic) f32 fontWeight;
@property (nonatomic) f32 opacity;
@property (nonatomic) f32 tabSleepMinutes;

/* Section views, swapped in/out by the toolbar */
@property (nonatomic, strong) NSView *sectionAppearance;
@property (nonatomic, strong) NSView *sectionTypography;
@property (nonatomic, strong) NSView *sectionBehavior;
@property (nonatomic, strong) NSView *currentSection;
@property (nonatomic, strong) NSView *sectionContainer;

/* Live-value labels — updated on slider drag */
@property (nonatomic, strong) NSTextField *fontSizeValueLabel; /* editable text input */
@property (nonatomic, strong) NSSlider    *fontSizeSlider;
@property (nonatomic, strong) NSSlider    *fontWeightSlider;
@property (nonatomic, strong) NSTextField *fontWeightValueLabel;
@property (nonatomic, strong) NSTextField *opacityValueLabel;
@property (nonatomic, strong) NSTextField *tabSleepValueLabel;
@end

@implementation LiuSettingsController

#pragma mark — UI helpers

/* Group box: subtle inset card with a header label, used as a
 * container for a related cluster of controls. Gives the panel its
 * "System Settings" sectioned look. */
- (NSView *)groupBoxWithTitle:(NSString *)title content:(NSView *)content {
    NSBox *box = [[[NSBox alloc] init] autorelease];
    box.translatesAutoresizingMaskIntoConstraints = NO;
    box.boxType = NSBoxCustom;
    box.borderColor = [NSColor.separatorColor colorWithAlphaComponent:0.5];
    box.borderWidth = 1.0;
    box.cornerRadius = 8.0;
    box.contentViewMargins = NSMakeSize(14, 12);
    box.fillColor = [NSColor.controlBackgroundColor colorWithAlphaComponent:0.35];
    box.title = @"";

    NSStackView *vstack = [[[NSStackView alloc] init] autorelease];
    vstack.translatesAutoresizingMaskIntoConstraints = NO;
    vstack.orientation = NSUserInterfaceLayoutOrientationVertical;
    vstack.alignment = NSLayoutAttributeLeading;
    vstack.spacing = 10;

    if (title.length > 0) {
        NSTextField *header = [NSTextField labelWithString:title.uppercaseString];
        header.font = [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold];
        header.textColor = NSColor.tertiaryLabelColor;
        [vstack addArrangedSubview:header];
    }
    [vstack addArrangedSubview:content];

    box.contentView = vstack;
    [NSLayoutConstraint activateConstraints:@[
        [vstack.topAnchor      constraintEqualToAnchor:box.contentView.topAnchor],
        [vstack.leadingAnchor  constraintEqualToAnchor:box.contentView.leadingAnchor],
        [vstack.trailingAnchor constraintEqualToAnchor:box.contentView.trailingAnchor],
        [vstack.bottomAnchor   constraintEqualToAnchor:box.contentView.bottomAnchor],
    ]];
    return box;
}

/* Form row: a label on the left and a control on the right, both
 * stretching to the available width. Mirrors NSGridView semantics
 * without the alignment-set-up boilerplate. */
- (NSStackView *)rowWithLabel:(NSString *)labelText control:(NSView *)control {
    NSTextField *lbl = [NSTextField labelWithString:labelText];
    lbl.font = [NSFont systemFontOfSize:13];
    lbl.textColor = NSColor.labelColor;
    [lbl setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSStackView *row = [[[NSStackView alloc] init] autorelease];
    row.translatesAutoresizingMaskIntoConstraints = NO;
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment = NSLayoutAttributeCenterY;
    row.distribution = NSStackViewDistributionFill;
    row.spacing = 12;
    [row addArrangedSubview:lbl];
    [row addArrangedSubview:control];
    [lbl.widthAnchor constraintEqualToConstant:120].active = YES;
    return row;
}

#pragma mark — Section builders

- (NSView *)buildAppearanceSection {
    NSStackView *col = [[[NSStackView alloc] init] autorelease];
    col.translatesAutoresizingMaskIntoConstraints = NO;
    col.orientation = NSUserInterfaceLayoutOrientationVertical;
    col.alignment = NSLayoutAttributeLeading;
    col.spacing = 16;
    col.edgeInsets = NSEdgeInsetsMake(20, 24, 20, 24);

    /* — Theme picker — */
    NSPopUpButton *themePop = [[[NSPopUpButton alloc] init] autorelease];
    themePop.translatesAutoresizingMaskIntoConstraints = NO;
    themePop.bezelStyle = NSBezelStyleRounded;
    themePop.target = self;
    themePop.action = @selector(themePicked:);
    for (NSString *name in self.themeNames) {
        [themePop addItemWithTitle:name];
    }
    if (self.currentTheme.length > 0 &&
        [self.themeNames containsObject:self.currentTheme]) {
        [themePop selectItemWithTitle:self.currentTheme];
    }
    [themePop.widthAnchor constraintGreaterThanOrEqualToConstant:240].active = YES;

    NSStackView *themeRow = [self rowWithLabel:@"Theme" control:themePop];
    NSView *themeBox = [self groupBoxWithTitle:@"Color theme" content:themeRow];
    [col addArrangedSubview:themeBox];

    /* — Window opacity slider — */
    self.opacityValueLabel = [NSTextField labelWithString:
        [NSString stringWithFormat:@"%d %%", (int)(self.opacity * 100.0f + 0.5f)]];
    self.opacityValueLabel.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
    self.opacityValueLabel.textColor = NSColor.secondaryLabelColor;
    self.opacityValueLabel.alignment = NSTextAlignmentRight;
    [self.opacityValueLabel.widthAnchor constraintEqualToConstant:48].active = YES;

    NSSlider *opSlider = [[[NSSlider alloc] init] autorelease];
    opSlider.translatesAutoresizingMaskIntoConstraints = NO;
    opSlider.minValue = 30;
    opSlider.maxValue = 100;
    opSlider.doubleValue = (double)(self.opacity * 100.0f);
    opSlider.continuous = YES;
    opSlider.target = self;
    opSlider.action = @selector(opacityChanged:);

    NSStackView *opRowControls = [[[NSStackView alloc] init] autorelease];
    opRowControls.translatesAutoresizingMaskIntoConstraints = NO;
    opRowControls.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    opRowControls.alignment = NSLayoutAttributeCenterY;
    opRowControls.spacing = 10;
    [opRowControls addArrangedSubview:opSlider];
    [opRowControls addArrangedSubview:self.opacityValueLabel];

    NSStackView *opRow = [self rowWithLabel:@"Opacity" control:opRowControls];
    NSView *opBox = [self groupBoxWithTitle:@"Window" content:opRow];
    [col addArrangedSubview:opBox];

    [col.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    return col;
}

- (NSView *)buildTypographySection {
    NSStackView *col = [[[NSStackView alloc] init] autorelease];
    col.translatesAutoresizingMaskIntoConstraints = NO;
    col.orientation = NSUserInterfaceLayoutOrientationVertical;
    col.alignment = NSLayoutAttributeLeading;
    col.spacing = 16;
    col.edgeInsets = NSEdgeInsetsMake(20, 24, 20, 24);

    /* — Font family picker — */
    NSPopUpButton *fontPop = [[[NSPopUpButton alloc] init] autorelease];
    fontPop.translatesAutoresizingMaskIntoConstraints = NO;
    fontPop.bezelStyle = NSBezelStyleRounded;
    fontPop.target = self;
    fontPop.action = @selector(fontPicked:);
    for (NSUInteger i = 0; i < self.fontNames.count; i++) {
        [fontPop addItemWithTitle:self.fontNames[i]];
        NSMenuItem *it = [fontPop itemAtIndex:(NSInteger)i];
        it.tag = (NSInteger)i;
    }
    /* Match current font by path */
    for (NSUInteger i = 0; i < self.fontPaths.count; i++) {
        if ([self.fontPaths[i] isEqualToString:self.currentFont]) {
            [fontPop selectItemAtIndex:(NSInteger)i];
            break;
        }
    }
    [fontPop.widthAnchor constraintGreaterThanOrEqualToConstant:240].active = YES;

    NSButton *importBtn = [NSButton buttonWithTitle:@"Add Custom Font…" target:self action:@selector(importFontClicked:)];
    importBtn.bezelStyle = NSBezelStyleRounded;
    importBtn.controlSize = NSControlSizeSmall;

    NSStackView *fontRowControls = [[[NSStackView alloc] init] autorelease];
    fontRowControls.translatesAutoresizingMaskIntoConstraints = NO;
    fontRowControls.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    fontRowControls.alignment = NSLayoutAttributeCenterY;
    fontRowControls.spacing = 10;
    [fontRowControls addArrangedSubview:fontPop];
    [fontRowControls addArrangedSubview:importBtn];

    NSStackView *familyRow = [self rowWithLabel:@"Family" control:fontRowControls];

    /* — Font size: slider + editable text input. The text field is the
     * authoritative display ("pt" suffix shown via a sibling label) so users
     * can type an exact value; the slider stays in sync via target/action. */
    self.fontSizeValueLabel = [[[NSTextField alloc] init] autorelease];
    self.fontSizeValueLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.fontSizeValueLabel.bordered = YES;
    self.fontSizeValueLabel.editable = YES;
    self.fontSizeValueLabel.selectable = YES;
    self.fontSizeValueLabel.bezeled = YES;
    self.fontSizeValueLabel.drawsBackground = YES;
    self.fontSizeValueLabel.font = [NSFont monospacedDigitSystemFontOfSize:12
                                                                     weight:NSFontWeightRegular];
    self.fontSizeValueLabel.alignment = NSTextAlignmentRight;
    self.fontSizeValueLabel.stringValue = [NSString stringWithFormat:@"%.0f", self.fontSize];
    self.fontSizeValueLabel.target = self;
    self.fontSizeValueLabel.action = @selector(sizeFieldChanged:);
    [self.fontSizeValueLabel.widthAnchor constraintEqualToConstant:54].active = YES;

    NSTextField *sizeUnitLabel = [NSTextField labelWithString:@"pt"];
    sizeUnitLabel.textColor = NSColor.secondaryLabelColor;
    sizeUnitLabel.font = [NSFont systemFontOfSize:11];

    self.fontSizeSlider = [[[NSSlider alloc] init] autorelease];
    self.fontSizeSlider.translatesAutoresizingMaskIntoConstraints = NO;
    /* Range aligned with on_native_value_change's clamp (6..96), so a
     * config-loaded value anywhere in that range is kept when the user
     * drags the slider. */
    self.fontSizeSlider.minValue = 6;
    self.fontSizeSlider.maxValue = 96;
    self.fontSizeSlider.doubleValue = (double)self.fontSize;
    self.fontSizeSlider.numberOfTickMarks = 0;
    self.fontSizeSlider.allowsTickMarkValuesOnly = NO;
    self.fontSizeSlider.continuous = YES;
    self.fontSizeSlider.target = self;
    self.fontSizeSlider.action = @selector(sizeSliderChanged:);

    NSStackView *sizeRowControls = [[[NSStackView alloc] init] autorelease];
    sizeRowControls.translatesAutoresizingMaskIntoConstraints = NO;
    sizeRowControls.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    sizeRowControls.alignment = NSLayoutAttributeCenterY;
    sizeRowControls.spacing = 8;
    [sizeRowControls addArrangedSubview:self.fontSizeSlider];
    [sizeRowControls addArrangedSubview:self.fontSizeValueLabel];
    [sizeRowControls addArrangedSubview:sizeUnitLabel];

    NSStackView *sizeRow = [self rowWithLabel:@"Size" control:sizeRowControls];

    /* — Line Weight: stroke width added to each glyph at rasterisation.
     * 0 = native, 1.5 ≈ heavy. Continuous so live preview is immediate. */
    self.fontWeightValueLabel = [NSTextField labelWithString:
        [NSString stringWithFormat:@"%.2f", self.fontWeight]];
    self.fontWeightValueLabel.font = [NSFont monospacedDigitSystemFontOfSize:12
                                                                       weight:NSFontWeightRegular];
    self.fontWeightValueLabel.textColor = NSColor.secondaryLabelColor;
    self.fontWeightValueLabel.alignment = NSTextAlignmentRight;
    [self.fontWeightValueLabel.widthAnchor constraintEqualToConstant:48].active = YES;

    self.fontWeightSlider = [[[NSSlider alloc] init] autorelease];
    self.fontWeightSlider.translatesAutoresizingMaskIntoConstraints = NO;
    /* Range aligned with on_native_value_change's clamp (0..2), so a
     * heavier stroke supplied through JSON is not re-clipped when the
     * user touches the slider. */
    self.fontWeightSlider.minValue = 0.0;
    self.fontWeightSlider.maxValue = 2.0;
    self.fontWeightSlider.doubleValue = (double)self.fontWeight;
    self.fontWeightSlider.continuous = YES;
    self.fontWeightSlider.target = self;
    self.fontWeightSlider.action = @selector(weightSliderChanged:);

    NSStackView *weightRowControls = [[[NSStackView alloc] init] autorelease];
    weightRowControls.translatesAutoresizingMaskIntoConstraints = NO;
    weightRowControls.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    weightRowControls.alignment = NSLayoutAttributeCenterY;
    weightRowControls.spacing = 10;
    [weightRowControls addArrangedSubview:self.fontWeightSlider];
    [weightRowControls addArrangedSubview:self.fontWeightValueLabel];

    NSStackView *weightRow = [self rowWithLabel:@"Line Weight" control:weightRowControls];

    NSStackView *fontStack = [[[NSStackView alloc] init] autorelease];
    fontStack.translatesAutoresizingMaskIntoConstraints = NO;
    fontStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    fontStack.alignment = NSLayoutAttributeLeading;
    fontStack.spacing = 12;
    [fontStack addArrangedSubview:familyRow];
    [fontStack addArrangedSubview:sizeRow];
    [fontStack addArrangedSubview:weightRow];

    NSView *fontBox = [self groupBoxWithTitle:@"Terminal font" content:fontStack];
    [col addArrangedSubview:fontBox];

    [col.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    return col;
}

- (NSView *)buildBehaviorSection {
    NSStackView *col = [[[NSStackView alloc] init] autorelease];
    col.translatesAutoresizingMaskIntoConstraints = NO;
    col.orientation = NSUserInterfaceLayoutOrientationVertical;
    col.alignment = NSLayoutAttributeLeading;
    col.spacing = 16;
    col.edgeInsets = NSEdgeInsetsMake(20, 24, 20, 24);

    /* — Tab sleep — */
    self.tabSleepValueLabel = [NSTextField labelWithString:
        self.tabSleepMinutes <= 0 ? @"Off"
        : [NSString stringWithFormat:@"%.0f min", self.tabSleepMinutes]];
    self.tabSleepValueLabel.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
    self.tabSleepValueLabel.textColor = NSColor.secondaryLabelColor;
    self.tabSleepValueLabel.alignment = NSTextAlignmentRight;
    [self.tabSleepValueLabel.widthAnchor constraintEqualToConstant:64].active = YES;

    NSSlider *sleepSlider = [[[NSSlider alloc] init] autorelease];
    sleepSlider.translatesAutoresizingMaskIntoConstraints = NO;
    sleepSlider.minValue = 0;
    sleepSlider.maxValue = 120;
    sleepSlider.doubleValue = (double)self.tabSleepMinutes;
    sleepSlider.numberOfTickMarks = 13;
    sleepSlider.allowsTickMarkValuesOnly = YES;   /* snap to 10-min steps */
    sleepSlider.tickMarkPosition = NSTickMarkPositionBelow;
    sleepSlider.continuous = YES;
    sleepSlider.target = self;
    sleepSlider.action = @selector(tabSleepSliderChanged:);

    NSStackView *sleepRowControls = [[[NSStackView alloc] init] autorelease];
    sleepRowControls.translatesAutoresizingMaskIntoConstraints = NO;
    sleepRowControls.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    sleepRowControls.alignment = NSLayoutAttributeCenterY;
    sleepRowControls.spacing = 10;
    [sleepRowControls addArrangedSubview:sleepSlider];
    [sleepRowControls addArrangedSubview:self.tabSleepValueLabel];

    NSStackView *sleepRow = [self rowWithLabel:@"Sleep idle tabs after" control:sleepRowControls];

    NSTextField *help = [NSTextField wrappingLabelWithString:
        @"Inactive tabs are paused after this many minutes to save CPU. Set to Off to keep all tabs running."];
    help.font = [NSFont systemFontOfSize:11];
    help.textColor = NSColor.tertiaryLabelColor;

    NSStackView *vs = [[[NSStackView alloc] init] autorelease];
    vs.translatesAutoresizingMaskIntoConstraints = NO;
    vs.orientation = NSUserInterfaceLayoutOrientationVertical;
    vs.alignment = NSLayoutAttributeLeading;
    vs.spacing = 8;
    [vs addArrangedSubview:sleepRow];
    [vs addArrangedSubview:help];

    NSView *box = [self groupBoxWithTitle:@"Tabs" content:vs];
    [col addArrangedSubview:box];

    [col.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    return col;
}

#pragma mark — Section swap

- (void)showSection:(NSView *)section {
    if (self.currentSection == section) return;
    NSArray *oldSubviews = [[self.sectionContainer.subviews copy] autorelease];
    for (NSView *v in oldSubviews) {
        [v removeFromSuperview];
    }
    section.translatesAutoresizingMaskIntoConstraints = NO;
    [self.sectionContainer addSubview:section];
    [NSLayoutConstraint activateConstraints:@[
        [section.topAnchor      constraintEqualToAnchor:self.sectionContainer.topAnchor],
        [section.leadingAnchor  constraintEqualToAnchor:self.sectionContainer.leadingAnchor],
        [section.trailingAnchor constraintEqualToAnchor:self.sectionContainer.trailingAnchor],
        [section.bottomAnchor   constraintLessThanOrEqualToAnchor:self.sectionContainer.bottomAnchor],
    ]];
    self.currentSection = section;
}

- (void)switchToAppearance:(id)sender { (void)sender; [self showSection:self.sectionAppearance]; }
- (void)switchToTypography:(id)sender { (void)sender; [self showSection:self.sectionTypography]; }
- (void)switchToBehavior:(id)sender   { (void)sender; [self showSection:self.sectionBehavior];   }

#pragma mark — Top-level layout

- (void)buildUI {
    NSPanel *panel = self.panel;
    NSView *content = panel.contentView;
    NSArray *oldSubviews = [[content.subviews copy] autorelease];
    for (NSView *subview in oldSubviews) {
        [subview removeFromSuperview];
    }

    /* Vibrancy backdrop — matches the rest of the app's modals. */
    NSVisualEffectView *blur = [[[NSVisualEffectView alloc]
        initWithFrame:content.bounds] autorelease];
    blur.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    blur.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    blur.material = NSVisualEffectMaterialUnderWindowBackground;
    blur.state = NSVisualEffectStateActive;
    [content addSubview:blur];

    /* Build all three sections up-front so swapping is free. */
    self.sectionAppearance = [self buildAppearanceSection];
    self.sectionTypography = [self buildTypographySection];
    self.sectionBehavior   = [self buildBehaviorSection];

    /* Segmented control acts as the section switcher — visually a
     * "Tab" row at the top, native-style. NSToolbar is overkill for
     * three siblings and forces a separate window chrome. */
    NSSegmentedControl *seg = [[[NSSegmentedControl alloc] init] autorelease];
    seg.translatesAutoresizingMaskIntoConstraints = NO;
    seg.segmentStyle = NSSegmentStyleAutomatic;
    seg.trackingMode = NSSegmentSwitchTrackingSelectOne;
    [seg setSegmentCount:3];
    [seg setLabel:@"Appearance"  forSegment:0];
    [seg setLabel:@"Typography"  forSegment:1];
    [seg setLabel:@"Behavior"    forSegment:2];
    [seg setSelectedSegment:0];
    seg.target = self;
    seg.action = @selector(segmentChanged:);

    /* Section container fills the middle. */
    self.sectionContainer = [[[NSView alloc] init] autorelease];
    self.sectionContainer.translatesAutoresizingMaskIntoConstraints = NO;

    /* Sticky bottom bar with Done. */
    NSButton *doneBtn = [NSButton buttonWithTitle:@"Done" target:self action:@selector(doneClicked:)];
    doneBtn.bezelStyle = NSBezelStyleRounded;
    doneBtn.keyEquivalent = @"\r";   /* Enter */

    NSStackView *footer = [[[NSStackView alloc] init] autorelease];
    footer.translatesAutoresizingMaskIntoConstraints = NO;
    footer.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    footer.alignment = NSLayoutAttributeCenterY;
    footer.distribution = NSStackViewDistributionFill;
    footer.edgeInsets = NSEdgeInsetsMake(12, 20, 14, 20);
    NSView *spring = [[[NSView alloc] init] autorelease];
    spring.translatesAutoresizingMaskIntoConstraints = NO;
    [spring setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [footer addArrangedSubview:spring];
    [footer addArrangedSubview:doneBtn];

    /* Header strip hosts the segmented control */
    NSView *header = [[[NSView alloc] init] autorelease];
    header.translatesAutoresizingMaskIntoConstraints = NO;
    [header addSubview:seg];
    [NSLayoutConstraint activateConstraints:@[
        [seg.centerXAnchor constraintEqualToAnchor:header.centerXAnchor],
        [seg.topAnchor     constraintEqualToAnchor:header.topAnchor    constant:14],
        [seg.bottomAnchor  constraintEqualToAnchor:header.bottomAnchor constant:-12],
    ]];

    NSStackView *root = [[[NSStackView alloc] init] autorelease];
    root.translatesAutoresizingMaskIntoConstraints = NO;
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeCenterX;
    root.distribution = NSStackViewDistributionFill;
    root.spacing = 0;
    [root addArrangedSubview:header];
    [root addArrangedSubview:self.sectionContainer];
    [root addArrangedSubview:footer];
    [content addSubview:root];

    [NSLayoutConstraint activateConstraints:@[
        [root.topAnchor      constraintEqualToAnchor:content.topAnchor],
        [root.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [root.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [root.bottomAnchor   constraintEqualToAnchor:content.bottomAnchor],
        [header.leadingAnchor  constraintEqualToAnchor:root.leadingAnchor],
        [header.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [self.sectionContainer.leadingAnchor  constraintEqualToAnchor:root.leadingAnchor],
        [self.sectionContainer.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [footer.leadingAnchor  constraintEqualToAnchor:root.leadingAnchor],
        [footer.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
    ]];

    [self showSection:self.sectionAppearance];
}

#pragma mark — Actions

- (void)segmentChanged:(NSSegmentedControl *)sender {
    switch (sender.selectedSegment) {
    case 0: [self showSection:self.sectionAppearance]; break;
    case 1: [self showSection:self.sectionTypography]; break;
    case 2: [self showSection:self.sectionBehavior];   break;
    default: break;
    }
}

- (void)themePicked:(NSPopUpButton *)sender {
    NSString *name = sender.titleOfSelectedItem;
    if (g_on_theme && name) g_on_theme(name.UTF8String);
}

- (void)fontPicked:(NSPopUpButton *)sender {
    NSInteger idx = sender.indexOfSelectedItem;
    if (idx < 0 || idx >= (NSInteger)self.fontPaths.count) return;
    NSString *path = self.fontPaths[(NSUInteger)idx];
    if (g_on_font && path) g_on_font(path.UTF8String);
}

- (void)sizeSliderChanged:(NSSlider *)sender {
    f32 v = (f32)sender.doubleValue;
    self.fontSize = v;
    /* Slider drags emit fractional doubles; round to whole pt to match
     * what the text field shows and what the engine ultimately stores. */
    f32 rv = roundf(v);
    self.fontSizeValueLabel.stringValue = [NSString stringWithFormat:@"%.0f", rv];
    if (g_on_value) g_on_value("font_size", rv);
}

/* User typed a value into the size field and pressed Enter / tabbed out.
 * Parses, clamps to slider range, mirrors back into the slider so the two
 * controls stay in lockstep. Empty / non-numeric input is rejected by
 * snapping back to the current size. */
- (void)sizeFieldChanged:(NSTextField *)sender {
    NSString *raw = sender.stringValue;
    NSScanner *scanner = [NSScanner scannerWithString:raw];
    scanner.charactersToBeSkipped = [NSCharacterSet whitespaceAndNewlineCharacterSet];
    double parsed = 0;
    BOOL ok = [scanner scanDouble:&parsed];
    if (!ok) {
        sender.stringValue = [NSString stringWithFormat:@"%.0f", self.fontSize];
        return;
    }
    if (parsed < self.fontSizeSlider.minValue) parsed = self.fontSizeSlider.minValue;
    if (parsed > self.fontSizeSlider.maxValue) parsed = self.fontSizeSlider.maxValue;
    f32 v = (f32)parsed;
    self.fontSize = v;
    self.fontSizeSlider.doubleValue = (double)v;
    sender.stringValue = [NSString stringWithFormat:@"%.0f", v];
    if (g_on_value) g_on_value("font_size", v);
}

- (void)weightSliderChanged:(NSSlider *)sender {
    f32 v = (f32)sender.doubleValue;
    self.fontWeight = v;
    self.fontWeightValueLabel.stringValue = [NSString stringWithFormat:@"%.2f", v];
    if (g_on_value) g_on_value("font_weight", v);
}

- (void)opacityChanged:(NSSlider *)sender {
    f32 pct = (f32)sender.doubleValue;
    self.opacity = pct / 100.0f;
    self.opacityValueLabel.stringValue = [NSString stringWithFormat:@"%d %%", (int)(pct + 0.5f)];
    if (g_on_value) g_on_value("opacity", self.opacity);
}

- (void)tabSleepSliderChanged:(NSSlider *)sender {
    f32 m = (f32)sender.doubleValue;
    self.tabSleepMinutes = m;
    self.tabSleepValueLabel.stringValue = (m <= 0)
        ? @"Off"
        : [NSString stringWithFormat:@"%.0f min", m];
    if (g_on_value) g_on_value("tab_sleep_idle_minutes", m);
}

- (void)importFontClicked:(id)sender {
    (void)sender;
    if (g_on_value) g_on_value("import_font", 1.0f);
}

- (void)doneClicked:(id)sender {
    (void)sender;
    [self animatedClose];
}

#pragma mark — Open / close animation

/* Lockstep alpha + scale + Y-slide, mirroring the in-app Cmd+K palette
 * close curve so every modal in Liu shares one motion vocabulary.
 *
 *   alpha:  quadratic ease-out → (1-t)^2 on close, t*(2-t) on open
 *   scale:  1.0 ↔ 0.96 (NSPanel.frame shrunk around centre)
 *   y-off:  0   ↔ 8 px (panel slides up as it fades on close)
 *
 * NSAnimationContext's animator proxy doesn't reliably animate
 * NSPanel.alphaValue when the contentView is vibrancy-backed (the
 * backing layer isn't always wired through the window-server animation
 * path; completion fires immediately and the panel snaps). A 60 Hz
 * NSTimer driving the three properties directly bypasses that quirk
 * and stays frame-rate-stable regardless of panel layer state. */

#define LIU_SETTINGS_CLOSE_DUR  0.16
#define LIU_SETTINGS_OPEN_DUR   0.22
#define LIU_SETTINGS_SCALE_FROM 0.96
#define LIU_SETTINGS_YOFF_MAX   8.0

/* Single CALayer opacity animation on the contentView. Vibrancy +
 * transparent titlebar + hidden title mean fading the contentView
 * visually fades the entire panel — no scale, no slide, just opacity.
 * GPU-driven, vsync-locked, no NSTimer / window-server alpha quirks. */
- (void)runPanelTransitionWithDuration:(NSTimeInterval)dur
                               opening:(BOOL)opening
                            onComplete:(void (^)(void))done {
    NSPanel *panel = self.panel;
    if (!panel) { if (done) done(); return; }
    NSView *cv = panel.contentView;
    cv.wantsLayer = YES;
    if (!cv.layer) cv.layer = [CALayer layer];

    CGFloat fromOpacity = opening ? 0.0f : 1.0f;
    CGFloat toOpacity   = opening ? 1.0f : 0.0f;

    /* Pin start state before adding the animation so the compositor's
     * first frame matches the from-value (no rest-pose flash). */
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    cv.layer.opacity = (float)fromOpacity;
    [CATransaction commit];

    CABasicAnimation *opAnim = [CABasicAnimation animationWithKeyPath:@"opacity"];
    opAnim.fromValue          = @(fromOpacity);
    opAnim.toValue            = @(toOpacity);
    opAnim.duration           = dur;
    opAnim.timingFunction     = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];
    opAnim.fillMode           = kCAFillModeForwards;
    opAnim.removedOnCompletion = NO;

    [CATransaction begin];
    [CATransaction setAnimationDuration:dur];
    [CATransaction setCompletionBlock:^{
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        cv.layer.opacity = opening ? 1.0f : 0.0f;
        [cv.layer removeAnimationForKey:@"liuOpacity"];
        [CATransaction commit];
        if (done) done();
    }];
    [cv.layer addAnimation:opAnim forKey:@"liuOpacity"];
    cv.layer.opacity = (float)toOpacity;
    [CATransaction commit];
}

- (void)animatedClose {
    NSPanel *panel = self.panel;
    if (!panel || !panel.visible) return;
    panel.animationBehavior = NSWindowAnimationBehaviorNone;
    NSRect orig = panel.frame;
    [self runPanelTransitionWithDuration:LIU_SETTINGS_CLOSE_DUR
                                 opening:NO
                              onComplete:^{
        [panel orderOut:nil];
        /* Restore the rest pose so the next animatedShow has a clean
         * baseline (runPanelTransition already snapped frame + alpha,
         * but orderOut means alpha=0 now — re-prep for next show). */
        [panel setFrame:orig display:NO];
        panel.alphaValue = 1.0;
    }];
}

- (void)animatedShow {
    NSPanel *panel = self.panel;
    if (!panel) return;
    panel.animationBehavior = NSWindowAnimationBehaviorNone;
    [panel makeKeyAndOrderFront:nil];
    [self runPanelTransitionWithDuration:LIU_SETTINGS_OPEN_DUR
                                 opening:YES
                              onComplete:nil];
}

/* Intercept the red traffic-light close so it goes through the same
 * fade-out path as the Done button. Returning NO defers the actual
 * window dismissal; animatedClose calls orderOut once the fade lands. */
- (BOOL)windowShouldClose:(NSWindow *)sender {
    if (sender == self.panel) {
        [self animatedClose];
        return NO;
    }
    return YES;
}

- (void)dealloc {
    if (_panel.delegate == self) _panel.delegate = nil;
    [_panel release];
    [_fontNames release];
    [_fontPaths release];
    [_fontInstalled release];
    [_themeNames release];
    [_currentFont release];
    [_currentTheme release];
    [_sectionAppearance release];
    [_sectionTypography release];
    [_sectionBehavior release];
    [_currentSection release];
    [_sectionContainer release];
    [_fontSizeValueLabel release];
    [_fontSizeSlider release];
    [_fontWeightSlider release];
    [_fontWeightValueLabel release];
    [_opacityValueLabel release];
    [_tabSleepValueLabel release];
    [super dealloc];
}

@end

static LiuSettingsController *g_settings_ctrl = nil;

void platform_show_settings(PlatformWindow *w,
                             const char **font_names, const char **font_paths,
                             const bool *font_installed, i32 font_count,
                             const char **theme_names, i32 theme_count,
                             const char *current_font, const char *current_theme,
                             f32 font_size, f32 font_weight,
                             f32 opacity, f32 tab_sleep_minutes,
                             SettingsFontCallback on_font,
                             SettingsThemeCallback on_theme,
                             SettingsValueCallback on_value) {
    if (!w || !w->ns_window) return;
    if (font_count < 0 || font_count > 4096 ||
        (font_count > 0 && (!font_names || !font_paths || !font_installed)))
        font_count = 0;
    if (theme_count < 0 || theme_count > 4096 ||
        (theme_count > 0 && !theme_names)) theme_count = 0;
    g_on_font = on_font;
    g_on_theme = on_theme;
    g_on_value = on_value;

    if (!g_settings_panel) {
        @autoreleasepool {
            /* Slightly wider + taller so the segmented section switcher
             * + sectioned group boxes have proper breathing room. */
            NSRect frame = NSMakeRect(200, 200, 600, 460);
            NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                               NSWindowStyleMaskUtilityWindow | NSWindowStyleMaskFullSizeContentView;
            g_settings_panel = [[NSPanel alloc] initWithContentRect:frame
                                                           styleMask:style
                                                             backing:NSBackingStoreBuffered
                                                               defer:NO];
            g_settings_panel.title = @"Settings";
            g_settings_panel.releasedWhenClosed = NO;
            g_settings_panel.titlebarAppearsTransparent = YES;
            g_settings_panel.titleVisibility = NSWindowTitleHidden;
            g_settings_panel.movableByWindowBackground = YES;

            /* Position relative to main window */
            NSRect main_frame = w->ns_window.frame;
            NSPoint center = NSMakePoint(NSMidX(main_frame) - 300, NSMidY(main_frame) - 230);
            [g_settings_panel setFrameOrigin:center];

            g_settings_ctrl = [[LiuSettingsController alloc] init];
            g_settings_ctrl.panel = g_settings_panel;
            g_settings_ctrl.fontNames = [NSMutableArray array];
            g_settings_ctrl.fontPaths = [NSMutableArray array];
            g_settings_ctrl.fontInstalled = [NSMutableArray array];
            g_settings_ctrl.themeNames = [NSMutableArray array];
            /* Intercept traffic-light close so it routes through the
             * controller's animated fade-out instead of vanishing. */
            g_settings_panel.delegate = g_settings_ctrl;
        }
    }

    [g_settings_ctrl.fontNames removeAllObjects];
    [g_settings_ctrl.fontPaths removeAllObjects];
    [g_settings_ctrl.fontInstalled removeAllObjects];
    [g_settings_ctrl.themeNames removeAllObjects];
    NSString *fontCurrent = [NSString stringWithUTF8String:
        (current_font ? current_font : "")];
    NSString *themeCurrent = [NSString stringWithUTF8String:
        (current_theme ? current_theme : "")];
    g_settings_ctrl.currentFont = fontCurrent ? fontCurrent : @"";
    g_settings_ctrl.currentTheme = themeCurrent ? themeCurrent : @"";
    g_settings_ctrl.fontSize = font_size;
    g_settings_ctrl.fontWeight = font_weight;
    g_settings_ctrl.opacity = opacity;
    g_settings_ctrl.tabSleepMinutes = tab_sleep_minutes;

    for (i32 i = 0; i < font_count; i++) {
        NSString *name = font_names[i]
            ? [NSString stringWithUTF8String:font_names[i]] : nil;
        NSString *path = font_paths[i]
            ? [NSString stringWithUTF8String:font_paths[i]] : nil;
        [g_settings_ctrl.fontNames addObject:(name ? name : @"")];
        [g_settings_ctrl.fontPaths addObject:(path ? path : @"")];
        [g_settings_ctrl.fontInstalled addObject:@(font_installed[i])];
    }
    for (i32 i = 0; i < theme_count; i++) {
        NSString *name = theme_names[i]
            ? [NSString stringWithUTF8String:theme_names[i]] : nil;
        if (name) [g_settings_ctrl.themeNames addObject:name];
    }

    [g_settings_ctrl buildUI];
    [g_settings_ctrl animatedShow];
}

void platform_close_settings(void) {
    if (g_settings_ctrl) {
        [g_settings_ctrl animatedClose];
    }
}

bool platform_settings_visible(void) {
    return g_settings_panel && g_settings_panel.visible;
}

static void platform_destroy_owned_ui(void) {
    /* Toasts are per window, so sweep every window still registered:
     * platform_shutdown() may run before the app has destroyed them all. */
    for (i32 i = 0; i < g_window_count; i++) platform_toast_destroy_for(g_windows[i]);
    if (g_settings_panel) {
        g_settings_panel.delegate = nil;
        [g_settings_panel orderOut:nil];
    }
    [g_settings_ctrl release];
    g_settings_ctrl = nil;
    [g_settings_panel close];
    [g_settings_panel release];
    g_settings_panel = nil;
    g_on_font = NULL;
    g_on_theme = NULL;
    g_on_value = NULL;
}

/* =========================================================================
 * Bell / Notifications
 * ========================================================================= */

void platform_play_bell(void) {
    @autoreleasepool {
        NSBeep();
    }
}

void platform_set_dock_badge(const char *text) {
    @autoreleasepool {
        if (text && text[0]) {
            [[NSApp dockTile] setBadgeLabel:[NSString stringWithUTF8String:text]];
        } else {
            [[NSApp dockTile] setBadgeLabel:nil];
        }
    }
}

/* There is no platform_post_notification() on macOS. The app's banners go
 * through app_post_banner() (ui.c) and the notify module's
 * platform_notify_desktop(). */

void platform_request_attention(PlatformWindow *w) {
    (void)w;   /* the Dock's bounce is the app's, whichever window asked */
    @autoreleasepool {
        [NSApp requestUserAttention:NSInformationalRequest];
    }
}

bool platform_is_app_focused(void) {
    /* The atomic, not `[NSApp isActive]`: callers include the notify server's
     * thread, and AppKit accessors are main-thread-only. See g_app_frontmost. */
    return atomic_load_explicit(&g_app_frontmost, memory_order_acquire);
}

bool platform_utf8_normalize_nfc(const char *src, char *dst, usize dstcap) {
    if (!dst || dstcap == 0) return false;
    if (!src) { dst[0] = '\0'; return false; }
    @autoreleasepool {
        /* Capture src into an NSString BEFORE touching dst: callers may pass
         * dst == src (e.g. re-normalizing a buffer in place), and clearing
         * dst[0] first would blank the input before it is read. */
        NSString *s = [NSString stringWithUTF8String:src];
        if (!s) { dst[0] = '\0'; return false; }
        NSMutableString *ms = [s mutableCopy];
        if (!ms) { dst[0] = '\0'; return false; }
        /* In-place; equivalent to creating a precomposedStringWithCanonicalMapping
         * but avoids a temporary NSString allocation. Form C = canonical
         * decomposition followed by canonical composition. */
        CFStringNormalize((__bridge CFMutableStringRef)ms, kCFStringNormalizationFormC);
        const char *u = [ms UTF8String];
        if (!u) { [ms release]; dst[0] = '\0'; return false; }
        usize n = strlen(u);
        if (n >= dstcap) n = dstcap - 1;
        memcpy(dst, u, n);
        dst[n] = '\0';
        [ms release];
        return true;
    }
}



/* =========================================================================
 * File watcher (config hot-reload)
 * ========================================================================= */

static dispatch_source_t g_file_watch_source = nil;
static FileWatchCallback g_file_watch_cb = NULL;
static void *g_file_watch_userdata = NULL;
static char g_file_watch_path[1024] = {0};
static bool g_file_watch_needs_rewatch = false;

static void platform_close_watch_source(void) {
    if (g_file_watch_source) {
        dispatch_source_cancel(g_file_watch_source);
        /* MRC: drop the +1 from dispatch_source_create. Cancellation releases
         * the source's own handler blocks (breaking the event-handler→source
         * retain), so this frees it once the cancel handler (close(fd)) runs.
         * The g_file_watch_source guard + nil below prevents a double-release
         * when the event handler re-enters this on DELETE/RENAME. */
        dispatch_release(g_file_watch_source);
        g_file_watch_source = nil;
    }
}

static bool platform_watch_file_internal(const char *path) {
    if (!path || !path[0]) return false;

    /* O_NONBLOCK: a FIFO at the path opens at once instead of holding the
     * main thread until something writes to it, and is not watched; the
     * poll tries again, so a regular file put back is picked up. */
    int fd = open(path, O_EVTONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return false;
    }

    dispatch_source_t source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_VNODE, fd,
        DISPATCH_VNODE_WRITE | DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME,
        dispatch_get_main_queue());
    if (!source) {
        close(fd);
        return false;
    }

    g_file_watch_source = source;
    dispatch_source_set_event_handler(source, ^{
        unsigned long events = dispatch_source_get_data(source);
        if (g_file_watch_cb) g_file_watch_cb(g_file_watch_path, g_file_watch_userdata);
        if (events & (DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME)) {
            g_file_watch_needs_rewatch = true;
            if (g_file_watch_source == source) {
                platform_close_watch_source();
            }
        }
    });
    dispatch_source_set_cancel_handler(source, ^{
        close(fd);
    });
    dispatch_resume(source);
    return true;
}

bool platform_watch_file(const char *path, FileWatchCallback cb, void *userdata) {
    @autoreleasepool {
        platform_close_watch_source();
        g_file_watch_cb = cb;
        g_file_watch_userdata = userdata;
        g_file_watch_needs_rewatch = false;
        snprintf(g_file_watch_path, sizeof(g_file_watch_path), "%s", path ? path : "");

        if (!platform_watch_file_internal(g_file_watch_path)) {
            g_file_watch_needs_rewatch = g_file_watch_path[0] != '\0';
            return false;
        }
        return true;
    }
}

void platform_poll_file_watches(void) {
    @autoreleasepool {
        if (g_file_watch_needs_rewatch &&
            !g_file_watch_source &&
            g_file_watch_path[0] &&
            g_file_watch_cb) {
            if (platform_watch_file_internal(g_file_watch_path)) {
                g_file_watch_needs_rewatch = false;
            }
        }
    }
}

void platform_unwatch_file(void) {
    platform_close_watch_source();
    g_file_watch_path[0] = '\0';
    g_file_watch_needs_rewatch = false;
    g_file_watch_cb = NULL;
    g_file_watch_userdata = NULL;
}

/* =========================================================================
 * IME cursor position
 * ========================================================================= */

void platform_set_ime_cursor_pos(f32 x, f32 y, f32 cell_w, f32 cell_h) {
    (void)x; (void)y; (void)cell_w; (void)cell_h;
}

void platform_ime_reset(PlatformWindow *w) { (void)w; }

/* =========================================================================
 * File dialog (native NSOpenPanel)
 * ========================================================================= */

const char *platform_open_file_dialog(const char *title, const char *extensions) {
    static char result_path[1024] = {0};
    result_path[0] = '\0';

    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        if (title) {
            [panel setMessage:[NSString stringWithUTF8String:title]];
        }

        /* Parse comma-separated extensions into NSArray for allowedFileTypes
         * (deprecated but compatible with macOS 11+) */
        if (extensions && extensions[0]) {
            NSMutableArray<NSString *> *exts = [NSMutableArray array];
            char ext_buf[512];
            snprintf(ext_buf, sizeof(ext_buf), "%s", extensions);
            char *tok = strtok(ext_buf, ",");
            while (tok) {
                while (*tok == ' ') tok++;
                [exts addObject:[NSString stringWithUTF8String:tok]];
                tok = strtok(NULL, ",");
            }
            if (exts.count > 0) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                [panel setAllowedFileTypes:exts];
#pragma clang diagnostic pop
            }
        }

        NSModalResponse response = [panel runModal];
        if (response == NSModalResponseOK) {
            NSURL *url = panel.URL;
            if (url) {
                const char *path = url.fileSystemRepresentation;
                if (path) snprintf(result_path, sizeof(result_path), "%s", path);
            }
        }
    }

    return result_path[0] ? result_path : NULL;
}

const char *platform_open_folder_dialog(const char *title) {
    static char result_path[1024] = {0};
    result_path[0] = '\0';
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:NO];
        [panel setCanChooseDirectories:YES];
        [panel setCanCreateDirectories:YES];
        [panel setAllowsMultipleSelection:NO];
        if (title) [panel setMessage:[NSString stringWithUTF8String:title]];
        if ([panel runModal] == NSModalResponseOK) {
            NSURL *url = panel.URL;
            const char *path = url ? url.fileSystemRepresentation : NULL;
            if (path) snprintf(result_path, sizeof result_path, "%s", path);
        }
    }
    return result_path[0] ? result_path : NULL;
}

/* =========================================================================
 * Audio recording (AVAudioRecorder → AAC/.m4a)
 *
 * This file is compiled under manual reference counting (no -fobjc-arc), so
 * the recorder is +1-owned via alloc and balanced with -release on stop.
 * ========================================================================= */

static AVAudioRecorder *g_audio_recorder = nil;
/* Bumped on every start AND stop. A permission grant that lands after the
 * user cancelled (closed Settings, picked another row) carries a stale
 * generation and is dropped, so a late grant can never start an untracked
 * recorder that holds the mic open. Only ever touched on the main thread. */
static int g_record_gen = 0;

/* Start a fresh recorder writing to `p`. Releases any prior recorder first.
 * Returns false (and leaves g_audio_recorder untouched) on failure. */
static bool liu_start_recorder(NSString *p) {
    NSURL *url = [NSURL fileURLWithPath:p];
    NSDictionary *settings = @{
        AVFormatIDKey:            @(kAudioFormatMPEG4AAC),
        AVSampleRateKey:          @44100.0,
        AVNumberOfChannelsKey:    @1,
        AVEncoderAudioQualityKey: @(AVAudioQualityHigh),
    };
    NSError *err = nil;
    AVAudioRecorder *rec =
        [[AVAudioRecorder alloc] initWithURL:url settings:settings error:&err];
    if (rec == nil) return false;
    if (err != nil || ![rec prepareToRecord] || ![rec record]) {
        [rec release];
        return false;
    }
    if (g_audio_recorder != nil) {
        [g_audio_recorder stop];
        [g_audio_recorder release];
    }
    g_audio_recorder = rec;   /* keep the +1 from alloc */
    return true;
}

PlatformMicPermission platform_audio_mic_permission(void) {
    AVAuthorizationStatus st =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    switch (st) {
        case AVAuthorizationStatusAuthorized:  return PLATFORM_MIC_GRANTED;
        case AVAuthorizationStatusDenied:
        case AVAuthorizationStatusRestricted:  return PLATFORM_MIC_DENIED;
        case AVAuthorizationStatusNotDetermined:
        default:                               return PLATFORM_MIC_UNKNOWN;
    }
}

bool platform_audio_record_start(const char *out_path) {
    if (!out_path || !*out_path) return false;
    @autoreleasepool {
        if (g_audio_recorder != nil) return false;   /* already recording */
        NSString *p = [NSString stringWithUTF8String:out_path];
        if (p == nil) return false;

        AVAuthorizationStatus st =
            [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        if (st == AVAuthorizationStatusAuthorized) {
            g_record_gen++;
            return liu_start_recorder(p);
        }
        if (st == AVAuthorizationStatusNotDetermined) {
            /* Capture starts on the main queue once the user answers the
             * system prompt — but only if this request is still the current
             * one (generation unchanged) and nothing else is recording.
             * `held` keeps the path alive across the async hop explicitly,
             * independent of block-copy retain semantics. */
            int my_gen = ++g_record_gen;
            NSString *held = [p retain];
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                     completionHandler:^(BOOL granted) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    @autoreleasepool {
                        if (granted && my_gen == g_record_gen &&
                            g_audio_recorder == nil) {
                            liu_start_recorder(held);
                        }
                        [held release];
                    }
                });
            }];
            return true;   /* optimistic — actual capture begins on grant */
        }
        return false;       /* denied / restricted */
    }
}

bool platform_audio_record_stop(void) {
    g_record_gen++;   /* invalidate any pending permission-grant start */
    if (g_audio_recorder == nil) return false;
    @autoreleasepool {
        [g_audio_recorder stop];
        [g_audio_recorder release];
        g_audio_recorder = nil;
    }
    return true;
}

bool platform_audio_recording(void) {
    return g_audio_recorder != nil && [g_audio_recorder isRecording];
}

f64 platform_audio_record_elapsed(void) {
    if (g_audio_recorder != nil && [g_audio_recorder isRecording]) {
        return (f64)[g_audio_recorder currentTime];
    }
    return 0.0;
}

void platform_open_microphone_settings(void) {
    @autoreleasepool {
        NSURL *u = [NSURL URLWithString:
            @"x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone"];
        if (!u) return;
        [[NSWorkspace sharedWorkspace] openURL:u
            configuration:[NSWorkspaceOpenConfiguration configuration]
            completionHandler:nil];
    }
}

/* =========================================================================
 * Live PCM capture (AVAudioEngine tap -> 16 kHz mono f32 ring)
 *
 * The tap block runs on a real-time audio thread, so the hand-off to the
 * main thread is a single-producer/single-consumer ring with relaxed/release
 * atomics and no locking. Taking a mutex here would be a priority-inversion
 * hazard against the audio thread, and dropping samples under back-pressure
 * is strictly better than stalling it — the drop is reported instead.
 *
 * The engine is asked for the input node's own format and an AVAudioConverter
 * brings it to 16 kHz mono. Installing a tap with a format that differs from
 * the node's is the classic way to get a -10868 crash on real hardware, and
 * built-in mics are 44.1/48 kHz stereo far more often than not.
 * ========================================================================= */

/* Four seconds of headroom. The consumer drains every frame, so this only
 * has to survive a stalled UI, not the whole utterance. */
#define LIU_CAP_RING 65536   /* power of two; masked indices */

/* Pinned input-device UID; empty means "whatever the system default is". */
static char               g_cap_dev_uid[PLATFORM_AUDIO_DEV_UID_CAP] = { 0 };

static AVAudioEngine     *g_cap_engine    = nil;
static AVAudioConverter  *g_cap_converter = nil;
static float             *g_cap_ring      = NULL;
static _Atomic uint64_t   g_cap_wr        = 0;   /* written  (producer) */
static _Atomic uint64_t   g_cap_rd        = 0;   /* consumed (consumer) */
static _Atomic bool       g_cap_overflow  = false;
static _Atomic bool       g_cap_running   = false;
static _Atomic uint32_t   g_cap_level     = 0;   /* f32 bits, published by the tap */
static int                g_cap_gen       = 0;

static void liu_cap_teardown(void) {
    if (g_cap_engine) {
        @try {
            [[g_cap_engine inputNode] removeTapOnBus:0];
        } @catch (NSException *e) { (void)e; }
        [g_cap_engine stop];
        [g_cap_engine release];
        g_cap_engine = nil;
    }
    [g_cap_converter release];
    g_cap_converter = nil;
    atomic_store(&g_cap_running, false);
    atomic_store(&g_cap_level, 0);
}

/* Push `n` samples into the ring. Runs on the audio thread. */
static void liu_cap_push(const float *src, uint32_t n) {
    uint64_t wr = atomic_load_explicit(&g_cap_wr, memory_order_relaxed);
    uint64_t rd = atomic_load_explicit(&g_cap_rd, memory_order_acquire);

    uint64_t free_slots = (uint64_t)LIU_CAP_RING - (wr - rd);
    if (n > free_slots) {
        atomic_store_explicit(&g_cap_overflow, true, memory_order_relaxed);
        n = (uint32_t)free_slots;
    }
    for (uint32_t i = 0; i < n; i++) {
        g_cap_ring[(wr + i) & (LIU_CAP_RING - 1)] = src[i];
    }
    atomic_store_explicit(&g_cap_wr, wr + n, memory_order_release);
}

/* ---- input-device enumeration + selection ------------------------------- */

/* A CFString property off a CoreAudio object, copied into `out`. */
static bool liu_cad_string(AudioObjectID dev, AudioObjectPropertySelector sel,
                           char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    AudioObjectPropertyAddress addr = {
        sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    CFStringRef s = NULL;
    UInt32 size = (UInt32)sizeof s;
    if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, &s) != noErr || !s)
        return false;
    bool ok = CFStringGetCString(s, out, (CFIndex)cap, kCFStringEncodingUTF8);
    CFRelease(s);
    return ok && out[0] != '\0';
}

/* Does this device have any input channels? A Mac lists output-only devices
 * (and aggregates with no input side) in the same table, and offering one as a
 * microphone produces a recording of silence. */
static bool liu_cad_has_input(AudioObjectID dev) {
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamConfiguration,
        kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &addr, 0, NULL, &size) != noErr ||
        size < sizeof(AudioBufferList)) return false;
    AudioBufferList *bl = (AudioBufferList *)malloc(size);
    if (!bl) return false;
    bool has = false;
    if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, bl) == noErr) {
        for (UInt32 i = 0; i < bl->mNumberBuffers; i++) {
            if (bl->mBuffers[i].mNumberChannels > 0) { has = true; break; }
        }
    }
    free(bl);
    return has;
}

static AudioObjectID liu_cad_default_input(void) {
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = (UInt32)sizeof dev;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL,
                                   &size, &dev) != noErr) {
        return kAudioObjectUnknown;
    }
    return dev;
}

/* Resolve a pinned UID to a device, or kAudioObjectUnknown when it is not
 * plugged in — the caller then leaves the engine on the system default rather
 * than refusing to record. */
static AudioObjectID liu_cad_device_for_uid(const char *uid) {
    if (!uid || !uid[0]) return kAudioObjectUnknown;
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL,
                                       &size) != noErr || size == 0) {
        return kAudioObjectUnknown;
    }
    AudioObjectID *ids = (AudioObjectID *)malloc(size);
    if (!ids) return kAudioObjectUnknown;
    AudioObjectID found = kAudioObjectUnknown;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL,
                                   &size, ids) == noErr) {
        const UInt32 n = size / (UInt32)sizeof(AudioObjectID);
        for (UInt32 i = 0; i < n; i++) {
            char u[PLATFORM_AUDIO_DEV_UID_CAP];
            if (!liu_cad_has_input(ids[i])) continue;
            if (liu_cad_string(ids[i], kAudioDevicePropertyDeviceUID, u, sizeof u) &&
                strcmp(u, uid) == 0) { found = ids[i]; break; }
        }
    }
    free(ids);
    return found;
}

i32 platform_audio_input_devices(PlatformAudioDevice *out, i32 max) {
    if (!out || max <= 0) return 0;
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL,
                                       &size) != noErr || size == 0) {
        return 0;
    }
    AudioObjectID *ids = (AudioObjectID *)malloc(size);
    if (!ids) return 0;
    i32 count = 0;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL,
                                   &size, ids) == noErr) {
        const AudioObjectID def = liu_cad_default_input();
        const UInt32 n = size / (UInt32)sizeof(AudioObjectID);
        for (UInt32 i = 0; i < n && count < max; i++) {
            if (!liu_cad_has_input(ids[i])) continue;
            PlatformAudioDevice *d = &out[count];
            memset(d, 0, sizeof *d);
            if (!liu_cad_string(ids[i], kAudioDevicePropertyDeviceUID,
                                d->uid, sizeof d->uid)) continue;
            /* Fall back to the UID when a device has no friendly name, so
             * every row can still be told apart. */
            if (!liu_cad_string(ids[i], kAudioObjectPropertyName,
                                d->name, sizeof d->name)) {
                snprintf(d->name, sizeof d->name, "%s", d->uid);
            }
            d->is_default = (ids[i] == def);
            count++;
        }
    }
    free(ids);
    return count;
}

void platform_audio_set_input_device(const char *uid) {
    snprintf(g_cap_dev_uid, sizeof g_cap_dev_uid, "%s", uid ? uid : "");
}

const char *platform_audio_input_device(void) { return g_cap_dev_uid; }

static bool liu_cap_engine_start(void) {
    @autoreleasepool {
        AVAudioEngine *engine = [[AVAudioEngine alloc] init];
        if (!engine) return false;

        AVAudioInputNode *input = [engine inputNode];

        /* Point the input node's AUHAL at the pinned device BEFORE asking for
         * its format. The order matters: -outputFormatForBus: reports the
         * CURRENT device's format, the tap must be installed with that exact
         * format (a mismatch is the classic -10868), and setting the device
         * afterwards would leave a converter built for the old one. An absent
         * UID is left alone deliberately: unplugging a USB mic should degrade
         * to the built-in one, not to a dictation that cannot start. */
        if (g_cap_dev_uid[0]) {
            AudioObjectID dev = liu_cad_device_for_uid(g_cap_dev_uid);
            AudioUnit au = [input audioUnit];
            if (dev != kAudioObjectUnknown && au) {
                (void)AudioUnitSetProperty(au, kAudioOutputUnitProperty_CurrentDevice,
                                           kAudioUnitScope_Global, 0,
                                           &dev, (UInt32)sizeof dev);
            }
        }

        AVAudioFormat *in_fmt = [input outputFormatForBus:0];
        if (!in_fmt || in_fmt.sampleRate <= 0 || in_fmt.channelCount == 0) {
            [engine release];
            return false;
        }

        AVAudioFormat *out_fmt =
            [[[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                              sampleRate:(double)PLATFORM_CAPTURE_RATE
                                                channels:1
                                             interleaved:NO] autorelease];
        if (!out_fmt) { [engine release]; return false; }

        AVAudioConverter *conv = [[AVAudioConverter alloc] initFromFormat:in_fmt toFormat:out_fmt];
        if (!conv) { [engine release]; return false; }

        /* Buffer big enough for the worst realistic hop after resampling. */
        const AVAudioFrameCount out_cap = 8192;

        [input installTapOnBus:0
                    bufferSize:4096
                        format:in_fmt
                         block:^(AVAudioPCMBuffer *buf, AVAudioTime *when) {
            (void)when;
            if (!atomic_load_explicit(&g_cap_running, memory_order_relaxed)) return;

            AVAudioPCMBuffer *out =
                [[AVAudioPCMBuffer alloc] initWithPCMFormat:out_fmt frameCapacity:out_cap];
            if (!out) return;

            __block AVAudioPCMBuffer *pending = buf;
            NSError *cerr = nil;
            AVAudioConverterOutputStatus st = [conv convertToBuffer:out error:&cerr
                withInputFromBlock:^AVAudioBuffer *(AVAudioPacketCount need,
                                                    AVAudioConverterInputStatus *status) {
                    (void)need;
                    /* One shot: the tap already holds exactly one input
                     * buffer, and claiming more would make the converter
                     * block waiting for audio that has not arrived. */
                    if (!pending) { *status = AVAudioConverterInputStatus_NoDataNow; return nil; }
                    AVAudioPCMBuffer *b = pending;
                    pending = nil;
                    *status = AVAudioConverterInputStatus_HaveData;
                    return b;
                }];

            if ((st == AVAudioConverterOutputStatus_HaveData ||
                 st == AVAudioConverterOutputStatus_InputRanDry) &&
                out.frameLength > 0) {
                const float *s = out.floatChannelData[0];
                const uint32_t n = (uint32_t)out.frameLength;

                float peak = 0.0f;
                for (uint32_t i = 0; i < n; i++) {
                    float a = fabsf(s[i]);
                    if (a > peak) peak = a;
                }
                /* Asymmetric smoothing: jump to a new peak immediately so the
                 * meter answers speech onset, decay slowly so it does not
                 * flicker between syllables. */
                uint32_t prev_bits = atomic_load_explicit(&g_cap_level, memory_order_relaxed);
                float prev; memcpy(&prev, &prev_bits, sizeof prev);
                float lvl = peak > prev ? peak : prev * 0.85f + peak * 0.15f;
                uint32_t bits; memcpy(&bits, &lvl, sizeof bits);
                atomic_store_explicit(&g_cap_level, bits, memory_order_relaxed);

                liu_cap_push(s, n);
            }
            [out release];
        }];

        NSError *err = nil;
        [engine prepare];
        if (![engine startAndReturnError:&err]) {
            @try { [input removeTapOnBus:0]; } @catch (NSException *e) { (void)e; }
            [conv release];
            [engine release];
            return false;
        }

        g_cap_engine    = engine;      /* keep the +1 from alloc */
        g_cap_converter = conv;
        atomic_store(&g_cap_running, true);
        return true;
    }
}

bool platform_audio_capture_start(void) {
    if (g_cap_engine != nil) return false;              /* already capturing */
    if (g_audio_recorder != nil) return false;          /* file recorder owns the mic */

    if (!g_cap_ring) {
        g_cap_ring = calloc(LIU_CAP_RING, sizeof *g_cap_ring);
        if (!g_cap_ring) return false;
    }
    atomic_store(&g_cap_wr, 0);
    atomic_store(&g_cap_rd, 0);
    atomic_store(&g_cap_overflow, false);
    atomic_store(&g_cap_level, 0);

    @autoreleasepool {
        AVAuthorizationStatus st =
            [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        if (st == AVAuthorizationStatusAuthorized) {
            g_cap_gen++;
            return liu_cap_engine_start();
        }
        if (st == AVAuthorizationStatusNotDetermined) {
            /* Same generation guard as the file recorder: a grant that lands
             * after the user cancelled must not silently open the mic. */
            int my_gen = ++g_cap_gen;
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                     completionHandler:^(BOOL granted) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    @autoreleasepool {
                        if (granted && my_gen == g_cap_gen && g_cap_engine == nil) {
                            liu_cap_engine_start();
                        }
                    }
                });
            }];
            return true;    /* optimistic — capture begins on grant */
        }
        return false;       /* denied / restricted */
    }
}

bool platform_audio_capture_stop(void) {
    g_cap_gen++;            /* invalidate a pending permission-grant start */
    if (g_cap_engine == nil) return false;
    atomic_store(&g_cap_running, false);
    @autoreleasepool { liu_cap_teardown(); }
    return true;
}

bool platform_audio_capturing(void) {
    return atomic_load(&g_cap_running);
}

i32 platform_audio_capture_read(f32 *out, i32 max_samples) {
    if (!out || max_samples <= 0 || !g_cap_ring) return 0;

    uint64_t rd = atomic_load_explicit(&g_cap_rd, memory_order_relaxed);
    uint64_t wr = atomic_load_explicit(&g_cap_wr, memory_order_acquire);

    uint64_t avail = wr - rd;
    if (avail > (uint64_t)max_samples) avail = (uint64_t)max_samples;
    for (uint64_t i = 0; i < avail; i++) {
        out[i] = g_cap_ring[(rd + i) & (LIU_CAP_RING - 1)];
    }
    atomic_store_explicit(&g_cap_rd, rd + avail, memory_order_release);
    return (i32)avail;
}

bool platform_audio_capture_overflowed(void) {
    return atomic_exchange(&g_cap_overflow, false);
}

/* ---- short clips (the dictation cues) ------------------------------------
 *
 * AVAudioPlayer over a WAV built in memory: it owns its own output, so it
 * neither needs nor disturbs the capture engine above, and asks the user for
 * nothing. Primed at set — prepareToPlay is the call that pays for the
 * hardware, and paying it at play would make the first cue of a session
 * late. */
static AVAudioPlayer *g_clip[PLATFORM_AUDIO_CLIPS];

bool platform_audio_clip_set(i32 slot, const f32 *mono, i32 frames, i32 sample_rate) {
    if (slot < 0 || slot >= PLATFORM_AUDIO_CLIPS) return false;
    if (!mono || frames <= 0 || frames > 48000 * 10 || sample_rate < 8000) return false;
    @autoreleasepool {
        const uint32_t bytes = (uint32_t)frames * 2u, rate = (uint32_t)sample_rate;
        NSMutableData *wav = [NSMutableData dataWithLength:44 + bytes];
        uint8_t *h = wav.mutableBytes;
        #define LIU_LE32(p, v) do { (p)[0] = (uint8_t)(v); (p)[1] = (uint8_t)((v) >> 8); \
                                    (p)[2] = (uint8_t)((v) >> 16); (p)[3] = (uint8_t)((v) >> 24); } while (0)
        memcpy(h, "RIFF", 4);  LIU_LE32(h + 4, 36u + bytes);
        memcpy(h + 8, "WAVEfmt ", 8); LIU_LE32(h + 16, 16u);
        h[20] = 1; h[21] = 0;                 /* PCM */
        h[22] = 1; h[23] = 0;                 /* mono */
        LIU_LE32(h + 24, rate); LIU_LE32(h + 28, rate * 2u);
        h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0;
        memcpy(h + 36, "data", 4); LIU_LE32(h + 40, bytes);
        #undef LIU_LE32
        for (i32 i = 0; i < frames; i++) {
            float v = mono[i];
            if (!(v >= -1.0f)) v = -1.0f;     /* also catches NaN */
            if (v > 1.0f) v = 1.0f;
            const int16_t q = (int16_t)lrintf(v * 32767.0f);
            h[44 + 2 * i] = (uint8_t)q; h[45 + 2 * i] = (uint8_t)((uint16_t)q >> 8);
        }
        NSError *err = nil;
        AVAudioPlayer *p = [[AVAudioPlayer alloc] initWithData:wav error:&err];
        if (!p) return false;
        [p prepareToPlay];
        [g_clip[slot] stop];
        [g_clip[slot] release];
        g_clip[slot] = p;                     /* retained by alloc */
    }
    return true;
}

bool platform_audio_clip_play(i32 slot, f32 gain) {
    if (slot < 0 || slot >= PLATFORM_AUDIO_CLIPS) return false;
    AVAudioPlayer *p = g_clip[slot];
    if (!p) return false;
    if (!(gain >= 0.0f)) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    p.volume = gain;
    if (p.playing) [p stop];
    p.currentTime = 0;
    return [p play] ? true : false;
}

f32 platform_audio_capture_level(void) {
    if (!atomic_load(&g_cap_running)) return 0.0f;
    uint32_t bits = atomic_load_explicit(&g_cap_level, memory_order_relaxed);
    float v; memcpy(&v, &bits, sizeof v);
    if (!(v >= 0.0f)) return 0.0f;
    return v > 1.0f ? 1.0f : v;
}
