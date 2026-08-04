#ifndef OUTPUT_STREAM_RTSP_H
#define OUTPUT_STREAM_RTSP_H

#include "common/common.h"

typedef struct {
    char url[APP_PATH_MAX_LEN];
    AVFormatContext *format_ctx;
    AVStream *stream;
    AVRational input_time_base;
    int header_written;
} output_stream_rtsp_ctx_t;

app_status_t output_stream_rtsp_init(output_stream_rtsp_ctx_t *ctx,
                                     const char *url,
                                     const AVCodecContext *encoder);
app_status_t output_stream_rtsp_send(output_stream_rtsp_ctx_t *ctx, AVPacket *packet);
int output_stream_rtsp_is_open(const output_stream_rtsp_ctx_t *ctx);
void output_stream_rtsp_deinit(output_stream_rtsp_ctx_t *ctx);

#endif
