#include "control/visca_camera/visca_camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int visca_debug_enabled(void) {
    static int cached = -1;

    if (cached < 0) {
        const char *env = getenv("VISCA_DEBUG");
        cached = (env != NULL && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    return cached;
}

static void visca_debug_log_packet(const char *direction,
                                   const uint8_t *packet,
                                   size_t packet_size) {
    size_t i;

    if (!visca_debug_enabled()) {
        return;
    }
    fprintf(stderr, "[VISCA %s]", direction);
    for (i = 0; i < packet_size; ++i) {
        fprintf(stderr, " %02X", (unsigned)packet[i]);
    }
    fprintf(stderr, "\n");
}

static int visca_is_reply_header(uint8_t byte) {
    if (byte == 0x88U) {
        return 1;
    }
    return byte >= 0x90U && (byte & 0x0fU) == 0U;
}

typedef enum {
    VISCA_WAIT_NONE = 0,
    VISCA_WAIT_ACK,
    VISCA_WAIT_COMPLETION
} visca_wait_mode_t;

static int64_t visca_now_ms(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static uint8_t visca_header_for_address(uint8_t address) {
    return (uint8_t)(0x80U | (address & 0x0fU));
}

static void visca_encode_nibble8(uint8_t value, uint8_t out[2]) {
    out[0] = (uint8_t)((value >> 4) & 0x0fU);
    out[1] = (uint8_t)(value & 0x0fU);
}

static void visca_encode_nibble12(uint16_t value, uint8_t out[3]) {
    out[0] = (uint8_t)((value >> 8) & 0x0fU);
    out[1] = (uint8_t)((value >> 4) & 0x0fU);
    out[2] = (uint8_t)(value & 0x0fU);
}

static void visca_encode_nibble16(uint16_t value, uint8_t out[4]) {
    out[0] = (uint8_t)((value >> 12) & 0x0fU);
    out[1] = (uint8_t)((value >> 8) & 0x0fU);
    out[2] = (uint8_t)((value >> 4) & 0x0fU);
    out[3] = (uint8_t)(value & 0x0fU);
}

static uint8_t visca_decode_nibble8(const uint8_t in[2]) {
    return (uint8_t)(((in[0] & 0x0fU) << 4) | (in[1] & 0x0fU));
}

static uint16_t visca_decode_nibble12(const uint8_t in[3]) {
    return (uint16_t)(((uint16_t)(in[0] & 0x0fU) << 8) |
                      ((uint16_t)(in[1] & 0x0fU) << 4) |
                      (uint16_t)(in[2] & 0x0fU));
}

static uint16_t visca_decode_nibble16(const uint8_t in[4]) {
    return (uint16_t)(((uint16_t)(in[0] & 0x0fU) << 12) |
                      ((uint16_t)(in[1] & 0x0fU) << 8) |
                      ((uint16_t)(in[2] & 0x0fU) << 4) |
                      (uint16_t)(in[3] & 0x0fU));
}

static visca_status_t visca_status_from_camera_error(uint8_t error) {
    switch (error) {
        case 0x01: return VISCA_ERR_MESSAGE_LENGTH;
        case 0x02: return VISCA_ERR_SYNTAX;
        case 0x03: return VISCA_ERR_BUFFER_FULL;
        case 0x04: return VISCA_ERR_CANCELLED;
        case 0x05: return VISCA_ERR_NO_SOCKET;
        case 0x41: return VISCA_ERR_NOT_EXECUTABLE;
        default: return VISCA_ERR_CAMERA;
    }
}

visca_status_t visca_camera_send_raw(visca_camera_t *camera,
                                     const uint8_t *packet,
                                     size_t packet_size) {
    size_t total_written = 0;

    if (camera == NULL || packet == NULL || packet_size < 3U ||
        packet_size > VISCA_PACKET_MAX_SIZE || camera->uart.fd < 0 ||
        packet[packet_size - 1U] != 0xffU) {
        return VISCA_ERR_PARAM;
    }

    while (total_written < packet_size) {
        size_t written = 0;
        uart_status_t uart_status = uart_base_write_data(&camera->uart,
                                                         packet + total_written,
                                                         packet_size - total_written,
                                                         &written);
        if (uart_status != UART_OK) {
            return VISCA_ERR_UART;
        }
        total_written += written;
    }
    visca_debug_log_packet("TX", packet, packet_size);
    return VISCA_OK;
}

visca_status_t visca_camera_read_packet(visca_camera_t *camera,
                                        uint8_t *packet,
                                        size_t packet_capacity,
                                        int timeout_ms,
                                        size_t *packet_size) {
    int64_t start_ms;
    size_t used = 0;

    if (camera == NULL || packet == NULL || packet_capacity < 3U ||
        packet_size == NULL || timeout_ms < -1 || camera->uart.fd < 0) {
        return VISCA_ERR_PARAM;
    }

    *packet_size = 0;
    start_ms = visca_now_ms();
    while (used < packet_capacity) {
        int remaining_ms = timeout_ms;
        size_t read_size = 0;
        uint8_t byte = 0;
        uart_status_t uart_status;

        if (timeout_ms >= 0) {
            int64_t elapsed_ms = visca_now_ms() - start_ms;
            if (elapsed_ms >= timeout_ms) {
                return VISCA_ERR_TIMEOUT;
            }
            remaining_ms = timeout_ms - (int)elapsed_ms;
        }

        uart_status = uart_base_read_data(&camera->uart,
                                          &byte,
                                          1U,
                                          1U,
                                          remaining_ms,
                                          &read_size);
        if (uart_status == UART_NO_DATA) {
            return VISCA_ERR_TIMEOUT;
        }
        if (uart_status != UART_OK || read_size != 1U) {
            return VISCA_ERR_UART;
        }

        if (used == 0U && !visca_is_reply_header(byte)) {
            if (visca_debug_enabled()) {
                fprintf(stderr, "[VISCA RX] discard noise byte %02X\n",
                        (unsigned)byte);
            }
            continue;
        }
        packet[used++] = byte;
        if (byte == 0xffU) {
            if (used < 3U) {
                used = 0;
                continue;
            }
            *packet_size = used;
            visca_debug_log_packet("RX", packet, used);
            return VISCA_OK;
        }
    }

    return VISCA_ERR_PROTOCOL;
}

static visca_status_t visca_wait_for_command(visca_camera_t *camera,
                                             visca_wait_mode_t wait_mode) {
    int64_t start_ms;
    uint8_t ack_socket = 0;
    int ack_seen = 0;

    if (wait_mode == VISCA_WAIT_NONE) {
        return VISCA_OK;
    }

    start_ms = visca_now_ms();
    for (;;) {
        uint8_t packet[VISCA_PACKET_MAX_SIZE];
        size_t packet_size = 0;
        int64_t elapsed_ms = visca_now_ms() - start_ms;
        int remaining_ms = camera->response_timeout_ms - (int)elapsed_ms;
        uint8_t message_type;
        uint8_t socket;
        visca_status_t status;

        if (remaining_ms <= 0) {
            return VISCA_ERR_TIMEOUT;
        }
        status = visca_camera_read_packet(camera,
                                          packet,
                                          sizeof(packet),
                                          remaining_ms,
                                          &packet_size);
        if (status != VISCA_OK) {
            return status;
        }
        if (packet_size < 3U) {
            continue;
        }

        message_type = (uint8_t)(packet[1] & 0xf0U);
        socket = (uint8_t)(packet[1] & 0x0fU);
        if (message_type == 0x40U) {
            camera->last_socket = socket;
            ack_socket = socket;
            ack_seen = 1;
            if (wait_mode == VISCA_WAIT_ACK) {
                return VISCA_OK;
            }
            continue;
        }
        if (message_type == 0x50U) {
            if ((ack_seen && socket == ack_socket) || (!ack_seen && socket == 0U)) {
                camera->last_socket = socket;
                return VISCA_OK;
            }
            continue;
        }
        if (message_type == 0x60U && packet_size >= 4U) {
            if ((ack_seen && socket == ack_socket) ||
                (!ack_seen && socket == 0U)) {
                camera->last_socket = socket;
                camera->last_camera_error = packet[2];
                return visca_status_from_camera_error(packet[2]);
            }
        }
    }
}

static visca_status_t visca_send_with_wait(visca_camera_t *camera,
                                           const uint8_t *packet,
                                           size_t packet_size,
                                           visca_wait_mode_t wait_mode) {
    visca_status_t status = visca_camera_send_raw(camera, packet, packet_size);

    if (status != VISCA_OK) {
        return status;
    }
    return visca_wait_for_command(camera, wait_mode);
}

static visca_status_t visca_command(visca_camera_t *camera,
                                    const uint8_t *command,
                                    size_t command_size,
                                    visca_wait_mode_t wait_mode) {
    uint8_t packet[VISCA_PACKET_MAX_SIZE];

    if (camera == NULL || command == NULL || command_size == 0U ||
        command_size > VISCA_PACKET_MAX_SIZE - 3U) {
        return VISCA_ERR_PARAM;
    }

    if (wait_mode != VISCA_WAIT_NONE &&
        uart_base_flush(&camera->uart, UART_FLUSH_INPUT) != UART_OK) {
        return VISCA_ERR_UART;
    }

    packet[0] = visca_header_for_address(camera->address);
    packet[1] = 0x01U;
    memcpy(packet + 2U, command, command_size);
    packet[2U + command_size] = 0xffU;
    return visca_send_with_wait(camera,
                                packet,
                                command_size + 3U,
                                wait_mode);
}

static visca_status_t visca_inquiry(visca_camera_t *camera,
                                    const uint8_t *inquiry,
                                    size_t inquiry_size,
                                    uint8_t *reply_data,
                                    size_t reply_capacity,
                                    size_t *reply_size) {
    uint8_t packet[VISCA_PACKET_MAX_SIZE];
    int64_t start_ms;
    visca_status_t status;

    if (camera == NULL || inquiry == NULL || inquiry_size == 0U ||
        inquiry_size > VISCA_PACKET_MAX_SIZE - 3U || reply_size == NULL) {
        return VISCA_ERR_PARAM;
    }

    if (uart_base_flush(&camera->uart, UART_FLUSH_INPUT) != UART_OK) {
        return VISCA_ERR_UART;
    }

    packet[0] = visca_header_for_address(camera->address);
    packet[1] = 0x09U;
    memcpy(packet + 2U, inquiry, inquiry_size);
    packet[2U + inquiry_size] = 0xffU;
    status = visca_camera_send_raw(camera, packet, inquiry_size + 3U);
    if (status != VISCA_OK) {
        return status;
    }

    *reply_size = 0;
    start_ms = visca_now_ms();
    for (;;) {
        uint8_t response[VISCA_PACKET_MAX_SIZE];
        size_t response_size = 0;
        int remaining_ms = camera->response_timeout_ms -
                           (int)(visca_now_ms() - start_ms);
        uint8_t message_type;

        if (remaining_ms <= 0) {
            return VISCA_ERR_TIMEOUT;
        }
        status = visca_camera_read_packet(camera,
                                          response,
                                          sizeof(response),
                                          remaining_ms,
                                          &response_size);
        if (status != VISCA_OK) {
            return status;
        }
        if (response_size < 3U) {
            continue;
        }

        message_type = (uint8_t)(response[1] & 0xf0U);
        if (message_type == 0x60U && response_size >= 4U) {
            camera->last_camera_error = response[2];
            return visca_status_from_camera_error(response[2]);
        }
        if (message_type != 0x50U) {
            continue;
        }
        if (response_size - 3U > reply_capacity) {
            return VISCA_ERR_MESSAGE_LENGTH;
        }
        if (response_size > 3U && reply_data != NULL) {
            memcpy(reply_data, response + 2U, response_size - 3U);
        }
        *reply_size = response_size - 3U;
        return VISCA_OK;
    }
}

visca_status_t visca_camera_open(visca_camera_t *camera,
                                 const char *device,
                                 uint32_t baudrate,
                                 uint8_t address) {
    uart_status_t uart_status;

    if (camera == NULL || device == NULL || device[0] == '\0' ||
        baudrate == 0U || address < 1U || address > 7U) {
        return VISCA_ERR_PARAM;
    }

    memset(camera, 0, sizeof(*camera));
    uart_base_config_init(&camera->uart);
    if (strlen(device) >= sizeof(camera->uart.device)) {
        return VISCA_ERR_PARAM;
    }
    memcpy(camera->uart.device, device, strlen(device) + 1U);
    camera->uart.baudrate = baudrate;
    camera->uart.data_bits = 8;
    camera->uart.parity = UART_PARITY_NONE;
    camera->uart.stop_bits = 1;
    camera->uart.hardware_flow_control = 0;
    camera->uart.software_flow_control = 0;
    camera->uart.is_rs485 = 0;
    camera->address = address;
    camera->response_timeout_ms = 2000;

    uart_status = uart_base_open_port(&camera->uart);
    if (uart_status != UART_OK) {
        return VISCA_ERR_UART;
    }
    uart_status = uart_base_flush(&camera->uart, UART_FLUSH_BOTH);
    if (uart_status != UART_OK) {
        uart_base_close_port(&camera->uart);
        return VISCA_ERR_UART;
    }
    return VISCA_OK;
}

void visca_camera_close(visca_camera_t *camera) {
    if (camera != NULL) {
        uart_base_close_port(&camera->uart);
    }
}

void visca_camera_set_timeout(visca_camera_t *camera, int timeout_ms) {
    if (camera != NULL && timeout_ms > 0) {
        camera->response_timeout_ms = timeout_ms;
    }
}

visca_status_t visca_camera_command_raw(visca_camera_t *camera,
                                        const uint8_t *command,
                                        size_t command_size) {
    return visca_send_with_wait(camera,
                                command,
                                command_size,
                                VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_command_raw_async(visca_camera_t *camera,
                                              const uint8_t *command,
                                              size_t command_size) {
    return visca_camera_send_raw(camera, command, command_size);
}

visca_status_t visca_camera_inquiry_raw(visca_camera_t *camera,
                                        const uint8_t *inquiry,
                                        size_t inquiry_size,
                                        uint8_t *reply_data,
                                        size_t reply_capacity,
                                        size_t *reply_size) {
    return visca_inquiry(camera,
                         inquiry,
                         inquiry_size,
                         reply_data,
                         reply_capacity,
                         reply_size);
}

const char *visca_camera_status_string(visca_status_t status) {
    switch (status) {
        case VISCA_OK: return "ok";
        case VISCA_ERR_PARAM: return "invalid parameter";
        case VISCA_ERR_UART: return "UART I/O error";
        case VISCA_ERR_TIMEOUT: return "VISCA response timeout";
        case VISCA_ERR_PROTOCOL: return "invalid VISCA packet";
        case VISCA_ERR_MESSAGE_LENGTH: return "VISCA message length error";
        case VISCA_ERR_SYNTAX: return "VISCA syntax error";
        case VISCA_ERR_BUFFER_FULL: return "VISCA command buffer full";
        case VISCA_ERR_CANCELLED: return "VISCA command cancelled";
        case VISCA_ERR_NO_SOCKET: return "VISCA socket not found";
        case VISCA_ERR_NOT_EXECUTABLE: return "VISCA command not executable";
        case VISCA_ERR_CAMERA: return "VISCA camera error";
        default: return "unknown VISCA status";
    }
}

static visca_status_t visca_command_switch(visca_camera_t *camera,
                                           uint8_t category,
                                           uint8_t command_code,
                                           int enabled) {
    const uint8_t command[] = {
        category, command_code, enabled ? 0x02U : 0x03U
    };
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

static visca_status_t visca_command_adjust(visca_camera_t *camera,
                                           uint8_t category,
                                           uint8_t command_code,
                                           visca_adjust_t adjustment) {
    uint8_t value;
    const uint8_t *command;
    uint8_t buffer[3];

    switch (adjustment) {
        case VISCA_ADJUST_RESET: value = 0x00U; break;
        case VISCA_ADJUST_UP: value = 0x02U; break;
        case VISCA_ADJUST_DOWN: value = 0x03U; break;
        default: return VISCA_ERR_PARAM;
    }
    buffer[0] = category;
    buffer[1] = command_code;
    buffer[2] = value;
    command = buffer;
    return visca_command(camera, command, sizeof(buffer), VISCA_WAIT_COMPLETION);
}

static visca_status_t visca_command_direct8(visca_camera_t *camera,
                                            uint8_t command_code,
                                            uint8_t value) {
    uint8_t command[] = {0x04, command_code, 0x00, 0x00, 0x00, 0x00};
    visca_encode_nibble8(value, command + 4U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

static visca_status_t visca_command_direct16(visca_camera_t *camera,
                                             uint8_t command_code,
                                             uint16_t value) {
    uint8_t command[] = {0x04, command_code, 0x00, 0x00, 0x00, 0x00};
    visca_encode_nibble16(value, command + 2U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_address_set(visca_camera_t *camera,
                                        uint8_t requested_address) {
    uint8_t packet[] = {0x88, 0x30, 0x00, 0xff};
    uint8_t reply[VISCA_PACKET_MAX_SIZE];
    size_t reply_size = 0;
    visca_status_t status;

    if (camera == NULL || requested_address < 1U || requested_address > 7U) {
        return VISCA_ERR_PARAM;
    }
    packet[2] = requested_address;
    status = visca_camera_send_raw(camera, packet, sizeof(packet));
    if (status != VISCA_OK) {
        return status;
    }
    status = visca_camera_read_packet(camera,
                                      reply,
                                      sizeof(reply),
                                      camera->response_timeout_ms,
                                      &reply_size);
    if (status != VISCA_OK) {
        return status;
    }
    if (reply_size != 4U || reply[0] != 0x88U || reply[1] != 0x30U) {
        return VISCA_ERR_PROTOCOL;
    }
    camera->address = requested_address;
    return VISCA_OK;
}

visca_status_t visca_camera_if_clear(visca_camera_t *camera) {
    const uint8_t command[] = {0x00, 0x01};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_if_clear_broadcast(visca_camera_t *camera) {
    const uint8_t packet[] = {0x88, 0x01, 0x00, 0x01, 0xff};
    uint8_t reply[VISCA_PACKET_MAX_SIZE];
    size_t reply_size = 0;
    visca_status_t status = visca_camera_send_raw(camera, packet, sizeof(packet));

    if (status != VISCA_OK) {
        return status;
    }
    status = visca_camera_read_packet(camera,
                                      reply,
                                      sizeof(reply),
                                      camera->response_timeout_ms,
                                      &reply_size);
    if (status != VISCA_OK) {
        return status;
    }
    return reply_size == sizeof(packet) && memcmp(reply, packet, sizeof(packet)) == 0
               ? VISCA_OK
               : VISCA_ERR_PROTOCOL;
}

visca_status_t visca_camera_cancel(visca_camera_t *camera, uint8_t socket) {
    uint8_t packet[3];
    visca_status_t status;

    if (camera == NULL || socket < 1U || socket > 2U) {
        return VISCA_ERR_PARAM;
    }
    packet[0] = visca_header_for_address(camera->address);
    packet[1] = (uint8_t)(0x20U | socket);
    packet[2] = 0xffU;
    status = visca_send_with_wait(camera,
                                  packet,
                                  sizeof(packet),
                                  VISCA_WAIT_COMPLETION);
    return status == VISCA_ERR_CANCELLED ? VISCA_OK : status;
}

visca_status_t visca_camera_set_power(visca_camera_t *camera, int on) {
    return visca_command_switch(camera, 0x04, 0x00, on);
}

visca_status_t visca_camera_initialize_lens(visca_camera_t *camera) {
    const uint8_t command[] = {0x04, 0x19, 0x01};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_reset(visca_camera_t *camera) {
    const uint8_t command[] = {0x04, 0x19, 0x03};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_id(visca_camera_t *camera, uint16_t id) {
    return visca_command_direct16(camera, 0x22, id);
}

visca_status_t visca_camera_zoom_stop(visca_camera_t *camera) {
    const uint8_t command[] = {0x04, 0x07, 0x00};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_zoom_tele(visca_camera_t *camera, uint8_t speed) {
    uint8_t command[] = {0x04, 0x07, 0x20};
    if (speed > 7U) return VISCA_ERR_PARAM;
    command[2] |= speed;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_ACK);
}

visca_status_t visca_camera_zoom_wide(visca_camera_t *camera, uint8_t speed) {
    uint8_t command[] = {0x04, 0x07, 0x30};
    if (speed > 7U) return VISCA_ERR_PARAM;
    command[2] |= speed;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_ACK);
}

visca_status_t visca_camera_zoom_direct(visca_camera_t *camera,
                                        uint16_t position) {
    if (position > 0x7ac0U) return VISCA_ERR_PARAM;
    return visca_command_direct16(camera, 0x47, position);
}

visca_status_t visca_camera_zoom_optical_ratio(visca_camera_t *camera,
                                               uint8_t ratio) {
    static const uint16_t position_table[20] = {
        0x0000, 0x0dc1, 0x186c, 0x2015, 0x2594,
        0x29b7, 0x2cfb, 0x2fb0, 0x320c, 0x342d,
        0x3608, 0x37aa, 0x391c, 0x3a66, 0x3b90,
        0x3c9c, 0x3d91, 0x3e72, 0x3f40, 0x4000
    };
    if (ratio < 1U || ratio > 20U) return VISCA_ERR_PARAM;
    return visca_camera_zoom_direct(camera, position_table[ratio - 1U]);
}

visca_status_t visca_camera_set_dzoom_mode(visca_camera_t *camera,
                                           visca_dzoom_mode_t mode) {
    uint8_t value;
    const uint8_t command_prefix[] = {0x04, 0x06};
    uint8_t command[3];

    switch (mode) {
        case VISCA_DZOOM_ON: value = 0x02; break;
        case VISCA_DZOOM_OFF: value = 0x03; break;
        case VISCA_DZOOM_SUPER_RESOLUTION: value = 0x04; break;
        default: return VISCA_ERR_PARAM;
    }
    memcpy(command, command_prefix, sizeof(command_prefix));
    command[2] = value;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_dzoom_combine(visca_camera_t *camera,
                                              int combine) {
    const uint8_t command[] = {0x04, 0x36, combine ? 0x00U : 0x01U};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_dzoom_stop(visca_camera_t *camera) {
    const uint8_t command[] = {0x04, 0x06, 0x00};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_dzoom_tele(visca_camera_t *camera, uint8_t speed) {
    uint8_t command[] = {0x04, 0x06, 0x20};
    if (speed > 7U) return VISCA_ERR_PARAM;
    command[2] |= speed;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_ACK);
}

visca_status_t visca_camera_dzoom_wide(visca_camera_t *camera, uint8_t speed) {
    uint8_t command[] = {0x04, 0x06, 0x30};
    if (speed > 7U) return VISCA_ERR_PARAM;
    command[2] |= speed;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_ACK);
}

visca_status_t visca_camera_dzoom_toggle_x1_max(visca_camera_t *camera) {
    const uint8_t command[] = {0x04, 0x06, 0x10};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_dzoom_direct(visca_camera_t *camera,
                                         uint8_t position) {
    uint8_t command[] = {0x04, 0x46, 0x00, 0x00, 0x00, 0x00};
    if (position > 0xebU) return VISCA_ERR_PARAM;
    visca_encode_nibble8(position, command + 4U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_zoom_focus_direct(visca_camera_t *camera,
                                              uint16_t zoom_position,
                                              uint16_t focus_position) {
    uint8_t command[] = {0x04, 0x47, 0, 0, 0, 0, 0, 0, 0, 0};
    if (zoom_position > 0x7ac0U || focus_position > 0xf000U) {
        return VISCA_ERR_PARAM;
    }
    visca_encode_nibble16(zoom_position, command + 2U);
    visca_encode_nibble16(focus_position, command + 6U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_continuous_zoom_reply(visca_camera_t *camera,
                                                     int enabled) {
    return visca_command_switch(camera, 0x04, 0x69, enabled);
}

visca_status_t visca_camera_set_zoom_reply_interval(visca_camera_t *camera,
                                                   uint8_t v_cycles) {
    uint8_t command[] = {0x04, 0x6a, 0x00, 0x00, 0x00, 0x00};
    if (v_cycles == 0U) return VISCA_ERR_PARAM;
    visca_encode_nibble8(v_cycles, command + 4U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_focus_mode(visca_camera_t *camera,
                                          visca_focus_mode_t mode) {
    uint8_t value;
    uint8_t command[] = {0x04, 0x38, 0x00};
    switch (mode) {
        case VISCA_FOCUS_AUTO: value = 0x02; break;
        case VISCA_FOCUS_MANUAL: value = 0x03; break;
        case VISCA_FOCUS_TOGGLE: value = 0x10; break;
        default: return VISCA_ERR_PARAM;
    }
    command[2] = value;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_focus_stop(visca_camera_t *camera) {
    const uint8_t command[] = {0x04, 0x08, 0x00};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_focus_far(visca_camera_t *camera, uint8_t speed) {
    uint8_t command[] = {0x04, 0x08, 0x20};
    if (speed > 7U) return VISCA_ERR_PARAM;
    command[2] |= speed;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_ACK);
}

visca_status_t visca_camera_focus_near(visca_camera_t *camera, uint8_t speed) {
    uint8_t command[] = {0x04, 0x08, 0x30};
    if (speed > 7U) return VISCA_ERR_PARAM;
    command[2] |= speed;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_ACK);
}

visca_status_t visca_camera_focus_direct(visca_camera_t *camera,
                                         uint16_t position) {
    if (position > 0xf000U) return VISCA_ERR_PARAM;
    return visca_command_direct16(camera, 0x48, position);
}

visca_status_t visca_camera_focus_one_push(visca_camera_t *camera) {
    const uint8_t command[] = {0x04, 0x18, 0x01};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_focus_near_limit(visca_camera_t *camera,
                                                uint16_t position) {
    if (position < 0x1000U || position > 0xf000U) return VISCA_ERR_PARAM;
    return visca_command_direct16(camera, 0x28, position);
}

visca_status_t visca_camera_set_af_mode(visca_camera_t *camera,
                                       visca_af_mode_t mode) {
    const uint8_t command[] = {0x04, 0x57, (uint8_t)mode};
    if (mode < VISCA_AF_NORMAL || mode > VISCA_AF_ZOOM_TRIGGER) {
        return VISCA_ERR_PARAM;
    }
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_af_time(visca_camera_t *camera,
                                       uint8_t active_seconds,
                                       uint8_t interval_seconds) {
    uint8_t command[] = {0x04, 0x27, 0, 0, 0, 0};
    visca_encode_nibble8(active_seconds, command + 2U);
    visca_encode_nibble8(interval_seconds, command + 4U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_af_sensitivity(visca_camera_t *camera,
                                              visca_af_sensitivity_t sensitivity) {
    if (sensitivity != VISCA_AF_SENSITIVITY_NORMAL &&
        sensitivity != VISCA_AF_SENSITIVITY_LOW) return VISCA_ERR_PARAM;
    return visca_command_switch(camera,
                                0x04,
                                0x58,
                                sensitivity == VISCA_AF_SENSITIVITY_NORMAL);
}

visca_status_t visca_camera_set_spot_focus(visca_camera_t *camera, int enabled) {
    return visca_command_switch(camera, 0x05, 0x08, enabled);
}

visca_status_t visca_camera_set_spot_focus_position(visca_camera_t *camera,
                                                   uint8_t x,
                                                   uint8_t y) {
    uint8_t command[] = {0x05, 0x68, 0, 0, 0, 0};
    if (x > 0x0fU || y > 0x0fU) return VISCA_ERR_PARAM;
    visca_encode_nibble8(x, command + 2U);
    visca_encode_nibble8(y, command + 4U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_ir_correction(visca_camera_t *camera,
                                             visca_ir_correction_t mode) {
    const uint8_t command[] = {0x04, 0x11, (uint8_t)mode};
    if (mode != VISCA_IR_CORRECTION_STANDARD &&
        mode != VISCA_IR_CORRECTION_LIGHT) return VISCA_ERR_PARAM;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_continuous_focus_reply(visca_camera_t *camera,
                                                      int enabled) {
    return visca_command_switch(camera, 0x04, 0x16, enabled);
}

visca_status_t visca_camera_set_focus_reply_interval(visca_camera_t *camera,
                                                    uint8_t v_cycles) {
    uint8_t command[] = {0x04, 0x1a, 0x00, 0x00, 0x00, 0x00};
    if (v_cycles == 0U) return VISCA_ERR_PARAM;
    visca_encode_nibble8(v_cycles, command + 4U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_wb_mode(visca_camera_t *camera,
                                       visca_wb_mode_t mode) {
    const uint8_t command[] = {0x04, 0x35, (uint8_t)mode};
    if (mode < VISCA_WB_AUTO || mode > VISCA_WB_SODIUM_LAMP_OUTDOOR_AUTO) {
        return VISCA_ERR_PARAM;
    }
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_wb_one_push(visca_camera_t *camera) {
    const uint8_t command[] = {0x04, 0x10, 0x05};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_adjust_r_gain(visca_camera_t *camera,
                                         visca_adjust_t adjustment) {
    return visca_command_adjust(camera, 0x04, 0x03, adjustment);
}

visca_status_t visca_camera_set_r_gain(visca_camera_t *camera, uint8_t value) {
    return visca_command_direct8(camera, 0x43, value);
}

visca_status_t visca_camera_adjust_b_gain(visca_camera_t *camera,
                                         visca_adjust_t adjustment) {
    return visca_command_adjust(camera, 0x04, 0x04, adjustment);
}

visca_status_t visca_camera_set_b_gain(visca_camera_t *camera, uint8_t value) {
    return visca_command_direct8(camera, 0x44, value);
}

visca_status_t visca_camera_set_ae_mode(visca_camera_t *camera,
                                       visca_ae_mode_t mode) {
    const uint8_t command[] = {0x04, 0x39, (uint8_t)mode};
    if (mode != VISCA_AE_FULL_AUTO && mode != VISCA_AE_MANUAL &&
        mode != VISCA_AE_SHUTTER_PRIORITY && mode != VISCA_AE_IRIS_PRIORITY &&
        mode != VISCA_AE_GAIN_PRIORITY && mode != VISCA_AE_BRIGHT) {
        return VISCA_ERR_PARAM;
    }
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_low_light_basis(visca_camera_t *camera,
                                               int enabled) {
    return visca_command_switch(camera, 0x05, 0x39, enabled);
}

visca_status_t visca_camera_set_low_light_basis_position(visca_camera_t *camera,
                                                        uint8_t position) {
    const uint8_t command[] = {0x05, 0x49, (uint8_t)(position & 0x0fU)};
    if (position > 0x0fU) return VISCA_ERR_PARAM;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_adjust_shutter(visca_camera_t *camera,
                                          visca_adjust_t adjustment) {
    return visca_command_adjust(camera, 0x04, 0x0a, adjustment);
}

visca_status_t visca_camera_set_shutter(visca_camera_t *camera, uint8_t position) {
    return visca_command_direct8(camera, 0x4a, position);
}

visca_status_t visca_camera_set_max_shutter_limit(visca_camera_t *camera,
                                                 uint8_t position) {
    uint8_t command[] = {0x05, 0x2a, 0x00, 0, 0};
    visca_encode_nibble8(position, command + 3U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_min_shutter_limit(visca_camera_t *camera,
                                                 uint8_t position) {
    uint8_t command[] = {0x05, 0x2a, 0x01, 0, 0};
    visca_encode_nibble8(position, command + 3U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_slow_shutter(visca_camera_t *camera, int enabled) {
    return visca_command_switch(camera, 0x04, 0x5a, enabled);
}

visca_status_t visca_camera_set_slow_shutter_limit(visca_camera_t *camera,
                                                  uint8_t position) {
    uint8_t command[] = {0x05, 0x5a, 0, 0};
    visca_encode_nibble8(position, command + 2U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_adjust_iris(visca_camera_t *camera,
                                       visca_adjust_t adjustment) {
    return visca_command_adjust(camera, 0x04, 0x0b, adjustment);
}

visca_status_t visca_camera_set_iris(visca_camera_t *camera, uint8_t position) {
    if (position > 0x19U) return VISCA_ERR_PARAM;
    return visca_command_direct8(camera, 0x4b, position);
}

visca_status_t visca_camera_adjust_gain(visca_camera_t *camera,
                                       visca_adjust_t adjustment) {
    return visca_command_adjust(camera, 0x04, 0x0c, adjustment);
}

visca_status_t visca_camera_set_gain(visca_camera_t *camera, uint8_t position) {
    if (position > 0x11U) return VISCA_ERR_PARAM;
    return visca_command_direct8(camera, 0x4c, position);
}

visca_status_t visca_camera_set_gain_limit(visca_camera_t *camera, uint8_t limit) {
    const uint8_t command[] = {0x04, 0x2c, (uint8_t)(limit & 0x0fU)};
    if (limit > 0x0dU) return VISCA_ERR_PARAM;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_gain_point(visca_camera_t *camera,
                                          uint8_t position) {
    uint8_t command[] = {0x05, 0x4c, 0, 0};
    visca_encode_nibble8(position, command + 2U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_enable_gain_point(visca_camera_t *camera, int enabled) {
    const uint8_t command[] = {0x05, 0x0c, enabled ? 0x02U : 0x03U};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_adjust_bright(visca_camera_t *camera,
                                         visca_adjust_t adjustment) {
    return visca_command_adjust(camera, 0x04, 0x0d, adjustment);
}

visca_status_t visca_camera_set_bright(visca_camera_t *camera, uint8_t position) {
    if (position > 0x29U) return VISCA_ERR_PARAM;
    return visca_command_direct8(camera, 0x4d, position);
}

visca_status_t visca_camera_enable_exposure_compensation(visca_camera_t *camera,
                                                        int enabled) {
    return visca_command_switch(camera, 0x04, 0x3e, enabled);
}

visca_status_t visca_camera_adjust_exposure_compensation(
    visca_camera_t *camera,
    visca_adjust_t adjustment) {
    return visca_command_adjust(camera, 0x04, 0x0e, adjustment);
}

visca_status_t visca_camera_set_exposure_compensation(visca_camera_t *camera,
                                                     uint8_t position) {
    if (position > 0x0eU) return VISCA_ERR_PARAM;
    return visca_command_direct8(camera, 0x4e, position);
}

visca_status_t visca_camera_set_backlight(visca_camera_t *camera, int enabled) {
    return visca_command_switch(camera, 0x04, 0x33, enabled);
}

visca_status_t visca_camera_set_spot_ae(visca_camera_t *camera, int enabled) {
    return visca_command_switch(camera, 0x04, 0x59, enabled);
}

visca_status_t visca_camera_set_spot_ae_position(visca_camera_t *camera,
                                                uint8_t x,
                                                uint8_t y) {
    uint8_t command[] = {0x04, 0x29, 0, 0, 0, 0};
    if (x > 0x0fU || y > 0x0fU) return VISCA_ERR_PARAM;
    visca_encode_nibble8(x, command + 2U);
    visca_encode_nibble8(y, command + 4U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_ae_response(visca_camera_t *camera,
                                           uint8_t response) {
    const uint8_t command[] = {0x04, 0x5d, response};
    if (response < 0x01U || response > 0x30U) return VISCA_ERR_PARAM;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_ve(visca_camera_t *camera, int enabled) {
    const uint8_t command[] = {0x04, 0x3d, enabled ? 0x06U : 0x03U};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_ve_parameters(visca_camera_t *camera,
                                             const visca_ve_params_t *params) {
    uint8_t command[] = {0x04, 0x2d, 0x00, 0, 0, 0, 0, 0, 0, 0};
    if (params == NULL || params->display_brightness > 6U ||
        params->brightness_compensation > 3U ||
        params->compensation_level > 2U) return VISCA_ERR_PARAM;
    command[3] = params->display_brightness;
    command[4] = params->brightness_compensation;
    command[5] = params->compensation_level;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_defog(visca_camera_t *camera,
                                     int enabled,
                                     uint8_t level) {
    const uint8_t command[] = {
        0x04, 0x37, enabled ? 0x02U : 0x03U, enabled ? level : 0x00U
    };
    if (enabled && level > 3U) return VISCA_ERR_PARAM;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_adjust_aperture(visca_camera_t *camera,
                                           visca_adjust_t adjustment) {
    return visca_command_adjust(camera, 0x04, 0x02, adjustment);
}

visca_status_t visca_camera_set_aperture_level(visca_camera_t *camera,
                                              uint8_t level) {
    if (level > 0x0fU) return VISCA_ERR_PARAM;
    return visca_command_direct8(camera, 0x42, level);
}

static visca_status_t visca_set_aperture_parameter(visca_camera_t *camera,
                                                  uint8_t selector,
                                                  uint8_t value) {
    const uint8_t command[] = {0x05, 0x42, selector, (uint8_t)(value & 0x0fU)};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_aperture_mode(visca_camera_t *camera,
                                             int manual) {
    return visca_set_aperture_parameter(camera, 0x01, manual ? 1U : 0U);
}

visca_status_t visca_camera_set_aperture_bandwidth(visca_camera_t *camera,
                                                  uint8_t bandwidth) {
    if (bandwidth > 4U) return VISCA_ERR_PARAM;
    return visca_set_aperture_parameter(camera, 0x02, bandwidth);
}

visca_status_t visca_camera_set_aperture_crispening(visca_camera_t *camera,
                                                   uint8_t value) {
    if (value > 7U) return VISCA_ERR_PARAM;
    return visca_set_aperture_parameter(camera, 0x03, value);
}

visca_status_t visca_camera_set_aperture_hv_balance(visca_camera_t *camera,
                                                   uint8_t value) {
    if (value < 5U || value > 9U) return VISCA_ERR_PARAM;
    return visca_set_aperture_parameter(camera, 0x04, value);
}

visca_status_t visca_camera_set_aperture_bw_balance(visca_camera_t *camera,
                                                   uint8_t value) {
    if (value > 4U) return VISCA_ERR_PARAM;
    return visca_set_aperture_parameter(camera, 0x05, value);
}

visca_status_t visca_camera_set_aperture_limit(visca_camera_t *camera,
                                              uint8_t value) {
    if (value > 7U) return VISCA_ERR_PARAM;
    return visca_set_aperture_parameter(camera, 0x06, value);
}

visca_status_t visca_camera_set_aperture_highlight_detail(visca_camera_t *camera,
                                                         uint8_t value) {
    if (value > 4U) return VISCA_ERR_PARAM;
    return visca_set_aperture_parameter(camera, 0x07, value);
}

visca_status_t visca_camera_set_aperture_super_low(visca_camera_t *camera,
                                                  uint8_t value) {
    if (value > 7U) return VISCA_ERR_PARAM;
    return visca_set_aperture_parameter(camera, 0x08, value);
}

visca_status_t visca_camera_set_high_resolution(visca_camera_t *camera,
                                               int enabled) {
    return visca_command_switch(camera, 0x04, 0x52, enabled);
}

visca_status_t visca_camera_set_noise_reduction(visca_camera_t *camera,
                                               uint8_t level) {
    const uint8_t command[] = {0x04, 0x53, level};
    if (level > 5U && level != 0x7fU) return VISCA_ERR_PARAM;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_noise_reduction_2d_3d(visca_camera_t *camera,
                                                     uint8_t level_2d,
                                                     uint8_t level_3d) {
    const uint8_t command[] = {0x05, 0x53, level_2d, level_3d};
    if (level_2d > 5U || level_3d > 5U) return VISCA_ERR_PARAM;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_stabilizer(visca_camera_t *camera, int enabled) {
    return visca_command_switch(camera, 0x04, 0x34, enabled);
}

visca_status_t visca_camera_set_gamma(visca_camera_t *camera,
                                     visca_gamma_mode_t mode) {
    const uint8_t command[] = {0x04, 0x5b, (uint8_t)mode};
    if (mode < VISCA_GAMMA_STANDARD || mode > VISCA_GAMMA_PATTERN) {
        return VISCA_ERR_PARAM;
    }
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_gamma_pattern(visca_camera_t *camera,
                                             uint16_t pattern) {
    uint8_t command[] = {0x05, 0x5b, 0, 0, 0};
    if (pattern < 1U || pattern > 0x200U) return VISCA_ERR_PARAM;
    visca_encode_nibble12(pattern, command + 2U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_gamma_offset(visca_camera_t *camera,
                                            int negative,
                                            uint8_t offset) {
    uint8_t command[] = {0x04, 0x1e, 0x00, 0x00, 0x00, 0, 0, 0};
    if (offset > 0x40U) return VISCA_ERR_PARAM;
    command[5] = negative ? 1U : 0U;
    visca_encode_nibble8(offset, command + 6U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_high_sensitivity(visca_camera_t *camera,
                                                int enabled) {
    return visca_command_switch(camera, 0x04, 0x5e, enabled);
}

visca_status_t visca_camera_set_lr_reverse(visca_camera_t *camera, int enabled) {
    return visca_command_switch(camera, 0x04, 0x61, enabled);
}

visca_status_t visca_camera_set_freeze(visca_camera_t *camera, int enabled) {
    return visca_command_switch(camera, 0x04, 0x62, enabled);
}

visca_status_t visca_camera_set_black_white(visca_camera_t *camera, int enabled) {
    const uint8_t command[] = {0x04, 0x63, enabled ? 0x04U : 0x00U};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_e_flip(visca_camera_t *camera, int enabled) {
    return visca_command_switch(camera, 0x04, 0x66, enabled);
}

visca_status_t visca_camera_set_icr(visca_camera_t *camera, int enabled) {
    return visca_command_switch(camera, 0x04, 0x01, enabled);
}

visca_status_t visca_camera_set_auto_icr(visca_camera_t *camera, int enabled) {
    return visca_command_switch(camera, 0x04, 0x51, enabled);
}

visca_status_t visca_camera_set_auto_icr_threshold(visca_camera_t *camera,
                                                  uint8_t threshold) {
    uint8_t command[] = {0x04, 0x21, 0x00, 0x00, 0, 0};
    visca_encode_nibble8(threshold, command + 4U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_auto_icr_alarm(visca_camera_t *camera,
                                              int enabled) {
    return visca_command_switch(camera, 0x04, 0x31, enabled);
}

static visca_status_t visca_memory_command(visca_camera_t *camera,
                                           uint8_t action,
                                           uint8_t number) {
    const uint8_t command[] = {0x04, 0x3f, action, number};
    if (number > 0x63U && number != 0x7fU) return VISCA_ERR_PARAM;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_memory_reset(visca_camera_t *camera, uint8_t number) {
    return visca_memory_command(camera, 0x00, number);
}

visca_status_t visca_camera_memory_set(visca_camera_t *camera, uint8_t number) {
    return visca_memory_command(camera, 0x01, number);
}

visca_status_t visca_camera_memory_recall(visca_camera_t *camera, uint8_t number) {
    return visca_memory_command(camera, 0x02, number);
}

visca_status_t visca_camera_custom_reset(visca_camera_t *camera) {
    return visca_memory_command(camera, 0x00, 0x7f);
}

visca_status_t visca_camera_custom_set(visca_camera_t *camera) {
    return visca_memory_command(camera, 0x01, 0x7f);
}

visca_status_t visca_camera_custom_recall(visca_camera_t *camera) {
    return visca_memory_command(camera, 0x02, 0x7f);
}

visca_status_t visca_camera_user_memory_write(visca_camera_t *camera,
                                             uint8_t address,
                                             uint16_t value) {
    uint8_t command[] = {0x04, 0x23, 0, 0, 0, 0, 0};
    if (address > 7U) return VISCA_ERR_PARAM;
    command[2] = address;
    visca_encode_nibble16(value, command + 3U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_display(visca_camera_t *camera, int enabled) {
    return visca_command_switch(camera, 0x04, 0x15, enabled);
}

visca_status_t visca_camera_toggle_display(visca_camera_t *camera) {
    const uint8_t command[] = {0x04, 0x15, 0x10};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_title_set_style(visca_camera_t *camera,
                                           uint8_t line,
                                           uint8_t horizontal_position,
                                           uint8_t color,
                                           int blink) {
    uint8_t command[] = {
        0x04, 0x73, 0, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    if (line >= VISCA_TITLE_LINE_COUNT || horizontal_position > 0x1fU ||
        color > 6U) return VISCA_ERR_PARAM;
    command[2] = (uint8_t)(0x10U | line);
    command[4] = horizontal_position;
    command[5] = color;
    command[6] = blink ? 1U : 0U;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_title_set_characters(visca_camera_t *camera,
                                                uint8_t line,
                                                uint8_t start_index,
                                                const uint8_t *character_codes,
                                                size_t character_count) {
    uint8_t command[13] = {0x04, 0x73};
    size_t i;

    if (line >= VISCA_TITLE_LINE_COUNT || character_codes == NULL ||
        character_count != 10U || (start_index != 0U && start_index != 10U)) {
        return VISCA_ERR_PARAM;
    }
    command[2] = (uint8_t)((start_index == 0U ? 0x20U : 0x30U) | line);
    for (i = 0; i < character_count; ++i) {
        if (character_codes[i] > 0x7fU) return VISCA_ERR_PARAM;
        command[3U + i] = character_codes[i];
    }
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

static int visca_valid_title_line_selector(uint8_t line) {
    return line < VISCA_TITLE_LINE_COUNT || line == 0x0fU;
}

visca_status_t visca_camera_title_clear(visca_camera_t *camera, uint8_t line) {
    const uint8_t command[] = {0x04, 0x74, (uint8_t)(0x10U | line)};
    if (!visca_valid_title_line_selector(line)) return VISCA_ERR_PARAM;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_title_enable(visca_camera_t *camera,
                                        uint8_t line,
                                        int enabled) {
    const uint8_t command[] = {
        0x04, 0x74, (uint8_t)((enabled ? 0x20U : 0x30U) | line)
    };
    if (!visca_valid_title_line_selector(line)) return VISCA_ERR_PARAM;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_mute(visca_camera_t *camera, int enabled) {
    return visca_command_switch(camera, 0x04, 0x75, enabled);
}

visca_status_t visca_camera_toggle_mute(visca_camera_t *camera) {
    const uint8_t command[] = {0x04, 0x75, 0x10};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_privacy_set_mask(visca_camera_t *camera,
                                            uint8_t mask,
                                            int new_position,
                                            uint8_t half_width,
                                            uint8_t half_height) {
    uint8_t command[] = {0x04, 0x76, mask, new_position ? 1U : 0U, 0, 0, 0, 0};
    if (mask >= 24U) return VISCA_ERR_PARAM;
    visca_encode_nibble8(half_width, command + 4U);
    visca_encode_nibble8(half_height, command + 6U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_privacy_set_table(visca_camera_t *camera,
                                             uint8_t table) {
    const uint8_t command[] = {0x05, 0x70, table};
    if (table > 1U) return VISCA_ERR_PARAM;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

static void visca_encode_u32_be(uint32_t value, uint8_t out[4]) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint32_t visca_decode_u32_be(const uint8_t in[4]) {
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

visca_status_t visca_camera_privacy_set_display(visca_camera_t *camera,
                                               uint32_t mask_bits) {
    uint8_t command[] = {0x04, 0x77, 0, 0, 0, 0};
    if ((mask_bits & 0xff000000U) != 0U) return VISCA_ERR_PARAM;
    visca_encode_u32_be(mask_bits, command + 2U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_privacy_set_color(visca_camera_t *camera,
                                             uint32_t color_select_bits,
                                             uint8_t color_zero,
                                             uint8_t color_one) {
    uint8_t command[] = {0x04, 0x78, 0, 0, 0, 0, color_zero, color_one};
    if ((color_select_bits & 0xff000000U) != 0U) return VISCA_ERR_PARAM;
    visca_encode_u32_be(color_select_bits, command + 2U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_privacy_set_pan_tilt_angle(visca_camera_t *camera,
                                                     uint16_t pan,
                                                     uint16_t tilt) {
    uint8_t command[] = {0x04, 0x79, 0, 0, 0, 0, 0, 0};
    if (pan > 0x0fffU || tilt > 0x0fffU) return VISCA_ERR_PARAM;
    visca_encode_nibble12(pan, command + 2U);
    visca_encode_nibble12(tilt, command + 5U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_privacy_set_ptz_mask(visca_camera_t *camera,
                                                uint8_t mask,
                                                uint16_t pan,
                                                uint16_t tilt,
                                                uint16_t zoom) {
    uint8_t command[] = {0x04, 0x7b, mask, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (mask >= 24U || pan > 0x0fffU || tilt > 0x0fffU || zoom > 0x7ac0U) {
        return VISCA_ERR_PARAM;
    }
    visca_encode_nibble12(pan, command + 3U);
    visca_encode_nibble12(tilt, command + 6U);
    visca_encode_nibble16(zoom, command + 9U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_privacy_set_non_interlock_mask(
    visca_camera_t *camera,
    uint8_t mask,
    uint8_t x,
    uint8_t y,
    uint8_t half_width,
    uint8_t half_height) {
    uint8_t command[] = {0x04, 0x6f, mask, 0, 0, 0, 0, 0, 0, 0, 0};
    if (mask >= 24U) return VISCA_ERR_PARAM;
    visca_encode_nibble8(x, command + 3U);
    visca_encode_nibble8(y, command + 5U);
    visca_encode_nibble8(half_width, command + 7U);
    visca_encode_nibble8(half_height, command + 9U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_center_line(visca_camera_t *camera, int enabled) {
    const uint8_t command[] = {0x04, 0x7c, enabled ? 0x04U : 0x03U};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_register_write(visca_camera_t *camera,
                                          uint8_t register_number,
                                          uint8_t value) {
    uint8_t command[] = {0x04, 0x24, register_number, 0, 0};
    if (register_number > 0x7fU) return VISCA_ERR_PARAM;
    visca_encode_nibble8(value, command + 3U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_color_enhancement_parameters(
    visca_camera_t *camera,
    uint8_t threshold,
    uint8_t high_luminance_color,
    uint8_t low_luminance_color) {
    const uint8_t command[] = {
        0x04, 0x20, threshold, 0x00, high_luminance_color,
        0x40, 0x40, low_luminance_color, 0x40, 0x40
    };
    if (threshold > 0x7fU || high_luminance_color > 0x7fU ||
        low_luminance_color > 0x7fU) return VISCA_ERR_PARAM;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_color_enhancement(visca_camera_t *camera,
                                                 int enabled) {
    return visca_command_switch(camera, 0x04, 0x50, enabled);
}

visca_status_t visca_camera_set_chroma_suppress(visca_camera_t *camera,
                                               uint8_t level) {
    const uint8_t command[] = {0x04, 0x5f, level};
    if (level > 3U) return VISCA_ERR_PARAM;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_color_gain(visca_camera_t *camera, uint8_t level) {
    if (level > 0x0eU) return VISCA_ERR_PARAM;
    return visca_command_direct8(camera, 0x49, level);
}

visca_status_t visca_camera_set_color_hue(visca_camera_t *camera, uint8_t level) {
    if (level > 0x0eU) return VISCA_ERR_PARAM;
    return visca_command_direct8(camera, 0x4f, level);
}

static visca_status_t visca_extended_adjust(visca_camera_t *camera,
                                            uint8_t feature,
                                            visca_adjust_t adjustment,
                                            uint8_t steps) {
    uint8_t operation;
    uint8_t command[] = {0x04, 0x1f, feature, 0, 0};

    switch (adjustment) {
        case VISCA_ADJUST_RESET: operation = 0x00; steps = 0x00; break;
        case VISCA_ADJUST_UP: operation = 0x02; break;
        case VISCA_ADJUST_DOWN: operation = 0x03; break;
        default: return VISCA_ERR_PARAM;
    }
    if (steps > 0x7fU) return VISCA_ERR_PARAM;
    command[3] = operation;
    command[4] = steps;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

static visca_status_t visca_extended_direct(visca_camera_t *camera,
                                            uint8_t feature,
                                            uint8_t value) {
    uint8_t command[] = {0x04, 0x1f, feature, 0x00, 0x00, 0, 0};
    visca_encode_nibble8(value, command + 5U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_extended_adjust_exposure_compensation(
    visca_camera_t *camera,
    visca_adjust_t adjustment,
    uint8_t steps) {
    return visca_extended_adjust(camera, 0x0e, adjustment, steps);
}

visca_status_t visca_camera_extended_set_exposure_compensation(
    visca_camera_t *camera,
    uint8_t level) {
    return visca_extended_direct(camera, 0x4e, level);
}

visca_status_t visca_camera_extended_adjust_aperture(visca_camera_t *camera,
                                                    visca_adjust_t adjustment,
                                                    uint8_t steps) {
    return visca_extended_adjust(camera, 0x02, adjustment, steps);
}

visca_status_t visca_camera_extended_set_aperture(visca_camera_t *camera,
                                                 uint8_t level) {
    return visca_extended_direct(camera, 0x42, level);
}

visca_status_t visca_camera_extended_set_auto_icr_threshold(
    visca_camera_t *camera,
    uint8_t threshold) {
    uint8_t command[] = {0x04, 0x1f, 0x21, 0x00, 0x00, 0, 0};
    visca_encode_nibble8(threshold, command + 5U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_extended_set_auto_icr_on_level(visca_camera_t *camera,
                                                         uint8_t level) {
    uint8_t command[] = {0x04, 0x1f, 0x21, 0x01, 0x00, 0, 0};
    if (level > 0x1cU) return VISCA_ERR_PARAM;
    visca_encode_nibble8(level, command + 5U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_extended_set_color_gain(visca_camera_t *camera,
                                                   uint8_t level) {
    return visca_extended_direct(camera, 0x49, level);
}

visca_status_t visca_camera_extended_set_color_hue(visca_camera_t *camera,
                                                  uint8_t level) {
    return visca_extended_direct(camera, 0x4f, level);
}

visca_status_t visca_camera_set_hlc(visca_camera_t *camera,
                                   uint8_t level,
                                   uint8_t mask_level) {
    const uint8_t command[] = {0x04, 0x14, level, mask_level};
    if (level > 3U || mask_level > 0x0fU) return VISCA_ERR_PARAM;
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_color_bar(visca_camera_t *camera,
                                         visca_color_bar_mode_t mode) {
    const uint8_t command[] = {0x7e, 0x04, 0x7d, (uint8_t)mode};
    if (mode < VISCA_COLOR_BAR_OFF || mode > VISCA_COLOR_BAR_GRAYSCALE) {
        return VISCA_ERR_PARAM;
    }
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_ept(visca_camera_t *camera, int enabled) {
    const uint8_t command[] = {0x7e, 0x06, 0x00, enabled ? 0x02U : 0x03U};
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_set_ept_position(visca_camera_t *camera,
                                            uint16_t pan,
                                            uint16_t tilt) {
    uint8_t command[] = {0x7e, 0x06, 0x20, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
    visca_encode_nibble16(pan, command + 5U);
    visca_encode_nibble16(tilt, command + 9U);
    return visca_command(camera, command, sizeof(command), VISCA_WAIT_COMPLETION);
}

visca_status_t visca_camera_enter_maintenance_mode(visca_camera_t *camera) {
    static const uint8_t steps[][3] = {
        {0x04, 0x00, 0x0c},
        {0x04, 0x00, 0x0d},
        {0x04, 0x00, 0x13},
        {0x04, 0x00, 0x04},
        {0x04, 0x00, 0x20}
    };
    size_t i;
    visca_status_t status = visca_camera_set_power(camera, 0);

    if (status != VISCA_OK) return status;
    for (i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        status = visca_command(camera,
                               steps[i],
                               sizeof(steps[i]),
                               VISCA_WAIT_COMPLETION);
        if (status != VISCA_OK) return status;
    }
    return VISCA_OK;
}

static visca_status_t visca_query_data(visca_camera_t *camera,
                                       const uint8_t *query,
                                       size_t query_size,
                                       uint8_t *data,
                                       size_t data_capacity,
                                       size_t minimum_size,
                                       size_t *data_size) {
    size_t received_size = 0;
    visca_status_t status;

    if (data == NULL || data_size == NULL) return VISCA_ERR_PARAM;
    status = visca_inquiry(camera,
                           query,
                           query_size,
                           data,
                           data_capacity,
                           &received_size);
    if (status != VISCA_OK) return status;
    if (received_size < minimum_size) return VISCA_ERR_PROTOCOL;
    *data_size = received_size;
    return VISCA_OK;
}

static visca_status_t visca_query_bool(visca_camera_t *camera,
                                       const uint8_t *query,
                                       size_t query_size,
                                       uint8_t on_value,
                                       uint8_t off_value,
                                       int *enabled) {
    uint8_t data[VISCA_PACKET_MAX_SIZE];
    size_t data_size = 0;
    visca_status_t status;

    if (enabled == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera,
                              query,
                              query_size,
                              data,
                              sizeof(data),
                              1U,
                              &data_size);
    if (status != VISCA_OK) return status;
    if (data[0] == on_value) {
        *enabled = 1;
        return VISCA_OK;
    }
    if (data[0] == off_value) {
        *enabled = 0;
        return VISCA_OK;
    }
    return VISCA_ERR_PROTOCOL;
}

static visca_status_t visca_query_nibble8(visca_camera_t *camera,
                                          const uint8_t *query,
                                          size_t query_size,
                                          size_t offset,
                                          uint8_t *value) {
    uint8_t data[VISCA_PACKET_MAX_SIZE];
    size_t data_size = 0;
    visca_status_t status;

    if (value == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera,
                              query,
                              query_size,
                              data,
                              sizeof(data),
                              offset + 2U,
                              &data_size);
    if (status != VISCA_OK) return status;
    *value = visca_decode_nibble8(data + offset);
    return VISCA_OK;
}

static visca_status_t visca_query_nibble16(visca_camera_t *camera,
                                           const uint8_t *query,
                                           size_t query_size,
                                           size_t offset,
                                           uint16_t *value) {
    uint8_t data[VISCA_PACKET_MAX_SIZE];
    size_t data_size = 0;
    visca_status_t status;

    if (value == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera,
                              query,
                              query_size,
                              data,
                              sizeof(data),
                              offset + 4U,
                              &data_size);
    if (status != VISCA_OK) return status;
    *value = visca_decode_nibble16(data + offset);
    return VISCA_OK;
}

visca_status_t visca_camera_get_power(visca_camera_t *camera, int *on) {
    const uint8_t query[] = {0x04, 0x00};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, on);
}

visca_status_t visca_camera_get_zoom_position(visca_camera_t *camera,
                                             uint16_t *position) {
    const uint8_t query[] = {0x04, 0x47};
    return visca_query_nibble16(camera, query, sizeof(query), 0U, position);
}

visca_status_t visca_camera_get_dzoom_mode(visca_camera_t *camera,
                                          visca_dzoom_mode_t *mode) {
    const uint8_t query[] = {0x04, 0x06};
    uint8_t data[2];
    size_t data_size = 0;
    visca_status_t status;

    if (mode == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 1, &data_size);
    if (status != VISCA_OK) return status;
    if (data[0] == 0x02) *mode = VISCA_DZOOM_ON;
    else if (data[0] == 0x03) *mode = VISCA_DZOOM_OFF;
    else if (data[0] == 0x04) *mode = VISCA_DZOOM_SUPER_RESOLUTION;
    else return VISCA_ERR_PROTOCOL;
    return VISCA_OK;
}

visca_status_t visca_camera_get_dzoom_combine(visca_camera_t *camera,
                                             int *combine) {
    const uint8_t query[] = {0x04, 0x36};
    return visca_query_bool(camera, query, sizeof(query), 0x00, 0x01, combine);
}

visca_status_t visca_camera_get_dzoom_position(visca_camera_t *camera,
                                              uint8_t *position) {
    const uint8_t query[] = {0x04, 0x46};
    return visca_query_nibble8(camera, query, sizeof(query), 2U, position);
}

visca_status_t visca_camera_get_focus_mode(visca_camera_t *camera,
                                          visca_focus_mode_t *mode) {
    const uint8_t query[] = {0x04, 0x38};
    int automatic;
    visca_status_t status;
    if (mode == NULL) return VISCA_ERR_PARAM;
    status = visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, &automatic);
    if (status == VISCA_OK) *mode = automatic ? VISCA_FOCUS_AUTO : VISCA_FOCUS_MANUAL;
    return status;
}

visca_status_t visca_camera_get_focus_position(visca_camera_t *camera,
                                              uint16_t *position) {
    const uint8_t query[] = {0x04, 0x48};
    return visca_query_nibble16(camera, query, sizeof(query), 0U, position);
}

visca_status_t visca_camera_get_focus_near_limit(visca_camera_t *camera,
                                                uint16_t *position) {
    const uint8_t query[] = {0x04, 0x28};
    return visca_query_nibble16(camera, query, sizeof(query), 0U, position);
}

visca_status_t visca_camera_get_spot_focus(visca_camera_t *camera, int *enabled) {
    const uint8_t query[] = {0x05, 0x08};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_spot_focus_position(visca_camera_t *camera,
                                                   uint8_t *x,
                                                   uint8_t *y) {
    const uint8_t query[] = {0x05, 0x68};
    uint8_t data[4];
    size_t data_size = 0;
    visca_status_t status;
    if (x == NULL || y == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 4, &data_size);
    if (status == VISCA_OK) {
        *x = visca_decode_nibble8(data);
        *y = visca_decode_nibble8(data + 2U);
    }
    return status;
}

visca_status_t visca_camera_get_af_sensitivity(visca_camera_t *camera,
                                              visca_af_sensitivity_t *sensitivity) {
    const uint8_t query[] = {0x04, 0x58};
    int normal;
    visca_status_t status;
    if (sensitivity == NULL) return VISCA_ERR_PARAM;
    status = visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, &normal);
    if (status == VISCA_OK) {
        *sensitivity = normal ? VISCA_AF_SENSITIVITY_NORMAL : VISCA_AF_SENSITIVITY_LOW;
    }
    return status;
}

visca_status_t visca_camera_get_af_mode(visca_camera_t *camera,
                                       visca_af_mode_t *mode) {
    const uint8_t query[] = {0x04, 0x57};
    uint8_t data[1];
    size_t data_size = 0;
    visca_status_t status;
    if (mode == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 1, &data_size);
    if (status != VISCA_OK) return status;
    if (data[0] > 2U) return VISCA_ERR_PROTOCOL;
    *mode = (visca_af_mode_t)data[0];
    return VISCA_OK;
}

visca_status_t visca_camera_get_af_time(visca_camera_t *camera,
                                       uint8_t *active_seconds,
                                       uint8_t *interval_seconds) {
    const uint8_t query[] = {0x04, 0x27};
    uint8_t data[4];
    size_t data_size = 0;
    visca_status_t status;
    if (active_seconds == NULL || interval_seconds == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 4, &data_size);
    if (status == VISCA_OK) {
        *active_seconds = visca_decode_nibble8(data);
        *interval_seconds = visca_decode_nibble8(data + 2U);
    }
    return status;
}

visca_status_t visca_camera_get_low_light_basis(visca_camera_t *camera,
                                               int *enabled) {
    const uint8_t query[] = {0x05, 0x39};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_low_light_basis_position(visca_camera_t *camera,
                                                        uint8_t *position) {
    const uint8_t query[] = {0x05, 0x49};
    uint8_t data[1];
    size_t data_size = 0;
    visca_status_t status;
    if (position == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 1, &data_size);
    if (status == VISCA_OK) *position = (uint8_t)(data[0] & 0x0fU);
    return status;
}

visca_status_t visca_camera_get_ir_correction(visca_camera_t *camera,
                                             visca_ir_correction_t *mode) {
    const uint8_t query[] = {0x04, 0x11};
    uint8_t data[1];
    size_t data_size = 0;
    visca_status_t status;
    if (mode == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 1, &data_size);
    if (status != VISCA_OK) return status;
    if (data[0] > 1U) return VISCA_ERR_PROTOCOL;
    *mode = (visca_ir_correction_t)data[0];
    return VISCA_OK;
}

visca_status_t visca_camera_get_wb_mode(visca_camera_t *camera,
                                       visca_wb_mode_t *mode) {
    const uint8_t query[] = {0x04, 0x35};
    uint8_t data[1];
    size_t data_size = 0;
    visca_status_t status;
    if (mode == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 1, &data_size);
    if (status != VISCA_OK) return status;
    if (data[0] > 9U) return VISCA_ERR_PROTOCOL;
    *mode = (visca_wb_mode_t)data[0];
    return VISCA_OK;
}

visca_status_t visca_camera_get_r_gain(visca_camera_t *camera, uint8_t *value) {
    const uint8_t query[] = {0x04, 0x43};
    return visca_query_nibble8(camera, query, sizeof(query), 2U, value);
}

visca_status_t visca_camera_get_b_gain(visca_camera_t *camera, uint8_t *value) {
    const uint8_t query[] = {0x04, 0x44};
    return visca_query_nibble8(camera, query, sizeof(query), 2U, value);
}

visca_status_t visca_camera_get_ae_mode(visca_camera_t *camera,
                                       visca_ae_mode_t *mode) {
    const uint8_t query[] = {0x04, 0x39};
    uint8_t data[1];
    size_t data_size = 0;
    visca_status_t status;
    if (mode == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 1, &data_size);
    if (status != VISCA_OK) return status;
    if (data[0] != VISCA_AE_FULL_AUTO && data[0] != VISCA_AE_MANUAL &&
        data[0] != VISCA_AE_SHUTTER_PRIORITY && data[0] != VISCA_AE_IRIS_PRIORITY &&
        data[0] != VISCA_AE_BRIGHT && data[0] != VISCA_AE_GAIN_PRIORITY) {
        return VISCA_ERR_PROTOCOL;
    }
    *mode = (visca_ae_mode_t)data[0];
    return VISCA_OK;
}

visca_status_t visca_camera_get_shutter(visca_camera_t *camera, uint8_t *position) {
    const uint8_t query[] = {0x04, 0x4a};
    return visca_query_nibble8(camera, query, sizeof(query), 2U, position);
}

visca_status_t visca_camera_get_max_shutter_limit(visca_camera_t *camera,
                                                 uint8_t *position) {
    const uint8_t query[] = {0x05, 0x2a, 0x00};
    return visca_query_nibble8(camera, query, sizeof(query), 0U, position);
}

visca_status_t visca_camera_get_min_shutter_limit(visca_camera_t *camera,
                                                 uint8_t *position) {
    const uint8_t query[] = {0x05, 0x2a, 0x01};
    return visca_query_nibble8(camera, query, sizeof(query), 0U, position);
}

visca_status_t visca_camera_get_slow_shutter(visca_camera_t *camera,
                                            int *enabled) {
    const uint8_t query[] = {0x04, 0x5a};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_slow_shutter_limit(visca_camera_t *camera,
                                                  uint8_t *position) {
    const uint8_t query[] = {0x05, 0x5a};
    return visca_query_nibble8(camera, query, sizeof(query), 0U, position);
}

visca_status_t visca_camera_get_iris(visca_camera_t *camera, uint8_t *position) {
    const uint8_t query[] = {0x04, 0x4b};
    return visca_query_nibble8(camera, query, sizeof(query), 2U, position);
}

visca_status_t visca_camera_get_gain(visca_camera_t *camera, uint8_t *position) {
    const uint8_t query[] = {0x04, 0x4c};
    return visca_query_nibble8(camera, query, sizeof(query), 2U, position);
}

visca_status_t visca_camera_get_gain_limit(visca_camera_t *camera, uint8_t *limit) {
    const uint8_t query[] = {0x04, 0x2c};
    uint8_t data[1];
    size_t data_size = 0;
    visca_status_t status;
    if (limit == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 1, &data_size);
    if (status == VISCA_OK) *limit = (uint8_t)(data[0] & 0x0fU);
    return status;
}

visca_status_t visca_camera_get_gain_point(visca_camera_t *camera,
                                          uint8_t *position) {
    const uint8_t query[] = {0x05, 0x4c};
    return visca_query_nibble8(camera, query, sizeof(query), 0U, position);
}

visca_status_t visca_camera_get_gain_point_enabled(visca_camera_t *camera,
                                                  int *enabled) {
    const uint8_t query[] = {0x05, 0x0c};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_bright(visca_camera_t *camera, uint8_t *position) {
    const uint8_t query[] = {0x04, 0x4d};
    return visca_query_nibble8(camera, query, sizeof(query), 2U, position);
}

visca_status_t visca_camera_get_exposure_compensation_enabled(
    visca_camera_t *camera,
    int *enabled) {
    const uint8_t query[] = {0x04, 0x3e};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_exposure_compensation(visca_camera_t *camera,
                                                     uint8_t *position) {
    const uint8_t query[] = {0x04, 0x4e};
    return visca_query_nibble8(camera, query, sizeof(query), 2U, position);
}

visca_status_t visca_camera_get_backlight(visca_camera_t *camera, int *enabled) {
    const uint8_t query[] = {0x04, 0x33};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_spot_ae(visca_camera_t *camera, int *enabled) {
    const uint8_t query[] = {0x04, 0x59};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_spot_ae_position(visca_camera_t *camera,
                                                uint8_t *x,
                                                uint8_t *y) {
    const uint8_t query[] = {0x04, 0x29};
    uint8_t data[4];
    size_t data_size = 0;
    visca_status_t status;
    if (x == NULL || y == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 4, &data_size);
    if (status == VISCA_OK) {
        *x = visca_decode_nibble8(data);
        *y = visca_decode_nibble8(data + 2U);
    }
    return status;
}

visca_status_t visca_camera_get_ve(visca_camera_t *camera, int *enabled) {
    const uint8_t query[] = {0x04, 0x3d};
    return visca_query_bool(camera, query, sizeof(query), 0x06, 0x03, enabled);
}

visca_status_t visca_camera_get_ve_parameters(visca_camera_t *camera,
                                             visca_ve_params_t *params) {
    const uint8_t query[] = {0x04, 0x2d};
    uint8_t data[8];
    size_t data_size = 0;
    visca_status_t status;
    if (params == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 4, &data_size);
    if (status == VISCA_OK) {
        params->display_brightness = (uint8_t)(data[1] & 0x0fU);
        params->brightness_compensation = (uint8_t)(data[2] & 0x0fU);
        params->compensation_level = (uint8_t)(data[3] & 0x0fU);
    }
    return status;
}

visca_status_t visca_camera_get_ae_response(visca_camera_t *camera,
                                           uint8_t *response) {
    const uint8_t query[] = {0x04, 0x5d};
    uint8_t data[1];
    size_t data_size = 0;
    visca_status_t status;
    if (response == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 1, &data_size);
    if (status == VISCA_OK) *response = data[0];
    return status;
}

visca_status_t visca_camera_get_defog(visca_camera_t *camera,
                                     int *enabled,
                                     uint8_t *level) {
    const uint8_t query[] = {0x04, 0x37};
    uint8_t data[2];
    size_t data_size = 0;
    visca_status_t status;
    if (enabled == NULL || level == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 2, &data_size);
    if (status != VISCA_OK) return status;
    if (data[0] == 0x02) *enabled = 1;
    else if (data[0] == 0x03) *enabled = 0;
    else return VISCA_ERR_PROTOCOL;
    *level = (uint8_t)(data[1] & 0x0fU);
    return VISCA_OK;
}

visca_status_t visca_camera_get_aperture_level(visca_camera_t *camera,
                                              uint8_t *level) {
    const uint8_t query[] = {0x04, 0x42};
    return visca_query_nibble8(camera, query, sizeof(query), 2U, level);
}

static visca_status_t visca_get_aperture_parameter(visca_camera_t *camera,
                                                  uint8_t selector,
                                                  uint8_t *value) {
    const uint8_t query[] = {0x05, 0x42, selector};
    uint8_t data[1];
    size_t data_size = 0;
    visca_status_t status;
    if (value == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 1, &data_size);
    if (status == VISCA_OK) *value = (uint8_t)(data[0] & 0x0fU);
    return status;
}

visca_status_t visca_camera_get_aperture_mode(visca_camera_t *camera,
                                             int *manual) {
    uint8_t value;
    visca_status_t status;
    if (manual == NULL) return VISCA_ERR_PARAM;
    status = visca_get_aperture_parameter(camera, 0x01, &value);
    if (status == VISCA_OK) *manual = value != 0U;
    return status;
}

visca_status_t visca_camera_get_aperture_bandwidth(visca_camera_t *camera,
                                                  uint8_t *value) {
    return visca_get_aperture_parameter(camera, 0x02, value);
}

visca_status_t visca_camera_get_aperture_crispening(visca_camera_t *camera,
                                                   uint8_t *value) {
    return visca_get_aperture_parameter(camera, 0x03, value);
}

visca_status_t visca_camera_get_aperture_hv_balance(visca_camera_t *camera,
                                                   uint8_t *value) {
    return visca_get_aperture_parameter(camera, 0x04, value);
}

visca_status_t visca_camera_get_aperture_bw_balance(visca_camera_t *camera,
                                                   uint8_t *value) {
    return visca_get_aperture_parameter(camera, 0x05, value);
}

visca_status_t visca_camera_get_aperture_limit(visca_camera_t *camera,
                                              uint8_t *value) {
    return visca_get_aperture_parameter(camera, 0x06, value);
}

visca_status_t visca_camera_get_aperture_highlight_detail(visca_camera_t *camera,
                                                         uint8_t *value) {
    return visca_get_aperture_parameter(camera, 0x07, value);
}

visca_status_t visca_camera_get_aperture_super_low(visca_camera_t *camera,
                                                  uint8_t *value) {
    return visca_get_aperture_parameter(camera, 0x08, value);
}

visca_status_t visca_camera_get_high_resolution(visca_camera_t *camera,
                                               int *enabled) {
    const uint8_t query[] = {0x04, 0x52};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_noise_reduction(visca_camera_t *camera,
                                               uint8_t *level) {
    const uint8_t query[] = {0x04, 0x53};
    uint8_t data[1];
    size_t data_size = 0;
    visca_status_t status;
    if (level == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 1, &data_size);
    if (status == VISCA_OK) *level = data[0];
    return status;
}

visca_status_t visca_camera_get_noise_reduction_2d_3d(visca_camera_t *camera,
                                                     uint8_t *level_2d,
                                                     uint8_t *level_3d) {
    const uint8_t query[] = {0x05, 0x53};
    uint8_t data[2];
    size_t data_size = 0;
    visca_status_t status;
    if (level_2d == NULL || level_3d == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 2, &data_size);
    if (status == VISCA_OK) {
        *level_2d = (uint8_t)(data[0] & 0x0fU);
        *level_3d = (uint8_t)(data[1] & 0x0fU);
    }
    return status;
}

visca_status_t visca_camera_get_stabilizer(visca_camera_t *camera, int *enabled) {
    const uint8_t query[] = {0x04, 0x34};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_gamma(visca_camera_t *camera,
                                     visca_gamma_mode_t *mode) {
    const uint8_t query[] = {0x04, 0x5b};
    uint8_t data[1];
    size_t data_size = 0;
    visca_status_t status;
    if (mode == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 1, &data_size);
    if (status != VISCA_OK) return status;
    if (data[0] > 2U) return VISCA_ERR_PROTOCOL;
    *mode = (visca_gamma_mode_t)data[0];
    return VISCA_OK;
}

visca_status_t visca_camera_get_gamma_pattern(visca_camera_t *camera,
                                             uint16_t *pattern) {
    const uint8_t query[] = {0x05, 0x5b};
    uint8_t data[3];
    size_t data_size = 0;
    visca_status_t status;
    if (pattern == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 3, &data_size);
    if (status == VISCA_OK) *pattern = visca_decode_nibble12(data);
    return status;
}

visca_status_t visca_camera_get_gamma_offset(visca_camera_t *camera,
                                            int *negative,
                                            uint8_t *offset) {
    const uint8_t query[] = {0x04, 0x1e};
    uint8_t data[6];
    size_t data_size = 0;
    visca_status_t status;
    if (negative == NULL || offset == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 6, &data_size);
    if (status == VISCA_OK) {
        *negative = (data[3] & 0x0fU) != 0U;
        *offset = visca_decode_nibble8(data + 4U);
    }
    return status;
}

visca_status_t visca_camera_get_high_sensitivity(visca_camera_t *camera,
                                                int *enabled) {
    const uint8_t query[] = {0x04, 0x5e};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_lr_reverse(visca_camera_t *camera, int *enabled) {
    const uint8_t query[] = {0x04, 0x61};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_freeze(visca_camera_t *camera, int *enabled) {
    const uint8_t query[] = {0x04, 0x62};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_black_white(visca_camera_t *camera, int *enabled) {
    const uint8_t query[] = {0x04, 0x63};
    return visca_query_bool(camera, query, sizeof(query), 0x04, 0x00, enabled);
}

visca_status_t visca_camera_get_e_flip(visca_camera_t *camera, int *enabled) {
    const uint8_t query[] = {0x04, 0x66};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_icr(visca_camera_t *camera, int *enabled) {
    const uint8_t query[] = {0x04, 0x01};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_auto_icr(visca_camera_t *camera, int *enabled) {
    const uint8_t query[] = {0x04, 0x51};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_auto_icr_threshold(visca_camera_t *camera,
                                                  uint8_t *threshold) {
    const uint8_t query[] = {0x04, 0x21};
    return visca_query_nibble8(camera, query, sizeof(query), 2U, threshold);
}

visca_status_t visca_camera_get_auto_icr_alarm(visca_camera_t *camera,
                                              int *enabled) {
    const uint8_t query[] = {0x04, 0x31};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_last_memory(visca_camera_t *camera,
                                           uint8_t *number) {
    const uint8_t query[] = {0x04, 0x3f};
    uint8_t data[1];
    size_t data_size = 0;
    visca_status_t status;
    if (number == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 1, &data_size);
    if (status == VISCA_OK) *number = data[0];
    return status;
}

visca_status_t visca_camera_user_memory_read(visca_camera_t *camera,
                                            uint8_t address,
                                            uint16_t *value) {
    const uint8_t query[] = {0x04, 0x23, address};
    if (address > 7U) return VISCA_ERR_PARAM;
    return visca_query_nibble16(camera, query, sizeof(query), 0U, value);
}

visca_status_t visca_camera_get_display(visca_camera_t *camera, int *enabled) {
    const uint8_t query[] = {0x04, 0x15};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_mute(visca_camera_t *camera, int *enabled) {
    const uint8_t query[] = {0x04, 0x75};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_privacy_table(visca_camera_t *camera,
                                             uint8_t *table) {
    const uint8_t query[] = {0x05, 0x70};
    uint8_t data[1];
    size_t data_size = 0;
    visca_status_t status;
    if (table == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 1, &data_size);
    if (status == VISCA_OK) *table = (uint8_t)(data[0] & 0x0fU);
    return status;
}

static visca_status_t visca_query_mask_bits(visca_camera_t *camera,
                                           const uint8_t *query,
                                           size_t query_size,
                                           uint32_t *mask_bits) {
    uint8_t data[4];
    size_t data_size = 0;
    visca_status_t status;
    if (mask_bits == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, query_size, data, sizeof(data), 4, &data_size);
    if (status == VISCA_OK) *mask_bits = visca_decode_u32_be(data);
    return status;
}

visca_status_t visca_camera_get_privacy_display(visca_camera_t *camera,
                                               uint32_t *mask_bits) {
    const uint8_t query[] = {0x04, 0x77};
    return visca_query_mask_bits(camera, query, sizeof(query), mask_bits);
}

visca_status_t visca_camera_get_privacy_pan_tilt(visca_camera_t *camera,
                                                uint16_t *pan,
                                                uint16_t *tilt) {
    const uint8_t query[] = {0x04, 0x79};
    uint8_t data[6];
    size_t data_size = 0;
    visca_status_t status;
    if (pan == NULL || tilt == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 6, &data_size);
    if (status == VISCA_OK) {
        *pan = visca_decode_nibble12(data);
        *tilt = visca_decode_nibble12(data + 3U);
    }
    return status;
}

visca_status_t visca_camera_get_privacy_ptz(visca_camera_t *camera,
                                           uint8_t mask,
                                           uint16_t *pan,
                                           uint16_t *tilt,
                                           uint16_t *zoom) {
    const uint8_t query[] = {0x04, 0x7b, mask};
    uint8_t data[10];
    size_t data_size = 0;
    visca_status_t status;
    if (mask >= 24U || pan == NULL || tilt == NULL || zoom == NULL) {
        return VISCA_ERR_PARAM;
    }
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 10, &data_size);
    if (status == VISCA_OK) {
        *pan = visca_decode_nibble12(data);
        *tilt = visca_decode_nibble12(data + 3U);
        *zoom = visca_decode_nibble16(data + 6U);
    }
    return status;
}

visca_status_t visca_camera_get_privacy_monitor(visca_camera_t *camera,
                                               uint32_t *mask_bits) {
    const uint8_t query[] = {0x04, 0x6f};
    return visca_query_mask_bits(camera, query, sizeof(query), mask_bits);
}

visca_status_t visca_camera_get_id(visca_camera_t *camera, uint16_t *id) {
    const uint8_t query[] = {0x04, 0x22};
    return visca_query_nibble16(camera, query, sizeof(query), 0U, id);
}

visca_status_t visca_camera_get_version(visca_camera_t *camera,
                                       visca_version_t *version) {
    const uint8_t query[] = {0x00, 0x02};
    size_t data_size = 0;
    visca_status_t status;
    if (version == NULL) return VISCA_ERR_PARAM;
    memset(version, 0, sizeof(*version));
    status = visca_query_data(camera,
                              query,
                              sizeof(query),
                              version->raw,
                              sizeof(version->raw),
                              sizeof(version->raw),
                              &data_size);
    if (status == VISCA_OK) {
        version->model_code = (uint16_t)(((uint16_t)version->raw[2] << 8) |
                                         version->raw[3]);
        version->rom_version = (uint32_t)(((uint16_t)version->raw[4] << 8) |
                                          version->raw[5]);
        version->socket_count = version->raw[6];
    }
    return status;
}

visca_status_t visca_camera_get_continuous_zoom_reply(visca_camera_t *camera,
                                                     int *enabled) {
    const uint8_t query[] = {0x04, 0x69};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_zoom_reply_interval(visca_camera_t *camera,
                                                   uint8_t *v_cycles) {
    const uint8_t query[] = {0x04, 0x6a};
    return visca_query_nibble8(camera, query, sizeof(query), 2U, v_cycles);
}

visca_status_t visca_camera_get_continuous_focus_reply(visca_camera_t *camera,
                                                      int *enabled) {
    const uint8_t query[] = {0x04, 0x16};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_focus_reply_interval(visca_camera_t *camera,
                                                    uint8_t *v_cycles) {
    const uint8_t query[] = {0x04, 0x1a};
    return visca_query_nibble8(camera, query, sizeof(query), 2U, v_cycles);
}

visca_status_t visca_camera_get_extended_auto_icr_on_level(visca_camera_t *camera,
                                                         uint8_t *level) {
    const uint8_t query[] = {0x04, 0x1f, 0x21, 0x01};
    return visca_query_nibble8(camera, query, sizeof(query), 2U, level);
}

visca_status_t visca_camera_get_minimum_shutter_enabled(visca_camera_t *camera,
                                                      int *enabled) {
    const uint8_t query[] = {0x04, 0x12};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_minimum_shutter_position(visca_camera_t *camera,
                                                       uint8_t *position) {
    const uint8_t query[] = {0x04, 0x13};
    return visca_query_nibble8(camera, query, sizeof(query), 2U, position);
}

visca_status_t visca_camera_get_hlc(visca_camera_t *camera,
                                   uint8_t *level,
                                   uint8_t *mask_level) {
    const uint8_t query[] = {0x04, 0x14};
    uint8_t data[2];
    size_t data_size = 0;
    visca_status_t status;
    if (level == NULL || mask_level == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 2, &data_size);
    if (status == VISCA_OK) {
        *level = (uint8_t)(data[0] & 0x0fU);
        *mask_level = (uint8_t)(data[1] & 0x0fU);
    }
    return status;
}

visca_status_t visca_camera_register_read(visca_camera_t *camera,
                                         uint8_t register_number,
                                         uint8_t *value) {
    const uint8_t query[] = {0x04, 0x24, register_number};
    if (register_number > 0x7fU) return VISCA_ERR_PARAM;
    return visca_query_nibble8(camera, query, sizeof(query), 0U, value);
}

visca_status_t visca_camera_get_color_bar(visca_camera_t *camera,
                                         visca_color_bar_mode_t *mode) {
    const uint8_t query[] = {0x7e, 0x04, 0x7d};
    uint8_t data[1];
    size_t data_size = 0;
    visca_status_t status;
    if (mode == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 1, &data_size);
    if (status != VISCA_OK) return status;
    if (data[0] > 3U) return VISCA_ERR_PROTOCOL;
    *mode = (visca_color_bar_mode_t)data[0];
    return VISCA_OK;
}

visca_status_t visca_camera_get_ept(visca_camera_t *camera, int *enabled) {
    const uint8_t query[] = {0x7e, 0x06, 0x00};
    return visca_query_bool(camera, query, sizeof(query), 0x02, 0x03, enabled);
}

visca_status_t visca_camera_get_ept_position(visca_camera_t *camera,
                                            uint16_t *pan,
                                            uint16_t *tilt) {
    const uint8_t query[] = {0x7e, 0x06, 0x20};
    uint8_t data[10];
    size_t data_size = 0;
    visca_status_t status;
    if (pan == NULL || tilt == NULL) return VISCA_ERR_PARAM;
    status = visca_query_data(camera, query, sizeof(query), data, sizeof(data), 10, &data_size);
    if (status == VISCA_OK) {
        *pan = visca_decode_nibble16(data + 2U);
        *tilt = visca_decode_nibble16(data + 6U);
    }
    return status;
}

visca_status_t visca_camera_get_temperature(visca_camera_t *camera,
                                           int8_t *raw_temperature) {
    const uint8_t query[] = {0x04, 0x68};
    uint8_t value;
    visca_status_t status;
    if (raw_temperature == NULL) return VISCA_ERR_PARAM;
    status = visca_query_nibble8(camera, query, sizeof(query), 2U, &value);
    if (status == VISCA_OK) *raw_temperature = (int8_t)value;
    return status;
}

visca_status_t visca_camera_get_block(visca_camera_t *camera,
                                     visca_block_query_t block,
                                     uint8_t *data,
                                     size_t data_capacity,
                                     size_t *data_size) {
    const uint8_t query[] = {0x7e, 0x7e, (uint8_t)block};
    if (block < VISCA_BLOCK_LENS || block > VISCA_BLOCK_EXTENDED_3) {
        return VISCA_ERR_PARAM;
    }
    return visca_query_data(camera,
                            query,
                            sizeof(query),
                            data,
                            data_capacity,
                            13U,
                            data_size);
}
