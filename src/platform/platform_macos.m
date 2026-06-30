/*
 * Liu - macOS platform layer (Cocoa + Metal/NSOpenGL)
 */
#import <Cocoa/Cocoa.h>
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
#include "ui/layout.h"
#include "core/crashlog.h"
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>           /* fmax/fmin in liu_center_traffic_lights */

/* =========================================================================
 * Internal state
 * ========================================================================= */

static struct {
    bool initialized;
    f64  time_base;
    mach_timebase_info_data_t timebase_info;
} g_platform = {0};

/* Event queue */
#define MAX_EVENTS 256
static PlatformEvent g_events[MAX_EVENTS];
static i32 g_event_read  = 0;
static i32 g_event_write = 0;
static i32 g_event_count = 0;

static bool g_option_as_alt = false;

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
    if (g_event_count >= MAX_EVENTS) {
        /* Queue saturated — e.g. a 120 Hz trackpad scroll (each tick pushes a
         * MOUSE_MOVE *and* a SCROLL) while an agent floods the PTY. Dropping
         * these high-frequency events is what made scrolling intermittently
         * "stick": a SCROLL could only merge into the single newest slot, but
         * that slot was usually the MOUSE_MOVE pushed right before it, so every
         * SCROLL got discarded until the queue drained. Instead scan back for
         * the most recent same-type event and fold into it — accumulate scroll
         * deltas, overwrite mouse-move with the latest position. Nothing lost. */
        if (e.type == EVENT_SCROLL || e.type == EVENT_MOUSE_MOVE ||
            e.type == EVENT_DRAG_ENTER) {
            for (i32 i = 0; i < MAX_EVENTS; i++) {
                i32 idx = (g_event_write + MAX_EVENTS - 1 - i) % MAX_EVENTS;
                PlatformEvent *p = &g_events[idx];
                if (p->type != e.type) continue;
                if (e.type == EVENT_SCROLL) {
                    if (p->scroll.mods == e.scroll.mods &&
                        p->scroll.precise == e.scroll.precise &&
                        p->scroll.momentum == e.scroll.momentum) {
                        p->scroll.dx += e.scroll.dx;
                        p->scroll.dy += e.scroll.dy;
                        return;
                    }
                } else if (e.type == EVENT_DRAG_ENTER) {
                    p->drag = e.drag; /* latest drag position wins */
                    return;
                } else {
                    p->mouse = e.mouse; /* latest hover position wins */
                    return;
                }
            }
        }
        return;
    }
    g_events[g_event_write] = e;
    g_event_write = (g_event_write + 1) % MAX_EVENTS;
    g_event_count++;
}

/* =========================================================================
 * Quake mode state
 * ========================================================================= */

static struct {
    bool     enabled;
    bool     visible;
    bool     animating;
    NSRect   saved_frame;  /* frame before quake mode was enabled */
    NSUInteger saved_style; /* window style before quake mode */
    NSInteger saved_level;  /* window level before quake mode */
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

@interface SSHAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation SSHAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}

/* Menu action handlers — push events to the queue */
- (void)menuNewTab:(id)sender      { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_NEW_TAB}); }
- (void)menuNewWindow:(id)sender   { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_NEW_WINDOW}); }
- (void)menuCloseTab:(id)sender    { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_CLOSE_TAB}); }
- (void)menuSettings:(id)sender    { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_SETTINGS}); }
- (void)menuSSHConnect:(id)sender  { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_SSH_CONNECT}); }
- (void)menuImportSSH:(id)sender   { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_IMPORT_SSH}); }
- (void)menuToggleSidebar:(id)sender { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_TOGGLE_SIDEBAR}); }
- (void)menuFontBigger:(id)sender  { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_FONT_BIGGER}); }
- (void)menuFontSmaller:(id)sender { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_FONT_SMALLER}); }
- (void)menuFontReset:(id)sender   { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_FONT_RESET}); }
- (void)menuFind:(id)sender        { (void)sender; push_event((PlatformEvent){.type = EVENT_MENU_FIND}); }
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
 * our usual main loop is starved.
 * ========================================================================= */
static PlatformRenderCallback g_render_cb = NULL;
static void                  *g_render_user = NULL;
static PlatformTitlebarZoomQuery g_zoom_query = NULL;
static void                     *g_zoom_user  = NULL;

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
 * by platform_set_titlebar_height so the lights track the real strip — which is
 * TOOLBAR_HEIGHT_PT (42) normally, but a shorter macOS drag strip (~30) when
 * the tab bar and toolbar icons are both hidden. Defaults to the common case. */
static CGFloat g_titlebar_strip_pt = TOOLBAR_HEIGHT_PT;

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
    CGFloat stripH = g_titlebar_strip_pt;
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

void platform_set_titlebar_height(f32 points) {
    if (points <= 0.0f) return;
    CGFloat v = (CGFloat)points;
    if (v == g_titlebar_strip_pt) return;
    g_titlebar_strip_pt = v;
    /* Re-center for the new strip height immediately (e.g. the user toggled the
     * tab bar off); the main window owns the standard buttons. */
    liu_center_traffic_lights([NSApp mainWindow]);
}

@implementation SSHWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    self.shouldClose = YES;
    PlatformEvent e = { .type = EVENT_CLOSE };
    push_event(e);
    return NO; /* We handle closing ourselves */
}
- (void)windowDidResize:(NSNotification *)notification {
    NSWindow *win = notification.object;
    NSRect frame = [[win contentView] frame];
    PlatformEvent e = {
        .type = EVENT_RESIZE,
        .resize = { .width = (i32)frame.size.width, .height = (i32)frame.size.height }
    };
    push_event(e);
    liu_center_traffic_lights(win);
}
- (void)windowDidBecomeKey:(NSNotification *)n {
    liu_center_traffic_lights(n.object);
    push_event((PlatformEvent){ .type = EVENT_FOCUS, .focus.focused = true });
}
- (void)windowDidResignKey:(NSNotification *)n {
    (void)n;
    push_event((PlatformEvent){ .type = EVENT_BLUR, .focus.focused = false });
}
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
    push_event((PlatformEvent){
        .type = EVENT_DPI_CHANGE,
        .dpi = { .scale = (f32)newScale }
    });
}
@end

/* =========================================================================
 * Custom NSView for input
 * ========================================================================= */

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
 * titlebar so our tab bar paints into the title-bar strip. AppKit's default
 * is to treat that strip as window-drag — so dragging a tab moved the
 * window instead. Returning NO from this override makes the entire content
 * view non-draggable; window dragging still works on the legitimate empty
 * areas of the title bar (right of the resource monitor) because AppKit
 * falls back to its own title-bar machinery there. */
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
    push_event((PlatformEvent){
        .type = EVENT_RESIZE,
        .resize = { .width = (i32)newSize.width, .height = (i32)newSize.height }
    });
}

/* Live resize: ensure clean rendering during drag.
 *
 * AppKit pumps the runloop in NSEventTrackingRunLoopMode while the user is
 * dragging the window edge — our main loop (which renders) doesn't get to
 * run until the drag ends, so the newly-exposed pixels in the larger
 * drawable would otherwise show as black. Solution: install an NSTimer
 * registered on the common runloop modes that calls the platform render
 * callback every frame while live-resize is active. */
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
    if (g_render_cb) g_render_cb(g_render_user);
}

- (void)liveResizeTick:(NSTimer *)t {
    (void)t;
    if (g_render_cb) g_render_cb(g_render_user);
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
    /* Push resize event so main loop re-renders with correct size */
    NSRect frame = [self bounds];
    push_event((PlatformEvent){
        .type = EVENT_RESIZE,
        .resize = { .width = (i32)frame.size.width, .height = (i32)frame.size.height }
    });
}

- (void)update {
    [super update];
    [[self openGLContext] update];
}

/* See SSHView (Metal) variant for the rationale — same fix on the OpenGL
 * path: drive an NSTimer in common runloop modes so we keep rendering while
 * the user is dragging the window edge. */
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
    if (g_render_cb) g_render_cb(g_render_user);
}

- (void)liveResizeTick:(NSTimer *)t {
    (void)t;
    if (g_render_cb) g_render_cb(g_render_user);
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
     * We emit CHAR_INPUT directly for Ctrl+A..Z so the terminal gets ^A..^Z.
     * Do this before KEY_DOWN so terminal_key_input doesn't also emit a
     * CSI-u sequence for the same physical key.
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
    if ((mods & MOD_ALT) && g_option_as_alt) {
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
 * with Y flipped (top=0 for our rendering coordinate system).
 * Uses convertPointToBacking for correct Retina scaling.
 */
- (void)pushMouseEvent:(EventType)type button:(i32)btn event:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    /* Convert point coordinates to backing (framebuffer) pixels */
    NSPoint bp = [self convertPointToBacking:p];
    NSRect fb = [self convertRectToBacking:self.bounds];
    i32 mx = (i32)bp.x;
    i32 my = (i32)(fb.size.height - bp.y); /* flip Y: bottom-up → top-down */
    push_event((PlatformEvent){
        .type = type,
        .mouse = { .button = btn, .x = mx, .y = my, .mods = get_mods(event.modifierFlags) }
    });
}

- (void)mouseDown:(NSEvent *)event {
    /* Save the event so platform_begin_file_drag can seed a drag session
     * from a later callback without waiting for the next mouseDragged.
     *
     * ARC is not enabled in this target — the ivar is a raw pointer, so we
     * must retain/release manually. Cocoa autoreleases the dispatched
     * NSEvent as soon as the mouse handler returns; without the retain the
     * pointer dangles into dead memory and any subsequent objc_msgSend
     * (e.g. [seed locationInWindow] in platform_begin_file_drag) crashes. */
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
            bool zoomable = !g_zoom_query || g_zoom_query(g_zoom_user, fb_x, fb_y);
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
         * trailing unit, otherwise we'd emit two lone surrogates and corrupt the
         * UTF-8 written to the PTY. BMP chars take the (u32)ch path unchanged. */
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
         * composed ASCII char for Alt+<key> (Meta) and prepends ESC, so Option+Q
         * sent "ESC @" instead of "@". When the user actually wants Option-as-
         * Meta they enable option_as_alt, and keyDown emits the raw key with
         * MOD_ALT *before* interpretKeyEvents — that path never reaches here. */
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

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    NSPasteboard *pb = [sender draggingPasteboard];
    if ([pb canReadObjectForClasses:@[[NSURL class]]
                            options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}]) {
        i32 dx = 0, dy = 0;
        [self draggingPoint:sender x:&dx y:&dy];
        push_event((PlatformEvent){ .type = EVENT_DRAG_ENTER, .drag = { .x = dx, .y = dy } });
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
        push_event((PlatformEvent){ .type = EVENT_DRAG_ENTER, .drag = { .x = dx, .y = dy } });
        return NSDragOperationCopy;
    }
    return NSDragOperationNone;
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
    (void)sender;
    push_event((PlatformEvent){ .type = EVENT_DRAG_EXIT });
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

    push_event((PlatformEvent){ .type = EVENT_DRAG_EXIT }); /* clear overlay */

    /* Drop location (framebuffer px, top-down) so the consumer can target the
     * pane under the cursor rather than the focused one. */
    i32 drop_x = 0, drop_y = 0;
    [self draggingPoint:sender x:&drop_x y:&drop_y];

    for (NSURL *url in urls) {
        const char *path = [[url path] UTF8String];
        if (!path) continue;
        i32 slot = drop_idx++ & 15; /* ring buffer of 16 slots */
        snprintf(drop_paths[slot], sizeof(drop_paths[slot]), "%s", path);
        push_event((PlatformEvent){
            .type = EVENT_DROP_FILE,
            .drop = { .path = drop_paths[slot], .x = drop_x, .y = drop_y }
        });
    }
    return YES;
}

/* ----------------------------------------------------------------------- *
 *  NSDraggingSource — required so a session we start is accepted by Finder
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
    NSValue *ctxVal = info[@"ctx"];
    NSValue *fnVal  = info[@"fn"];
    void *ctx = ctxVal ? [ctxVal pointerValue] : NULL;
    void (*fn)(void *, const char *) =
        fnVal ? (void (*)(void *, const char *))[fnVal pointerValue] : NULL;

    if (!fn) {
        completionHandler([NSError errorWithDomain:NSCocoaErrorDomain
                                              code:NSFileWriteUnknownError
                                          userInfo:nil]);
        return;
    }
    /* Run libssh2 on this provider queue — matches the existing
     * sftp_upload_thread pattern and keeps the main thread free so the
     * sidebar progress panel actually renders while Finder receives the
     * file. Concurrent libssh2 calls from the main thread on the same
     * session race on internal state; in practice libssh2's writeable
     * socket contention is rare and the transfer completes cleanly.
     * A session-level mutex would make this airtight — follow-up. */
    fn(ctx, [[url path] UTF8String]);
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
    }
    return self;
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

struct PlatformWindow {
    NSWindow            *ns_window;
    SSHView             *view;
    SSHWindowDelegate   *delegate;
    NSVisualEffectView  *vibrancy;       /* under-content blur layer; nil until set_transparent */
#ifdef USE_METAL
    id<MTLDevice>       mtl_device;
    CAMetalLayer       *mtl_layer;
    id<MTLCommandQueue> mtl_queue;
#else
    NSOpenGLContext    *gl_context;
#endif
    bool               should_close;
};

/* =========================================================================
 * Implementation
 * ========================================================================= */

/* Funnel an uncaught Objective-C exception (e.g. an invalid window title) into
 * the crash log before the default handler abort()s. Liu pumps its own event
 * loop (nextEventMatchingMask + sendEvent), so exceptions thrown inside AppKit
 * propagate out to us and terminate the app through this handler. */
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

bool platform_init(void) {
    if (g_platform.initialized) return true;
    NSSetUncaughtExceptionHandler(liu_uncaught_exception_handler);
    @autoreleasepool {
        [NSApplication sharedApplication];
        SSHAppDelegate *del = [[SSHAppDelegate alloc] init];
        [NSApp setDelegate:del];

        /* ---- Menu Bar ---- */
        NSMenu *menuBar = [[NSMenu alloc] init];

        /* App menu (Liu) */
        NSMenuItem *appMenuItem = [[NSMenuItem alloc] initWithTitle:@"Liu" action:nil keyEquivalent:@""];
        NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"Liu"];
        [appMenu addItemWithTitle:@"About Liu" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:@"Settings..." action:@selector(menuSettings:) keyEquivalent:@","];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:@"Hide Liu" action:@selector(hide:) keyEquivalent:@"h"];
        NSMenuItem *hideOthers = [appMenu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
        [hideOthers setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
        [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:@"Quit Liu" action:@selector(terminate:) keyEquivalent:@"q"];
        [appMenuItem setSubmenu:appMenu];
        [menuBar addItem:appMenuItem];

        /* Shell menu */
        NSMenuItem *shellMenuItem = [[NSMenuItem alloc] init];
        NSMenu *shellMenu = [[NSMenu alloc] initWithTitle:@"Shell"];
        [shellMenu addItemWithTitle:@"New Tab" action:@selector(menuNewTab:) keyEquivalent:@"t"];
        [shellMenu addItemWithTitle:@"New Window" action:@selector(menuNewWindow:) keyEquivalent:@"n"];
        [shellMenu addItem:[NSMenuItem separatorItem]];
        [shellMenu addItemWithTitle:@"Close Tab" action:@selector(menuCloseTab:) keyEquivalent:@"w"];
        [shellMenu addItem:[NSMenuItem separatorItem]];
        [shellMenu addItemWithTitle:@"SSH Connect..." action:@selector(menuSSHConnect:) keyEquivalent:@""];
        [shellMenu addItemWithTitle:@"Import SSH Config" action:@selector(menuImportSSH:) keyEquivalent:@""];
        [shellMenuItem setSubmenu:shellMenu];
        [menuBar addItem:shellMenuItem];

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

        /* Theme menu */
        NSMenuItem *themeMenuItem = [[NSMenuItem alloc] init];
        NSMenu *themeMenu = [[NSMenu alloc] initWithTitle:@"Theme"];
        NSArray *themes = @[@"Liu", @"Dark", @"Light", @"Solarized Dark",
                           @"Monokai", @"Dracula", @"Nord", @"Gruvbox", @"Catppuccin Mocha"];
        for (NSString *name in themes) {
            NSMenuItem *item = [themeMenu addItemWithTitle:name action:@selector(menuSelectTheme:) keyEquivalent:@""];
            [item setRepresentedObject:name];
        }
        [themeMenuItem setSubmenu:themeMenu];
        [menuBar addItem:themeMenuItem];

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

        [NSApp setMainMenu:menuBar];

        [NSApp finishLaunching];
    }

    mach_timebase_info(&g_platform.timebase_info);
    g_platform.time_base = (f64)mach_absolute_time() *
        g_platform.timebase_info.numer / g_platform.timebase_info.denom / 1e9;
    g_platform.initialized = true;
    return true;
}

void platform_shutdown(void) {
    for (i32 i = 0; i < MAX_SYMBOL_CACHE; i++) {
        if (g_symbol_cache[i].pixels) {
            free(g_symbol_cache[i].pixels);
            g_symbol_cache[i].pixels = NULL;
        }
        g_symbol_cache[i].valid = false;
    }
    g_platform.initialized = false;
}

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

        /* Transparent title bar — our content extends into the title bar area */
        [w->ns_window setTitlebarAppearsTransparent:YES];
        [w->ns_window setTitleVisibility:NSWindowTitleHidden];
        NSString *initTitle = cfg->title ? [NSString stringWithUTF8String:cfg->title] : nil;
        [w->ns_window setTitle:(initTitle ? initTitle : @"")];
        [w->ns_window center];
        [w->ns_window setAcceptsMouseMovedEvents:YES];

        w->delegate = [[SSHWindowDelegate alloc] init];
        [w->ns_window setDelegate:w->delegate];

#ifdef USE_METAL
        w->view = [[SSHView alloc] initWithFrame:rect];
        [w->view setWantsLayer:YES];
        /* Redraw immediately on bounds change — prevents stale content during resize */
        w->view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
        w->view.layerContentsPlacement = NSViewLayerContentsPlacementTopLeft;
        [w->ns_window setContentView:w->view];
        [w->ns_window makeFirstResponder:w->view];

        w->mtl_device = MTLCreateSystemDefaultDevice();
        w->mtl_layer = (CAMetalLayer *)w->view.layer;
        w->mtl_layer.device = w->mtl_device;
        w->mtl_layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
        /* Triple-buffered to give the CPU/GPU pipeline enough headroom on
         * 120 Hz ProMotion displays — with only 2 drawables, the next frame
         * blocks on the previous present, occasionally dropping us to 60 Hz
         * during modal animations. Triple lets the CPU prepare frame N+2
         * while the GPU renders N+1 and the compositor scans out N. Costs
         * ~5-10 MB of extra IOSurface at retina; the smoothness win is
         * visible whenever something animates (modals, hover, blur). */
        w->mtl_layer.maximumDrawableCount = 3;
        /* framebufferOnly = NO so the renderer can blit the drawable into a
         * separate texture for backdrop-blur capture under modals. Costs
         * ~5MB extra per drawable on Retina; only matters when blur is used. */
        w->mtl_layer.framebufferOnly = NO;
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
        /* OpenGL pixel format — 3.2 Core Profile */
        NSOpenGLPixelFormatAttribute attrs[] = {
            NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
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
        [w->view setWantsBestResolutionOpenGLSurface:YES];
        [w->ns_window setContentView:w->view];
        [w->ns_window makeFirstResponder:w->view];

        w->gl_context = [w->view openGLContext];
        [w->gl_context makeCurrentContext];

        if (cfg->vsync) {
            GLint swapInterval = 1;
            [w->gl_context setValues:&swapInterval forParameter:NSOpenGLContextParameterSwapInterval];
        }
#endif

        [w->ns_window makeKeyAndOrderFront:nil];
        return w;
    }
}

void platform_window_destroy(PlatformWindow *w) {
    if (!w) return;
    @autoreleasepool {
        [w->ns_window close];
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
         * window buttons back to their default (too-high) origin — visible as
         * the traffic lights jumping up when the active tab changes. Re-apply
         * our centering: once now, and once on the next runloop tick because
         * the reset lands on a later layout pass. */
        NSWindow *win = w->ns_window;
        liu_center_traffic_lights(win);
        dispatch_async(dispatch_get_main_queue(), ^{ liu_center_traffic_lights(win); });
    }
}

void platform_window_set_transparent(PlatformWindow *w, bool transparent) {
    @autoreleasepool {
        if (transparent) {
            [w->ns_window setOpaque:NO];
            [w->ns_window setBackgroundColor:[NSColor clearColor]];
#ifdef USE_METAL
            w->mtl_layer.opaque = NO;
#endif
            /* Add the under-content vibrancy view exactly once. Re-toggling
             * transparent before would leak a fresh NSVisualEffectView per
             * call; we now cache it on the window struct and just bring it
             * back to active state if it already exists. */
            if (!w->vibrancy) {
                NSView *content = [w->ns_window contentView];
                w->vibrancy = [[NSVisualEffectView alloc] initWithFrame:content.bounds];
                w->vibrancy.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
                w->vibrancy.blendingMode = NSVisualEffectBlendingModeBehindWindow;
                w->vibrancy.material = NSVisualEffectMaterialHUDWindow;
                w->vibrancy.state = NSVisualEffectStateActive;
                [content addSubview:w->vibrancy positioned:NSWindowBelow relativeTo:nil];
            } else {
                w->vibrancy.state = NSVisualEffectStateActive;
                [w->vibrancy setHidden:NO];
            }
        } else {
            [w->ns_window setOpaque:YES];
            [w->ns_window setBackgroundColor:[NSColor blackColor]];
#ifdef USE_METAL
            w->mtl_layer.opaque = YES;
#endif
            /* Hide instead of removing — keeps the cached view ready for
             * the next transparent toggle without re-allocating. */
            if (w->vibrancy) [w->vibrancy setHidden:YES];
        }
    }
}

void platform_window_set_opacity(PlatformWindow *w, f32 opacity) {
    @autoreleasepool {
        /* Don't use setAlphaValue — that makes the ENTIRE window transparent
         * including overlays like settings panel. Instead, make the window
         * non-opaque with a clear background so the Metal layer can render
         * terminal content with reduced alpha while overlays stay fully opaque. */
        if (opacity < 1.0f) {
            [w->ns_window setOpaque:NO];
            [w->ns_window setBackgroundColor:[NSColor clearColor]];
#ifdef USE_METAL
            w->mtl_layer.opaque = NO;
#endif
        } else {
            [w->ns_window setOpaque:YES];
            [w->ns_window setBackgroundColor:[NSColor blackColor]];
#ifdef USE_METAL
            w->mtl_layer.opaque = YES;
#endif
        }
    }
}

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
        NSRect r = vf;
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
        if (w->view->_lastMouseEvent) {
            [w->ns_window performWindowDragWithEvent:w->view->_lastMouseEvent];
        }
    }
}

bool platform_window_should_close(PlatformWindow *w) {
    return w->delegate.shouldClose;
}

void platform_window_swap_buffers(PlatformWindow *w) {
#ifdef USE_METAL
    (void)w; /* Metal presents in renderer_end_frame */
#else
    [w->gl_context flushBuffer];
#endif
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

        for (i32 i = 0; i < count; i++) {
            const PlatformFilePromise *it = &items[i];

            id<NSPasteboardWriting> writer = nil;
            if (it->is_remote) {
                /* NSFilePromiseProvider with our view as delegate.
                 * Attach ctx + provider fn via userInfo. */
                NSString *type = (NSString *)UTTypeData.identifier;  /* generic file promise */
                /* Try to resolve a better UTI from the filename extension. */
                NSString *name = [NSString stringWithUTF8String:it->display_name];
                NSString *ext = [name pathExtension];
                if (ext.length > 0) {
                    UTType *tt = [UTType typeWithFilenameExtension:ext];
                    if (tt && tt.identifier) type = tt.identifier;
                }
                NSFilePromiseProvider *prov =
                    [[NSFilePromiseProvider alloc] initWithFileType:type
                                                           delegate:view];
                prov.userInfo = @{
                    @"name": name,
                    @"ctx":  [NSValue valueWithPointer:it->ctx],
                    @"fn":   [NSValue valueWithPointer:(void *)it->provider],
                };
                writer = prov;
            } else {
                /* Local item — use a file URL directly. */
                NSURL *url = [NSURL fileURLWithPath:
                                 [NSString stringWithUTF8String:it->local_path]];
                NSPasteboardItem *pi = [[NSPasteboardItem alloc] init];
                [pi setString:[url absoluteString] forType:NSPasteboardTypeFileURL];
                writer = pi;
            }

            NSDraggingItem *di = [[NSDraggingItem alloc] initWithPasteboardWriter:writer];
            /* Stack subsequent items 8 pt below each other so Finder's
             * multi-file badge renders a legible pile. */
            NSRect frame = NSMakeRect(origin.x - 8, origin.y - 8 - i*8, 28, 28);
            [di setDraggingFrame:frame contents:nil];
            [drag_items addObject:di];
        }

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

void platform_poll_events(void) {
    @autoreleasepool {
        for (;;) {
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
            [NSApp sendEvent:event];
        }
    }
}

void platform_wait_events(f64 timeout_sec) {
    @autoreleasepool {
        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:timeout_sec];
        NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
            untilDate:until
            inMode:NSDefaultRunLoopMode
            dequeue:YES];
        if (event) [NSApp sendEvent:event];
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
 * arrivals. Resuming before the drain would defeat the whole thing. ========================================================================= */

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

static void watch_post_wake_event(void) {
    /* Coalesce wake-ups: if a previous one is still queued on the main
     * run loop waiting to be processed, skip this one. Otherwise every
     * byte of a large read would queue its own NSEvent and the main
     * queue would back up even without the re-fire bug. */
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &g_wake_pending, &expected, true,
            memory_order_acquire, memory_order_relaxed)) {
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        atomic_store_explicit(&g_wake_pending, false, memory_order_release);
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
    });
}

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
        /* Suspend immediately so we don't get re-fired before the main
         * thread drains. Paired with dispatch_resume in platform_resume_watches. */
        dispatch_suspend(src);
        atomic_store_explicit(susp, 1, memory_order_release);
        atomic_fetch_add_explicit(&g_fd_fire_count, 1, memory_order_release);
        atomic_fetch_add_explicit(&g_fd_fire_total, 1, memory_order_release);
        watch_post_wake_event();
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
     * the lock + scan entirely. This was the second-largest contributor
     * to idle-frame syscall jitter after cursor blink. */
    static _Atomic uint64_t last_seen_fire_total = 0;
    uint64_t total = atomic_load_explicit(&g_fd_fire_total, memory_order_acquire);
    uint64_t seen = atomic_exchange_explicit(&last_seen_fire_total, total,
                                              memory_order_acq_rel);
    if (total == seen) return;
    pthread_mutex_lock(&g_watch_lock);
    for (int i = 0; i < WATCH_MAX_FD; i++) {
        if (!g_watch_sources[i]) continue;
        int was = atomic_exchange_explicit(&g_watch_suspended[i], 0,
                                            memory_order_acq_rel);
        if (was) dispatch_resume(g_watch_sources[i]);
    }
    pthread_mutex_unlock(&g_watch_lock);
}

i32 platform_take_fd_fire_count(void) {
    return (i32)atomic_exchange_explicit(&g_fd_fire_count, 0,
                                          memory_order_acq_rel);
}

bool platform_next_event(PlatformEvent *event) {
    if (g_event_count == 0) return false;
    *event = g_events[g_event_read];
    g_event_read = (g_event_read + 1) % MAX_EVENTS;
    g_event_count--;
    return true;
}

/* =========================================================================
 * Clipboard
 * ========================================================================= */

/* NSString's -UTF8String is valid only until the NSString is deallocated.
 * Inside @autoreleasepool {...} it drains at the closing brace, so returning
 * that pointer handed the caller a use-after-free — breaking paste in a way
 * that "sometimes worked" depending on what the next allocation overwrote.
 * Copy into a persistent buffer; each call overwrites the previous result
 * (callers consume the value before the next call, which holds today). */
static char   *g_clip_buf = NULL;
static size_t  g_clip_cap = 0;

const char *platform_clipboard_get(void) {
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSString *str = [pb stringForType:NSPasteboardTypeString];
        const char *utf8 = str ? [str UTF8String] : NULL;
        if (!utf8) {
            if (g_clip_buf) g_clip_buf[0] = '\0';
            return g_clip_buf ? g_clip_buf : "";
        }
        size_t len = strlen(utf8);
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

void platform_clipboard_set(const char *text) {
    if (!text) return;
    @autoreleasepool {
        /* stringWithUTF8String returns nil on invalid UTF-8 — don't clobber
         * the pasteboard with a nil setString (crashes on some macOS builds). */
        NSString *ns = [NSString stringWithUTF8String:text];
        if (!ns) return;
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:ns forType:NSPasteboardTypeString];
    }
}

void platform_set_option_as_alt(bool enable) {
    g_option_as_alt = enable;
}

/* =========================================================================
 * URL opening
 * ========================================================================= */

void platform_open_url(const char *url) {
    if (!url || !url[0]) return;
    @autoreleasepool {
        NSString *str = [NSString stringWithUTF8String:url];
        if (!str) return;
        /* Distinguish a real URL from a filesystem path. NSURL URLWithString
         * silently produces a half-baked NSURL for bare paths (no scheme),
         * which NSWorkspace then refuses to open — so a markdown link like
         * "./other.md" used to no-op. fileURLWithPath handles escaping and
         * file:// construction for us. */
        bool has_scheme = ([str rangeOfString:@"://"].location != NSNotFound);
        NSURL *nsurl = has_scheme ? [NSURL URLWithString:str]
                                  : [NSURL fileURLWithPath:str];
        if (nsurl) {
            [[NSWorkspace sharedWorkspace] openURL:nsurl];
        }
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
 * Time
 * ========================================================================= */

f64 platform_time_sec(void) {
    u64 now = mach_absolute_time();
    f64 sec = (f64)now * g_platform.timebase_info.numer / g_platform.timebase_info.denom / 1e9;
    return sec - g_platform.time_base;
}

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
    if (!name || !name[0] || pixel_size <= 0 || !pixels || !width || !height) return false;

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

        /* Clean rasterization pipeline.
         *
         * Old version had three rendering bugs that combined to produce
         * the "icons look ugly" complaint:
         *
         *   1. NSCalibratedRGBColorSpace into a Metal MTLPixelFormatRGBA8Unorm_sRGB
         *      texture: the GPU sampler then decoded the bitmap's calibrated
         *      gamma as if it were sRGB, darkening edges and softening the AA.
         *      Fixed by going through a CGBitmapContext with an explicit
         *      sRGB color space so the pixel buffer matches the texture.
         *
         *   2. 2× supersample then bilinear downsample on the GPU.
         *      SF Symbols are vector — drawing them at native pixel_size
         *      lets CoreGraphics' high-quality AA produce sharper output
         *      than a bilinear box-filter from a 2× source.
         *
         *   3. NSFontWeightSemibold + NSImageSymbolScaleMedium: too heavy
         *      for the small (~14–18 dpi) toolbar icons; strokes ran into
         *      one another. NSFontWeightRegular + Small is a much better
         *      balance and matches the SF Symbol palette used in macOS
         *      menu bars / Finder. */
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
        [image drawInRect:NSMakeRect(0, 0, side, side)
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
         * lets us paint the user's RGB pre-multiplied by that coverage. The
         * vshift re-centers the visible glyph vertically. */
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
    @autoreleasepool {
        if (enable == g_quake.enabled) return;

        if (enable) {
            /* Save current window state */
            g_quake.saved_frame = [w->ns_window frame];
            g_quake.saved_style = [w->ns_window styleMask];
            g_quake.saved_level = [w->ns_window level];
            g_quake.enabled = true;
            g_quake.visible = true;

            if (g_quake.height_ratio <= 0.0f) g_quake.height_ratio = 0.4f;
            if (g_quake.anim_duration <= 0.0f) g_quake.anim_duration = 0.2f;

            /* Configure window for quake mode */
            [w->ns_window setStyleMask:NSWindowStyleMaskBorderless |
                                       NSWindowStyleMaskFullSizeContentView];
            [w->ns_window setLevel:NSFloatingWindowLevel];
            [w->ns_window setHasShadow:YES];
            [w->ns_window setOpaque:YES];

            /* Set frame to full width, specified height, top of screen */
            NSScreen *screen = [w->ns_window screen];
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
            [w->ns_window setFrame:offscreenFrame display:NO];
            [w->ns_window makeKeyAndOrderFront:nil];

            g_quake.animating = true;
            [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
                ctx.duration = g_quake.anim_duration;
                ctx.timingFunction = [CAMediaTimingFunction functionWithName:
                    kCAMediaTimingFunctionEaseOut];
                [[w->ns_window animator] setFrame:quakeFrame display:YES];
            } completionHandler:^{
                g_quake.animating = false;
                push_event((PlatformEvent){
                    .type = EVENT_RESIZE,
                    .resize = {
                        .width = (i32)quakeFrame.size.width,
                        .height = (i32)quakeFrame.size.height
                    }
                });
            }];

            [NSApp activateIgnoringOtherApps:YES];
        } else {
            /* Restore original window state */
            g_quake.enabled = false;
            g_quake.visible = false;

            [w->ns_window setStyleMask:g_quake.saved_style];
            [w->ns_window setLevel:g_quake.saved_level];
            [w->ns_window setFrame:g_quake.saved_frame display:YES animate:YES];
            [w->ns_window setTitlebarAppearsTransparent:YES];
            [w->ns_window setTitleVisibility:NSWindowTitleHidden];
        }
    }
}

void platform_toggle_quake_window(PlatformWindow *w) {
    @autoreleasepool {
        if (!g_quake.enabled) return;
        if (g_quake.animating) return;

        NSScreen *screen = [w->ns_window screen];
            if (!screen) screen = [NSScreen mainScreen];
        NSRect screenFrame = [screen visibleFrame];
        f32 quake_h = screenFrame.size.height * g_quake.height_ratio;

        if (g_quake.visible) {
            /* Slide up and hide */
            g_quake.animating = true;
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
                [[w->ns_window animator] setFrame:offscreenFrame display:YES];
            } completionHandler:^{
                g_quake.animating = false;
                g_quake.visible = false;
                [w->ns_window orderOut:nil];
            }];
        } else {
            /* Slide down and show */
            NSRect offscreenFrame = NSMakeRect(
                screenFrame.origin.x,
                screenFrame.origin.y + screenFrame.size.height,
                screenFrame.size.width,
                quake_h
            );
            [w->ns_window setFrame:offscreenFrame display:NO];
            [w->ns_window makeKeyAndOrderFront:nil];

            NSRect quakeFrame = NSMakeRect(
                screenFrame.origin.x,
                screenFrame.origin.y + screenFrame.size.height - quake_h,
                screenFrame.size.width,
                quake_h
            );

            g_quake.animating = true;
            g_quake.visible = true;
            [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
                ctx.duration = g_quake.anim_duration;
                ctx.timingFunction = [CAMediaTimingFunction functionWithName:
                    kCAMediaTimingFunctionEaseOut];
                [[w->ns_window animator] setFrame:quakeFrame display:YES];
            } completionHandler:^{
                g_quake.animating = false;
                push_event((PlatformEvent){
                    .type = EVENT_RESIZE,
                    .resize = {
                        .width = (i32)quakeFrame.size.width,
                        .height = (i32)quakeFrame.size.height
                    }
                });
            }];

            [NSApp activateIgnoringOtherApps:YES];
        }
    }
}

bool platform_is_quake_mode(PlatformWindow *w) {
    (void)w;
    return g_quake.enabled;
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

/* =========================================================================
 * Native Settings Panel — NSPanel + NSToolbar + Auto Layout
 *
 * Replaces the previous one-screen scrolling grid of NSButtons.
 *
 * Design goals:
 *   - Sectioned: Appearance / Typography / Behavior — System Settings cadence.
 *   - Native controls: NSPopUpButton for picklists, NSSlider with live
 *     value label for ranges, NSColorWell-like swatch for theme preview.
 *   - Auto Layout via NSStackView so reflow is automatic on font-size /
 *     locale changes; no manual NSRect math.
 *   - Vibrancy backdrop matching the rest of the chrome.
 *   - Sticky bottom action row (Done / Reset).
 *
 * The C-side entrypoint signature `platform_show_settings(...)` is
 * unchanged so main.c doesn't have to know any of this exists.
 * ========================================================================= */

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
    NSBox *box = [[NSBox alloc] init];
    box.translatesAutoresizingMaskIntoConstraints = NO;
    box.boxType = NSBoxCustom;
    box.borderColor = [NSColor.separatorColor colorWithAlphaComponent:0.5];
    box.borderWidth = 1.0;
    box.cornerRadius = 8.0;
    box.contentViewMargins = NSMakeSize(14, 12);
    box.fillColor = [NSColor.controlBackgroundColor colorWithAlphaComponent:0.35];
    box.title = @"";

    NSStackView *vstack = [[NSStackView alloc] init];
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

    NSStackView *row = [[NSStackView alloc] init];
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
    NSStackView *col = [[NSStackView alloc] init];
    col.translatesAutoresizingMaskIntoConstraints = NO;
    col.orientation = NSUserInterfaceLayoutOrientationVertical;
    col.alignment = NSLayoutAttributeLeading;
    col.spacing = 16;
    col.edgeInsets = NSEdgeInsetsMake(20, 24, 20, 24);

    /* — Theme picker — */
    NSPopUpButton *themePop = [[NSPopUpButton alloc] init];
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

    NSSlider *opSlider = [[NSSlider alloc] init];
    opSlider.translatesAutoresizingMaskIntoConstraints = NO;
    opSlider.minValue = 30;
    opSlider.maxValue = 100;
    opSlider.doubleValue = (double)(self.opacity * 100.0f);
    opSlider.continuous = YES;
    opSlider.target = self;
    opSlider.action = @selector(opacityChanged:);

    NSStackView *opRowControls = [[NSStackView alloc] init];
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
    NSStackView *col = [[NSStackView alloc] init];
    col.translatesAutoresizingMaskIntoConstraints = NO;
    col.orientation = NSUserInterfaceLayoutOrientationVertical;
    col.alignment = NSLayoutAttributeLeading;
    col.spacing = 16;
    col.edgeInsets = NSEdgeInsetsMake(20, 24, 20, 24);

    /* — Font family picker — */
    NSPopUpButton *fontPop = [[NSPopUpButton alloc] init];
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

    NSStackView *fontRowControls = [[NSStackView alloc] init];
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
    self.fontSizeValueLabel = [[NSTextField alloc] init];
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

    self.fontSizeSlider = [[NSSlider alloc] init];
    self.fontSizeSlider.translatesAutoresizingMaskIntoConstraints = NO;
    /* Range aligned with on_native_value_change's clamp (6..96) so a
     * config-loaded value above 48 doesn't snap back to the old slider
     * cap when the user drags it. */
    self.fontSizeSlider.minValue = 6;
    self.fontSizeSlider.maxValue = 96;
    self.fontSizeSlider.doubleValue = (double)self.fontSize;
    self.fontSizeSlider.numberOfTickMarks = 0;
    self.fontSizeSlider.allowsTickMarkValuesOnly = NO;
    self.fontSizeSlider.continuous = YES;
    self.fontSizeSlider.target = self;
    self.fontSizeSlider.action = @selector(sizeSliderChanged:);

    NSStackView *sizeRowControls = [[NSStackView alloc] init];
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

    self.fontWeightSlider = [[NSSlider alloc] init];
    self.fontWeightSlider.translatesAutoresizingMaskIntoConstraints = NO;
    /* Range aligned with on_native_value_change's clamp (0..2). The
     * previous 1.5 cap silently re-clipped a heavier stroke supplied
     * through JSON whenever the user touched the slider. */
    self.fontWeightSlider.minValue = 0.0;
    self.fontWeightSlider.maxValue = 2.0;
    self.fontWeightSlider.doubleValue = (double)self.fontWeight;
    self.fontWeightSlider.continuous = YES;
    self.fontWeightSlider.target = self;
    self.fontWeightSlider.action = @selector(weightSliderChanged:);

    NSStackView *weightRowControls = [[NSStackView alloc] init];
    weightRowControls.translatesAutoresizingMaskIntoConstraints = NO;
    weightRowControls.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    weightRowControls.alignment = NSLayoutAttributeCenterY;
    weightRowControls.spacing = 10;
    [weightRowControls addArrangedSubview:self.fontWeightSlider];
    [weightRowControls addArrangedSubview:self.fontWeightValueLabel];

    NSStackView *weightRow = [self rowWithLabel:@"Line Weight" control:weightRowControls];

    NSStackView *fontStack = [[NSStackView alloc] init];
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
    NSStackView *col = [[NSStackView alloc] init];
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

    NSSlider *sleepSlider = [[NSSlider alloc] init];
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

    NSStackView *sleepRowControls = [[NSStackView alloc] init];
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

    NSStackView *vs = [[NSStackView alloc] init];
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
    for (NSView *v in [self.sectionContainer.subviews copy]) {
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
    for (NSView *subview in [content.subviews copy]) {
        [subview removeFromSuperview];
    }

    /* Vibrancy backdrop — matches the rest of the app's modals. */
    NSVisualEffectView *blur = [[NSVisualEffectView alloc] initWithFrame:content.bounds];
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
    NSSegmentedControl *seg = [[NSSegmentedControl alloc] init];
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
    self.sectionContainer = [[NSView alloc] init];
    self.sectionContainer.translatesAutoresizingMaskIntoConstraints = NO;

    /* Sticky bottom bar with Done. */
    NSButton *doneBtn = [NSButton buttonWithTitle:@"Done" target:self action:@selector(doneClicked:)];
    doneBtn.bezelStyle = NSBezelStyleRounded;
    doneBtn.keyEquivalent = @"\r";   /* Enter */

    NSStackView *footer = [[NSStackView alloc] init];
    footer.translatesAutoresizingMaskIntoConstraints = NO;
    footer.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    footer.alignment = NSLayoutAttributeCenterY;
    footer.distribution = NSStackViewDistributionFill;
    footer.edgeInsets = NSEdgeInsetsMake(12, 20, 14, 20);
    NSView *spring = [[NSView alloc] init];
    spring.translatesAutoresizingMaskIntoConstraints = NO;
    [spring setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [footer addArrangedSubview:spring];
    [footer addArrangedSubview:doneBtn];

    /* Header strip hosts the segmented control */
    NSView *header = [[NSView alloc] init];
    header.translatesAutoresizingMaskIntoConstraints = NO;
    [header addSubview:seg];
    [NSLayoutConstraint activateConstraints:@[
        [seg.centerXAnchor constraintEqualToAnchor:header.centerXAnchor],
        [seg.topAnchor     constraintEqualToAnchor:header.topAnchor    constant:14],
        [seg.bottomAnchor  constraintEqualToAnchor:header.bottomAnchor constant:-12],
    ]];

    NSStackView *root = [[NSStackView alloc] init];
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
            g_settings_ctrl.fontNames = [NSMutableArray new];
            g_settings_ctrl.fontPaths = [NSMutableArray new];
            g_settings_ctrl.fontInstalled = [NSMutableArray new];
            g_settings_ctrl.themeNames = [NSMutableArray new];
            /* Intercept traffic-light close so it routes through the
             * controller's animated fade-out instead of vanishing. */
            g_settings_panel.delegate = g_settings_ctrl;
        }
    }

    [g_settings_ctrl.fontNames removeAllObjects];
    [g_settings_ctrl.fontPaths removeAllObjects];
    [g_settings_ctrl.fontInstalled removeAllObjects];
    [g_settings_ctrl.themeNames removeAllObjects];
    g_settings_ctrl.currentFont = [NSString stringWithUTF8String:(current_font ? current_font : "")];
    g_settings_ctrl.currentTheme = [NSString stringWithUTF8String:(current_theme ? current_theme : "")];
    g_settings_ctrl.fontSize = font_size;
    g_settings_ctrl.fontWeight = font_weight;
    g_settings_ctrl.opacity = opacity;
    g_settings_ctrl.tabSleepMinutes = tab_sleep_minutes;

    for (i32 i = 0; i < font_count; i++) {
        [g_settings_ctrl.fontNames addObject:[NSString stringWithUTF8String:font_names[i]]];
        [g_settings_ctrl.fontPaths addObject:[NSString stringWithUTF8String:font_paths[i]]];
        [g_settings_ctrl.fontInstalled addObject:@(font_installed[i])];
    }
    for (i32 i = 0; i < theme_count; i++) {
        [g_settings_ctrl.themeNames addObject:[NSString stringWithUTF8String:theme_names[i]]];
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

void platform_post_notification(const char *title, const char *body) {
    @autoreleasepool {
        if (@available(macOS 11.0, *)) {
            /* Use UNUserNotificationCenter (modern API) */
            /* Requires UserNotifications framework -- fallback to NSLog if not linked */
            Class UNClass = NSClassFromString(@"UNUserNotificationCenter");
            if (UNClass) {
                /* UserNotifications requires entitlements; fall through to log */
            }
            /* For a CLI/non-sandboxed app, NSLog is the simplest notification */
            NSLog(@"Liu: %s -- %s", title, body);
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            NSUserNotification *notif = [[NSUserNotification alloc] init];
            notif.title = [NSString stringWithUTF8String:title];
            notif.informativeText = [NSString stringWithUTF8String:body];
            notif.soundName = NSUserNotificationDefaultSoundName;
            [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:notif];
#pragma clang diagnostic pop
        }
    }
}

void platform_request_attention(void) {
    @autoreleasepool {
        [NSApp requestUserAttention:NSInformationalRequest];
    }
}

bool platform_is_app_focused(void) {
    return [NSApp isActive];
}

bool platform_utf8_normalize_nfc(const char *src, char *dst, usize dstcap) {
    if (!dst || dstcap == 0) return false;
    if (!src) { dst[0] = '\0'; return false; }
    @autoreleasepool {
        /* Capture src into an NSString BEFORE touching dst — callers may pass
         * dst == src (e.g. re-normalizing a buffer in place), and clearing
         * dst[0] first would blank the input out from under us. */
        NSString *s = [NSString stringWithUTF8String:src];
        if (!s) { dst[0] = '\0'; return false; }
        NSMutableString *ms = [s mutableCopy];
        if (!ms) { dst[0] = '\0'; return false; }
        /* In-place; equivalent to creating a precomposedStringWithCanonicalMapping
         * but avoids a temporary NSString allocation. Form C = canonical
         * decomposition followed by canonical composition. */
        CFStringNormalize((__bridge CFMutableStringRef)ms, kCFStringNormalizationFormC);
        const char *u = [ms UTF8String];
        if (!u) { dst[0] = '\0'; return false; }
        usize n = strlen(u);
        if (n >= dstcap) n = dstcap - 1;
        memcpy(dst, u, n);
        dst[n] = '\0';
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
        /* MRC: drop our +1 from dispatch_source_create. Cancellation releases
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

    int fd = open(path, O_EVTONLY);
    if (fd < 0) return false;

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
