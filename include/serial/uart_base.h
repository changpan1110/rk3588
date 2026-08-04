#ifndef UART_BASE_H
#define UART_BASE_H

#include "common/common.h"

app_status_t uart_base_open_port(uart_config_t *cfg);
app_status_t uart_base_read_data(uart_config_t *cfg, uint8_t *buf, size_t buf_size, ssize_t *read_size);
void uart_base_close_port(uart_config_t *cfg);

#endif
