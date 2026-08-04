#ifndef OUTPUT_RECORD_H
#define OUTPUT_RECORD_H

#include "common/common.h"
#include "output/output_store_mp4.h"

typedef struct {
    char path[APP_PATH_MAX_LEN];
    int is_recording;
    video_encode_params_t encode;
    output_store_mp4_ctx_t mp4;
} output_record_ctx_t;

typedef struct {
    const char *path;
    video_encode_params_t encode;
} output_record_params_t;

app_status_t output_record_init(output_record_ctx_t *ctx);
app_status_t output_record_start(output_record_ctx_t *ctx, const output_record_params_t *params);
app_status_t output_record_write(output_record_ctx_t *ctx, const encoded_packet_t *packet);
app_status_t output_record_stop(output_record_ctx_t *ctx);
int output_record_is_running(const output_record_ctx_t *ctx);
void output_record_deinit(output_record_ctx_t *ctx);

#endif
