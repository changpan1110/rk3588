#ifndef RK3588_VISCA_4K_CAMERA_H
#define RK3588_VISCA_4K_CAMERA_H

#include <stdint.h>

#include "control/visca_camera/visca_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VISCA_4K_CAMERA_ACTION_ZOOM_TELE = 0,
    VISCA_4K_CAMERA_ACTION_ZOOM_WIDE,
    VISCA_4K_CAMERA_ACTION_FOCUS_FAR,
    VISCA_4K_CAMERA_ACTION_FOCUS_NEAR
} visca_4k_camera_action_t;

/* Initialize with the selected serial port/baudrate and create the thread. */
visca_status_t visca_4k_camera_init(const char *device, uint32_t baudrate);
/* Stop the action thread and close the VISCA port. */
void visca_4k_camera_stop(void);
/* Release/reset module state after stop. */
void visca_4k_camera_deinit(void);
int visca_4k_camera_is_running(void);

/* event: 1=press, 2=release. A release stops the current movement. */
visca_status_t visca_4k_camera_action_request(
    visca_4k_camera_action_t action,
    uint8_t event,
    uint8_t speed);

#ifdef __cplusplus
}
#endif

#endif
