/*
 * Liu - shared networking helpers
 */
#include "core/net.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

int net_tcp_connect(const char *hostname, int port) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = {0}, *res;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, port_str, &hints, &res) != 0) return -1;

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return -1; }

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        close(sock);
        freeaddrinfo(res);
        return -1;
    }

    /* Disable Nagle's algorithm for interactive traffic. Single-byte SSH
     * keystrokes would otherwise be held by the kernel waiting for more
     * data to batch, adding up to ~40 ms per direction (~80 ms round-trip)
     * on top of network RTT. All callers here are interactive channels
     * (SSH main link, proxy hops, telnet, port-forward destinations) —
     * none benefit from Nagle. Failure to set it is non-fatal. */
    int one = 1;
    (void)setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                     (const char *)&one, sizeof(one));

    freeaddrinfo(res);
    return sock;
}

void net_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

const char *net_home_dir(void) {
    const char *home = getenv("HOME");
    return home ? home : "/tmp";
}

const char *net_ssh_dir(void) {
    static char buf[512] = {0};
    if (!buf[0]) {
        snprintf(buf, sizeof(buf), "%s/.ssh/", net_home_dir());
    }
    return buf;
}
