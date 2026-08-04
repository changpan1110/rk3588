#include "serial/sbus.h"

#include <errno.h>
#include <string.h>

#ifdef __linux__
#include <asm/ioctls.h>
#include <asm/termbits.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#define SBUS_HEADER 0x0f
#define SBUS_FLAGS_INDEX 23
#define SBUS_FOOTER_INDEX 24
#define SBUS_PAYLOAD_LAST_INDEX 22

static int sbus_footer_valid(uint8_t footer) {
    return footer == 0x00 || (footer & 0x0fU) == 0x04U;
}

static uint16_t sbus_channel_from_payload(const uint8_t *frame, size_t channel) {
    const size_t bit_offset = channel * 11U;
    const size_t byte_index = 1U + bit_offset / 8U;
    const unsigned int shift = (unsigned int)(bit_offset % 8U);
    uint32_t packed = frame[byte_index];

    if (byte_index + 1U <= SBUS_PAYLOAD_LAST_INDEX) {
        packed |= (uint32_t)frame[byte_index + 1U] << 8U;
    }
    if (byte_index + 2U <= SBUS_PAYLOAD_LAST_INDEX) {
        packed |= (uint32_t)frame[byte_index + 2U] << 16U;
    }

    return (uint16_t)((packed >> shift) & 0x07ffU);
}

static int16_t sbus_scale_signed(uint16_t raw) {
    int32_t scaled;

    if (raw <= SBUS_CHANNEL_MIN) {
        return -1000;
    }
    if (raw >= SBUS_CHANNEL_MAX) {
        return 1000;
    }
    if (raw < SBUS_CHANNEL_CENTER) {
        scaled = -1000 +
                 ((int32_t)(raw - SBUS_CHANNEL_MIN) * 1000) /
                     (SBUS_CHANNEL_CENTER - SBUS_CHANNEL_MIN);
    } else {
        scaled = ((int32_t)(raw - SBUS_CHANNEL_CENTER) * 1000) /
                 (SBUS_CHANNEL_MAX - SBUS_CHANNEL_CENTER);
    }

    return (int16_t)scaled;
}

void sbus_parser_init(sbus_parser_t *parser) {
    if (parser == NULL) {
        return;
    }

    memset(parser, 0, sizeof(*parser));
}

sbus_status_t sbus_parse_frame(const uint8_t frame[SBUS_FRAME_SIZE], sbus_frame_t *result) {
    size_t channel;

    if (frame == NULL || result == NULL) {
        return SBUS_ERR_PARAM;
    }
    if (frame[0] != SBUS_HEADER || !sbus_footer_valid(frame[SBUS_FOOTER_INDEX])) {
        return SBUS_ERR_FRAME;
    }

    memset(result, 0, sizeof(*result));
    for (channel = 0; channel < SBUS_CHANNEL_COUNT; ++channel) {
        result->channels[channel] = sbus_channel_from_payload(frame, channel);
    }

    result->digital_channel_17 = (uint8_t)((frame[SBUS_FLAGS_INDEX] >> 0U) & 0x01U);
    result->digital_channel_18 = (uint8_t)((frame[SBUS_FLAGS_INDEX] >> 1U) & 0x01U);
    result->frame_lost = (uint8_t)((frame[SBUS_FLAGS_INDEX] >> 2U) & 0x01U);
    result->failsafe = (uint8_t)((frame[SBUS_FLAGS_INDEX] >> 3U) & 0x01U);
    return SBUS_OK;
}

static void sbus_parser_resync(sbus_parser_t *parser) {
    size_t next_header;

    for (next_header = 1; next_header < parser->buffered; ++next_header) {
        if (parser->buffer[next_header] == SBUS_HEADER) {
            const size_t remaining = parser->buffered - next_header;
            memmove(parser->buffer, parser->buffer + next_header, remaining);
            parser->buffered = remaining;
            return;
        }
    }

    parser->buffered = 0;
}

size_t sbus_parser_feed(sbus_parser_t *parser,
                        const uint8_t *data,
                        size_t data_size,
                        sbus_frame_t *latest_frame) {
    size_t index;
    size_t frame_count = 0;

    if (parser == NULL || data == NULL || latest_frame == NULL) {
        return 0;
    }

    for (index = 0; index < data_size; ++index) {
        if (parser->buffered == 0 && data[index] != SBUS_HEADER) {
            continue;
        }

        parser->buffer[parser->buffered++] = data[index];
        if (parser->buffered != SBUS_FRAME_SIZE) {
            continue;
        }

        if (sbus_parse_frame(parser->buffer, latest_frame) == SBUS_OK) {
            ++frame_count;
            parser->buffered = 0;
        } else {
            sbus_parser_resync(parser);
        }
    }

    return frame_count;
}

void sbus_channels_decode(const sbus_frame_t *frame,
                          sbus_controls_t *controls) {
    size_t channel;

    if (frame == NULL || controls == NULL) {
        return;
    }

    memset(controls, 0, sizeof(*controls));
    for (channel = 0; channel < SBUS_CONTROL_CHANNEL_COUNT; ++channel) {
        controls->raw[channel] = frame->channels[channel];
    }

    controls->joystick[0] = sbus_scale_signed(frame->channels[0]);
    controls->joystick[1] = sbus_scale_signed(frame->channels[1]);
    controls->knob[0] = sbus_scale_signed(frame->channels[2]);
    controls->knob[1] = sbus_scale_signed(frame->channels[3]);
    for (channel = 0; channel < 4; ++channel) {
        controls->button[channel] =
            (uint8_t)(frame->channels[channel + 4U] >= SBUS_CHANNEL_CENTER);
    }
    controls->frame_lost = frame->frame_lost;
    controls->failsafe = frame->failsafe;
}

#ifdef __linux__
static sbus_status_t sbus_configure_port(int fd) {
    struct termios2 tty;

    if (ioctl(fd, TCGETS2, &tty) < 0) {
        return SBUS_ERR_IO;
    }

    tty.c_iflag = IGNBRK | INPCK;
    tty.c_oflag = 0;
    tty.c_lflag = 0;
    tty.c_cflag &= ~(CBAUD | CSIZE | PARODD | CRTSCTS);
    tty.c_cflag |= BOTHER | CS8 | PARENB | CSTOPB | CLOCAL | CREAD;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    tty.c_ispeed = SBUS_BAUDRATE;
    tty.c_ospeed = SBUS_BAUDRATE;

    if (ioctl(fd, TCSETS2, &tty) < 0) {
        return SBUS_ERR_IO;
    }
    if (ioctl(fd, TCGETS2, &tty) < 0) {
        return SBUS_ERR_IO;
    }
    if (tty.c_ispeed != SBUS_BAUDRATE || tty.c_ospeed != SBUS_BAUDRATE) {
        return SBUS_ERR_UNSUPPORTED;
    }

    return SBUS_OK;
}
#endif

sbus_status_t sbus_data_init(sbus_data_t *data, const char *device) {
    sbus_status_t status;

    if (data == NULL || device == NULL || device[0] == '\0') {
        return SBUS_ERR_PARAM;
    }

    data->fd = -1;
    sbus_parser_init(&data->parser);

#ifndef __linux__
    (void)status;
    return SBUS_ERR_UNSUPPORTED;
#else
    data->fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (data->fd < 0) {
        return SBUS_ERR_IO;
    }

    status = sbus_configure_port(data->fd);
    if (status != SBUS_OK) {
        close(data->fd);
        data->fd = -1;
        return status;
    }

    return SBUS_OK;
#endif
}

sbus_status_t sbus_data_receive(sbus_data_t *data,
                                sbus_frame_t *frame) {
#ifndef __linux__
    (void)data;
    (void)frame;
    return SBUS_ERR_UNSUPPORTED;
#else
    uint8_t buffer[128];
    ssize_t bytes_read;
    size_t frame_count;

    if (data == NULL || frame == NULL || data->fd < 0) {
        return SBUS_ERR_PARAM;
    }

    bytes_read = read(data->fd, buffer, sizeof(buffer));
    if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return SBUS_NO_FRAME;
        }
        return SBUS_ERR_IO;
    }
    if (bytes_read == 0) {
        return SBUS_NO_FRAME;
    }

    frame_count = sbus_parser_feed(&data->parser,
                                   buffer,
                                   (size_t)bytes_read,
                                   frame);
    if (frame_count == 0) {
        return SBUS_NO_FRAME;
    }

    return SBUS_OK;
#endif
}

static void sbus_build_frame_bytes(const sbus_frame_t *frame,
                                   uint8_t bytes[SBUS_FRAME_SIZE]) {
    size_t channel;

    memset(bytes, 0, SBUS_FRAME_SIZE);
    bytes[0] = SBUS_HEADER;
    for (channel = 0; channel < SBUS_CHANNEL_COUNT; ++channel) {
        const size_t bit_offset = channel * 11U;
        const uint16_t value = frame->channels[channel] & 0x07ffU;
        size_t bit;

        for (bit = 0; bit < 11U; ++bit) {
            if ((value & (uint16_t)(1U << bit)) != 0U) {
                const size_t payload_bit = bit_offset + bit;
                bytes[1U + payload_bit / 8U] |=
                    (uint8_t)(1U << (payload_bit % 8U));
            }
        }
    }
    bytes[SBUS_FLAGS_INDEX] =
        (uint8_t)((frame->digital_channel_17 ? 1U : 0U) |
                  (frame->digital_channel_18 ? 2U : 0U) |
                  (frame->frame_lost ? 4U : 0U) |
                  (frame->failsafe ? 8U : 0U));
    bytes[SBUS_FOOTER_INDEX] = 0x00;
}

sbus_status_t sbus_data_send(sbus_data_t *data, const sbus_frame_t *frame) {
#ifndef __linux__
    (void)data;
    (void)frame;
    return SBUS_ERR_UNSUPPORTED;
#else
    uint8_t bytes[SBUS_FRAME_SIZE];
    size_t sent = 0;

    if (data == NULL || frame == NULL || data->fd < 0) {
        return SBUS_ERR_PARAM;
    }

    sbus_build_frame_bytes(frame, bytes);
    while (sent < sizeof(bytes)) {
        ssize_t written = write(data->fd, bytes + sent, sizeof(bytes) - sent);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SBUS_ERR_IO;
        }
        if (written == 0) {
            return SBUS_ERR_IO;
        }
        sent += (size_t)written;
    }
    return SBUS_OK;
#endif
}

void sbus_data_deinit(sbus_data_t *data) {
    if (data == NULL) {
        return;
    }

    if (data->fd >= 0) {
#ifdef __linux__
        close(data->fd);
#endif
    }
    data->fd = -1;
    sbus_parser_init(&data->parser);
}

const char *sbus_status_string(sbus_status_t status) {
    switch (status) {
        case SBUS_OK: return "ok";
        case SBUS_NO_FRAME: return "no complete frame";
        case SBUS_ERR_PARAM: return "invalid parameter";
        case SBUS_ERR_IO: return "serial I/O error";
        case SBUS_ERR_FRAME: return "invalid SBUS frame";
        case SBUS_ERR_UNSUPPORTED: return "100000 baud is not supported";
        case SBUS_ERR_NOMEM: return "out of memory";
        default: return "unknown SBUS status";
    }
}
