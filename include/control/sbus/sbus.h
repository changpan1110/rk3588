// #ifndef SBUS_H
// #define SBUS_H

// #include <stddef.h>
// #include <stdint.h>

// #include "input/serial/uart_base.h"

// #ifdef __cplusplus
// extern "C" {
// #endif

// #define SBUS_BAUDRATE 100000
// #define SBUS_FRAME_SIZE 25
// #define SBUS_CHANNEL_COUNT 16
// #define SBUS_CONTROL_CHANNEL_COUNT 8

// #define SBUS_CHANNEL_MIN 172
// #define SBUS_CHANNEL_CENTER 992
// #define SBUS_CHANNEL_MAX 1811

// typedef enum {
//     SBUS_OK = 0,
//     SBUS_NO_FRAME = 1,
//     SBUS_ERR_PARAM = -1,
//     SBUS_ERR_IO = -2,
//     SBUS_ERR_FRAME = -3,
//     SBUS_ERR_UNSUPPORTED = -4,
//     SBUS_ERR_NOMEM = -5
// } sbus_status_t;

// typedef struct {
//     uint16_t channels[SBUS_CHANNEL_COUNT];
//     uint8_t digital_channel_17;
//     uint8_t digital_channel_18;
//     uint8_t frame_lost;
//     uint8_t failsafe;
// } sbus_frame_t;

// typedef struct {
//     uint16_t raw[SBUS_CONTROL_CHANNEL_COUNT];
//     int16_t joystick[2];
//     int16_t knob[2];
//     uint8_t button[4];
//     uint8_t frame_lost;
//     uint8_t failsafe;
// } sbus_controls_t;

// typedef struct {
//     uint8_t buffer[SBUS_FRAME_SIZE];
//     size_t buffered;
//     int rx_timing_enabled;
//     int64_t last_uart_read_us;
//     int64_t last_frame_us;
//     uint64_t uart_read_sequence;
//     uint64_t frame_sequence;
// } sbus_parser_t;

// typedef struct {
//     uart_config_t uart;
//     sbus_parser_t parser;
// } sbus_data_t;

// void sbus_parser_init(sbus_parser_t *parser);
// sbus_status_t sbus_parse_frame(const uint8_t frame[SBUS_FRAME_SIZE], sbus_frame_t *result);
// size_t sbus_parser_feed(sbus_parser_t *parser,
//                         const uint8_t *data,
//                         size_t data_size,
//                         sbus_frame_t *latest_frame);
// void sbus_channels_decode(const sbus_frame_t *frame,
//                           sbus_controls_t *controls);

// /* Opens and configures one SBUS serial device at 100000 baud, 8E2. */
// sbus_status_t sbus_data_init(sbus_data_t *data, const char *device);
// /* Receives serial bytes and parses the latest complete SBUS frame. */
// sbus_status_t sbus_data_receive(sbus_data_t *data,
//                                 sbus_frame_t *frame);
// /* Encodes one SBUS frame and sends its 25 bytes to the serial device. */
// sbus_status_t sbus_data_send(sbus_data_t *data, const sbus_frame_t *frame);
// void sbus_data_deinit(sbus_data_t *data);

// const char *sbus_status_string(sbus_status_t status);

// #ifdef __cplusplus
// }
// #endif

// #endif
