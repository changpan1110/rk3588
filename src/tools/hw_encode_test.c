#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "hw_encode_test.c"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/dict.h>

#include "common/common.h"
#include "common/debug.h"
#include "input/input_csi0.h"
#include "input/input_csi1.h"
#include "input/input_hdmi_in.h"
#include "input/input_usb.h"
#include "output/output_encode_common.h"

#define HW_ENCODE_TEST_DEFAULT_DEVICE "/dev/video40"
#define HW_ENCODE_TEST_DEFAULT_FORMAT ""
#define HW_ENCODE_TEST_DEFAULT_WIDTH 3840
#define HW_ENCODE_TEST_DEFAULT_HEIGHT 2160
#define HW_ENCODE_TEST_DEFAULT_FPS 30
#define HW_ENCODE_TEST_DEFAULT_OUTPUT "/tmp/hdmi_4k.h264"
#define HW_ENCODE_TEST_DEFAULT_DURATION_SEC 10
#define HW_ENCODE_TEST_QUEUE_SIZE 8

typedef struct {
    video_source_type_t source_type;
    video_input_config_t cfg;
    const char *output_path;
    const char *encoder_name;
    int duration_sec;
    int bitrate;
    int gop;
} hw_encode_test_config_t;

typedef struct {
    hw_encode_test_config_t cfg;
    union {
        input_usb_ctx_t usb;
        input_csi0_ctx_t csi0;
        input_csi1_ctx_t csi1;
        input_hdmi_in_ctx_t hdmi_in;
    } input;
    output_encode_convert_ctx_t nv12_convert;
    const AVCodec *codec;
    AVCodecContext *codec_ctx;
    AVPacket *packet;
    FILE *fp;
    int64_t last_progress_us;

    pthread_mutex_t queue_lock;
    pthread_cond_t queue_cond;
    int queue_initialized;
    AVFrame *queue[HW_ENCODE_TEST_QUEUE_SIZE];
    int queue_head;
    int queue_count;
    int queue_max_count;
    int queue_dropped;

    pthread_t encode_thread;
    int encode_thread_started;
    int encode_stop;
    app_status_t encode_status;
    int encoded_frames;
    int encoded_packets;
    int64_t total_encode_us;
    int measured_encode_calls;
} hw_encode_test_ctx_t;

static app_status_t hw_encode_test_queue_init(hw_encode_test_ctx_t *ctx) {
    if (pthread_mutex_init(&ctx->queue_lock, NULL) != 0) {
        return APP_ERR_IO;
    }
    if (pthread_cond_init(&ctx->queue_cond, NULL) != 0) {
        pthread_mutex_destroy(&ctx->queue_lock);
        return APP_ERR_IO;
    }
    ctx->queue_initialized = 1;
    ctx->encode_status = APP_OK;
    return APP_OK;
}

static app_status_t hw_encode_test_queue_push(hw_encode_test_ctx_t *ctx,
                                              const AVFrame *frame) {
    AVFrame *slot_frame;
    int slot;
    int ret;

    pthread_mutex_lock(&ctx->queue_lock);
    if (ctx->encode_status != APP_OK || ctx->encode_stop) {
        app_status_t status = ctx->encode_status != APP_OK
                                  ? ctx->encode_status
                                  : APP_ERR_BUSY;
        pthread_mutex_unlock(&ctx->queue_lock);
        return status;
    }

    if (ctx->queue_count == HW_ENCODE_TEST_QUEUE_SIZE) {
        slot_frame = ctx->queue[ctx->queue_head];
        if (slot_frame != NULL) {
            av_frame_unref(slot_frame);
        }
        ctx->queue_head = (ctx->queue_head + 1) % HW_ENCODE_TEST_QUEUE_SIZE;
        ctx->queue_count--;
        ctx->queue_dropped++;
    }

    slot = (ctx->queue_head + ctx->queue_count) % HW_ENCODE_TEST_QUEUE_SIZE;
    if (ctx->queue[slot] == NULL) {
        ctx->queue[slot] = av_frame_alloc();
        if (ctx->queue[slot] == NULL) {
            pthread_mutex_unlock(&ctx->queue_lock);
            return APP_ERR_NOMEM;
        }
    } else {
        av_frame_unref(ctx->queue[slot]);
    }

    ret = av_frame_ref(ctx->queue[slot], frame);
    if (ret < 0) {
        pthread_mutex_unlock(&ctx->queue_lock);
        return APP_ERR_FFMPEG;
    }
    ctx->queue_count++;
    if (ctx->queue_count > ctx->queue_max_count) {
        ctx->queue_max_count = ctx->queue_count;
    }
    pthread_cond_signal(&ctx->queue_cond);
    pthread_mutex_unlock(&ctx->queue_lock);
    return APP_OK;
}

static int hw_encode_test_queue_pop_locked(hw_encode_test_ctx_t *ctx,
                                           AVFrame *frame) {
    if (ctx->queue_count == 0) {
        return 0;
    }
    av_frame_unref(frame);
    if (av_frame_ref(frame, ctx->queue[ctx->queue_head]) < 0) {
        return 0;
    }
    av_frame_unref(ctx->queue[ctx->queue_head]);
    ctx->queue_head = (ctx->queue_head + 1) % HW_ENCODE_TEST_QUEUE_SIZE;
    ctx->queue_count--;
    return 1;
}

static void hw_encode_test_stop_encode_thread(hw_encode_test_ctx_t *ctx) {
    if (ctx == NULL || !ctx->encode_thread_started) {
        return;
    }
    pthread_mutex_lock(&ctx->queue_lock);
    ctx->encode_stop = 1;
    pthread_cond_broadcast(&ctx->queue_cond);
    pthread_mutex_unlock(&ctx->queue_lock);
    pthread_join(ctx->encode_thread, NULL);
    ctx->encode_thread_started = 0;
}

static void hw_encode_test_queue_deinit(hw_encode_test_ctx_t *ctx) {
    int i;

    if (ctx == NULL || !ctx->queue_initialized) {
        return;
    }
    for (i = 0; i < HW_ENCODE_TEST_QUEUE_SIZE; ++i) {
        if (ctx->queue[i] != NULL) {
            av_frame_free(&ctx->queue[i]);
        }
    }
    pthread_cond_destroy(&ctx->queue_cond);
    pthread_mutex_destroy(&ctx->queue_lock);
    ctx->queue_initialized = 0;
}

static void hw_encode_test_print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [<usb|csi0|csi1|hdmi_in> <device> <format> <width> <height> <fps> <output.h264> [seconds]]\n"
            "Default:\n"
            "  %s -> hdmi_in /dev/video40 3840x2160@30 /tmp/hdmi_4k.h264 10s\n"
            "Examples:\n"
            "  %s usb /dev/video41 mjpeg 1280 720 30 /tmp/usb0.h264 10\n"
            "  %s hdmi_in /dev/video40 \"\" 0 0 0 /tmp/hdmi.h264 10\n",
            prog, prog, prog, prog);
}

static const char *hw_encode_test_source_name(video_source_type_t source_type) {
    switch (source_type) {
        case VIDEO_SOURCE_USB: return "usb";
        case VIDEO_SOURCE_CSI0: return "csi0";
        case VIDEO_SOURCE_CSI1: return "csi1";
        case VIDEO_SOURCE_HDMI_IN: return "hdmi_in";
        default: return "unknown";
    }
}

static void hw_encode_test_print_progress(const hw_encode_test_ctx_t *ctx,
                                          int encoded_frames,
                                          int64_t elapsed_us) {
    if (ctx == NULL) {
        return;
    }

    printf("\r\033[Ksource=%s target=%ds elapsed=%.2fs frames=%d avg_fps=%.2f output=%s",
           hw_encode_test_source_name(ctx->cfg.source_type),
           ctx->cfg.duration_sec,
           (double)elapsed_us / 1000000.0,
           encoded_frames,
           elapsed_us > 0 ? ((double)encoded_frames * 1000000.0 / (double)elapsed_us) : 0.0,
           ctx->cfg.output_path);
    fflush(stdout);
}

static app_status_t hw_encode_test_parse_args(hw_encode_test_config_t *cfg, int argc, char **argv) {
    if (cfg == NULL || argv == NULL) {
        return APP_ERR_PARAM;
    }
    if (argc != 1 && argc != 8 && argc != 9) {
        hw_encode_test_print_usage(argv[0]);
        return APP_ERR_PARAM;
    }

    memset(cfg, 0, sizeof(*cfg));
    if (argc == 1) {
        cfg->source_type = VIDEO_SOURCE_HDMI_IN;
        strncpy(cfg->cfg.name, "hdmi_in0", sizeof(cfg->cfg.name) - 1);
        strncpy(cfg->cfg.device, HW_ENCODE_TEST_DEFAULT_DEVICE, sizeof(cfg->cfg.device) - 1);
        strncpy(cfg->cfg.input_format, HW_ENCODE_TEST_DEFAULT_FORMAT, sizeof(cfg->cfg.input_format) - 1);
        cfg->cfg.width = HW_ENCODE_TEST_DEFAULT_WIDTH;
        cfg->cfg.height = HW_ENCODE_TEST_DEFAULT_HEIGHT;
        cfg->cfg.fps = HW_ENCODE_TEST_DEFAULT_FPS;
        cfg->cfg.source_type = cfg->source_type;
        cfg->output_path = HW_ENCODE_TEST_DEFAULT_OUTPUT;
        cfg->duration_sec = HW_ENCODE_TEST_DEFAULT_DURATION_SEC;
        cfg->bitrate = 16000000;
        cfg->gop = cfg->cfg.fps;
        cfg->encoder_name = "h264_rkmpp";
        return APP_OK;
    }

    if (strcmp(argv[1], "usb") == 0) {
        cfg->source_type = VIDEO_SOURCE_USB;
        strncpy(cfg->cfg.name, "usb0", sizeof(cfg->cfg.name) - 1);
    } else if (strcmp(argv[1], "csi0") == 0) {
        cfg->source_type = VIDEO_SOURCE_CSI0;
        strncpy(cfg->cfg.name, "csi0", sizeof(cfg->cfg.name) - 1);
    } else if (strcmp(argv[1], "csi1") == 0) {
        cfg->source_type = VIDEO_SOURCE_CSI1;
        strncpy(cfg->cfg.name, "csi1", sizeof(cfg->cfg.name) - 1);
    } else if (strcmp(argv[1], "hdmi_in") == 0) {
        cfg->source_type = VIDEO_SOURCE_HDMI_IN;
        strncpy(cfg->cfg.name, "hdmi_in0", sizeof(cfg->cfg.name) - 1);
    } else {
        hw_encode_test_print_usage(argv[0]);
        return APP_ERR_PARAM;
    }

    strncpy(cfg->cfg.device, argv[2], sizeof(cfg->cfg.device) - 1);
    strncpy(cfg->cfg.input_format, argv[3], sizeof(cfg->cfg.input_format) - 1);
    cfg->cfg.width = atoi(argv[4]);
    cfg->cfg.height = atoi(argv[5]);
    cfg->cfg.fps = atoi(argv[6]);
    cfg->cfg.source_type = cfg->source_type;
    cfg->output_path = argv[7];
    cfg->duration_sec = (argc == 9) ? atoi(argv[8]) : 10;
    if (cfg->duration_sec <= 0) {
        cfg->duration_sec = 10;
    }
    cfg->bitrate = cfg->source_type == VIDEO_SOURCE_HDMI_IN ? 16000000 : 4000000;
    cfg->gop = cfg->cfg.fps > 0 ? cfg->cfg.fps : 30;
    cfg->encoder_name = "h264_rkmpp";
    return APP_OK;
}

static app_status_t hw_encode_test_open_input(hw_encode_test_ctx_t *ctx) {
    switch (ctx->cfg.source_type) {
        case VIDEO_SOURCE_USB:
            return input_usb_open_hw(&ctx->input.usb, &ctx->cfg.cfg);
        case VIDEO_SOURCE_CSI0:
            return input_csi0_open(&ctx->input.csi0, &ctx->cfg.cfg);
        case VIDEO_SOURCE_CSI1:
            return input_csi1_open(&ctx->input.csi1, &ctx->cfg.cfg);
        case VIDEO_SOURCE_HDMI_IN:
            return input_hdmi_in_open(&ctx->input.hdmi_in, &ctx->cfg.cfg);
        default:
            return APP_ERR_PARAM;
    }
}

static app_status_t hw_encode_test_read_input(hw_encode_test_ctx_t *ctx, video_frame_t *frame) {
    switch (ctx->cfg.source_type) {
        case VIDEO_SOURCE_USB:
            return input_usb_read(&ctx->input.usb, frame);
        case VIDEO_SOURCE_CSI0:
            return input_csi0_read(&ctx->input.csi0, frame);
        case VIDEO_SOURCE_CSI1:
            return input_csi1_read(&ctx->input.csi1, frame);
        case VIDEO_SOURCE_HDMI_IN:
            return input_hdmi_in_read(&ctx->input.hdmi_in, frame);
        default:
            return APP_ERR_PARAM;
    }
}

static void hw_encode_test_close_input(hw_encode_test_ctx_t *ctx) {
    switch (ctx->cfg.source_type) {
        case VIDEO_SOURCE_USB:
            input_usb_close(&ctx->input.usb);
            break;
        case VIDEO_SOURCE_CSI0:
            input_csi0_close(&ctx->input.csi0);
            break;
        case VIDEO_SOURCE_CSI1:
            input_csi1_close(&ctx->input.csi1);
            break;
        case VIDEO_SOURCE_HDMI_IN:
            input_hdmi_in_close(&ctx->input.hdmi_in);
            break;
        default:
            break;
    }
}

static const AVCodec *hw_encode_test_find_encoder(const char *preferred_name) {
    const AVCodec *codec;

    codec = avcodec_find_encoder_by_name(preferred_name);
    if (codec != NULL) {
        LOGI("selected hardware encoder: %s", preferred_name);
        return codec;
    }

    codec = avcodec_find_encoder_by_name("h264_v4l2m2m");
    if (codec != NULL) {
        LOGW("encoder %s not found, fallback to h264_v4l2m2m", preferred_name);
        return codec;
    }

    return NULL;
}

static app_status_t hw_encode_test_open_encoder(hw_encode_test_ctx_t *ctx, int width, int height, int fps) {
    AVDictionary *opts = NULL;
    int ret;

    ctx->codec = hw_encode_test_find_encoder(ctx->cfg.encoder_name);
    if (ctx->codec == NULL) {
        LOGE("hardware encoder not found: %s", ctx->cfg.encoder_name);
        return APP_ERR_UNSUPPORTED;
    }

    ctx->codec_ctx = avcodec_alloc_context3(ctx->codec);
    if (ctx->codec_ctx == NULL) {
        return APP_ERR_NOMEM;
    }

    ctx->codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    ctx->codec_ctx->codec_id = AV_CODEC_ID_H264;
    ctx->codec_ctx->width = width;
    ctx->codec_ctx->height = height;
    ctx->codec_ctx->pix_fmt = AV_PIX_FMT_NV12;
    ctx->codec_ctx->time_base = (AVRational){1, 1000000};
    ctx->codec_ctx->framerate = (AVRational){fps > 0 ? fps : 30, 1};
    ctx->codec_ctx->bit_rate = ctx->cfg.bitrate;
    ctx->codec_ctx->gop_size = ctx->cfg.gop;
    ctx->codec_ctx->max_b_frames = 0;

    if (strcmp(ctx->codec->name, "h264_rkmpp") == 0) {
        av_dict_set(&opts, "rc_mode", "CBR", 0);
    }
    ret = avcodec_open2(ctx->codec_ctx, ctx->codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        LOGE("avcodec_open2 failed for %s: %d", ctx->codec->name, ret);
        return APP_ERR_FFMPEG;
    }

    ctx->packet = av_packet_alloc();
    if (ctx->packet == NULL) {
        return APP_ERR_NOMEM;
    }

    LOGI("encoder opened name=%s %dx%d@%d bitrate=%d",
         ctx->codec->name,
         width,
         height,
         fps > 0 ? fps : 30,
         ctx->cfg.bitrate);
    return APP_OK;
}

static app_status_t hw_encode_test_write_packets(hw_encode_test_ctx_t *ctx, int *packet_count) {
    int ret;

    if (ctx == NULL || ctx->packet == NULL || ctx->fp == NULL) {
        return APP_ERR_PARAM;
    }

    while ((ret = avcodec_receive_packet(ctx->codec_ctx, ctx->packet)) >= 0) {
        if (fwrite(ctx->packet->data, 1, ctx->packet->size, ctx->fp) != (size_t)ctx->packet->size) {
            LOGE("write packet failed");
            av_packet_unref(ctx->packet);
            return APP_ERR_IO;
        }
        if (packet_count != NULL) {
            (*packet_count)++;
        }
        LOGT("encoded packet size=%d key=%d pts=%lld",
             ctx->packet->size,
             (ctx->packet->flags & AV_PKT_FLAG_KEY) != 0,
             (long long)ctx->packet->pts);
        av_packet_unref(ctx->packet);
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return APP_OK;
    }

    LOGE("avcodec_receive_packet failed: %d", ret);
    return APP_ERR_FFMPEG;
}

static app_status_t hw_encode_test_encode_frame(hw_encode_test_ctx_t *ctx, const AVFrame *src_frame, int *packet_count) {
    const AVFrame *encode_frame;
    int converted = 0;
    int ret;

    if (ctx == NULL || src_frame == NULL) {
        return APP_ERR_PARAM;
    }

    if (ctx->nv12_convert.dst_width != src_frame->width ||
        ctx->nv12_convert.dst_height != src_frame->height ||
        ctx->nv12_convert.dst_fmt != AV_PIX_FMT_NV12) {
        output_encode_convert_deinit(&ctx->nv12_convert);
        ret = output_encode_convert_init(&ctx->nv12_convert,
                                         src_frame->width,
                                         src_frame->height,
                                         AV_PIX_FMT_NV12);
        if (ret != APP_OK) {
            return ret;
        }
    }

    encode_frame = output_encode_prepare_frame(&ctx->nv12_convert, src_frame, &converted);
    if (encode_frame == NULL) {
        LOGE("prepare nv12 frame failed src=%dx%d fmt=%d",
             src_frame->width,
             src_frame->height,
             src_frame->format);
        return APP_ERR_FFMPEG;
    }

    ret = avcodec_send_frame(ctx->codec_ctx, encode_frame);
    if (ret < 0) {
        LOGE("avcodec_send_frame failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    return hw_encode_test_write_packets(ctx, packet_count);
}

static void *hw_encode_test_encode_thread(void *opaque) {
    hw_encode_test_ctx_t *ctx = (hw_encode_test_ctx_t *)opaque;
    AVFrame *frame;
    app_status_t status;
    int64_t operation_started_us;

    frame = av_frame_alloc();
    if (frame == NULL) {
        pthread_mutex_lock(&ctx->queue_lock);
        ctx->encode_status = APP_ERR_NOMEM;
        ctx->encode_stop = 1;
        pthread_cond_broadcast(&ctx->queue_cond);
        pthread_mutex_unlock(&ctx->queue_lock);
        return NULL;
    }

    while (1) {
        pthread_mutex_lock(&ctx->queue_lock);
        while (ctx->queue_count == 0 && !ctx->encode_stop) {
            pthread_cond_wait(&ctx->queue_cond, &ctx->queue_lock);
        }
        if (ctx->queue_count == 0 && ctx->encode_stop) {
            pthread_mutex_unlock(&ctx->queue_lock);
            break;
        }
        if (!hw_encode_test_queue_pop_locked(ctx, frame)) {
            pthread_mutex_unlock(&ctx->queue_lock);
            continue;
        }
        pthread_mutex_unlock(&ctx->queue_lock);

        operation_started_us = app_get_time_us();
        status = hw_encode_test_encode_frame(ctx, frame, &ctx->encoded_packets);
        operation_started_us = app_get_time_us() - operation_started_us;

        pthread_mutex_lock(&ctx->queue_lock);
        ctx->total_encode_us += operation_started_us;
        ctx->measured_encode_calls++;
        if (status != APP_OK) {
            ctx->encode_status = status;
            ctx->encode_stop = 1;
            pthread_cond_broadcast(&ctx->queue_cond);
            pthread_mutex_unlock(&ctx->queue_lock);
            break;
        }
        ctx->encoded_frames++;
        pthread_mutex_unlock(&ctx->queue_lock);
    }

    av_frame_free(&frame);
    return NULL;
}

static app_status_t hw_encode_test_flush_encoder(hw_encode_test_ctx_t *ctx, int *packet_count) {
    int ret;

    ret = avcodec_send_frame(ctx->codec_ctx, NULL);
    if (ret < 0) {
        LOGE("avcodec_send_frame flush failed: %d", ret);
        return APP_ERR_FFMPEG;
    }
    return hw_encode_test_write_packets(ctx, packet_count);
}

static void hw_encode_test_cleanup(hw_encode_test_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    hw_encode_test_stop_encode_thread(ctx);
    hw_encode_test_close_input(ctx);
    if (ctx->packet != NULL) {
        av_packet_free(&ctx->packet);
    }
    if (ctx->codec_ctx != NULL) {
        avcodec_free_context(&ctx->codec_ctx);
    }
    output_encode_convert_deinit(&ctx->nv12_convert);
    if (ctx->fp != NULL) {
        fclose(ctx->fp);
    }
    hw_encode_test_queue_deinit(ctx);
    memset(&ctx->input, 0, sizeof(ctx->input));
}

int main(int argc, char **argv) {
    hw_encode_test_ctx_t ctx;
    video_frame_t frame;
    app_status_t status;
    int captured_frames = 0;
    int encoded_frames;
    int encoded_packets;
    int queue_dropped;
    int queue_max_count;
    int fps;
    int64_t start_us;
    int64_t elapsed_us = 0;
    int64_t frame_pts;
    int warmup_converted = 0;
    int warmup_packets = 0;
    int64_t warmup_started_us;
    int64_t operation_started_us;
    int64_t total_read_us = 0;
    int64_t total_encode_us;
    int measured_read_calls = 0;
    int measured_encode_calls;
    const AVFrame *warmup_frame;
    double read_avg_ms;
    double encode_avg_ms;
    double parallel_stage_ms;

    memset(&ctx, 0, sizeof(ctx));
    status = hw_encode_test_parse_args(&ctx.cfg, argc, argv);
    if (status != APP_OK) {
        return EXIT_FAILURE;
    }

#ifdef HAVE_LIBAVDEVICE
    extern void avdevice_register_all(void);
    avdevice_register_all();
#endif
    avformat_network_init();

    status = hw_encode_test_open_input(&ctx);
    if (status != APP_OK) {
        LOGE("open input failed source=%s status=%s",
             hw_encode_test_source_name(ctx.cfg.source_type),
             app_status_str(status));
        hw_encode_test_cleanup(&ctx);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }

    ctx.fp = fopen(ctx.cfg.output_path, "wb");
    if (ctx.fp == NULL) {
        LOGE("open output failed: %s", ctx.cfg.output_path);
        hw_encode_test_cleanup(&ctx);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }

    memset(&frame, 0, sizeof(frame));
    frame.av_frame = av_frame_alloc();
    if (frame.av_frame == NULL) {
        LOGE("av_frame_alloc failed");
        hw_encode_test_cleanup(&ctx);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }

    fps = ctx.cfg.cfg.fps > 0 ? ctx.cfg.cfg.fps : 30;

    status = hw_encode_test_read_input(&ctx, &frame);
    if (status != APP_OK) {
        LOGE("read first input frame failed: %s", app_status_str(status));
        av_frame_free(&frame.av_frame);
        hw_encode_test_cleanup(&ctx);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }
    status = hw_encode_test_open_encoder(&ctx,
                                         frame.av_frame->width,
                                         frame.av_frame->height,
                                         fps);
    if (status != APP_OK) {
        av_frame_free(&frame.av_frame);
        hw_encode_test_cleanup(&ctx);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }
    status = output_encode_convert_init(&ctx.nv12_convert,
                                        frame.av_frame->width,
                                        frame.av_frame->height,
                                        AV_PIX_FMT_NV12);
    if (status != APP_OK) {
        LOGE("initialize nv12 conversion failed: %s", app_status_str(status));
        av_frame_free(&frame.av_frame);
        hw_encode_test_cleanup(&ctx);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }

    warmup_frame = output_encode_prepare_frame(&ctx.nv12_convert,
                                               frame.av_frame,
                                               &warmup_converted);
    if (warmup_frame == NULL) {
        LOGE("warm up nv12 conversion failed");
        av_frame_free(&frame.av_frame);
        hw_encode_test_cleanup(&ctx);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }

    warmup_started_us = app_get_time_us();
    frame.av_frame->pts = 0;
    status = hw_encode_test_encode_frame(&ctx, frame.av_frame, &warmup_packets);
    if (status != APP_OK) {
        LOGE("warm up hardware encoder failed: %s", app_status_str(status));
        av_frame_free(&frame.av_frame);
        hw_encode_test_cleanup(&ctx);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }
    LOGI("input, conversion and encoder warmup complete in %.3f ms packets=%d; "
         "performance timer starts now",
         (double)(app_get_time_us() - warmup_started_us) / 1000.0,
         warmup_packets);

    status = hw_encode_test_queue_init(&ctx);
    if (status != APP_OK) {
        LOGE("initialize frame queue failed: %s", app_status_str(status));
        av_frame_free(&frame.av_frame);
        hw_encode_test_cleanup(&ctx);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }
    if (pthread_create(&ctx.encode_thread, NULL, hw_encode_test_encode_thread, &ctx) != 0) {
        LOGE("create encode thread failed");
        av_frame_free(&frame.av_frame);
        hw_encode_test_cleanup(&ctx);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }
    ctx.encode_thread_started = 1;
    LOGI("pipeline mode=parallel decode producer + RKRGA/MPP encode consumer queue=%d",
         HW_ENCODE_TEST_QUEUE_SIZE);

    start_us = app_get_time_us();
    while (1) {
        app_status_t encode_status;
        int progress_frames;

        elapsed_us = app_get_time_us() - start_us;
        if (elapsed_us >= (int64_t)ctx.cfg.duration_sec * 1000000LL) {
            break;
        }

        av_frame_unref(frame.av_frame);
        operation_started_us = app_get_time_us();
        status = hw_encode_test_read_input(&ctx, &frame);
        total_read_us += app_get_time_us() - operation_started_us;
        measured_read_calls++;
        if (status != APP_OK) {
            if (status != APP_ERR_EOF) {
                LOGW("read input failed: %s", app_status_str(status));
            }
            continue;
        }
        elapsed_us = app_get_time_us() - start_us;
        if (elapsed_us >= (int64_t)ctx.cfg.duration_sec * 1000000LL) {
            break;
        }

        frame_pts = elapsed_us;
        frame.av_frame->pts = frame_pts;
        status = hw_encode_test_queue_push(&ctx, frame.av_frame);
        if (status != APP_OK) {
            LOGE("queue frame failed: %s", app_status_str(status));
            break;
        }
        captured_frames++;

        pthread_mutex_lock(&ctx.queue_lock);
        progress_frames = ctx.encoded_frames;
        encode_status = ctx.encode_status;
        pthread_mutex_unlock(&ctx.queue_lock);
        if (encode_status != APP_OK) {
            LOGE("encode thread failed: %s", app_status_str(encode_status));
            break;
        }
        if (elapsed_us - ctx.last_progress_us >= 500000) {
            hw_encode_test_print_progress(&ctx, progress_frames, elapsed_us);
            ctx.last_progress_us = elapsed_us;
        }
    }

    elapsed_us = app_get_time_us() - start_us;
    hw_encode_test_stop_encode_thread(&ctx);

    pthread_mutex_lock(&ctx.queue_lock);
    status = ctx.encode_status;
    encoded_frames = ctx.encoded_frames;
    encoded_packets = ctx.encoded_packets;
    total_encode_us = ctx.total_encode_us;
    measured_encode_calls = ctx.measured_encode_calls;
    queue_dropped = ctx.queue_dropped;
    queue_max_count = ctx.queue_max_count;
    pthread_mutex_unlock(&ctx.queue_lock);
    if (status != APP_OK) {
        LOGE("encode thread failed: %s", app_status_str(status));
        av_frame_free(&frame.av_frame);
        hw_encode_test_cleanup(&ctx);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }

    status = hw_encode_test_flush_encoder(&ctx, &ctx.encoded_packets);
    if (status != APP_OK) {
        LOGW("flush encoder failed: %s", app_status_str(status));
    }
    encoded_packets = ctx.encoded_packets;

    hw_encode_test_print_progress(&ctx, encoded_frames, elapsed_us);
    printf("\n");
    LOGI("hw encode test done source=%s target=%.3fs actual=%.3fs captured=%d "
         "frames=%d packets=%d avg_fps=%.2f dropped=%d max_queue=%d output=%s",
         hw_encode_test_source_name(ctx.cfg.source_type),
         (double)ctx.cfg.duration_sec,
         (double)elapsed_us / 1000000.0,
         captured_frames,
         encoded_frames,
         encoded_packets,
         elapsed_us > 0 ? ((double)encoded_frames * 1000000.0 / (double)elapsed_us) : 0.0,
         queue_dropped,
         queue_max_count,
         ctx.cfg.output_path);
    read_avg_ms = measured_read_calls > 0
                      ? (double)total_read_us / (double)measured_read_calls / 1000.0
                      : 0.0;
    encode_avg_ms = measured_encode_calls > 0
                        ? (double)total_encode_us / (double)measured_encode_calls / 1000.0
                        : 0.0;
    parallel_stage_ms = read_avg_ms > encode_avg_ms ? read_avg_ms : encode_avg_ms;
    LOGI("parallel timing decode_read_avg=%.3fms encode_avg=%.3fms "
         "pipeline_stage_max=%.3fms theoretical_fps=%.2f",
         read_avg_ms,
         encode_avg_ms,
         parallel_stage_ms,
         parallel_stage_ms > 0.0 ? 1000.0 / parallel_stage_ms : 0.0);

    av_frame_free(&frame.av_frame);
    hw_encode_test_cleanup(&ctx);
    avformat_network_deinit();
    return EXIT_SUCCESS;
}
