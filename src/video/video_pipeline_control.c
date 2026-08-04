#define LOG_FILE_NAME "video_pipeline_control.c"

#include "video/video_pipeline_control.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VP_CONTROL_QUEUE_CAPACITY 32

typedef enum {
    VP_CONTROL_REQUEST_RECORD_START = 0,
    VP_CONTROL_REQUEST_RECORD_START_ALL,
    VP_CONTROL_REQUEST_RECORD_STOP,
    VP_CONTROL_REQUEST_RECORD_STOP_ALL,
    VP_CONTROL_REQUEST_SNAPSHOT,
    VP_CONTROL_REQUEST_STREAM_SELECT,
    VP_CONTROL_REQUEST_STREAM_ENABLE,
    VP_CONTROL_REQUEST_OSD_ENABLE,
    VP_CONTROL_REQUEST_OSD_POSITION
} vp_control_request_type_t;

typedef struct {
    vp_control_request_type_t type;
    uint64_t request_id;
    char channel[APP_NAME_MAX_LEN];
    char path[APP_PATH_MAX_LEN];
    int enabled;
    int x;
    int y;
} vp_control_request_t;

struct vp_control {
    vp_ctx_t *pipeline;
    pthread_mutex_t lock;
    vp_osd_params_t osd;

    pthread_mutex_t queue_lock;
    pthread_cond_t queue_cond;
    pthread_t worker_thread;
    int worker_started;
    int stop_requested;
    vp_control_request_t queue[VP_CONTROL_QUEUE_CAPACITY];
    int queue_head;
    int queue_count;
    uint64_t next_request_id;
    vp_control_event_callback_t event_callback;
    void *event_opaque;
};

static app_status_t vp_control_find_channel(vp_control_t *control,
                                            const char *channel,
                                            vp_channel_id_t *out_channel) {
    if (control == NULL || control->pipeline == NULL ||
        channel == NULL || channel[0] == '\0' || out_channel == NULL) {
        return APP_ERR_PARAM;
    }
    if (vp_channel_find(control->pipeline, channel, out_channel) != 0) {
        return APP_ERR_PARAM;
    }
    return APP_OK;
}

static app_status_t vp_control_copy_arg(char *dst,
                                        size_t dst_size,
                                        const char *src) {
    size_t length;

    if (dst == NULL || dst_size == 0) {
        return APP_ERR_PARAM;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return APP_OK;
    }
    length = strlen(src);
    if (length >= dst_size) {
        return APP_ERR_PARAM;
    }
    memcpy(dst, src, length + 1);
    return APP_OK;
}

static void vp_control_emit(vp_control_t *control,
                            const vp_control_event_t *event) {
    vp_control_event_callback_t callback;
    void *opaque;

    pthread_mutex_lock(&control->queue_lock);
    callback = control->event_callback;
    opaque = control->event_opaque;
    pthread_mutex_unlock(&control->queue_lock);
    if (callback != NULL) {
        callback(event, opaque);
    }
}

static void vp_control_event_init(vp_control_event_t *event,
                                  vp_control_event_type_t type,
                                  const vp_control_request_t *request) {
    memset(event, 0, sizeof(*event));
    event->type = type;
    event->request_id = request->request_id;
    snprintf(event->channel, sizeof(event->channel), "%s", request->channel);
    snprintf(event->path, sizeof(event->path), "%s", request->path);
    event->enabled = request->enabled;
    event->x = request->x;
    event->y = request->y;
}

static void vp_control_event_set_recording(vp_control_t *control,
                                           vp_control_event_t *event) {
    vp_channel_id_t id;

    if (event->channel[0] != '\0' &&
        vp_channel_find(control->pipeline, event->channel, &id) == 0) {
        event->recording = vp_is_recording(control->pipeline, id);
    }
}

static app_status_t vp_control_snapshot_ex(vp_control_t *control,
                                           const char *channel,
                                           const char *jpg_path,
                                           char *out_path,
                                           size_t out_size) {
    vp_channel_id_t id;
    app_status_t status;

    pthread_mutex_lock(&control->lock);
    status = vp_control_find_channel(control, channel, &id);
    if (status == APP_OK) {
        status = vp_snapshot_ex(control->pipeline,
                                id,
                                jpg_path,
                                out_path,
                                out_size);
    }
    pthread_mutex_unlock(&control->lock);
    return status;
}

static void vp_control_execute_request(vp_control_t *control,
                                       const vp_control_request_t *request) {
    vp_control_event_t event;
    app_status_t status;

    switch (request->type) {
        case VP_CONTROL_REQUEST_RECORD_START: {
            vp_channel_id_t id;

            vp_control_event_init(&event,
                                  VP_CONTROL_EVENT_RECORD_START_COMPLETE,
                                  request);
            status = vp_control_record_start(control,
                                             request->channel,
                                             request->path[0] != '\0'
                                                 ? request->path
                                                 : NULL);
            event.status = status;
            if (status == APP_OK &&
                vp_channel_find(control->pipeline, request->channel, &id) == 0) {
                vp_record_get_path(control->pipeline,
                                   id,
                                   event.path,
                                   sizeof(event.path));
            }
            vp_control_event_set_recording(control, &event);
            vp_control_emit(control, &event);
            break;
        }
        case VP_CONTROL_REQUEST_RECORD_START_ALL: {
            int channel_count = vp_channel_count(control->pipeline);
            int *was_recording;
            int i;

            vp_control_event_init(&event,
                                  VP_CONTROL_EVENT_RECORD_START_ALL_COMPLETE,
                                  request);
            was_recording = (int *)calloc((size_t)channel_count,
                                          sizeof(*was_recording));
            if (was_recording != NULL) {
                for (i = 0; i < channel_count; ++i) {
                    was_recording[i] = vp_is_recording(control->pipeline,
                                                       (vp_channel_id_t)i);
                }
            }
            status = vp_control_record_start_all(control,
                                                 request->path[0] != '\0'
                                                     ? request->path
                                                     : NULL);
            event.status = status;
            for (i = 0; i < channel_count; ++i) {
                int is_recording = vp_is_recording(control->pipeline,
                                                   (vp_channel_id_t)i);

                if (is_recording &&
                    (was_recording == NULL || !was_recording[i])) {
                    vp_control_event_t channel_event;

                    event.completed_count++;
                    vp_control_event_init(&channel_event,
                                          VP_CONTROL_EVENT_RECORD_START_COMPLETE,
                                          request);
                    channel_event.status = APP_OK;
                    snprintf(channel_event.channel,
                             sizeof(channel_event.channel),
                             "%s",
                             vp_channel_name(control->pipeline,
                                             (vp_channel_id_t)i));
                    vp_record_get_path(control->pipeline,
                                       (vp_channel_id_t)i,
                                       channel_event.path,
                                       sizeof(channel_event.path));
                    channel_event.recording = 1;
                    vp_control_emit(control, &channel_event);
                }
            }
            free(was_recording);
            vp_control_emit(control, &event);
            break;
        }
        case VP_CONTROL_REQUEST_RECORD_STOP: {
            vp_control_event_init(&event,
                                  VP_CONTROL_EVENT_RECORD_STOP_COMPLETE,
                                  request);
            status = vp_control_record_stop_ex(control,
                                               request->channel,
                                               &event.record_stats);
            event.status = status;
            if (status == APP_OK) {
                snprintf(event.path,
                         sizeof(event.path),
                         "%s",
                         event.record_stats.path);
                event.completed_count = 1;
            }
            vp_control_event_set_recording(control, &event);
            vp_control_emit(control, &event);
            break;
        }
        case VP_CONTROL_REQUEST_RECORD_STOP_ALL: {
            vp_record_stats_t *stats;
            int capacity = vp_channel_count(control->pipeline);
            int count = 0;
            int i;

            stats = (vp_record_stats_t *)calloc((size_t)capacity, sizeof(*stats));
            vp_control_event_init(&event,
                                  VP_CONTROL_EVENT_RECORD_STOP_ALL_COMPLETE,
                                  request);
            if (stats == NULL) {
                event.status = APP_ERR_NOMEM;
                vp_control_emit(control, &event);
                break;
            }
            status = vp_control_record_stop_all_ex(control,
                                                   stats,
                                                   capacity,
                                                   &count);
            for (i = 0; i < count; ++i) {
                vp_control_event_t channel_event;

                vp_control_event_init(&channel_event,
                                      VP_CONTROL_EVENT_RECORD_STOP_COMPLETE,
                                      request);
                channel_event.status = APP_OK;
                channel_event.completed_count = 1;
                channel_event.record_stats = stats[i];
                snprintf(channel_event.channel,
                         sizeof(channel_event.channel),
                         "%s",
                         stats[i].channel);
                snprintf(channel_event.path,
                         sizeof(channel_event.path),
                         "%s",
                         stats[i].path);
                channel_event.recording = 0;
                vp_control_emit(control, &channel_event);
            }
            free(stats);
            event.status = status;
            event.completed_count = count;
            vp_control_emit(control, &event);
            break;
        }
        case VP_CONTROL_REQUEST_SNAPSHOT:
            vp_control_event_init(&event,
                                  VP_CONTROL_EVENT_SNAPSHOT_COMPLETE,
                                  request);
            event.status = vp_control_snapshot_ex(control,
                                                  request->channel,
                                                  request->path[0] != '\0'
                                                      ? request->path
                                                      : NULL,
                                                  event.path,
                                                  sizeof(event.path));
            if (event.status == APP_OK) {
                vp_osd_notify_snapshot(control->pipeline);
            }
            vp_control_event_set_recording(control, &event);
            vp_control_emit(control, &event);
            break;
        case VP_CONTROL_REQUEST_STREAM_SELECT:
            vp_control_event_init(&event,
                                  VP_CONTROL_EVENT_STREAM_SELECT_COMPLETE,
                                  request);
            event.status = vp_control_stream_select(control, request->channel);
            vp_control_event_set_recording(control, &event);
            vp_control_emit(control, &event);
            break;
        case VP_CONTROL_REQUEST_STREAM_ENABLE:
            vp_control_event_init(&event,
                                  VP_CONTROL_EVENT_STREAM_ENABLE_COMPLETE,
                                  request);
            event.status = vp_control_stream_set_enabled(control, request->enabled);
            vp_control_emit(control, &event);
            break;
        case VP_CONTROL_REQUEST_OSD_ENABLE:
            vp_control_event_init(&event,
                                  VP_CONTROL_EVENT_OSD_ENABLE_COMPLETE,
                                  request);
            event.status = vp_control_osd_set_enabled(control, request->enabled);
            vp_control_emit(control, &event);
            break;
        case VP_CONTROL_REQUEST_OSD_POSITION:
            vp_control_event_init(&event,
                                  VP_CONTROL_EVENT_OSD_POSITION_COMPLETE,
                                  request);
            event.status = vp_control_osd_set_position(control, request->x, request->y);
            vp_control_emit(control, &event);
            break;
        default:
            break;
    }
}

static void *vp_control_worker(void *opaque) {
    vp_control_t *control = (vp_control_t *)opaque;

    while (1) {
        vp_control_request_t request;

        pthread_mutex_lock(&control->queue_lock);
        while (control->queue_count == 0 && !control->stop_requested) {
            pthread_cond_wait(&control->queue_cond, &control->queue_lock);
        }
        if (control->queue_count == 0 && control->stop_requested) {
            pthread_mutex_unlock(&control->queue_lock);
            break;
        }
        request = control->queue[control->queue_head];
        control->queue_head = (control->queue_head + 1) % VP_CONTROL_QUEUE_CAPACITY;
        control->queue_count--;
        pthread_mutex_unlock(&control->queue_lock);

        vp_control_execute_request(control, &request);
    }
    return NULL;
}

static app_status_t vp_control_submit(vp_control_t *control,
                                      vp_control_request_type_t type,
                                      const char *channel,
                                      const char *path,
                                      int enabled,
                                      int x,
                                      int y,
                                      uint64_t *out_request_id) {
    vp_control_request_t request;
    int tail;

    if (out_request_id != NULL) {
        *out_request_id = 0;
    }
    if (control == NULL) {
        return APP_ERR_PARAM;
    }
    memset(&request, 0, sizeof(request));
    request.type = type;
    request.enabled = enabled != 0;
    request.x = x;
    request.y = y;
    if (vp_control_copy_arg(request.channel,
                            sizeof(request.channel),
                            channel) != APP_OK ||
        vp_control_copy_arg(request.path,
                            sizeof(request.path),
                            path) != APP_OK) {
        return APP_ERR_PARAM;
    }

    pthread_mutex_lock(&control->queue_lock);
    if (control->stop_requested) {
        pthread_mutex_unlock(&control->queue_lock);
        return APP_ERR_BUSY;
    }
    if (control->queue_count == VP_CONTROL_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&control->queue_lock);
        return APP_ERR_BUSY;
    }
    request.request_id = control->next_request_id++;
    if (control->next_request_id == 0) {
        control->next_request_id = 1;
    }
    tail = (control->queue_head + control->queue_count) % VP_CONTROL_QUEUE_CAPACITY;
    control->queue[tail] = request;
    control->queue_count++;
    pthread_cond_signal(&control->queue_cond);
    pthread_mutex_unlock(&control->queue_lock);

    if (out_request_id != NULL) {
        *out_request_id = request.request_id;
    }
    return APP_OK;
}

app_status_t vp_control_init(vp_control_t **out_control, vp_ctx_t *pipeline) {
    vp_control_t *control;
    app_status_t status;

    if (out_control == NULL || pipeline == NULL) {
        return APP_ERR_PARAM;
    }
    *out_control = NULL;

    control = (vp_control_t *)calloc(1, sizeof(*control));
    if (control == NULL) {
        return APP_ERR_NOMEM;
    }
    if (pthread_mutex_init(&control->lock, NULL) != 0) {
        free(control);
        return APP_ERR_IO;
    }
    if (pthread_mutex_init(&control->queue_lock, NULL) != 0) {
        pthread_mutex_destroy(&control->lock);
        free(control);
        return APP_ERR_IO;
    }
    if (pthread_cond_init(&control->queue_cond, NULL) != 0) {
        pthread_mutex_destroy(&control->queue_lock);
        pthread_mutex_destroy(&control->lock);
        free(control);
        return APP_ERR_IO;
    }

    control->pipeline = pipeline;
    control->next_request_id = 1;
    control->osd.cross_x = -1;
    control->osd.cross_y = -1;
    status = vp_osd_update(pipeline, &control->osd);
    if (status != APP_OK) {
        pthread_cond_destroy(&control->queue_cond);
        pthread_mutex_destroy(&control->queue_lock);
        pthread_mutex_destroy(&control->lock);
        free(control);
        return status;
    }
    control->osd.seq++;
    if (pthread_create(&control->worker_thread, NULL, vp_control_worker, control) != 0) {
        pthread_cond_destroy(&control->queue_cond);
        pthread_mutex_destroy(&control->queue_lock);
        pthread_mutex_destroy(&control->lock);
        free(control);
        return APP_ERR_IO;
    }
    control->worker_started = 1;

    *out_control = control;
    return APP_OK;
}

void vp_control_deinit(vp_control_t *control) {
    if (control == NULL) {
        return;
    }
    pthread_mutex_lock(&control->queue_lock);
    control->stop_requested = 1;
    pthread_cond_broadcast(&control->queue_cond);
    pthread_mutex_unlock(&control->queue_lock);
    if (control->worker_started) {
        pthread_join(control->worker_thread, NULL);
        control->worker_started = 0;
    }
    pthread_cond_destroy(&control->queue_cond);
    pthread_mutex_destroy(&control->queue_lock);
    pthread_mutex_destroy(&control->lock);
    free(control);
}

app_status_t vp_control_set_event_callback(vp_control_t *control,
                                           vp_control_event_callback_t callback,
                                           void *opaque) {
    if (control == NULL) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&control->queue_lock);
    control->event_callback = callback;
    control->event_opaque = opaque;
    pthread_mutex_unlock(&control->queue_lock);
    return APP_OK;
}

app_status_t vp_control_record_start_async(vp_control_t *control,
                                           const char *channel,
                                           const char *mp4_path,
                                           uint64_t *out_request_id) {
    if (channel == NULL || channel[0] == '\0') {
        return APP_ERR_PARAM;
    }
    return vp_control_submit(control,
                             VP_CONTROL_REQUEST_RECORD_START,
                             channel,
                             mp4_path,
                             0,
                             0,
                             0,
                             out_request_id);
}

app_status_t vp_control_record_start_all_async(vp_control_t *control,
                                               const char *root_dir,
                                               uint64_t *out_request_id) {
    return vp_control_submit(control,
                             VP_CONTROL_REQUEST_RECORD_START_ALL,
                             NULL,
                             root_dir,
                             0,
                             0,
                             0,
                             out_request_id);
}

app_status_t vp_control_record_stop_async(vp_control_t *control,
                                          const char *channel,
                                          uint64_t *out_request_id) {
    if (channel == NULL || channel[0] == '\0') {
        return APP_ERR_PARAM;
    }
    return vp_control_submit(control,
                             VP_CONTROL_REQUEST_RECORD_STOP,
                             channel,
                             NULL,
                             0,
                             0,
                             0,
                             out_request_id);
}

app_status_t vp_control_record_stop_all_async(vp_control_t *control,
                                              uint64_t *out_request_id) {
    return vp_control_submit(control,
                             VP_CONTROL_REQUEST_RECORD_STOP_ALL,
                             NULL,
                             NULL,
                             0,
                             0,
                             0,
                             out_request_id);
}

app_status_t vp_control_snapshot_async(vp_control_t *control,
                                       const char *channel,
                                       const char *jpg_path,
                                       uint64_t *out_request_id) {
    if (channel == NULL || channel[0] == '\0') {
        return APP_ERR_PARAM;
    }
    return vp_control_submit(control,
                             VP_CONTROL_REQUEST_SNAPSHOT,
                             channel,
                             jpg_path,
                             0,
                             0,
                             0,
                             out_request_id);
}

app_status_t vp_control_stream_select_async(vp_control_t *control,
                                            const char *channel,
                                            uint64_t *out_request_id) {
    if (channel == NULL || channel[0] == '\0') {
        return APP_ERR_PARAM;
    }
    return vp_control_submit(control,
                             VP_CONTROL_REQUEST_STREAM_SELECT,
                             channel,
                             NULL,
                             0,
                             0,
                             0,
                             out_request_id);
}

app_status_t vp_control_stream_set_enabled_async(vp_control_t *control,
                                                 int enabled,
                                                 uint64_t *out_request_id) {
    return vp_control_submit(control,
                             VP_CONTROL_REQUEST_STREAM_ENABLE,
                             NULL,
                             NULL,
                             enabled,
                             0,
                             0,
                             out_request_id);
}

app_status_t vp_control_osd_set_enabled_async(vp_control_t *control,
                                              int enabled,
                                              uint64_t *out_request_id) {
    return vp_control_submit(control,
                             VP_CONTROL_REQUEST_OSD_ENABLE,
                             NULL,
                             NULL,
                             enabled,
                             0,
                             0,
                             out_request_id);
}

app_status_t vp_control_osd_set_position_async(vp_control_t *control,
                                               int x,
                                               int y,
                                               uint64_t *out_request_id) {
    return vp_control_submit(control,
                             VP_CONTROL_REQUEST_OSD_POSITION,
                             NULL,
                             NULL,
                             0,
                             x,
                             y,
                             out_request_id);
}

app_status_t vp_control_record_start(vp_control_t *control,
                                     const char *channel,
                                     const char *mp4_path) {
    vp_channel_id_t id;
    app_status_t status;

    if (control == NULL) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&control->lock);
    status = vp_control_find_channel(control, channel, &id);
    if (status == APP_OK) {
        status = vp_record_start(control->pipeline, id, mp4_path);
    }
    pthread_mutex_unlock(&control->lock);
    return status;
}

app_status_t vp_control_record_start_all(vp_control_t *control, const char *root_dir) {
    app_status_t status;

    if (control == NULL || control->pipeline == NULL) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&control->lock);
    status = vp_record_start_all(control->pipeline, root_dir);
    pthread_mutex_unlock(&control->lock);
    return status;
}

app_status_t vp_control_record_stop(vp_control_t *control, const char *channel) {
    return vp_control_record_stop_ex(control, channel, NULL);
}

app_status_t vp_control_record_stop_ex(vp_control_t *control,
                                       const char *channel,
                                       vp_record_stats_t *out_stats) {
    vp_channel_id_t id;
    app_status_t status;

    if (control == NULL) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&control->lock);
    status = vp_control_find_channel(control, channel, &id);
    if (status == APP_OK) {
        status = vp_record_stop_ex(control->pipeline, id, out_stats);
    }
    pthread_mutex_unlock(&control->lock);
    return status;
}

app_status_t vp_control_record_stop_all(vp_control_t *control) {
    return vp_control_record_stop_all_ex(control, NULL, 0, NULL);
}

app_status_t vp_control_record_stop_all_ex(vp_control_t *control,
                                           vp_record_stats_t *stats,
                                           int stats_capacity,
                                           int *out_count) {
    app_status_t status;

    if (control == NULL || control->pipeline == NULL) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&control->lock);
    status = vp_record_stop_all_ex(control->pipeline,
                                   stats,
                                   stats_capacity,
                                   out_count);
    pthread_mutex_unlock(&control->lock);
    return status;
}

app_status_t vp_control_snapshot(vp_control_t *control,
                                 const char *channel,
                                 const char *jpg_path) {
    vp_channel_id_t id;
    app_status_t status;

    if (control == NULL) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&control->lock);
    status = vp_control_find_channel(control, channel, &id);
    if (status == APP_OK) {
        status = vp_snapshot(control->pipeline, id, jpg_path);
    }
    pthread_mutex_unlock(&control->lock);
    return status;
}

app_status_t vp_control_stream_select(vp_control_t *control, const char *channel) {
    vp_channel_id_t id;
    app_status_t status;

    if (control == NULL) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&control->lock);
    status = vp_control_find_channel(control, channel, &id);
    if (status == APP_OK) {
        status = vp_stream_select(control->pipeline, id);
    }
    pthread_mutex_unlock(&control->lock);
    return status;
}

app_status_t vp_control_stream_set_enabled(vp_control_t *control, int enabled) {
    app_status_t status;

    if (control == NULL || control->pipeline == NULL) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&control->lock);
    status = vp_stream_set_enabled(control->pipeline, enabled);
    pthread_mutex_unlock(&control->lock);
    return status;
}

int vp_control_stream_is_enabled(vp_control_t *control) {
    int enabled;

    if (control == NULL || control->pipeline == NULL) {
        return 0;
    }
    pthread_mutex_lock(&control->lock);
    enabled = vp_stream_is_enabled(control->pipeline);
    pthread_mutex_unlock(&control->lock);
    return enabled;
}

app_status_t vp_control_osd_set_enabled(vp_control_t *control, int enabled) {
    app_status_t status;

    if (control == NULL || control->pipeline == NULL) {
        return APP_ERR_PARAM;
    }

    pthread_mutex_lock(&control->lock);
    control->osd.show_crosshair = enabled != 0;
    status = vp_osd_update(control->pipeline, &control->osd);
    if (status == APP_OK) {
        control->osd.seq++;
    }
    pthread_mutex_unlock(&control->lock);
    return status;
}

app_status_t vp_control_osd_set_position(vp_control_t *control, int x, int y) {
    app_status_t status;

    if (control == NULL || control->pipeline == NULL) {
        return APP_ERR_PARAM;
    }

    pthread_mutex_lock(&control->lock);
    control->osd.cross_x = x;
    control->osd.cross_y = y;
    control->osd.show_crosshair = 1;
    status = vp_osd_update(control->pipeline, &control->osd);
    if (status == APP_OK) {
        control->osd.seq++;
    }
    pthread_mutex_unlock(&control->lock);
    return status;
}

app_status_t vp_control_osd_get(vp_control_t *control, vp_osd_params_t *out_params) {
    if (control == NULL || out_params == NULL) {
        return APP_ERR_PARAM;
    }

    pthread_mutex_lock(&control->lock);
    *out_params = control->osd;
    pthread_mutex_unlock(&control->lock);
    return APP_OK;
}
