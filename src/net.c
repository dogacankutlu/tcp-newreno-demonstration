#include "net.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>

int net_open(int port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        perror("socket");
        return -1;
    }

    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind to port %d failed: %s\n", port, strerror(errno));
        close(s);
        return -1;
    }
    return s;
}

int net_send(int sock, const char *host, int port, const char *msg) {
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) {
        fprintf(stderr, "net_send: bad host '%s'\n", host);
        return -1;
    }
    ssize_t n = sendto(sock, msg, strlen(msg), 0,
                       (struct sockaddr *)&dst, sizeof(dst));
    return (n < 0) ? -1 : 0;
}

int net_recv(int sock, char *buf, size_t buflen, char *from_host, int *from_port) {
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    ssize_t n = recvfrom(sock, buf, buflen - 1, 0,
                         (struct sockaddr *)&src, &slen);
    if (n < 0) return -1;
    buf[n] = '\0';
    if (from_host) inet_ntop(AF_INET, &src.sin_addr, from_host, 64);
    if (from_port) *from_port = ntohs(src.sin_port);
    return (int)n;
}
