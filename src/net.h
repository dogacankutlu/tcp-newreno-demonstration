#ifndef NET_H
#define NET_H

#include "common.h"

int  net_open(int port);
int  net_send(int sock, const char *host, int port, const char *msg);
int  net_recv(int sock, char *buf, size_t buflen, char *from_host, int *from_port);

#endif
