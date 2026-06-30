#ifndef SSH_PORT_FORWARD_H
#define SSH_PORT_FORWARD_H

#include "core/types.h"
#include <libssh2.h>

typedef enum {
    FWD_LOCAL = 0,
    FWD_REMOTE = 1,
    FWD_DYNAMIC = 2,
} ForwardType;

typedef struct {
    ForwardType type;
    i32         local_port;
    char        remote_host[256];
    i32         remote_port;
    bool        active;
} ForwardRule;

i32  forward_add_local(LIBSSH2_SESSION *owner, i32 local_port, const char *remote_host, i32 remote_port);
i32  forward_add_socks5(LIBSSH2_SESSION *owner, i32 local_port);
void forward_remove(LIBSSH2_SESSION *owner, i32 index);
void forward_poll(LIBSSH2_SESSION *owner);
i32  forward_count(LIBSSH2_SESSION *owner);
bool forward_get(LIBSSH2_SESSION *owner, i32 index, ForwardRule *out);
void forward_cleanup(LIBSSH2_SESSION *owner);
void forward_cleanup_all(void);

#endif
