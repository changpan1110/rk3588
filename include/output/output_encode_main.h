#ifndef OUTPUT_ENCODE_MAIN_H
#define OUTPUT_ENCODE_MAIN_H

#include "common/common.h"
#include "output/output_encode_common.h"

typedef struct {
    encoder_mode_t mode;
    video_encode_params_t params;
    output_encode_convert_ctx_t convert;
} output_encode_main_ctx_t;

app_status_t output_encode_main_init(output_encode_main_ctx_t *ctx,
                                     encoder_mode_t mode,
                                     const video_encode_params_t *params);
app_status_t output_encode_main_push(output_encode_main_ctx_t *ctx, const video_frame_t *frame);
void output_encode_main_deinit(output_encode_main_ctx_t *ctx);

#endif
