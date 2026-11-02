/*
 * socket_owner — which socket file at a path is the one Liu bound, and the list
 * of them the exit path sweeps.
 *
 * A Unix-socket forward (ssh/ssh_session.c) listens on a path of its own. The
 * file is Liu's to remove when the forward ends — and only that file: the path
 * can by then hold something else (another Liu bound it after this one's
 * socket was replaced, the user put a file there, a program of theirs made a
 * socket of its own), so removing "any socket this user owns" could take a live
 * listener away from whoever had made it.
 *
 * So the bind records the file's identity (device and inode, read once the
 * socket is in place and its mode is set), and the stop removes the path only
 * while it still names that file. The same identity is what the process-exit
 * sweep checks: quitting runs `exit()`, which never destroys the panes, so the
 * stop that removes a forward's socket does not run for the forwards still up
 * at a quit.
 *
 * Pure of libssh2 and of the session: it works on real sockets and paths alone.
 */
#ifndef SSH_SOCKET_OWNER_H
#define SSH_SOCKET_OWNER_H

#include "core/types.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    dev_t dev;
    ino_t ino;
    bool  valid;       /* false: nothing was bound, nothing is ever removed */
} SockIdent;

/* The identity of the file `st` describes — valid only for a socket. */
static inline SockIdent sock_ident_of(const struct stat *st) {
    SockIdent id = { 0, 0, false };
    if (st && S_ISSOCK(st->st_mode)) {
        id.dev = st->st_dev;
        id.ino = st->st_ino;
        id.valid = true;
    }
    return id;
}

/* Is the file `st` describes still the socket `id` names? Same device, same
 * inode, and still a socket: a regular file or a link put there since, or a
 * socket another process bound after this one went, is not. */
static inline bool sock_ident_matches(const SockIdent *id, const struct stat *st) {
    return id && id->valid && st && S_ISSOCK(st->st_mode) &&
           st->st_dev == id->dev && st->st_ino == id->ino;
}

/* The identity of the socket now at `path` (lstat: a link there is no socket).
 * An invalid identity when there is none. */
static inline SockIdent sock_ident_at(const char *path) {
    struct stat st;
    if (!path || !path[0] || lstat(path, &st) != 0) return (SockIdent){ 0, 0, false };
    return sock_ident_of(&st);
}

/* Removes `path` when it is still the socket `id` names. Returns whether it
 * did; a path that holds anything else is left exactly as it is. */
static inline bool sock_unlink_if_ours(const char *path, const SockIdent *id) {
    struct stat st;
    if (!path || !path[0] || !id || !id->valid) return false;
    if (lstat(path, &st) != 0 || !sock_ident_matches(id, &st)) return false;
    return unlink(path) == 0;
}

/* ---- The sockets the process has bound, for the exit sweep ------------------
 *
 * Bounded: the table holds what the forwards of every window can bind at once
 * (four a connection); a bind past it still works and is removed by its own
 * stop, only the exit sweep does not know it. The caller serialises access. */

#define SOCK_REG_CAP      128
#define SOCK_REG_PATH_CAP 512

typedef struct {
    char      path[SOCK_REG_PATH_CAP];
    SockIdent id;
} SockRegEntry;

typedef struct {
    SockRegEntry e[SOCK_REG_CAP];
    i32          n;
} SockRegistry;

/* A socket bound at `path`. A path already listed (the same forward bound
 * again after a reconnect, or replaced by another bind) takes the new
 * identity. False when the path does not fit or the table is full. */
static inline bool sock_reg_add(SockRegistry *r, const char *path, SockIdent id) {
    if (!r || !path || !id.valid) return false;
    usize len = strlen(path);
    if (len == 0 || len >= SOCK_REG_PATH_CAP) return false;
    for (i32 i = 0; i < r->n; i++) {
        if (strcmp(r->e[i].path, path) == 0) { r->e[i].id = id; return true; }
    }
    if (r->n >= SOCK_REG_CAP) return false;
    memcpy(r->e[r->n].path, path, len + 1);
    r->e[r->n].id = id;
    r->n++;
    return true;
}

/* The forward at `path` ended: forget it, but only the entry that names `id`
 * (a later bind of the same path owns a newer entry, which stays). */
static inline void sock_reg_forget(SockRegistry *r, const char *path, const SockIdent *id) {
    if (!r || !path || !id) return;
    for (i32 i = 0; i < r->n; i++) {
        if (strcmp(r->e[i].path, path) != 0 ||
            r->e[i].id.dev != id->dev || r->e[i].id.ino != id->ino) continue;
        r->e[i] = r->e[r->n - 1];
        r->n--;
        return;
    }
}

/* The exit sweep: removes every listed path that is still its socket, empties
 * the table, and returns how many files went. */
static inline i32 sock_reg_sweep(SockRegistry *r) {
    i32 gone = 0;
    if (!r) return 0;
    for (i32 i = 0; i < r->n; i++)
        if (sock_unlink_if_ours(r->e[i].path, &r->e[i].id)) gone++;
    r->n = 0;
    return gone;
}

#endif /* SSH_SOCKET_OWNER_H */
