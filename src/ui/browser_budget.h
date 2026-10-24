/*
 * Liu — how many web views this PROCESS may hold open.
 *
 * `BROWSER_MAX_TABS` is "at most 4 browser views", and the toast says so in
 * words. The cap is over the process, not over one window's `app->tabs`.
 *
 * Every WKWebView starts its own com.apple.WebKit.WebContent process, plus one
 * shared Networking and one shared GPU process for the whole app. They live
 * outside Liu's address space and therefore outside the memory ledger
 * entirely, so nothing can see them approach MEM_BUDGET_SOFT_TOTAL. Sharing
 * one WKProcessPool does not collapse them: WebKit is process-per-view
 * regardless of the pool, so the count is the only lever.
 *
 * The two counting rules live in this header so they can be checked without
 * linking ui.c:
 *
 *   1. A browser opened in a SPLIT PANE is a web view too, so a window's
 *      tally counts browser panes as well as `kind == TAB_BROWSER` tabs.
 *   2. The count is over the process, not the window. It is passed in rather
 *      than derived from an AppState, because an AppState cannot see its
 *      siblings.
 */
#ifndef UI_BROWSER_BUDGET_H
#define UI_BROWSER_BUDGET_H

#include "core/types.h"

/* BROWSER_MAX_TABS lives in ui/layout.h and browser/browser_types.h; this
 * header takes it as an argument so the policy can be tested against any cap
 * and cannot drift from whichever copy a caller happened to include. */

/* May one more web view be created? `live` is the count across every window.
 *
 * A cap of zero or less means "no browser at all" and refuses everything,
 * rather than wrapping into "unlimited" the way a `live > cap - 1` test on
 * unsigned arithmetic would. */
static inline bool browser_view_admit(i32 live, i32 cap) {
    if (cap <= 0) return false;
    if (live < 0) live = 0;
    return live < cap;
}

/* What the counter should read, given each window's own tally. The real code
 * keeps a running total instead of summing on demand — this is the definition
 * that total has to agree with, and the reason it cannot be computed inside
 * one AppState. */
static inline i32 browser_views_total(const i32 *per_window, i32 windows) {
    if (!per_window || windows <= 0) return 0;
    i32 n = 0;
    for (i32 i = 0; i < windows; i++) {
        i32 v = per_window[i];
        if (v > 0) n += v;
    }
    return n;
}

/* One window's tally: browser TABS plus browser PANES. Both are a WKWebView and
 * both cost a WebContent process. */
static inline i32 browser_views_in_window(i32 browser_tabs, i32 browser_panes) {
    if (browser_tabs < 0) browser_tabs = 0;
    if (browser_panes < 0) browser_panes = 0;
    return browser_tabs + browser_panes;
}

#endif /* UI_BROWSER_BUDGET_H */
