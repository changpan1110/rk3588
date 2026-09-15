#include "input/network/network_endpoint.h"

#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

int network_endpoint_resolve_ipv4(const char *host,
                                  int port,
                                  network_endpoint_type_t type,
                                  int passive,
                                  struct sockaddr_in *out_address) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char port_text[16];
    int status;

    if (port <= 0 || port > 65535 || out_address == NULL ||
        (type != NETWORK_ENDPOINT_UDP && type != NETWORK_ENDPOINT_TCP)) {
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = type == NETWORK_ENDPOINT_UDP ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    snprintf(port_text, sizeof(port_text), "%d", port);
    status = getaddrinfo(
        host != NULL && host[0] != '\0' ? host : NULL,
        port_text,
        &hints,
        &result);
    if (status != 0 || result == NULL || result->ai_addrlen < sizeof(*out_address)) {
        if (result != NULL) {
            freeaddrinfo(result);
        }
        return -1;
    }

    memcpy(out_address, result->ai_addr, sizeof(*out_address));
    freeaddrinfo(result);
    return 0;
}

int network_endpoint_set_nonblocking(int fd) {
    int flags;

    if (fd < 0) {
        return -1;
    }
    flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
}
