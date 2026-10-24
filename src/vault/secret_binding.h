/* Authenticated, length-delimited routing stored inside a v2 secret envelope.
 * Keep it independent of mutable SQLite metadata so deleting a host does not
 * prevent manually revealing its secret. */
#ifndef LIU_SECRET_BINDING_H
#define LIU_SECRET_BINDING_H
#include "vault/vault.h"
#include <string.h>

#define SECRET_BINDING_CAP 1024u
#define SECRET_ENVELOPE_HEADER 10u
#define SECRET_ENVELOPE_MAGIC "LIUSEC2!"

typedef struct {
    char id[33];
    VaultSecretKind kind;
    char host_id[64], scope[16], env_name[64];
    char hostname[256], username[128];
    i32 port, protocol;
} SecretBinding;

static inline bool binding_text(char *out, usize cap, const char *in) {
    if (!in) in = "";
    usize n = strnlen(in, cap);
    if (n >= cap) return false;
    memcpy(out, in, n + 1);
    return true;
}

static inline bool binding_string_put(u8 *out, usize cap, usize *at, const char *s) {
    usize n = strlen(s);
    if (n > 65535u || *at > cap || cap - *at < n + 2u) return false;
    out[(*at)++] = (u8)(n >> 8); out[(*at)++] = (u8)n;
    memcpy(out + *at, s, n); *at += n;
    return true;
}

static inline bool binding_string_get(const u8 *in, usize len, usize *at,
                                      char *out, usize cap) {
    if (*at > len || len - *at < 2u) return false;
    usize n = ((usize)in[*at] << 8) | in[*at + 1]; *at += 2;
    if (n >= cap || n > len - *at || memchr(in + *at, 0, n)) return false;
    memcpy(out, in + *at, n); out[n] = 0; *at += n;
    return true;
}

static inline usize binding_encode(const SecretBinding *b, u8 *out, usize cap) {
    static const char domain[] = "liu-v2-sec:";
    usize at = sizeof domain - 1;
    if (!b || !out || cap < at + 1 || b->kind < VAULT_SECRET_PASSWORD ||
        b->kind > VAULT_SECRET_NOTE) return 0;
    memcpy(out, domain, at); out[at++] = (u8)b->kind;
    if (!binding_string_put(out, cap, &at, b->id) ||
        !binding_string_put(out, cap, &at, b->host_id) ||
        !binding_string_put(out, cap, &at, b->scope) ||
        !binding_string_put(out, cap, &at, b->env_name) ||
        !binding_string_put(out, cap, &at, b->hostname) ||
        !binding_string_put(out, cap, &at, b->username) || cap - at < 8) return 0;
    u32 nums[2] = {(u32)b->port, (u32)b->protocol};
    for (usize k = 0; k < 2; k++)
        for (i32 shift = 24; shift >= 0; shift -= 8) out[at++] = (u8)(nums[k] >> shift);
    return at;
}

static inline bool binding_decode(const u8 *in, usize len, SecretBinding *b) {
    static const char domain[] = "liu-v2-sec:";
    usize at = sizeof domain - 1;
    if (!in || !b || len < at + 1 || len > SECRET_BINDING_CAP ||
        memcmp(in, domain, at)) return false;
    memset(b, 0, sizeof *b); b->kind = (VaultSecretKind)in[at++];
    if (b->kind < VAULT_SECRET_PASSWORD || b->kind > VAULT_SECRET_NOTE ||
        !binding_string_get(in, len, &at, b->id, sizeof b->id) ||
        !binding_string_get(in, len, &at, b->host_id, sizeof b->host_id) ||
        !binding_string_get(in, len, &at, b->scope, sizeof b->scope) ||
        !binding_string_get(in, len, &at, b->env_name, sizeof b->env_name) ||
        !binding_string_get(in, len, &at, b->hostname, sizeof b->hostname) ||
        !binding_string_get(in, len, &at, b->username, sizeof b->username) ||
        len - at != 8u || strlen(b->id) != 32u) return false;
    u32 nums[2] = {0, 0};
    for (usize k = 0; k < 2; k++)
        for (usize j = 0; j < 4; j++) nums[k] = (nums[k] << 8) | in[at++];
    if (nums[0] > 65535u || nums[1] > PROTO_SERIAL) return false;
    b->port = (i32)nums[0]; b->protocol = (i32)nums[1];
    return true;
}

static inline bool binding_recipient(const SecretBinding *b, const VaultHost *h) {
    i32 port = h ? h->port : 0;
    if (h && h->protocol != PROTO_LOCAL && !port) port = 22;
    return b && h && b->host_id[0] && !strcmp(b->host_id, h->id) &&
           !strcmp(b->hostname, h->hostname) && !strcmp(b->username, h->username) &&
           b->port == port && b->protocol == (i32)h->protocol;
}
#endif
