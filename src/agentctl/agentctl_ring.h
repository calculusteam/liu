/*
 * Liu — a job's log as the job tabs' hover and the chat read it.
 *
 * The newest lines of one job (a delegated agent, or a subagent an agent in a
 * pane started), bounded by count and by bytes; the oldest go first and are
 * counted. Each line carries its style (AgentctlStyle) and no escape
 * sequence. Not locked: the owner's lock covers it.
 */
#ifndef AGENTCTL_RING_H
#define AGENTCTL_RING_H

#include "core/types.h"
#include "agentctl/agentctl.h"

#include <stdlib.h>
#include <string.h>

#define AGENTCTL_RING_LINES    256
#define AGENTCTL_RING_BYTES    (128u * 1024u)
#define AGENTCTL_RING_LINE_MAX 2048u

typedef struct {
    char *text;
    u32   len;
    u8    style;
} AgentctlRingLine;

typedef struct {
    AgentctlRingLine l[AGENTCTL_RING_LINES];
    u32 head;        /* index of the oldest */
    u32 count;
    u32 bytes;
    u32 dropped;     /* lines evicted from the front */
} AgentctlRing;

static inline void agentctl_ring_free(AgentctlRing *r) {
    for (u32 i = 0; i < r->count; i++) free(r->l[(r->head + i) % AGENTCTL_RING_LINES].text);
    memset(r, 0, sizeof *r);
}

static inline void agentctl_ring_drop_oldest(AgentctlRing *r) {
    if (r->count == 0) return;
    AgentctlRingLine *o = &r->l[r->head];
    r->bytes -= o->len;
    free(o->text);
    o->text = NULL;
    r->head = (r->head + 1) % AGENTCTL_RING_LINES;
    r->count--;
    r->dropped++;
}

/* One stored line: cut on a character boundary when it is longer than the
 * ring keeps one. */
static inline void agentctl_ring_push_one(AgentctlRing *r, u8 style, const char *s, u32 n) {
    bool cut = false;
    if (n > AGENTCTL_RING_LINE_MAX) {
        n = AGENTCTL_RING_LINE_MAX;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        cut = true;
    }
    u32 len = n + (cut ? 3u : 0u);
    char *t = malloc(len + 1);
    if (!t) return;
    memcpy(t, s, n);
    if (cut) memcpy(t + n, "\xE2\x80\xA6", 3);
    t[len] = '\0';
    while (r->count > 0 && (r->count == AGENTCTL_RING_LINES || r->bytes + len > AGENTCTL_RING_BYTES))
        agentctl_ring_drop_oldest(r);
    AgentctlRingLine *d = &r->l[(r->head + r->count) % AGENTCTL_RING_LINES];
    d->text = t;
    d->len = len;
    d->style = style;
    r->count++;
    r->bytes += len;
}

/* A logical line: one stored line per line of its text. */
static inline void agentctl_ring_push(AgentctlRing *r, u8 style, const u8 *s, u32 n) {
    u32 i = 0;
    while (i <= n) {
        const u8 *nl = i < n ? memchr(s + i, '\n', n - i) : NULL;
        u32 piece = nl ? (u32)(nl - (s + i)) : n - i;
        agentctl_ring_push_one(r, style, (const char *)s + i, piece);
        if (!nl) break;
        i += piece + 1;
        if (i == n) break;   /* a trailing newline opens no empty line */
    }
}

static inline const AgentctlRingLine *agentctl_ring_at(const AgentctlRing *r, u32 i) {
    return &r->l[(r->head + i) % AGENTCTL_RING_LINES];
}

/* A card's lines: the newest AGENTCTL_CARD_LINES, oldest first, each cut on
 * a character boundary to fit and ending in an ellipsis where it was cut. */
static inline void agentctl_ring_card_lines(const AgentctlRing *ring, AgentctlCard *c) {
    u32 take = ring->count < AGENTCTL_CARD_LINES ? ring->count : AGENTCTL_CARD_LINES;
    for (u32 k = 0; k < take; k++) {
        const AgentctlRingLine *rl = agentctl_ring_at(ring, ring->count - take + k);
        AgentctlCardLine *cl = &c->lines[k];
        cl->style = rl->style;
        usize len = rl->len;
        bool cut = len > AGENTCTL_CARD_LINE_CAP - 1;
        if (cut) {
            len = AGENTCTL_CARD_LINE_CAP - 1 - 3;
            while (len > 0 && ((unsigned char)rl->text[len] & 0xC0) == 0x80) len--;
        }
        memcpy(cl->text, rl->text, len);
        if (cut) { memcpy(cl->text + len, "\xE2\x80\xA6", 3); len += 3; }
        cl->text[len] = '\0';
    }
    c->nlines = (i32)take;
}

/* The whole ring, copied into a snapshot's `lines` and `bytes` (malloc'd;
 * agentctl_snapshot_free). False when memory ran out: then neither is set. */
static inline bool agentctl_ring_snapshot_lines(const AgentctlRing *ring, AgentctlSnapshot *out) {
    u32 n = ring->count;
    out->dropped = ring->dropped;
    out->lines = n ? calloc(n, sizeof *out->lines) : NULL;
    out->bytes = malloc(ring->bytes + 1);
    if ((n && !out->lines) || !out->bytes) {
        free(out->lines);
        free(out->bytes);
        out->lines = NULL;
        out->bytes = NULL;
        out->nlines = 0;
        return false;
    }
    u32 off = 0;
    for (u32 i = 0; i < n; i++) {
        const AgentctlRingLine *rl = agentctl_ring_at(ring, i);
        memcpy(out->bytes + off, rl->text, rl->len);
        out->lines[i].style = rl->style;
        out->lines[i].off = off;
        out->lines[i].len = rl->len;
        off += rl->len;
    }
    out->bytes[off] = '\0';
    out->nlines = (i32)n;
    return true;
}

#endif /* AGENTCTL_RING_H */
