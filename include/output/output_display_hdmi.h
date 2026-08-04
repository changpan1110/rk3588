#ifndef OUTPUT_DISPLAY_HDMI_H
#define OUTPUT_DISPLAY_HDMI_H

#include "common/common.h"

typedef struct {
    int connector_id;
} output_display_hdmi_ctx_t;

app_status_t output_display_hdmi_init(output_display_hdmi_ctx_t *ctx, int connector_id);
app_status_t output_display_hdmi_show(output_display_hdmi_ctx_t *ctx, const video_frame_t *frame);
void output_display_hdmi_deinit(output_display_hdmi_ctx_t *ctx);

#endif
