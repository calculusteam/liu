/*
 * Liu Browser navigation policy: the in-app browser NAVIGATES to LOCAL
 * networks only — loopback, RFC 1918 LANs, link-local, and Tailscale's 100.64/10 and
 * fd7a:115c:a1e0::/48 (plus any IPv6 ULA). The public internet is refused.
 *
 * A name is judged by what it resolves to, not by how it is spelled, so
 * MagicDNS short names, split-DNS names and mDNS `.local` hosts all work
 * while a public site does not, however it is written.
 *
 * Only DOCUMENTS are policed (browser_nav_decide): a local site's CDN scripts,
 * web fonts and API calls load normally.
 */
#include "browser/browser_nav.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static bool lower_copy(char *dst, usize cap, const char *src) {
    if (!dst || cap == 0) return false;
    usize i = 0;
    if (src) {
        for (; src[i] && i + 1 < cap; i++) {
            unsigned char c = (unsigned char)src[i];
            dst[i] = (char)tolower(c);
        }
    }
    dst[i] = '\0';
    return !src || src[i] == '\0';
}

/* Strip IPv6 brackets and a zone id ("[fe80::1%25en0]" -> "fe80::1") into
 * a lower-cased copy. False when the host does not fit. */
static bool bare_host(char *dst, usize cap, const char *host) {
    if (!lower_copy(dst, cap, host)) return false;
    if (dst[0] == '[') {
        usize n = strlen(dst);
        if (n < 3 || dst[n - 1] != ']') return false;
        memmove(dst, dst + 1, n - 2);
        dst[n - 2] = '\0';
        char *zone = strchr(dst, '%');
        if (zone) *zone = '\0';
    }
    return dst[0] != '\0';
}

bool browser_nav_host_is_loopback(const char *host) {
    if (!host || !host[0]) return false;
    char h[256];
    if (!bare_host(h, sizeof h, host)) return false;
    if (strcmp(h, "localhost") == 0) return true;
    /* *.localhost — RFC 6761 reserves the whole tree for loopback. */
    usize len = strlen(h);
    if (len > 10 && strcmp(h + len - 10, ".localhost") == 0) return true;
    u8 a4[4], a6[16];
    if (inet_pton(AF_INET, h, a4) == 1) return a4[0] == 127;
    if (inet_pton(AF_INET6, h, a6) == 1) {
        static const u8 k_loop6[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        return memcmp(a6, k_loop6, 16) == 0;
    }
    return false;
}

bool browser_nav_ipv4_is_local(const u8 a[4]) {
    if (!a) return false;
    if (a[0] == 0)   return true;                          /* 0/8: "this host" (0.0.0.0 dev servers) */
    if (a[0] == 127) return true;                          /* loopback */
    if (a[0] == 10)  return true;                          /* RFC 1918 */
    if (a[0] == 172 && (a[1] & 0xF0) == 16) return true;   /* RFC 1918 172.16/12 */
    if (a[0] == 192 && a[1] == 168) return true;           /* RFC 1918 */
    if (a[0] == 169 && a[1] == 254) return true;           /* link-local */
    if (a[0] == 100 && (a[1] & 0xC0) == 64) return true;   /* 100.64/10 CGNAT: Tailscale */
    return false;
}

bool browser_nav_ipv6_is_local(const u8 a[16]) {
    if (!a) return false;
    static const u8 k_zero[15] = {0};
    /* :: and ::1 */
    if (memcmp(a, k_zero, 15) == 0 && (a[15] == 0 || a[15] == 1)) return true;
    if ((a[0] & 0xFE) == 0xFC) return true;                   /* fc00::/7 ULA (Tailscale fd7a:115c:a1e0::/48) */
    if (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) return true;   /* fe80::/10 link-local */
    if (a[0] == 0xFE && (a[1] & 0xC0) == 0xC0) return true;   /* fec0::/10 old site-local */
    /* ::ffff:a.b.c.d (mapped) and 64:ff9b::a.b.c.d (NAT64) carry an IPv4
     * address — judge that one, or a public v4 walks in dressed as v6. */
    static const u8 k_mapped[12] = {0,0,0,0,0,0,0,0,0,0,0xFF,0xFF};
    static const u8 k_nat64[12]  = {0,0x64,0xFF,0x9B,0,0,0,0,0,0,0,0};
    if (memcmp(a, k_mapped, 12) == 0 || memcmp(a, k_nat64, 12) == 0)
        return browser_nav_ipv4_is_local(a + 12);
    return false;
}

BrowserNavClass browser_nav_host_classify(const char *host) {
    if (!host || !host[0]) return BROWSER_NAV_INVALID;
    char h[256];
    if (!bare_host(h, sizeof h, host)) return BROWSER_NAV_INVALID;
    if (browser_nav_host_is_loopback(h)) return BROWSER_NAV_LOCAL;
    u8 a4[4], a6[16];
    if (inet_pton(AF_INET, h, a4) == 1)
        return browser_nav_ipv4_is_local(a4) ? BROWSER_NAV_LOCAL : BROWSER_NAV_PUBLIC;
    if (inet_pton(AF_INET6, h, a6) == 1)
        return browser_nav_ipv6_is_local(a6) ? BROWSER_NAV_LOCAL : BROWSER_NAV_PUBLIC;
    /* A name — MagicDNS (`box`, `box.tailnet.ts.net`), mDNS (`nas.local`), a
     * split-DNS corporate name, or a public site. Only the resolver can tell,
     * and so does NOT a suffix list: `*.ts.net` is also Tailscale Funnel, and
     * `2130706433` / `127.1` are legacy IPv4 spellings getaddrinfo parses. */
    return BROWSER_NAV_NEEDS_DNS;
}

BrowserNavClass browser_nav_url_classify(const char *url) {
    if (!url || !url[0]) return BROWSER_NAV_INVALID;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return BROWSER_NAV_INVALID;
    char host[256];
    if (!browser_nav_host(url, host, sizeof host)) return BROWSER_NAV_INVALID;
    return browser_nav_host_classify(host);
}

BrowserNavClass browser_nav_resolve_host(const char *host) {
    BrowserNavClass c = browser_nav_host_classify(host);
    if (c != BROWSER_NAV_NEEDS_DNS) return c;
    char h[256];
    if (!bare_host(h, sizeof h, host)) return BROWSER_NAV_INVALID;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(h, NULL, &hints, &res) != 0 || !res) return BROWSER_NAV_INVALID;

    /* EVERY address must be local: WebKit may connect to any of them, so one
     * public record in the set is a way out to the internet. */
    bool any = false, all_local = true;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET && ai->ai_addr) {
            const struct sockaddr_in *sa = (const struct sockaddr_in *)ai->ai_addr;
            u8 a[4];
            memcpy(a, &sa->sin_addr, 4);
            any = true;
            if (!browser_nav_ipv4_is_local(a)) all_local = false;
        } else if (ai->ai_family == AF_INET6 && ai->ai_addr) {
            const struct sockaddr_in6 *sa = (const struct sockaddr_in6 *)ai->ai_addr;
            u8 a[16];
            memcpy(a, &sa->sin6_addr, 16);
            any = true;
            if (!browser_nav_ipv6_is_local(a)) all_local = false;
        }
    }
    freeaddrinfo(res);
    if (!any) return BROWSER_NAV_INVALID;
    return all_local ? BROWSER_NAV_LOCAL : BROWSER_NAV_PUBLIC;
}

bool browser_nav_is_allowed_resolved(const char *url) {
    BrowserNavClass c = browser_nav_url_classify(url);
    if (c == BROWSER_NAV_NEEDS_DNS) {
        char host[256];
        if (!browser_nav_host(url, host, sizeof host)) return false;
        c = browser_nav_resolve_host(host);
    }
    return c == BROWSER_NAV_LOCAL;
}

bool browser_nav_host(const char *url, char *out, usize out_cap) {
    if (!out || out_cap == 0) return false;
    out[0] = '\0';
    if (!url || !url[0]) return false;

    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;
    else return false;

    /* host[:port][/...] */
    const char *end = p;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    char hostport[512];
    usize n = (usize)(end - p);
    if (n == 0 || n >= sizeof hostport) return false;
    memcpy(hostport, p, n);
    hostport[n] = '\0';
    /* Userinfo is unnecessary for local previews and makes a hand-written
     * authority parser dangerous (`localhost:80@evil.test` could be
     * classified as localhost). Backslashes/whitespace are likewise parsed
     * inconsistently across URL stacks, so reject them at the boundary. */
    if (strchr(hostport, '@') || strchr(hostport, '\\')) return false;
    for (usize i = 0; i < n; i++)
        if ((u8)hostport[i] <= 0x20 || (u8)hostport[i] == 0x7f) return false;

    /* Drop port (careful with IPv6 [::1]:port). */
    if (hostport[0] == '[') {
        char *rb = strchr(hostport, ']');
        if (!rb) return false;
        const char *suffix = rb + 1;
        if (*suffix) {
            if (*suffix++ != ':' || !*suffix) return false;
            u32 port = 0;
            for (; *suffix; suffix++) {
                if (*suffix < '0' || *suffix > '9') return false;
                port = port * 10u + (u32)(*suffix - '0');
                if (port > 65535u) return false;
            }
            if (port == 0) return false;
        }
        usize hn = (usize)(rb - hostport + 1);
        if (hn >= out_cap) return false;
        memcpy(out, hostport, hn);
        out[hn] = '\0';
        return true;
    }
    char *colon = strchr(hostport, ':');
    if (colon) {
        if (strchr(colon + 1, ':') || !colon[1]) return false;
        u32 port = 0;
        for (char *q = colon + 1; *q; q++) {
            if (*q < '0' || *q > '9') return false;
            port = port * 10u + (u32)(*q - '0');
            if (port > 65535u) return false;
        }
        if (port == 0) return false;
        *colon = '\0';
    }
    if (!hostport[0]) return false;
    int wrote = snprintf(out, out_cap, "%s", hostport);
    return wrote > 0 && (usize)wrote < out_cap;
}

bool browser_nav_is_allowed(const char *url) {
    BrowserNavClass c = browser_nav_url_classify(url);
    return c == BROWSER_NAV_LOCAL || c == BROWSER_NAV_NEEDS_DNS;
}

/* ---- Per-frame decision ---------------------------------------------------
 *
 * What is refused is BROWSING the public web, not a local site's own
 * dependencies. WebKit asks the navigation delegate only about documents —
 * the main frame and iframes — so scripts, styles, fonts, images, fetch/XHR
 * and WebSockets a local page pulls from a CDN or an API never come here and
 * load as they would in any browser.
 *
 *   main frame   local → allow; public → refused (a real click goes to the
 *                system browser instead); a name → resolve first.
 *   iframe       the site embedding something (YouTube, Stripe, a map) →
 *                allow, no lookup needed. But a CLICK inside it that leads to
 *                the public web is browsing in a box, so it is handed to the
 *                system browser like a main-frame click. about:/data:/blob:
 *                frames are the page's own content. */
static bool scheme_is(const char *url, const char *scheme) {
    usize n = strlen(scheme);
    return url && strncmp(url, scheme, n) == 0;
}

BrowserNavAction browser_nav_decide(const char *url, BrowserNavClass cls,
                                    bool main_frame, bool link_click) {
    if (cls == BROWSER_NAV_LOCAL) return BROWSER_NAV_ALLOW;
    if (!main_frame) {
        if (cls == BROWSER_NAV_INVALID)
            return (scheme_is(url, "about:") || scheme_is(url, "data:") ||
                    scheme_is(url, "blob:"))
                ? BROWSER_NAV_ALLOW : BROWSER_NAV_REFUSE;
        if (!link_click) return BROWSER_NAV_ALLOW;          /* an embed */
    }
    if (cls == BROWSER_NAV_NEEDS_DNS) return BROWSER_NAV_RESOLVE;
    if (cls == BROWSER_NAV_PUBLIC)
        return link_click ? BROWSER_NAV_HANDOFF : BROWSER_NAV_REFUSE;
    return BROWSER_NAV_REFUSE;
}

/* Shared bounded parsing for user input and allowlist rules. */
static bool trim_copy(const char *in, char *out, usize cap) {
    if (!out || !cap) return false;
    out[0] = '\0';
    if (!in) return false;
    while (isspace((u8)*in)) in++;
    usize n = strlen(in);
    while (n && isspace((u8)in[n - 1])) n--;
    if (!n || n >= cap) return false;
    memcpy(out, in, n); out[n] = '\0';
    for (usize i = 0; i < n; i++)
        if ((u8)out[i] <= 0x20 || (u8)out[i] == 0x7f || out[i] == '\\') return false;
    return true;
}

typedef struct {
    char host[256];
    u32 port;                 /* 0 = all ports, for a bare host rule */
    i32 scheme;               /* 0 = both, 1 = http, 2 = https */
    bool wildcard;
} BrowserRule;

/* Reject ambiguous authority spellings rather than letting the C parser and
 * WebKit disagree. Numeric addresses are canonicalized, names are ASCII DNS
 * labels (international names can use their xn-- spelling). */
static bool canonical_host(char *host) {
    char bare[256];
    if (!bare_host(bare, sizeof bare, host)) return false;
    u8 a[16];
    if (host[0] == '[') {
        if (strchr(host, '%') || inet_pton(AF_INET6, bare, a) != 1) return false;
        char ip[INET6_ADDRSTRLEN];
        if (!inet_ntop(AF_INET6, a, ip, sizeof ip)) return false;
        snprintf(host, 256, "[%s]", ip);
        return true;
    }
    if (inet_pton(AF_INET, bare, a) == 1) {
        return inet_ntop(AF_INET, a, host, 256) != NULL;
    }
    usize n = strlen(bare);
    if (n && bare[n - 1] == '.') bare[--n] = '\0';
    if (!n || n > 253) return false;
    usize label = 0;
    for (usize i = 0; i <= n; i++) {
        u8 c = (u8)bare[i];
        if (!c || c == '.') {
            if (!label || label > 63 || bare[i - label] == '-' || bare[i - 1] == '-') return false;
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
            label++;
        }
    }
    snprintf(host, 256, "%s", bare);
    return true;
}

static bool parse_rule_url(const char *input, bool rule, BrowserRule *r) {
    char text[2048];
    memset(r, 0, sizeof *r);
    if (!trim_copy(input, text, sizeof text)) return false;
    const char *authority = text;
    if (strncasecmp(text, "http://", 7) == 0) { r->scheme = 1; authority += 7; }
    else if (strncasecmp(text, "https://", 8) == 0) { r->scheme = 2; authority += 8; }
    else if (!rule) return false;
    const char *end = authority + strcspn(authority, "/?#");
    if (!r->scheme && *end) return false;    /* paths require a full URL */
    if (rule && strncmp(authority, "*.", 2) == 0) {
        r->wildcard = true; authority += 2;
    }
    char url[600];
    usize n = (usize)(end - authority);
    if (!n || n > 500) return false;
    int wrote = snprintf(url, sizeof url, "%s%.*s", r->scheme == 2 ? "https://" : "http://", (int)n, authority);
    if (wrote < 0 || (usize)wrote >= sizeof url || !browser_nav_host(url, r->host, sizeof r->host) ||
        !canonical_host(r->host)) return false;
    const char *port = NULL;
    if (*authority == '[') {
        const char *rb = memchr(authority, ']', n);
        if (rb && rb + 1 < end && rb[1] == ':') port = rb + 2;
    } else {
        const char *colon = memchr(authority, ':', n);
        if (colon) port = colon + 1;
    }
    if (port) {
        for (; port < end; port++) r->port = r->port * 10 + (u32)(*port - '0');
    } else if (r->scheme) r->port = r->scheme == 2 ? 443 : 80;
    if (r->wildcard) {
        u8 ip[16];
        if (!strchr(r->host, '.') || r->host[0] == '[' || inet_pton(AF_INET, r->host, ip) == 1) return false;
    }
    return true;
}

bool browser_nav_rule_normalize(const char *input, char *out, usize cap) {
    if (!out || !cap) return false;
    out[0] = '\0';
    BrowserRule r;
    if (!parse_rule_url(input, true, &r)) return false;
    char port[16] = "";
    if (r.port && !(r.scheme == 1 && r.port == 80) && !(r.scheme == 2 && r.port == 443))
        snprintf(port, sizeof port, ":%u", r.port);
    int n = snprintf(out, cap, "%s%s%s%s", r.scheme == 2 ? "https://" : r.scheme == 1 ? "http://" : "",
                     r.wildcard ? "*." : "", r.host, port);
    if (n <= 0 || (usize)n >= cap) { out[0] = '\0'; return false; }
    return true;
}

bool browser_nav_allowlist_add(BrowserAllowlist *list, const char *input) {
    if (!list || list->count < 0 || list->count >= BROWSER_ALLOWLIST_MAX) return false;
    char rule[BROWSER_RULE_CAP];
    if (!browser_nav_rule_normalize(input, rule, sizeof rule)) return false;
    for (i32 i = 0; i < list->count; i++) {
        char existing[BROWSER_RULE_CAP];
        if (browser_nav_rule_normalize(list->rules[i], existing, sizeof existing) && strcmp(existing, rule) == 0)
            return false;
    }
    snprintf(list->rules[list->count++], BROWSER_RULE_CAP, "%s", rule);
    return true;
}

bool browser_nav_allowlist_remove(BrowserAllowlist *list, i32 index) {
    if (!list || list->count < 0 || list->count > BROWSER_ALLOWLIST_MAX || index < 0 || index >= list->count) return false;
    memmove(list->rules[index], list->rules[index + 1], (usize)(list->count - index - 1) * BROWSER_RULE_CAP);
    memset(list->rules[--list->count], 0, BROWSER_RULE_CAP);
    return true;
}

bool browser_nav_allowlist_matches(const BrowserAllowlist *list, const char *url) {
    if (!list || list->count <= 0 || list->count > BROWSER_ALLOWLIST_MAX) return false;
    BrowserRule target;
    if (!parse_rule_url(url, false, &target)) return false;
    for (i32 i = 0; i < list->count; i++) {
        BrowserRule r;
        if (strnlen(list->rules[i], BROWSER_RULE_CAP) >= BROWSER_RULE_CAP ||
            !parse_rule_url(list->rules[i], true, &r)) continue;
        if ((r.scheme && r.scheme != target.scheme) || (r.port && r.port != target.port)) continue;
        if (!r.wildcard && strcmp(r.host, target.host) == 0) return true;
        usize hn = strlen(target.host), rn = strlen(r.host);
        if (r.wildcard && hn > rn + 1 && target.host[hn - rn - 1] == '.' &&
            strcmp(target.host + hn - rn, r.host) == 0) return true;
    }
    return false;
}

bool browser_nav_is_allowed_with_list(const char *url, const BrowserAllowlist *list) {
    return browser_nav_is_allowed(url) || browser_nav_allowlist_matches(list, url);
}

bool browser_nav_is_allowed_resolved_with_list(const char *url, const BrowserAllowlist *list) {
    return browser_nav_allowlist_matches(list, url) || browser_nav_is_allowed_resolved(url);
}

BrowserNavAction browser_nav_decide_with_list(const char *url, BrowserNavClass cls,
                                             bool main_frame, bool link_click,
                                             const BrowserAllowlist *list) {
    if (browser_nav_allowlist_matches(list, url)) return BROWSER_NAV_ALLOW;
    return browser_nav_decide(url, cls, main_frame, link_click);
}

bool browser_nav_normalize(const char *in, char *out, usize out_cap) {
    if (!out || !out_cap) return false;
    out[0] = '\0';
    char text[2048], url[2048];
    if (!trim_copy(in, text, sizeof text)) return false;
    int wrote;
    if (strncasecmp(text, "http://", 7) == 0 || strncasecmp(text, "https://", 8) == 0) {
        usize scheme_n = tolower((u8)text[4]) == 's' ? 8 : 7;
        wrote = snprintf(url, sizeof url, "%s%s", scheme_n == 8 ? "https://" : "http://", text + scheme_n);
    } else {
        if (strstr(text, "://") || strncasecmp(text, "javascript:", 11) == 0 ||
            strncasecmp(text, "data:", 5) == 0 || strncasecmp(text, "about:", 6) == 0 ||
            strncasecmp(text, "file:", 5) == 0 || strncasecmp(text, "blob:", 5) == 0) return false;
        const char *port = text[0] == ':' ? text + 1 : text;
        usize digits = strspn(port, "0123456789");
        bool shorthand = digits && (!port[digits] || port[digits] == '/');
        if (text[0] == ':' && !shorthand) return false;
        if (shorthand) {
            wrote = snprintf(url, sizeof url, "http://localhost:%.*s%s", (int)digits, port,
                             port[digits] ? port + digits : "/");
        } else {
            /* Explicit ports and local names are usually dev servers. Public
             * domain names without a port default to HTTPS. */
            char host[256], probe[2048];
            wrote = snprintf(probe, sizeof probe, "http://%s", text);
            if (wrote < 0 || (usize)wrote >= sizeof probe || !browser_nav_host(probe, host, sizeof host)) return false;
            usize hn = strlen(host);
            bool local_name = !strchr(host, '.') || (hn >= 6 && strcasecmp(host + hn - 6, ".local") == 0) ||
                              browser_nav_host_is_loopback(host);
            BrowserNavClass cls = browser_nav_host_classify(host);
            const char *end = text + strcspn(text, "/?#");
            bool explicit_port = text[0] == '[' ? (memchr(text, ']', (usize)(end - text)) != end - 1) :
                                 memchr(text, ':', (usize)(end - text)) != NULL;
            const char *scheme = local_name || explicit_port || cls != BROWSER_NAV_NEEDS_DNS ? "http://" : "https://";
            wrote = snprintf(url, sizeof url, "%s%s", scheme, text);
        }
    }
    char host[256];
    if (wrote <= 0 || (usize)wrote >= sizeof url || !browser_nav_host(url, host, sizeof host) ||
        strlen(url) >= out_cap) return false;
    memcpy(out, url, strlen(url) + 1);
    return true;
}

/* Why a typed address will not open, or NULL when it will (or nothing was
 * typed). Every input the normalizer turns down — a `file:` path, a
 * `javascript:` line, another scheme, a user name in the address, a port past
 * 65535, words with a space — gets a reason, as does a host the allowlist
 * refuses. */
const char *browser_nav_typed_refusal(const char *typed, const BrowserAllowlist *list) {
    if (!typed) return NULL;
    while (isspace((u8)*typed)) typed++;
    if (!*typed) return NULL;
    char nav[2048];                    /* browser_nav_normalize's own bound */
    if (browser_nav_normalize(typed, nav, sizeof nav))
        return browser_nav_is_allowed_with_list(nav, list)
            ? NULL : "Add this site in Settings > Browser > Allowed sites";
    static const char *const schemes[] = { "javascript:", "data:", "about:", "file:", "blob:" };
    bool other_scheme = false;
    const char *sep = strstr(typed, "://");
    if (sep && strncasecmp(typed, "http://", 7) != 0 && strncasecmp(typed, "https://", 8) != 0)
        other_scheme = true;
    for (usize i = 0; i < sizeof schemes / sizeof schemes[0]; i++)
        if (strncasecmp(typed, schemes[i], strlen(schemes[i])) == 0) other_scheme = true;
    if (other_scheme) return "The browser opens http and https addresses only";
    const char *auth = sep ? sep + 3 : typed;
    const char *auth_end = auth + strcspn(auth, "/?#");
    if (memchr(auth, '@', (usize)(auth_end - auth)))
        return "An address with a user name or password is not opened";
    return "Not a web address the browser can open";
}
