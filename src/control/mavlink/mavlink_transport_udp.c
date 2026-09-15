#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "mavlink_transport_udp.c"

#include "control/mavlink/mavlink_transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/debug.h"
#include "input/network/network_endpoint.h"

typedef struct {
    int fd;
    struct sockaddr_storage peer;
    socklen_t peer_length;
} mavlink_udp_reply_t;

static int mavlink_udp_send(
    void *opaque,
    const uint8_t *data,
    size_t size) {
    mavlink_udp_reply_t *reply = (mavlink_udp_reply_t *)opaque;
    ssize_t written = sendto(
        reply->fd,
        data,
        size,
        0,
        (const struct sockaddr *)&reply->peer,
        reply->peer_length);

    return written == (ssize_t)size ? 0 : -1;
}

int mavlink_transport_udp_run(
    const mavlink_control_config_t *config,
    const mavlink_transport_runtime_t *runtime) {
    struct sockaddr_in address;
    uint8_t buffer[2048];
    int fd;
    int reuse = 1;

    if (network_endpoint_resolve_ipv4(
            config->udp_bind_ip,
            config->udp_port,
            NETWORK_ENDPOINT_UDP,
            1,
            &address) != 0) {
        LOGE("MAVLink UDP address resolution failed for %s:%d",
             config->udp_bind_ip,
             config->udp_port);
        return -1;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        LOGE("MAVLink UDP socket failed: %s", strerror(errno));
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        network_endpoint_set_nonblocking(fd) != 0) {
        LOGE("MAVLink UDP bind %s:%d failed: %s",
             config->udp_bind_ip,
             config->udp_port,
             strerror(errno));
        close(fd);
        return -1;
    }

    LOGI("MAVLink UDP listening on %s:%d",
         config->udp_bind_ip,
         config->udp_port);
    while (!runtime->should_stop(runtime->opaque)) {
        struct pollfd descriptor = {fd, POLLIN, 0};
        int poll_result = poll(&descriptor, 1, MAVLINK_CONTROL_POLL_MS);

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOGW("MAVLink UDP poll failed: %s", strerror(errno));
            break;
        }
        if (poll_result == 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }

        for (;;) {
            mavlink_udp_reply_t reply;
            ssize_t received;

            memset(&reply, 0, sizeof(reply));
            reply.fd = fd;
            reply.peer_length = sizeof(reply.peer);
            received = recvfrom(
                fd,
                buffer,
                sizeof(buffer),
                0,
                (struct sockaddr *)&reply.peer,
                &reply.peer_length);
            if (received > 0) {
                if (runtime->on_data(
                        runtime->opaque,
                        buffer,
                        (size_t)received,
                        mavlink_udp_send,
                        &reply) != 0) {
                    LOGW("MAVLink UDP packet handling failed");
                }
                continue;
            }
            if (received < 0 && errno == EINTR) {
                continue;
            }
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            if (received < 0) {
                LOGW("MAVLink UDP receive failed: %s", strerror(errno));
            }
            break;
        }
    }

    close(fd);
    return 0;
}
