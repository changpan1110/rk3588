#ifndef SRT_OUTPUT_H
#define SRT_OUTPUT_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "common/common.h"

typedef struct {
    AVFormatContext *format_ctx;
    AVStream *stream;
    AVRational input_time_base;
    char url[APP_PATH_MAX_LEN];
    char passphrase_file[APP_PATH_MAX_LEN];
    int header_written;
} srt_output_ctx_t;

app_status_t srt_output_init(srt_output_ctx_t *ctx,
                             const char *url,
                             const char *passphrase_file,
                             const AVCodecContext *encoder);
app_status_t srt_output_send(srt_output_ctx_t *ctx,
                             const AVPacket *packet);
int srt_output_is_open(const srt_output_ctx_t *ctx);
void srt_output_deinit(srt_output_ctx_t *ctx);

#endif
