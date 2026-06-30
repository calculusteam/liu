/*
 * Liu — authenticated encryption for vault secrets
 *
 * This header exposes three layers:
 *
 *   1. Low-level AEAD primitives (AES-256-GCM on macOS).
 *      `crypto_aead_encrypt` / `crypto_aead_decrypt` accept additional
 *      authenticated data (AAD). Smart Vault binds the row identity
 *      (id || kind || host_id || updated_at) into AAD so an attacker with
 *      DB-write access cannot move a ciphertext between rows.
 *
 *   2. Parameterized KDF (`crypto_kdf_pbkdf2`).
 *      Hard-coded iteration counts paint the codebase into a corner when
 *      OWASP guidance changes. Smart Vault uses 600_000 (2023 guidance);
 *      the value is stored in vault_meta so a future bump can migrate
 *      existing vaults without schema changes.
 *
 *   3. DEK wrap / unwrap helpers (`crypto_wrap_dek` / `crypto_unwrap_dek`).
 *      The Data-Encryption-Key is a random 32-byte secret generated once
 *      at vault init. It is wrapped with a Key-Encryption-Key derived from
 *      the user's master password. A version byte + iteration count +
 *      salt are bound as AAD so an attacker cannot roll back `vault_meta`
 *      to downgrade KDF iterations.
 *
 *   4. Constant-time zeroing (`crypto_secure_zero`).
 *      `memset(ptr, 0, n)` on a local buffer is legally a no-op under LTO
 *      once the buffer is dead. A `volatile` pointer loop survives.
 *
 * The legacy `crypto_encrypt` / `crypto_decrypt` helpers are retained for
 * callers that want a one-shot password-wrapped blob without having to
 * manage a DEK themselves. They are now thin wrappers over the primitives
 * above with empty AAD.
 *
 * Non-macOS platforms intentionally fail loud: every encrypt / decrypt
 * call returns false. The existing metadata vault (hosts, snippets …)
 * continues to work without secret storage. Link libsodium or OpenSSL to
 * populate those paths.
 *
 * On-disk format produced by `crypto_encrypt` (legacy, unchanged):
 *   [ salt(16) | iv(12) | tag(16) | ciphertext(pt_len) ]
 */
#ifndef VAULT_CRYPTO_H
#define VAULT_CRYPTO_H

#include "core/types.h"

/* =========================================================================
 * Compile-time sizes
 * ========================================================================= */

#define CRYPTO_KDF_SALT_LEN           16u
#define CRYPTO_KEY_LEN                32u   /* AES-256 */
#define CRYPTO_AEAD_NONCE_LEN         12u   /* GCM recommended nonce */
#define CRYPTO_AEAD_TAG_LEN           16u   /* GCM authentication tag */
#define CRYPTO_DEFAULT_KDF_ITERATIONS 600000u

/* Wrapped-DEK envelope: [nonce(12) | tag(16) | ct(32)] = 60 bytes. */
#define CRYPTO_WRAPPED_DEK_LEN \
    (CRYPTO_AEAD_NONCE_LEN + CRYPTO_AEAD_TAG_LEN + CRYPTO_KEY_LEN)

/* =========================================================================
 * Random + zero
 * ========================================================================= */

/* Fill `buf` with `len` cryptographically strong random bytes. */
bool crypto_random(u8 *buf, usize len);

/* Zero `len` bytes starting at `p` in a way the optimizer cannot elide.
 * Safe on NULL / zero-length input. */
void crypto_secure_zero(void *p, usize len);

/* =========================================================================
 * Key derivation
 * ========================================================================= */

/* PBKDF2-HMAC-SHA256 with caller-controlled iteration count.
 *   password     — NUL-terminated master password.
 *   salt         — random bytes (typically CRYPTO_KDF_SALT_LEN).
 *   salt_len     — length of `salt`.
 *   iterations   — must be > 0.
 *   key_out      — destination, at least `key_len` bytes.
 *   key_len      — how many bytes to derive (typically CRYPTO_KEY_LEN).
 * Returns false on invalid args or platform without real crypto. */
bool crypto_kdf_pbkdf2(const char *password,
                       const u8 *salt, usize salt_len,
                       u32 iterations,
                       u8 *key_out, usize key_len);

/* =========================================================================
 * AEAD — AES-256-GCM (authenticated encryption with associated data)
 * ========================================================================= */

/* Encrypt `pt` under `key` + `nonce`. Writes `pt_len` bytes of ciphertext
 * to `ct_out` and CRYPTO_AEAD_TAG_LEN bytes of tag to `tag_out`. `aad` /
 * `aad_len` may be NULL / 0 for no associated data. Returns false on
 * invalid args, OOM, or platform without real crypto. */
bool crypto_aead_encrypt(const u8 *key,
                         const u8 *nonce,
                         const u8 *aad, usize aad_len,
                         const u8 *pt, usize pt_len,
                         u8 *ct_out,
                         u8 *tag_out);

/* Decrypt `ct` under `key` + `nonce`, verifying `tag`. Writes `ct_len`
 * bytes of plaintext to `pt_out`. Returns false on tag mismatch (wrong
 * key / tampered input / wrong AAD). On failure, `pt_out` is zeroed. */
bool crypto_aead_decrypt(const u8 *key,
                         const u8 *nonce,
                         const u8 *aad, usize aad_len,
                         const u8 *ct, usize ct_len,
                         const u8 *tag,
                         u8 *pt_out);

/* =========================================================================
 * DEK wrap / unwrap
 *
 *   AAD is [version(1) | iter_be(4) | salt(salt_len)]. The caller stores
 *   salt, iterations, and the resulting blob separately (in vault_meta
 *   rows). A rollback of the `kdf_iterations` row by an attacker with DB
 *   write access fails AAD verification on unwrap.
 * ========================================================================= */

#define CRYPTO_DEK_WRAP_VERSION 0x01u

/* Wrap `dek` (CRYPTO_KEY_LEN bytes) under `kek` (CRYPTO_KEY_LEN bytes).
 * `blob_out` must have capacity >= CRYPTO_WRAPPED_DEK_LEN. Writes exactly
 * CRYPTO_WRAPPED_DEK_LEN bytes on success. */
bool crypto_wrap_dek(const u8 *kek,
                     const u8 *dek,
                     u32 iterations,
                     const u8 *salt, usize salt_len,
                     u8 *blob_out, usize blob_cap);

/* Unwrap a CRYPTO_WRAPPED_DEK_LEN-byte blob. The caller supplies the
 * iteration count and salt as stored in vault_meta; these are validated
 * via AAD, so a modified `iterations` row produces an unwrap failure
 * rather than a silent downgrade. On success writes CRYPTO_KEY_LEN
 * bytes to `dek_out`. */
bool crypto_unwrap_dek(const u8 *kek,
                       const u8 *blob, usize blob_len,
                       u32 iterations,
                       const u8 *salt, usize salt_len,
                       u8 *dek_out);

/* =========================================================================
 * Legacy password-wrapped one-shot API (kept for convenience / tests)
 * ========================================================================= */

/* Derive a key from `password` via PBKDF2 (CRYPTO_DEFAULT_KDF_ITERATIONS),
 * encrypt `plaintext`, and emit a self-contained blob of the format
 * [ salt(16) | iv(12) | tag(16) | ciphertext ]. Caller frees the returned
 * buffer. Returns NULL on failure. No AAD. */
u8 *crypto_encrypt(const u8 *plaintext, usize pt_len,
                   const char *password, usize *out_len);

/* Inverse of `crypto_encrypt`. Returns NULL on tag mismatch / wrong
 * password. Result has a NUL terminator past `*out_len` for convenience
 * when the plaintext is text. */
u8 *crypto_decrypt(const u8 *encrypted, usize enc_len,
                   const char *password, usize *out_len);

#endif /* VAULT_CRYPTO_H */
