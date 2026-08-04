#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "usb_mjpeg_hw_preview.c"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include "common/common.h"
#include "common/debug.h"
#include "output/output_display_gui.h"

typedef struct {
    const char *device;
    const char *input_format;
    int width;
    int height;
    int fps;
    int use_hw_decoder;
} usb_preview_config_t;

typedef struct {
    AVFormatContext *fmt_ctx;
    AVCodecContext *dec_ctx;
    AVPacket *packet;
    AVFrame *decode_frame;
    AVFrame *sw_frame;
    AVFrame *gui_frame;
    struct SwsContext *gui_sws;
    int video_stream_index;
    uint64_t decoded_frame_count;
} usb_preview_ctx_t;

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_stop_signal(int signo) {
    (void)signo;
    g_stop_requested = 1;
}

static const char *pix_fmt_name(enum AVPixelFormat fmt) {
    const char *name = av_get_pix_fmt_name(fmt);
    return name != NULL ? name : "unknown";
}

static void log_frame_sample(const char *tag, const AVFrame *frame) {
    unsigned int sum = 0;
    int count = 0;
    int x;
    int y;

    if (frame == NULL || frame->data[0] == NULL) {
        LOGW("%s frame sample unavailable", tag);
        return;
    }

    for (y = 0; y < frame->height && y < 8; ++y) {
        for (x = 0; x < frame->width && x < 32; ++x) {
            sum += frame->data[0][y * frame->linesize[0] + x];
            ++count;
        }
    }

    LOGI("%s sample sum=%u count=%d first_bytes=%u,%u,%u,%u fmt=%s linesize0=%d",
         tag,
         sum,
         count,
         frame->data[0][0],
         frame->data[0][1],
         frame->data[0][2],
         frame->data[0][3],
         pix_fmt_name((enum AVPixelFormat)frame->format),
         frame->linesize[0]);
}

static void save_frame_luma_pgm(const char *path, const AVFrame *frame) {
    FILE *fp;
    int y;

    if (frame == NULL || frame->data[0] == NULL) {
        return;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        LOGW("save_frame_luma_pgm open failed: %s", path);
        return;
    }

    fprintf(fp, "P5\n%d %d\n255\n", frame->width, frame->height);
    for (y = 0; y < frame->height; ++y) {
        fwrite(frame->data[0] + y * frame->linesize[0], 1, (size_t)frame->width, fp);
    }
    fclose(fp);
    LOGI("saved frame luma to %s", path);
}

static int frame_needs_transfer(const AVFrame *frame) {
    if (frame == NULL) {
        return 0;
    }
    if (frame->hw_frames_ctx != NULL) {
        return 1;
    }
#ifdef AV_PIX_FMT_DRM_PRIME
    if (frame->format == AV_PIX_FMT_DRM_PRIME) {
        return 1;
    }
#endif
    return 0;
}

static const AVCodec *find_mjpeg_decoder(int use_hw_decoder) {
    const AVCodec *decoder = NULL;

    if (use_hw_decoder) {
        decoder = avcodec_find_decoder_by_name("mjpeg_rkmpp");
        if (decoder != NULL) {
            LOGI("usb hw preview decoder select: %s", decoder->name);
            return decoder;
        }
        LOGW("usb hw preview decoder mjpeg_rkmpp not found, fallback to software");
    }

    decoder = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (decoder != NULL) {
        LOGI("usb hw preview decoder select: %s", decoder->name);
    }
    return decoder;
}

static app_status_t usb_preview_open(usb_preview_ctx_t *ctx, const usb_preview_config_t *cfg) {
    const AVInputFormat *input_fmt;
    const AVCodec *decoder;
    AVDictionary *options = NULL;
    char video_size[32];
    char fps_text[16];
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
    snprintf(fps_text, sizeof(fps_text), "%d", cfg->fps);
    av_dict_set(&options, "video_size", video_size, 0);
    av_dict_set(&options, "framerate", fps_text, 0);
    av_dict_set(&options, "input_format", cfg->input_format, 0);

    ret = avformat_open_input(&ctx->fmt_ctx, cfg->device, (AVInputFormat *)input_fmt, &options);
    av_dict_free(&options);
    if (ret < 0) {
        LOGE("usb hw preview avformat_open_input failed for %s: %d", cfg->device, ret);
        return APP_ERR_FFMPEG;
    }

    ret = avformat_find_stream_info(ctx->fmt_ctx, NULL);
    if (ret < 0) {
        LOGE("usb hw preview avformat_find_stream_info failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    ret = av_find_best_stream(ctx->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret < 0) {
        LOGE("usb hw preview av_find_best_stream failed: %d", ret);
        return APP_ERR_FFMPEG;
    }
    ctx->video_stream_index = ret;

    decoder = find_mjpeg_decoder(cfg->use_hw_decoder);
    if (decoder == NULL) {
        LOGE("usb hw preview decoder not found");
        return APP_ERR_UNSUPPORTED;
    }

    ctx->dec_ctx = avcodec_alloc_context3(decoder);
    if (ctx->dec_ctx == NULL) {
        return APP_ERR_NOMEM;
    }

    ret = avcodec_parameters_to_context(ctx->dec_ctx, ctx->fmt_ctx->streams[ctx->video_stream_index]->codecpar);
    if (ret < 0) {
        LOGE("usb hw preview avcodec_parameters_to_context failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    ret = avcodec_open2(ctx->dec_ctx, decoder, NULL);
    if (ret < 0) {
        LOGE("usb hw preview avcodec_open2 failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    ctx->packet = av_packet_alloc();
    ctx->decode_frame = av_frame_alloc();
    ctx->sw_frame = av_frame_alloc();
    ctx->gui_frame = av_frame_alloc();
    if (ctx->packet == NULL || ctx->decode_frame == NULL ||
        ctx->sw_frame == NULL || ctx->gui_frame == NULL) {
        return APP_ERR_NOMEM;
    }

    return APP_OK;
}

static void usb_preview_close(usb_preview_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    if (ctx->packet != NULL) {
        av_packet_free(&ctx->packet);
    }
    if (ctx->decode_frame != NULL) {
        av_frame_free(&ctx->decode_frame);
    }
    if (ctx->sw_frame != NULL) {
        av_frame_free(&ctx->sw_frame);
    }
    if (ctx->gui_frame != NULL) {
        av_frame_free(&ctx->gui_frame);
    }
    sws_freeContext(ctx->gui_sws);
    if (ctx->dec_ctx != NULL) {
        avcodec_free_context(&ctx->dec_ctx);
    }
    if (ctx->fmt_ctx != NULL) {
        avformat_close_input(&ctx->fmt_ctx);
    }
    memset(ctx, 0, sizeof(*ctx));
}

static app_status_t usb_preview_receive_frame(usb_preview_ctx_t *ctx, AVFrame **out_frame) {
    int ret;

    if (ctx == NULL || out_frame == NULL) {
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
            LOGE("usb hw preview avcodec_send_packet failed: %d", ret);
            return APP_ERR_FFMPEG;
        }

        av_frame_unref(ctx->decode_frame);
        ret = avcodec_receive_frame(ctx->dec_ctx, ctx->decode_frame);
        if (ret == AVERROR(EAGAIN)) {
            continue;
        }
        if (ret == AVERROR_EOF) {
            return APP_ERR_EOF;
        }
        if (ret < 0) {
            LOGE("usb hw preview avcodec_receive_frame failed: %d", ret);
            return APP_ERR_FFMPEG;
        }

        ++ctx->decoded_frame_count;
        if (ctx->decoded_frame_count == 1 || ctx->decoded_frame_count % 120 == 0) {
            LOGD("usb hw preview frame=%llu: %dx%d fmt=%s hw_ctx=%p",
                 (unsigned long long)ctx->decoded_frame_count,
                 ctx->decode_frame->width,
                 ctx->decode_frame->height,
                 pix_fmt_name((enum AVPixelFormat)ctx->decode_frame->format),
                 (void *)ctx->decode_frame->hw_frames_ctx);
        }

        if (!frame_needs_transfer(ctx->decode_frame)) {
            if (ctx->decoded_frame_count == 1 || ctx->decoded_frame_count % 120 == 0) {
                log_frame_sample("usb hw preview sw frame", ctx->decode_frame);
            }
            *out_frame = ctx->decode_frame;
            return APP_OK;
        }

        av_frame_unref(ctx->sw_frame);
        ret = av_hwframe_transfer_data(ctx->sw_frame, ctx->decode_frame, 0);
        if (ret < 0) {
            LOGE("usb hw preview av_hwframe_transfer_data failed: %d src_fmt=%s",
                 ret,
                 pix_fmt_name((enum AVPixelFormat)ctx->decode_frame->format));
            return APP_ERR_FFMPEG;
        }

        ret = av_frame_copy_props(ctx->sw_frame, ctx->decode_frame);
        if (ret < 0) {
            LOGE("usb hw preview av_frame_copy_props failed: %d", ret);
            return APP_ERR_FFMPEG;
        }

        if (ctx->decoded_frame_count == 1 || ctx->decoded_frame_count % 120 == 0) {
            LOGD("usb hw preview transferred frame: %dx%d fmt=%s",
                 ctx->sw_frame->width,
                 ctx->sw_frame->height,
                 pix_fmt_name((enum AVPixelFormat)ctx->sw_frame->format));
            log_frame_sample("usb hw preview transferred", ctx->sw_frame);
        }
        *out_frame = ctx->sw_frame;
        return APP_OK;
    }

    return APP_ERR_EOF;
}

static app_status_t usb_preview_prepare_gui_frame(usb_preview_ctx_t *ctx,
                                                  const AVFrame *decoded,
                                                  AVFrame **out_frame) {
    int scaled_height;
    int ret;

    if (ctx == NULL || decoded == NULL || out_frame == NULL) {
        return APP_ERR_PARAM;
    }

    if (decoded->data[0] == NULL || decoded->width <= 0 || decoded->height <= 0) {
        LOGE("usb hw preview decoded frame has no accessible image data");
        return APP_ERR_FFMPEG;
    }

    if (ctx->gui_frame->width != decoded->width ||
        ctx->gui_frame->height != decoded->height ||
        ctx->gui_frame->format != AV_PIX_FMT_YUV420P ||
        ctx->gui_frame->data[0] == NULL) {
        av_frame_unref(ctx->gui_frame);
        ctx->gui_frame->format = AV_PIX_FMT_YUV420P;
        ctx->gui_frame->width = decoded->width;
        ctx->gui_frame->height = decoded->height;

        ret = av_frame_get_buffer(ctx->gui_frame, 32);
        if (ret < 0) {
            LOGE("usb hw preview gui frame allocation failed: %d", ret);
            return APP_ERR_NOMEM;
        }
    }

    ret = av_frame_make_writable(ctx->gui_frame);
    if (ret < 0) {
        LOGE("usb hw preview gui frame is not writable: %d", ret);
        return APP_ERR_FFMPEG;
    }

    ctx->gui_sws = sws_getCachedContext(ctx->gui_sws,
                                        decoded->width,
                                        decoded->height,
                                        (enum AVPixelFormat)decoded->format,
                                        decoded->width,
                                        decoded->height,
                                        AV_PIX_FMT_YUV420P,
                                        SWS_FAST_BILINEAR,
                                        NULL,
                                        NULL,
                                        NULL);
    if (ctx->gui_sws == NULL) {
        LOGE("usb hw preview cannot convert %s to yuv420p",
             pix_fmt_name((enum AVPixelFormat)decoded->format));
        return APP_ERR_FFMPEG;
    }

    scaled_height = sws_scale(ctx->gui_sws,
                              (const uint8_t * const *)decoded->data,
                              decoded->linesize,
                              0,
                              decoded->height,
                              ctx->gui_frame->data,
                              ctx->gui_frame->linesize);
    if (scaled_height != decoded->height) {
        LOGE("usb hw preview color conversion failed: output rows=%d expected=%d",
             scaled_height,
             decoded->height);
        return APP_ERR_FFMPEG;
    }

    ctx->gui_frame->pts = decoded->pts;
    ctx->gui_frame->sample_aspect_ratio = decoded->sample_aspect_ratio;
    ctx->gui_frame->color_range = AVCOL_RANGE_MPEG;
    ctx->gui_frame->colorspace = decoded->colorspace;
    ctx->gui_frame->color_primaries = decoded->color_primaries;
    ctx->gui_frame->color_trc = decoded->color_trc;
    ctx->gui_frame->chroma_location = decoded->chroma_location;

    *out_frame = ctx->gui_frame;
    return APP_OK;
}

int main(int argc, char **argv) {
    usb_preview_config_t cfg = {
        .device = "/dev/video41",
        .input_format = "mjpeg",
        .width = 1280,
        .height = 720,
        .fps = 30,
        .use_hw_decoder = 1
    };
    usb_preview_ctx_t preview;
    output_display_gui_ctx_t gui;
    video_frame_t frame;
    app_status_t status;
    AVFrame *decoded = NULL;
    AVFrame *gui_frame = NULL;
    int dumped_once = 0;
    int conversion_logged = 0;

    if (argc > 1) {
        cfg.device = argv[1];
    }
    if (argc > 2) {
        cfg.width = atoi(argv[2]);
    }
    if (argc > 3) {
        cfg.height = atoi(argv[3]);
    }
    if (argc > 4) {
        cfg.fps = atoi(argv[4]);
    }

    signal(SIGINT, handle_stop_signal);
    signal(SIGTERM, handle_stop_signal);

    avdevice_register_all();
    avformat_network_init();

    LOGI("usb hw preview start device=%s %dx%d@%d fmt=%s",
         cfg.device,
         cfg.width,
         cfg.height,
         cfg.fps,
         cfg.input_format);

    status = usb_preview_open(&preview, &cfg);
    if (status != APP_OK) {
        LOGE("usb_preview_open failed: %s", app_status_str(status));
        avformat_network_deinit();
        return EXIT_FAILURE;
    }

    status = output_display_gui_init(&gui, 1, 1280, 720);
    if (status != APP_OK) {
        LOGE("output_display_gui_init failed: %s", app_status_str(status));
        usb_preview_close(&preview);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }

    memset(&frame, 0, sizeof(frame));
    while (!g_stop_requested) {
        if (output_display_gui_poll_quit(&gui)) {
            break;
        }

        status = usb_preview_receive_frame(&preview, &decoded);
        if (status == APP_ERR_EOF) {
            usleep(10000);
            continue;
        }
        if (status != APP_OK) {
            LOGW("usb_preview_receive_frame failed: %s", app_status_str(status));
            usleep(10000);
            continue;
        }

        status = usb_preview_prepare_gui_frame(&preview, decoded, &gui_frame);
        if (status != APP_OK) {
            LOGE("usb_preview_prepare_gui_frame failed: %s", app_status_str(status));
            break;
        }

        if (!conversion_logged) {
            LOGI("usb hw preview display conversion: decoder=%s output=%s -> gui=yuv420p",
                 preview.dec_ctx->codec->name,
                 pix_fmt_name((enum AVPixelFormat)decoded->format));
            conversion_logged = 1;
        }

        frame.av_frame = gui_frame;
        frame.pts_us = app_get_time_us();
        frame.source_type = VIDEO_SOURCE_USB;
        snprintf(frame.source_name, sizeof(frame.source_name), "%s", "usb-hw");

        if (!dumped_once) {
            save_frame_luma_pgm("/tmp/usb_hw_preview_y.pgm", gui_frame);
            dumped_once = 1;
        }

        status = output_display_gui_show(&gui, &frame);
        if (status != APP_OK) {
            LOGE("output_display_gui_show failed: %s", app_status_str(status));
            break;
        }
    }

    output_display_gui_deinit(&gui);
    usb_preview_close(&preview);
    avformat_network_deinit();
    return EXIT_SUCCESS;
}
