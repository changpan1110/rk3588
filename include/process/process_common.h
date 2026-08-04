#ifndef PROCESS_COMMON_H
#define PROCESS_COMMON_H

#include "common/common.h"
#include "output/output_encode_common.h"

typedef struct {
    int pip_enabled;
    int main_index;
    int sub_index;
    output_encode_convert_ctx_t yuv420_convert;
} process_common_ctx_t;

app_status_t process_common_init(process_common_ctx_t *ctx);
app_status_t process_common_convert_to_yuv420(process_common_ctx_t *ctx, video_frame_t *frame);
void process_common_deinit(process_common_ctx_t *ctx);

#endif
