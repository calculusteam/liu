/* update_pubkey.h — Ed25519 public key for verifying auto-update artifacts.
 *
 * This is the SOLE trust anchor for the auto-updater: every downloaded update
 * artifact carries a detached Ed25519 signature, and the client refuses to
 * extract/install anything that doesn't verify against this key. Compiling the
 * key in (rather than fetching it) means an attacker who controls the feed or
 * CDN still cannot ship a build this app will run.
 *
 * A public key is NOT a secret — committing it is correct and makes builds
 * reproducible and key rotation a reviewable diff. The matching PRIVATE key is
 * never in the repo; it lives offline and as the CI secret ED25519_PRIVATE_KEY.
 *
 * Regenerate with:
 *   openssl genpkey -algorithm ed25519 -out liu_update_ed25519.key
 *   openssl pkey -in liu_update_ed25519.key -pubout -outform DER | tail -c 32 | xxd -i
 * and paste the 32 bytes below. Rotation: ship a release signed by the OLD key
 * whose binary embeds the NEW key, then sign subsequent releases with the new key.
 */
#ifndef LIU_UPDATE_PUBKEY_H
#define LIU_UPDATE_PUBKEY_H

#define LIU_UPDATE_PUBKEY_LEN 32

/* Raw 32-byte Ed25519 public key (matches liu_update_ed25519.key). */
static const unsigned char LIU_UPDATE_PUBKEY[LIU_UPDATE_PUBKEY_LEN] = {
    0x56, 0x74, 0x8c, 0x7a, 0x1c, 0xe5, 0x3b, 0xdc, 0x08, 0x68, 0x16, 0x5e,
    0xe0, 0x5d, 0x47, 0x37, 0xcf, 0x70, 0x48, 0xee, 0x6f, 0xdd, 0x6e, 0x19,
    0x1b, 0xfc, 0x78, 0x17, 0x8d, 0x9c, 0xae, 0x0e
};

#endif /* LIU_UPDATE_PUBKEY_H */
