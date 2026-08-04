#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "uart_base.c"

#include "serial/uart_base.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "common/debug.h"

static speed_t uart_baud_to_flag(int baudrate) {
    switch (baudrate) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default: return B115200;
    }
}

app_status_t uart_base_open_port(uart_config_t *cfg) {
    struct termios tty;

    if (cfg == NULL || cfg->device[0] == '\0') {
        return APP_ERR_PARAM;
    }

    cfg->fd = open(cfg->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (cfg->fd < 0) {
        LOGE("open uart failed: %s", cfg->device);
        return APP_ERR_IO;
    }

    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(cfg->fd, &tty) != 0) {
        close(cfg->fd);
        cfg->fd = -1;
        return APP_ERR_IO;
    }

    cfsetispeed(&tty, uart_baud_to_flag(cfg->baudrate));
    cfsetospeed(&tty, uart_baud_to_flag(cfg->baudrate));
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag = IGNBRK;
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(cfg->fd, TCSANOW, &tty) != 0) {
        close(cfg->fd);
        cfg->fd = -1;
        return APP_ERR_IO;
    }

    LOGI("uart open: %s baud=%d rs485=%d", cfg->device, cfg->baudrate, cfg->is_rs485);
    return APP_OK;
}

app_status_t uart_base_read_data(uart_config_t *cfg, uint8_t *buf, size_t buf_size, ssize_t *read_size) {
    ssize_t ret;

    if (cfg == NULL || buf == NULL || read_size == NULL) {
        return APP_ERR_PARAM;
    }

    ret = read(cfg->fd, buf, buf_size);
    if (ret < 0) {
        if (errno == EAGAIN) {
            *read_size = 0;
            return APP_OK;
        }
        return APP_ERR_IO;
    }

    *read_size = ret;
    return APP_OK;
}

void uart_base_close_port(uart_config_t *cfg) {
    if (cfg == NULL) {
        return;
    }

    if (cfg->fd >= 0) {
        close(cfg->fd);
        cfg->fd = -1;
    }
}
