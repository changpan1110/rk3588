#ifndef INPUT_HDMI_IN_H
#define INPUT_HDMI_IN_H

#include "common/common.h"
#include "input/input_common.h"

typedef struct input_hdmi_native_ctx input_hdmi_native_ctx_t;

typedef struct {
    video_input_config_t cfg;
    ffmpeg_input_ctx_t decoder;
    input_hdmi_native_ctx_t *native;
    int use_native;
    int is_opened;
} input_hdmi_in_ctx_t;

app_status_t input_hdmi_in_open(input_hdmi_in_ctx_t *ctx, const video_input_config_t *cfg);
app_status_t input_hdmi_in_read(input_hdmi_in_ctx_t *ctx, video_frame_t *frame);
void input_hdmi_in_close(input_hdmi_in_ctx_t *ctx);

#endif
