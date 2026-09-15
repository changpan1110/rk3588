#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "udp_output.c"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/debug.h"
#include "output/stream/udp/udp_output.h"

#define UDP_OUTPUT_RTP_PAYLOAD_MTU 1200

static void udp_output_write_rtp_header(uint8_t *buffer,
                                        int marker,
                                        uint16_t sequence,
                                        uint32_t timestamp,
                                        uint32_t ssrc) {
    buffer[0] = 0x80;
    buffer[1] = (uint8_t)(96 | (marker ? 0x80 : 0));
    buffer[2] = (uint8_t)(sequence >> 8);
    buffer[3] = (uint8_t)(sequence & 0xff);
    buffer[4] = (uint8_t)(timestamp >> 24);
    buffer[5] = (uint8_t)(timestamp >> 16);
    buffer[6] = (uint8_t)(timestamp >> 8);
    buffer[7] = (uint8_t)(timestamp & 0xff);
    buffer[8] = (uint8_t)(ssrc >> 24);
    buffer[9] = (uint8_t)(ssrc >> 16);
    buffer[10] = (uint8_t)(ssrc >> 8);
    buffer[11] = (uint8_t)(ssrc & 0xff);
}

static app_status_t udp_output_send_datagram(udp_output_ctx_t *ctx,
                                             const uint8_t *buffer,
                                             size_t size) {
    ssize_t sent;

    sent = sendto(ctx->socket_fd,
                  buffer,
                  size,
                  0,
                  (const struct sockaddr *)&ctx->destination,
                  sizeof(ctx->destination));
    if (sent < 0 || (size_t)sent != size) {
        LOGW("UDP/RTP send failed: %s", strerror(errno));
        return APP_ERR_IO;
    }
    return APP_OK;
}

static app_status_t udp_output_send_nal(udp_output_ctx_t *ctx,
                                        const uint8_t *nal,
                                        size_t nal_size,
                                        uint32_t timestamp,
                                        int marker) {
    uint8_t buffer[UDP_OUTPUT_RTP_PAYLOAD_MTU + 32];
    const size_t payload_capacity = UDP_OUTPUT_RTP_PAYLOAD_MTU;

    if (nal_size <= payload_capacity) {
        udp_output_write_rtp_header(buffer,
                                    marker,
                                    ctx->sequence,
                                    timestamp,
                                    ctx->ssrc);
        memcpy(buffer + 12, nal, nal_size);
        ctx->sequence++;
        return udp_output_send_datagram(ctx, buffer, 12 + nal_size);
    }

    {
        uint8_t fu_indicator = (uint8_t)((nal[0] & 0xe0) | 28);
        uint8_t nal_type = (uint8_t)(nal[0] & 0x1f);
        const uint8_t *position = nal + 1;
        size_t remaining = nal_size - 1;
        int first = 1;

        while (remaining > 0) {
            size_t chunk = remaining > payload_capacity
                               ? payload_capacity
                               : remaining;
            int last = chunk == remaining;
            app_status_t status;

            udp_output_write_rtp_header(buffer,
                                        marker && last,
                                        ctx->sequence,
                                        timestamp,
                                        ctx->ssrc);
            buffer[12] = fu_indicator;
            buffer[13] = (uint8_t)((first ? 0x80 : 0) |
                                   (last ? 0x40 : 0) |
                                   nal_type);
            memcpy(buffer + 14, position, chunk);
            ctx->sequence++;
            status = udp_output_send_datagram(ctx, buffer, 14 + chunk);
            if (status != APP_OK) {
                return status;
            }
            position += chunk;
            remaining -= chunk;
            first = 0;
        }
    }

    return APP_OK;
}

app_status_t udp_output_init(udp_output_ctx_t *ctx,
                             const char *destination_ip,
                             int destination_port) {
    if (ctx == NULL || destination_ip == NULL || destination_ip[0] == '\0' ||
        destination_port <= 0 || destination_port > 65535) {
        return APP_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->socket_fd = -1;
    ctx->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->socket_fd < 0) {
        LOGE("UDP socket failed: %s", strerror(errno));
        return APP_ERR_IO;
    }

    ctx->destination.sin_family = AF_INET;
    ctx->destination.sin_port = htons((uint16_t)destination_port);
    if (inet_pton(AF_INET, destination_ip, &ctx->destination.sin_addr) != 1) {
        LOGE("UDP destination IP invalid: %s", destination_ip);
        udp_output_deinit(ctx);
        return APP_ERR_PARAM;
    }

    ctx->ssrc = 0x524b3538;
    LOGI("stream destination udp://%s:%d (RTP/H264, payload_mtu=%d)",
         destination_ip,
         destination_port,
         UDP_OUTPUT_RTP_PAYLOAD_MTU);
    return APP_OK;
}

app_status_t udp_output_send(udp_output_ctx_t *ctx,
                             const uint8_t *data,
                             size_t size,
                             int64_t pts_us) {
    const uint8_t *end;
    const uint8_t *current;
    uint32_t timestamp;

    if (!udp_output_is_open(ctx) || data == NULL || size == 0) {
        return APP_ERR_PARAM;
    }

    end = data + size;
    current = data;
    timestamp = (uint32_t)((pts_us * 90) / 1000);

    while (current + 4 <= end) {
        size_t start_code_size;
        const uint8_t *nal_start;
        const uint8_t *next;
        const uint8_t *nal_end;
        int is_last;
        app_status_t status;

        if (!(current[0] == 0 && current[1] == 0 &&
              (current[2] == 1 ||
               (current[2] == 0 && current[3] == 1)))) {
            current++;
            continue;
        }
        start_code_size = current[2] == 1 ? 3U : 4U;
        nal_start = current + start_code_size;

        next = nal_start;
        while (next + 4 <= end &&
               !(next[0] == 0 && next[1] == 0 &&
                 (next[2] == 1 ||
                  (next[2] == 0 && next[3] == 1)))) {
            next++;
        }
        nal_end = next + 4 <= end ? next : end;
        is_last = next + 4 > end;

        if (nal_end > nal_start) {
            status = udp_output_send_nal(ctx,
                                         nal_start,
                                         (size_t)(nal_end - nal_start),
                                         timestamp,
                                         is_last);
            if (status != APP_OK) {
                return status;
            }
        }
        current = next;
    }

    return APP_OK;
}

int udp_output_is_open(const udp_output_ctx_t *ctx) {
    return ctx != NULL && ctx->socket_fd >= 0;
}

void udp_output_deinit(udp_output_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    if (ctx->socket_fd >= 0) {
        close(ctx->socket_fd);
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->socket_fd = -1;
}
