#ifndef OUTPUT_ENCODE_SUB_H
#define OUTPUT_ENCODE_SUB_H

#include "common/common.h"
#include "output/output_encode_common.h"

typedef struct {
    encoder_mode_t mode;
    video_encode_params_t params;
    int enabled;
    output_encode_convert_ctx_t convert;
} output_encode_sub_ctx_t;

app_status_t output_encode_sub_init(output_encode_sub_ctx_t *ctx,
                                    encoder_mode_t mode,
                                    const video_encode_params_t *params,
                                    int enabled);
app_status_t output_encode_sub_push(output_encode_sub_ctx_t *ctx, const video_frame_t *frame);
void output_encode_sub_deinit(output_encode_sub_ctx_t *ctx);

#endif
