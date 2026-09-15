#define _POSIX_C_SOURCE 200809L

#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "dji_rsdk_protocol.c"

#include "common/debug.h"
#include "control/dji_rsdk/dji_rsdk.h"
#include "control/dji_rsdk/dji_rsdk_crc.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define DJI_RSDK_SOF 0xAAU
#define DJI_RSDK_HEADER_SIZE 12U
#define DJI_RSDK_COMMAND_PREFIX_SIZE 2U
#define DJI_RSDK_CRC32_SIZE 4U
#define DJI_RSDK_PACKET_OVERHEAD \
    (DJI_RSDK_HEADER_SIZE + DJI_RSDK_COMMAND_PREFIX_SIZE + DJI_RSDK_CRC32_SIZE)
#define DJI_RSDK_SEND_TIMEOUT_MS 1000
#define DJI_RSDK_FRAME_TYPE_ACK 0x20U
#define DJI_RSDK_VERSION_SHIFT 10U
#define DJI_RSDK_LENGTH_MASK 0x03FFU
#define DJI_RSDK_SOF_OFFSET 0U
#define DJI_RSDK_LENGTH_OFFSET 1U
#define DJI_RSDK_COMMAND_TYPE_OFFSET 3U
#define DJI_RSDK_ENCRYPTION_OFFSET 4U
#define DJI_RSDK_SEQUENCE_OFFSET 8U
#define DJI_RSDK_CRC16_OFFSET 10U
#define DJI_RSDK_COMMAND_SET_OFFSET 12U
#define DJI_RSDK_COMMAND_ID_OFFSET 13U
#define DJI_RSDK_COMMAND_DATA_OFFSET 14U
#define DJI_RSDK_PACKET_LOG_PREFIX_SIZE 160U
#define DJI_RSDK_PACKET_LOG_BYTE_SIZE 3U
#define DJI_RSDK_PACKET_LOG_BUFFER_SIZE \
    (DJI_RSDK_PACKET_LOG_PREFIX_SIZE + \
     DJI_RSDK_MAX_PACKET_SIZE * DJI_RSDK_PACKET_LOG_BYTE_SIZE)

#define DJI_RSDK_READ_U16_LE(data) \
    ((uint16_t)((uint16_t)(data)[0] | ((uint16_t)(data)[1] << 8)))

#define DJI_RSDK_READ_U32_LE(data) \
    ((uint32_t)(data)[0] | ((uint32_t)(data)[1] << 8) | \
     ((uint32_t)(data)[2] << 16) | ((uint32_t)(data)[3] << 24))

#define DJI_RSDK_WRITE_U16_LE(data, value)                 \
    do {                                                   \
        uint8_t *dji_data_ = (data);                       \
        uint16_t dji_value_ = (uint16_t)(value);           \
        dji_data_[0] = (uint8_t)(dji_value_ & 0xFFU);      \
        dji_data_[1] = (uint8_t)(dji_value_ >> 8);         \
    } while (0)

#define DJI_RSDK_WRITE_U32_LE(data, value)                     \
    do {                                                       \
        uint8_t *dji_data_ = (data);                           \
        uint32_t dji_value_ = (uint32_t)(value);               \
        dji_data_[0] = (uint8_t)(dji_value_ & 0xFFU);          \
        dji_data_[1] = (uint8_t)((dji_value_ >> 8) & 0xFFU);   \
        dji_data_[2] = (uint8_t)((dji_value_ >> 16) & 0xFFU);  \
        dji_data_[3] = (uint8_t)(dji_value_ >> 24);            \
    } while (0)

static int64_t dji_rsdk_now_ms(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static dji_rsdk_status_t dji_rsdk_from_can_status(can_socket_status_t status) {
    switch (status) {
        case CAN_SOCKET_OK: return DJI_RSDK_OK;
        case CAN_SOCKET_NO_DATA: return DJI_RSDK_NO_DATA;
        case CAN_SOCKET_ERR_TIMEOUT: return DJI_RSDK_ERR_TIMEOUT;
        case CAN_SOCKET_ERR_PARAM: return DJI_RSDK_ERR_PARAM;
        case CAN_SOCKET_ERR_STATE: return DJI_RSDK_ERR_STATE;
        default: return DJI_RSDK_ERR_CAN;
    }
}

static void dji_rsdk_log_packet(const dji_rsdk_t *rsdk,
                                const char *direction,
                                const uint8_t *packet,
                                size_t packet_size) {
    char log_buffer[DJI_RSDK_PACKET_LOG_BUFFER_SIZE];
    size_t used;
    size_t i;
    int written;

    if (rsdk == NULL || packet == NULL || !rsdk->loghex_enabled) {
        return;
    }

    written = snprintf(
        log_buffer,
        sizeof(log_buffer),
        "[DJI_RSDK][%s] packet_len=%zu cmd_data_len=%zu can_frames=%zu bytes=",
        direction,
        packet_size,
        packet_size >= DJI_RSDK_PACKET_OVERHEAD
            ? packet_size - DJI_RSDK_PACKET_OVERHEAD
            : 0U,
        (packet_size + CAN_SOCKET_CLASSIC_DATA_MAX - 1U) /
            CAN_SOCKET_CLASSIC_DATA_MAX);
    if (written < 0) {
        return;
    }
    used = (size_t)written < sizeof(log_buffer)
               ? (size_t)written
               : sizeof(log_buffer) - 1U;

    for (i = 0U; i < packet_size && used < sizeof(log_buffer) - 1U; ++i) {
        written = snprintf(log_buffer + used,
                           sizeof(log_buffer) - used,
                           "%s%02X",
                           i == 0U ? "" : " ",
                           (unsigned)packet[i]);
        if (written < 0) {
            return;
        }
        used += (size_t)written < sizeof(log_buffer) - used
                    ? (size_t)written
                    : sizeof(log_buffer) - used - 1U;
    }
    LOGD("%s", log_buffer);
}

static dji_rsdk_status_t dji_rsdk_build_packet(
    uint16_t sequence,
    uint8_t command_set,
    uint8_t command_id,
    const uint8_t *data,
    size_t data_size,
    dji_rsdk_ack_policy_t ack_policy,
    uint8_t *packet,
    size_t packet_capacity,
    size_t *packet_size) {
    size_t total_size;

    if ((data_size > 0U && data == NULL) || packet == NULL ||
        packet_size == NULL || data_size > DJI_RSDK_MAX_COMMAND_DATA_SIZE ||
        (ack_policy != DJI_RSDK_ACK_NONE &&
         ack_policy != DJI_RSDK_ACK_OPTIONAL &&
         ack_policy != DJI_RSDK_ACK_REQUIRED)) {
        return DJI_RSDK_ERR_PARAM;
    }
    total_size = data_size + DJI_RSDK_PACKET_OVERHEAD;
    if (total_size > packet_capacity || total_size > DJI_RSDK_MAX_PACKET_SIZE) {
        return DJI_RSDK_ERR_BUFFER;
    }

    memset(packet, 0, total_size);
    packet[DJI_RSDK_SOF_OFFSET] = DJI_RSDK_SOF;
    DJI_RSDK_WRITE_U16_LE(packet + DJI_RSDK_LENGTH_OFFSET, total_size);
    packet[DJI_RSDK_COMMAND_TYPE_OFFSET] = (uint8_t)ack_policy;
    DJI_RSDK_WRITE_U16_LE(packet + DJI_RSDK_SEQUENCE_OFFSET, sequence);
    DJI_RSDK_WRITE_U16_LE(
        packet + DJI_RSDK_CRC16_OFFSET,
        dji_rsdk_crc16_calculate(packet, DJI_RSDK_CRC16_OFFSET));
    packet[DJI_RSDK_COMMAND_SET_OFFSET] = command_set;
    packet[DJI_RSDK_COMMAND_ID_OFFSET] = command_id;
    if (data_size > 0U) {
        memcpy(packet + DJI_RSDK_COMMAND_DATA_OFFSET, data, data_size);
    }
    DJI_RSDK_WRITE_U32_LE(
        packet + total_size - DJI_RSDK_CRC32_SIZE,
        dji_rsdk_crc32_calculate(packet,
                                 total_size - DJI_RSDK_CRC32_SIZE));
    *packet_size = total_size;
    return DJI_RSDK_OK;
}

static void dji_rsdk_discard_rx(dji_rsdk_t *rsdk, size_t size) {
    if (size >= rsdk->rx_size) {
        rsdk->rx_size = 0U;
        return;
    }
    memmove(rsdk->rx_buffer,
            rsdk->rx_buffer + size,
            rsdk->rx_size - size);
    rsdk->rx_size -= size;
}

static dji_rsdk_status_t dji_rsdk_extract_frame(dji_rsdk_t *rsdk,
                                                dji_rsdk_frame_t *frame) {
    size_t sof_offset = 0U;
    uint16_t version_length;
    size_t packet_size;
    size_t data_size;
    uint16_t expected_crc16;
    uint32_t expected_crc32;

    while (sof_offset < rsdk->rx_size &&
           rsdk->rx_buffer[sof_offset] != DJI_RSDK_SOF) {
        ++sof_offset;
    }
    if (sof_offset > 0U) {
        dji_rsdk_discard_rx(rsdk, sof_offset);
    }
    if (rsdk->rx_size < 3U) {
        return DJI_RSDK_NO_DATA;
    }

    version_length = DJI_RSDK_READ_U16_LE(
        rsdk->rx_buffer + DJI_RSDK_LENGTH_OFFSET);
    packet_size = version_length & DJI_RSDK_LENGTH_MASK;
    if ((version_length >> DJI_RSDK_VERSION_SHIFT) != 0U ||
        packet_size < DJI_RSDK_PACKET_OVERHEAD ||
        packet_size > DJI_RSDK_MAX_PACKET_SIZE) {
        dji_rsdk_discard_rx(rsdk, 1U);
        return DJI_RSDK_ERR_PROTOCOL;
    }
    if (rsdk->rx_size < packet_size) {
        return DJI_RSDK_NO_DATA;
    }

    expected_crc16 = DJI_RSDK_READ_U16_LE(
        rsdk->rx_buffer + DJI_RSDK_CRC16_OFFSET);
    expected_crc32 = DJI_RSDK_READ_U32_LE(
        rsdk->rx_buffer + packet_size - DJI_RSDK_CRC32_SIZE);
    if (!dji_rsdk_crc16_verify(rsdk->rx_buffer,
                               DJI_RSDK_CRC16_OFFSET,
                               expected_crc16) ||
        !dji_rsdk_crc32_verify(rsdk->rx_buffer,
                               packet_size - DJI_RSDK_CRC32_SIZE,
                               expected_crc32)) {
        dji_rsdk_discard_rx(rsdk, 1U);
        return DJI_RSDK_ERR_CRC;
    }
    if (rsdk->rx_buffer[DJI_RSDK_ENCRYPTION_OFFSET] != 0U) {
        dji_rsdk_discard_rx(rsdk, packet_size);
        return DJI_RSDK_ERR_PROTOCOL;
    }

    memset(frame, 0, sizeof(*frame));
    frame->sequence = DJI_RSDK_READ_U16_LE(
        rsdk->rx_buffer + DJI_RSDK_SEQUENCE_OFFSET);
    frame->command_type =
        rsdk->rx_buffer[DJI_RSDK_COMMAND_TYPE_OFFSET] & 0x1FU;
    frame->is_ack =
        (rsdk->rx_buffer[DJI_RSDK_COMMAND_TYPE_OFFSET] &
         DJI_RSDK_FRAME_TYPE_ACK) != 0U;
    frame->command_set = rsdk->rx_buffer[DJI_RSDK_COMMAND_SET_OFFSET];
    frame->command_id = rsdk->rx_buffer[DJI_RSDK_COMMAND_ID_OFFSET];
    data_size = packet_size - DJI_RSDK_PACKET_OVERHEAD;
    frame->data_size = data_size;
    if (data_size > 0U) {
        memcpy(frame->data,
               rsdk->rx_buffer + DJI_RSDK_COMMAND_DATA_OFFSET,
               data_size);
    }
    dji_rsdk_log_packet(rsdk, "RX", rsdk->rx_buffer, packet_size);
    dji_rsdk_discard_rx(rsdk, packet_size);
    return DJI_RSDK_OK;
}

void dji_rsdk_init(dji_rsdk_t *rsdk) {
    if (rsdk == NULL) {
        return;
    }
    memset(rsdk, 0, sizeof(*rsdk));
    can_socket_init(&rsdk->can);
    rsdk->next_sequence = 1U;
    rsdk->initialized = 1;
}

dji_rsdk_status_t dji_rsdk_open(dji_rsdk_t *rsdk,
                                const char *can_interface) {
    can_socket_status_t status;

    if (rsdk == NULL || can_interface == NULL || can_interface[0] == '\0') {
        return DJI_RSDK_ERR_PARAM;
    }
    if (!rsdk->initialized) {
        dji_rsdk_init(rsdk);
    }
    status = can_socket_open(&rsdk->can,
                             can_interface,
                             DJI_RSDK_PC_TX_CAN_ID,
                             DJI_RSDK_PC_RX_CAN_ID);
    if (status != CAN_SOCKET_OK) {
        return dji_rsdk_from_can_status(status);
    }
    can_socket_set_log_frames(&rsdk->can, rsdk->loghex_enabled);
    rsdk->rx_size = 0U;
    return DJI_RSDK_OK;
}

void dji_rsdk_close(dji_rsdk_t *rsdk) {
    if (rsdk == NULL) {
        return;
    }
    can_socket_close(&rsdk->can);
    rsdk->rx_size = 0U;
}

int dji_rsdk_is_open(const dji_rsdk_t *rsdk) {
    return rsdk != NULL && can_socket_is_open(&rsdk->can);
}

void dji_rsdk_set_debug(dji_rsdk_t *rsdk, int enabled) {
    if (rsdk != NULL) {
        rsdk->debug_enabled = enabled != 0;
    }
}

void dji_rsdk_set_loghex(dji_rsdk_t *rsdk, int enabled) {
    if (rsdk != NULL) {
        rsdk->loghex_enabled = enabled != 0;
        can_socket_set_log_frames(&rsdk->can, enabled);
    }
}

void dji_rsdk_set_unsolicited_callback(
    dji_rsdk_t *rsdk,
    dji_rsdk_unsolicited_callback_t callback,
    void *user_data) {
    if (rsdk != NULL) {
        rsdk->unsolicited_callback = callback;
        rsdk->unsolicited_user_data = user_data;
    }
}

dji_rsdk_status_t dji_rsdk_send_command(
    dji_rsdk_t *rsdk,
    uint8_t command_set,
    uint8_t command_id,
    const uint8_t *data,
    size_t data_size,
    dji_rsdk_ack_policy_t ack_policy,
    uint16_t *sequence) {
    uint8_t packet[DJI_RSDK_MAX_PACKET_SIZE];
    size_t packet_size = 0U;
    uint16_t current_sequence;
    dji_rsdk_status_t status;

    if (rsdk == NULL || !dji_rsdk_is_open(rsdk)) {
        return DJI_RSDK_ERR_STATE;
    }
    current_sequence = rsdk->next_sequence++;
    if (rsdk->next_sequence == 0U) {
        rsdk->next_sequence = 1U;
    }
    status = dji_rsdk_build_packet(current_sequence,
                                   command_set,
                                   command_id,
                                   data,
                                   data_size,
                                   ack_policy,
                                   packet,
                                   sizeof(packet),
                                   &packet_size);
    if (status != DJI_RSDK_OK) {
        return status;
    }
    dji_rsdk_log_packet(rsdk, "TX", packet, packet_size);
    status = dji_rsdk_from_can_status(can_socket_send(&rsdk->can,
                                                       packet,
                                                       packet_size,
                                                       DJI_RSDK_SEND_TIMEOUT_MS));
    if (status == DJI_RSDK_OK && sequence != NULL) {
        *sequence = current_sequence;
    }
    return status;
}

dji_rsdk_status_t dji_rsdk_receive_frame(dji_rsdk_t *rsdk,
                                         int timeout_ms,
                                         dji_rsdk_frame_t *frame) {
    int64_t deadline_ms;
    dji_rsdk_status_t last_parse_error = DJI_RSDK_NO_DATA;

    if (rsdk == NULL || frame == NULL || timeout_ms < 0) {
        return DJI_RSDK_ERR_PARAM;
    }
    if (!dji_rsdk_is_open(rsdk)) {
        return DJI_RSDK_ERR_STATE;
    }
    deadline_ms = dji_rsdk_now_ms() + timeout_ms;

    for (;;) {
        dji_rsdk_status_t status = dji_rsdk_extract_frame(rsdk, frame);

        if (status == DJI_RSDK_OK) {
            return status;
        }
        if (status == DJI_RSDK_ERR_CRC || status == DJI_RSDK_ERR_PROTOCOL) {
            last_parse_error = status;
            continue;
        }
        if (status != DJI_RSDK_NO_DATA) {
            return status;
        }

        {
            uint8_t chunk[CAN_SOCKET_CLASSIC_DATA_MAX];
            size_t chunk_size = 0U;
            int64_t remaining = deadline_ms - dji_rsdk_now_ms();
            can_socket_status_t can_status;

            if (remaining <= 0) {
                return last_parse_error == DJI_RSDK_NO_DATA
                           ? DJI_RSDK_ERR_TIMEOUT
                           : last_parse_error;
            }
            can_status = can_socket_receive(
                &rsdk->can,
                chunk,
                &chunk_size,
                remaining > 2147483647LL ? 2147483647 : (int)remaining);
            if (can_status != CAN_SOCKET_OK) {
                return dji_rsdk_from_can_status(can_status);
            }
            if (chunk_size == 0U) {
                continue;
            }
            if (rsdk->rx_size + chunk_size > sizeof(rsdk->rx_buffer)) {
                rsdk->rx_size = 0U;
                return DJI_RSDK_ERR_BUFFER;
            }
            memcpy(rsdk->rx_buffer + rsdk->rx_size, chunk, chunk_size);
            rsdk->rx_size += chunk_size;
        }
    }
}

dji_rsdk_status_t dji_rsdk_request(dji_rsdk_t *rsdk,
                                   uint8_t command_set,
                                   uint8_t command_id,
                                   const uint8_t *request_data,
                                   size_t request_size,
                                   int timeout_ms,
                                   uint8_t *response_data,
                                   size_t response_capacity,
                                   size_t *response_size,
                                   uint8_t *remote_return_code) {
    uint16_t sequence = 0U;
    int64_t deadline_ms;
    dji_rsdk_status_t status;

    if (timeout_ms < 0 ||
        (response_capacity > 0U && response_data == NULL) ||
        response_size == NULL) {
        return DJI_RSDK_ERR_PARAM;
    }
    *response_size = 0U;
    if (remote_return_code != NULL) {
        *remote_return_code = 0xFFU;
    }

    status = dji_rsdk_send_command(rsdk,
                                   command_set,
                                   command_id,
                                   request_data,
                                   request_size,
                                   DJI_RSDK_ACK_REQUIRED,
                                   &sequence);
    if (status != DJI_RSDK_OK) {
        return status;
    }

    deadline_ms = dji_rsdk_now_ms() + timeout_ms;
    for (;;) {
        dji_rsdk_frame_t frame;
        int64_t remaining = deadline_ms - dji_rsdk_now_ms();

        if (remaining <= 0) {
            return DJI_RSDK_ERR_TIMEOUT;
        }
        status = dji_rsdk_receive_frame(
            rsdk,
            remaining > 2147483647LL ? 2147483647 : (int)remaining,
            &frame);
        if (status != DJI_RSDK_OK) {
            return status;
        }
        if (!frame.is_ack || frame.sequence != sequence ||
            frame.command_set != command_set || frame.command_id != command_id) {
            if (rsdk->unsolicited_callback != NULL) {
                rsdk->unsolicited_callback(&frame,
                                           rsdk->unsolicited_user_data);
            } else if (rsdk->debug_enabled) {
                LOGD("[DJI_RSDK] ignored frame seq=%u set=0x%02X id=0x%02X",
                     (unsigned)frame.sequence,
                     (unsigned)frame.command_set,
                     (unsigned)frame.command_id);
            }
            continue;
        }
        if (frame.data_size < 1U) {
            return DJI_RSDK_ERR_PROTOCOL;
        }
        if (remote_return_code != NULL) {
            *remote_return_code = frame.data[0];
        }
        if (frame.data_size - 1U > response_capacity) {
            return DJI_RSDK_ERR_BUFFER;
        }
        if (frame.data_size > 1U) {
            memcpy(response_data, frame.data + 1U, frame.data_size - 1U);
        }
        *response_size = frame.data_size - 1U;
        return frame.data[0] == 0U ? DJI_RSDK_OK : DJI_RSDK_ERR_REMOTE;
    }
}

dji_rsdk_status_t dji_rsdk_protocol_self_test(void) {
    static const uint8_t expected[] = {
        0xAA, 0x1A, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
        0x22, 0x11, 0xA2, 0x42, 0x0E, 0x00, 0x20, 0x00,
        0x30, 0x00, 0x40, 0x00, 0x01, 0x14, 0x7B, 0x40,
        0x97, 0xBE
    };
    static const uint8_t command_data[] = {
        0x20, 0x00, 0x30, 0x00, 0x40, 0x00, 0x01, 0x14
    };
    uint8_t packet[sizeof(expected)];
    size_t packet_size = 0U;
    dji_rsdk_status_t status;

    if (!dji_rsdk_crc_self_test()) {
        return DJI_RSDK_ERR_CRC;
    }
    status = dji_rsdk_build_packet(0x1122U,
                                   DJI_RSDK_GIMBAL_COMMAND_SET,
                                   0x00U,
                                   command_data,
                                   sizeof(command_data),
                                   DJI_RSDK_ACK_REQUIRED,
                                   packet,
                                   sizeof(packet),
                                   &packet_size);
    if (status != DJI_RSDK_OK || packet_size != sizeof(expected) ||
        memcmp(packet, expected, sizeof(expected)) != 0) {
        return DJI_RSDK_ERR_CRC;
    }
    return DJI_RSDK_OK;
}

const char *dji_rsdk_status_string(dji_rsdk_status_t status) {
    switch (status) {
        case DJI_RSDK_OK: return "ok";
        case DJI_RSDK_NO_DATA: return "no data";
        case DJI_RSDK_ERR_PARAM: return "invalid parameter";
        case DJI_RSDK_ERR_STATE: return "R SDK CAN interface is not open";
        case DJI_RSDK_ERR_CAN: return "SocketCAN error";
        case DJI_RSDK_ERR_TIMEOUT: return "timeout";
        case DJI_RSDK_ERR_CRC: return "CRC check failed";
        case DJI_RSDK_ERR_PROTOCOL: return "invalid R SDK packet";
        case DJI_RSDK_ERR_REMOTE: return "gimbal returned an error";
        case DJI_RSDK_ERR_RANGE: return "value is outside the documented range";
        case DJI_RSDK_ERR_BUFFER: return "buffer is too small";
        default: return "unknown R SDK status";
    }
}

const char *dji_rsdk_remote_code_string(uint8_t return_code) {
    switch (return_code) {
        case 0x00: return "command succeeded";
        case 0x01: return "command parse error";
        case 0x02: return "command execution failed";
        case 0xFF: return "undefined error";
        default: return "unknown return code";
    }
}
