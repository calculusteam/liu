/*
 * Liu - Mosh wrapper
 * Spawns mosh-client binary via PTY for UDP-based roaming connections.
 */
#ifndef MOSH_H
#define MOSH_H

#include "core/types.h"

typedef struct MoshSession MoshSession;

/* Check if mosh is installed on the system */
bool mosh_available(void);

/* Create a mosh session to host. Uses SSH for initial key exchange.
 * cols/rows for initial PTY size. */
MoshSession *mosh_create(const char *host, const char *user, i32 port,
                          i32 cols, i32 rows);
void  mosh_destroy(MoshSession *ms);
i32   mosh_read(MoshSession *ms, u8 *buf, i32 buf_size);
i32   mosh_write(MoshSession *ms, const u8 *data, i32 len);
void  mosh_resize(MoshSession *ms, i32 cols, i32 rows);
bool  mosh_is_alive(MoshSession *ms);

/* Returns the child pid for SIGSTOP/SIGCONT-based suspend, or -1 if none. */
i32   mosh_child_pid(MoshSession *ms);

#endif
