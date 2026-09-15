#ifndef RK3588_MAVLINK_CONTROL_SERVICE_H
#define RK3588_MAVLINK_CONTROL_SERVICE_H

#include <stdint.h>

#include "common/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mavlink_control_service mavlink_control_service_t;

typedef enum {
    MAVLINK_CONTROL_KEY_VIDEO_SWITCH = 1,
    MAVLINK_CONTROL_KEY_SNAPSHOT = 2,
    MAVLINK_CONTROL_KEY_RECORD_TOGGLE = 3,
    MAVLINK_CONTROL_KEY_ZOOM_IN = 4,
    MAVLINK_CONTROL_KEY_ZOOM_OUT = 5,
    MAVLINK_CONTROL_KEY_TRIGGER = 6,
    MAVLINK_CONTROL_KEY_FOCUS_IN = 7,
    MAVLINK_CONTROL_KEY_FOCUS_OUT = 8,
    MAVLINK_CONTROL_KEY_UNLOCK = 9,
    MAVLINK_CONTROL_KEY_THERMAL_PSEUDOCOLOR = 10,
    MAVLINK_CONTROL_KEY_LASER_SINGLE_MEASURE = 11,
    MAVLINK_CONTROL_KEY_LASER_CONTINUOUS_MEASURE = 12,
    MAVLINK_CONTROL_KEY_OSD_ENABLED = 13,
    MAVLINK_CONTROL_KEY_GIMBAL_UP = 14,
    MAVLINK_CONTROL_KEY_GIMBAL_DOWN = 15,
    MAVLINK_CONTROL_KEY_GIMBAL_LEFT = 16,
    MAVLINK_CONTROL_KEY_GIMBAL_RIGHT = 17,
    MAVLINK_CONTROL_KEY_GIMBAL_CENTER = 18
} mavlink_control_key_t;

typedef enum {
    MAVLINK_CONTROL_EVENT_CLICK = 0,
    MAVLINK_CONTROL_EVENT_PRESS = 1,
    MAVLINK_CONTROL_EVENT_RELEASE = 2
} mavlink_control_event_type_t;

typedef struct {
    uint8_t source_system;
    uint8_t source_component;
    mavlink_control_key_t key;
    mavlink_control_event_type_t event;
    uint8_t value;
    uint32_t sequence;
    uint64_t command_uid;
} mavlink_control_key_event_t;

/*
 * Runs in the MAVLink receiver thread after a valid, non-duplicate key event.
 * Return the business queue status so COMMAND_ACK reflects the real result.
 */
typedef app_status_t (*mavlink_control_key_callback_t)(
    const mavlink_control_key_event_t *event,
    void *user_data,
    uint64_t *out_request_id);

const char *mavlink_control_key_name(mavlink_control_key_t key);
const char *mavlink_control_event_name(mavlink_control_event_type_t event);

/*
 * Loads transport configuration, registers the key callback, and starts one
 * receiver thread. UDP/TCP/serial transport details remain inside the module.
 */
app_status_t mavlink_control_service_init(
    mavlink_control_service_t **out_service,
    const char *config_path,
    mavlink_control_key_callback_t key_callback,
    void *user_data);

/* Stops and joins the receiver thread. Safe to call with NULL. */
void mavlink_control_service_deinit(mavlink_control_service_t *service);

#ifdef __cplusplus
}
#endif

#endif
