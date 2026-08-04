#ifndef INPUT_CSI0_H
#define INPUT_CSI0_H

#include "common/common.h"
#include "input/input_common.h"

typedef struct {
    video_input_config_t cfg;
    ffmpeg_input_ctx_t decoder;
    int is_opened;
} input_csi0_ctx_t;

app_status_t input_csi0_open(input_csi0_ctx_t *ctx, const video_input_config_t *cfg);
app_status_t input_csi0_read(input_csi0_ctx_t *ctx, video_frame_t *frame);
void input_csi0_close(input_csi0_ctx_t *ctx);

#endif
