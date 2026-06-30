/*
 * Liu - SSH key generation and management
 * Generates RSA, Ed25519, ECDSA keys with optional passphrase.
 * Scans ~/.ssh/ for existing keys.
 */
#ifndef KEYGEN_H
#define KEYGEN_H

#include "core/types.h"

typedef enum {
    KEYGEN_RSA_2048,
    KEYGEN_RSA_4096,
    KEYGEN_ED25519,
    KEYGEN_ECDSA_P256,
    KEYGEN_ECDSA_P384,
    KEYGEN_ECDSA_P521,
} KeyAlgorithm;

typedef struct {
    char  public_key[4096];     /* OpenSSH format */
    char  private_key[8192];    /* PEM format */
    char  fingerprint[128];     /* SHA256 fingerprint */
    char  algorithm[32];
} GeneratedKey;

/* Info about an existing SSH key on disk */
typedef struct {
    char name[64];              /* filename (e.g. "id_ed25519") */
    char type[16];              /* key type (ed25519/rsa/ecdsa) */
    char fingerprint[128];      /* SHA256 fingerprint */
    bool has_passphrase;        /* true if private key is encrypted */
    char path[512];             /* full path to private key */
    char pub_path[512];         /* full path to public key */
    i32  bits;                  /* key size in bits (for RSA) */
} KeyInfo;

/* Generate an SSH key pair. Returns true on success.
 * passphrase may be NULL or empty for no passphrase. */
bool keygen_generate(KeyAlgorithm algo, const char *comment,
                     const char *passphrase, GeneratedKey *out);

/* Generate a key and save directly to the given path.
 * Sets permissions: 0600 for private, 0644 for public.
 * Returns true on success. */
bool keygen_generate_to_file(KeyAlgorithm algo, const char *comment,
                             const char *passphrase, const char *filepath,
                             char *fingerprint_out, i32 fp_size);

/* Scan a directory for SSH key files.
 * Populates keys array, sets *count. Returns number found. */
i32 ssh_scan_keys(const char *ssh_dir, KeyInfo *keys, i32 *count, i32 max);

/* Delete a key pair (private + public). Returns true on success. */
bool ssh_delete_key(const char *private_key_path);

/* Read the public key content for clipboard copy.
 * Returns a malloc'd string or NULL. Caller must free. */
char *ssh_read_public_key(const char *pub_key_path);

/* Get algorithm name string */
const char *keygen_algo_name(KeyAlgorithm algo);

/* Get the default SSH directory path (~/.ssh/).
 * Writes to buf, returns buf. */
const char *ssh_get_default_dir(char *buf, i32 buf_size);

/* Add a private key to the running ssh-agent. Returns true on success.
 * The agent may prompt for a passphrase if the key is protected. */
bool keygen_add_to_agent(const char *key_path);

/* Change the passphrase on an existing private key file.
 * old_passphrase may be NULL/empty for unencrypted keys.
 * new_passphrase may be NULL/empty to remove the passphrase.
 * Returns true on success. */
bool keygen_change_passphrase(const char *key_path,
                              const char *old_passphrase,
                              const char *new_passphrase);

/* Check whether a private key file is passphrase-protected.
 * Returns true if the key is encrypted, false if unencrypted or on error. */
bool keygen_is_encrypted(const char *key_path);

#endif
