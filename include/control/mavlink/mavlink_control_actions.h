#ifndef RK3588_MAVLINK_CONTROL_ACTIONS_H
#define RK3588_MAVLINK_CONTROL_ACTIONS_H

#include "control/mavlink/mavlink_control_service.h"
#include "control/video_pipeline/video_pipeline_control.h"
#include "process/video/video_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mavlink_control_actions mavlink_control_actions_t;

app_status_t mavlink_control_actions_init(
    mavlink_control_actions_t **out_actions,
    vp_ctx_t *pipeline,
    vp_control_t *video_control);

void mavlink_control_actions_deinit(mavlink_control_actions_t *actions);

/* Callback passed directly to mavlink_control_service_init(). */
app_status_t mavlink_control_actions_handle(
    const mavlink_control_key_event_t *event,
    void *user_data,
    uint64_t *out_request_id);

#ifdef __cplusplus
}
#endif

#endif
