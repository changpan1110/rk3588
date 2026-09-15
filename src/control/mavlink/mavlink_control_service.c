#define _DEFAULT_SOURCE
#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "mavlink_control_service.c"

#include "control/mavlink/mavlink_control_service.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/debug.h"
#include "control/mavlink/mavlink_web_control.h"
#include "control/mavlink/mavlink_transport.h"

#define MAVLINK_CONTROL_DEFAULT_CONFIG \
    "/home/cat/rk3588/config/mavlink_control.conf"

struct mavlink_control_service {
    mavlink_control_config_t config;
    mavlink_web_control_t protocol;
    mavlink_message_t message;
    mavlink_status_t parser_status;
    mavlink_control_key_callback_t key_callback;
    void *user_data;
    pthread_t thread;
    pthread_mutex_t lock;
    int stop_requested;
    int thread_started;
};

_Static_assert(
    MAVLINK_CONTROL_KEY_VIDEO_SWITCH == MAVLINK_WEB_ACTION_VIDEO_SWITCH,
    "MAVLink video switch key mismatch");
_Static_assert(
    MAVLINK_CONTROL_KEY_GIMBAL_CENTER == MAVLINK_WEB_ACTION_GIMBAL_CENTER,
    "MAVLink last key mismatch");

const char *mavlink_control_key_name(mavlink_control_key_t key) {
    switch (key) {
    case MAVLINK_CONTROL_KEY_VIDEO_SWITCH:
        return "video.switch";
    case MAVLINK_CONTROL_KEY_SNAPSHOT:
        return "camera.snapshot";
    case MAVLINK_CONTROL_KEY_RECORD_TOGGLE:
        return "record.toggle";
    case MAVLINK_CONTROL_KEY_ZOOM_IN:
        return "camera.zoom_in";
    case MAVLINK_CONTROL_KEY_ZOOM_OUT:
        return "camera.zoom_out";
    case MAVLINK_CONTROL_KEY_TRIGGER:
        return "device.trigger";
    case MAVLINK_CONTROL_KEY_FOCUS_IN:
        return "camera.focus_in";
    case MAVLINK_CONTROL_KEY_FOCUS_OUT:
        return "camera.focus_out";
    case MAVLINK_CONTROL_KEY_UNLOCK:
        return "device.unlock";
    case MAVLINK_CONTROL_KEY_THERMAL_PSEUDOCOLOR:
        return "thermal.pseudocolor";
    case MAVLINK_CONTROL_KEY_LASER_SINGLE_MEASURE:
        return "laser.single_measure";
    case MAVLINK_CONTROL_KEY_LASER_CONTINUOUS_MEASURE:
        return "laser.continuous_measure";
    case MAVLINK_CONTROL_KEY_OSD_ENABLED:
        return "osd.enabled";
    case MAVLINK_CONTROL_KEY_GIMBAL_UP:
        return "gimbal.up";
    case MAVLINK_CONTROL_KEY_GIMBAL_DOWN:
        return "gimbal.down";
    case MAVLINK_CONTROL_KEY_GIMBAL_LEFT:
        return "gimbal.left";
    case MAVLINK_CONTROL_KEY_GIMBAL_RIGHT:
        return "gimbal.right";
    case MAVLINK_CONTROL_KEY_GIMBAL_CENTER:
        return "gimbal.center";
    default:
        return "unknown";
    }
}

const char *mavlink_control_event_name(mavlink_control_event_type_t event) {
    switch (event) {
    case MAVLINK_CONTROL_EVENT_CLICK:
        return "click";
    case MAVLINK_CONTROL_EVENT_PRESS:
        return "press";
    case MAVLINK_CONTROL_EVENT_RELEASE:
        return "release";
    default:
        return "unknown";
    }
}

static void mavlink_control_log_key_map(void) {
    LOGD("MAVLink key map: 1=video.switch 2=camera.snapshot "
         "3=record.toggle 4=camera.zoom_in 5=camera.zoom_out "
         "6=device.trigger 7=camera.focus_in 8=camera.focus_out "
         "9=device.unlock 10=thermal.pseudocolor "
         "11=laser.single_measure 12=laser.continuous_measure "
         "13=osd.enabled 14=gimbal.up 15=gimbal.down "
         "16=gimbal.left 17=gimbal.right 18=gimbal.center");
}

static int mavlink_control_should_stop(void *opaque) {
    mavlink_control_service_t *service =
        (mavlink_control_service_t *)opaque;
    int stop_requested;

    pthread_mutex_lock(&service->lock);
    stop_requested = service->stop_requested;
    pthread_mutex_unlock(&service->lock);
    return stop_requested;
}

static uint8_t mavlink_control_status_to_result(app_status_t status) {
    switch (status) {
    case APP_OK:
        return MAV_RESULT_ACCEPTED;
    case APP_ERR_BUSY:
        return MAV_RESULT_TEMPORARILY_REJECTED;
    case APP_ERR_UNSUPPORTED:
        return MAV_RESULT_UNSUPPORTED;
    case APP_ERR_PARAM:
        return MAV_RESULT_DENIED;
    default:
        return MAV_RESULT_FAILED;
    }
}

static uint8_t mavlink_control_notify_key(
    const mavlink_web_command_t *command,
    void *user_data) {
    mavlink_control_service_t *service =
        (mavlink_control_service_t *)user_data;
    mavlink_control_key_event_t event;
    app_status_t status;
    uint64_t request_id = 0U;

    memset(&event, 0, sizeof(event));
    event.source_system = command->source_system;
    event.source_component = command->source_component;
    event.key = (mavlink_control_key_t)command->action;
    event.event = (mavlink_control_event_type_t)command->event;
    event.value = command->value;
    event.sequence = command->sequence;
    event.command_uid = command->command_uid;

    if (service->config.debug) {
        LOGD("MAVLink key callback notify source=%u/%u key=%u(%s) "
             "event=%s value=%u sequence=%u uid=%016llx",
             event.source_system,
             event.source_component,
             (unsigned int)event.key,
             mavlink_control_key_name(event.key),
             mavlink_control_event_name(event.event),
             (unsigned int)event.value,
             event.sequence,
             (unsigned long long)event.command_uid);
    }

    status = service->key_callback(
        &event,
        service->user_data,
        &request_id);
    LOGD("MAVLink key callback result key=%u(%s) event=%s value=%u request=%llu "
         "status=%s mav_result=%u",
         (unsigned int)event.key,
         mavlink_control_key_name(event.key),
         mavlink_control_event_name(event.event),
         (unsigned int)event.value,
         (unsigned long long)request_id,
         app_status_str(status),
         (unsigned int)mavlink_control_status_to_result(status));
    return mavlink_control_status_to_result(status);
}

static void mavlink_control_log_raw(
    const mavlink_control_service_t *service,
    const mavlink_message_t *message) {
    mavlink_command_long_t payload;

    if (!service->config.debug ||
        message->msgid != MAVLINK_MSG_ID_COMMAND_LONG) {
        return;
    }
    mavlink_msg_command_long_decode(message, &payload);
    if (payload.command != MAV_CMD_USER_1) {
        return;
    }
    LOGD("MAVLink key raw packet_seq=%u source=%u/%u target=%u/%u command=%u "
         "confirmation=%u p1=%.0f p2=%.0f p3=%.0f p4=%.0f "
         "p5=%.0f p6=%.0f p7=%.0f",
         message->seq,
         message->sysid,
         message->compid,
         payload.target_system,
         payload.target_component,
         payload.command,
         payload.confirmation,
         payload.param1,
         payload.param2,
         payload.param3,
         payload.param4,
         payload.param5,
         payload.param6,
         payload.param7);
}

static int mavlink_control_process_bytes(
    void *opaque,
    const uint8_t *data,
    size_t size,
    mavlink_transport_send_fn send_fn,
    void *send_opaque) {
    mavlink_control_service_t *service =
        (mavlink_control_service_t *)opaque;
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (mavlink_parse_char(
                MAVLINK_COMM_0,
                data[index],
                &service->message,
                &service->parser_status)) {
            mavlink_message_t ack;
            mavlink_web_handle_info_t info;

            mavlink_control_log_raw(service, &service->message);
            if (mavlink_web_control_handle_ex(
                    &service->protocol,
                    &service->message,
                    service->config.system_id,
                    service->config.component_id,
                    mavlink_control_notify_key,
                    service,
                    &ack,
                    &info)) {
                uint8_t ack_buffer[MAVLINK_MAX_PACKET_LEN];
                uint16_t ack_length =
                    mavlink_msg_to_send_buffer(ack_buffer, &ack);

                if (service->config.debug) {
                    LOGD("MAVLink key handled key=%u(%s) event=%s value=%u sequence=%u "
                         "uid=%016llx duplicate=%u result=%u",
                         (unsigned int)info.command.action,
                             mavlink_control_key_name(
                                 (mavlink_control_key_t)info.command.action),
                         mavlink_control_event_name(
                             (mavlink_control_event_type_t)info.command.event),
                         (unsigned int)info.command.value,
                         info.command.sequence,
                         (unsigned long long)info.command.command_uid,
                         (unsigned int)info.duplicate,
                         (unsigned int)info.result);
                }
                if (send_fn(send_opaque, ack_buffer, ack_length) != 0) {
                    LOGW("MAVLink COMMAND_ACK send failed: %s", strerror(errno));
                    return -1;
                }
                if (service->config.debug) {
                    LOGD("MAVLink COMMAND_ACK sent command=%u result=%u "
                         "target=%u/%u bytes=%u",
                         MAV_CMD_USER_1,
                         (unsigned int)info.result,
                         info.command.source_system,
                         info.command.source_component,
                         ack_length);
                }
            }
        }
    }
    return 0;
}

static void mavlink_control_wait_reconnect(
    mavlink_control_service_t *service) {
    int elapsed = 0;

    while (elapsed < MAVLINK_CONTROL_RECONNECT_MS &&
           !mavlink_control_should_stop(service)) {
        usleep(100U * 1000U);
        elapsed += 100;
    }
}

static void *mavlink_control_thread(void *opaque) {
    mavlink_control_service_t *service =
        (mavlink_control_service_t *)opaque;
    mavlink_transport_runtime_t runtime = {
        .should_stop = mavlink_control_should_stop,
        .on_data = mavlink_control_process_bytes,
        .opaque = service
    };

    while (!mavlink_control_should_stop(service)) {
        memset(&service->message, 0, sizeof(service->message));
        memset(&service->parser_status, 0, sizeof(service->parser_status));
        (void)mavlink_transport_run(&service->config, &runtime);
        if (!mavlink_control_should_stop(service)) {
            mavlink_control_wait_reconnect(service);
        }
    }
    return NULL;
}

app_status_t mavlink_control_service_init(
    mavlink_control_service_t **out_service,
    const char *config_path,
    mavlink_control_key_callback_t key_callback,
    void *user_data) {
    mavlink_control_service_t *service;
    const char *effective_path = config_path != NULL && config_path[0] != '\0'
                                     ? config_path
                                     : MAVLINK_CONTROL_DEFAULT_CONFIG;

    if (out_service == NULL || key_callback == NULL) {
        return APP_ERR_PARAM;
    }
    *out_service = NULL;
    service = (mavlink_control_service_t *)calloc(1, sizeof(*service));
    if (service == NULL) {
        return APP_ERR_NOMEM;
    }
    service->key_callback = key_callback;
    service->user_data = user_data;
    if (mavlink_control_config_load(&service->config, effective_path) != 0) {
        free(service);
        return APP_ERR_PARAM;
    }
    if (pthread_mutex_init(&service->lock, NULL) != 0) {
        free(service);
        return APP_ERR_IO;
    }
    mavlink_web_control_init(&service->protocol);
    LOGI("MAVLink control init config=%s enabled=%d debug=%d transport=%s "
         "system=%u component=%u callback=registered",
         effective_path,
         service->config.enabled,
         service->config.debug,
         mavlink_control_transport_name(service->config.transport),
         service->config.system_id,
         service->config.component_id);
    if (service->config.debug) {
        mavlink_control_log_key_map();
    }
    if (service->config.enabled) {
        if (pthread_create(
                &service->thread,
                NULL,
                mavlink_control_thread,
                service) != 0) {
            pthread_mutex_destroy(&service->lock);
            free(service);
            return APP_ERR_IO;
        }
        service->thread_started = 1;
    }
    *out_service = service;
    return APP_OK;
}

void mavlink_control_service_deinit(mavlink_control_service_t *service) {
    if (service == NULL) {
        return;
    }
    pthread_mutex_lock(&service->lock);
    service->stop_requested = 1;
    pthread_mutex_unlock(&service->lock);

    if (service->thread_started) {
        pthread_join(service->thread, NULL);
        service->thread_started = 0;
    }
    pthread_mutex_destroy(&service->lock);
    free(service);
}
