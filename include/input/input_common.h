#ifndef INPUT_COMMON_H
#define INPUT_COMMON_H

#include "common/common.h"

typedef struct {
    AVFormatContext *fmt_ctx;
    AVCodecContext *dec_ctx;
    AVPacket *packet;
    int video_stream_index;
    enum AVCodecID codec_id;
} ffmpeg_input_ctx_t;

void ffmpeg_input_log_stream(const video_input_config_t *cfg, const ffmpeg_input_ctx_t *ctx);

#endif
