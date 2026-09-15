#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "mavlink_transport_tcp.c"

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
} mavlink_tcp_reply_t;

static int mavlink_tcp_send(
    void *opaque,
    const uint8_t *data,
    size_t size) {
    mavlink_tcp_reply_t *reply = (mavlink_tcp_reply_t *)opaque;
    size_t offset = 0U;

    while (offset < size) {
        ssize_t written = send(
            reply->fd,
            data + offset,
            size - offset,
            MSG_NOSIGNAL);

        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = {reply->fd, POLLOUT, 0};
            if (poll(&descriptor, 1, MAVLINK_CONTROL_POLL_MS) > 0) {
                continue;
            }
        }
        return -1;
    }
    return 0;
}

static int mavlink_tcp_run_connection(
    int fd,
    const mavlink_transport_runtime_t *runtime) {
    mavlink_tcp_reply_t reply = {fd};
    uint8_t buffer[2048];

    while (!runtime->should_stop(runtime->opaque)) {
        struct pollfd descriptor = {fd, POLLIN, 0};
        int poll_result = poll(&descriptor, 1, MAVLINK_CONTROL_POLL_MS);

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (poll_result == 0) {
            continue;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return -1;
        }
        if ((descriptor.revents & POLLIN) != 0) {
            ssize_t received = recv(fd, buffer, sizeof(buffer), 0);

            if (received > 0) {
                if (runtime->on_data(
                        runtime->opaque,
                        buffer,
                        (size_t)received,
                        mavlink_tcp_send,
                        &reply) != 0) {
                    return -1;
                }
            } else if (received == 0) {
                return -1;
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                return -1;
            }
        }
    }
    return 0;
}

static int mavlink_tcp_open_server(
    const mavlink_control_config_t *config) {
    struct sockaddr_in address;
    int fd;
    int reuse = 1;

    if (network_endpoint_resolve_ipv4(
            config->tcp_host,
            config->tcp_port,
            NETWORK_ENDPOINT_TCP,
            1,
            &address) != 0) {
        LOGE("MAVLink TCP address resolution failed for %s:%d",
             config->tcp_host,
             config->tcp_port);
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 1) != 0 ||
        network_endpoint_set_nonblocking(fd) != 0) {
        LOGE("MAVLink TCP listen %s:%d failed: %s",
             config->tcp_host,
             config->tcp_port,
             strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int mavlink_tcp_open_client(
    const mavlink_control_config_t *config,
    const mavlink_transport_runtime_t *runtime) {
    struct sockaddr_in address;
    int elapsed_ms = 0;
    int fd;

    if (network_endpoint_resolve_ipv4(
            config->tcp_host,
            config->tcp_port,
            NETWORK_ENDPOINT_TCP,
            0,
            &address) != 0) {
        LOGE("MAVLink TCP address resolution failed for %s:%d",
             config->tcp_host,
             config->tcp_port);
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || network_endpoint_set_nonblocking(fd) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0) {
        return fd;
    }
    if (errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    while (!runtime->should_stop(runtime->opaque) && elapsed_ms < 2000) {
        struct pollfd descriptor = {fd, POLLOUT, 0};
        int poll_result = poll(&descriptor, 1, MAVLINK_CONTROL_POLL_MS);

        if (poll_result < 0 && errno == EINTR) {
            continue;
        }
        if (poll_result <= 0) {
            if (poll_result == 0) {
                elapsed_ms += MAVLINK_CONTROL_POLL_MS;
                continue;
            }
            close(fd);
            return -1;
        }
        if ((descriptor.revents & POLLOUT) != 0) {
            socklen_t error_length;
            int socket_error = 0;

            error_length = sizeof(socket_error);
            if (getsockopt(
                    fd,
                    SOL_SOCKET,
                    SO_ERROR,
                    &socket_error,
                    &error_length) == 0 &&
                socket_error == 0) {
                return fd;
            }
            errno = socket_error;
            close(fd);
            return -1;
        }
    }

    errno = ETIMEDOUT;
    close(fd);
    return -1;
}

static int mavlink_tcp_run_client(
    const mavlink_control_config_t *config,
    const mavlink_transport_runtime_t *runtime) {
    int fd = mavlink_tcp_open_client(config, runtime);

    if (fd < 0) {
        if (!runtime->should_stop(runtime->opaque)) {
            LOGW("MAVLink TCP connect %s:%d failed: %s",
                 config->tcp_host,
                 config->tcp_port,
                 strerror(errno));
        }
        return -1;
    }
    LOGI("MAVLink TCP connected to %s:%d",
         config->tcp_host,
         config->tcp_port);
    (void)mavlink_tcp_run_connection(fd, runtime);
    close(fd);
    LOGI("MAVLink TCP connection closed");
    return 0;
}

static int mavlink_tcp_run_server(
    const mavlink_control_config_t *config,
    const mavlink_transport_runtime_t *runtime) {
    int listen_fd = mavlink_tcp_open_server(config);

    if (listen_fd < 0) {
        return -1;
    }
    LOGI("MAVLink TCP server listening on %s:%d",
         config->tcp_host,
         config->tcp_port);
    while (!runtime->should_stop(runtime->opaque)) {
        struct pollfd descriptor = {listen_fd, POLLIN, 0};
        int poll_result = poll(&descriptor, 1, MAVLINK_CONTROL_POLL_MS);

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (poll_result > 0 && (descriptor.revents & POLLIN) != 0) {
            int client_fd = accept(listen_fd, NULL, NULL);

            if (client_fd >= 0) {
                if (network_endpoint_set_nonblocking(client_fd) != 0) {
                    close(client_fd);
                    continue;
                }
                LOGI("MAVLink TCP client connected");
                (void)mavlink_tcp_run_connection(client_fd, runtime);
                close(client_fd);
                LOGI("MAVLink TCP client disconnected");
            }
        }
    }

    close(listen_fd);
    return 0;
}

int mavlink_transport_tcp_run(
    const mavlink_control_config_t *config,
    const mavlink_transport_runtime_t *runtime) {
    return config->tcp_mode == MAVLINK_CONTROL_TCP_CLIENT
               ? mavlink_tcp_run_client(config, runtime)
               : mavlink_tcp_run_server(config, runtime);
}
