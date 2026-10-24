/*
 * Liu Browser — WKWebView host (macOS) + Select Element bridge.
 */
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include "browser/browser.h"
#include "browser/browser_nav.h"
#include "browser/pick_element_js.h"
#include "browser/browser_chrome_js.h"
#include "browser/browser_gesture_js.h"
#include "platform/platform.h"
#include "core/text_sanitize.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>

@interface LiuWKWebView : WKWebView
@end

@implementation LiuWKWebView
- (BOOL)performKeyEquivalent:(NSEvent *)event {
    if (event.type != NSEventTypeKeyDown)
        return [super performKeyEquivalent:event];
    i32 key = platform_map_nsevent_key((__bridge void *)event);
    u32 mods = platform_map_nsevent_mods((__bridge void *)event);
    if (platform_is_liu_chrome_cmd_key(key, mods)) {
        platform_push_key_down(key, mods, event.isARepeat);
        return YES;
    }
    return [super performKeyEquivalent:event];
}
@end

/* Outermost native view of the browser: a masked window into the page box.
 *
 * It exists because a native view cannot be *covered* by Metal. Liu's Metal
 * chrome — the file-viewer/editor panel above all — is drawn into the same
 * window and simply overlaps whatever it sits on, but the WebView is a real
 * subview stacked ABOVE the Metal layer, so an overlapping panel ends up
 * underneath it and the two fight over the same pixels. Clipping reproduces
 * "covered": the visible region shrinks while the page box inside keeps its
 * full frame, so layout, scroll position and device-emulation scaling are all
 * untouched (see webBox).
 *
 * It is also the one place Liu can get its Cmd+drag pane gesture back over a
 * browser pane: every mouse event inside the page area is consumed by WebKit
 * and the app's hit test (which arms the pane drag) never runs. While
 * `cmdPassthrough` is on — set only when the browser really sits in a split, so
 * a full-tab browser keeps Cmd+click on links — a Cmd-held hit test reports
 * "not mine" and AppKit routes the whole click/drag to the SSHView underneath,
 * exactly as it does over a terminal pane. Modifier state is read live
 * (`+[NSEvent modifierFlags]`) because hit testing carries no event. */
@interface LiuBrowserBox : NSView
@property (nonatomic, assign) BOOL cmdPassthrough;
@end

@implementation LiuBrowserBox
- (NSView *)hitTest:(NSPoint)point {
    if (_cmdPassthrough &&
        ([NSEvent modifierFlags] & NSEventModifierFlagCommand) != 0)
        return nil;
    return [super hitTest:point];
}
@end

/* Navigation decisions belong to the main thread. A superseded decision is
 * completed immediately; its uncancellable resolver may finish in the
 * background, but cannot launch another lookup or touch WebKit from there. */
@interface LiuBrowserDNSRequest : NSObject
@property(nonatomic,copy) void (^handler)(WKNavigationActionPolicy);
@property(nonatomic,copy) NSString *url, *host;
@property(nonatomic,assign) bool mainFrame, userClick, completed;
@property(nonatomic,assign) u64 seq;
- (void)finish:(WKNavigationActionPolicy)policy;
@end
@implementation LiuBrowserDNSRequest
- (void)finish:(WKNavigationActionPolicy)policy {
    if (self.completed) return;
    self.completed = true;
    [self retain];
    void (^handler)(WKNavigationActionPolicy) = [self.handler copy];
    self.handler = nil;
    self.url = nil;  /* a cancelled lookup needs only the bounded hostname */
    if (handler) handler(policy);
    [handler release];
    [self release];
}
- (void)dealloc { self.handler=nil; self.url=nil; self.host=nil; [super dealloc]; }
@end

#ifdef BROWSER_SELFTEST
static BrowserNavClass (*liu_test_resolver)(const char *);
static WKWebsiteDataStore *liu_test_data_store;
void browser_selftest_set_data_store(WKWebsiteDataStore *store);
void browser_selftest_set_data_store(WKWebsiteDataStore *store) {
    if (store == liu_test_data_store) return;
    [liu_test_data_store release]; liu_test_data_store = [store retain];
}
void browser_selftest_set_resolver(BrowserNavClass (*fn)(const char *));
void browser_selftest_set_resolver(BrowserNavClass (*fn)(const char *)) { liu_test_resolver=fn; }
#endif
static BrowserNavClass LiuResolveBrowserHost(const char *host) {
#ifdef BROWSER_SELFTEST
    if (liu_test_resolver) return liu_test_resolver(host);
#endif
    return browser_nav_resolve_host(host);
}

@interface LiuBrowserHost : NSObject <WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler>
@property (nonatomic, strong) LiuWKWebView *webView;
/* Plain container the WKWebView fills. Device emulation scales THIS view's
 * bounds, never the WKWebView's own: WebKit positions its remote-hosted
 * content layers assuming frame == bounds, and scaling the web view directly
 * leaves the page double-scaled and shifted inside the pane. A vanilla NSView
 * has no such assumption, so AppKit's bounds transform composes cleanly over
 * the hosted layer tree while the web view keeps laying out at the exact
 * device size. */
@property (nonatomic, strong) NSView *webBox;
/* Masked window into webBox — visibility, Cmd pass-through and occlusion
 * clipping all live here. See LiuBrowserBox. */
@property (nonatomic, strong) LiuBrowserBox *clipBox;
@property (nonatomic, assign) bool loading;
@property (nonatomic, assign) bool canBack;
@property (nonatomic, assign) bool canFwd;
@property (nonatomic, assign) bool pickMode;
@property (nonatomic, copy) NSString *pickToken;
@property (nonatomic, copy) NSString *chromeToken;
@property (nonatomic, retain) WKNavigation *internalNavigation;
@property (nonatomic, assign) bool internalPolicyPending;
@property (nonatomic, retain) LiuBrowserDNSRequest *dnsRunning, *dnsWaiting;
@property (nonatomic, copy) NSString *gestureURL, *gestureFrameURL;
@property (nonatomic, copy) NSString *gestureToken;
@property (nonatomic, assign) bool gestureMainFrame;
@property (nonatomic, assign) f64 gestureTime;
@property (nonatomic, assign) bool hasPendingPick;
@property (nonatomic, copy)   NSString *pageTitle;
@property (nonatomic, copy)   NSString *pageURL;
@property (nonatomic, copy)   NSString *statusMsg;
/* Pending pick JSON body (UTF-8), owned by host until take_pick. */
@property (nonatomic, assign) char *pendingJson;
@property (nonatomic, assign) f64 lastMsgTime;
@property (nonatomic, assign) i32 msgBurst;
/* Connection-error page state. */
@property (nonatomic, assign) bool errorLoadPending;  /* error HTML load in flight */
@property (nonatomic, assign) bool showingError;      /* error page displayed */
@property (nonatomic, assign) bool hasPendingAsk;     /* user tapped "Ask Agent" */
@property (nonatomic, copy)   NSString *errURL;
@property (nonatomic, copy)   NSString *errReason;
@property (nonatomic, copy)   NSString *errDomain;
@property (nonatomic, copy)   NSString *errDesc;
@property (nonatomic, assign) i32 errCode;
@property (nonatomic, copy)   NSString *errTitle;   /* page headline */
@property (nonatomic, copy)   NSString *errHint;    /* details-panel tip */
/* Bumped by every main-frame policy decision: an async DNS verdict whose
 * number is stale belongs to a navigation the user already left. */
@property (nonatomic, assign) u64 policySeq;
@property (nonatomic, assign) const BrowserAllowlist *allowlist;
/* Internal start page (intro / Recents) state. */
@property (nonatomic, assign) bool startLoadPending;  /* start HTML load in flight */
@property (nonatomic, assign) bool showingStart;      /* start page displayed */
@property (nonatomic, assign) bool hasPendingNav;     /* Recents entry clicked */
@property (nonatomic, copy)   NSString *pendingNavURL;
/* Terminal-theme styling for the internal documents: `:root` declarations and
 * the resolved monospace family. Retained so a theme switch can re-render an
 * on-screen internal page, and `lastRecents` so that re-render reproduces the
 * same start page (intro vs Recents) rather than dropping back to the intro. */
@property (nonatomic, copy)   NSString *pageThemeCSS;
@property (nonatomic, copy)   NSString *pageMonoFamily;
@property (nonatomic, retain) NSArray  *lastRecents;

/* Monotonic state-change counter. Every poll-visible property below bumps it
 * from its (custom) setter, so browser_native_poll is O(1) on the hot path:
 * one counter compare instead of three NSString→UTF8 conversions plus copies
 * per main-loop wake. URL and back/forward state have independent KVO notices:
 * same-document navigation need not change loading or the page title. */
@property (nonatomic, assign) u64 revision;
@end

/* Fallback `:root` declarations for the internal pages, used only until the
 * first browser_native_set_page_theme() push lands (or if the caller passes
 * nothing). Mirrors THEME_Liu so an unthemed first paint is not jarring; the
 * real values always come from the active terminal theme. See the variable
 * contract in app_browser_send_page_theme(). */
static NSString *LiuBrowserFallbackThemeCSS(void) {
    return @"--bg:#121212;--surface:#1a1a1a;--border:#2e2e2e;"
           @"--fg:#e6e6e6;--fg-strong:#cdcdcd;--dim:#8a8a8a;--faint:#6f6f6f;"
           @"--fg-rgb:230,230,230;--accent:#c273ef;"
           @"--accent-rgb:194,115,239;color-scheme:dark";
}

/* Colour roles shared by both internal documents. Everything reads from the
 * `:root` variables pushed from the terminal theme — no literal colour may
 * appear below this line, or the page stops following the theme. */
#define LIU_PAGE_BASE_CSS \
    @"html,body{height:100%;margin:0}" \
    @"body{background:var(--bg);color:var(--fg);" \
    @"font:400 14px/1.5 -apple-system,BlinkMacSystemFont,'SF Pro Text',system-ui,sans-serif;" \
    @"display:flex;align-items:center;justify-content:center;" \
    @"-webkit-user-select:none;user-select:none;-webkit-font-smoothing:antialiased}" \
    @"::selection{background:rgba(var(--accent-rgb),.30)}"

/* Connection-error document loaded into the WebView when a navigation fails.
 * Tokens (__HOSTPORT__ etc.) are substituted with HTML-escaped values. Kept as
 * one self-contained page (inline CSS+JS, no subresources) so it renders offline.
 * "Show Details" is a pure client-side toggle; "Ask Agent" posts to the native
 * liuBrowser message handler, which routes the error to the focused agent. */
static NSString *LiuBrowserErrorTemplate(void) {
    return
    @"<!doctype html><html><head><meta charset=\"utf-8\">"
    @"<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'; img-src data:; form-action 'none'; base-uri 'none'\">"
    @"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    @"<title>__HOSTPORT__</title><style>"
    @":root{__THEME__}"
    LIU_PAGE_BASE_CSS
    @".wrap{text-align:center;max-width:540px;padding:24px}"
    @"h1{margin:0 0 12px;font-size:30px;font-weight:600;letter-spacing:-.01em;"
    @"color:var(--fg-strong)}"
    @"p.sub{margin:0 0 28px;font-size:15px;color:var(--faint);"
    @"word-break:break-word}"
    @".btns{display:flex;gap:12px;justify-content:center;flex-wrap:wrap}"
    @"button{font:inherit;font-size:14px;color:var(--fg-strong);"
    @"background:transparent;"
    @"border:1px solid var(--border);border-radius:9px;padding:8px 18px;cursor:pointer;"
    @"transition:border-color .12s,color .12s,background .12s}"
    @"button:hover{border-color:var(--accent);color:var(--fg);"
    @"background:rgba(var(--accent-rgb),.10)}"
    @"button:disabled{opacity:.75;cursor:default;border-color:var(--accent);"
    @"color:var(--fg)}"
    @"#details{display:none;margin:24px auto 0;max-width:520px;text-align:left;"
    @"background:var(--surface);border:1px solid var(--border);border-radius:10px;"
    @"padding:14px 16px;font-family:var(--mono);font-size:12px;"
    @"line-height:1.6;color:var(--dim);white-space:pre-wrap;"
    @"word-break:break-word}"
    @"#details b{color:var(--fg-strong);font-weight:600}"
    @"#details .hint{color:var(--faint)}"
    @"@media (max-width:420px){h1{font-size:22px}p.sub{font-size:13px}}"
    @"</style></head><body><div class=\"wrap\">"
    @"<h1>__TITLE__</h1>"
    @"<p class=\"sub\">__HOSTPORT__ __REASON__. (__CODE__)</p>"
    @"<div class=\"btns\">"
    @"<button id=\"ask\">Ask Agent</button>"
    @"<button id=\"show\">Show Details</button></div>"
    @"<div id=\"details\"><b>URL</b>      __URL__\n<b>Error</b>    __REASON__ (__CODE__)\n"
    @"<b>Domain</b>   __DOMAIN__\n<b>Message</b>  __DESC__\n\n"
    @"<span class=\"hint\">__HINT__</span></div>"
    @"</div></body></html>";
}

/* Globe glyph used by the intro + Recents rows. */
#define LIU_GLOBE_SVG \
    @"<svg width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" " \
    @"stroke=\"currentColor\" stroke-width=\"1.6\"><circle cx=\"12\" cy=\"12\" r=\"9\"/>" \
    @"<path d=\"M3 12h18\"/><ellipse cx=\"12\" cy=\"12\" rx=\"4\" ry=\"9\"/></svg>"

/* Shared shell for the internal start pages (intro + Recents). __BODY__ is the
 * page-specific markup; a delegated click on any [data-url] row asks native to
 * navigate. Self-contained (no subresources) so it renders with no network. */
static NSString *LiuBrowserStartShell(void) {
    return
    @"<!doctype html><html><head><meta charset=\"utf-8\">"
    @"<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'; img-src data:; form-action 'none'; base-uri 'none'\">"
    @"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    @"<title>Liu Browser</title><style>"
    @":root{__THEME__}"
    LIU_PAGE_BASE_CSS
    @".card{width:70%;max-width:900px;box-sizing:border-box;"
    @"border:1px solid var(--border);border-radius:16px;padding:18px 22px;"
    @"background:var(--surface)}"
    @".card h2{margin:0 0 6px;font-size:15px;font-weight:500;color:var(--dim)}"
    @".row{display:flex;align-items:center;gap:12px;padding:10px 8px;border-radius:9px;"
    @"cursor:pointer;color:var(--fg);text-decoration:none;transition:background .12s}"
    @".row:hover{background:rgba(var(--accent-rgb),.10)}"
    @".row svg{flex:0 0 auto;color:var(--faint)}"
    @".row:hover svg{color:var(--accent)}"
    @".row span{font-size:17px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    @".intro{text-align:center;max-width:520px;padding:24px}"
    @".intro .globe{color:var(--accent);opacity:.9;margin-bottom:16px}"
    @".intro h1{margin:0 0 10px;font-size:26px;font-weight:600;color:var(--fg)}"
    @".intro p{margin:0 0 8px;font-size:14px;color:var(--dim);line-height:1.6}"
    @".intro p b{color:var(--fg-strong);font-weight:600}"
    @".intro .hint{margin-top:18px;font-size:13px;color:var(--faint)}"
    @".intro code{font-family:var(--mono);color:var(--accent);"
    @"background:rgba(var(--accent-rgb),.10);padding:1px 6px;border-radius:5px}"
    @"</style></head><body>__BODY__</body></html>";
}

/* First-run welcome screen shown when there are no recents. */
static NSString *LiuBrowserIntroBody(void) {
    return
    @"<div class=\"intro\"><div class=\"globe\">"
    @"<svg width=\"56\" height=\"56\" viewBox=\"0 0 24 24\" fill=\"none\" "
    @"stroke=\"currentColor\" stroke-width=\"1.3\"><circle cx=\"12\" cy=\"12\" r=\"9\"/>"
    @"<path d=\"M3 12h18\"/><ellipse cx=\"12\" cy=\"12\" rx=\"4\" ry=\"9\"/></svg></div>"
    @"<h1>Liu Browser</h1>"
    @"<p>Preview your local dev servers right inside Liu — no switching to a "
    @"separate app while you build.</p>"
    @"<p>Then use <b>Select Element</b> to hand any part of the page to your AI "
    @"agent as context for the change you want.</p>"
    @"<div class=\"hint\">Type a URL above, e.g. <code>localhost:5173</code>, to get started.</div>"
    @"</div>";
}

/* SPA/history changes have no full document load to publish their state. */
static void *kLiuTitleKVOCtx = &kLiuTitleKVOCtx;
static void *kLiuURLKVOCtx = &kLiuURLKVOCtx;
static void *kLiuBackKVOCtx = &kLiuBackKVOCtx;
static void *kLiuForwardKVOCtx = &kLiuForwardKVOCtx;

/* A named client world has its own globals, DOM wrappers and message handlers.
 * Page scripts can change the DOM, but cannot call or replace this bridge. */
static WKContentWorld *LiuBrowserWorld(void) {
    return [WKContentWorld worldWithName:@"LiuNativeBrowser"];
}

@implementation LiuBrowserHost

/* --- revision-bumping setters -------------------------------------------
 * Only the poll-visible state bumps; equality early-outs keep repeated
 * assignments of the same value (statusMsg = @"" etc.) from causing redraws.
 * MRC: string setters own the copy semantics by hand. */
#define LIU_BUMP_SCALAR_SETTER(Setter, ivar, Type)          \
    - (void)Setter:(Type)v {                                \
        if (ivar == v) return;                              \
        ivar = v;                                           \
        _revision++;                                        \
    }
LIU_BUMP_SCALAR_SETTER(setLoading,        _loading,        bool)
LIU_BUMP_SCALAR_SETTER(setPickMode,       _pickMode,       bool)
LIU_BUMP_SCALAR_SETTER(setHasPendingPick, _hasPendingPick, bool)
LIU_BUMP_SCALAR_SETTER(setHasPendingAsk,  _hasPendingAsk,  bool)
LIU_BUMP_SCALAR_SETTER(setHasPendingNav,  _hasPendingNav,  bool)
LIU_BUMP_SCALAR_SETTER(setCanBack, _canBack, bool)
LIU_BUMP_SCALAR_SETTER(setCanFwd,  _canFwd,  bool)
#undef LIU_BUMP_SCALAR_SETTER

#define LIU_BUMP_STRING_SETTER(Setter, ivar)                              \
    - (void)Setter:(NSString *)v {                                        \
        if (ivar == v || (v && ivar && [ivar isEqualToString:v])) return; \
        [ivar release];                                                   \
        ivar = [v copy];                                                  \
        _revision++;                                                      \
    }
LIU_BUMP_STRING_SETTER(setPageTitle, _pageTitle)
LIU_BUMP_STRING_SETTER(setPageURL,   _pageURL)
LIU_BUMP_STRING_SETTER(setStatusMsg, _statusMsg)
#undef LIU_BUMP_STRING_SETTER

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                        change:(NSDictionary *)change context:(void *)context {
    if (context == kLiuTitleKVOCtx) {
        NSString *t = self.webView.title;
        self.pageTitle = t ? t : @"";      /* setter dedups + bumps revision */
        return;
    }
    if (context == kLiuURLKVOCtx) {
        /* Internal pages show the empty/failed URL, not a synthetic document. */
        if (!self.startLoadPending && !self.showingStart &&
            !self.errorLoadPending && !self.showingError)
            self.pageURL = self.webView.URL.absoluteString ?: @"";
        return;
    }
    if (context == kLiuBackKVOCtx) {
        self.canBack = self.webView.canGoBack;
        return;
    }
    if (context == kLiuForwardKVOCtx) {
        self.canFwd = self.webView.canGoForward;
        return;
    }
    [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
}

- (instancetype)initWithParentView:(NSView *)parent {
    self = [super init];
    if (!self) return nil;

    WKUserContentController *uc = [[WKUserContentController alloc] init];
    NSString *src = [NSString stringWithUTF8String:BROWSER_PICK_ELEMENT_JS];
    if (src) {
        WKUserScript *script = [[WKUserScript alloc]
            initWithSource:src
            injectionTime:WKUserScriptInjectionTimeAtDocumentStart
            forMainFrameOnly:YES inContentWorld:LiuBrowserWorld()];
        [uc addUserScript:script];
        [script release];
    }
    /* Chrome is installed only after Liu's own internal navigation finishes. */
    WKUserScript *gesture = [[WKUserScript alloc]
        initWithSource:[NSString stringWithUTF8String:BROWSER_GESTURE_JS]
        injectionTime:WKUserScriptInjectionTimeAtDocumentStart
        forMainFrameOnly:NO inContentWorld:LiuBrowserWorld()];
    [uc addUserScript:gesture]; [gesture release];
    [uc addScriptMessageHandler:self contentWorld:LiuBrowserWorld() name:@"liuPick"];
    [uc addScriptMessageHandler:self contentWorld:LiuBrowserWorld() name:@"liuBrowser"];
    [uc addScriptMessageHandler:self contentWorld:LiuBrowserWorld() name:@"liuGesture"];

    WKWebViewConfiguration *cfg = [[WKWebViewConfiguration alloc] init];
    cfg.userContentController = uc;
    cfg.preferences.javaScriptCanOpenWindowsAutomatically = NO;
#ifdef BROWSER_SELFTEST
    cfg.websiteDataStore = liu_test_data_store ? liu_test_data_store : [WKWebsiteDataStore nonPersistentDataStore];
#endif

    LiuWKWebView *wv = [[LiuWKWebView alloc] initWithFrame:CGRectMake(0, 0, 1, 1)
                                             configuration:cfg];
    if (!wv) {
        [cfg release];
        [uc release];
        [self release];
        return nil;
    }
    wv.navigationDelegate = self;
    wv.UIDelegate = self;
    wv.wantsLayer = YES;
    wv.layer.backgroundColor = [[NSColor blackColor] CGColor];
    /* SPA title tracking (see kLiuTitleKVOCtx). Removed in teardown. */
    [wv addObserver:self forKeyPath:@"title" options:0 context:kLiuTitleKVOCtx];
    [wv addObserver:self forKeyPath:@"URL" options:0 context:kLiuURLKVOCtx];
    [wv addObserver:self forKeyPath:@"canGoBack" options:0 context:kLiuBackKVOCtx];
    [wv addObserver:self forKeyPath:@"canGoForward" options:0 context:kLiuForwardKVOCtx];

    NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
    box.wantsLayer = YES;
    box.layer.masksToBounds = YES;
    [box addSubview:wv];

    /* Visibility lives on the clip view, not the web view: a hidden view is
     * skipped by hit testing, and nothing may swallow clicks meant for the
     * Metal UI underneath while no browser is on screen. */
    LiuBrowserBox *clip = [[LiuBrowserBox alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
    clip.wantsLayer = YES;
    clip.layer.masksToBounds = YES;
    clip.hidden = YES;
    [clip addSubview:box];
    [parent addSubview:clip positioned:NSWindowAbove relativeTo:nil];
    self.webBox = box;
    self.clipBox = clip;
    [box release];
    [clip release];
    self.webView = wv;
    self.pageTitle = @"";
    self.pageURL = @"";
    self.statusMsg = @"";
    self.pickMode = false;
    self.hasPendingPick = false;
    self.pendingJson = NULL;
    self.errorLoadPending = false;
    self.showingError = false;
    self.hasPendingAsk = false;
    self.errURL = @"";
    self.errReason = @"";
    self.errDomain = @"";
    self.errDesc = @"";
    self.errCode = 0;
    self.errTitle = @"";
    self.errHint = @"";
    self.policySeq = 0;
    self.startLoadPending = false;
    self.showingStart = false;
    self.hasPendingNav = false;
    self.pendingNavURL = @"";
    self.pageThemeCSS = @"";
    self.pageMonoFamily = @"";
    self.lastRecents = nil;
    [wv release];
    [cfg release];
    [uc release];
    return self;
}

- (void)dealloc {
    [self teardown];
    self.pageTitle = nil;
    self.pageURL = nil;
    self.statusMsg = nil;
    self.errURL = nil;
    self.errReason = nil;
    self.errDomain = nil;
    self.errDesc = nil;
    self.errTitle = nil;
    self.errHint = nil;
    self.pendingNavURL = nil;
    self.pageThemeCSS = nil;
    self.pageMonoFamily = nil;
    self.lastRecents = nil;
    self.pickToken = nil;
    self.chromeToken = nil;
    self.internalNavigation = nil;
    self.gestureURL = nil; self.gestureFrameURL = nil;
    self.gestureToken = nil;
    [super dealloc];
}

- (void)teardown {
    ++self.policySeq;
    [self clearDocumentAuthority];
    [self cancelDNSRequests];
    if (self.webView) {
        @try { [self.webView removeObserver:self forKeyPath:@"title"
                                    context:kLiuTitleKVOCtx]; }
        @catch (NSException *e) { (void)e; /* already removed */ }
        @try { [self.webView removeObserver:self forKeyPath:@"URL"
                                    context:kLiuURLKVOCtx]; }
        @catch (NSException *e) { (void)e; }
        @try { [self.webView removeObserver:self forKeyPath:@"canGoBack"
                                    context:kLiuBackKVOCtx]; }
        @catch (NSException *e) { (void)e; }
        @try { [self.webView removeObserver:self forKeyPath:@"canGoForward"
                                    context:kLiuForwardKVOCtx]; }
        @catch (NSException *e) { (void)e; }
        [self.webView.configuration.userContentController
            removeScriptMessageHandlerForName:@"liuPick" contentWorld:LiuBrowserWorld()];
        [self.webView.configuration.userContentController
            removeScriptMessageHandlerForName:@"liuBrowser" contentWorld:LiuBrowserWorld()];
        [self.webView.configuration.userContentController
            removeScriptMessageHandlerForName:@"liuGesture" contentWorld:LiuBrowserWorld()];
        self.webView.navigationDelegate = nil;
        self.webView.UIDelegate = nil;
        [self.webView stopLoading];
        [self.webView removeFromSuperview];
        self.webView = nil;
    }
    if (self.webBox) {
        [self.webBox removeFromSuperview];
        self.webBox = nil;
    }
    if (self.clipBox) {
        [self.clipBox removeFromSuperview];
        self.clipBox = nil;
    }
    free(self.pendingJson);
    self.pendingJson = NULL;
}

- (void)clearDocumentAuthority {
    self.pickToken = nil;
    self.chromeToken = nil;
    self.gestureURL = nil;
    self.gestureFrameURL = nil;
    self.gestureToken = nil;
    self.gestureTime = 0;
    self.internalPolicyPending = false;
    self.hasPendingAsk = false;
    self.hasPendingNav = false;
    self.pendingNavURL = nil;
    free(self.pendingJson);
    self.pendingJson = NULL;
    self.hasPendingPick = false;
}

- (void)installInternalChrome {
    self.chromeToken = [NSUUID UUID].UUIDString;
    NSString *js = [NSString stringWithFormat:@"(%@)('%@');",
        [NSString stringWithUTF8String:BROWSER_CHROME_JS], self.chromeToken];
    [self.webView evaluateJavaScript:js inFrame:nil inContentWorld:LiuBrowserWorld()
                  completionHandler:nil];
}

/* NO when arming was refused: Liu's own start and error pages are not a page to
 * pick from — the picker's capture-phase click would take the Recents row or the
 * Ask Agent button and send Liu's own markup to the agent. Arming does not take
 * the keyboard either: a page that reloads while picking (a dev server's full
 * reload) re-arms here, and would take the keys from whatever pane was typing.
 */
- (BOOL)setPickModeEnabled:(BOOL)on {
    /* Refused, it is OFF — also when a pick session that was on reaches an
     * error page (the re-arm after a load), so the tab does not go on saying
     * "Select an element…" over a page that cannot be picked. */
    BOOL refused = on && (self.showingStart || self.showingError ||
                          self.startLoadPending || self.errorLoadPending);
    if (refused) on = NO;
    self.gestureURL = nil; self.gestureFrameURL = nil;
    self.gestureToken = nil;
    self.pickMode = on;
    self.pickToken = on ? [NSUUID UUID].UUIDString : nil;
    NSString *js = on ? [NSString stringWithFormat:
        @"window.__liuPickSetMode && window.__liuPickSetMode(true, '%@');", self.pickToken]
        : @"window.__liuPickSetMode && window.__liuPickSetMode(false);";
    [self.webView evaluateJavaScript:js inFrame:nil inContentWorld:LiuBrowserWorld()
                  completionHandler:nil];
    if (on) {
        self.statusMsg = @"Select an element…";
    } else if ([self.statusMsg isEqualToString:@"Select an element…"]) {
        self.statusMsg = @"";
    }
    return !refused;
}

#pragma mark - WKScriptMessageHandler

/* Defined below, with the schema it enforces. */
static NSData *liu_pick_sanitize(id body, NSString *sourceURL);

- (void)userContentController:(WKUserContentController *)userContentController
      didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)userContentController;
    if (!self.webView || message.webView != self.webView ||
        message.world != LiuBrowserWorld())
        return;
    if ([message.name isEqualToString:@"liuGesture"]) {
        if (self.pickMode) return;
        if (![message.body isKindOfClass:[NSDictionary class]]) return;
        id url = ((NSDictionary *)message.body)[@"url"];
        id token = ((NSDictionary *)message.body)[@"requestToken"];
        if (![token isKindOfClass:[NSString class]] || [(NSString *)token length] != 32 ||
            [(NSString *)token rangeOfCharacterFromSet:
                [[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location != NSNotFound) return;
        if (![url isKindOfClass:[NSString class]] || ![(NSString *)url length] ||
            [(NSString *)url length] >= BROWSER_URL_CAP ||
            (![(NSString *)url hasPrefix:@"http://"] && ![(NSString *)url hasPrefix:@"https://"])) return;
        if ([(NSString *)url lengthOfBytesUsingEncoding:NSUTF8StringEncoding] >= BROWSER_URL_CAP) return;
        NSString *frameURL = message.frameInfo.request.URL.absoluteString;
        if (!frameURL.length || frameURL.length >= BROWSER_URL_CAP) return;
        if (message.frameInfo.isMainFrame &&
            ![message.frameInfo.request.URL isEqual:self.webView.URL]) return;
        self.gestureURL = url;
        self.gestureToken = token;
        self.gestureFrameURL = frameURL;
        self.gestureMainFrame = message.frameInfo.isMainFrame;
        self.gestureTime = platform_time_sec();
        return;
    }
    if (!message.frameInfo.isMainFrame) return;

    /* Internal chrome actions ("Ask Agent", Recents nav) — handled regardless
     * of pick mode. */
    if ([message.name isEqualToString:@"liuBrowser"]) {
        if (!self.chromeToken.length ||
            ![self.webView.URL.absoluteString isEqualToString:@"about:blank"] ||
            ![message.frameInfo.request.URL.absoluteString isEqualToString:@"about:blank"])
            return;
        NSString *type = nil, *navURL = nil;
        if ([message.body isKindOfClass:[NSDictionary class]]) {
            id token = ((NSDictionary *)message.body)[@"requestToken"];
            if (![token isKindOfClass:[NSString class]] || ![token isEqual:self.chromeToken]) return;
            type = ((NSDictionary *)message.body)[@"type"];
            navURL = ((NSDictionary *)message.body)[@"url"];
        }
        if (![type isKindOfClass:[NSString class]]) return;
        if ([type isEqualToString:@"askAgent"] && self.showingError) {
            self.hasPendingAsk = true;
        } else if ([type isEqualToString:@"navigate"] && self.showingStart &&
                   [navURL isKindOfClass:[NSString class]] && navURL.length &&
                   navURL.length < BROWSER_URL_CAP &&
                   [navURL lengthOfBytesUsingEncoding:NSUTF8StringEncoding] < BROWSER_URL_CAP) {
            const char *candidate = navURL.UTF8String;
            if (candidate && browser_nav_is_allowed_with_list(candidate, self.allowlist)) {
                self.pendingNavURL = navURL;
                self.hasPendingNav = true;
            }
        }
        return;
    }

    if (!self.pickMode) return;
    if (![message.name isEqualToString:@"liuPick"]) return;
    if (![message.body isKindOfClass:[NSDictionary class]]) return;
    NSDictionary *body = message.body;
    id token = body[@"requestToken"], type = body[@"type"];
    if (![token isKindOfClass:[NSString class]] || !self.pickToken.length ||
        ![token isEqualToString:self.pickToken] || ![type isKindOfClass:[NSString class]]) return;
    if ([type isEqualToString:@"cancel"]) {
        [self setPickModeEnabled:NO];
        return;
    }
    if (![type isEqualToString:@"pick"]) return;

    f64 now = platform_time_sec();
    if (now - self.lastMsgTime < 0.1) {
        self.msgBurst++;
        if (self.msgBurst > 10) return;
    } else {
        self.msgBurst = 0;
    }
    self.lastMsgTime = now;

    /* Schema-driven, and the order matters.
     *
     * The body is an arbitrary JavaScript object graph chosen by the page, so
     * handing it straight to NSJSONSerialization and checking the 64 KiB
     * policy on the RESULT would pay for the expensive serialization of a
     * deeply nested or enormous structure before rejecting it.
     *
     * Liu needs a small, fixed set of fields. Extracting exactly those (each
     * clipped) and serializing the extraction makes the work bounded by
     * construction; the 64 KiB check stays as the final gate on the result. */
    NSData *data = liu_pick_sanitize(body, message.frameInfo.request.URL.absoluteString ?: @"");
    if (!data || data.length == 0 || data.length > 64 * 1024) return;

    free(self.pendingJson);
    self.pendingJson = malloc(data.length + 1);
    if (!self.pendingJson) return;
    memcpy(self.pendingJson, data.bytes, data.length);
    self.pendingJson[data.length] = '\0';
    self.hasPendingPick = true;
    self.pickMode = false;
    self.pickToken = nil;
    self.statusMsg = @"Element picked";
}

#pragma mark - WKNavigationDelegate

- (bool)consumeLinkGesture:(WKNavigationAction *)action {
    f64 age = platform_time_sec() - self.gestureTime;
    if (!self.webView || !self.gestureURL.length || !self.gestureToken.length || age < 0 || age > 1.0 ||
        self.gestureMainFrame != action.sourceFrame.isMainFrame ||
        ![self.gestureFrameURL isEqualToString:action.sourceFrame.request.URL.absoluteString] ||
        ![self.gestureURL isEqualToString:action.request.URL.absoluteString]) return false;
    self.gestureURL = nil; self.gestureFrameURL = nil;
    return true;
}

/* WKFrameInfo's public API has no stable frame/document identity. URL equality
 * alone lets a sibling iframe with the same URL reuse another frame's gesture.
 * Evaluate in the action's source frame: only that exact isolated document can
 * hold the unpredictable event token, and consume it there as well. */
- (void)authorizeLinkAction:(WKNavigationAction *)action completion:(void (^)(bool))completion {
    NSString *token = [[self.gestureToken copy] autorelease];
    f64 time = self.gestureTime;
    if (action.navigationType != WKNavigationTypeLinkActivated || ![self consumeLinkGesture:action]) {
        completion(false);
        return;
    }
    self.gestureToken = nil;
    u64 seq = self.policySeq;
    WKWebView *web = self.webView;
    NSString *js = [NSString stringWithFormat:
        @"(() => { if (window.__liuGestureToken !== '%@') return false; window.__liuGestureToken = ''; return true; })()", token];
    [web evaluateJavaScript:js inFrame:action.sourceFrame inContentWorld:LiuBrowserWorld()
         completionHandler:^(id result, NSError *error) {
        f64 age = platform_time_sec() - time;
        completion(!error && self.webView == web && seq == self.policySeq && age >= 0 && age <= 1.0 &&
                   [result isKindOfClass:[NSNumber class]] && [result boolValue]);
    }];
}

- (void)cancelDNSRequests {
    LiuBrowserDNSRequest *running = [self.dnsRunning retain];
    LiuBrowserDNSRequest *waiting = [self.dnsWaiting retain];
    self.dnsWaiting = nil;
    [running finish:WKNavigationActionPolicyCancel];
    [waiting finish:WKNavigationActionPolicyCancel];
    [running release]; [waiting release];
}

- (void)startDNSRequest:(LiuBrowserDNSRequest *)request {
    self.dnsRunning = request;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        BrowserNavClass verdict = LiuResolveBrowserHost(request.host.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!request.completed) {
                NSString *url = [request.url retain];
                [self applyNavAction:browser_nav_decide_with_list(url.UTF8String, verdict,
                                                        request.mainFrame, request.userClick, self.allowlist)
                             handler:^(WKNavigationActionPolicy p) { [request finish:p]; }
                                 url:url verdict:verdict mainFrame:request.mainFrame seq:request.seq];
                [url release];
            }
            self.dnsRunning = nil;
            LiuBrowserDNSRequest *next = [self.dnsWaiting retain];
            self.dnsWaiting = nil;
            if (next && !next.completed) {
                if (self.webView && next.seq == self.policySeq) [self startDNSRequest:next];
                else [next finish:WKNavigationActionPolicyCancel];
            }
            [next release];
        });
    });
}

- (void)webView:(WKWebView *)webView
decidePolicyForNavigationAction:(WKNavigationAction *)action
decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    if (webView != self.webView || !self.webView) {
        decisionHandler(WKNavigationActionPolicyCancel);
        return;
    }
    u64 seq = self.policySeq;
    [self authorizeLinkAction:action completion:^(bool userClick) {
        if (self.webView != webView || seq != self.policySeq) {
            decisionHandler(WKNavigationActionPolicyCancel);
            return;
        }
        [self decideNavigationAction:action userClick:userClick handler:decisionHandler];
    }];
}

/* An authenticated target=_blank click to an allowed site stays in this
 * preview. Loading through WebKit runs the same redirect policy again. */
- (void)openAuthorizedPopupURL:(NSURL *)url {
    const char *s = url.absoluteString.UTF8String;
    if (!s || strnlen(s, BROWSER_URL_CAP) >= BROWSER_URL_CAP ||
        (strncmp(s, "http://", 7) != 0 && strncmp(s, "https://", 8) != 0)) return;
    if (browser_nav_url_classify(s) == BROWSER_NAV_LOCAL ||
        browser_nav_allowlist_matches(self.allowlist, s))
        [self.webView loadRequest:[NSURLRequest requestWithURL:url]];
    else
        platform_open_url(s);
}

/* A navigation that only moves to a fragment of the document on screen: it
 * loads nothing, and WebKit says nothing when it is done (no didFinish). */
static bool liu_same_document(NSURL *from, NSURL *to) {
    if (!from || !to || !to.fragment) return false;
    NSString *a = from.absoluteString, *b = to.absoluteString;
    NSRange ha = [a rangeOfString:@"#"], hb = [b rangeOfString:@"#"];
    if (ha.location != NSNotFound) a = [a substringToIndex:ha.location];
    if (hb.location != NSNotFound) b = [b substringToIndex:hb.location];
    return [a isEqualToString:b];
}

- (void)decideNavigationAction:(WKNavigationAction *)action userClick:(bool)userClick
                      handler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    if (!self.webView) { decisionHandler(WKNavigationActionPolicyCancel); return; }
    NSURL *url = action.request.URL;
    bool mainFrame = action.targetFrame == nil || action.targetFrame.isMainFrame;
    /* Both WebKit delegates can receive a popup request. Consume the same
     * private gesture once, so either callback may handle it but never both. */
    if (action.targetFrame == nil) {
        u64 seq = self.policySeq;
        decisionHandler(WKNavigationActionPolicyCancel);
        if (self.webView && seq == self.policySeq && userClick &&
            ([url.scheme isEqualToString:@"http"] || [url.scheme isEqualToString:@"https"]))
            [self openAuthorizedPopupURL:url];
        return;
    }
    bool internal = self.internalPolicyPending && self.internalNavigation &&
        (self.errorLoadPending || self.startLoadPending) && mainFrame &&
        action.navigationType == WKNavigationTypeOther &&
        [url.absoluteString isEqualToString:@"about:blank"];
    #if __MAC_OS_X_VERSION_MAX_ALLOWED >= 270000
    if (@available(macOS 27.0, *))
        internal = internal && action.mainFrameNavigation == self.internalNavigation;
    #endif
    if (internal) {
        /* Older WebKit has no public navigation identity on the action:
         * consume the single blank load allowance; chrome still requires the
         * exact navigation's completion and a fresh private token. */
        self.internalPolicyPending = false;
        decisionHandler(WKNavigationActionPolicyAllow);
        return;
    }
    NSString *urlStr = url.absoluteString;
    u64 seq = mainFrame ? ++self.policySeq : self.policySeq;
    /* The document stays for a fragment move: its picker and chrome tokens
     * stay with it (cleared, an armed Select Element stopped delivering). */
    if (mainFrame && !(action.navigationType != WKNavigationTypeReload &&
                       liu_same_document(self.webView.URL, url))) {
        [self clearDocumentAuthority];
        [self cancelDNSRequests];
    } else if (mainFrame) {
        [self cancelDNSRequests];
    } else {
        /* A subframe load also expires any gesture from its old document. */
        self.gestureURL = nil; self.gestureFrameURL = nil;
        self.gestureToken = nil;
    }
    if (!self.webView || seq != self.policySeq) {
        decisionHandler(WKNavigationActionPolicyCancel);
        return;
    }
    if (mainFrame && (!urlStr.length || urlStr.length >= BROWSER_URL_CAP ||
                      [urlStr lengthOfBytesUsingEncoding:NSUTF8StringEncoding] >= BROWSER_URL_CAP)) {
        decisionHandler(WKNavigationActionPolicyCancel);
        return;
    }
    const char *s = urlStr.UTF8String;

    BrowserNavClass cls = browser_nav_url_classify(s);
    BrowserNavAction act = browser_nav_decide_with_list(s, cls, mainFrame, userClick, self.allowlist);
    if (act == BROWSER_NAV_RESOLVE) {
        /* A host is local only when every resolved address is local,
         * including split-DNS and mDNS names. getaddrinfo can block, so
         * resolve off the main thread and return the decision here. The
         * completion block retains self. */
        char host[256];
        if (!browser_nav_host(s, host, sizeof host)) {
            decisionHandler(WKNavigationActionPolicyCancel);
            return;
        }
        /* Clicked subframes share the same bounded resolver admission. */
        [self cancelDNSRequests];
        if (!self.webView || seq != self.policySeq) {
            decisionHandler(WKNavigationActionPolicyCancel);
            return;
        }
        LiuBrowserDNSRequest *request = [[LiuBrowserDNSRequest alloc] init];
        request.host = [NSString stringWithUTF8String:host];
        request.url = urlStr; request.seq = seq;
        request.mainFrame = mainFrame; request.userClick = userClick;
        request.handler = decisionHandler;
        if (self.dnsRunning) {
            [self.dnsWaiting finish:WKNavigationActionPolicyCancel];
            self.dnsWaiting = request;
        } else [self startDNSRequest:request];
        [request release];
        return;
    }
    [self applyNavAction:act handler:decisionHandler url:urlStr verdict:cls
               mainFrame:mainFrame seq:seq];
}

- (void)applyNavAction:(BrowserNavAction)act
               handler:(void (^)(WKNavigationActionPolicy))decisionHandler
                   url:(NSString *)urlStr verdict:(BrowserNavClass)verdict
             mainFrame:(bool)mainFrame seq:(u64)seq {
    if (!self.webView || seq != self.policySeq) {
        decisionHandler(WKNavigationActionPolicyCancel);
        return;
    }
    if (act == BROWSER_NAV_ALLOW) {
        decisionHandler(WKNavigationActionPolicyAllow);
        return;
    }
    /* WebKit does not always issue a failure callback for a refused custom
     * scheme redirect. End the app's loading state before the cancellation
     * callback, which may synchronously start a new navigation. */
    if (mainFrame) {
        self.loading = false;
        self.statusMsg = act == BROWSER_NAV_HANDOFF ? @"" : @"Navigation blocked";
    }
    decisionHandler(WKNavigationActionPolicyCancel);
    if (!self.webView || seq != self.policySeq) return;
    if (act == BROWSER_NAV_HANDOFF) {
        platform_open_url(urlStr.UTF8String);
        return;
    }
    [self refuseURL:urlStr verdict:verdict mainFrame:mainFrame seq:seq];
}

/* User-activated public HTTP(S) links are handed to the system browser by
 * browser_nav_decide; this method displays only navigations refused by the
 * local-only policy. */
- (void)refuseURL:(NSString *)urlStr verdict:(BrowserNavClass)verdict
        mainFrame:(bool)mainFrame seq:(u64)seq {
    if (!self.webView) return;                    /* torn down meanwhile */
    const char *s = urlStr.UTF8String;
    bool web = s && (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0);
    if (!mainFrame || !web || seq != self.policySeq) return;
    self.loading = false;
    self.errURL = urlStr;
    self.errCode = -20;                           /* Chromium ERR_BLOCKED_BY_CLIENT */
    self.errDomain = @"LiuBrowserPolicy";
    if (verdict == BROWSER_NAV_PUBLIC) {
        self.errTitle = @"Site not allowed";
        self.errReason = @"is on the public internet";
        self.errDesc = @"Add this site in Settings > Browser > Allowed sites to open it in Liu.";
    } else {
        self.errTitle = @"Can't reach local address";
        self.errReason = @"did not resolve to a local address";
        self.errDesc = @"The name could not be resolved.";
    }
    self.errHint = @"Localhost, private networks, and Tailscale work automatically. "
                   @"Other sites need an entry in Settings > Browser > Allowed sites.";
    [self renderErrorPage];
}

- (void)webView:(WKWebView *)webView
didReceiveAuthenticationChallenge:(NSURLAuthenticationChallenge *)challenge
completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential *))completionHandler {
    /* The loopback trust exception below belongs to THIS tab's live view and
     * to nothing else. Like every other delegate method here, this one opens
     * with the test, so a challenge delivered for a view the tab does not
     * own, or after the tab was closed, is never answered "trust it" for a
     * local name. */
    if (!self.webView || webView != self.webView) {
        completionHandler(NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);
        return;
    }
    NSString *host = challenge.protectionSpace.host;
    const char *h = host.UTF8String;
    bool loop = browser_nav_host_is_loopback(h ? h : "");
    if (loop &&
        [challenge.protectionSpace.authenticationMethod
            isEqualToString:NSURLAuthenticationMethodServerTrust] &&
        challenge.protectionSpace.serverTrust) {
        NSURLCredential *cred =
            [NSURLCredential credentialForTrust:challenge.protectionSpace.serverTrust];
        completionHandler(NSURLSessionAuthChallengeUseCredential, cred);
        return;
    }
    /* Non-loopback hosts (a LAN box, a tailnet name) reach here only after
     * the local-only policy vetted them. Let the system validate the
     * certificate the normal way — cancelling the server-trust challenge
     * would reject every real HTTPS site with NSURLErrorCancelled, and the
     * page would never load (blank white view). */
    completionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
}

/* What the server asked to be SAVED is not rendered. WebKit's default shows
 * any response whose type it can display, `Content-Disposition: attachment`
 * or not — and a host that serves user content as attachments does so exactly
 * so that it will NOT run as a page of its origin. Liu's browser downloads
 * nothing, so such a response (and one WebKit cannot show at all) is simply
 * refused, and the tab says why. */
- (void)webView:(WKWebView *)webView
decidePolicyForNavigationResponse:(WKNavigationResponse *)navigationResponse
decisionHandler:(void (^)(WKNavigationResponsePolicy))decisionHandler {
    if (!self.webView || webView != self.webView) {
        decisionHandler(WKNavigationResponsePolicyCancel);
        return;
    }
    bool attachment = false;
    if ([navigationResponse.response isKindOfClass:[NSHTTPURLResponse class]]) {
        NSString *disposition = [(NSHTTPURLResponse *)navigationResponse.response
                                    valueForHTTPHeaderField:@"Content-Disposition"];
        NSString *kind = [[disposition stringByTrimmingCharactersInSet:
                              [NSCharacterSet whitespaceCharacterSet]] lowercaseString];
        attachment = kind && [kind hasPrefix:@"attachment"];
    }
    if (attachment || !navigationResponse.canShowMIMEType) {
        if (navigationResponse.forMainFrame)
            self.statusMsg = @"Not shown: the server sent this as a download";
        decisionHandler(WKNavigationResponsePolicyCancel);
        return;
    }
    decisionHandler(WKNavigationResponsePolicyAllow);
}

- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)nav {
    if (!self.webView || webView != self.webView) return;
    if (nav == self.internalNavigation)
        return;   /* an internal chrome page is loading */
    [self clearDocumentAuthority];
    self.internalNavigation = nil;
    self.errorLoadPending = false;
    self.startLoadPending = false;
    self.loading = true;
    self.showingError = false;
    self.showingStart = false;
    self.hasPendingAsk = false;
    self.hasPendingNav = false;
    self.statusMsg = @"Loading…";
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)nav {
    if (!self.webView || webView != self.webView) return;
    self.loading = false;
    self.canBack = webView.canGoBack;
    self.canFwd  = webView.canGoForward;
    if (nav == self.internalNavigation && self.errorLoadPending) {
        /* Connection-error page finished — keep the failed URL in the bar so the
         * user sees what they tried, and mark the page as an error surface. */
        self.errorLoadPending = false;
        self.showingError = true;
        [self installInternalChrome];
        if (self.errURL.length) self.pageURL = self.errURL;
        self.statusMsg = @"";
        if (self.pickMode) [self setPickModeEnabled:YES];   /* refused: turns it off */
        return;
    }
    if (nav == self.internalNavigation && self.startLoadPending) {
        /* Intro / Recents start page finished — no real URL, so leave the bar
         * empty and ready for input. */
        self.startLoadPending = false;
        self.showingStart = true;
        [self installInternalChrome];
        self.pageURL = @"";
        self.pageTitle = @"";
        self.statusMsg = @"";
        if (self.pickMode) [self setPickModeEnabled:YES];   /* refused: turns it off */
        return;
    }
    self.showingError = false;
    self.showingStart = false;
    self.pageTitle = webView.title ? webView.title : @"";
    self.pageURL = webView.URL.absoluteString
        ? webView.URL.absoluteString : @"";
    if (!self.pickMode) self.statusMsg = @"";
    /* Re-apply pick mode after SPA full navigation. */
    if (self.pickMode)
        [self setPickModeEnabled:YES];
}

- (void)handleLoadFailure:(WKWebView *)webView error:(NSError *)error {
    if (!self.webView || webView != self.webView) return;
    self.loading = false;
    /* User-cancelled or policy-interrupted loads (e.g. a redirect handed off to
     * the system browser) are not real failures — never show the error page. */
    if ([error.domain isEqualToString:NSURLErrorDomain] &&
        error.code == NSURLErrorCancelled)
        return;
    if ([error.domain isEqualToString:@"WebKitErrorDomain"] && error.code == 102)
        return;
    if (self.errorLoadPending) return;   /* guard against error-page recursion */

    NSString *failed = error.userInfo[NSURLErrorFailingURLStringErrorKey];
    if (!failed) {
        NSURL *u = error.userInfo[NSURLErrorFailingURLErrorKey];
        failed = u.absoluteString;
    }
    if (!failed) failed = self.pageURL;
    [self showConnectionErrorForURL:failed error:error];
}

- (void)webView:(WKWebView *)webView
didFailProvisionalNavigation:(WKNavigation *)nav
      withError:(NSError *)error {
    (void)nav;
    [self handleLoadFailure:webView error:error];
}

- (void)webView:(WKWebView *)webView
didFailNavigation:(WKNavigation *)nav
      withError:(NSError *)error {
    (void)nav;
    [self handleLoadFailure:webView error:error];
}

#pragma mark - Connection error page

- (NSString *)htmlEscape:(NSString *)s {
    if (!s) return @"";
    NSMutableString *m = [s mutableCopy];
    [m replaceOccurrencesOfString:@"&" withString:@"&amp;"  options:0 range:NSMakeRange(0, m.length)];
    [m replaceOccurrencesOfString:@"<" withString:@"&lt;"   options:0 range:NSMakeRange(0, m.length)];
    [m replaceOccurrencesOfString:@">" withString:@"&gt;"   options:0 range:NSMakeRange(0, m.length)];
    [m replaceOccurrencesOfString:@"\"" withString:@"&quot;" options:0 range:NSMakeRange(0, m.length)];
    return [m autorelease];
}

/* Map an NSError to a human phrase + a Chromium-style net error code that web
 * devs recognise (a bare localhost dev server that isn't up → ERR_CONNECTION_
 * REFUSED / -102, matching the design). Unknown errors fall back to the raw
 * NSError message + code. */
- (void)mapError:(NSError *)e reason:(NSString **)reason code:(i32 *)code {
    NSInteger c = e.code;
    if ([e.domain isEqualToString:NSURLErrorDomain]) {
        switch (c) {
        case NSURLErrorCannotConnectToHost:    *reason = @"refused to connect";                *code = -102; return;
        case NSURLErrorTimedOut:               *reason = @"took too long to respond";          *code = -7;   return;
        case NSURLErrorCannotFindHost:         *reason = @"server address could not be found"; *code = -105; return;
        case NSURLErrorNotConnectedToInternet: *reason = @"there is no internet connection";   *code = -106; return;
        case NSURLErrorNetworkConnectionLost:  *reason = @"the connection was reset";          *code = -101; return;
        case NSURLErrorSecureConnectionFailed: *reason = @"the secure connection failed";      *code = -113; return;
        default: break;
        }
    }
    *reason = e.localizedDescription.length ? e.localizedDescription : @"couldn't be loaded";
    if (c > INT32_MAX) *code = INT32_MAX;
    else if (c < INT32_MIN) *code = INT32_MIN;
    else *code = (i32)c;
}

/* Build + load the error document from the already-captured err* fields. Split
 * from showConnectionErrorForURL: so a theme push can re-render the page it is
 * currently showing without an NSError to hand. */
- (void)renderErrorPage {
    /* host:port for the headline. */
    NSString *hostport = self.errURL.length ? self.errURL : @"";
    NSURLComponents *comp = self.errURL.length
        ? [NSURLComponents componentsWithString:self.errURL] : nil;
    if (comp.host.length)
        hostport = comp.port ? [NSString stringWithFormat:@"%@:%@", comp.host, comp.port]
                             : comp.host;

    NSString *codeStr = [@(self.errCode) stringValue];
    NSString *html = LiuBrowserErrorTemplate();
    html = [html stringByReplacingOccurrencesOfString:@"__TITLE__"
             withString:[self htmlEscape:self.errTitle.length ? self.errTitle
                                                              : @"Can't connect to server"]];
    html = [html stringByReplacingOccurrencesOfString:@"__HINT__"
             withString:[self htmlEscape:self.errHint.length ? self.errHint
                 : @"Tip: make sure your dev server is running (e.g. npm run dev), "
                   @"then reload this page."]];
    html = [html stringByReplacingOccurrencesOfString:@"__HOSTPORT__" withString:[self htmlEscape:hostport]];
    html = [html stringByReplacingOccurrencesOfString:@"__REASON__"   withString:[self htmlEscape:self.errReason]];
    html = [html stringByReplacingOccurrencesOfString:@"__CODE__"     withString:codeStr];
    html = [html stringByReplacingOccurrencesOfString:@"__URL__"      withString:[self htmlEscape:self.errURL]];
    html = [html stringByReplacingOccurrencesOfString:@"__DOMAIN__"   withString:[self htmlEscape:self.errDomain]];
    html = [html stringByReplacingOccurrencesOfString:@"__DESC__"     withString:[self htmlEscape:self.errDesc]];
    html = [self applyPageTheme:html];

    self.showingError = false;
    self.showingStart = false;
    self.startLoadPending = false;
    self.errorLoadPending = true;
    self.hasPendingAsk = false;
    self.hasPendingNav = false;
    [self clearDocumentAuthority];
    ++self.policySeq;
    [self cancelDNSRequests];
    self.internalPolicyPending = true;
    self.internalNavigation = [self.webView loadHTMLString:html baseURL:nil];
    self.statusMsg = [NSString stringWithFormat:@"%@ %@", hostport, self.errReason];
}

- (void)showConnectionErrorForURL:(NSString *)urlStr error:(NSError *)error {
    NSString *reason = nil; i32 code = 0;
    [self mapError:error reason:&reason code:&code];

    self.errURL = urlStr.length ? urlStr : @"";
    self.errReason = reason ? reason : @"";
    self.errCode = code;
    self.errDomain = error.domain ? error.domain : @"";
    self.errDesc = error.localizedDescription ? error.localizedDescription : @"";
    self.errTitle = @"";
    self.errHint = @"";

    [self renderErrorPage];
}

/* Compact URL label for a Recents row: drop the scheme + trailing slash. */
- (NSString *)displayURL:(NSString *)u {
    NSString *s = u ? u : @"";
    if ([s hasPrefix:@"https://"]) s = [s substringFromIndex:8];
    else if ([s hasPrefix:@"http://"]) s = [s substringFromIndex:7];
    if (s.length > 1 && [s hasSuffix:@"/"]) s = [s substringToIndex:s.length - 1];
    return s;
}

/* Substitute the theme tokens into an internal document. Splitting this out
 * keeps the error page and the start page on one definition of "themed". */
- (NSString *)applyPageTheme:(NSString *)html {
    NSString *css = self.pageThemeCSS.length ? self.pageThemeCSS
                                             : LiuBrowserFallbackThemeCSS();
    NSString *family = self.pageMonoFamily ?: @"";
    /* Font metadata is not HTML or CSS source. Keep it inside its CSS string. */
    family = [family stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];
    family = [family stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
    family = [family stringByReplacingOccurrencesOfString:@"<" withString:@"\\3c "];
    family = [family stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
    family = [family stringByReplacingOccurrencesOfString:@"\r" withString:@" "];
    NSString *mono = family.length
        ? [NSString stringWithFormat:@"\"%@\",ui-monospace,SFMono-Regular,Menlo,"
                                     @"Consolas,monospace", family]
        : @"ui-monospace,SFMono-Regular,Menlo,Consolas,monospace";
    css = [css stringByAppendingFormat:@";--mono:%@", mono];
    return [html stringByReplacingOccurrencesOfString:@"__THEME__" withString:css];
}

/* "#rrggbb" out of a `--<name>:` declaration, as an NSColor. Returns nil when
 * the variable is absent or malformed — callers keep their previous colour. */
- (NSColor *)colorFromThemeCSS:(NSString *)css var:(NSString *)name {
    if (!css.length) return nil;
    NSRange k = [css rangeOfString:[NSString stringWithFormat:@"--%@:#", name]];
    if (k.location == NSNotFound) return nil;
    NSUInteger start = k.location + k.length;
    if (start + 6 > css.length) return nil;
    NSString *hex = [css substringWithRange:NSMakeRange(start, 6)];
    unsigned int v = 0;
    if (![[NSScanner scannerWithString:hex] scanHexInt:&v]) return nil;
    return [NSColor colorWithSRGBRed:((v >> 16) & 0xFF) / 255.0
                               green:((v >> 8) & 0xFF) / 255.0
                                blue:(v & 0xFF) / 255.0
                               alpha:1.0];
}

- (void)setPageThemeCSS:(NSString *)css monoFamily:(NSString *)mono {
    self.pageThemeCSS = css;
    self.pageMonoFamily = mono;

    /* The WebView's own backdrop: what shows through an overscroll rubber band
     * and during the blank moment between navigations. Left at the default it
     * flashed black against every theme. */
    NSColor *bg = [self colorFromThemeCSS:css var:@"bg"];
    if (bg && self.webView) {
        self.webView.layer.backgroundColor = [bg CGColor];
        if ([self.webView respondsToSelector:@selector(setUnderPageBackgroundColor:)])
            self.webView.underPageBackgroundColor = bg;
    }

    /* Recolour an internal page that is already on screen. A live site is left
     * alone — Liu themes its own documents, never the user's. */
    if (self.showingStart || self.startLoadPending) {
        [self showStartPageWithRecents:self.lastRecents];
    } else if (self.showingError || self.errorLoadPending) {
        [self renderErrorPage];
    }
}

- (void)showStartPageWithRecents:(NSArray *)recents {
    self.lastRecents = recents;
    NSString *body;
    NSMutableString *rows = [NSMutableString string];
    for (id item in recents) {
        if (![item isKindOfClass:[NSString class]]) continue;
        NSString *u = (NSString *)item;
        if (!u.length) continue;
        [rows appendFormat:@"<a class=\"row\" data-url=\"%@\">%@<span>%@</span></a>",
            [self htmlEscape:u], LIU_GLOBE_SVG, [self htmlEscape:[self displayURL:u]]];
    }
    if (rows.length == 0)
        body = LiuBrowserIntroBody();   /* no history → welcome screen */
    else
        body = [NSString stringWithFormat:@"<div class=\"card\"><h2>Recents</h2>%@</div>", rows];

    NSString *html = LiuBrowserStartShell();
    html = [html stringByReplacingOccurrencesOfString:@"__BODY__" withString:body];
    html = [self applyPageTheme:html];

    self.startLoadPending = true;
    self.errorLoadPending = false;
    self.showingStart = false;
    self.showingError = false;
    self.hasPendingAsk = false;
    self.hasPendingNav = false;
    [self clearDocumentAuthority];
    ++self.policySeq;
    [self cancelDNSRequests];
    self.internalPolicyPending = true;
    self.internalNavigation = [self.webView loadHTMLString:html baseURL:nil];
    self.statusMsg = @"";
    self.pageURL = @"";
    self.pageTitle = @"";
}

#pragma mark - WKUIDelegate

/* This preview has no device-permission UI. Omitting the optional delegate
 * means Prompt, not Deny: explicitly fail closed, including stale/foreign
 * views and capture types introduced by a newer WebKit. No origin or frame
 * can authorize capture, and the decision must always be completed. */
- (void)webView:(WKWebView *)webView
requestMediaCapturePermissionForOrigin:(WKSecurityOrigin *)origin
initiatedByFrame:(WKFrameInfo *)frame
          type:(WKMediaCaptureType)type
decisionHandler:(void (^)(WKPermissionDecision))decisionHandler API_AVAILABLE(macos(12.0)) {
    (void)webView; (void)origin; (void)frame; (void)type;
    decisionHandler(WKPermissionDecisionDeny);
}

- (WKWebView *)webView:(WKWebView *)webView
createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
   forNavigationAction:(WKNavigationAction *)action
        windowFeatures:(WKWindowFeatures *)windowFeatures {
    (void)webView; (void)configuration; (void)windowFeatures;
    if (!self.webView || webView != self.webView) return nil;
    /* target=_blank / window.open: the same rule as the navigation
     * delegate — only a real click, and only http(s), may leave for the
     * system browser. A script-driven window.open() of any other scheme
     * never reaches the OS opener. */
    NSURL *url = action.request.URL;
    const char *s = url ? url.absoluteString.UTF8String : NULL;
    if (s && s[0] && (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0)) {
        u64 seq = self.policySeq;
        [self authorizeLinkAction:action completion:^(bool authorized) {
            if (authorized && self.webView && self.policySeq == seq)
                [self openAuthorizedPopupURL:url];
        }];
    }
    return nil;
}

@end

/* ---- JSON helpers (minimal, no cJSON dependency in ObjC TU) ---- */

/* ---------------------------------------------------------------------------
 * The liuPick message schema
 *
 * Every field parse_pick_json() reads, and nothing else. `attrs` is posted by
 * pick_element.js and never consumed, so it is dropped here rather than
 * serialized and thrown away.
 *
 * Each cap is the destination C buffer's byte budget, excluding its NUL.
 * Bound UTF-16 work first, then UTF-8 output; never expand to a full grapheme,
 * because combining marks can turn one grapheme into megabytes of input.
 * ------------------------------------------------------------------------- */
/* Both the work budget and output budget are hard limits. In particular,
 * UTF-8 C buffers must not receive half a code point or an embedded NUL. */
static NSString *liu_pick_clip(id v, NSUInteger cap) {
    if (![v isKindOfClass:[NSString class]] || !cap) return nil;
    NSString *s = (NSString *)v;
    NSUInteger units = MIN(s.length, cap);
    if (!units) return nil;
    if (units < s.length && CFStringIsSurrogateHighCharacter([s characterAtIndex:units - 1]) &&
        CFStringIsSurrogateLowCharacter([s characterAtIndex:units])) units--;
    s = [s substringToIndex:units];
    s = [s stringByReplacingOccurrencesOfString:[NSString stringWithFormat:@"%C", (unichar)0]
                                   withString:@"\\x00"];
    NSData *utf8 = [s dataUsingEncoding:NSUTF8StringEncoding];
    if (!utf8.length) return nil;
    NSUInteger bytes = MIN(utf8.length, cap);
    const u8 *data = utf8.bytes;
    if (bytes < utf8.length)
        while (bytes && (data[bytes] & 0xc0) == 0x80) bytes--;
    return [[[NSString alloc] initWithBytes:data length:bytes encoding:NSUTF8StringEncoding] autorelease];
}

static NSData *liu_pick_sanitize(id body, NSString *sourceURL) {
    /* A bare string body is the cancel path; bound it before converting. */
    if ([body isKindOfClass:[NSString class]]) {
        NSString *s = liu_pick_clip(body, 64 * 1024);
        return s ? [s dataUsingEncoding:NSUTF8StringEncoding] : nil;
    }
    if (![body isKindOfClass:[NSDictionary class]]) return nil;
    NSDictionary *in = (NSDictionary *)body;

    static const struct { __unsafe_unretained NSString *key; NSUInteger cap; } fields[] = {
        { @"type",        32 },
        { @"url",         BROWSER_URL_CAP - 1 },
        { @"tag",         63 },
        { @"id",          127 },
        { @"classes",     255 },
        { @"cssSelector", BROWSER_SEL_CAP - 1 },
        { @"cssPath",     BROWSER_PATH_CAP - 1 },
        { @"text",        BROWSER_TEXT_CAP - 1 },
        { @"ariaLabel",   255 },
        { @"role",        63 },
        { @"dataTestid",  127 },
        { @"html",        BROWSER_HTML_CAP - 1 },
    };

    NSMutableDictionary *out = [NSMutableDictionary dictionaryWithCapacity:16];
    for (usize i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        id value = sourceURL && [fields[i].key isEqualToString:@"url"] ? sourceURL : in[fields[i].key];
        NSString *v = liu_pick_clip(value, fields[i].cap);
        if (v) out[fields[i].key] = v;
    }
    id html = in[@"html"];
    if ([html isKindOfClass:[NSString class]] &&
        [out[@"html"] length] < [(NSString *)html length])
        out[@"htmlTruncated"] = @YES;

    /* rect: four numbers, read individually so a nested object of any other
     * shape contributes nothing. */
    id rect = in[@"rect"];
    if ([rect isKindOfClass:[NSDictionary class]]) {
        NSMutableDictionary *r = [NSMutableDictionary dictionaryWithCapacity:4];
        static const char *const rk[] = { "x", "y", "w", "h" };
        for (usize i = 0; i < 4; i++) {
            NSString *k = @(rk[i]);
            id n = ((NSDictionary *)rect)[k];
            if ([n isKindOfClass:[NSNumber class]]) {
                double d = [(NSNumber *)n doubleValue];
                /* NaN/inf cannot be serialized as JSON and would fail the
                 * whole message, losing a pick over one bad coordinate. */
                if (isfinite(d)) r[k] = @(d);
            }
        }
        if (r.count) out[@"rect"] = r;
    }

    /* limitations: a short list of short strings. */
    id lim = in[@"limitations"];
    if ([lim isKindOfClass:[NSArray class]]) {
        NSMutableArray *a = [NSMutableArray arrayWithCapacity:8];
        NSUInteger limit = [(NSArray *)lim count];
        if (limit > 8) limit = 8;
        for (NSUInteger i = 0; i < limit; i++) {
            id e = ((NSArray *)lim)[i];
            NSString *v = liu_pick_clip(e, 128);
            if (v) [a addObject:v];
        }
        if (a.count) out[@"limitations"] = a;
    }

    if (out.count == 0) return nil;
    if (![NSJSONSerialization isValidJSONObject:out]) return nil;
    return [NSJSONSerialization dataWithJSONObject:out options:0 error:nil];
}

static void liu_pick_copy(id value, char *out, usize cap) {
    NSString *s = liu_pick_clip(value, cap - 1);
    const char *utf8 = s.UTF8String;
    if (utf8) memcpy(out, utf8, strlen(utf8) + 1);
}

static void parse_pick_json(const char *json, BrowserPick *out) {
    memset(out, 0, sizeof *out);
    if (!json) return;
    usize len = strnlen(json, 64u * 1024u + 1u);
    if (len > 64u * 1024u) return;
    NSData *data = [NSData dataWithBytes:json length:len];
    id value = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![value isKindOfClass:[NSDictionary class]]) return;
    NSDictionary *body = value;
    if (![body[@"type"] isEqual:@"pick"]) return;
    #define PICK_COPY(key, field) liu_pick_copy(body[@key], out->field, sizeof out->field)
    PICK_COPY("url", page_url);
    PICK_COPY("tag", tag);
    PICK_COPY("id", id);
    PICK_COPY("classes", classes);
    PICK_COPY("cssSelector", selector);
    PICK_COPY("cssPath", css_path);
    PICK_COPY("text", text);
    PICK_COPY("ariaLabel", aria_label);
    PICK_COPY("role", role);
    PICK_COPY("dataTestid", data_testid);
    PICK_COPY("html", html);
    #undef PICK_COPY
    out->html_len = (i32)strlen(out->html);
    out->html_truncated = [body[@"htmlTruncated"] isEqual:@YES] ||
                          strstr(out->html, "<!-- truncated -->") != NULL;
    id rect = body[@"rect"];
    if ([rect isKindOfClass:[NSDictionary class]]) {
        NSString *keys[] = { @"x", @"y", @"w", @"h" };
        f32 *coords[] = { &out->x, &out->y, &out->w, &out->h };
        for (usize i = 0; i < 4; i++) {
            id n = rect[keys[i]];
            if (![n isKindOfClass:[NSNumber class]]) continue;
            f64 d = [n doubleValue];
            if (isfinite(d)) *coords[i] = (f32)fmax(-10000000.0, fmin(10000000.0, d));
        }
    }
    id limitations = body[@"limitations"];
    if ([limitations isKindOfClass:[NSArray class]]) {
        NSMutableArray *strings = [NSMutableArray arrayWithCapacity:8];
        for (NSUInteger i = 0; i < MIN((NSUInteger)8, [limitations count]); i++) {
            NSString *s = liu_pick_clip(limitations[i], sizeof out->limitations - 1);
            if (s) [strings addObject:s];
        }
        liu_pick_copy([strings componentsJoinedByString:@", "], out->limitations, sizeof out->limitations);
    }
}

/* Export the pick schema sanitizer only when the conditional support option
 * is enabled.
 */
#ifdef BROWSER_SELFTEST
NSData *browser_selftest_pick_sanitize(id body);
NSData *browser_selftest_pick_sanitize(id body) { return liu_pick_sanitize(body, nil); }
void browser_selftest_pick_parse(const char *json, BrowserPick *out);
void browser_selftest_pick_parse(const char *json, BrowserPick *out) { parse_pick_json(json, out); }
#endif

struct BrowserNative {
    LiuBrowserHost *host;
    u64    polled_rev;    /* host.revision at the last poll that copied out */
    i32    last_visible;  /* -1 unset / 0 hidden / 1 visible — skips repeat setHidden */
    bool   have_frame;    /* last_frame valid — skips repeat setFrame */
    NSRect last_frame;    /* page box, window points (unclipped) */
    NSRect clip;          /* region the page may show through, window points */
    bool   have_clip;     /* false = unclipped (clip covers everything) */
    i32    vp_w, vp_h;    /* active device viewport in CSS px; 0 = fill */
    bool   cmd_passthrough; /* mirrors clipBox.cmdPassthrough — skips repeat sets */
};

/* Push last_frame + clip into AppKit. The clip view takes the *visible* rect
 * and the page box keeps its full frame inside it, offset so the page does not
 * move on screen when the clip changes — that is what makes clipping read as
 * "the Metal panel covers the browser" rather than "the page got resized".
 * Callers must have validated last_frame; must run on the main thread. */
static void browser_apply_geometry(BrowserNative *n) {
    NSRect f = n->last_frame;
    NSRect c = n->have_clip ? NSIntersectionRect(f, n->clip) : f;
    @autoreleasepool {
        if (NSIsEmptyRect(c)) {
            /* Fully occluded: nothing to show, and a zero-size layer-backed
             * view is a good way to make AppKit unhappy. */
            n->host.clipBox.hidden = YES;
            return;
        }
        n->host.clipBox.frame = c;
        /* Only an explicit set_visible(true) may reveal it — `-1` is the
         * never-told-yet state and must stay hidden. */
        n->host.clipBox.hidden = (n->last_visible != 1);
        n->host.webBox.frame = NSMakeRect(f.origin.x - c.origin.x,
                                          f.origin.y - c.origin.y,
                                          f.size.width, f.size.height);
        if (n->vp_w > 0 && n->vp_h > 0) {
            /* box bounds > box frame ⇒ AppKit scales the box's subtree down
             * while the web view (frame == box bounds == device size) lays out
             * at the device's CSS viewport. The scale must be on the BOX:
             * WKWebView itself does not survive frame != bounds (see webBox). */
            [n->host.webBox setBoundsSize:NSMakeSize((CGFloat)n->vp_w,
                                                     (CGFloat)n->vp_h)];
            n->host.webView.frame = NSMakeRect(0, 0, (CGFloat)n->vp_w,
                                                     (CGFloat)n->vp_h);
        } else {
            /* No device viewport: bounds follow frame again. */
            [n->host.webBox setBoundsSize:f.size];
            n->host.webView.frame = NSMakeRect(0, 0, f.size.width, f.size.height);
        }
    }
}

BrowserNative *browser_native_create(PlatformWindow *window) {
    void *nv = platform_window_native_view(window);
    if (!nv) return NULL;
    NSView *parent = (__bridge NSView *)nv;
    BrowserNative *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->last_visible = -1;   /* unknown: first set_visible must reach AppKit */
    @autoreleasepool {
        n->host = [[LiuBrowserHost alloc] initWithParentView:parent];
        if (!n->host) { free(n); return NULL; }
    }
    return n;
}

bool browser_native_attach_window(BrowserNative *n, PlatformWindow *window) {
    if (!n || !n->host) return false;
    void *nv = platform_window_native_view(window);
    if (!nv) return false;
    NSView *parent = (__bridge NSView *)nv;
    @autoreleasepool {
        NSView *clip = n->host.clipBox;
        if (!clip) return false;
        if (clip.superview == parent) return true;
        /* Leaving a window while holding its first responder would leave that
         * window with a responder that is no longer in it. */
        NSWindow *old = n->host.webView.window;
        if (old && old.firstResponder == n->host.webView)
            [old makeFirstResponder:old.contentView];
        [clip retain];                 /* the superview holds the only strong ref */
        clip.hidden = YES;             /* never seen in the new window before it is placed */
        [clip removeFromSuperview];
        [parent addSubview:clip positioned:NSWindowAbove relativeTo:nil];
        [clip release];
        /* Clear cached visibility and geometry state when moving the view to a
         * new window so the next update uses the new window's coordinates. */
        n->last_visible = -1;
        n->have_frame   = false;
        n->have_clip    = false;
        n->clip         = NSZeroRect;
    }
    return true;
}

bool browser_native_in_window(BrowserNative *n, PlatformWindow *window) {
    if (!n || !n->host || !n->host.clipBox) return false;
    void *nv = platform_window_native_view(window);
    return nv && n->host.clipBox.superview == (__bridge NSView *)nv;
}

void browser_native_destroy(BrowserNative *n) {
    if (!n) return;
    @autoreleasepool {
        if (n->host) {
            [n->host teardown];
            [n->host release];
            n->host = nil;
        }
    }
    free(n);
}

void browser_native_set_visible(BrowserNative *n, bool visible) {
    if (!n || !n->host) return;
    /* Called every main-loop wake for every browser-carrying tab — skip the
     * ObjC round trip (autoreleasepool + setHidden + responder juggling)
     * entirely when nothing changed. */
    if (n->last_visible == (visible ? 1 : 0)) return;
    n->last_visible = visible ? 1 : 0;
    @autoreleasepool {
        /* Showing a fully-occluded browser must stay hidden — let the geometry
         * pass decide, it owns that rule. */
        if (visible && n->have_frame) browser_apply_geometry(n);
        else n->host.clipBox.hidden = !visible;
        if (!visible) {
            NSWindow *w = n->host.webView.window;
            if (w && w.firstResponder == n->host.webView)
                [w makeFirstResponder:w.contentView];
        }
    }
}

void browser_native_set_cmd_passthrough(BrowserNative *n, bool enabled) {
    if (!n || !n->host) return;
    /* Called every main-loop wake — same per-wake no-op guard as set_visible. */
    if (n->cmd_passthrough == enabled) return;
    n->cmd_passthrough = enabled;
    @autoreleasepool {
        n->host.clipBox.cmdPassthrough = enabled ? YES : NO;
    }
}

void browser_native_set_clip_pt(BrowserNative *n, bool clipped,
                                f32 x_pt, f32 y_pt, f32 w_pt, f32 h_pt) {
    if (!n || !n->host || !n->host.clipBox) return;
    bool want_clip = clipped && isfinite(x_pt) && isfinite(y_pt) &&
                     isfinite(w_pt) && isfinite(h_pt);
    if (w_pt < 0.0f) w_pt = 0.0f;
    if (h_pt < 0.0f) h_pt = 0.0f;
    NSRect c = want_clip ? NSMakeRect(x_pt, y_pt, w_pt, h_pt) : NSZeroRect;
    /* Called every main-loop wake — same per-wake no-op guard as set_visible. */
    if (n->have_clip == want_clip &&
        (!want_clip || NSEqualRects(n->clip, c))) return;
    n->have_clip = want_clip;
    n->clip = c;
    if (n->have_frame) browser_apply_geometry(n);
}

void browser_native_set_frame_pt(BrowserNative *n,
                                 f32 x_pt, f32 y_pt, f32 w_pt, f32 h_pt) {
    if (!n || !n->host || !n->host.webView || !n->host.webBox) return;
    if (!isfinite(x_pt) || !isfinite(y_pt) ||
        !isfinite(w_pt) || !isfinite(h_pt)) return;
    if (w_pt < 1.0f) w_pt = 1.0f;
    if (h_pt < 1.0f) h_pt = 1.0f;
    NSRect r = NSMakeRect(x_pt, y_pt, w_pt, h_pt);
    if (n->have_frame && NSEqualRects(n->last_frame, r) &&
        n->vp_w == 0 && n->vp_h == 0) return;        /* per-wake no-op */
    n->have_frame = true;
    n->last_frame = r;
    n->vp_w = 0;
    n->vp_h = 0;
    browser_apply_geometry(n);
}

void browser_native_set_viewport_pt(BrowserNative *n,
                                    f32 x_pt, f32 y_pt, f32 w_pt, f32 h_pt,
                                    i32 vw, i32 vh) {
    if (!n || !n->host || !n->host.webView || !n->host.webBox) return;
    if (!isfinite(x_pt) || !isfinite(y_pt) ||
        !isfinite(w_pt) || !isfinite(h_pt)) return;
    if (w_pt < 1.0f) w_pt = 1.0f;
    if (h_pt < 1.0f) h_pt = 1.0f;
    if (vw < 0) vw = 0;
    if (vh < 0) vh = 0;

    NSRect r = NSMakeRect(x_pt, y_pt, w_pt, h_pt);
    if (n->have_frame && NSEqualRects(n->last_frame, r) &&
        n->vp_w == vw && n->vp_h == vh) return;      /* per-wake no-op */
    n->have_frame = true;
    n->last_frame = r;
    n->vp_w = vw;
    n->vp_h = vh;
    browser_apply_geometry(n);
}

void browser_native_make_first_responder(BrowserNative *n) {
    if (!n || !n->host) return;
    @autoreleasepool {
        [n->host.webView.window makeFirstResponder:n->host.webView];
    }
}

/* The keyboard back from the page to Liu's own view — the one the page's
 * clip box sits in. Cmd+L pressed in the page reaches Liu as a forwarded key
 * equivalent and focused the address bar, but the page kept the keyboard:
 * the address typed and its Enter went into the page's focused form. */
void browser_native_resign_first_responder(BrowserNative *n) {
    if (!n || !n->host) return;
    @autoreleasepool {
        NSWindow *w = n->host.webView.window;
        NSView *liu = n->host.clipBox.superview;
        NSResponder *fr = w.firstResponder;
        if (w && liu && [fr isKindOfClass:[NSView class]] &&
            [(NSView *)fr isDescendantOf:n->host.webView])
            [w makeFirstResponder:liu];
    }
}

void browser_native_set_allowlist(BrowserNative *n, const BrowserAllowlist *list) {
    if (n && n->host) n->host.allowlist = list;
}

bool browser_native_navigate(BrowserNative *n, const char *url) {
    if (!n || !n->host || !url || !url[0]) return false;
    if (strnlen(url, BROWSER_URL_CAP) >= BROWSER_URL_CAP) return false;
    if (!browser_nav_is_allowed_with_list(url, n->host.allowlist)) return false;
    @autoreleasepool {
        NSString *s = [NSString stringWithUTF8String:url];
        NSURL *u = [NSURL URLWithString:s];
        if (!u) return false;
        /* Typed "…/#/settings" over "…/#/home": nothing loads and nothing
         * finishes, so a spinner set here stayed on for good. */
        bool same_doc = !n->host.showingStart && !n->host.showingError &&
                        liu_same_document(n->host.webView.URL, u);
        [n->host.webView loadRequest:[NSURLRequest requestWithURL:u]];
        n->host.pageURL = s;
        if (!same_doc) {
            n->host.loading = true;
            n->host.statusMsg = @"Loading…";
        }
    }
    return true;
}

/* Shared retry path: on the connection-error page there is nothing cached to
 * bypass, so every flavour of reload just re-requests the failed URL. */
static bool liu_browser_retry_error_page(LiuBrowserHost *h) {
    if (!h.showingError || !h.errURL.length) return false;
    NSURL *u = [NSURL URLWithString:h.errURL];
    if (!u) return false;
    [h.webView loadRequest:[NSURLRequest requestWithURL:u]];
    h.loading = true;
    h.statusMsg = @"Loading…";
    return true;
}

void browser_native_reload(BrowserNative *n) {
    if (!n || !n->host) return;
    @autoreleasepool {
        LiuBrowserHost *h = n->host;
        /* On the connection-error page, reload retries the failed URL (the dev
         * server may have just come up) rather than re-showing the error doc. */
        if (liu_browser_retry_error_page(h)) return;
        [h.webView reload];
    }
}


void browser_native_reload_hard(BrowserNative *n) {
    if (!n || !n->host) return;
    @autoreleasepool {
        LiuBrowserHost *h = n->host;
        if (liu_browser_retry_error_page(h)) return;
        /* reloadFromOrigin is the cache-bypassing reload; plain -reload will
         * happily serve the very bundle the user is trying to get rid of. */
        [h.webView reloadFromOrigin];
        h.statusMsg = @"Hard reload…";
    }
}

void browser_native_reset_and_reload(BrowserNative *n) {
    if (!n || !n->host) return;
    @autoreleasepool {
        LiuBrowserHost *h = n->host;
        WKWebsiteDataStore *store = h.webView.configuration.websiteDataStore;
        NSSet *types = [WKWebsiteDataStore allWebsiteDataTypes];
        h.statusMsg = @"Clearing site data…";
        [store removeDataOfTypes:types
                   modifiedSince:[NSDate distantPast]
               completionHandler:^{
            /* The host may have been torn down while the purge was in flight
             * (tab closed); the retained `h` keeps this safe to touch. */
            if (!h.webView) return;
            if (liu_browser_retry_error_page(h)) return;
            [h.webView reloadFromOrigin];
            h.loading = true;
            h.statusMsg = @"Site data cleared - reloading…";
        }];
    }
}

void browser_native_go_back(BrowserNative *n) {
    if (!n || !n->host) return;
    @autoreleasepool { [n->host.webView goBack]; }
}

void browser_native_go_forward(BrowserNative *n) {
    if (!n || !n->host) return;
    @autoreleasepool { [n->host.webView goForward]; }
}

void browser_native_stop(BrowserNative *n) {
    if (!n || !n->host) return;
    @autoreleasepool {
        [n->host.webView stopLoading];
        n->host.loading = false;
    }
}

bool browser_native_poll(BrowserNative *n, BrowserTab *tab) {
    if (!n || !n->host || !tab) return false;
    @autoreleasepool {
        LiuBrowserHost *h = n->host;
        /* O(1) hot path: nothing mutated since the last poll → done. Every
         * poll-visible property bumps h.revision from its setter, including
         * history/URL changes that arrive without a new document load. */
        u64 rev = h.revision;
        if (rev == n->polled_rev) return false;
        n->polled_rev = rev;
        tab->loading = h.loading;
        tab->can_go_back = h.canBack;
        tab->can_go_forward = h.canFwd;
        tab->pick_mode = h.pickMode;
        const char *title = h.pageTitle.UTF8String;
        const char *page_url = h.pageURL.UTF8String;
        const char *status = h.statusMsg.UTF8String;
        /* Use the page title for the tab label after removing bidi overrides,
         * invisible characters, C1 controls, and line breaks. Truncate without
         * splitting a character. */
        if (h.pageTitle.length && title)
            (void)liu_clean_dictation(title, tab->title, (i32)sizeof tab->title);
        else
            tab->title[0] = '\0';
        if (h.pageURL.length && page_url)
            snprintf(tab->url, sizeof tab->url, "%s", page_url);
        else
            tab->url[0] = '\0';
        if (h.statusMsg.length && status)
            snprintf(tab->status, sizeof tab->status, "%s", status);
        else
            tab->status[0] = '\0';
        tab->pending_pick = h.hasPendingPick;
        tab->pending_ask = h.hasPendingAsk;
        tab->pending_nav = h.hasPendingNav;
    }
    return true;
}

bool browser_native_set_pick_mode(BrowserNative *n, bool enabled) {
    if (!n || !n->host) return false;
    @autoreleasepool {
        return [n->host setPickModeEnabled:enabled ? YES : NO] ? true : false;
    }
}

void browser_native_set_pick_theme(BrowserNative *n, const char *theme_json) {
    if (!n || !n->host || !n->host.webView || !theme_json || !theme_json[0]) return;
    @autoreleasepool {
        NSString *obj = [NSString stringWithUTF8String:theme_json];
        if (!obj) return;
        NSString *js = [NSString stringWithFormat:
            @"window.__liuPickSetTheme && window.__liuPickSetTheme(%@);", obj];
        [n->host.webView evaluateJavaScript:js inFrame:nil inContentWorld:LiuBrowserWorld()
                        completionHandler:nil];
    }
}

/* Family name of a font FILE, or nil. Only returned when the family also
 * resolves through the normal font lookup: the page renders in WebKit's own
 * process, which cannot see a font Liu merely read off disk, so an
 * un-installed family would silently fall back anyway. */
static NSString *LiuResolveMonoFamily(const char *font_path) {
    if (!font_path || !font_path[0]) return nil;
    NSString *p = [NSString stringWithUTF8String:font_path];
    if (!p.length) return nil;
    NSURL *url = [NSURL fileURLWithPath:p];
    NSArray *descs = (NSArray *)CTFontManagerCreateFontDescriptorsFromURL(
        (__bridge CFURLRef)url);
    if (!descs) return nil;
    NSString *family = nil;
    if (descs.count > 0) {
        CTFontDescriptorRef d = (__bridge CTFontDescriptorRef)descs[0];
        NSString *f = (NSString *)CTFontDescriptorCopyAttribute(
            d, kCTFontFamilyNameAttribute);
        if (f.length && [NSFont fontWithName:f size:12.0])
            family = [[f retain] autorelease];
        [f release];
    }
    [descs release];
    return family;
}

void browser_native_set_page_theme(BrowserNative *n, const char *css_decls,
                                   const char *font_path) {
    if (!n || !n->host || !css_decls || !css_decls[0]) return;
    @autoreleasepool {
        NSString *css = [NSString stringWithUTF8String:css_decls];
        if (!css.length) return;
        /* The declarations are interpolated into a <style> block, so a stray
         * "<" or "}" would break out of the rule. Liu builds this string
         * itself, but the guard keeps that a local invariant. */
        if ([css rangeOfCharacterFromSet:
                [NSCharacterSet characterSetWithCharactersInString:@"<>{}"]]
                .location != NSNotFound) return;
        [n->host setPageThemeCSS:css
                      monoFamily:LiuResolveMonoFamily(font_path)];
    }
}

bool browser_native_take_pick(BrowserNative *n, BrowserPick *out) {
    if (!n || !n->host || !out) return false;
    @autoreleasepool {
        LiuBrowserHost *h = n->host;
        if (!h.hasPendingPick || !h.pendingJson) return false;
        parse_pick_json(h.pendingJson, out);
        free(h.pendingJson);
        h.pendingJson = NULL;
        h.hasPendingPick = false;
        return out->tag[0] != '\0' || out->selector[0] != '\0' || out->html[0] != '\0';
    }
}

bool browser_native_take_ask(BrowserNative *n, char *url, usize url_cap,
                             char *reason, usize reason_cap, i32 *code) {
    if (!n || !n->host) return false;
    if ((url && url_cap == 0) || (reason && reason_cap == 0)) return false;
    @autoreleasepool {
        LiuBrowserHost *h = n->host;
        if (!h.hasPendingAsk) return false;
        const char *us = h.errURL.UTF8String ? h.errURL.UTF8String : "";
        const char *rs = h.errReason.UTF8String ? h.errReason.UTF8String : "";
        /* A cut address is another address: that one is refused, and the
         * request with it, rather than left pending for every later poll. */
        if (url && url_cap && strlen(us) >= url_cap) { h.hasPendingAsk = false; return false; }
        if (url && url_cap) snprintf(url, url_cap, "%s", us);
        /* The reason is words for the agent, and the system's own are long: an
         * untrusted certificate's is ~180 bytes, more than the caller's 128. A
         * reason that does not fit is cut, on a character, rather than refusing
         * the whole request. */
        if (reason && reason_cap) {
            usize n = strlen(rs);
            if (n >= reason_cap) {
                n = reason_cap - 1;
                while (n > 0 && ((unsigned char)rs[n] & 0xC0) == 0x80) n--;
            }
            memcpy(reason, rs, n);
            reason[n] = '\0';
        }
        if (code) *code = h.errCode;
        h.hasPendingAsk = false;
        return true;
    }
}

void browser_native_show_start_page(BrowserNative *n, const char *recents_json) {
    if (!n || !n->host) return;
    @autoreleasepool {
        NSArray *recents = nil;
        if (recents_json && recents_json[0]) {
            usize json_len = strnlen(recents_json, 256u * 1024u + 1u);
            if (json_len > 256u * 1024u) return;
            NSData *d = [NSData dataWithBytes:recents_json length:json_len];
            id arr = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
            if ([arr isKindOfClass:[NSArray class]]) recents = arr;
        }
        [n->host showStartPageWithRecents:recents];
    }
}

bool browser_native_take_nav(BrowserNative *n, char *url, usize url_cap) {
    if (!n || !n->host || !url || url_cap == 0) return false;
    url[0] = '\0';
    @autoreleasepool {
        LiuBrowserHost *h = n->host;
        if (!h.hasPendingNav) return false;
        const char *pending = h.pendingNavURL.UTF8String
                            ? h.pendingNavURL.UTF8String : "";
        usize len = strlen(pending);
        if (len == 0 || len >= url_cap || !browser_nav_is_allowed_with_list(pending, h.allowlist)) {
            h.hasPendingNav = false;
            return false;
        }
        memcpy(url, pending, len + 1u);
        h.hasPendingNav = false;
        return url[0] != '\0';
    }
}
