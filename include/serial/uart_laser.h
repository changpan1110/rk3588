#ifndef UART_LASER_H
#define UART_LASER_H

#include "common/common.h"

typedef struct {
    float distance_m;
    int signal_level;
} laser_data_t;

app_status_t uart_laser_parse_line(const char *line, laser_data_t *data);

#endif
