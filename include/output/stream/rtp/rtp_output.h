#ifndef RTP_OUTPUT_H
#define RTP_OUTPUT_H

#include <netinet/in.h>

#include "common/common.h"

typedef struct {
    int socket_fd;
    struct sockaddr_in destination;
    uint16_t sequence;
    uint32_t ssrc;
} rtp_output_ctx_t;

app_status_t rtp_output_init(rtp_output_ctx_t *ctx,
                                    const char *destination_ip,
                                    int destination_port);
app_status_t rtp_output_send(rtp_output_ctx_t *ctx,
                                    const uint8_t *data,
                                    size_t size,
                                    int64_t pts_us);
int rtp_output_is_open(const rtp_output_ctx_t *ctx);
void rtp_output_deinit(rtp_output_ctx_t *ctx);

#endif
