/*
 * Liu - shared networking helpers
 */
#ifndef CORE_NET_H
#define CORE_NET_H

#include "core/types.h"

/* TCP connect to host:port. Returns socket fd or -1. */
int net_tcp_connect(const char *hostname, int port);

/* Set socket to non-blocking mode. */
void net_set_nonblocking(int fd);

/* Get path to ~/.ssh/ directory (with trailing slash). */
const char *net_ssh_dir(void);

/* Get $HOME directory. */
const char *net_home_dir(void);

#endif
