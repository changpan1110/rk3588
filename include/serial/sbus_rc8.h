#ifndef SBUS_RC8_H
#define SBUS_RC8_H

#include "serial/sbus.h"

#define SBUS_RC8_CHANNEL_COUNT 8

typedef struct sbus_rc8 sbus_rc8_t;

/*
 * Initializes the 8-channel SBUS receiver and its receive thread.
 * device is the serial device only, for example "/dev/ttyACM0".
 */
sbus_status_t sbus_rc8_init(sbus_rc8_t **out_rc8,
                            const char *device);

/*
 * Receives data for one RC channel (channel_number is 1..8).
 * timeout_ms=0 is non-blocking; a positive value waits up to that duration.
 */
sbus_status_t sbus_rc8_channel_receive(
    sbus_rc8_t *rc8,
    unsigned int channel_number,
    uint16_t *channel_value,
    int timeout_ms);

void sbus_rc8_deinit(sbus_rc8_t *rc8);

#endif
