#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "mavlink_control_actions.c"

#include "control/mavlink/mavlink_control_actions.h"
#include "control/high_speed_camera/high_speed_camera.h"
#include "control/dji_rsdk/dji_rsdk.h"
#include "control/sfl0603_laser/sfl0603_laser.h"
#include "control/thermal_camera/thermal_camera.h"
#include "control/visca_4k_camera/visca_4k_camera.h"
#include "service/dji_rsdk_service.h"
#include "service/high_speed_camera_service.h"
#include "service/motor_service.h"
#include "service/sfl0603_laser_service.h"
#include "service/thermal_camera_service.h"

#include <stdlib.h>
#include <string.h>

#include "common/debug.h"

_Static_assert(THERMAL_CAMERA_PSEUDOCOLOR_WHITE_HOT == 0U,
               "thermal pseudocolor base mismatch");
_Static_assert(THERMAL_CAMERA_PSEUDOCOLOR_DEEP_BLUE == 14U,
               "thermal pseudocolor range mismatch");

struct mavlink_control_actions {
    vp_ctx_t *pipeline;
    vp_control_t *video_control;
};

static int mavlink_control_any_recording(vp_ctx_t *pipeline) {
    int channel;

    for (channel = 0; channel < vp_channel_count(pipeline); ++channel) {
        if (vp_is_recording(pipeline, (vp_channel_id_t)channel)) {
            return 1;
        }
    }
    return 0;
}

static const char *mavlink_control_pseudocolor_name(uint8_t mode) {
    static const char *names[] = {
        "white-hot", "black-hot", "fusion-1", "rainbow", "fusion-2",
        "iron-red-1", "iron-red-2", "deep-brown", "color-1", "color-2",
        "ice-fire", "rain", "green-hot", "red-hot", "deep-blue"
    };

    return mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "invalid";
}

static app_status_t mavlink_control_dji_status(int status) {
    if (status == DJI_RSDK_OK) {
        return APP_OK;
    }
    if (status == DJI_RSDK_ERR_PARAM || status == DJI_RSDK_ERR_RANGE) {
        return APP_ERR_PARAM;
    }
    if (status == DJI_RSDK_ERR_STATE) {
        return APP_ERR_BUSY;
    }
    return APP_ERR_IO;
}

static app_status_t mavlink_control_visca_status(visca_status_t status) {
    if (status == VISCA_OK) {
        return APP_OK;
    }
    if (status == VISCA_ERR_PARAM) {
        return APP_ERR_PARAM;
    }
    if (status == VISCA_ERR_UART || status == VISCA_ERR_TIMEOUT) {
        return APP_ERR_IO;
    }
    return APP_ERR_BUSY;
}

static app_status_t mavlink_control_laser_status(
    sfl0603_laser_status_t status) {
    if (status == SFL0603_LASER_OK) {
        return APP_OK;
    }
    if (status == SFL0603_LASER_ERR_PARAM) {
        return APP_ERR_PARAM;
    }
    if (status == SFL0603_LASER_ERR_STATE) {
        return APP_ERR_BUSY;
    }
    return APP_ERR_IO;
}

static app_status_t mavlink_control_modbus_status(modbus_status_t status) {
    if (status == MODBUS_OK) {
        return APP_OK;
    }
    if (status == MODBUS_ERR_PARAM) {
        return APP_ERR_PARAM;
    }
    return APP_ERR_IO;
}

app_status_t mavlink_control_actions_init(
    mavlink_control_actions_t **out_actions,
    vp_ctx_t *pipeline,
    vp_control_t *video_control) {
    mavlink_control_actions_t *actions;

    if (out_actions == NULL || pipeline == NULL || video_control == NULL) {
        return APP_ERR_PARAM;
    }
    *out_actions = NULL;
    actions = (mavlink_control_actions_t *)calloc(1, sizeof(*actions));
    if (actions == NULL) {
        return APP_ERR_NOMEM;
    }
    actions->pipeline = pipeline;
    actions->video_control = video_control;
    *out_actions = actions;
    LOGI("MAVLink action dispatcher initialized");
    return APP_OK;
}

void mavlink_control_actions_deinit(mavlink_control_actions_t *actions) {
    free(actions);
}

app_status_t mavlink_control_actions_handle(
    const mavlink_control_key_event_t *event,
    void *user_data,
    uint64_t *out_request_id) {
    mavlink_control_actions_t *actions =
        (mavlink_control_actions_t *)user_data;
    app_status_t status = APP_ERR_UNSUPPORTED;
    uint64_t request_id = 0U;
    const char *channel_name = NULL;

    if (event == NULL || actions == NULL || actions->pipeline == NULL ||
        actions->video_control == NULL) {
        return APP_ERR_PARAM;
    }

    LOGD("MAVLink action key=%u(%s) event=%s value=%u sequence=%u uid=%016llx",
         (unsigned int)event->key,
         mavlink_control_key_name(event->key),
         mavlink_control_event_name(event->event),
         (unsigned int)event->value,
         event->sequence,
         (unsigned long long)event->command_uid);

    switch (event->key) {
    case MAVLINK_CONTROL_KEY_VIDEO_SWITCH: {
        int channel_count = vp_channel_count(actions->pipeline);
        vp_channel_id_t current = vp_stream_current(actions->pipeline);
        vp_channel_id_t next;

        if (channel_count <= 0 || current < 0 || current >= channel_count) {
            status = APP_ERR_PARAM;
            break;
        }
        next = (current + 1) % channel_count;
        channel_name = vp_channel_name(actions->pipeline, next);
        status = vp_control_stream_select_async(
            actions->video_control, channel_name, &request_id);
        break;
    }

    case MAVLINK_CONTROL_KEY_SNAPSHOT: {
        int channel_count = vp_channel_count(actions->pipeline);
        vp_channel_id_t current = vp_stream_current(actions->pipeline);

        if (channel_count <= 0 || current < 0 || current >= channel_count) {
            status = APP_ERR_PARAM;
            break;
        }
        channel_name = vp_channel_name(actions->pipeline, current);
        status = vp_control_snapshot_async(
            actions->video_control, channel_name, NULL, &request_id);
        break;
    }

    case MAVLINK_CONTROL_KEY_RECORD_TOGGLE:
        status = mavlink_control_any_recording(actions->pipeline)
                     ? vp_control_record_stop_all_async(
                           actions->video_control, &request_id)
                     : vp_control_record_start_all_async(
                           actions->video_control, NULL, &request_id);
        break;

    case MAVLINK_CONTROL_KEY_ZOOM_IN:
    case MAVLINK_CONTROL_KEY_ZOOM_OUT: {
        visca_4k_camera_action_t action =
            event->key == MAVLINK_CONTROL_KEY_ZOOM_IN
                ? VISCA_4K_CAMERA_ACTION_ZOOM_TELE
                : VISCA_4K_CAMERA_ACTION_ZOOM_WIDE;

        status = mavlink_control_visca_status(
            visca_4k_camera_action_request(action,
                                           (uint8_t)event->event,
                                           event->value));
        break;
    }

    case MAVLINK_CONTROL_KEY_FOCUS_IN:
    case MAVLINK_CONTROL_KEY_FOCUS_OUT:
    {
        int direction = event->key == MAVLINK_CONTROL_KEY_FOCUS_IN
                            ? MOTOR_DIRECTION_FORWARD
                            : MOTOR_DIRECTION_REVERSE;

        if (event->event == MAVLINK_CONTROL_EVENT_RELEASE) {
            status = mavlink_control_modbus_status(motor_service_request_stop());
        } else {
            status = mavlink_control_modbus_status(
                motor_service_request_move(direction, event->value));
        }
        break;
    }

    case MAVLINK_CONTROL_KEY_TRIGGER:
        if (event->event == MAVLINK_CONTROL_EVENT_RELEASE) {
            status = APP_OK;
            break;
        }
        LOGI("MAVLink trigger requested");
        status = mavlink_control_modbus_status(motor_service_request_trigger());
        break;

    case MAVLINK_CONTROL_KEY_UNLOCK:
        if (event->event == MAVLINK_CONTROL_EVENT_RELEASE) {
            /* Unlock is a one-shot command; a release must not send again. */
            status = APP_OK;
            break;
        }
        status = high_speed_camera_service_request_unlock() ==
                         HIGH_SPEED_CAMERA_OK
                     ? APP_OK
                     : APP_ERR_IO;
        break;

    case MAVLINK_CONTROL_KEY_THERMAL_PSEUDOCOLOR:
        if (event->value > 14U) {
            status = APP_ERR_PARAM;
            break;
        }
        if (event->event == MAVLINK_CONTROL_EVENT_RELEASE) {
            status = APP_OK;
            break;
        }
        LOGI("MAVLink thermal pseudocolor mode=%u(%s)",
             (unsigned int)event->value,
             mavlink_control_pseudocolor_name(event->value));
        {
            int thermal_status =
                thermal_camera_service_request_pseudocolor(event->value);
            if (thermal_status == THERMAL_CAMERA_OK) {
                status = APP_OK;
            } else if (thermal_status == THERMAL_CAMERA_ERR_BUSY) {
                status = APP_ERR_BUSY;
            } else if (thermal_status == THERMAL_CAMERA_ERR_PARAM ||
                       thermal_status == THERMAL_CAMERA_ERR_RANGE) {
                status = APP_ERR_PARAM;
            } else {
                status = APP_ERR_IO;
            }
        }
        break;

    case MAVLINK_CONTROL_KEY_LASER_SINGLE_MEASURE:
        if (event->event == MAVLINK_CONTROL_EVENT_RELEASE) {
            status = APP_OK;
            break;
        }
        LOGI("MAVLink laser single measurement requested");
        status = mavlink_control_laser_status(
            sfl0603_laser_service_request_single_measure());
        break;

    case MAVLINK_CONTROL_KEY_LASER_CONTINUOUS_MEASURE:
        if (event->value > 1U) {
            status = APP_ERR_PARAM;
            break;
        }
        if (event->event == MAVLINK_CONTROL_EVENT_RELEASE) {
            status = APP_OK;
            break;
        }
        LOGI("MAVLink laser continuous measurement %s (period_ms=1000)",
             event->value != 0U ? "start" : "stop");
        status = mavlink_control_laser_status(
            sfl0603_laser_service_request_continuous(1000U,
                                                  event->value != 0U));
        if (status == APP_OK) {
            (void)vp_control_osd_set_laser_continuous(
                actions->video_control,
                event->value != 0U);
        }
        break;

    case MAVLINK_CONTROL_KEY_OSD_ENABLED:
        if (event->value > 1U) {
            status = APP_ERR_PARAM;
            break;
        }
        status = vp_control_osd_set_enabled_async(
            actions->video_control,
            event->value != 0U,
            &request_id);
        break;

    case MAVLINK_CONTROL_KEY_GIMBAL_UP:
    case MAVLINK_CONTROL_KEY_GIMBAL_DOWN:
    case MAVLINK_CONTROL_KEY_GIMBAL_LEFT:
    case MAVLINK_CONTROL_KEY_GIMBAL_RIGHT: {
        dji_rsdk_gimbal_direction_t direction;

        switch (event->key) {
        case MAVLINK_CONTROL_KEY_GIMBAL_UP:
            direction = DJI_RSDK_GIMBAL_DIRECTION_UP;
            break;
        case MAVLINK_CONTROL_KEY_GIMBAL_DOWN:
            direction = DJI_RSDK_GIMBAL_DIRECTION_DOWN;
            break;
        case MAVLINK_CONTROL_KEY_GIMBAL_LEFT:
            direction = DJI_RSDK_GIMBAL_DIRECTION_LEFT;
            break;
        default:
            direction = DJI_RSDK_GIMBAL_DIRECTION_RIGHT;
            break;
        }
        status = mavlink_control_dji_status(
            dji_rsdk_service_set_gimbal_direction(
                direction,
                event->event == MAVLINK_CONTROL_EVENT_PRESS,
                event->value));
        break;
    }

    case MAVLINK_CONTROL_KEY_GIMBAL_CENTER:
        status = mavlink_control_dji_status(
            dji_rsdk_service_center_gimbal());
        break;

    default:
        status = APP_ERR_PARAM;
        break;
    }

    if (out_request_id != NULL) {
        *out_request_id = request_id;
    }
    LOGD("MAVLink action result key=%u(%s) event=%s value=%u channel=%s "
         "request=%llu status=%s",
         (unsigned int)event->key,
         mavlink_control_key_name(event->key),
         mavlink_control_event_name(event->event),
         (unsigned int)event->value,
         channel_name != NULL ? channel_name : "-",
         (unsigned long long)request_id,
         app_status_str(status));
    return status;
}
