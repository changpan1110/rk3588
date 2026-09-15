#define _POSIX_C_SOURCE 200809L

#include "control/sfl0603_laser/sfl0603_laser.h"

#include "input/serial/uart_base.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define SFL0603_LASER_FRAME_HEAD 0x55U
#define SFL0603_LASER_REQUEST_DATA_SIZE 2U
#define SFL0603_LASER_FRAME_OVERHEAD 4U
#define SFL0603_LASER_MAX_FRAME_SIZE \
    (SFL0603_LASER_MAX_DATA_SIZE + SFL0603_LASER_FRAME_OVERHEAD)
#define SFL0603_LASER_INTERBYTE_TIMEOUT_MS 100

typedef struct {
    uart_config_t uart;
    uint8_t rx_frame[SFL0603_LASER_MAX_FRAME_SIZE];
    size_t rx_size;
    size_t expected_size;
    int64_t last_rx_byte_ms;
    int initialized;
    int opened;
    int debug_enabled;
    int loghex_enabled;
} sfl0603_laser_context_t;

static sfl0603_laser_context_t g_sfl0603_laser;

static int64_t sfl0603_laser_now_ms(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static uint16_t sfl0603_laser_read_u16_be(const uint8_t *data) {
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t sfl0603_laser_read_u24_be(const uint8_t *data) {
    return ((uint32_t)data[0] << 16) |
           ((uint32_t)data[1] << 8) |
           (uint32_t)data[2];
}

static uint32_t sfl0603_laser_read_u32_be(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static void sfl0603_laser_write_u16_be(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)(value & 0xFFU);
}

static uint8_t sfl0603_laser_checksum(const uint8_t *data, size_t size) {
    uint8_t checksum = 0U;
    size_t i;

    for (i = 0U; i < size; ++i) {
        checksum ^= data[i];
    }
    return checksum;
}

static int sfl0603_laser_command_valid(uint8_t command) {
    switch (command) {
        case SFL0603_LASER_CMD_STANDBY:
        case SFL0603_LASER_CMD_SINGLE_MEASURE:
        case SFL0603_LASER_CMD_CONTINUOUS_MEASURE:
        case SFL0603_LASER_CMD_SELF_CHECK:
        case SFL0603_LASER_CMD_MIN_DISTANCE:
        case SFL0603_LASER_CMD_EMISSION_COUNT:
        case SFL0603_LASER_CMD_TARGET_MODE:
        case SFL0603_LASER_CMD_BAUDRATE:
            return 1;
        default:
            return 0;
    }
}

static void sfl0603_laser_loghex(const char *direction,
                                 const uint8_t *data,
                                 size_t size) {
    size_t i;

    if (!g_sfl0603_laser.loghex_enabled) {
        return;
    }
    fprintf(stderr, "[SFL0603][%s]", direction);
    for (i = 0U; i < size; ++i) {
        fprintf(stderr, " %02X", (unsigned)data[i]);
    }
    fprintf(stderr, "\n");
}

static size_t sfl0603_laser_build_frame(uint8_t command,
                                        const uint8_t *data,
                                        size_t data_length,
                                        uint8_t *frame) {
    size_t frame_size = data_length + SFL0603_LASER_FRAME_OVERHEAD;

    frame[0] = SFL0603_LASER_FRAME_HEAD;
    frame[1] = command;
    frame[2] = (uint8_t)data_length;
    if (data_length > 0U) {
        memcpy(frame + 3U, data, data_length);
    }
    frame[frame_size - 1U] = sfl0603_laser_checksum(frame, frame_size - 1U);
    return frame_size;
}

static sfl0603_laser_status_t sfl0603_laser_decode_frame(
    const uint8_t *raw,
    size_t raw_size,
    sfl0603_laser_frame_t *frame) {
    size_t expected_size;

    if (raw == NULL || frame == NULL || raw_size < SFL0603_LASER_FRAME_OVERHEAD) {
        return SFL0603_LASER_ERR_PARAM;
    }
    if (raw[0] != SFL0603_LASER_FRAME_HEAD ||
        raw[2] > SFL0603_LASER_MAX_DATA_SIZE) {
        return SFL0603_LASER_ERR_PROTOCOL;
    }
    expected_size = (size_t)raw[2] + SFL0603_LASER_FRAME_OVERHEAD;
    if (raw_size != expected_size) {
        return SFL0603_LASER_ERR_PROTOCOL;
    }
    if (sfl0603_laser_checksum(raw, raw_size - 1U) != raw[raw_size - 1U]) {
        return SFL0603_LASER_ERR_CHECKSUM;
    }

    memset(frame, 0, sizeof(*frame));
    frame->command = raw[1];
    frame->data_length = raw[2];
    if (frame->data_length > 0U) {
        memcpy(frame->data, raw + 3U, frame->data_length);
    }
    return SFL0603_LASER_OK;
}

static sfl0603_laser_status_t sfl0603_laser_write_all(const uint8_t *data,
                                                       size_t size) {
    size_t total = 0U;

    while (total < size) {
        size_t written = 0U;
        uart_status_t status = uart_base_write_data(&g_sfl0603_laser.uart,
                                                    data + total,
                                                    size - total,
                                                    &written);
        if (status != UART_OK || written == 0U) {
            return SFL0603_LASER_ERR_UART;
        }
        total += written;
    }
    return SFL0603_LASER_OK;
}

static sfl0603_laser_status_t sfl0603_laser_wait_for_command(
    uint8_t command,
    int timeout_ms,
    sfl0603_laser_frame_t *frame) {
    int64_t start_ms;

    if (timeout_ms < 0 || frame == NULL) {
        return SFL0603_LASER_ERR_PARAM;
    }
    start_ms = sfl0603_laser_now_ms();
    for (;;) {
        int64_t elapsed_ms = sfl0603_laser_now_ms() - start_ms;
        int remaining_ms;
        sfl0603_laser_status_t status;

        if (elapsed_ms >= timeout_ms) {
            return SFL0603_LASER_ERR_TIMEOUT;
        }
        remaining_ms = timeout_ms - (int)elapsed_ms;
        status = sfl0603_laser_read_frame(remaining_ms, frame);
        if (status != SFL0603_LASER_OK) {
            return status;
        }
        if (frame->command == command) {
            return SFL0603_LASER_OK;
        }
        if (g_sfl0603_laser.debug_enabled) {
            fprintf(stderr,
                    "[SFL0603] ignore command 0x%02X while waiting for 0x%02X\n",
                    (unsigned)frame->command,
                    (unsigned)command);
        }
    }
}

static sfl0603_laser_status_t sfl0603_laser_request(
    uint8_t command,
    const uint8_t data[SFL0603_LASER_REQUEST_DATA_SIZE],
    int timeout_ms,
    sfl0603_laser_frame_t *response) {
    sfl0603_laser_status_t status;

    status = sfl0603_laser_send_command((sfl0603_laser_command_t)command,
                                        data,
                                        SFL0603_LASER_REQUEST_DATA_SIZE);
    if (status != SFL0603_LASER_OK) {
        return status;
    }
    return sfl0603_laser_wait_for_command(command, timeout_ms, response);
}

static sfl0603_laser_status_t sfl0603_laser_expect_echo(
    uint8_t command,
    const uint8_t expected_data[SFL0603_LASER_REQUEST_DATA_SIZE],
    int timeout_ms) {
    sfl0603_laser_frame_t response;
    sfl0603_laser_status_t status = sfl0603_laser_request(
        command, expected_data, timeout_ms, &response);

    if (status != SFL0603_LASER_OK) {
        return status;
    }
    if (response.data_length != SFL0603_LASER_REQUEST_DATA_SIZE ||
        memcmp(response.data,
               expected_data,
               SFL0603_LASER_REQUEST_DATA_SIZE) != 0) {
        return SFL0603_LASER_ERR_PROTOCOL;
    }
    return SFL0603_LASER_OK;
}

static sfl0603_laser_status_t sfl0603_laser_parse_measurement(
    const sfl0603_laser_frame_t *frame,
    sfl0603_laser_measurement_t *measurement) {
    uint8_t flags;

    if (frame == NULL || measurement == NULL ||
        (frame->command != SFL0603_LASER_CMD_SINGLE_MEASURE &&
         frame->command != SFL0603_LASER_CMD_CONTINUOUS_MEASURE) ||
        frame->data_length != SFL0603_LASER_MAX_DATA_SIZE) {
        return SFL0603_LASER_ERR_PROTOCOL;
    }

    memset(measurement, 0, sizeof(*measurement));
    flags = frame->data[0];
    measurement->flags = flags;
    measurement->distance_dm[0] = sfl0603_laser_read_u24_be(frame->data + 1U);
    measurement->distance_dm[1] = sfl0603_laser_read_u24_be(frame->data + 4U);
    measurement->distance_dm[2] = sfl0603_laser_read_u24_be(frame->data + 7U);
    measurement->main_wave_present = (flags & 0x80U) != 0U;
    measurement->echo_present = (flags & 0x40U) != 0U;
    measurement->laser_ok = (flags & 0x20U) != 0U;
    measurement->timing_ok = (flags & 0x10U) != 0U;
    measurement->apd_ok = (flags & 0x04U) != 0U;
    measurement->front_target_present = (flags & 0x02U) != 0U;
    measurement->rear_target_present = (flags & 0x01U) != 0U;
    return SFL0603_LASER_OK;
}

sfl0603_laser_status_t sfl0603_laser_init(void) {
    if (g_sfl0603_laser.opened) {
        uart_base_close_port(&g_sfl0603_laser.uart);
    }
    memset(&g_sfl0603_laser, 0, sizeof(g_sfl0603_laser));
    uart_base_config_init(&g_sfl0603_laser.uart);
    g_sfl0603_laser.initialized = 1;
    return SFL0603_LASER_OK;
}

sfl0603_laser_status_t sfl0603_laser_open(const char *device,
                                          uint32_t baudrate) {
    size_t device_length;
    uart_status_t status;

    if (device == NULL || device[0] == '\0' || baudrate == 0U) {
        return SFL0603_LASER_ERR_PARAM;
    }
    if (!g_sfl0603_laser.initialized) {
        (void)sfl0603_laser_init();
    }
    if (g_sfl0603_laser.opened) {
        sfl0603_laser_close();
    }
    device_length = strlen(device);
    if (device_length >= sizeof(g_sfl0603_laser.uart.device)) {
        return SFL0603_LASER_ERR_PARAM;
    }

    uart_base_config_init(&g_sfl0603_laser.uart);
    memcpy(g_sfl0603_laser.uart.device, device, device_length + 1U);
    g_sfl0603_laser.uart.baudrate = baudrate;
    g_sfl0603_laser.uart.data_bits = 8U;
    g_sfl0603_laser.uart.parity = UART_PARITY_NONE;
    g_sfl0603_laser.uart.stop_bits = 1U;
    status = uart_base_open_port(&g_sfl0603_laser.uart);
    if (status != UART_OK) {
        return SFL0603_LASER_ERR_UART;
    }
    status = uart_base_flush(&g_sfl0603_laser.uart, UART_FLUSH_BOTH);
    if (status != UART_OK) {
        uart_base_close_port(&g_sfl0603_laser.uart);
        return SFL0603_LASER_ERR_UART;
    }

    g_sfl0603_laser.opened = 1;
    g_sfl0603_laser.rx_size = 0U;
    g_sfl0603_laser.expected_size = 0U;
    if (g_sfl0603_laser.debug_enabled) {
        fprintf(stderr,
                "[SFL0603] opened %s baud=%u format=8N1\n",
                device,
                (unsigned)baudrate);
    }
    return SFL0603_LASER_OK;
}

void sfl0603_laser_close(void) {
    if (g_sfl0603_laser.opened) {
        uart_base_close_port(&g_sfl0603_laser.uart);
    }
    g_sfl0603_laser.opened = 0;
    g_sfl0603_laser.rx_size = 0U;
    g_sfl0603_laser.expected_size = 0U;
}

int sfl0603_laser_is_open(void) {
    return g_sfl0603_laser.opened;
}

uint32_t sfl0603_laser_get_baudrate(void) {
    return g_sfl0603_laser.uart.baudrate;
}

void sfl0603_laser_set_debug(int enabled) {
    g_sfl0603_laser.debug_enabled = enabled ? 1 : 0;
}

void sfl0603_laser_set_loghex(int enabled) {
    g_sfl0603_laser.loghex_enabled = enabled ? 1 : 0;
}

int sfl0603_laser_get_debug(void) {
    return g_sfl0603_laser.debug_enabled;
}

int sfl0603_laser_get_loghex(void) {
    return g_sfl0603_laser.loghex_enabled;
}

sfl0603_laser_status_t sfl0603_laser_send_command(
    sfl0603_laser_command_t command,
    const uint8_t *data,
    size_t data_length) {
    uint8_t frame[SFL0603_LASER_MAX_FRAME_SIZE];
    size_t frame_size;
    sfl0603_laser_status_t status;

    if (!g_sfl0603_laser.opened) {
        return SFL0603_LASER_ERR_STATE;
    }
    if (!sfl0603_laser_command_valid((uint8_t)command) ||
        data_length > SFL0603_LASER_MAX_DATA_SIZE ||
        (data_length > 0U && data == NULL)) {
        return SFL0603_LASER_ERR_PARAM;
    }

    frame_size = sfl0603_laser_build_frame((uint8_t)command,
                                           data,
                                           data_length,
                                           frame);
    status = sfl0603_laser_write_all(frame, frame_size);
    if (status == SFL0603_LASER_OK) {
        sfl0603_laser_loghex("TX", frame, frame_size);
    }
    return status;
}

sfl0603_laser_status_t sfl0603_laser_read_frame(
    int timeout_ms,
    sfl0603_laser_frame_t *frame) {
    int64_t start_ms;

    if (!g_sfl0603_laser.opened) {
        return SFL0603_LASER_ERR_STATE;
    }
    if (timeout_ms < -1 || frame == NULL) {
        return SFL0603_LASER_ERR_PARAM;
    }

    start_ms = sfl0603_laser_now_ms();
    for (;;) {
        uint8_t byte = 0U;
        size_t read_size = 0U;
        int wait_ms = timeout_ms;
        uart_status_t uart_status;

        if (g_sfl0603_laser.rx_size > 0U &&
            sfl0603_laser_now_ms() - g_sfl0603_laser.last_rx_byte_ms >=
                SFL0603_LASER_INTERBYTE_TIMEOUT_MS) {
            g_sfl0603_laser.rx_size = 0U;
            g_sfl0603_laser.expected_size = 0U;
        }
        if (timeout_ms > 0) {
            int64_t elapsed_ms = sfl0603_laser_now_ms() - start_ms;
            if (elapsed_ms >= timeout_ms) {
                return SFL0603_LASER_ERR_TIMEOUT;
            }
            wait_ms = timeout_ms - (int)elapsed_ms;
        } else if (timeout_ms == 0) {
            wait_ms = 0;
        }

        uart_status = uart_base_read_data(&g_sfl0603_laser.uart,
                                          &byte,
                                          1U,
                                          1U,
                                          wait_ms,
                                          &read_size);
        if (uart_status == UART_NO_DATA) {
            return timeout_ms == 0 ? SFL0603_LASER_NO_DATA
                                   : SFL0603_LASER_ERR_TIMEOUT;
        }
        if (uart_status != UART_OK || read_size != 1U) {
            return SFL0603_LASER_ERR_UART;
        }
        g_sfl0603_laser.last_rx_byte_ms = sfl0603_laser_now_ms();

        if (g_sfl0603_laser.rx_size == 0U) {
            if (byte != SFL0603_LASER_FRAME_HEAD) {
                continue;
            }
            g_sfl0603_laser.rx_frame[g_sfl0603_laser.rx_size++] = byte;
            continue;
        }

        g_sfl0603_laser.rx_frame[g_sfl0603_laser.rx_size++] = byte;
        if (g_sfl0603_laser.rx_size == 3U) {
            if (g_sfl0603_laser.rx_frame[2] > SFL0603_LASER_MAX_DATA_SIZE) {
                g_sfl0603_laser.rx_size = 0U;
                g_sfl0603_laser.expected_size = 0U;
                return SFL0603_LASER_ERR_PROTOCOL;
            }
            g_sfl0603_laser.expected_size =
                (size_t)g_sfl0603_laser.rx_frame[2] +
                SFL0603_LASER_FRAME_OVERHEAD;
        }
        if (g_sfl0603_laser.expected_size > 0U &&
            g_sfl0603_laser.rx_size == g_sfl0603_laser.expected_size) {
            sfl0603_laser_status_t status;

            sfl0603_laser_loghex("RX",
                                 g_sfl0603_laser.rx_frame,
                                 g_sfl0603_laser.rx_size);
            status = sfl0603_laser_decode_frame(g_sfl0603_laser.rx_frame,
                                                g_sfl0603_laser.rx_size,
                                                frame);
            g_sfl0603_laser.rx_size = 0U;
            g_sfl0603_laser.expected_size = 0U;
            return status;
        }
    }
}

sfl0603_laser_status_t sfl0603_laser_standby(int timeout_ms) {
    static const uint8_t data[2] = {0x00U, 0x00U};
    return sfl0603_laser_expect_echo(SFL0603_LASER_CMD_STANDBY,
                                    data,
                                    timeout_ms);
}

sfl0603_laser_status_t sfl0603_laser_measure_once(
    int timeout_ms,
    sfl0603_laser_measurement_t *measurement) {
    static const uint8_t data[2] = {0x00U, 0x00U};
    sfl0603_laser_frame_t response;
    sfl0603_laser_status_t status;

    if (measurement == NULL) {
        return SFL0603_LASER_ERR_PARAM;
    }
    status = sfl0603_laser_request(SFL0603_LASER_CMD_SINGLE_MEASURE,
                                   data,
                                   timeout_ms,
                                   &response);
    if (status != SFL0603_LASER_OK) {
        return status;
    }
    return sfl0603_laser_parse_measurement(&response, measurement);
}

sfl0603_laser_status_t sfl0603_laser_start_continuous(
    uint16_t period_ms,
    int timeout_ms,
    sfl0603_laser_measurement_t *first_measurement) {
    uint8_t data[2];
    sfl0603_laser_frame_t response;
    sfl0603_laser_measurement_t ignored_measurement;
    sfl0603_laser_status_t status;

    if (period_ms == 0U) {
        return SFL0603_LASER_ERR_PARAM;
    }
    sfl0603_laser_write_u16_be(data, period_ms);
    status = sfl0603_laser_request(SFL0603_LASER_CMD_CONTINUOUS_MEASURE,
                                   data,
                                   timeout_ms,
                                   &response);
    if (status != SFL0603_LASER_OK) {
        return status;
    }
    return sfl0603_laser_parse_measurement(
        &response,
        first_measurement != NULL ? first_measurement : &ignored_measurement);
}

sfl0603_laser_status_t sfl0603_laser_read_measurement(
    int timeout_ms,
    sfl0603_laser_measurement_t *measurement) {
    sfl0603_laser_frame_t frame;
    sfl0603_laser_status_t status;

    if (measurement == NULL) {
        return SFL0603_LASER_ERR_PARAM;
    }
    status = sfl0603_laser_wait_for_command(
        SFL0603_LASER_CMD_CONTINUOUS_MEASURE, timeout_ms, &frame);
    if (status != SFL0603_LASER_OK) {
        return status;
    }
    return sfl0603_laser_parse_measurement(&frame, measurement);
}

sfl0603_laser_status_t sfl0603_laser_run_self_check(
    int timeout_ms,
    sfl0603_laser_self_check_t *result) {
    static const uint8_t data[2] = {0x00U, 0x00U};
    sfl0603_laser_frame_t response;
    sfl0603_laser_status_t status;

    if (result == NULL) {
        return SFL0603_LASER_ERR_PARAM;
    }
    status = sfl0603_laser_request(SFL0603_LASER_CMD_SELF_CHECK,
                                   data,
                                   timeout_ms,
                                   &response);
    if (status != SFL0603_LASER_OK) {
        return status;
    }
    if (response.data_length != 8U) {
        return SFL0603_LASER_ERR_PROTOCOL;
    }

    result->negative_5v_centi_volts =
        sfl0603_laser_read_u16_be(response.data);
    result->blind_zone_m = sfl0603_laser_read_u16_be(response.data + 2U);
    result->apd_high_voltage_v = response.data[4];
    result->apd_temperature_c = (int8_t)response.data[5];
    result->positive_5v_centi_volts =
        sfl0603_laser_read_u16_be(response.data + 6U);
    return SFL0603_LASER_OK;
}

sfl0603_laser_status_t sfl0603_laser_set_min_distance(
    uint16_t distance_m,
    int timeout_ms) {
    uint8_t data[2];

    sfl0603_laser_write_u16_be(data, distance_m);
    return sfl0603_laser_expect_echo(SFL0603_LASER_CMD_MIN_DISTANCE,
                                    data,
                                    timeout_ms);
}

sfl0603_laser_status_t sfl0603_laser_query_emission_count(
    int timeout_ms,
    uint32_t *count) {
    static const uint8_t data[2] = {0x00U, 0x00U};
    sfl0603_laser_frame_t response;
    sfl0603_laser_status_t status;

    if (count == NULL) {
        return SFL0603_LASER_ERR_PARAM;
    }
    status = sfl0603_laser_request(SFL0603_LASER_CMD_EMISSION_COUNT,
                                   data,
                                   timeout_ms,
                                   &response);
    if (status != SFL0603_LASER_OK) {
        return status;
    }
    if (response.data_length != 4U) {
        return SFL0603_LASER_ERR_PROTOCOL;
    }
    *count = sfl0603_laser_read_u32_be(response.data);
    return SFL0603_LASER_OK;
}

sfl0603_laser_status_t sfl0603_laser_set_target_mode(
    sfl0603_laser_target_mode_t mode,
    int timeout_ms) {
    uint8_t data[2];

    if (mode != SFL0603_LASER_TARGET_SINGLE &&
        mode != SFL0603_LASER_TARGET_THREE &&
        mode != SFL0603_LASER_TARGET_FIRST_LAST) {
        return SFL0603_LASER_ERR_PARAM;
    }
    sfl0603_laser_write_u16_be(data, (uint16_t)mode);
    return sfl0603_laser_expect_echo(SFL0603_LASER_CMD_TARGET_MODE,
                                    data,
                                    timeout_ms);
}

sfl0603_laser_status_t sfl0603_laser_change_baudrate(
    uint32_t new_baudrate,
    int timeout_ms) {
    char device[UART_DEVICE_PATH_MAX];
    uint32_t scaled_baudrate;
    uint8_t data[2];
    sfl0603_laser_status_t status;

    if (!g_sfl0603_laser.opened || new_baudrate == 0U ||
        new_baudrate % 100U != 0U) {
        return SFL0603_LASER_ERR_PARAM;
    }
    scaled_baudrate = new_baudrate / 100U;
    if (scaled_baudrate > 0xFFFFU) {
        return SFL0603_LASER_ERR_PARAM;
    }

    sfl0603_laser_write_u16_be(data, (uint16_t)scaled_baudrate);
    status = sfl0603_laser_expect_echo(SFL0603_LASER_CMD_BAUDRATE,
                                      data,
                                      timeout_ms);
    if (status != SFL0603_LASER_OK) {
        return status;
    }

    memcpy(device, g_sfl0603_laser.uart.device, sizeof(device));
    device[sizeof(device) - 1U] = '\0';
    sfl0603_laser_close();
    return sfl0603_laser_open(device, new_baudrate);
}

double sfl0603_laser_distance_m(uint32_t distance_dm) {
    return (double)distance_dm / 10.0;
}

sfl0603_laser_status_t sfl0603_laser_self_test(void) {
    static const struct {
        uint8_t command;
        uint8_t data[2];
        uint8_t expected[6];
    } request_cases[] = {
        {0x00U, {0x00U, 0x00U}, {0x55U, 0x00U, 0x02U, 0x00U, 0x00U, 0x57U}},
        {0x01U, {0x00U, 0x00U}, {0x55U, 0x01U, 0x02U, 0x00U, 0x00U, 0x56U}},
        {0x02U, {0x03U, 0xE8U}, {0x55U, 0x02U, 0x02U, 0x03U, 0xE8U, 0xBEU}},
        {0x03U, {0x00U, 0x00U}, {0x55U, 0x03U, 0x02U, 0x00U, 0x00U, 0x54U}},
        {0x04U, {0x00U, 0x64U}, {0x55U, 0x04U, 0x02U, 0x00U, 0x64U, 0x37U}},
        {0x06U, {0x00U, 0x00U}, {0x55U, 0x06U, 0x02U, 0x00U, 0x00U, 0x51U}},
        {0x22U, {0x00U, 0x01U}, {0x55U, 0x22U, 0x02U, 0x00U, 0x01U, 0x74U}},
        {0x26U, {0x04U, 0x80U}, {0x55U, 0x26U, 0x02U, 0x04U, 0x80U, 0xF5U}}
    };
    static const uint8_t measurement_raw[14] = {
        0x55U, 0x01U, 0x0AU, 0xFCU,
        0x00U, 0x00U, 0x64U,
        0x00U, 0x00U, 0xC8U,
        0x00U, 0x01U, 0x2CU, 0x23U
    };
    size_t i;

    for (i = 0U; i < sizeof(request_cases) / sizeof(request_cases[0]); ++i) {
        uint8_t actual[6];
        size_t actual_size = sfl0603_laser_build_frame(
            request_cases[i].command,
            request_cases[i].data,
            sizeof(request_cases[i].data),
            actual);
        if (actual_size != sizeof(actual) ||
            memcmp(actual, request_cases[i].expected, sizeof(actual)) != 0) {
            return SFL0603_LASER_ERR_PROTOCOL;
        }
    }

    {
        sfl0603_laser_frame_t frame;
        sfl0603_laser_measurement_t measurement;
        sfl0603_laser_status_t status = sfl0603_laser_decode_frame(
            measurement_raw, sizeof(measurement_raw), &frame);
        if (status != SFL0603_LASER_OK ||
            sfl0603_laser_parse_measurement(&frame, &measurement) !=
                SFL0603_LASER_OK ||
            measurement.distance_dm[0] != 100U ||
            measurement.distance_dm[1] != 200U ||
            measurement.distance_dm[2] != 300U ||
            !measurement.main_wave_present ||
            !measurement.echo_present ||
            !measurement.laser_ok ||
            !measurement.timing_ok ||
            !measurement.apd_ok) {
            return SFL0603_LASER_ERR_PROTOCOL;
        }
    }
    return SFL0603_LASER_OK;
}

const char *sfl0603_laser_command_name(uint8_t command) {
    switch (command) {
        case SFL0603_LASER_CMD_STANDBY: return "standby";
        case SFL0603_LASER_CMD_SINGLE_MEASURE: return "single measure";
        case SFL0603_LASER_CMD_CONTINUOUS_MEASURE: return "continuous measure";
        case SFL0603_LASER_CMD_SELF_CHECK: return "self check";
        case SFL0603_LASER_CMD_MIN_DISTANCE: return "minimum distance";
        case SFL0603_LASER_CMD_EMISSION_COUNT: return "emission count";
        case SFL0603_LASER_CMD_TARGET_MODE: return "target mode";
        case SFL0603_LASER_CMD_BAUDRATE: return "baudrate";
        default: return "unknown";
    }
}

const char *sfl0603_laser_status_string(sfl0603_laser_status_t status) {
    switch (status) {
        case SFL0603_LASER_OK: return "ok";
        case SFL0603_LASER_NO_DATA: return "no data";
        case SFL0603_LASER_ERR_PARAM: return "invalid parameter";
        case SFL0603_LASER_ERR_STATE: return "invalid state";
        case SFL0603_LASER_ERR_UART: return "UART error";
        case SFL0603_LASER_ERR_TIMEOUT: return "response timeout";
        case SFL0603_LASER_ERR_CHECKSUM: return "checksum error";
        case SFL0603_LASER_ERR_PROTOCOL: return "protocol error";
        default: return "unknown status";
    }
}
