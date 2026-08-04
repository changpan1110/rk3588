#ifndef OUTPUT_STREAM_RTMP_H
#define OUTPUT_STREAM_RTMP_H

#include "common/common.h"

typedef struct {
    char url[APP_PATH_MAX_LEN];
} output_stream_rtmp_ctx_t;

app_status_t output_stream_rtmp_init(output_stream_rtmp_ctx_t *ctx, const char *url);
app_status_t output_stream_rtmp_send(output_stream_rtmp_ctx_t *ctx, const uint8_t *data, size_t size);
void output_stream_rtmp_deinit(output_stream_rtmp_ctx_t *ctx);

#endif
