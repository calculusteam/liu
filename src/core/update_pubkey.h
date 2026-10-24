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
 * never in the repository; releases are signed offline (scripts/release.sh).
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
    0x85, 0xf5, 0x73, 0x31, 0xdf, 0xaa, 0xe5, 0x7a, 0x68, 0x6b, 0x07, 0x7a,
    0xb3, 0xed, 0xaf, 0x19, 0xd2, 0x6c, 0xbe, 0x58, 0x25, 0xed, 0x63, 0x8e,
    0xe7, 0x93, 0x67, 0xde, 0x06, 0x73, 0x6d, 0x5e
};

#endif /* LIU_UPDATE_PUBKEY_H */
