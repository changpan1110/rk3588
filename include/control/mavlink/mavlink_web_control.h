#ifndef RK3588_MAVLINK_WEB_CONTROL_H
#define RK3588_MAVLINK_WEB_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#include "mavlink/common/mavlink.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAVLINK_WEB_CONTROL_CACHE_SIZE 64U

typedef enum {
    MAVLINK_WEB_ACTION_VIDEO_SWITCH = 1,
    MAVLINK_WEB_ACTION_SNAPSHOT = 2,
    MAVLINK_WEB_ACTION_RECORD_TOGGLE = 3,
    MAVLINK_WEB_ACTION_ZOOM_IN = 4,
    MAVLINK_WEB_ACTION_ZOOM_OUT = 5,
    MAVLINK_WEB_ACTION_TRIGGER = 6,
    MAVLINK_WEB_ACTION_FOCUS_IN = 7,
    MAVLINK_WEB_ACTION_FOCUS_OUT = 8,
    MAVLINK_WEB_ACTION_UNLOCK = 9,
    MAVLINK_WEB_ACTION_THERMAL_PSEUDOCOLOR = 10,
    MAVLINK_WEB_ACTION_LASER_SINGLE_MEASURE = 11,
    MAVLINK_WEB_ACTION_LASER_CONTINUOUS_MEASURE = 12,
    MAVLINK_WEB_ACTION_OSD_ENABLED = 13,
    MAVLINK_WEB_ACTION_GIMBAL_UP = 14,
    MAVLINK_WEB_ACTION_GIMBAL_DOWN = 15,
    MAVLINK_WEB_ACTION_GIMBAL_LEFT = 16,
    MAVLINK_WEB_ACTION_GIMBAL_RIGHT = 17,
    MAVLINK_WEB_ACTION_GIMBAL_CENTER = 18
} mavlink_web_action_t;

typedef enum {
    MAVLINK_WEB_EVENT_CLICK = 0,
    MAVLINK_WEB_EVENT_PRESS = 1,
    MAVLINK_WEB_EVENT_RELEASE = 2
} mavlink_web_event_t;

typedef struct {
    uint8_t source_system;
    uint8_t source_component;
    mavlink_web_action_t action;
    mavlink_web_event_t event;
    uint8_t value;
    uint32_t sequence;
    uint64_t command_uid;
} mavlink_web_command_t;

typedef uint8_t (*mavlink_web_command_handler_t)(
    const mavlink_web_command_t *command,
    void *user_data);

typedef struct {
    uint8_t valid;
    uint8_t source_system;
    uint8_t source_component;
    uint8_t result;
    uint64_t command_uid;
} mavlink_web_dedupe_entry_t;

typedef struct {
    mavlink_web_dedupe_entry_t cache[MAVLINK_WEB_CONTROL_CACHE_SIZE];
    size_t next_cache_index;
} mavlink_web_control_t;

typedef struct {
    mavlink_web_command_t command;
    uint8_t result;
    uint8_t duplicate;
} mavlink_web_handle_info_t;

void mavlink_web_control_init(mavlink_web_control_t *control);

/*
 * Returns 1 when message is an RK3588 web command and ack_out was filled.
 * Returns 0 when message belongs to another MAVLink command or target.
 */
int mavlink_web_control_handle(
    mavlink_web_control_t *control,
    const mavlink_message_t *message,
    uint8_t local_system,
    uint8_t local_component,
    mavlink_web_command_handler_t handler,
    void *user_data,
    mavlink_message_t *ack_out);

/* Same operation, with decoded command/result details for diagnostics. */
int mavlink_web_control_handle_ex(
    mavlink_web_control_t *control,
    const mavlink_message_t *message,
    uint8_t local_system,
    uint8_t local_component,
    mavlink_web_command_handler_t handler,
    void *user_data,
    mavlink_message_t *ack_out,
    mavlink_web_handle_info_t *info_out);

#ifdef __cplusplus
}
#endif

#endif
