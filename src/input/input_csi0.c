#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "input_csi0.c"

#include "input/input_csi0.h"

#include <stdio.h>
#include <string.h>

#include "common/debug.h"

static int input_csi0_is_rawvideo(enum AVCodecID codec_id) {
    return codec_id == AV_CODEC_ID_RAWVIDEO;
}

static int input_csi0_is_raw_v4l2_format(const char *fmt) {
    if (fmt == NULL || fmt[0] == '\0') {
        return 0;
    }

    return strcmp(fmt, "uyvy422") == 0 ||
           strcmp(fmt, "yuyv422") == 0 ||
           strcmp(fmt, "nv12") == 0 ||
           strcmp(fmt, "nv21") == 0 ||
           strcmp(fmt, "nv16") == 0 ||
           strcmp(fmt, "nv61") == 0 ||
           strcmp(fmt, "nm12") == 0 ||
           strcmp(fmt, "nm21") == 0;
}

static const AVCodec *input_csi0_select_decoder(enum AVCodecID codec_id) {
    const AVCodec *decoder = NULL;

    if (input_csi0_is_rawvideo(codec_id)) {
        decoder = avcodec_find_decoder(codec_id);
        if (decoder != NULL) {
            LOGI("csi0 rawvideo path: %s", decoder->name);
        }
        return decoder;
    }

    switch (codec_id) {
        case AV_CODEC_ID_H264:
            decoder = avcodec_find_decoder_by_name("h264_rkmpp");
            if (decoder != NULL) {
                LOGI("csi0 decoder select: h264_rkmpp");
                return decoder;
            }
            break;
        case AV_CODEC_ID_HEVC:
            decoder = avcodec_find_decoder_by_name("hevc_rkmpp");
            if (decoder != NULL) {
                LOGI("csi0 decoder select: hevc_rkmpp");
                return decoder;
            }
            break;
        case AV_CODEC_ID_MJPEG:
            decoder = avcodec_find_decoder_by_name("mjpeg_rkmpp");
            if (decoder != NULL) {
                LOGI("csi0 decoder select: mjpeg_rkmpp");
                return decoder;
            }
            break;
        default:
            break;
    }

    decoder = avcodec_find_decoder(codec_id);
    if (decoder != NULL) {
        LOGI("csi0 decoder fallback: %s", decoder->name);
    }
    return decoder;
}

static app_status_t input_csi0_open_decoder(input_csi0_ctx_t *ctx) {
    AVStream *stream;
    const AVCodec *decoder;
    int ret;

    stream = ctx->decoder.fmt_ctx->streams[ctx->decoder.video_stream_index];
    if (input_csi0_is_rawvideo(stream->codecpar->codec_id)) {
        LOGI("csi0 stream codec is rawvideo, skip rkmpp compressed decoder selection");
    }
    decoder = input_csi0_select_decoder(stream->codecpar->codec_id);
    if (decoder == NULL) {
        LOGE("csi0 decoder not found for codec id=%d", stream->codecpar->codec_id);
        return APP_ERR_UNSUPPORTED;
    }

    ctx->decoder.dec_ctx = avcodec_alloc_context3(decoder);
    if (ctx->decoder.dec_ctx == NULL) {
        return APP_ERR_NOMEM;
    }

    ret = avcodec_parameters_to_context(ctx->decoder.dec_ctx, stream->codecpar);
    if (ret < 0) {
        LOGE("csi0 avcodec_parameters_to_context failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    ret = avcodec_open2(ctx->decoder.dec_ctx, decoder, NULL);
    if (ret < 0) {
        LOGE("csi0 avcodec_open2 failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    ctx->decoder.codec_id = stream->codecpar->codec_id;
    return APP_OK;
}

static app_status_t input_csi0_open_device(input_csi0_ctx_t *ctx) {
    char video_size[32];
    char fps[16];
    const AVInputFormat *input_fmt;
    AVDictionary *options = NULL;
    int ret;

    input_fmt = av_find_input_format("video4linux2");
    if (input_fmt == NULL) {
        LOGE("csi0 video4linux2 input format not found");
        return APP_ERR_UNSUPPORTED;
    }

    if (ctx->cfg.width > 0 && ctx->cfg.height > 0) {
        snprintf(video_size, sizeof(video_size), "%dx%d", ctx->cfg.width, ctx->cfg.height);
        av_dict_set(&options, "video_size", video_size, 0);
    }

    if (ctx->cfg.fps > 0 && !input_csi0_is_raw_v4l2_format(ctx->cfg.input_format)) {
        snprintf(fps, sizeof(fps), "%d", ctx->cfg.fps);
        av_dict_set(&options, "framerate", fps, 0);
    }

    if (ctx->cfg.input_format[0] != '\0') {
        av_dict_set(&options, "input_format", ctx->cfg.input_format, 0);
    }

    if (ctx->cfg.input_format[0] != '\0' || (ctx->cfg.width > 0 && ctx->cfg.height > 0)) {
        LOGI("csi0 request format: size=%s fps=%s fmt=%s",
             ctx->cfg.width > 0 && ctx->cfg.height > 0 ? video_size : "driver-default",
             (ctx->cfg.fps > 0 && !input_csi0_is_raw_v4l2_format(ctx->cfg.input_format)) ? fps : "driver-default",
             ctx->cfg.input_format[0] != '\0' ? ctx->cfg.input_format : "driver-default");
    } else {
        LOGI("csi0 use driver current format");
    }

    ret = avformat_open_input(&ctx->decoder.fmt_ctx, ctx->cfg.device, (AVInputFormat *)input_fmt, &options);
    av_dict_free(&options);
    if (ret < 0) {
        LOGE("csi0 avformat_open_input failed for %s: %d", ctx->cfg.device, ret);
        return APP_ERR_FFMPEG;
    }

    /*
     * v4l2 只有一个视频流,avformat_open_input 时已填好 codecpar。
     * 不调 avformat_find_stream_info:设备无信号时它会永久阻塞在帧读取上,
     * 导致整路初始化卡死。改为直接取流,没信号的设备由采集线程读帧重试。
     */
    ret = av_find_best_stream(ctx->decoder.fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret < 0) {
        LOGE("csi0 av_find_best_stream failed: %d", ret);
        return APP_ERR_FFMPEG;
    }
    ctx->decoder.video_stream_index = ret;
    {
        AVStream *stream = ctx->decoder.fmt_ctx->streams[ctx->decoder.video_stream_index];
        ctx->cfg.width = stream->codecpar->width;
        ctx->cfg.height = stream->codecpar->height;
    }
    ffmpeg_input_log_stream(&ctx->cfg, &ctx->decoder);

    ctx->decoder.packet = av_packet_alloc();
    if (ctx->decoder.packet == NULL) {
        return APP_ERR_NOMEM;
    }

    return input_csi0_open_decoder(ctx);
}

static app_status_t input_csi0_read_decoder(input_csi0_ctx_t *ctx, AVFrame *frame) {
    int ret;

    while ((ret = av_read_frame(ctx->decoder.fmt_ctx, ctx->decoder.packet)) >= 0) {
        if (ctx->decoder.packet->stream_index != ctx->decoder.video_stream_index) {
            av_packet_unref(ctx->decoder.packet);
            continue;
        }

        ret = avcodec_send_packet(ctx->decoder.dec_ctx, ctx->decoder.packet);
        av_packet_unref(ctx->decoder.packet);
        if (ret < 0) {
            LOGE("csi0 avcodec_send_packet failed: %d", ret);
            return APP_ERR_FFMPEG;
        }

        ret = avcodec_receive_frame(ctx->decoder.dec_ctx, frame);
        if (ret == 0) {
            return APP_OK;
        }
        if (ret == AVERROR(EAGAIN)) {
            continue;
        }
        if (ret == AVERROR_EOF) {
            return APP_ERR_EOF;
        }

        LOGE("csi0 avcodec_receive_frame failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    return APP_ERR_EOF;
}

app_status_t input_csi0_open(input_csi0_ctx_t *ctx, const video_input_config_t *cfg) {
    app_status_t status;

    if (ctx == NULL || cfg == NULL) {
        return APP_ERR_PARAM;
    }

    *ctx = (input_csi0_ctx_t){0};
    ctx->cfg = *cfg;
    LOGI("open csi0 input: %s %dx%d@%d fmt=%s",
        ctx->cfg.device, ctx->cfg.width, ctx->cfg.height, ctx->cfg.fps, ctx->cfg.input_format);
    status = input_csi0_open_device(ctx);
    if (status != APP_OK) {
        ctx->is_opened = 0;
        return status;
    }
    ctx->is_opened = 1;
    return APP_OK;
}

app_status_t input_csi0_read(input_csi0_ctx_t *ctx, video_frame_t *frame) {
    app_status_t status;

    if (ctx == NULL || frame == NULL || frame->av_frame == NULL) {
        return APP_ERR_PARAM;
    }
    if (!ctx->is_opened) {
        return APP_ERR_IO;
    }

    status = input_csi0_read_decoder(ctx, frame->av_frame);
    if (status != APP_OK) {
        return status;
    }

    frame->pts_us = app_get_time_us();
    frame->source_type = VIDEO_SOURCE_CSI0;
    strncpy(frame->source_name, "csi0", sizeof(frame->source_name) - 1);
    frame->source_name[sizeof(frame->source_name) - 1] = '\0';
    return APP_OK;
}

void input_csi0_close(input_csi0_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    if (ctx->decoder.packet != NULL) {
        av_packet_free(&ctx->decoder.packet);
    }
    if (ctx->decoder.dec_ctx != NULL) {
        avcodec_free_context(&ctx->decoder.dec_ctx);
    }
    if (ctx->decoder.fmt_ctx != NULL) {
        avformat_close_input(&ctx->decoder.fmt_ctx);
    }
    memset(&ctx->decoder, 0, sizeof(ctx->decoder));
    ctx->is_opened = 0;
}
