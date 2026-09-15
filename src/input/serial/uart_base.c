#include "input/serial/uart_base.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#ifdef __linux__
#include <asm/ioctls.h>
#include <asm/termbits.h>
#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

static int uart_base_config_valid(const uart_config_t *cfg) {
    if (cfg == NULL || cfg->device[0] == '\0' || cfg->baudrate == 0) {
        return 0;
    }
    if (cfg->data_bits < 5 || cfg->data_bits > 8) {
        return 0;
    }
    if (cfg->parity != UART_PARITY_NONE &&
        cfg->parity != UART_PARITY_EVEN &&
        cfg->parity != UART_PARITY_ODD) {
        return 0;
    }
    if (cfg->stop_bits != 1 && cfg->stop_bits != 2) {
        return 0;
    }
    return 1;
}

void uart_base_config_init(uart_config_t *cfg) {
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->baudrate = 115200;
    cfg->data_bits = 8;
    cfg->parity = UART_PARITY_NONE;
    cfg->stop_bits = 1;
    cfg->nonblocking = 0;
    cfg->fd = -1;
}

#ifdef __linux__
static unsigned int uart_base_data_bits_flag(uint8_t data_bits) {
    switch (data_bits) {
        case 5: return CS5;
        case 6: return CS6;
        case 7: return CS7;
        case 8: return CS8;
        default: return 0;
    }
}

static uart_status_t uart_base_configure_rs485(const uart_config_t *cfg) {
    struct serial_rs485 rs485;

    if (!cfg->is_rs485) {
        return UART_OK;
    }

    memset(&rs485, 0, sizeof(rs485));
    rs485.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
    if (ioctl(cfg->fd, TIOCSRS485, &rs485) < 0) {
        return UART_ERR_UNSUPPORTED;
    }
    return UART_OK;
}

static uart_status_t uart_base_configure_port(uart_config_t *cfg) {
    struct termios2 tty;
    unsigned int data_bits_flag;

    data_bits_flag = uart_base_data_bits_flag(cfg->data_bits);
    if (data_bits_flag == 0) {
        return UART_ERR_PARAM;
    }
    if (ioctl(cfg->fd, TCGETS2, &tty) < 0) {
        return UART_ERR_IO;
    }

    tty.c_iflag = IGNBRK;
    tty.c_oflag = 0;
    tty.c_lflag = 0;
    tty.c_cflag &= ~(CBAUD | CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_cflag |= BOTHER | data_bits_flag | CLOCAL | CREAD;

    if (cfg->parity == UART_PARITY_EVEN) {
        tty.c_cflag |= PARENB;
        tty.c_iflag |= INPCK;
    } else if (cfg->parity == UART_PARITY_ODD) {
        tty.c_cflag |= PARENB | PARODD;
        tty.c_iflag |= INPCK;
    }
    if (cfg->stop_bits == 2) {
        tty.c_cflag |= CSTOPB;
    }
    if (cfg->hardware_flow_control) {
        tty.c_cflag |= CRTSCTS;
    }
    if (cfg->software_flow_control) {
        tty.c_iflag |= IXON | IXOFF | IXANY;
    } else {
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    }

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    tty.c_ispeed = cfg->baudrate;
    tty.c_ospeed = cfg->baudrate;

    if (ioctl(cfg->fd, TCSETS2, &tty) < 0) {
        return UART_ERR_IO;
    }
    if (ioctl(cfg->fd, TCGETS2, &tty) < 0) {
        return UART_ERR_IO;
    }
    if (tty.c_ispeed != cfg->baudrate || tty.c_ospeed != cfg->baudrate) {
        return UART_ERR_UNSUPPORTED;
    }

    return uart_base_configure_rs485(cfg);
}
#endif

uart_status_t uart_base_open_port(uart_config_t *cfg) {
#ifndef __linux__
    (void)cfg;
    return UART_ERR_UNSUPPORTED;
#else
    int open_flags;
    uart_status_t status;

    if (!uart_base_config_valid(cfg)) {
        return UART_ERR_PARAM;
    }

    cfg->fd = -1;
    open_flags = O_RDWR | O_NOCTTY;
    if (cfg->nonblocking) {
        open_flags |= O_NONBLOCK;
    }
    cfg->fd = open(cfg->device, open_flags);
    if (cfg->fd < 0) {
        return UART_ERR_IO;
    }

    status = uart_base_configure_port(cfg);
    if (status != UART_OK) {
        close(cfg->fd);
        cfg->fd = -1;
        return status;
    }

    return UART_OK;
#endif
}

uart_status_t uart_base_read_data(uart_config_t *cfg,
                                  uint8_t *buf,
                                  size_t buf_size,
                                  size_t min_read_size,
                                  int timeout_ms,
                                  size_t *read_size) {
#ifndef __linux__
    (void)cfg;
    (void)buf;
    (void)buf_size;
    (void)min_read_size;
    (void)timeout_ms;
    (void)read_size;
    return UART_ERR_UNSUPPORTED;
#else
    struct pollfd poll_fd;
    int poll_status;
    int64_t start_ms = 0;
    size_t total_read = 0;

    if (cfg == NULL || buf == NULL || buf_size == 0 || min_read_size == 0 ||
        min_read_size > buf_size || timeout_ms < -1 ||
        read_size == NULL || cfg->fd < 0) {
        return UART_ERR_PARAM;
    }

    *read_size = 0;
    if (timeout_ms >= 0) {
        struct timespec now;

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return UART_ERR_IO;
        }
        start_ms = (int64_t)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
    }

    poll_fd.fd = cfg->fd;
    poll_fd.events = POLLIN;
    while (total_read < min_read_size) {
        int wait_ms = timeout_ms;
        ssize_t result;

        if (timeout_ms >= 0) {
            struct timespec now;
            int64_t elapsed_ms;

            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
                return UART_ERR_IO;
            }
            elapsed_ms = ((int64_t)now.tv_sec * 1000LL +
                          now.tv_nsec / 1000000LL) - start_ms;
            wait_ms = elapsed_ms >= timeout_ms
                          ? 0
                          : timeout_ms - (int)elapsed_ms;
        }

        poll_fd.revents = 0;
        poll_status = poll(&poll_fd, 1, wait_ms);
        if (poll_status < 0) {
            if (errno == EINTR) {
                continue;
            }
            return UART_ERR_IO;
        }
        if (poll_status == 0) {
            break;
        }
        if ((poll_fd.revents & (POLLERR | POLLNVAL)) != 0) {
            return UART_ERR_IO;
        }
        if ((poll_fd.revents & POLLIN) == 0) {
            if ((poll_fd.revents & POLLHUP) != 0) {
                return UART_ERR_IO;
            }
            continue;
        }

        result = read(cfg->fd, buf + total_read, buf_size - total_read);
        if (result < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            return UART_ERR_IO;
        }
        if (result == 0) {
            break;
        }
        total_read += (size_t)result;
        if (total_read == buf_size) {
            break;
        }
    }

    *read_size = total_read;
    return total_read > 0 ? UART_OK : UART_NO_DATA;
#endif
}

uart_status_t uart_base_write_data(uart_config_t *cfg,
                                   const uint8_t *buf,
                                   size_t data_size,
                                   size_t *write_size) {
#ifndef __linux__
    (void)cfg;
    (void)buf;
    (void)data_size;
    (void)write_size;
    return UART_ERR_UNSUPPORTED;
#else
    ssize_t result;

    if (cfg == NULL || buf == NULL || data_size == 0 ||
        write_size == NULL || cfg->fd < 0) {
        return UART_ERR_PARAM;
    }

    *write_size = 0;
    result = write(cfg->fd, buf, data_size);
    if (result < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return UART_NO_DATA;
        }
        return UART_ERR_IO;
    }
    if (result == 0) {
        return UART_NO_DATA;
    }

    *write_size = (size_t)result;
    return UART_OK;
#endif
}

uart_status_t uart_base_flush(uart_config_t *cfg, uart_flush_t flush_mode) {
#ifndef __linux__
    (void)cfg;
    (void)flush_mode;
    return UART_ERR_UNSUPPORTED;
#else
    if (cfg == NULL || cfg->fd < 0 ||
        (flush_mode != UART_FLUSH_INPUT &&
         flush_mode != UART_FLUSH_OUTPUT &&
         flush_mode != UART_FLUSH_BOTH)) {
        return UART_ERR_PARAM;
    }

    if (ioctl(cfg->fd, TCFLSH, (int)flush_mode) < 0) {
        return UART_ERR_IO;
    }
    return UART_OK;
#endif
}

void uart_base_close_port(uart_config_t *cfg) {
    if (cfg == NULL) {
        return;
    }

#ifdef __linux__
    if (cfg->fd >= 0) {
        close(cfg->fd);
    }
#endif
    cfg->fd = -1;
}

const char *uart_base_status_string(uart_status_t status) {
    switch (status) {
        case UART_OK: return "ok";
        case UART_NO_DATA: return "no data available";
        case UART_ERR_PARAM: return "invalid UART parameter";
        case UART_ERR_IO: return "UART I/O error";
        case UART_ERR_UNSUPPORTED: return "UART configuration is not supported";
        default: return "unknown UART status";
    }
}
