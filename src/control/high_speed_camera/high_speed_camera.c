#define LOG_LOCAL_LEVEL LOG_LEVEL_TRACE
#define LOG_FILE_NAME "high_speed_camera.c"

#include "control/high_speed_camera/high_speed_camera.h"

#include "common/debug.h"
#include "input/serial/uart_base.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define HIGH_SPEED_CAMERA_FRAME_HEAD 0xAAU
#define HIGH_SPEED_CAMERA_FRAME_TAIL 0x55U
#define HIGH_SPEED_CAMERA_INTERBYTE_TIMEOUT_MS 100
#define HIGH_SPEED_CAMERA_KEY_COUNT 7U

typedef struct {
    uart_config_t uart;
    uint8_t key_counts[HIGH_SPEED_CAMERA_KEY_COUNT + 1U];
    uint8_t heartbeat_count;
    uint8_t error_count;
    uint8_t rx_frame[HIGH_SPEED_CAMERA_FRAME_SIZE];
    size_t rx_size;
    int64_t last_rx_byte_ms;
    int64_t last_valid_frame_ms;
    int initialized;
    int opened;
    int connected;
    int debug_enabled;
    int loghex_enabled;
} high_speed_camera_context_t;

static high_speed_camera_context_t g_high_speed_camera;

static int64_t high_speed_camera_now_ms(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static uint8_t high_speed_camera_next_count(uint8_t *count) {
    if (*count == 0U || *count == 255U) {
        *count = 1U;
    } else {
        ++(*count);
    }
    return *count;
}

static uint16_t high_speed_camera_crc16(const uint8_t *data, size_t size) {
    uint16_t crc = 0xFFFFU;
    size_t i;

    for (i = 0; i < size; ++i) {
        int bit;

        crc ^= (uint16_t)data[i] << 8;
        for (bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

static void high_speed_camera_build_frame(uint8_t type,
                                          uint8_t data,
                                          uint8_t count,
                                          uint8_t frame[HIGH_SPEED_CAMERA_FRAME_SIZE]) {
    uint16_t crc;

    frame[0] = HIGH_SPEED_CAMERA_FRAME_HEAD;
    frame[1] = type;
    frame[2] = data;
    crc = high_speed_camera_crc16(frame, 3U);
    frame[3] = (uint8_t)(crc >> 8);
    frame[4] = (uint8_t)(crc & 0xFFU);
    frame[5] = count;
    frame[6] = HIGH_SPEED_CAMERA_FRAME_TAIL;
}

static void high_speed_camera_loghex(const char *direction,
                                     const uint8_t *data,
                                     size_t size) {
    size_t i;

    if (!g_high_speed_camera.loghex_enabled) {
        return;
    }
    fprintf(stderr, "[HIGH_SPEED_CAMERA][LOGHEX][%s]", direction);
    for (i = 0; i < size; ++i) {
        fprintf(stderr, " %02X", (unsigned)data[i]);
    }
    fprintf(stderr, "\n");
}

static high_speed_camera_status_t high_speed_camera_write_frame(
    const uint8_t frame[HIGH_SPEED_CAMERA_FRAME_SIZE]) {
    size_t total = 0U;

    if (!g_high_speed_camera.opened) {
        return HIGH_SPEED_CAMERA_ERR_STATE;
    }
    while (total < HIGH_SPEED_CAMERA_FRAME_SIZE) {
        size_t written = 0U;
        uart_status_t uart_status = uart_base_write_data(
            &g_high_speed_camera.uart,
            frame + total,
            HIGH_SPEED_CAMERA_FRAME_SIZE - total,
            &written);

        if (uart_status != UART_OK || written == 0U) {
            return HIGH_SPEED_CAMERA_ERR_UART;
        }
        total += written;
    }
    high_speed_camera_loghex("TX", frame, HIGH_SPEED_CAMERA_FRAME_SIZE);
    return HIGH_SPEED_CAMERA_OK;
}

static high_speed_camera_status_t high_speed_camera_send_frame(uint8_t type,
                                                               uint8_t data,
                                                               uint8_t count) {
    uint8_t frame[HIGH_SPEED_CAMERA_FRAME_SIZE];

    high_speed_camera_build_frame(type, data, count, frame);
    if (g_high_speed_camera.debug_enabled) {
        LOGT("send type=%s(0x%02X) data=0x%02X count=%u",
             high_speed_camera_frame_type_name(type),
             (unsigned)type,
             (unsigned)data,
             (unsigned)count);
    }
    return high_speed_camera_write_frame(frame);
}

static int high_speed_camera_frame_fields_valid(uint8_t type,
                                                uint8_t data,
                                                uint8_t count) {
    if (count == 0U) {
        return 0;
    }
    switch (type) {
        case HIGH_SPEED_CAMERA_FRAME_KEY_REQUEST:
            return data >= HIGH_SPEED_CAMERA_KEY_PREV &&
                   data <= HIGH_SPEED_CAMERA_KEY_UNLOCK;
        case HIGH_SPEED_CAMERA_FRAME_HEARTBEAT_REQUEST:
            return data == 0U;
        case HIGH_SPEED_CAMERA_FRAME_HEARTBEAT_RESPONSE:
            return data <= HIGH_SPEED_CAMERA_KEY_UNLOCK;
        case HIGH_SPEED_CAMERA_FRAME_ERROR_RESPONSE:
            return data == 0U;
        default:
            return 0;
    }
}

static high_speed_camera_status_t high_speed_camera_validate_frame(
    const uint8_t frame[HIGH_SPEED_CAMERA_FRAME_SIZE]) {
    uint16_t expected_crc;
    uint16_t received_crc;

    if (frame[0] != HIGH_SPEED_CAMERA_FRAME_HEAD ||
        frame[6] != HIGH_SPEED_CAMERA_FRAME_TAIL) {
        return HIGH_SPEED_CAMERA_ERR_PROTOCOL;
    }
    expected_crc = high_speed_camera_crc16(frame, 3U);
    received_crc = (uint16_t)(((uint16_t)frame[3] << 8) | frame[4]);
    if (received_crc != expected_crc) {
        return HIGH_SPEED_CAMERA_ERR_CRC;
    }
    if (!high_speed_camera_frame_fields_valid(frame[1], frame[2], frame[5])) {
        return HIGH_SPEED_CAMERA_ERR_PROTOCOL;
    }
    return HIGH_SPEED_CAMERA_OK;
}

static void high_speed_camera_update_disconnect_state(void) {
    int64_t now_ms;

    if (!g_high_speed_camera.connected) {
        return;
    }
    now_ms = high_speed_camera_now_ms();
    if (now_ms - g_high_speed_camera.last_valid_frame_ms >=
        HIGH_SPEED_CAMERA_DISCONNECT_TIMEOUT_MS) {
        g_high_speed_camera.connected = 0;
        if (g_high_speed_camera.debug_enabled) {
            LOGW("no valid frame for %d ms, key board disconnected",
                 HIGH_SPEED_CAMERA_DISCONNECT_TIMEOUT_MS);
        }
    }
}

static high_speed_camera_status_t high_speed_camera_reject_frame(
    high_speed_camera_status_t reason) {
    uint8_t count = high_speed_camera_next_count(&g_high_speed_camera.error_count);
    high_speed_camera_status_t send_status;

    if (g_high_speed_camera.debug_enabled) {
        LOGW("invalid frame: %s; send NAK count=%u",
             high_speed_camera_status_string(reason),
             (unsigned)count);
    }
    send_status = high_speed_camera_send_frame(
        HIGH_SPEED_CAMERA_FRAME_ERROR_RESPONSE, 0U, count);
    return send_status == HIGH_SPEED_CAMERA_OK ? reason : send_status;
}

high_speed_camera_status_t high_speed_camera_init(void) {
    if (g_high_speed_camera.opened) {
        uart_base_close_port(&g_high_speed_camera.uart);
    }
    memset(&g_high_speed_camera, 0, sizeof(g_high_speed_camera));
    uart_base_config_init(&g_high_speed_camera.uart);
    g_high_speed_camera.initialized = 1;
    return HIGH_SPEED_CAMERA_OK;
}

high_speed_camera_status_t high_speed_camera_open(const char *device,
                                                  uint32_t baudrate) {
    uart_status_t uart_status;
    size_t device_length;

    if (device == NULL || device[0] == '\0' || baudrate == 0U) {
        return HIGH_SPEED_CAMERA_ERR_PARAM;
    }
    if (!g_high_speed_camera.initialized) {
        (void)high_speed_camera_init();
    }
    if (g_high_speed_camera.opened) {
        high_speed_camera_close();
    }
    device_length = strlen(device);
    if (device_length >= sizeof(g_high_speed_camera.uart.device)) {
        return HIGH_SPEED_CAMERA_ERR_PARAM;
    }

    uart_base_config_init(&g_high_speed_camera.uart);
    memcpy(g_high_speed_camera.uart.device, device, device_length + 1U);
    g_high_speed_camera.uart.baudrate = baudrate;
    g_high_speed_camera.uart.data_bits = 8U;
    g_high_speed_camera.uart.parity = UART_PARITY_NONE;
    g_high_speed_camera.uart.stop_bits = 1U;
    g_high_speed_camera.uart.hardware_flow_control = 0U;
    g_high_speed_camera.uart.software_flow_control = 0U;
    /* Protocol frames are only seven bytes. Non-blocking mode prevents a
     * disconnected/stalled UART from trapping the camera worker in write(). */
    g_high_speed_camera.uart.nonblocking = 1U;
    g_high_speed_camera.uart.is_rs485 = 0U;

    uart_status = uart_base_open_port(&g_high_speed_camera.uart);
    if (uart_status != UART_OK) {
        return HIGH_SPEED_CAMERA_ERR_UART;
    }
    uart_status = uart_base_flush(&g_high_speed_camera.uart, UART_FLUSH_BOTH);
    if (uart_status != UART_OK) {
        uart_base_close_port(&g_high_speed_camera.uart);
        return HIGH_SPEED_CAMERA_ERR_UART;
    }
    g_high_speed_camera.opened = 1;
    g_high_speed_camera.connected = 0;
    g_high_speed_camera.rx_size = 0U;
    if (g_high_speed_camera.debug_enabled) {
        LOGD("opened %s baud=%u format=8N1",
             device,
             (unsigned)baudrate);
    }
    return HIGH_SPEED_CAMERA_OK;
}

void high_speed_camera_close(void) {
    if (g_high_speed_camera.opened) {
        uart_base_close_port(&g_high_speed_camera.uart);
        if (g_high_speed_camera.debug_enabled) {
            LOGD("serial port closed");
        }
    }
    g_high_speed_camera.opened = 0;
    g_high_speed_camera.connected = 0;
    g_high_speed_camera.rx_size = 0U;
}

int high_speed_camera_is_open(void) {
    return g_high_speed_camera.opened;
}

int high_speed_camera_is_connected(void) {
    high_speed_camera_update_disconnect_state();
    return g_high_speed_camera.connected;
}

void high_speed_camera_set_debug(int enabled) {
    g_high_speed_camera.debug_enabled = enabled ? 1 : 0;
}

void high_speed_camera_set_loghex(int enabled) {
    g_high_speed_camera.loghex_enabled = enabled ? 1 : 0;
}

int high_speed_camera_get_debug(void) {
    return g_high_speed_camera.debug_enabled;
}

int high_speed_camera_get_loghex(void) {
    return g_high_speed_camera.loghex_enabled;
}

high_speed_camera_status_t high_speed_camera_send_key(
    high_speed_camera_key_t key) {
    uint8_t count;

    if (key < HIGH_SPEED_CAMERA_KEY_PREV ||
        key > HIGH_SPEED_CAMERA_KEY_UNLOCK) {
        return HIGH_SPEED_CAMERA_ERR_PARAM;
    }
    count = high_speed_camera_next_count(
        &g_high_speed_camera.key_counts[(unsigned)key]);
    return high_speed_camera_send_frame(
        HIGH_SPEED_CAMERA_FRAME_KEY_REQUEST, (uint8_t)key, count);
}

high_speed_camera_status_t high_speed_camera_send_heartbeat(void) {
    uint8_t count = high_speed_camera_next_count(
        &g_high_speed_camera.heartbeat_count);

    return high_speed_camera_send_frame(
        HIGH_SPEED_CAMERA_FRAME_HEARTBEAT_REQUEST, 0U, count);
}

high_speed_camera_status_t high_speed_camera_send_error_response(void) {
    uint8_t count = high_speed_camera_next_count(&g_high_speed_camera.error_count);

    return high_speed_camera_send_frame(
        HIGH_SPEED_CAMERA_FRAME_ERROR_RESPONSE, 0U, count);
}

high_speed_camera_status_t high_speed_camera_process_once(
    int timeout_ms,
    uint8_t *frame_type,
    uint8_t *data,
    uint8_t *count) {
    int64_t start_ms;

    if (!g_high_speed_camera.opened || timeout_ms < -1) {
        return HIGH_SPEED_CAMERA_ERR_STATE;
    }
    if (frame_type != NULL) *frame_type = 0U;
    if (data != NULL) *data = 0U;
    if (count != NULL) *count = 0U;

    start_ms = high_speed_camera_now_ms();
    for (;;) {
        uint8_t byte = 0U;
        size_t read_size = 0U;
        int wait_ms = timeout_ms;
        uart_status_t uart_status;

        high_speed_camera_update_disconnect_state();
        if (timeout_ms >= 0) {
            int64_t elapsed_ms = high_speed_camera_now_ms() - start_ms;
            if (elapsed_ms >= timeout_ms) {
                if (g_high_speed_camera.rx_size > 0U &&
                    high_speed_camera_now_ms() -
                        g_high_speed_camera.last_rx_byte_ms >=
                        HIGH_SPEED_CAMERA_INTERBYTE_TIMEOUT_MS) {
                    g_high_speed_camera.rx_size = 0U;
                    return high_speed_camera_reject_frame(
                        HIGH_SPEED_CAMERA_ERR_PROTOCOL);
                }
                return HIGH_SPEED_CAMERA_NO_DATA;
            }
            wait_ms = timeout_ms - (int)elapsed_ms;
        }

        uart_status = uart_base_read_data(&g_high_speed_camera.uart,
                                          &byte,
                                          1U,
                                          1U,
                                          wait_ms,
                                          &read_size);
        if (uart_status == UART_NO_DATA) {
            if (g_high_speed_camera.rx_size > 0U &&
                high_speed_camera_now_ms() -
                    g_high_speed_camera.last_rx_byte_ms >=
                    HIGH_SPEED_CAMERA_INTERBYTE_TIMEOUT_MS) {
                g_high_speed_camera.rx_size = 0U;
                return high_speed_camera_reject_frame(
                    HIGH_SPEED_CAMERA_ERR_PROTOCOL);
            }
            return HIGH_SPEED_CAMERA_NO_DATA;
        }
        if (uart_status != UART_OK || read_size != 1U) {
            return HIGH_SPEED_CAMERA_ERR_UART;
        }

        g_high_speed_camera.last_rx_byte_ms = high_speed_camera_now_ms();
        if (g_high_speed_camera.rx_size == 0U) {
            if (byte != HIGH_SPEED_CAMERA_FRAME_HEAD) {
                high_speed_camera_loghex("RX-INVALID", &byte, 1U);
                return high_speed_camera_reject_frame(
                    HIGH_SPEED_CAMERA_ERR_PROTOCOL);
            }
            g_high_speed_camera.rx_frame[g_high_speed_camera.rx_size++] = byte;
            continue;
        }

        if (byte == HIGH_SPEED_CAMERA_FRAME_HEAD) {
            high_speed_camera_loghex("RX-INVALID",
                                     g_high_speed_camera.rx_frame,
                                     g_high_speed_camera.rx_size);
            g_high_speed_camera.rx_frame[0] = byte;
            g_high_speed_camera.rx_size = 1U;
            (void)high_speed_camera_reject_frame(
                HIGH_SPEED_CAMERA_ERR_PROTOCOL);
            continue;
        }

        g_high_speed_camera.rx_frame[g_high_speed_camera.rx_size++] = byte;
        if (g_high_speed_camera.rx_size == HIGH_SPEED_CAMERA_FRAME_SIZE) {
            high_speed_camera_status_t status;
            uint8_t type_value = g_high_speed_camera.rx_frame[1];
            uint8_t data_value = g_high_speed_camera.rx_frame[2];
            uint8_t count_value = g_high_speed_camera.rx_frame[5];

            high_speed_camera_loghex("RX",
                                     g_high_speed_camera.rx_frame,
                                     HIGH_SPEED_CAMERA_FRAME_SIZE);
            status = high_speed_camera_validate_frame(
                g_high_speed_camera.rx_frame);
            g_high_speed_camera.rx_size = 0U;
            if (status != HIGH_SPEED_CAMERA_OK) {
                return high_speed_camera_reject_frame(status);
            }

            g_high_speed_camera.last_valid_frame_ms =
                high_speed_camera_now_ms();
            if (!g_high_speed_camera.connected &&
                g_high_speed_camera.debug_enabled) {
                LOGD("valid frame received, key board connected");
            }
            g_high_speed_camera.connected = 1;
            if (g_high_speed_camera.debug_enabled) {
                LOGT("receive type=%s(0x%02X) data=0x%02X count=%u",
                     high_speed_camera_frame_type_name(type_value),
                     (unsigned)type_value,
                     (unsigned)data_value,
                     (unsigned)count_value);
            }

            if (frame_type != NULL) *frame_type = type_value;
            if (data != NULL) *data = data_value;
            if (count != NULL) *count = count_value;

            if (type_value == HIGH_SPEED_CAMERA_FRAME_KEY_REQUEST ||
                type_value == HIGH_SPEED_CAMERA_FRAME_HEARTBEAT_REQUEST) {
                status = high_speed_camera_send_frame(
                    HIGH_SPEED_CAMERA_FRAME_HEARTBEAT_RESPONSE,
                    data_value,
                    count_value);
                if (status != HIGH_SPEED_CAMERA_OK) {
                    return status;
                }
            }
            return HIGH_SPEED_CAMERA_OK;
        }
    }
}

high_speed_camera_status_t high_speed_camera_self_test(void) {
    static const struct {
        uint8_t type;
        uint8_t data;
        uint8_t count;
        uint8_t expected[HIGH_SPEED_CAMERA_FRAME_SIZE];
    } cases[] = {
        {0x01U, 0x01U, 0x01U, {0xAAU, 0x01U, 0x01U, 0x95U, 0xD1U, 0x01U, 0x55U}},
        {0x03U, 0x01U, 0x01U, {0xAAU, 0x03U, 0x01U, 0xF3U, 0xB3U, 0x01U, 0x55U}},
        {0x02U, 0x00U, 0x01U, {0xAAU, 0x02U, 0x00U, 0xD0U, 0xA3U, 0x01U, 0x55U}},
        {0x03U, 0x00U, 0x01U, {0xAAU, 0x03U, 0x00U, 0xE3U, 0x92U, 0x01U, 0x55U}},
        {0x04U, 0x00U, 0x01U, {0xAAU, 0x04U, 0x00U, 0x7AU, 0x05U, 0x01U, 0x55U}}
    };
    size_t i;

    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t actual[HIGH_SPEED_CAMERA_FRAME_SIZE];

        high_speed_camera_build_frame(cases[i].type,
                                      cases[i].data,
                                      cases[i].count,
                                      actual);
        high_speed_camera_loghex("SELFTEST", actual, sizeof(actual));
        if (memcmp(actual, cases[i].expected, sizeof(actual)) != 0 ||
            high_speed_camera_validate_frame(actual) != HIGH_SPEED_CAMERA_OK) {
            high_speed_camera_loghex("SELFTEST-ACTUAL", actual, sizeof(actual));
            high_speed_camera_loghex("SELFTEST-EXPECT",
                                     cases[i].expected,
                                     sizeof(cases[i].expected));
            return HIGH_SPEED_CAMERA_ERR_PROTOCOL;
        }
    }
    if (g_high_speed_camera.debug_enabled) {
        LOGD("protocol self-test passed (%u specification frames)",
             (unsigned)(sizeof(cases) / sizeof(cases[0])));
    }
    return HIGH_SPEED_CAMERA_OK;
}

const char *high_speed_camera_key_name(uint8_t key_id) {
    switch (key_id) {
        case HIGH_SPEED_CAMERA_KEY_PREV: return "prev";
        case HIGH_SPEED_CAMERA_KEY_MENU: return "menu";
        case HIGH_SPEED_CAMERA_KEY_NEXT: return "next";
        case HIGH_SPEED_CAMERA_KEY_TRIGGER: return "trigger";
        case HIGH_SPEED_CAMERA_KEY_PLAYBACK: return "playback";
        case HIGH_SPEED_CAMERA_KEY_STOP_RECORDING: return "stop recording";
        case HIGH_SPEED_CAMERA_KEY_UNLOCK: return "unlock";
        default: return "unknown key";
    }
}

const char *high_speed_camera_frame_type_name(uint8_t frame_type) {
    switch (frame_type) {
        case HIGH_SPEED_CAMERA_FRAME_KEY_REQUEST: return "key request";
        case HIGH_SPEED_CAMERA_FRAME_HEARTBEAT_REQUEST: return "heartbeat request";
        case HIGH_SPEED_CAMERA_FRAME_HEARTBEAT_RESPONSE: return "heartbeat response";
        case HIGH_SPEED_CAMERA_FRAME_ERROR_RESPONSE: return "error response";
        default: return "unknown frame";
    }
}

const char *high_speed_camera_status_string(high_speed_camera_status_t status) {
    switch (status) {
        case HIGH_SPEED_CAMERA_OK: return "ok";
        case HIGH_SPEED_CAMERA_NO_DATA: return "no data";
        case HIGH_SPEED_CAMERA_ERR_PARAM: return "invalid parameter";
        case HIGH_SPEED_CAMERA_ERR_STATE: return "serial port is not open";
        case HIGH_SPEED_CAMERA_ERR_UART: return "UART I/O error";
        case HIGH_SPEED_CAMERA_ERR_CRC: return "CRC16 check failed";
        case HIGH_SPEED_CAMERA_ERR_PROTOCOL: return "invalid protocol frame";
        default: return "unknown status";
    }
}
