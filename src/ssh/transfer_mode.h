/*
 * transfer_mode — the permission bits a file copied over SFTP is created with.
 *
 * The source's own bits (the receiving side's umask still applies), and never
 * set-id or sticky bits, so a script copied either way stays executable. The
 * owner can always read and write the copy. Replacing an existing remote file
 * keeps that file's mode (sftp.c).
 *
 * Pure.
 */
#ifndef SSH_TRANSFER_MODE_H
#define SSH_TRANSFER_MODE_H

#include "core/types.h"

/* `known`: the source's mode was read. `fallback`: the mode used when not. */
static inline u32 transfer_create_mode(bool known, u32 source_mode, u32 fallback) {
    if (!known) return fallback;
    return (source_mode & 0777u) | 0600u;
}

#endif /* SSH_TRANSFER_MODE_H */
