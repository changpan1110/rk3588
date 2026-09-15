#ifndef UART_BASE_H
#define UART_BASE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UART_DEVICE_PATH_MAX 128

typedef enum {
    UART_OK = 0,
    UART_NO_DATA = 1,
    UART_ERR_PARAM = -1,
    UART_ERR_IO = -2,
    UART_ERR_UNSUPPORTED = -3
} uart_status_t;

typedef enum {
    UART_PARITY_NONE = 0,
    UART_PARITY_EVEN,
    UART_PARITY_ODD
} uart_parity_t;

typedef enum {
    UART_FLUSH_INPUT = 0,
    UART_FLUSH_OUTPUT = 1,
    UART_FLUSH_BOTH = 2
} uart_flush_t;

typedef struct {
    char device[UART_DEVICE_PATH_MAX];
    uint32_t baudrate;
    uint8_t data_bits;
    uart_parity_t parity;
    uint8_t stop_bits;
    uint8_t hardware_flow_control;
    uint8_t software_flow_control;
    uint8_t nonblocking;
    uint8_t is_rs485;
    int fd;
} uart_config_t;

void uart_base_config_init(uart_config_t *cfg);
uart_status_t uart_base_open_port(uart_config_t *cfg);
/* timeout_ms: -1 waits forever, 0 polls once, positive values are milliseconds. */
uart_status_t uart_base_read_data(uart_config_t *cfg,
                                  uint8_t *buf,
                                  size_t buf_size,
                                  size_t min_read_size,
                                  int timeout_ms,
                                  size_t *read_size);
uart_status_t uart_base_write_data(uart_config_t *cfg,
                                   const uint8_t *buf,
                                   size_t data_size,
                                   size_t *write_size);
uart_status_t uart_base_flush(uart_config_t *cfg, uart_flush_t flush_mode);
void uart_base_close_port(uart_config_t *cfg);

const char *uart_base_status_string(uart_status_t status);

#ifdef __cplusplus
}
#endif

#endif
