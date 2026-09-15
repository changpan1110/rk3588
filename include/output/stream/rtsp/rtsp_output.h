#ifndef RTSP_OUTPUT_H
#define RTSP_OUTPUT_H

#include "common/common.h"

typedef struct {
    char url[APP_PATH_MAX_LEN];
    char transport[8];
    AVFormatContext *format_ctx;
    AVStream *stream;
    AVRational input_time_base;
    int header_written;
} rtsp_output_ctx_t;

app_status_t rtsp_output_init(rtsp_output_ctx_t *ctx,
                              const char *url,
                              const char *transport,
                              const AVCodecContext *encoder);
app_status_t rtsp_output_send(rtsp_output_ctx_t *ctx, AVPacket *packet);
int rtsp_output_is_open(const rtsp_output_ctx_t *ctx);
void rtsp_output_deinit(rtsp_output_ctx_t *ctx);

#endif
