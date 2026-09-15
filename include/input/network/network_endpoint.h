#ifndef INPUT_NETWORK_ENDPOINT_H
#define INPUT_NETWORK_ENDPOINT_H

#include <netinet/in.h>

typedef enum {
    NETWORK_ENDPOINT_UDP = 0,
    NETWORK_ENDPOINT_TCP
} network_endpoint_type_t;

int network_endpoint_resolve_ipv4(const char *host,
                                  int port,
                                  network_endpoint_type_t type,
                                  int passive,
                                  struct sockaddr_in *out_address);
int network_endpoint_set_nonblocking(int fd);

#endif
