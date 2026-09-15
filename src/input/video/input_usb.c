#define LOG_LOCAL_LEVEL LOG_LEVEL_TRACE
#define LOG_FILE_NAME "input_usb.c"

#include "input/video/input_usb.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

#include "common/debug.h"

#define INPUT_USB_RKMPP_EXTRA_HW_FRAMES 8

static const char *input_usb_pix_fmt_name(enum AVPixelFormat fmt) {
    const char *name = av_get_pix_fmt_name(fmt);
    return name != NULL ? name : "unknown";
}

static void input_usb_log_hw_frame(input_usb_ctx_t *ctx, const AVFrame *frame) {
    const AVHWFramesContext *frames_ctx;

    if (frame == NULL || frame->hw_frames_ctx == NULL) {
        return;
    }

    ++ctx->hw_frame_count;
    if (ctx->hw_frame_count != 1) {
        return;
    }

    frames_ctx = (const AVHWFramesContext *)frame->hw_frames_ctx->data;
    LOGD("usb hardware frame retained count=%llu fmt=%s sw_fmt=%s",
         (unsigned long long)ctx->hw_frame_count,
         input_usb_pix_fmt_name((enum AVPixelFormat)frame->format),
         frames_ctx != NULL
             ? input_usb_pix_fmt_name(frames_ctx->sw_format)
             : "unknown");
}

static int input_usb_frame_is_hw(const AVFrame *frame) {
    if (frame == NULL) {
        return 0;
    }
    if (frame->hw_frames_ctx != NULL) {
        return 1;
    }
    return frame->format == AV_PIX_FMT_DRM_PRIME;
}

static app_status_t input_usb_transfer_hw_frame(input_usb_ctx_t *ctx, AVFrame *frame) {
    int ret;

    if (ctx->sw_frame == NULL) {
        ctx->sw_frame = av_frame_alloc();
        if (ctx->sw_frame == NULL) {
            return APP_ERR_NOMEM;
        }
    }

    av_frame_unref(ctx->sw_frame);
    ret = av_hwframe_transfer_data(ctx->sw_frame, frame, 0);
    if (ret < 0) {
        LOGE("usb av_hwframe_transfer_data failed: %d src_fmt=%s",
             ret,
             input_usb_pix_fmt_name((enum AVPixelFormat)frame->format));
        return APP_ERR_FFMPEG;
    }
    ret = av_frame_copy_props(ctx->sw_frame, frame);
    if (ret < 0) {
        LOGE("usb av_frame_copy_props failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    av_frame_unref(frame);
    av_frame_move_ref(frame, ctx->sw_frame);
    ++ctx->hw_transfer_count;
    if (ctx->hw_transfer_count == 1) {
        LOGD("usb hardware frame downloaded count=%llu fmt=%s",
             (unsigned long long)ctx->hw_transfer_count,
             input_usb_pix_fmt_name((enum AVPixelFormat)frame->format));
    }
    return APP_OK;
}

static const AVCodec *input_usb_select_decoder(enum AVCodecID codec_id) {
    const AVCodec *decoder = NULL;

    if (codec_id == AV_CODEC_ID_MJPEG) {
        decoder = avcodec_find_decoder_by_name("mjpeg_rkmpp");
        if (decoder == NULL) {
            LOGE("required USB hardware MJPEG decoder mjpeg_rkmpp not found");
            return NULL;
        }

        LOGI("usb decoder select hardware for MJPEG: %s", decoder->name);
        return decoder;
    }

    decoder = avcodec_find_decoder(codec_id);
    if (decoder != NULL) {
        LOGD("usb decoder select software: %s", decoder->name);
    }
    return decoder;
}

static app_status_t input_usb_open_decoder(input_usb_ctx_t *ctx) {
    AVStream *stream;
    const AVCodec *decoder;
    int ret;

    stream = ctx->decoder.fmt_ctx->streams[ctx->decoder.video_stream_index];
    decoder = input_usb_select_decoder(stream->codecpar->codec_id);
    if (decoder == NULL) {
        LOGE("usb decoder not found for codec id=%d", stream->codecpar->codec_id);
        return APP_ERR_UNSUPPORTED;
    }

    ctx->decoder.dec_ctx = avcodec_alloc_context3(decoder);
    if (ctx->decoder.dec_ctx == NULL) {
        return APP_ERR_NOMEM;
    }

    ret = avcodec_parameters_to_context(ctx->decoder.dec_ctx, stream->codecpar);
    if (ret < 0) {
        LOGE("usb avcodec_parameters_to_context failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    if (strcmp(decoder->name, "mjpeg_rkmpp") == 0) {
        ctx->decoder.dec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->decoder.dec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        ctx->decoder.dec_ctx->thread_count = 1;
        ctx->decoder.dec_ctx->extra_hw_frames = INPUT_USB_RKMPP_EXTRA_HW_FRAMES;
    }

    ret = avcodec_open2(ctx->decoder.dec_ctx, decoder, NULL);
    if (ret < 0) {
        LOGE("usb avcodec_open2 failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    ctx->decoder.codec_id = stream->codecpar->codec_id;
    LOGD("usb decoder opened: %s low_delay=%d threads=%d extra_hw_frames=%d",
         decoder->name,
         (ctx->decoder.dec_ctx->flags & AV_CODEC_FLAG_LOW_DELAY) != 0,
         ctx->decoder.dec_ctx->thread_count,
         ctx->decoder.dec_ctx->extra_hw_frames);
    return APP_OK;
}

static app_status_t input_usb_open_device(input_usb_ctx_t *ctx) {
    char video_size[32];
    char fps[16];
    const AVInputFormat *input_fmt;
    AVDictionary *options = NULL;
    int ret;

    input_fmt = av_find_input_format("video4linux2");
    if (input_fmt == NULL) {
        LOGE("usb video4linux2 input format not found");
        return APP_ERR_UNSUPPORTED;
    }

    snprintf(video_size, sizeof(video_size), "%dx%d", ctx->cfg.width, ctx->cfg.height);
    snprintf(fps, sizeof(fps), "%d", ctx->cfg.fps);

    av_dict_set(&options, "video_size", video_size, 0);
    av_dict_set(&options, "framerate", fps, 0);
    if (ctx->cfg.input_format[0] != '\0') {
        av_dict_set(&options, "input_format", ctx->cfg.input_format, 0);
    }

    ctx->decoder.fmt_ctx = avformat_alloc_context();
    if (ctx->decoder.fmt_ctx == NULL) {
        av_dict_free(&options);
        return APP_ERR_NOMEM;
    }
    /* Open V4L2 in non-blocking mode so av_read_frame() returns EAGAIN
     * instead of blocking forever when the camera stops delivering frames
     * (e.g. measured fps drops to 0). */
    ctx->decoder.fmt_ctx->flags |= AVFMT_FLAG_NONBLOCK;
    ret = avformat_open_input(&ctx->decoder.fmt_ctx, ctx->cfg.device,
                              (AVInputFormat *)input_fmt, &options);
    av_dict_free(&options);
    if (ret < 0) {
        if (ret == AVERROR(ENOSPC)) {
            LOGE("usb stream start failed for %s: USB bandwidth exhausted; use another controller or lower resolution/fps",
                 ctx->cfg.device);
        } else {
            LOGE("usb avformat_open_input failed for %s: %d", ctx->cfg.device, ret);
        }
        return APP_ERR_FFMPEG;
    }

    /*
     * v4l2 只有一个视频流,avformat_open_input 时已填好 codecpar。
     * 不调 avformat_find_stream_info:设备无信号时它会永久阻塞在帧读取上。
     */
    ret = av_find_best_stream(ctx->decoder.fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret < 0) {
        LOGE("usb av_find_best_stream failed: %d", ret);
        return APP_ERR_FFMPEG;
    }
    ctx->decoder.video_stream_index = ret;
    ffmpeg_input_log_stream(&ctx->cfg, &ctx->decoder);

    ctx->decoder.packet = av_packet_alloc();
    if (ctx->decoder.packet == NULL) {
        return APP_ERR_NOMEM;
    }

    return input_usb_open_decoder(ctx);
}

static app_status_t input_usb_read_decoder(input_usb_ctx_t *ctx, AVFrame *frame) {
    int64_t decode_start_us;
    int ret;

    while ((ret = av_read_frame(ctx->decoder.fmt_ctx, ctx->decoder.packet)) >= 0) {
        if (ctx->decoder.packet->stream_index != ctx->decoder.video_stream_index) {
            av_packet_unref(ctx->decoder.packet);
            continue;
        }
        if (ctx->decoder.packet->size <= 0 || ctx->decoder.packet->data == NULL) {
            LOGW("usb drop empty compressed frame device=%s size=%d",
                 ctx->cfg.device,
                 ctx->decoder.packet->size);
            av_packet_unref(ctx->decoder.packet);
            continue;
        }

        decode_start_us = app_get_time_us();
        ret = avcodec_send_packet(ctx->decoder.dec_ctx, ctx->decoder.packet);
        av_packet_unref(ctx->decoder.packet);
        if (ret == AVERROR(EINVAL) || ret == AVERROR_INVALIDDATA) {
            LOGW("usb drop invalid compressed frame device=%s decoder=%s error=%d",
                 ctx->cfg.device,
                 ctx->decoder.dec_ctx->codec != NULL
                     ? ctx->decoder.dec_ctx->codec->name
                     : "unknown",
                 ret);
            continue;
        }
        if (ret < 0) {
            LOGE("usb avcodec_send_packet failed: %d", ret);
            return APP_ERR_FFMPEG;
        }

        ret = avcodec_receive_frame(ctx->decoder.dec_ctx, frame);
        if (ret == 0) {
            int64_t decode_us = app_get_time_us() - decode_start_us;

            ctx->decode_timing_frames++;
            ctx->decode_timing_sum_us += decode_us;
            if (decode_us > ctx->decode_timing_max_us) {
                ctx->decode_timing_max_us = decode_us;
            }
            if (input_usb_frame_is_hw(frame)) {
                if (!ctx->retain_hw_frames) {
                    return input_usb_transfer_hw_frame(ctx, frame);
                }
                input_usb_log_hw_frame(ctx, frame);
            }
            return APP_OK;
        }
        if (ret == AVERROR(EAGAIN)) {
            continue;
        }
        if (ret == AVERROR_EOF) {
            return APP_ERR_EOF;
        }

        LOGE("usb avcodec_receive_frame failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    if (ret == AVERROR(EAGAIN)) {
        /* Non-blocking V4L2 read: no complete frame is ready yet. */
        return APP_ERR_AGAIN;
    }
    if (ret == AVERROR(ENODEV) || ret == AVERROR(EIO) ||
        ret == AVERROR(EPIPE)) {
        LOGW("usb device disconnected while reading: %s error=%d",
             ctx->cfg.device,
             ret);
        return APP_ERR_IO;
    }
    if (ret == AVERROR_EOF) {
        return APP_ERR_EOF;
    }
    LOGE("usb av_read_frame failed for %s: %d", ctx->cfg.device, ret);
    return APP_ERR_FFMPEG;
}

static app_status_t input_usb_open_internal(input_usb_ctx_t *ctx,
                                            const video_input_config_t *cfg,
                                            int retain_hw_frames) {
    app_status_t status;

    if (ctx == NULL || cfg == NULL) {
        return APP_ERR_PARAM;
    }

    *ctx = (input_usb_ctx_t){0};
    ctx->cfg = *cfg;
    ctx->retain_hw_frames = retain_hw_frames != 0;
    LOGD("open usb input: %s %dx%d@%d fmt=%s hw_frames=%s",
         ctx->cfg.device,
         ctx->cfg.width,
         ctx->cfg.height,
         ctx->cfg.fps,
         ctx->cfg.input_format,
         ctx->retain_hw_frames ? "retain" : "download");
    status = input_usb_open_device(ctx);
    if (status != APP_OK) {
        input_usb_close(ctx);
        return status;
    }
    ctx->is_opened = 1;
    return APP_OK;
}

app_status_t input_usb_open(input_usb_ctx_t *ctx, const video_input_config_t *cfg) {
    return input_usb_open_internal(ctx, cfg, 0);
}

app_status_t input_usb_open_hw(input_usb_ctx_t *ctx, const video_input_config_t *cfg) {
    return input_usb_open_internal(ctx, cfg, 1);
}

app_status_t input_usb_read(input_usb_ctx_t *ctx, video_frame_t *frame) {
    app_status_t status;

    if (ctx == NULL || frame == NULL || frame->av_frame == NULL) {
        return APP_ERR_PARAM;
    }
    if (!ctx->is_opened) {
        return APP_ERR_IO;
    }

    status = input_usb_read_decoder(ctx, frame->av_frame);
    if (status != APP_OK) {
        return status;
    }

    LOGT("usb decoded frame: %dx%d fmt=%d",
         frame->av_frame->width,
         frame->av_frame->height,
         frame->av_frame->format);

    frame->pts_us = app_get_time_us();
    frame->source_type = VIDEO_SOURCE_USB;
    strncpy(frame->source_name, ctx->cfg.name, sizeof(frame->source_name) - 1);
    frame->source_name[sizeof(frame->source_name) - 1] = '\0';
    return APP_OK;
}

void input_usb_close(input_usb_ctx_t *ctx) {
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
    if (ctx->sw_frame != NULL) {
        av_frame_free(&ctx->sw_frame);
    }
    memset(&ctx->decoder, 0, sizeof(ctx->decoder));
    ctx->is_opened = 0;
}
