#ifndef THERMAL_CAMERA_H
#define THERMAL_CAMERA_H

#include "input/serial/uart_base.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define THERMAL_CAMERA_DEVICE_ADDRESS 0x36U
#define THERMAL_CAMERA_FRAME_BEGIN 0xF0U
#define THERMAL_CAMERA_FRAME_END 0xFFU
#define THERMAL_CAMERA_MAX_DATA_SIZE 32U
#define THERMAL_CAMERA_MAX_FRAME_SIZE (THERMAL_CAMERA_MAX_DATA_SIZE + 8U)
#define THERMAL_CAMERA_DEFAULT_BAUDRATE 115200U

typedef enum {
    THERMAL_CAMERA_OK = 0,
    THERMAL_CAMERA_NO_DATA = 1,
    THERMAL_CAMERA_ERR_PARAM = -1,
    THERMAL_CAMERA_ERR_STATE = -2,
    THERMAL_CAMERA_ERR_UART = -3,
    THERMAL_CAMERA_ERR_TIMEOUT = -4,
    THERMAL_CAMERA_ERR_PROTOCOL = -5,
    THERMAL_CAMERA_ERR_CHECKSUM = -6,
    THERMAL_CAMERA_ERR_REMOTE = -7,
    THERMAL_CAMERA_ERR_RANGE = -8,
    THERMAL_CAMERA_ERR_BUSY = -9
} thermal_camera_status_t;

typedef enum {
    THERMAL_CAMERA_PSEUDOCOLOR_WHITE_HOT = 0x00,
    THERMAL_CAMERA_PSEUDOCOLOR_BLACK_HOT = 0x01,
    THERMAL_CAMERA_PSEUDOCOLOR_FUSION_1 = 0x02,
    THERMAL_CAMERA_PSEUDOCOLOR_RAINBOW = 0x03,
    THERMAL_CAMERA_PSEUDOCOLOR_FUSION_2 = 0x04,
    THERMAL_CAMERA_PSEUDOCOLOR_IRON_1 = 0x05,
    THERMAL_CAMERA_PSEUDOCOLOR_IRON_2 = 0x06,
    THERMAL_CAMERA_PSEUDOCOLOR_SEPIA = 0x07,
    THERMAL_CAMERA_PSEUDOCOLOR_COLOR_1 = 0x08,
    THERMAL_CAMERA_PSEUDOCOLOR_COLOR_2 = 0x09,
    THERMAL_CAMERA_PSEUDOCOLOR_ICE_FIRE = 0x0A,
    THERMAL_CAMERA_PSEUDOCOLOR_RAIN = 0x0B,
    THERMAL_CAMERA_PSEUDOCOLOR_GREEN_HOT = 0x0C,
    THERMAL_CAMERA_PSEUDOCOLOR_RED_HOT = 0x0D,
    THERMAL_CAMERA_PSEUDOCOLOR_DEEP_BLUE = 0x0E
} thermal_camera_pseudocolor_t;

typedef struct {
    uart_config_t uart;
    int opened;
    int debug_enabled;
    int loghex_enabled;
    int response_timeout_ms;
} thermal_camera_t;

void thermal_camera_init(thermal_camera_t *camera);
thermal_camera_status_t thermal_camera_open(thermal_camera_t *camera,
                                            const char *device,
                                            uint32_t baudrate);
void thermal_camera_close(thermal_camera_t *camera);
int thermal_camera_is_open(const thermal_camera_t *camera);
void thermal_camera_set_debug(thermal_camera_t *camera, int enabled);
void thermal_camera_set_loghex(thermal_camera_t *camera, int enabled);

thermal_camera_status_t thermal_camera_command(thermal_camera_t *camera,
                                               uint8_t class_id,
                                               uint8_t subcommand,
                                               uint8_t read_write,
                                               const uint8_t *data,
                                               size_t data_size,
                                               uint8_t *response_data,
                                               size_t response_capacity,
                                               size_t *response_size,
                                               uint8_t *response_flag);

thermal_camera_status_t thermal_camera_set_pseudocolor(
    thermal_camera_t *camera, thermal_camera_pseudocolor_t mode);
thermal_camera_status_t thermal_camera_get_pseudocolor(
    thermal_camera_t *camera, thermal_camera_pseudocolor_t *mode);
thermal_camera_status_t thermal_camera_set_brightness(thermal_camera_t *camera,
                                                      uint8_t value);
thermal_camera_status_t thermal_camera_get_brightness(thermal_camera_t *camera,
                                                      uint8_t *value);
thermal_camera_status_t thermal_camera_set_contrast(thermal_camera_t *camera,
                                                    uint8_t value);
thermal_camera_status_t thermal_camera_get_contrast(thermal_camera_t *camera,
                                                    uint8_t *value);
thermal_camera_status_t thermal_camera_query(thermal_camera_t *camera,
                                             uint8_t class_id,
                                             uint8_t subcommand,
                                             uint8_t *data,
                                             size_t capacity,
                                             size_t *size);
const char *thermal_camera_status_string(thermal_camera_status_t status);
const char *thermal_camera_pseudocolor_name(uint8_t mode);

#ifdef __cplusplus
}
#endif

#endif
