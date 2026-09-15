// #include "control/sbus/sbus.h"

// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <time.h>

// #define SBUS_HEADER 0x0f
// #define SBUS_FLAGS_INDEX 23
// #define SBUS_FOOTER_INDEX 24
// #define SBUS_PAYLOAD_LAST_INDEX 22
// #define SBUS_RECEIVE_BUFFER_SIZE 128
// #define SBUS_RECEIVE_TIMEOUT_MS 30
// #define SBUS_EXPECTED_FRAME_PERIOD_US 10000LL
// #define SBUS_FRAME_DELAY_THRESHOLD_US 15000LL

// static int64_t sbus_monotonic_us(void) {
//     struct timespec now;

//     if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
//         return 0;
//     }
//     return (int64_t)now.tv_sec * 1000000LL + now.tv_nsec / 1000LL;
// }

// static int sbus_timing_debug_enabled(void) {
//     const char *value = getenv("SBUS_RX_TIMING");

//     return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
// }

// static uint64_t sbus_estimated_missed_frames(int64_t frame_period_us) {
//     uint64_t periods;

//     if (frame_period_us < SBUS_FRAME_DELAY_THRESHOLD_US) {
//         return 0;
//     }

//     periods = (uint64_t)((frame_period_us +
//                           SBUS_EXPECTED_FRAME_PERIOD_US / 2) /
//                          SBUS_EXPECTED_FRAME_PERIOD_US);
//     return periods > 0 ? periods - 1U : 0;
// }

// static int sbus_footer_valid(uint8_t footer) {
//     return footer == 0x00 || (footer & 0x0fU) == 0x04U;
// }

// static uint16_t sbus_channel_from_payload(const uint8_t *frame, size_t channel) {
//     const size_t bit_offset = channel * 11U;
//     const size_t byte_index = 1U + bit_offset / 8U;
//     const unsigned int shift = (unsigned int)(bit_offset % 8U);
//     uint32_t packed = frame[byte_index];

//     if (byte_index + 1U <= SBUS_PAYLOAD_LAST_INDEX) {
//         packed |= (uint32_t)frame[byte_index + 1U] << 8U;
//     }
//     if (byte_index + 2U <= SBUS_PAYLOAD_LAST_INDEX) {
//         packed |= (uint32_t)frame[byte_index + 2U] << 16U;
//     }

//     return (uint16_t)((packed >> shift) & 0x07ffU);
// }

// static int16_t sbus_scale_signed(uint16_t raw) {
//     int32_t scaled;

//     if (raw <= SBUS_CHANNEL_MIN) {
//         return -1000;
//     }
//     if (raw >= SBUS_CHANNEL_MAX) {
//         return 1000;
//     }
//     if (raw < SBUS_CHANNEL_CENTER) {
//         scaled = -1000 +
//                  ((int32_t)(raw - SBUS_CHANNEL_MIN) * 1000) /
//                      (SBUS_CHANNEL_CENTER - SBUS_CHANNEL_MIN);
//     } else {
//         scaled = ((int32_t)(raw - SBUS_CHANNEL_CENTER) * 1000) /
//                  (SBUS_CHANNEL_MAX - SBUS_CHANNEL_CENTER);
//     }

//     return (int16_t)scaled;
// }

// void sbus_parser_init(sbus_parser_t *parser) {
//     if (parser == NULL) {
//         return;
//     }

//     memset(parser, 0, sizeof(*parser));
// }

// sbus_status_t sbus_parse_frame(const uint8_t frame[SBUS_FRAME_SIZE], sbus_frame_t *result) {
//     size_t channel;

//     if (frame == NULL || result == NULL) {
//         return SBUS_ERR_PARAM;
//     }
//     if (frame[0] != SBUS_HEADER || !sbus_footer_valid(frame[SBUS_FOOTER_INDEX])) {
//         return SBUS_ERR_FRAME;
//     }

//     memset(result, 0, sizeof(*result));
//     for (channel = 0; channel < SBUS_CHANNEL_COUNT; ++channel) {
//         result->channels[channel] = sbus_channel_from_payload(frame, channel);
//     }

//     result->digital_channel_17 = (uint8_t)((frame[SBUS_FLAGS_INDEX] >> 0U) & 0x01U);
//     result->digital_channel_18 = (uint8_t)((frame[SBUS_FLAGS_INDEX] >> 1U) & 0x01U);
//     result->frame_lost = (uint8_t)((frame[SBUS_FLAGS_INDEX] >> 2U) & 0x01U);
//     result->failsafe = (uint8_t)((frame[SBUS_FLAGS_INDEX] >> 3U) & 0x01U);
//     return SBUS_OK;
// }

// static void sbus_parser_resync(sbus_parser_t *parser) {
//     size_t next_header;

//     for (next_header = 1; next_header < parser->buffered; ++next_header) {
//         if (parser->buffer[next_header] == SBUS_HEADER) {
//             const size_t remaining = parser->buffered - next_header;
//             memmove(parser->buffer, parser->buffer + next_header, remaining);
//             parser->buffered = remaining;
//             return;
//         }
//     }

//     parser->buffered = 0;
// }

// size_t sbus_parser_feed(sbus_parser_t *parser,
//                         const uint8_t *data,
//                         size_t data_size,
//                         sbus_frame_t *latest_frame) {
//     size_t index;
//     size_t frame_count = 0;

//     if (parser == NULL || data == NULL || latest_frame == NULL) {
//         return 0;
//     }

//     for (index = 0; index < data_size; ++index) {
//         if (parser->buffered == 0 && data[index] != SBUS_HEADER) {
//             continue;
//         }

//         parser->buffer[parser->buffered++] = data[index];
//         if (parser->buffered != SBUS_FRAME_SIZE) {
//             continue;
//         }

//         if (sbus_parse_frame(parser->buffer, latest_frame) == SBUS_OK) {
//             ++frame_count;
//             parser->buffered = 0;
//         } else {
//             sbus_parser_resync(parser);
//         }
//     }

//     return frame_count;
// }

// void sbus_channels_decode(const sbus_frame_t *frame,
//                           sbus_controls_t *controls) {
//     size_t channel;

//     if (frame == NULL || controls == NULL) {
//         return;
//     }

//     memset(controls, 0, sizeof(*controls));
//     for (channel = 0; channel < SBUS_CONTROL_CHANNEL_COUNT; ++channel) {
//         controls->raw[channel] = frame->channels[channel];
//     }

//     controls->joystick[0] = sbus_scale_signed(frame->channels[0]);
//     controls->joystick[1] = sbus_scale_signed(frame->channels[1]);
//     controls->knob[0] = sbus_scale_signed(frame->channels[2]);
//     controls->knob[1] = sbus_scale_signed(frame->channels[3]);
//     for (channel = 0; channel < 4; ++channel) {
//         controls->button[channel] =
//             (uint8_t)(frame->channels[channel + 4U] >= SBUS_CHANNEL_CENTER);
//     }
//     controls->frame_lost = frame->frame_lost;
//     controls->failsafe = frame->failsafe;
// }

// static sbus_status_t sbus_status_from_uart(uart_status_t status) {
//     switch (status) {
//         case UART_OK: return SBUS_OK;
//         case UART_NO_DATA: return SBUS_NO_FRAME;
//         case UART_ERR_PARAM: return SBUS_ERR_PARAM;
//         case UART_ERR_UNSUPPORTED: return SBUS_ERR_UNSUPPORTED;
//         case UART_ERR_IO: return SBUS_ERR_IO;
//         default: return SBUS_ERR_IO;
//     }
// }

// sbus_status_t sbus_data_init(sbus_data_t *data, const char *device) {
//     uart_status_t status;

//     if (data == NULL || device == NULL || device[0] == '\0' ||
//         strlen(device) >= sizeof(data->uart.device)) {
//         return SBUS_ERR_PARAM;
//     }

//     uart_base_config_init(&data->uart);
//     sbus_parser_init(&data->parser);
//     data->parser.rx_timing_enabled = sbus_timing_debug_enabled();
//     snprintf(data->uart.device, sizeof(data->uart.device), "%s", device);
//     data->uart.baudrate = SBUS_BAUDRATE;
//     data->uart.data_bits = 8;
//     data->uart.parity = UART_PARITY_EVEN;
//     data->uart.stop_bits = 2;
//     data->uart.nonblocking = 0;

//     status = uart_base_open_port(&data->uart);
//     if (status != UART_OK) {
//         return sbus_status_from_uart(status);
//     }

//     status = uart_base_flush(&data->uart, UART_FLUSH_INPUT);
//     if (status != UART_OK) {
//         uart_base_close_port(&data->uart);
//         return sbus_status_from_uart(status);
//     }

//     if (data->parser.rx_timing_enabled) {
//         fprintf(stderr,
//                 "SBUS UART timing enabled device=%s expected_frame_ms=%.3f\n",
//                 data->uart.device,
//                 (double)SBUS_EXPECTED_FRAME_PERIOD_US / 1000.0);
//         fflush(stderr);
//     }

//     return sbus_status_from_uart(status);
// }

// sbus_status_t sbus_data_receive(sbus_data_t *data,
//                                 sbus_frame_t *frame) {
//     uint8_t buffer[SBUS_RECEIVE_BUFFER_SIZE];
//     size_t bytes_read;
//     size_t frame_count;
//     size_t parser_buffered_before;
//     int64_t read_begin_us;
//     int64_t read_end_us;
//     int64_t uart_period_us = 0;
//     int64_t frame_period_us = 0;
//     uint64_t estimated_missed_frames = 0;
//     uart_status_t status;

//     if (data == NULL || frame == NULL || data->uart.fd < 0) {
//         return SBUS_ERR_PARAM;
//     }

//     parser_buffered_before = data->parser.buffered;
//     read_begin_us = sbus_monotonic_us();
//     status = uart_base_read_data(&data->uart,
//                                  buffer,
//                                  sizeof(buffer),
//                                  1,
//                                  SBUS_RECEIVE_TIMEOUT_MS,
//                                  &bytes_read);
//     read_end_us = sbus_monotonic_us();
//     if (status != UART_OK) {
//         if (data->parser.rx_timing_enabled && status == UART_NO_DATA) {
//             fprintf(stderr,
//                     "SBUS UART RX timeout mono_us=%lld wait_ms=%.3f "
//                     "parser_buffered=%zu\n",
//                     (long long)read_end_us,
//                     (double)(read_end_us - read_begin_us) / 1000.0,
//                     data->parser.buffered);
//             fflush(stderr);
//         }
//         return sbus_status_from_uart(status);
//     }

//     data->parser.uart_read_sequence++;
//     if (data->parser.last_uart_read_us > 0 &&
//         read_end_us > data->parser.last_uart_read_us) {
//         uart_period_us = read_end_us - data->parser.last_uart_read_us;
//     }
//     data->parser.last_uart_read_us = read_end_us;

//     frame_count = sbus_parser_feed(&data->parser,
//                                    buffer,
//                                    bytes_read,
//                                    frame);
//     if (frame_count > 0) {
//         data->parser.frame_sequence += frame_count;
//         if (data->parser.last_frame_us > 0 &&
//             read_end_us > data->parser.last_frame_us) {
//             frame_period_us = read_end_us - data->parser.last_frame_us;
//             estimated_missed_frames =
//                 sbus_estimated_missed_frames(frame_period_us);
//         }
//         data->parser.last_frame_us = read_end_us;
//     }

//     if (data->parser.rx_timing_enabled) {
//         fprintf(stderr,
//                 "SBUS UART RX seq=%llu mono_us=%lld bytes=%zu "
//                 "wait_ms=%.3f uart_period_ms=%.3f parser=%zu->%zu "
//                 "frames=%zu frame_seq=%llu frame_period_ms=%.3f "
//                 "estimated_missed=%llu%s\n",
//                 (unsigned long long)data->parser.uart_read_sequence,
//                 (long long)read_end_us,
//                 bytes_read,
//                 (double)(read_end_us - read_begin_us) / 1000.0,
//                 (double)uart_period_us / 1000.0,
//                 parser_buffered_before,
//                 data->parser.buffered,
//                 frame_count,
//                 (unsigned long long)data->parser.frame_sequence,
//                 (double)frame_period_us / 1000.0,
//                 (unsigned long long)estimated_missed_frames,
//                 frame_count > 1 ? " backlog=YES" : "");
//         fflush(stderr);
//     }
//     if (frame_count == 0) {
//         return SBUS_NO_FRAME;
//     }

//     return SBUS_OK;
// }

// static void sbus_build_frame_bytes(const sbus_frame_t *frame,
//                                    uint8_t bytes[SBUS_FRAME_SIZE]) {
//     size_t channel;

//     memset(bytes, 0, SBUS_FRAME_SIZE);
//     bytes[0] = SBUS_HEADER;
//     for (channel = 0; channel < SBUS_CHANNEL_COUNT; ++channel) {
//         const size_t bit_offset = channel * 11U;
//         const uint16_t value = frame->channels[channel] & 0x07ffU;
//         size_t bit;

//         for (bit = 0; bit < 11U; ++bit) {
//             if ((value & (uint16_t)(1U << bit)) != 0U) {
//                 const size_t payload_bit = bit_offset + bit;
//                 bytes[1U + payload_bit / 8U] |=
//                     (uint8_t)(1U << (payload_bit % 8U));
//             }
//         }
//     }
//     bytes[SBUS_FLAGS_INDEX] =
//         (uint8_t)((frame->digital_channel_17 ? 1U : 0U) |
//                   (frame->digital_channel_18 ? 2U : 0U) |
//                   (frame->frame_lost ? 4U : 0U) |
//                   (frame->failsafe ? 8U : 0U));
//     bytes[SBUS_FOOTER_INDEX] = 0x00;
// }

// sbus_status_t sbus_data_send(sbus_data_t *data, const sbus_frame_t *frame) {
//     uint8_t bytes[SBUS_FRAME_SIZE];
//     size_t sent = 0;
//     uart_status_t status;

//     if (data == NULL || frame == NULL || data->uart.fd < 0) {
//         return SBUS_ERR_PARAM;
//     }

//     sbus_build_frame_bytes(frame, bytes);
//     while (sent < sizeof(bytes)) {
//         size_t written = 0;

//         status = uart_base_write_data(&data->uart,
//                                       bytes + sent,
//                                       sizeof(bytes) - sent,
//                                       &written);
//         if (status != UART_OK) {
//             return sbus_status_from_uart(status);
//         }
//         sent += written;
//     }
//     return SBUS_OK;
// }

// void sbus_data_deinit(sbus_data_t *data) {
//     if (data == NULL) {
//         return;
//     }

//     uart_base_close_port(&data->uart);
//     sbus_parser_init(&data->parser);
// }

// const char *sbus_status_string(sbus_status_t status) {
//     switch (status) {
//         case SBUS_OK: return "ok";
//         case SBUS_NO_FRAME: return "no complete frame";
//         case SBUS_ERR_PARAM: return "invalid parameter";
//         case SBUS_ERR_IO: return "serial I/O error";
//         case SBUS_ERR_FRAME: return "invalid SBUS frame";
//         case SBUS_ERR_UNSUPPORTED: return "100000 baud is not supported";
//         case SBUS_ERR_NOMEM: return "out of memory";
//         default: return "unknown SBUS status";
//     }
// }
