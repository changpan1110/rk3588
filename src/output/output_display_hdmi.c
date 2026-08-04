#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "output_display_hdmi.c"

#include "output/output_display_hdmi.h"

#include <string.h>

#include "common/debug.h"

app_status_t output_display_hdmi_init(output_display_hdmi_ctx_t *ctx, int connector_id) {
    if (ctx == NULL) {
        return APP_ERR_PARAM;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->connector_id = connector_id;
    LOGI("hdmi display init connector_id=%d", connector_id);
    return APP_OK;
}

app_status_t output_display_hdmi_show(output_display_hdmi_ctx_t *ctx, const video_frame_t *frame) {
    if (ctx == NULL || frame == NULL || frame->av_frame == NULL) {
        return APP_ERR_PARAM;
    }
    LOGD("display frame source=%s size=%dx%d", frame->source_name, frame->av_frame->width, frame->av_frame->height);
    return APP_OK;
}

void output_display_hdmi_deinit(output_display_hdmi_ctx_t *ctx) {
    (void)ctx;
}
