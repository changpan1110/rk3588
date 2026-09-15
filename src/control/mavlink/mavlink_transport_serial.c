#define _DEFAULT_SOURCE
#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "mavlink_transport_serial.c"

#include "control/mavlink/mavlink_transport.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common/debug.h"
#include "input/serial/uart_base.h"

typedef struct {
    uart_config_t *uart;
} mavlink_serial_reply_t;

static int mavlink_serial_send(
    void *opaque,
    const uint8_t *data,
    size_t size) {
    mavlink_serial_reply_t *reply = (mavlink_serial_reply_t *)opaque;
    size_t offset = 0U;

    while (offset < size) {
        size_t written = 0U;
        uart_status_t status = uart_base_write_data(
            reply->uart,
            data + offset,
            size - offset,
            &written);

        if (status == UART_OK && written > 0U) {
            offset += written;
            continue;
        }
        if (status == UART_NO_DATA) {
            usleep(1000U);
            continue;
        }
        LOGW("MAVLink serial write failed: %s",
             uart_base_status_string(status));
        return -1;
    }
    return 0;
}

int mavlink_transport_serial_run(
    const mavlink_control_config_t *config,
    const mavlink_transport_runtime_t *runtime) {
    mavlink_serial_reply_t reply;
    uart_config_t uart;
    uint8_t buffer[2048];
    uart_status_t status;

    uart_base_config_init(&uart);
    if (snprintf(uart.device, sizeof(uart.device), "%s", config->serial_device) < 0 ||
        strlen(config->serial_device) >= sizeof(uart.device)) {
        return -1;
    }
    uart.baudrate = (uint32_t)config->serial_baud;
    uart.nonblocking = 1U;
    status = uart_base_open_port(&uart);
    if (status != UART_OK) {
        LOGE("MAVLink serial open %s failed: %s",
             config->serial_device,
             uart_base_status_string(status));
        return -1;
    }
    (void)uart_base_flush(&uart, UART_FLUSH_BOTH);
    reply.uart = &uart;
    LOGI("MAVLink serial opened: %s baud=%d",
         config->serial_device,
         config->serial_baud);
    while (!runtime->should_stop(runtime->opaque)) {
        size_t received = 0U;

        status = uart_base_read_data(
            &uart,
            buffer,
            sizeof(buffer),
            1U,
            MAVLINK_CONTROL_POLL_MS,
            &received);
        if (status == UART_NO_DATA) {
            continue;
        }
        if (status != UART_OK) {
            LOGW("MAVLink serial read failed: %s",
                 uart_base_status_string(status));
            break;
        }
        if (runtime->on_data(
                runtime->opaque,
                buffer,
                received,
                mavlink_serial_send,
                &reply) != 0) {
            break;
        }
    }

    uart_base_close_port(&uart);
    LOGI("MAVLink serial closed: %s", config->serial_device);
    return 0;
}
