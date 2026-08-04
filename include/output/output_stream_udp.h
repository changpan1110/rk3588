#ifndef OUTPUT_STREAM_UDP_H
#define OUTPUT_STREAM_UDP_H

#include <netinet/in.h>

#include "common/common.h"

typedef struct {
    int socket_fd;
    struct sockaddr_in destination;
    uint16_t sequence;
    uint32_t ssrc;
} output_stream_udp_ctx_t;

app_status_t output_stream_udp_init(output_stream_udp_ctx_t *ctx,
                                    const char *destination_ip,
                                    int destination_port);
app_status_t output_stream_udp_send(output_stream_udp_ctx_t *ctx,
                                    const uint8_t *data,
                                    size_t size,
                                    int64_t pts_us);
int output_stream_udp_is_open(const output_stream_udp_ctx_t *ctx);
void output_stream_udp_deinit(output_stream_udp_ctx_t *ctx);

#endif
