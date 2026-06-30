/*
 * Liu — authenticated encryption for vault secrets
 *
 * Crypto backend: OpenSSL EVP. On macOS it comes from the vendored
 * third_party/macos-deps build (shared with libssh2); on Linux from the
 * system libssl/libcrypto. An earlier revision used CommonCrypto's SPI
 * `CCCryptorGCMOneshot{Encrypt,Decrypt}`, but on macOS 26+ those return
 * `kCCUnimplemented` and the public CommonCrypto header no longer exposes
 * `kCCModeGCM`. OpenSSL is already in the link graph for libssh2 and has
 * been stable for decades.
 *
 * A platform without OpenSSL linked fails loud: a previous XOR fallback was
 * removed in an audit — advertising encryption you do not provide is worse
 * than failing loud.
 */
#include "core/types.h"
#include "vault/crypto.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(PLATFORM_MACOS) || defined(PLATFORM_LINUX)
    #include <openssl/evp.h>
    #include <openssl/rand.h>
    #include <openssl/err.h>
#endif

/* =========================================================================
 * Random bytes
 * ========================================================================= */

bool crypto_random(u8 *buf, usize len) {
    if (!buf || len == 0) return false;
#if defined(PLATFORM_MACOS) || defined(PLATFORM_LINUX)
    return RAND_bytes(buf, (int)len) == 1;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    bool ok = fread(buf, 1, len, f) == len;
    fclose(f);
    return ok;
#endif
}

/* =========================================================================
 * Constant-time zero
 *
 * `memset(p, 0, n)` on a local buffer is legally optimised away by the
 * compiler once the buffer is dead. A `volatile` pointer loop forces
 * every write to happen. We avoid Annex-K `memset_s` because its
 * feature-test requirements are inconsistent across SDK revisions.
 * ========================================================================= */

void crypto_secure_zero(void *p, usize len) {
    if (!p || len == 0) return;
    volatile u8 *v = (volatile u8 *)p;
    while (len--) *v++ = 0;
}

/* =========================================================================
 * PBKDF2-HMAC-SHA256 (parameterized)
 * ========================================================================= */

/* Sane bounds for a DB-sourced PBKDF2 iteration count. The value lives in a
 * vault_meta row (u32) and is fed to APIs that take a signed `int` — an
 * untrusted/corrupt count above INT_MAX would cast to a negative iteration
 * count. We also reject absurdly low counts. The upper bound is generous
 * (~83x the 600k default) so legitimate future OWASP bumps are not weakened. */
#define CRYPTO_KDF_MIN_ITERATIONS 1000u
#define CRYPTO_KDF_MAX_ITERATIONS 50000000u

bool crypto_kdf_pbkdf2(const char *password,
                       const u8 *salt, usize salt_len,
                       u32 iterations,
                       u8 *key_out, usize key_len) {
    if (!password || !salt || !key_out) return false;
    if (salt_len == 0 || key_len == 0 || iterations == 0) return false;
    /* Clamp/validate the (possibly DB-sourced) count before it is cast to a
     * signed int below — a huge u32 would otherwise become negative. */
    if (iterations < CRYPTO_KDF_MIN_ITERATIONS ||
        iterations > CRYPTO_KDF_MAX_ITERATIONS) return false;
#if defined(PLATFORM_MACOS) || defined(PLATFORM_LINUX)
    return PKCS5_PBKDF2_HMAC(
        password, (int)strlen(password),
        salt, (int)salt_len,
        (int)iterations,
        EVP_sha256(),
        (int)key_len, key_out) == 1;
#else
    (void)password; (void)iterations;
    fprintf(stderr,
            "vault/crypto: PBKDF2 not available on this platform — "
            "link libsodium or OpenSSL.\n");
    return false;
#endif
}

/* =========================================================================
 * AES-256-GCM AEAD primitives
 * ========================================================================= */

#if defined(PLATFORM_MACOS) || defined(PLATFORM_LINUX)
/* Shared boilerplate for setting up a GCM cipher context. Returns NULL on
 * failure; caller must `EVP_CIPHER_CTX_free` on success. */
static EVP_CIPHER_CTX *gcm_init_ctx(const u8 *key, const u8 *nonce, bool encrypt) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return NULL;

    const EVP_CIPHER *cipher = EVP_aes_256_gcm();
    int init_fn = encrypt
        ? EVP_EncryptInit_ex(ctx, cipher, NULL, NULL, NULL)
        : EVP_DecryptInit_ex(ctx, cipher, NULL, NULL, NULL);
    if (init_fn != 1) goto fail;

    /* Explicitly set the IV length to 12 bytes (GCM default is 12, but we
     * set it anyway for clarity and forwards-compat). */
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                            (int)CRYPTO_AEAD_NONCE_LEN, NULL) != 1) goto fail;

    int set_fn = encrypt
        ? EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce)
        : EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce);
    if (set_fn != 1) goto fail;

    return ctx;
fail:
    EVP_CIPHER_CTX_free(ctx);
    return NULL;
}
#endif

bool crypto_aead_encrypt(const u8 *key,
                         const u8 *nonce,
                         const u8 *aad, usize aad_len,
                         const u8 *pt, usize pt_len,
                         u8 *ct_out,
                         u8 *tag_out) {
    if (!key || !nonce || !tag_out) return false;
    if (pt_len > 0 && (!pt || !ct_out)) return false;
    if (aad_len > 0 && !aad) return false;
#if defined(PLATFORM_MACOS) || defined(PLATFORM_LINUX)
    EVP_CIPHER_CTX *ctx = gcm_init_ctx(key, nonce, true);
    if (!ctx) return false;

    int out_len = 0;
    bool ok = true;

    /* AAD first (no output) */
    if (aad_len > 0) {
        if (EVP_EncryptUpdate(ctx, NULL, &out_len, aad, (int)aad_len) != 1) {
            ok = false;
        }
    }

    /* Plaintext → ciphertext */
    if (ok && pt_len > 0) {
        if (EVP_EncryptUpdate(ctx, ct_out, &out_len, pt, (int)pt_len) != 1) {
            ok = false;
        }
    }

    /* Finalize (GCM writes nothing here but must be called) */
    if (ok) {
        int final_len = 0;
        u8 dummy[16];  /* GCM writes 0 bytes but OpenSSL expects a buffer */
        if (EVP_EncryptFinal_ex(ctx, dummy, &final_len) != 1) {
            ok = false;
        }
    }

    /* Extract tag */
    if (ok) {
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                                (int)CRYPTO_AEAD_TAG_LEN, tag_out) != 1) {
            ok = false;
        }
    }

    EVP_CIPHER_CTX_free(ctx);
    return ok;
#else
    (void)aad; (void)aad_len; (void)pt; (void)pt_len; (void)ct_out;
    fprintf(stderr,
            "vault/crypto: AES-256-GCM not available on this platform — "
            "refusing to pretend-encrypt.\n");
    return false;
#endif
}

bool crypto_aead_decrypt(const u8 *key,
                         const u8 *nonce,
                         const u8 *aad, usize aad_len,
                         const u8 *ct, usize ct_len,
                         const u8 *tag,
                         u8 *pt_out) {
    if (!key || !nonce || !tag) return false;
    if (ct_len > 0 && (!ct || !pt_out)) return false;
    if (aad_len > 0 && !aad) return false;
#if defined(PLATFORM_MACOS) || defined(PLATFORM_LINUX)
    EVP_CIPHER_CTX *ctx = gcm_init_ctx(key, nonce, false);
    if (!ctx) return false;

    int out_len = 0;
    bool ok = true;

    if (aad_len > 0) {
        if (EVP_DecryptUpdate(ctx, NULL, &out_len, aad, (int)aad_len) != 1) {
            ok = false;
        }
    }

    if (ok && ct_len > 0) {
        if (EVP_DecryptUpdate(ctx, pt_out, &out_len, ct, (int)ct_len) != 1) {
            ok = false;
        }
    }

    /* Supply the tag before finalizing — this is how OpenSSL's GCM
     * interface surfaces the tag check: Final returns 1 iff the tag
     * verifies. */
    if (ok) {
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                                (int)CRYPTO_AEAD_TAG_LEN,
                                (void *)tag) != 1) {
            ok = false;
        }
    }

    if (ok) {
        int final_len = 0;
        u8 dummy[16];
        if (EVP_DecryptFinal_ex(ctx, dummy, &final_len) != 1) {
            ok = false;   /* tag mismatch or other failure */
        }
    }

    EVP_CIPHER_CTX_free(ctx);
    if (!ok && pt_out && ct_len > 0) {
        /* Scrub any partial plaintext so callers can't accidentally
         * consume unauthenticated bytes. */
        crypto_secure_zero(pt_out, ct_len);
    }
    return ok;
#else
    (void)aad; (void)aad_len; (void)ct; (void)ct_len; (void)pt_out;
    fprintf(stderr,
            "vault/crypto: AES-256-GCM not available on this platform — "
            "refusing to pretend-decrypt.\n");
    return false;
#endif
}

/* =========================================================================
 * DEK wrap / unwrap
 *
 * AAD layout = [version(1) | iter_be(4) | salt(salt_len)]
 *
 * Binding iter + salt authenticates the KDF parameters: a rollback of
 * `vault_meta.kdf_iterations` makes the AAD mismatch at unwrap time.
 * ========================================================================= */

static usize build_wrap_aad(u8 *out, usize cap,
                            u32 iterations,
                            const u8 *salt, usize salt_len) {
    usize need = 1 + 4 + salt_len;
    if (cap < need) return 0;
    out[0] = CRYPTO_DEK_WRAP_VERSION;
    out[1] = (u8)((iterations >> 24) & 0xFF);
    out[2] = (u8)((iterations >> 16) & 0xFF);
    out[3] = (u8)((iterations >> 8)  & 0xFF);
    out[4] = (u8)( iterations        & 0xFF);
    memcpy(out + 5, salt, salt_len);
    return need;
}

bool crypto_wrap_dek(const u8 *kek,
                     const u8 *dek,
                     u32 iterations,
                     const u8 *salt, usize salt_len,
                     u8 *blob_out, usize blob_cap) {
    if (!kek || !dek || !salt || !blob_out) return false;
    if (salt_len == 0 || iterations == 0) return false;
    if (blob_cap < CRYPTO_WRAPPED_DEK_LEN) return false;

    u8 aad[1 + 4 + 64];
    if (salt_len > 64) return false;
    usize aad_len = build_wrap_aad(aad, sizeof aad, iterations, salt, salt_len);
    if (aad_len == 0) return false;

    u8 *nonce   = blob_out;
    u8 *tag_out = blob_out + CRYPTO_AEAD_NONCE_LEN;
    u8 *ct_out  = blob_out + CRYPTO_AEAD_NONCE_LEN + CRYPTO_AEAD_TAG_LEN;

    if (!crypto_random(nonce, CRYPTO_AEAD_NONCE_LEN)) {
        crypto_secure_zero(aad, aad_len);
        return false;
    }

    bool ok = crypto_aead_encrypt(kek, nonce,
                                  aad, aad_len,
                                  dek, CRYPTO_KEY_LEN,
                                  ct_out,
                                  tag_out);
    crypto_secure_zero(aad, aad_len);
    if (!ok) {
        crypto_secure_zero(blob_out, CRYPTO_WRAPPED_DEK_LEN);
        return false;
    }
    return true;
}

bool crypto_unwrap_dek(const u8 *kek,
                       const u8 *blob, usize blob_len,
                       u32 iterations,
                       const u8 *salt, usize salt_len,
                       u8 *dek_out) {
    if (!kek || !blob || !salt || !dek_out) return false;
    if (salt_len == 0 || iterations == 0) return false;
    if (blob_len != CRYPTO_WRAPPED_DEK_LEN) return false;

    u8 aad[1 + 4 + 64];
    if (salt_len > 64) return false;
    usize aad_len = build_wrap_aad(aad, sizeof aad, iterations, salt, salt_len);
    if (aad_len == 0) return false;

    const u8 *nonce = blob;
    const u8 *tag   = blob + CRYPTO_AEAD_NONCE_LEN;
    const u8 *ct    = blob + CRYPTO_AEAD_NONCE_LEN + CRYPTO_AEAD_TAG_LEN;

    bool ok = crypto_aead_decrypt(kek, nonce,
                                  aad, aad_len,
                                  ct, CRYPTO_KEY_LEN,
                                  tag,
                                  dek_out);
    crypto_secure_zero(aad, aad_len);
    if (!ok) {
        crypto_secure_zero(dek_out, CRYPTO_KEY_LEN);
        return false;
    }
    return true;
}

/* =========================================================================
 * Legacy password-wrapped one-shot API
 *
 * Re-implemented on top of the primitives above with empty AAD so there
 * is a single crypto code path.
 * ========================================================================= */

#define LEGACY_SALT_LEN    CRYPTO_KDF_SALT_LEN
#define LEGACY_KEY_LEN     CRYPTO_KEY_LEN
#define LEGACY_IV_LEN      CRYPTO_AEAD_NONCE_LEN
#define LEGACY_TAG_LEN     CRYPTO_AEAD_TAG_LEN
#define LEGACY_ITERATIONS  CRYPTO_DEFAULT_KDF_ITERATIONS

u8 *crypto_encrypt(const u8 *plaintext, usize pt_len,
                   const char *password, usize *out_len) {
    if (!plaintext || !password || !out_len) return NULL;

    u8 salt[LEGACY_SALT_LEN], iv[LEGACY_IV_LEN], key[LEGACY_KEY_LEN];
    if (!crypto_random(salt, LEGACY_SALT_LEN)) return NULL;
    if (!crypto_random(iv, LEGACY_IV_LEN))     return NULL;
    if (!crypto_kdf_pbkdf2(password, salt, LEGACY_SALT_LEN,
                           LEGACY_ITERATIONS, key, LEGACY_KEY_LEN)) {
        return NULL;
    }

    usize total = LEGACY_SALT_LEN + LEGACY_IV_LEN + LEGACY_TAG_LEN + pt_len;
    u8 *output = malloc(total);
    if (!output) { crypto_secure_zero(key, LEGACY_KEY_LEN); return NULL; }

    memcpy(output, salt, LEGACY_SALT_LEN);
    memcpy(output + LEGACY_SALT_LEN, iv, LEGACY_IV_LEN);
    u8 *tag_out = output + LEGACY_SALT_LEN + LEGACY_IV_LEN;
    u8 *ct_out  = output + LEGACY_SALT_LEN + LEGACY_IV_LEN + LEGACY_TAG_LEN;

    bool ok = crypto_aead_encrypt(key, iv, NULL, 0,
                                  plaintext, pt_len,
                                  ct_out, tag_out);
    crypto_secure_zero(key, LEGACY_KEY_LEN);
    if (!ok) { free(output); return NULL; }

    *out_len = total;
    return output;
}

u8 *crypto_decrypt(const u8 *encrypted, usize enc_len,
                   const char *password, usize *out_len) {
    if (!encrypted || !password || !out_len) return NULL;
    const usize prefix = LEGACY_SALT_LEN + LEGACY_IV_LEN + LEGACY_TAG_LEN;
    if (enc_len < prefix) return NULL;

    const u8 *salt = encrypted;
    const u8 *iv   = encrypted + LEGACY_SALT_LEN;
    const u8 *tag  = encrypted + LEGACY_SALT_LEN + LEGACY_IV_LEN;
    const u8 *ct   = encrypted + prefix;
    usize ct_len   = enc_len - prefix;

    u8 key[LEGACY_KEY_LEN];
    if (!crypto_kdf_pbkdf2(password, salt, LEGACY_SALT_LEN,
                           LEGACY_ITERATIONS, key, LEGACY_KEY_LEN)) {
        return NULL;
    }

    /* Extra byte for convenience NUL terminator. */
    u8 *plaintext = malloc(ct_len + 1);
    if (!plaintext) { crypto_secure_zero(key, LEGACY_KEY_LEN); return NULL; }

    bool ok = crypto_aead_decrypt(key, iv, NULL, 0,
                                  ct, ct_len, tag,
                                  plaintext);
    crypto_secure_zero(key, LEGACY_KEY_LEN);
    if (!ok) {
        free(plaintext);
        return NULL;
    }
    plaintext[ct_len] = '\0';
    *out_len = ct_len;
    return plaintext;
}
