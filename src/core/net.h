/*
 * Liu - shared networking helpers
 */
#ifndef CORE_NET_H
#define CORE_NET_H

#include "core/types.h"

#include <stdatomic.h>

/* How long a connect may take before it is given up on, name lookup included.
 * Without a limit, a host that swallows SYNs would cost the kernel's own
 * give-up time (about 75 s on macOS) per address. */
#define NET_CONNECT_TIMEOUT_MS 20000

/* TCP connect to host:port. Returns a BLOCKING socket fd (TCP_NODELAY set), or
 * -1. Bounded by NET_CONNECT_TIMEOUT_MS; still synchronous, so not for the
 * main thread — see net_connect_begin. */
int net_tcp_connect(const char *hostname, int port);

/* The same, with its own deadline and a way out: returns -1 as soon as
 * `cancel` (may be NULL) reads true, without waiting for the lookup or the
 * connect to give up by themselves. For callers already on a worker thread
 * that someone may need to join. */
int net_tcp_connect_ex(const char *hostname, int port, int timeout_ms,
                       const atomic_bool *cancel);

/* A connect the caller does not wait for.
 *
 * The lookup and the connect run on a thread of their own and the caller asks
 * how it went. That is the only form the main thread may use: getaddrinfo
 * cannot be interrupted and cannot be given a deadline, so "non-blocking
 * connect" alone still parks the caller in the resolver.
 *
 * net_connect_begin    NULL when the arguments are refused or no thread could
 *                      be made. `wake` (may be NULL) is called from the
 *                      connecting thread once the attempt has ended.
 * net_connect_poll     NET_CONNECT_PENDING while it runs. Otherwise the handle
 *                      is consumed: a blocking fd the caller now owns, or
 *                      NET_CONNECT_FAILED.
 * net_connect_abandon  walk away. The handle is consumed; the attempt ends by
 *                      itself and closes a socket it wins. */
typedef struct NetConnect NetConnect;
#define NET_CONNECT_PENDING (-2)
#define NET_CONNECT_FAILED  (-1)
NetConnect *net_connect_begin(const char *hostname, int port, int timeout_ms,
                              void (*wake)(void));
int         net_connect_poll(NetConnect *c);
void        net_connect_abandon(NetConnect *c);

/* Threads currently inside a hostname lookup (numeric hosts do not count).
 * Bounded by NET_RESOLVER_MAX: past it net_connect_begin() refuses a
 * non-numeric host — a resolver that has stopped answering must not turn every
 * timed-out attempt into one more thread parked in getaddrinfo. */
#define NET_RESOLVER_MAX 16
int         net_resolver_inflight(void);

/* Set socket to non-blocking mode. Returns false on invalid fd/fcntl error. */
bool net_set_nonblocking(int fd);

/* Get path to ~/.ssh/ directory (with trailing slash). */
const char *net_ssh_dir(void);

/* Get an absolute home directory from $HOME or the account database. Returns
 * an empty string when neither can be resolved (never falls back to /tmp). */
const char *net_home_dir(void);

#endif
