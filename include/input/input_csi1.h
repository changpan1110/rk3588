#ifndef INPUT_CSI1_H
#define INPUT_CSI1_H

#include "common/common.h"
#include "input/input_common.h"

typedef struct {
    video_input_config_t cfg;
    ffmpeg_input_ctx_t decoder;
    int is_opened;
} input_csi1_ctx_t;

app_status_t input_csi1_open(input_csi1_ctx_t *ctx, const video_input_config_t *cfg);
app_status_t input_csi1_read(input_csi1_ctx_t *ctx, video_frame_t *frame);
void input_csi1_close(input_csi1_ctx_t *ctx);

#endif
