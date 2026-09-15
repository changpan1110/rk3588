#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "can_socket.c"

#include "common/debug.h"
#include "input/can/can_socket.h"

#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CAN_SOCKET_LOG_BUFFER_SIZE 256U

static int64_t can_socket_now_ms(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static int can_socket_remaining_ms(int64_t deadline_ms) {
    int64_t remaining = deadline_ms - can_socket_now_ms();

    if (remaining <= 0) {
        return 0;
    }
    return remaining > 2147483647LL ? 2147483647 : (int)remaining;
}

static can_socket_status_t can_socket_system_error(can_socket_t *socket_can,
                                                   int system_error) {
    if (socket_can != NULL) {
        socket_can->last_system_error = system_error;
    }
    return CAN_SOCKET_ERR_SYSTEM;
}

static void can_socket_log_frame(const can_socket_t *socket_can,
                                 const char *direction,
                                 const struct can_frame *frame,
                                 int system_error) {
    char log_buffer[CAN_SOCKET_LOG_BUFFER_SIZE];
    size_t used;
    size_t i;
    int written;

    if (socket_can == NULL || frame == NULL || !socket_can->log_frames) {
        return;
    }

    written = snprintf(log_buffer,
                       sizeof(log_buffer),
                       "[SOCKETCAN][%s] id=0x%03X dlc=%u data=",
                       direction,
                       (unsigned)(frame->can_id & CAN_SFF_MASK),
                       (unsigned)frame->can_dlc);
    if (written < 0) {
        return;
    }
    used = (size_t)written < sizeof(log_buffer)
               ? (size_t)written
               : sizeof(log_buffer) - 1U;

    for (i = 0U; i < frame->can_dlc && used < sizeof(log_buffer) - 1U; ++i) {
        written = snprintf(log_buffer + used,
                           sizeof(log_buffer) - used,
                           "%s%02X",
                           i == 0U ? "" : " ",
                           frame->data[i]);
        if (written < 0) {
            return;
        }
        used += (size_t)written < sizeof(log_buffer) - used
                    ? (size_t)written
                    : sizeof(log_buffer) - used - 1U;
    }
    if (system_error != 0) {
        snprintf(log_buffer + used,
                 sizeof(log_buffer) - used,
                 " errno=%d (%s)",
                 system_error,
                 strerror(system_error));
        LOGE("%s", log_buffer);
        return;
    }
    LOGD("%s", log_buffer);
}

static can_socket_status_t can_socket_wait(can_socket_t *socket_can,
                                           short events,
                                           int timeout_ms) {
    struct pollfd descriptor;
    int status;

    descriptor.fd = socket_can->fd;
    descriptor.events = events;
    descriptor.revents = 0;
    do {
        status = poll(&descriptor, 1, timeout_ms);
    } while (status < 0 && errno == EINTR);

    if (status == 0) {
        return CAN_SOCKET_ERR_TIMEOUT;
    }
    if (status < 0) {
        return can_socket_system_error(socket_can, errno);
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return can_socket_system_error(socket_can, EIO);
    }
    return (descriptor.revents & events) != 0 ? CAN_SOCKET_OK
                                               : CAN_SOCKET_NO_DATA;
}

void can_socket_init(can_socket_t *socket_can) {
    if (socket_can == NULL) {
        return;
    }
    memset(socket_can, 0, sizeof(*socket_can));
    socket_can->fd = -1;
}

can_socket_status_t can_socket_open(can_socket_t *socket_can,
                                    const char *interface_name,
                                    uint32_t tx_id,
                                    uint32_t rx_id) {
    struct ifreq interface_request;
    struct sockaddr_can address;
    struct can_filter filter;
    int fd;

    if (socket_can == NULL || interface_name == NULL ||
        interface_name[0] == '\0' ||
        strlen(interface_name) >= CAN_SOCKET_INTERFACE_NAME_MAX ||
        tx_id > CAN_SFF_MASK || rx_id > CAN_SFF_MASK) {
        return CAN_SOCKET_ERR_PARAM;
    }

    can_socket_close(socket_can);
    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        return can_socket_system_error(socket_can, errno);
    }

    memset(&interface_request, 0, sizeof(interface_request));
    snprintf(interface_request.ifr_name,
             sizeof(interface_request.ifr_name),
             "%s",
             interface_name);
    if (ioctl(fd, SIOCGIFINDEX, &interface_request) != 0) {
        int system_error = errno;
        close(fd);
        return can_socket_system_error(socket_can, system_error);
    }

    filter.can_id = rx_id;
    filter.can_mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG;
    if (setsockopt(fd,
                   SOL_CAN_RAW,
                   CAN_RAW_FILTER,
                   &filter,
                   sizeof(filter)) != 0) {
        int system_error = errno;
        close(fd);
        return can_socket_system_error(socket_can, system_error);
    }

    memset(&address, 0, sizeof(address));
    address.can_family = AF_CAN;
    address.can_ifindex = interface_request.ifr_ifindex;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        int system_error = errno;
        close(fd);
        return can_socket_system_error(socket_can, system_error);
    }

    socket_can->fd = fd;
    socket_can->tx_id = tx_id;
    socket_can->rx_id = rx_id;
    snprintf(socket_can->interface_name,
             sizeof(socket_can->interface_name),
             "%s",
             interface_name);
    return CAN_SOCKET_OK;
}

void can_socket_close(can_socket_t *socket_can) {
    if (socket_can == NULL) {
        return;
    }
    if (socket_can->fd >= 0) {
        close(socket_can->fd);
    }
    can_socket_init(socket_can);
}

int can_socket_is_open(const can_socket_t *socket_can) {
    return socket_can != NULL && socket_can->fd >= 0;
}

void can_socket_set_log_frames(can_socket_t *socket_can, int enabled) {
    if (socket_can != NULL) {
        socket_can->log_frames = enabled != 0;
    }
}

int can_socket_last_system_error(const can_socket_t *socket_can) {
    return socket_can != NULL ? socket_can->last_system_error : 0;
}

can_socket_status_t can_socket_send(can_socket_t *socket_can,
                                    const uint8_t *data,
                                    size_t size,
                                    int timeout_ms) {
    size_t offset = 0U;
    int64_t deadline_ms;

    if (socket_can == NULL || data == NULL || size == 0U || timeout_ms < 0) {
        return CAN_SOCKET_ERR_PARAM;
    }
    if (!can_socket_is_open(socket_can)) {
        return CAN_SOCKET_ERR_STATE;
    }
    deadline_ms = can_socket_now_ms() + timeout_ms;

    while (offset < size) {
        struct can_frame frame;
        size_t chunk_size = size - offset;
        ssize_t written;
        can_socket_status_t status;

        if (chunk_size > CAN_SOCKET_CLASSIC_DATA_MAX) {
            chunk_size = CAN_SOCKET_CLASSIC_DATA_MAX;
        }
        memset(&frame, 0, sizeof(frame));
        frame.can_id = socket_can->tx_id;
        frame.can_dlc = (uint8_t)chunk_size;
        memcpy(frame.data, data + offset, chunk_size);

        status = can_socket_wait(socket_can,
                                 POLLOUT,
                                 can_socket_remaining_ms(deadline_ms));
        if (status != CAN_SOCKET_OK) {
            return status;
        }
        do {
            written = write(socket_can->fd, &frame, sizeof(frame));
        } while (written < 0 && errno == EINTR);
        if (written != (ssize_t)sizeof(frame)) {
            int system_error = written < 0 ? errno : EIO;

            can_socket_log_frame(socket_can,
                                 "TX-FAIL",
                                 &frame,
                                 system_error);
            return can_socket_system_error(socket_can, system_error);
        }
        can_socket_log_frame(socket_can, "TX", &frame, 0);
        offset += chunk_size;
    }
    return CAN_SOCKET_OK;
}

can_socket_status_t can_socket_receive(can_socket_t *socket_can,
                                       uint8_t data[CAN_SOCKET_CLASSIC_DATA_MAX],
                                       size_t *size,
                                       int timeout_ms) {
    struct can_frame frame;
    ssize_t received;
    can_socket_status_t status;

    if (socket_can == NULL || data == NULL || size == NULL || timeout_ms < 0) {
        return CAN_SOCKET_ERR_PARAM;
    }
    if (!can_socket_is_open(socket_can)) {
        return CAN_SOCKET_ERR_STATE;
    }

    status = can_socket_wait(socket_can, POLLIN, timeout_ms);
    if (status != CAN_SOCKET_OK) {
        return status;
    }
    do {
        received = read(socket_can->fd, &frame, sizeof(frame));
    } while (received < 0 && errno == EINTR);
    if (received != (ssize_t)sizeof(frame)) {
        return can_socket_system_error(socket_can,
                                       received < 0 ? errno : EIO);
    }
    if ((frame.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) != 0U ||
        (frame.can_id & CAN_SFF_MASK) != socket_can->rx_id ||
        frame.can_dlc > CAN_SOCKET_CLASSIC_DATA_MAX) {
        return CAN_SOCKET_ERR_PROTOCOL;
    }

    *size = frame.can_dlc;
    memcpy(data, frame.data, *size);
    can_socket_log_frame(socket_can, "RX", &frame, 0);
    return CAN_SOCKET_OK;
}

const char *can_socket_status_string(can_socket_status_t status) {
    switch (status) {
        case CAN_SOCKET_OK: return "ok";
        case CAN_SOCKET_NO_DATA: return "no data";
        case CAN_SOCKET_ERR_PARAM: return "invalid parameter";
        case CAN_SOCKET_ERR_STATE: return "CAN socket is not open";
        case CAN_SOCKET_ERR_SYSTEM: return "SocketCAN system error";
        case CAN_SOCKET_ERR_TIMEOUT: return "timeout";
        case CAN_SOCKET_ERR_PROTOCOL: return "unexpected CAN frame";
        default: return "unknown CAN status";
    }
}
