#ifndef VIDEO_PIPELINE_CONTROL_H
#define VIDEO_PIPELINE_CONTROL_H

#include "video/video_pipeline.h"

/*
 * High-level control API for buttons, UART handlers, and command shells.
 * Channel arguments use the configured channel name, for example "usb1".
 * Requests are serialized so multiple control sources can share one instance.
 */
typedef struct vp_control vp_control_t;

typedef enum {
    VP_CONTROL_EVENT_RECORD_START_COMPLETE = 0,
    VP_CONTROL_EVENT_RECORD_START_ALL_COMPLETE,
    VP_CONTROL_EVENT_RECORD_STOP_COMPLETE,
    VP_CONTROL_EVENT_RECORD_STOP_ALL_COMPLETE,
    VP_CONTROL_EVENT_SNAPSHOT_COMPLETE,
    VP_CONTROL_EVENT_STREAM_SELECT_COMPLETE,
    VP_CONTROL_EVENT_STREAM_ENABLE_COMPLETE,
    VP_CONTROL_EVENT_OSD_ENABLE_COMPLETE,
    VP_CONTROL_EVENT_OSD_POSITION_COMPLETE
} vp_control_event_type_t;

typedef struct {
    vp_control_event_type_t type;
    uint64_t request_id;
    app_status_t status;
    char channel[APP_NAME_MAX_LEN];
    char path[APP_PATH_MAX_LEN];
    int recording;
    int enabled;
    int x;
    int y;
    int completed_count;
    vp_record_stats_t record_stats;
} vp_control_event_t;

/*
 * Called from the control worker thread. Keep it short and non-blocking.
 * The event is valid only during the callback; do not call vp_control_deinit here.
 */
typedef void (*vp_control_event_callback_t)(const vp_control_event_t *event,
                                            void *opaque);

app_status_t vp_control_init(vp_control_t **out_control, vp_ctx_t *pipeline);
void vp_control_deinit(vp_control_t *control);
app_status_t vp_control_set_event_callback(vp_control_t *control,
                                           vp_control_event_callback_t callback,
                                           void *opaque);

/*
 * Asynchronous control API for UART, remote-control, and button threads.
 * APP_OK means the request was queued. Completion is reported through callback.
 */
app_status_t vp_control_record_start_async(vp_control_t *control,
                                           const char *channel,
                                           const char *mp4_path,
                                           uint64_t *out_request_id);
app_status_t vp_control_record_start_all_async(vp_control_t *control,
                                               const char *root_dir,
                                               uint64_t *out_request_id);
app_status_t vp_control_record_stop_async(vp_control_t *control,
                                          const char *channel,
                                          uint64_t *out_request_id);
app_status_t vp_control_record_stop_all_async(vp_control_t *control,
                                              uint64_t *out_request_id);
app_status_t vp_control_snapshot_async(vp_control_t *control,
                                       const char *channel,
                                       const char *jpg_path,
                                       uint64_t *out_request_id);
app_status_t vp_control_stream_select_async(vp_control_t *control,
                                            const char *channel,
                                            uint64_t *out_request_id);
app_status_t vp_control_stream_set_enabled_async(vp_control_t *control,
                                                 int enabled,
                                                 uint64_t *out_request_id);
app_status_t vp_control_osd_set_enabled_async(vp_control_t *control,
                                              int enabled,
                                              uint64_t *out_request_id);
app_status_t vp_control_osd_set_position_async(vp_control_t *control,
                                               int x,
                                               int y,
                                               uint64_t *out_request_id);

/* Synchronous compatibility API. Do not call these from input/event threads. */
app_status_t vp_control_record_start(vp_control_t *control,
                                     const char *channel,
                                     const char *mp4_path);
app_status_t vp_control_record_start_all(vp_control_t *control, const char *root_dir);
app_status_t vp_control_record_stop(vp_control_t *control, const char *channel);
app_status_t vp_control_record_stop_all(vp_control_t *control);
app_status_t vp_control_record_stop_ex(vp_control_t *control,
                                       const char *channel,
                                       vp_record_stats_t *out_stats);
app_status_t vp_control_record_stop_all_ex(vp_control_t *control,
                                           vp_record_stats_t *stats,
                                           int stats_capacity,
                                           int *out_count);

app_status_t vp_control_snapshot(vp_control_t *control,
                                 const char *channel,
                                 const char *jpg_path);
app_status_t vp_control_stream_select(vp_control_t *control, const char *channel);
app_status_t vp_control_stream_set_enabled(vp_control_t *control, int enabled);
int vp_control_stream_is_enabled(vp_control_t *control);

app_status_t vp_control_osd_set_enabled(vp_control_t *control, int enabled);
/* Setting a position also enables the crosshair. Use -1, -1 for image center. */
app_status_t vp_control_osd_set_position(vp_control_t *control, int x, int y);
app_status_t vp_control_osd_get(vp_control_t *control, vp_osd_params_t *out_params);

#endif
