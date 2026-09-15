#ifndef HIGH_SPEED_CAMERA_H
#define HIGH_SPEED_CAMERA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HIGH_SPEED_CAMERA_FRAME_SIZE 7U
#define HIGH_SPEED_CAMERA_DISCONNECT_TIMEOUT_MS 5000

typedef enum {
    HIGH_SPEED_CAMERA_OK = 0,
    HIGH_SPEED_CAMERA_NO_DATA = 1,
    HIGH_SPEED_CAMERA_ERR_PARAM = -1,
    HIGH_SPEED_CAMERA_ERR_STATE = -2,
    HIGH_SPEED_CAMERA_ERR_UART = -3,
    HIGH_SPEED_CAMERA_ERR_CRC = -4,
    HIGH_SPEED_CAMERA_ERR_PROTOCOL = -5
} high_speed_camera_status_t;

typedef enum {
    HIGH_SPEED_CAMERA_FRAME_KEY_REQUEST = 0x01,
    HIGH_SPEED_CAMERA_FRAME_HEARTBEAT_REQUEST = 0x02,
    HIGH_SPEED_CAMERA_FRAME_HEARTBEAT_RESPONSE = 0x03,
    HIGH_SPEED_CAMERA_FRAME_ERROR_RESPONSE = 0x04
} high_speed_camera_frame_type_t;

typedef enum {
    HIGH_SPEED_CAMERA_KEY_PREV = 1,
    HIGH_SPEED_CAMERA_KEY_MENU = 2,
    HIGH_SPEED_CAMERA_KEY_NEXT = 3,
    HIGH_SPEED_CAMERA_KEY_TRIGGER = 4,
    HIGH_SPEED_CAMERA_KEY_PLAYBACK = 5,
    HIGH_SPEED_CAMERA_KEY_STOP_RECORDING = 6,
    HIGH_SPEED_CAMERA_KEY_UNLOCK = 7
} high_speed_camera_key_t;

/* The module owns all protocol and UART state. */
high_speed_camera_status_t high_speed_camera_init(void);
high_speed_camera_status_t high_speed_camera_open(const char *device,
                                                  uint32_t baudrate);
void high_speed_camera_close(void);
int high_speed_camera_is_open(void);
int high_speed_camera_is_connected(void);

void high_speed_camera_set_debug(int enabled);
void high_speed_camera_set_loghex(int enabled);
int high_speed_camera_get_debug(void);
int high_speed_camera_get_loghex(void);

high_speed_camera_status_t high_speed_camera_send_key(
    high_speed_camera_key_t key);
high_speed_camera_status_t high_speed_camera_send_heartbeat(void);
high_speed_camera_status_t high_speed_camera_send_error_response(void);

/* Valid key/heartbeat requests are acknowledged automatically. */
high_speed_camera_status_t high_speed_camera_process_once(
    int timeout_ms,
    uint8_t *frame_type,
    uint8_t *data,
    uint8_t *count);

/* Checks the protocol against all frames supplied in the specification. */
high_speed_camera_status_t high_speed_camera_self_test(void);

const char *high_speed_camera_key_name(uint8_t key_id);
const char *high_speed_camera_frame_type_name(uint8_t frame_type);
const char *high_speed_camera_status_string(high_speed_camera_status_t status);

#ifdef __cplusplus
}
#endif

#endif
