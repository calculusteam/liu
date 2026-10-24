#ifndef BROWSER_NAV_H
#define BROWSER_NAV_H

#include "core/types.h"

#define BROWSER_ALLOWLIST_MAX 32
#define BROWSER_RULE_CAP 320
typedef struct {
    char rules[BROWSER_ALLOWLIST_MAX][BROWSER_RULE_CAP];
    i32 count;
} BrowserAllowlist;

/* Bare hosts allow HTTP(S) on any port. *.host allows subdomains only.
 * A URL grants its origin (scheme, host, effective port), across all paths.
 * Invalid rules never grant access. No DNS or allocation in these helpers. */
bool browser_nav_rule_normalize(const char *input, char *out, usize cap);
bool browser_nav_allowlist_add(BrowserAllowlist *list, const char *input);
bool browser_nav_allowlist_remove(BrowserAllowlist *list, i32 index);
bool browser_nav_allowlist_matches(const BrowserAllowlist *list, const char *url);
bool browser_nav_is_allowed_with_list(const char *url, const BrowserAllowlist *list);
bool browser_nav_is_allowed_resolved_with_list(const char *url, const BrowserAllowlist *list);

/* The built-in policy (without additional rules) NAVIGATES to local networks: loopback, private
 * LANs (RFC 1918, link-local, IPv6 ULA) and Tailscale (100.64/10,
 * fd7a:115c::/48). Browsing to the public internet is refused; what a local
 * page itself loads from it (CDN, fonts, APIs, embeds) is not. */
typedef enum {
    BROWSER_NAV_INVALID = 0,   /* not http(s), unparseable, or unresolvable */
    BROWSER_NAV_LOCAL,         /* loopback / private / tailnet */
    BROWSER_NAV_PUBLIC,        /* public internet */
    BROWSER_NAV_NEEDS_DNS,     /* a name — only resolving it can tell */
} BrowserNavClass;

/* Normalize user input into a full URL when possible.
 * Accepts ":5173", "localhost:3000", "http://127.0.0.1:8080/", etc.
 * Returns false if input is empty. out always NUL-terminated when true. */
bool browser_nav_normalize(const char *in, char *out, usize out_cap);

/* Why an address typed into the bar does not open, in words for a notice —
 * NULL when it opens, or when there is nothing to say (an empty field). */
const char *browser_nav_typed_refusal(const char *typed, const BrowserAllowlist *list);

/* Cheap, non-blocking gate: http/https whose host is a local address literal
 * or a NAME (still to be resolved — the WebView's policy delegate does that
 * off the main thread). False for a public address literal. */
bool browser_nav_is_allowed(const char *url);

/* Full gate: as above, and a name must resolve ONLY to local addresses.
 * BLOCKS on DNS — never call it on the main thread of the macOS app. */
bool browser_nav_is_allowed_resolved(const char *url);

/* Classify a URL / host without touching the network. */
BrowserNavClass browser_nav_url_classify(const char *url);
BrowserNavClass browser_nav_host_classify(const char *host);

/* Resolve a host (BLOCKING getaddrinfo) and classify what it resolves to:
 * LOCAL only if every address is local, PUBLIC if any is not, INVALID when it
 * does not resolve. Address literals are answered without a lookup. */
BrowserNavClass browser_nav_resolve_host(const char *host);

/* Address predicates (network byte order). */
bool browser_nav_ipv4_is_local(const u8 a[4]);
bool browser_nav_ipv6_is_local(const u8 a[16]);

/* Extract host from URL (no port). out may be empty on parse failure. */
bool browser_nav_host(const char *url, char *out, usize out_cap);

/* True if host is loopback (localhost / *.localhost / 127/8 / ::1). */
bool browser_nav_host_is_loopback(const char *host);

typedef enum {
    BROWSER_NAV_ALLOW = 0,
    BROWSER_NAV_REFUSE,     /* cancel; a main frame gets the "blocked" page */
    BROWSER_NAV_HANDOFF,    /* cancel and open in the system browser */
    BROWSER_NAV_RESOLVE,    /* a name — resolve, then decide again */
} BrowserNavAction;

/* What to do with one document navigation (main frame or iframe). `cls` is
 * browser_nav_url_classify's answer, or the resolver's once it has one.
 * Only documents come here — subresources (CDN scripts, fonts, fetch) are
 * never policed, so a local site's own dependencies load normally. */
BrowserNavAction browser_nav_decide(const char *url, BrowserNavClass cls,
                                    bool main_frame, bool link_click);
BrowserNavAction browser_nav_decide_with_list(const char *url, BrowserNavClass cls,
                                             bool main_frame, bool link_click,
                                             const BrowserAllowlist *list);

#endif
