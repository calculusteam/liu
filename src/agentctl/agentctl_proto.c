/*
 * Liu — agent-control frames: validation, writing, incremental reading.
 */
#include "agentctl/agentctl_proto.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    u32 magic;
    u16 version;
    u16 op;
    u32 job;
    u32 len;
} AgentctlHeaderWire;
_Static_assert(sizeof(AgentctlHeaderWire) == AGENTCTL_HEADER_SIZE, "header is 16 bytes");

bool agentctl_header_parse(const u8 *hdr, u16 *op, u32 *job, u32 *len) {
    if (!hdr) return false;
    AgentctlHeaderWire h;
    memcpy(&h, hdr, sizeof h);
    if (h.magic != AGENTCTL_MAGIC || h.version != AGENTCTL_VERSION) return false;
    if (h.len > AGENTCTL_MAX_PAYLOAD || h.op == AGOP_NONE) return false;
    if (op)  *op = h.op;
    if (job) *job = h.job;
    if (len) *len = h.len;
    return true;
}

bool agentctl_payload_valid(const u8 *payload, u32 len) {
    if (len && !payload) return false;
    u32 pos = 0, count = 0;
    while (pos < len) {
        if (len - pos < AGENTCTL_FIELD_HEAD) return false;
        u32 flen;
        memcpy(&flen, payload + pos + 2, sizeof flen);
        pos += AGENTCTL_FIELD_HEAD;
        if (flen > len - pos) return false;
        pos += flen;
        if (++count > AGENTCTL_MAX_FIELDS) return false;
    }
    return true;
}

bool agentctl_validate(const u8 *buf, usize len, AgentctlFrame *out) {
    if (!buf || len < AGENTCTL_HEADER_SIZE) return false;
    u16 op; u32 job, plen;
    if (!agentctl_header_parse(buf, &op, &job, &plen)) return false;
    if ((usize)plen != len - AGENTCTL_HEADER_SIZE) return false;
    const u8 *payload = buf + AGENTCTL_HEADER_SIZE;
    if (!agentctl_payload_valid(payload, plen)) return false;
    if (out) {
        out->op = op;
        out->job = job;
        out->payload = payload;
        out->len = plen;
    }
    return true;
}

bool agentctl_next(const AgentctlFrame *f, u32 *pos, u16 *key, const u8 **data, u32 *len) {
    if (!f || !pos || *pos >= f->len || f->len - *pos < AGENTCTL_FIELD_HEAD) return false;
    u16 k; u32 n;
    memcpy(&k, f->payload + *pos, sizeof k);
    memcpy(&n, f->payload + *pos + 2, sizeof n);
    u32 at = *pos + AGENTCTL_FIELD_HEAD;
    if (n > f->len - at) return false;
    if (key)  *key = k;
    if (data) *data = f->payload + at;
    if (len)  *len = n;
    *pos = at + n;
    return true;
}

bool agentctl_get(const AgentctlFrame *f, u16 key, const u8 **data, u32 *len) {
    u32 pos = 0;
    u16 k; const u8 *d; u32 n;
    while (agentctl_next(f, &pos, &k, &d, &n)) {
        if (k != key) continue;
        if (data) *data = d;
        if (len)  *len = n;
        return true;
    }
    return false;
}

bool agentctl_get_str(const AgentctlFrame *f, u16 key, char *out, usize cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    const u8 *d; u32 n;
    if (!agentctl_get(f, key, &d, &n)) return false;
    if ((usize)n >= cap || (n && memchr(d, '\0', n))) return false;
    if (n) memcpy(out, d, n);
    out[n] = '\0';
    return true;
}

bool agentctl_get_u64(const AgentctlFrame *f, u16 key, u64 *out) {
    char s[24];
    if (!agentctl_get_str(f, key, s, sizeof s) || !s[0]) return false;
    u64 v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        u64 d = (u64)(*p - '0');
        if (v > (UINT64_MAX - d) / 10u) return false;
        v = v * 10u + d;
    }
    if (out) *out = v;
    return true;
}

/* ------------------------------------------------------------- writing --- */

static bool msg_reserve(AgentctlMsg *m, u32 more) {
    if (m->failed) return false;
    u32 limit = AGENTCTL_HEADER_SIZE + AGENTCTL_MAX_PAYLOAD;
    if (more > limit - m->len) { m->failed = true; return false; }
    u32 want = m->len + more;
    if (want <= m->cap) return true;
    u32 cap = m->cap ? m->cap : 256u;
    while (cap < want) cap = (cap > limit / 2u) ? limit : cap * 2u;
    u8 *nb = realloc(m->buf, cap);
    if (!nb) { m->failed = true; return false; }
    m->buf = nb;
    m->cap = cap;
    return true;
}

bool agentctl_msg_init(AgentctlMsg *m, u16 op, u32 job) {
    if (!m) return false;
    memset(m, 0, sizeof *m);
    if (!msg_reserve(m, AGENTCTL_HEADER_SIZE)) return false;
    AgentctlHeaderWire h = { AGENTCTL_MAGIC, AGENTCTL_VERSION, op, job, 0 };
    memcpy(m->buf, &h, sizeof h);
    m->len = AGENTCTL_HEADER_SIZE;
    return true;
}

void agentctl_msg_put(AgentctlMsg *m, u16 key, const void *data, u32 len) {
    if (!m || m->failed) return;
    if (len && !data) { m->failed = true; return; }
    if (!msg_reserve(m, AGENTCTL_FIELD_HEAD + len)) return;
    memcpy(m->buf + m->len, &key, sizeof key);
    memcpy(m->buf + m->len + 2, &len, sizeof len);
    if (len) memcpy(m->buf + m->len + AGENTCTL_FIELD_HEAD, data, len);
    m->len += AGENTCTL_FIELD_HEAD + len;
}

void agentctl_msg_put_str(AgentctlMsg *m, u16 key, const char *s) {
    if (!s) s = "";
    usize n = strlen(s);
    if (n > AGENTCTL_MAX_PAYLOAD) { if (m) m->failed = true; return; }
    agentctl_msg_put(m, key, s, (u32)n);
}

void agentctl_msg_put_u64(AgentctlMsg *m, u16 key, u64 v) {
    char s[24];
    int n = snprintf(s, sizeof s, "%llu", (unsigned long long)v);
    agentctl_msg_put(m, key, s, (u32)n);
}

bool agentctl_msg_end(AgentctlMsg *m) {
    if (!m || m->failed || !m->buf) return false;
    u32 plen = m->len - AGENTCTL_HEADER_SIZE;
    u32 fields = 0, pos = AGENTCTL_HEADER_SIZE;
    while (pos < m->len) {
        u32 n;
        memcpy(&n, m->buf + pos + 2, sizeof n);
        pos += AGENTCTL_FIELD_HEAD + n;
        fields++;
    }
    if (fields > AGENTCTL_MAX_FIELDS) { m->failed = true; return false; }
    memcpy(m->buf + offsetof(AgentctlHeaderWire, len), &plen, sizeof plen);
    return true;
}

void agentctl_msg_free(AgentctlMsg *m) {
    if (!m) return;
    free(m->buf);
    memset(m, 0, sizeof *m);
}

/* ------------------------------------------------------------- reading --- */

void agentctl_reader_reset(AgentctlReader *r) {
    if (!r) return;
    free(r->payload);
    memset(r, 0, sizeof *r);
}

int agentctl_reader_feed(AgentctlReader *r, int fd) {
    if (!r) return -1;
    for (;;) {
        if (r->hdr_have < AGENTCTL_HEADER_SIZE) {
            ssize_t got = read(fd, r->hdr + r->hdr_have, AGENTCTL_HEADER_SIZE - r->hdr_have);
            if (got == 0) return -1;
            if (got < 0) {
                if (errno == EINTR) continue;
                return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
            }
            r->hdr_have += (u32)got;
            if (r->hdr_have < AGENTCTL_HEADER_SIZE) continue;
            if (!agentctl_header_parse(r->hdr, &r->op, &r->job, &r->need)) return -1;
            r->have = 0;
            r->payload = malloc(r->need ? r->need : 1u);
            if (!r->payload) return -1;
        }
        if (r->have < r->need) {
            ssize_t got = read(fd, r->payload + r->have, r->need - r->have);
            if (got == 0) return -1;
            if (got < 0) {
                if (errno == EINTR) continue;
                return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
            }
            r->have += (u32)got;
            if (r->have < r->need) continue;
        }
        if (!agentctl_payload_valid(r->payload, r->need)) return -1;
        return 1;
    }
}

void agentctl_reader_frame(const AgentctlReader *r, AgentctlFrame *out) {
    if (!r || !out) return;
    out->op = r->op;
    out->job = r->job;
    out->payload = r->payload;
    out->len = r->need;
}

bool agentctl_write_all(int fd, const u8 *buf, usize len) {
    usize off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd p = { .fd = fd, .events = POLLOUT };
                if (poll(&p, 1, 5000) <= 0) return false;
                continue;
            }
            return false;
        }
        off += (usize)n;
    }
    return true;
}

static i64 mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_exact(int fd, u8 *buf, u32 want, i64 deadline) {
    u32 have = 0;
    while (have < want) {
        int wait = -1;
        if (deadline >= 0) {
            i64 left = deadline - mono_ms();
            if (left <= 0) return 0;
            wait = left > 0x7fffffff ? 0x7fffffff : (int)left;
        }
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int pr = poll(&p, 1, wait);
        if (pr < 0) { if (errno == EINTR) continue; return -1; }
        if (pr == 0) return 0;
        ssize_t n = read(fd, buf + have, want - have);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        have += (u32)n;
    }
    return 1;
}

int agentctl_read_frame(int fd, i32 timeout_ms, AgentctlFrame *out, u8 **storage) {
    if (!out || !storage) return -1;
    *storage = NULL;
    i64 deadline = timeout_ms < 0 ? -1 : mono_ms() + timeout_ms;
    u8 hdr[AGENTCTL_HEADER_SIZE];
    int rc = read_exact(fd, hdr, sizeof hdr, deadline);
    if (rc <= 0) return rc;
    u16 op; u32 job, len;
    if (!agentctl_header_parse(hdr, &op, &job, &len)) return -1;
    u8 *p = malloc(len ? len : 1u);
    if (!p) return -1;
    /* The header has arrived: the rest of the frame is already on its way, so
     * it is read without the caller's deadline cutting it in half. */
    if (len && read_exact(fd, p, len, mono_ms() + 10000) != 1) { free(p); return -1; }
    if (!agentctl_payload_valid(p, len)) { free(p); return -1; }
    out->op = op;
    out->job = job;
    out->payload = p;
    out->len = len;
    *storage = p;
    return 1;
}
