#define LOG_FILE_NAME "sbus_rc8_command.c"

#include "app/sbus_rc8_command.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/debug.h"

#define SBUS_RC8_CYCLE_CHANNEL 5
#define SBUS_RC8_RECORD_CHANNEL 6
#define SBUS_RC8_SNAPSHOT_CHANNEL 7
#define SBUS_RC8_COMMAND_IDLE_US 1000

struct sbus_rc8_command {
    sbus_rc8_t *sbus_rc8;
    vp_ctx_t *pipeline;
    vp_control_t *control;
    pthread_t thread;
    pthread_mutex_t lock;
    char cycle_channels[3][APP_NAME_MAX_LEN];
    int cycle_position;
    int ch5_initialized;
    int ch5_last_pressed;
    int ch6_initialized;
    int ch6_last_pressed;
    int ch7_initialized;
    int ch7_last_pressed;
    int stop_requested;
    int thread_initialized;
};

static int sbus_rc8_command_should_stop(sbus_rc8_command_t *rc_control) {
    int stop_requested;

    pthread_mutex_lock(&rc_control->lock);
    stop_requested = rc_control->stop_requested;
    pthread_mutex_unlock(&rc_control->lock);
    return stop_requested;
}

static int sbus_rc8_command_any_recording(vp_ctx_t *pipeline) {
    int channel;

    for (channel = 0; channel < vp_channel_count(pipeline); ++channel) {
        if (vp_is_recording(pipeline, (vp_channel_id_t)channel)) {
            return 1;
        }
    }
    return 0;
}

static void sbus_rc8_command_log_submit(const char *action,
                                        app_status_t status,
                                        uint64_t request_id) {
    if (status == APP_OK) {
        LOGI("SBUS RC8 %s queued request=%llu",
             action,
             (unsigned long long)request_id);
    } else {
        LOGW("SBUS RC8 %s queue failed: %s",
             action,
             app_status_str(status));
    }
}

static void sbus_rc8_command_handle_cycle_button(
    sbus_rc8_command_t *rc_control,
    uint16_t channel_value) {
    app_status_t status;
    uint64_t request_id = 0;
    int pressed = channel_value >= SBUS_CHANNEL_CENTER;

    if (!rc_control->ch5_initialized) {
        rc_control->ch5_initialized = 1;
        rc_control->ch5_last_pressed = pressed;
        return;
    }
    if (pressed && !rc_control->ch5_last_pressed) {
        rc_control->cycle_position = (rc_control->cycle_position + 1) % 3;
        status = vp_control_stream_select_async(
            rc_control->control,
            rc_control->cycle_channels[rc_control->cycle_position],
            &request_id);
        sbus_rc8_command_log_submit("stream cycle", status, request_id);
        if (status != APP_OK) {
            rc_control->cycle_position =
                (rc_control->cycle_position + 2) % 3;
        }
    }
    rc_control->ch5_last_pressed = pressed;
}

static void sbus_rc8_command_handle_record_button(
    sbus_rc8_command_t *rc_control,
    uint16_t channel_value) {
    app_status_t status;
    uint64_t request_id = 0;
    int pressed = channel_value >= SBUS_CHANNEL_CENTER;

    if (!rc_control->ch6_initialized) {
        rc_control->ch6_initialized = 1;
        rc_control->ch6_last_pressed = pressed;
        return;
    }
    if (pressed && !rc_control->ch6_last_pressed) {
        if (sbus_rc8_command_any_recording(rc_control->pipeline)) {
            status = vp_control_record_stop_all_async(rc_control->control,
                                                      &request_id);
            sbus_rc8_command_log_submit("record stop all",
                                        status,
                                        request_id);
        } else {
            status = vp_control_record_start_all_async(rc_control->control,
                                                       NULL,
                                                       &request_id);
            sbus_rc8_command_log_submit("record start all",
                                        status,
                                        request_id);
        }
    }
    rc_control->ch6_last_pressed = pressed;
}

static void sbus_rc8_command_handle_snapshot_button(
    sbus_rc8_command_t *rc_control,
    uint16_t channel_value) {
    const char *selected_channel;
    app_status_t status;
    uint64_t request_id = 0;
    int pressed = channel_value >= SBUS_CHANNEL_CENTER;

    if (!rc_control->ch7_initialized) {
        rc_control->ch7_initialized = 1;
        rc_control->ch7_last_pressed = pressed;
        return;
    }
    if (pressed && !rc_control->ch7_last_pressed) {
        selected_channel = vp_channel_name(
            rc_control->pipeline,
            vp_stream_current(rc_control->pipeline));
        status = vp_control_snapshot_async(
            rc_control->control,
            selected_channel,
            NULL,
            &request_id);
        sbus_rc8_command_log_submit("snapshot", status, request_id);
    }
    rc_control->ch7_last_pressed = pressed;
}

static int sbus_rc8_command_read_channel(
    sbus_rc8_command_t *rc_control,
    unsigned int channel_number,
    uint16_t *channel_value) {
    sbus_status_t status;

    status = sbus_rc8_channel_receive(rc_control->sbus_rc8,
                                      channel_number,
                                      channel_value,
                                      0);
    if (status == SBUS_OK) {
        return 1;
    }
    if (status != SBUS_NO_FRAME) {
        LOGW("SBUS RC8 CH%u receive failed: %s",
             channel_number,
             sbus_status_string(status));
    }
    return 0;
}

static void *sbus_rc8_command_thread(void *opaque) {
    sbus_rc8_command_t *rc_control = (sbus_rc8_command_t *)opaque;
    uint16_t channel_value;

    while (!sbus_rc8_command_should_stop(rc_control)) {
        int received = 0;

        while (sbus_rc8_command_read_channel(rc_control,
                                             SBUS_RC8_CYCLE_CHANNEL,
                                             &channel_value)) {
            sbus_rc8_command_handle_cycle_button(rc_control,
                                                  channel_value);
            received = 1;
        }
        while (sbus_rc8_command_read_channel(rc_control,
                                             SBUS_RC8_RECORD_CHANNEL,
                                             &channel_value)) {
            sbus_rc8_command_handle_record_button(rc_control,
                                                   channel_value);
            received = 1;
        }
        while (sbus_rc8_command_read_channel(rc_control,
                                             SBUS_RC8_SNAPSHOT_CHANNEL,
                                             &channel_value)) {
            sbus_rc8_command_handle_snapshot_button(rc_control,
                                                     channel_value);
            received = 1;
        }
        if (!received) {
            usleep(SBUS_RC8_COMMAND_IDLE_US);
        }
    }
    return NULL;
}

app_status_t sbus_rc8_command_init(
    sbus_rc8_command_t **out_command,
    sbus_rc8_t *sbus_rc8,
    vp_ctx_t *pipeline,
    vp_control_t *control,
    const char *const cycle_channels[3]) {
    sbus_rc8_command_t *rc_control;
    int channel;

    if (out_command == NULL) {
        return APP_ERR_PARAM;
    }
    *out_command = NULL;
    if (sbus_rc8 == NULL || pipeline == NULL || control == NULL ||
        cycle_channels == NULL) {
        return APP_ERR_PARAM;
    }
    for (channel = 0; channel < 3; ++channel) {
        if (cycle_channels[channel] == NULL ||
            cycle_channels[channel][0] == '\0' ||
            strlen(cycle_channels[channel]) >= APP_NAME_MAX_LEN) {
            return APP_ERR_PARAM;
        }
    }

    rc_control = (sbus_rc8_command_t *)calloc(1, sizeof(*rc_control));
    if (rc_control == NULL) {
        return APP_ERR_NOMEM;
    }
    rc_control->sbus_rc8 = sbus_rc8;
    rc_control->pipeline = pipeline;
    rc_control->control = control;
    rc_control->cycle_position = 0;
    for (channel = 0; channel < 3; ++channel) {
        snprintf(rc_control->cycle_channels[channel],
                 sizeof(rc_control->cycle_channels[channel]),
                 "%s",
                 cycle_channels[channel]);
    }

    if (pthread_mutex_init(&rc_control->lock, NULL) != 0) {
        free(rc_control);
        return APP_ERR_IO;
    }
    if (pthread_create(&rc_control->thread,
                       NULL,
                       sbus_rc8_command_thread,
                       rc_control) != 0) {
        pthread_mutex_destroy(&rc_control->lock);
        free(rc_control);
        return APP_ERR_IO;
    }
    rc_control->thread_initialized = 1;
    *out_command = rc_control;
    return APP_OK;
}

void sbus_rc8_command_deinit(sbus_rc8_command_t *rc_control) {
    if (rc_control == NULL) {
        return;
    }
    if (rc_control->thread_initialized) {
        pthread_mutex_lock(&rc_control->lock);
        rc_control->stop_requested = 1;
        pthread_mutex_unlock(&rc_control->lock);
        pthread_join(rc_control->thread, NULL);
        rc_control->thread_initialized = 0;
    }
    pthread_mutex_destroy(&rc_control->lock);
    free(rc_control);
}
