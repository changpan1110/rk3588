#include "serial/uart_laser.h"

#include <stdio.h>

app_status_t uart_laser_parse_line(const char *line, laser_data_t *data) {
    float distance_m;
    int signal_level;
    int matched;

    if (line == NULL || data == NULL) {
        return APP_ERR_PARAM;
    }

    matched = sscanf(line, "DIST=%f,SIGNAL=%d", &distance_m, &signal_level);
    if (matched != 2) {
        return APP_ERR_PARAM;
    }

    data->distance_m = distance_m;
    data->signal_level = signal_level;
    return APP_OK;
}
