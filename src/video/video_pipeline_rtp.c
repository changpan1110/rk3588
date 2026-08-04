#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "video_pipeline_rtp.c"

#include "video_pipeline_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/debug.h"

int vp_udp_open(vp_ctx_t *p, const char *ip, int port) {
    p->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (p->udp_fd < 0) {
        LOGE("udp socket failed: %s", strerror(errno));
        return -1;
    }

    memset(&p->udp_addr, 0, sizeof(p->udp_addr));
    p->udp_addr.sin_family = AF_INET;
    p->udp_addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &p->udp_addr.sin_addr) != 1) {
        LOGE("udp dest ip invalid: %s", ip);
        close(p->udp_fd);
        p->udp_fd = -1;
        return -1;
    }
    p->rtp_seq = 0;
    p->rtp_ssrc = 0x524B3538;
    LOGI("stream dest udp://%s:%d (RTP/H264)", ip, port);
    return 0;
}

static void vp_rtp_write_header(uint8_t *buf, int marker, uint16_t seq, uint32_t ts, uint32_t ssrc) {
    buf[0] = 0x80;
    buf[1] = (uint8_t)(96 | (marker ? 0x80 : 0));
    buf[2] = (uint8_t)(seq >> 8);
    buf[3] = (uint8_t)(seq & 0xFF);
    buf[4] = (uint8_t)(ts >> 24);
    buf[5] = (uint8_t)(ts >> 16);
    buf[6] = (uint8_t)(ts >> 8);
    buf[7] = (uint8_t)(ts & 0xFF);
    buf[8] = (uint8_t)(ssrc >> 24);
    buf[9] = (uint8_t)(ssrc >> 16);
    buf[10] = (uint8_t)(ssrc >> 8);
    buf[11] = (uint8_t)(ssrc & 0xFF);
}

static void vp_rtp_send_nal(vp_ctx_t *p, const uint8_t *nal, int nal_size, uint32_t ts, int marker) {
    uint8_t buf[VP_RTP_MTU + 32];
    int payload_cap = VP_RTP_MTU;

    if (p->udp_fd < 0) {
        return;
    }

    if (nal_size <= payload_cap) {
        vp_rtp_write_header(buf, marker, p->rtp_seq, ts, p->rtp_ssrc);
        memcpy(buf + 12, nal, (size_t)nal_size);
        sendto(p->udp_fd, buf, (size_t)(12 + nal_size), 0,
               (struct sockaddr *)&p->udp_addr, sizeof(p->udp_addr));
        p->rtp_seq++;
        return;
    }

    {
        uint8_t fu_indicator = (uint8_t)((nal[0] & 0xE0) | 28);
        uint8_t nal_type = (uint8_t)(nal[0] & 0x1F);
        const uint8_t *pos = nal + 1;
        int remaining = nal_size - 1;
        int first = 1;

        while (remaining > 0) {
            int chunk = remaining > payload_cap ? payload_cap : remaining;
            int last = (chunk == remaining);

            vp_rtp_write_header(buf, marker && last, p->rtp_seq, ts, p->rtp_ssrc);
            buf[12] = fu_indicator;
            buf[13] = (uint8_t)((first ? 0x80 : 0) | (last ? 0x40 : 0) | nal_type);
            memcpy(buf + 14, pos, (size_t)chunk);
            sendto(p->udp_fd, buf, (size_t)(14 + chunk), 0,
                   (struct sockaddr *)&p->udp_addr, sizeof(p->udp_addr));
            p->rtp_seq++;
            pos += chunk;
            remaining -= chunk;
            first = 0;
        }
    }
}

void vp_rtp_send_packet(vp_ctx_t *p, const uint8_t *data, int size, int64_t pts_us) {
    const uint8_t *end = data + size;
    const uint8_t *cur = data;
    uint32_t ts = (uint32_t)((pts_us * 90) / 1000);

    while (cur + 4 <= end) {
        int sc_len;
        const uint8_t *nal_start;
        const uint8_t *next;
        const uint8_t *nal_end;
        int is_last;

        if (!(cur[0] == 0 && cur[1] == 0 && (cur[2] == 1 || (cur[2] == 0 && cur[3] == 1)))) {
            cur++;
            continue;
        }
        sc_len = (cur[2] == 1) ? 3 : 4;
        nal_start = cur + sc_len;

        next = nal_start;
        while (next + 4 <= end &&
               !(next[0] == 0 && next[1] == 0 && (next[2] == 1 || (next[2] == 0 && next[3] == 1)))) {
            next++;
        }
        nal_end = (next + 4 <= end) ? next : end;
        is_last = (next + 4 > end);

        if (nal_end > nal_start) {
            vp_rtp_send_nal(p, nal_start, (int)(nal_end - nal_start), ts, is_last);
        }
        cur = next;
    }
}
