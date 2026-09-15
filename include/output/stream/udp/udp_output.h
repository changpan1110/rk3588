#ifndef UDP_OUTPUT_H
#define UDP_OUTPUT_H

#include <netinet/in.h>

#include "common/common.h"

typedef struct {
    int socket_fd;
    struct sockaddr_in destination;
    uint16_t sequence;
    uint32_t ssrc;
} udp_output_ctx_t;

app_status_t udp_output_init(udp_output_ctx_t *ctx,
                             const char *destination_ip,
                             int destination_port);
app_status_t udp_output_send(udp_output_ctx_t *ctx,
                             const uint8_t *data,
                             size_t size,
                             int64_t pts_us);
int udp_output_is_open(const udp_output_ctx_t *ctx);
void udp_output_deinit(udp_output_ctx_t *ctx);

#endif
