#ifndef PROCESS_VIDEO_COMMON_H
#define PROCESS_VIDEO_COMMON_H

#include "common/common.h"
#include "process/video/video_frame_convert.h"

typedef struct {
    int pip_enabled;
    int main_index;
    int sub_index;
    video_frame_convert_ctx_t yuv420_convert;
} process_common_ctx_t;

app_status_t process_common_init(process_common_ctx_t *ctx);
app_status_t process_common_convert_to_yuv420(process_common_ctx_t *ctx, video_frame_t *frame);
void process_common_deinit(process_common_ctx_t *ctx);

#endif
