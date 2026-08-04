#ifndef OUTPUT_STREAM_RTSP_H
#define OUTPUT_STREAM_RTSP_H

#include "common/common.h"

typedef struct {
    char url[APP_PATH_MAX_LEN];
} output_stream_rtsp_ctx_t;

app_status_t output_stream_rtsp_init(output_stream_rtsp_ctx_t *ctx, const char *url);
app_status_t output_stream_rtsp_send(output_stream_rtsp_ctx_t *ctx, const uint8_t *data, size_t size);
void output_stream_rtsp_deinit(output_stream_rtsp_ctx_t *ctx);

#endif
