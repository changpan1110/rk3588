#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "input_common.c"

#include "input/input_common.h"

#include <stdio.h>
#include <string.h>

#include <libavutil/log.h>

#include "common/debug.h"

void ffmpeg_input_log_stream(const video_input_config_t *cfg, const ffmpeg_input_ctx_t *ctx) {
    int previous_log_level;

    if (cfg == NULL || ctx == NULL || ctx->fmt_ctx == NULL ||
        ctx->video_stream_index < 0 ||
        ctx->video_stream_index >= (int)ctx->fmt_ctx->nb_streams) {
        return;
    }

    previous_log_level = av_log_get_level();
    if (previous_log_level < AV_LOG_INFO) {
        av_log_set_level(AV_LOG_INFO);
    }
    av_dump_format(ctx->fmt_ctx, 0, cfg->device, 0);
    if (previous_log_level < AV_LOG_INFO) {
        av_log_set_level(previous_log_level);
    }
}

static app_status_t ffmpeg_open_decoder(ffmpeg_input_ctx_t *ctx) {
    AVStream *stream = ctx->fmt_ctx->streams[ctx->video_stream_index];
    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    int ret;

    if (decoder == NULL) {
        LOGE("decoder not found for codec id=%d", stream->codecpar->codec_id);
        return APP_ERR_UNSUPPORTED;
    }

    ctx->dec_ctx = avcodec_alloc_context3(decoder);
    if (ctx->dec_ctx == NULL) {
        return APP_ERR_NOMEM;
    }

    ret = avcodec_parameters_to_context(ctx->dec_ctx, stream->codecpar);
    if (ret < 0) {
        LOGE("avcodec_parameters_to_context failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    ret = avcodec_open2(ctx->dec_ctx, decoder, NULL);
    if (ret < 0) {
        LOGE("avcodec_open2 failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    ctx->codec_id = stream->codecpar->codec_id;
    return APP_OK;
}

app_status_t ffmpeg_input_open(ffmpeg_input_ctx_t *ctx, const video_input_config_t *cfg) {
    char video_size[32];
    char fps[16];
    const AVInputFormat *input_fmt;
    AVDictionary *options = NULL;
    int ret;

    if (ctx == NULL || cfg == NULL) {
        return APP_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));

    input_fmt = av_find_input_format("video4linux2");
    if (input_fmt == NULL) {
        LOGE("video4linux2 input format not found");
        return APP_ERR_UNSUPPORTED;
    }

    snprintf(video_size, sizeof(video_size), "%dx%d", cfg->width, cfg->height);
    snprintf(fps, sizeof(fps), "%d", cfg->fps);

    av_dict_set(&options, "video_size", video_size, 0);
    av_dict_set(&options, "framerate", fps, 0);
    if (cfg->input_format[0] != '\0') {
        av_dict_set(&options, "input_format", cfg->input_format, 0);
    }

    ret = avformat_open_input(&ctx->fmt_ctx, cfg->device, (AVInputFormat *)input_fmt, &options);
    av_dict_free(&options);
    if (ret < 0) {
        LOGE("avformat_open_input failed for %s: %d", cfg->device, ret);
        return APP_ERR_FFMPEG;
    }

    /* v4l2 只有一个流,跳过 find_stream_info,避免无信号设备永久阻塞 */
    ret = av_find_best_stream(ctx->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret < 0) {
        LOGE("av_find_best_stream failed: %d", ret);
        return APP_ERR_FFMPEG;
    }
    ctx->video_stream_index = ret;
    ffmpeg_input_log_stream(cfg, ctx);

    ctx->packet = av_packet_alloc();
    if (ctx->packet == NULL) {
        return APP_ERR_NOMEM;
    }

    return ffmpeg_open_decoder(ctx);
}

app_status_t ffmpeg_input_read_frame(ffmpeg_input_ctx_t *ctx, AVFrame *frame) {
    int ret;

    if (ctx == NULL || frame == NULL) {
        return APP_ERR_PARAM;
    }

    while ((ret = av_read_frame(ctx->fmt_ctx, ctx->packet)) >= 0) {
        if (ctx->packet->stream_index != ctx->video_stream_index) {
            av_packet_unref(ctx->packet);
            continue;
        }

        ret = avcodec_send_packet(ctx->dec_ctx, ctx->packet);
        av_packet_unref(ctx->packet);
        if (ret < 0) {
            LOGE("avcodec_send_packet failed: %d", ret);
            return APP_ERR_FFMPEG;
        }

        ret = avcodec_receive_frame(ctx->dec_ctx, frame);
        if (ret == 0) {
            return APP_OK;
        }
        if (ret == AVERROR(EAGAIN)) {
            continue;
        }
        if (ret == AVERROR_EOF) {
            return APP_ERR_EOF;
        }

        LOGE("avcodec_receive_frame failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    return APP_ERR_EOF;
}

void ffmpeg_input_close(ffmpeg_input_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    if (ctx->packet != NULL) {
        av_packet_free(&ctx->packet);
    }
    if (ctx->dec_ctx != NULL) {
        avcodec_free_context(&ctx->dec_ctx);
    }
    if (ctx->fmt_ctx != NULL) {
        avformat_close_input(&ctx->fmt_ctx);
    }
    memset(ctx, 0, sizeof(*ctx));
}
