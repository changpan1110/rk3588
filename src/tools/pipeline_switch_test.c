#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "pipeline_switch_test.c"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/common.h"
#include "common/debug.h"
#include "input/input_hdmi_in.h"
#include "input/input_usb.h"
#include "output/output_encode_common.h"

#define PIPELINE_SOURCE_COUNT 3

typedef enum {
    PIPELINE_SOURCE_USB0 = 0,
    PIPELINE_SOURCE_USB1,
    PIPELINE_SOURCE_HDMI
} pipeline_source_id_t;

typedef struct {
    pipeline_source_id_t id;
    const char *name;
    video_input_config_t cfg;
    union {
        input_usb_ctx_t usb;
        input_hdmi_in_ctx_t hdmi_in;
    } input;
    int opened;
    int stop;
    int read_failures;
    unsigned long frame_seq;
    pthread_t thread;
    pthread_mutex_t lock;
    output_encode_convert_ctx_t nv12_convert;
    AVFrame *latest_frame;
    int latest_ready;
} pipeline_source_ctx_t;

typedef struct {
    pipeline_source_ctx_t sources[PIPELINE_SOURCE_COUNT];
    pthread_mutex_t state_lock;
    pipeline_source_id_t stream_source;
    int record_enabled[PIPELINE_SOURCE_COUNT];
    int stop;
} pipeline_test_ctx_t;

static const char *pipeline_source_name(pipeline_source_id_t id) {
    switch (id) {
        case PIPELINE_SOURCE_USB0: return "usb0";
        case PIPELINE_SOURCE_USB1: return "usb1";
        case PIPELINE_SOURCE_HDMI: return "hdmi";
        default: return "unknown";
    }
}

static pipeline_source_id_t pipeline_parse_source(const char *text) {
    if (text == NULL) {
        return PIPELINE_SOURCE_COUNT;
    }
    if (strcmp(text, "usb0") == 0) {
        return PIPELINE_SOURCE_USB0;
    }
    if (strcmp(text, "usb1") == 0) {
        return PIPELINE_SOURCE_USB1;
    }
    if (strcmp(text, "hdmi") == 0 || strcmp(text, "hdmi_in") == 0) {
        return PIPELINE_SOURCE_HDMI;
    }
    return PIPELINE_SOURCE_COUNT;
}

static app_status_t pipeline_source_open(pipeline_source_ctx_t *src) {
    if (src == NULL) {
        return APP_ERR_PARAM;
    }

    switch (src->id) {
        case PIPELINE_SOURCE_USB0:
        case PIPELINE_SOURCE_USB1:
            return input_usb_open_hw(&src->input.usb, &src->cfg);
        case PIPELINE_SOURCE_HDMI:
            return input_hdmi_in_open(&src->input.hdmi_in, &src->cfg);
        default:
            return APP_ERR_PARAM;
    }
}

static app_status_t pipeline_source_read(pipeline_source_ctx_t *src, video_frame_t *frame) {
    if (src == NULL || frame == NULL) {
        return APP_ERR_PARAM;
    }

    switch (src->id) {
        case PIPELINE_SOURCE_USB0:
        case PIPELINE_SOURCE_USB1:
            return input_usb_read(&src->input.usb, frame);
        case PIPELINE_SOURCE_HDMI:
            return input_hdmi_in_read(&src->input.hdmi_in, frame);
        default:
            return APP_ERR_PARAM;
    }
}

static void pipeline_source_close(pipeline_source_ctx_t *src) {
    if (src == NULL) {
        return;
    }

    switch (src->id) {
        case PIPELINE_SOURCE_USB0:
        case PIPELINE_SOURCE_USB1:
            input_usb_close(&src->input.usb);
            break;
        case PIPELINE_SOURCE_HDMI:
            input_hdmi_in_close(&src->input.hdmi_in);
            break;
        default:
            break;
    }
}

static app_status_t pipeline_source_store_nv12(pipeline_source_ctx_t *src, const AVFrame *frame) {
    const AVFrame *nv12_frame;
    int converted = 0;
    int ret;

    if (src == NULL || frame == NULL) {
        return APP_ERR_PARAM;
    }

    if (src->nv12_convert.dst_width != frame->width ||
        src->nv12_convert.dst_height != frame->height ||
        src->nv12_convert.dst_fmt != AV_PIX_FMT_NV12) {
        output_encode_convert_deinit(&src->nv12_convert);
        ret = output_encode_convert_init(&src->nv12_convert,
                                         frame->width,
                                         frame->height,
                                         AV_PIX_FMT_NV12);
        if (ret != APP_OK) {
            return ret;
        }
    }

    nv12_frame = output_encode_prepare_frame(&src->nv12_convert, frame, &converted);
    if (nv12_frame == NULL) {
        LOGE("%s convert to nv12 failed src=%dx%d fmt=%d",
             src->name,
             frame->width,
             frame->height,
             frame->format);
        return APP_ERR_FFMPEG;
    }

    if (src->latest_frame == NULL) {
        src->latest_frame = av_frame_alloc();
        if (src->latest_frame == NULL) {
            return APP_ERR_NOMEM;
        }
    } else {
        av_frame_unref(src->latest_frame);
    }

    ret = av_frame_ref(src->latest_frame, nv12_frame);
    if (ret < 0) {
        LOGE("%s av_frame_ref failed: %d", src->name, ret);
        return APP_ERR_FFMPEG;
    }

    src->latest_ready = 1;
    src->frame_seq++;
    return APP_OK;
}

static void *pipeline_source_thread(void *opaque) {
    pipeline_source_ctx_t *src = (pipeline_source_ctx_t *)opaque;
    video_frame_t frame;
    app_status_t status;

    memset(&frame, 0, sizeof(frame));
    frame.av_frame = av_frame_alloc();
    if (frame.av_frame == NULL) {
        LOGE("%s av_frame_alloc failed", src->name);
        return NULL;
    }

    while (!src->stop) {
        av_frame_unref(frame.av_frame);
        status = pipeline_source_read(src, &frame);
        if (status != APP_OK) {
            src->read_failures++;
            if (src->read_failures % 100 == 1) {
                LOGW("%s read failed: %s", src->name, app_status_str(status));
            }
            usleep(10000);
            continue;
        }

        src->read_failures = 0;
        pthread_mutex_lock(&src->lock);
        pipeline_source_store_nv12(src, frame.av_frame);
        pthread_mutex_unlock(&src->lock);
    }

    av_frame_free(&frame.av_frame);
    return NULL;
}

static void pipeline_source_init(pipeline_source_ctx_t *src,
                                 pipeline_source_id_t id,
                                 const char *name,
                                 const char *device,
                                 const char *fmt,
                                 int width,
                                 int height,
                                 int fps) {
    memset(src, 0, sizeof(*src));
    src->id = id;
    src->name = name;
    strncpy(src->cfg.name, name, sizeof(src->cfg.name) - 1);
    strncpy(src->cfg.device, device, sizeof(src->cfg.device) - 1);
    strncpy(src->cfg.input_format, fmt, sizeof(src->cfg.input_format) - 1);
    src->cfg.width = width;
    src->cfg.height = height;
    src->cfg.fps = fps;
    src->cfg.source_type = (id == PIPELINE_SOURCE_HDMI) ? VIDEO_SOURCE_HDMI_IN : VIDEO_SOURCE_USB;
    pthread_mutex_init(&src->lock, NULL);
}

static int pipeline_source_start(pipeline_source_ctx_t *src) {
    app_status_t status;

    status = pipeline_source_open(src);
    if (status != APP_OK) {
        LOGE("%s open failed: %s", src->name, app_status_str(status));
        return -1;
    }

    src->opened = 1;
    if (pthread_create(&src->thread, NULL, pipeline_source_thread, src) != 0) {
        LOGE("%s pthread_create failed", src->name);
        pipeline_source_close(src);
        src->opened = 0;
        return -1;
    }
    return 0;
}

static void pipeline_source_deinit(pipeline_source_ctx_t *src) {
    if (src == NULL) {
        return;
    }

    src->stop = 1;
    if (src->opened) {
        pthread_join(src->thread, NULL);
        pipeline_source_close(src);
    }
    if (src->latest_frame != NULL) {
        av_frame_free(&src->latest_frame);
    }
    output_encode_convert_deinit(&src->nv12_convert);
    pthread_mutex_destroy(&src->lock);
    memset(src, 0, sizeof(*src));
}

static void pipeline_print_help(void) {
    printf("commands:\n");
    printf("  stream usb0|usb1|hdmi\n");
    printf("  record start all|usb0|usb1|hdmi\n");
    printf("  record stop all|usb0|usb1|hdmi\n");
    printf("  status\n");
    printf("  quit\n");
    fflush(stdout);
}

static void pipeline_print_status(const pipeline_test_ctx_t *ctx) {
    int i;

    if (ctx == NULL) {
        return;
    }

    printf("stream source: %s\n", pipeline_source_name(ctx->stream_source));
    for (i = 0; i < PIPELINE_SOURCE_COUNT; ++i) {
        const pipeline_source_ctx_t *src = &ctx->sources[i];
        printf("  %-5s record=%d seq=%lu ready=%d\n",
               src->name,
               ctx->record_enabled[i],
               src->frame_seq,
               src->latest_ready);
    }
    fflush(stdout);
}

static void pipeline_set_record_flag(pipeline_test_ctx_t *ctx, pipeline_source_id_t id, int enabled) {
    int i;

    if (ctx == NULL) {
        return;
    }

    pthread_mutex_lock(&ctx->state_lock);
    if (id == PIPELINE_SOURCE_COUNT) {
        for (i = 0; i < PIPELINE_SOURCE_COUNT; ++i) {
            ctx->record_enabled[i] = enabled;
        }
    } else {
        ctx->record_enabled[id] = enabled;
    }
    pthread_mutex_unlock(&ctx->state_lock);
}

static void pipeline_handle_command(pipeline_test_ctx_t *ctx, char *line) {
    char *cmd;
    char *arg1;
    char *arg2;
    pipeline_source_id_t id;

    cmd = strtok(line, " \t\r\n");
    if (cmd == NULL) {
        return;
    }

    if (strcmp(cmd, "stream") == 0) {
        arg1 = strtok(NULL, " \t\r\n");
        id = pipeline_parse_source(arg1);
        if (id >= PIPELINE_SOURCE_COUNT) {
            printf("invalid source\n");
            fflush(stdout);
            return;
        }
        pthread_mutex_lock(&ctx->state_lock);
        ctx->stream_source = id;
        pthread_mutex_unlock(&ctx->state_lock);
        printf("switch stream to %s\n", pipeline_source_name(id));
        fflush(stdout);
        return;
    }

    if (strcmp(cmd, "record") == 0) {
        arg1 = strtok(NULL, " \t\r\n");
        arg2 = strtok(NULL, " \t\r\n");
        if (arg1 == NULL || arg2 == NULL) {
            printf("usage: record start|stop all|usb0|usb1|hdmi\n");
            fflush(stdout);
            return;
        }
        id = strcmp(arg2, "all") == 0 ? PIPELINE_SOURCE_COUNT : pipeline_parse_source(arg2);
        if (id > PIPELINE_SOURCE_COUNT) {
            printf("invalid source\n");
            fflush(stdout);
            return;
        }
        if (strcmp(arg1, "start") == 0) {
            pipeline_set_record_flag(ctx, id, 1);
            printf("record start %s\n", arg2);
        } else if (strcmp(arg1, "stop") == 0) {
            pipeline_set_record_flag(ctx, id, 0);
            printf("record stop %s\n", arg2);
        } else {
            printf("usage: record start|stop all|usb0|usb1|hdmi\n");
        }
        fflush(stdout);
        return;
    }

    if (strcmp(cmd, "status") == 0) {
        pipeline_print_status(ctx);
        return;
    }

    if (strcmp(cmd, "help") == 0) {
        pipeline_print_help();
        return;
    }

    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        ctx->stop = 1;
        return;
    }

    printf("unknown command\n");
    fflush(stdout);
}

static void *pipeline_command_thread(void *opaque) {
    pipeline_test_ctx_t *ctx = (pipeline_test_ctx_t *)opaque;
    char line[128];

    pipeline_print_help();
    while (!ctx->stop && fgets(line, sizeof(line), stdin) != NULL) {
        pipeline_handle_command(ctx, line);
    }
    ctx->stop = 1;
    return NULL;
}

int main(void) {
    pipeline_test_ctx_t ctx;
    pthread_t command_thread;
    unsigned long last_seq[PIPELINE_SOURCE_COUNT] = {0};
    int last_record[PIPELINE_SOURCE_COUNT] = {0};
    pipeline_source_id_t last_stream_source;
    int i;

#ifdef HAVE_LIBAVDEVICE
    extern void avdevice_register_all(void);
    avdevice_register_all();
#endif
    avformat_network_init();

    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.state_lock, NULL);
    ctx.stream_source = PIPELINE_SOURCE_HDMI;

    pipeline_source_init(&ctx.sources[0], PIPELINE_SOURCE_USB0, "usb0", "/dev/video41", "mjpeg", 1280, 720, 30);
    pipeline_source_init(&ctx.sources[1], PIPELINE_SOURCE_USB1, "usb1", "/dev/video43", "mjpeg", 640, 480, 30);
    pipeline_source_init(&ctx.sources[2], PIPELINE_SOURCE_HDMI, "hdmi", "/dev/video40", "", 0, 0, 0);

    for (i = 0; i < PIPELINE_SOURCE_COUNT; ++i) {
        if (pipeline_source_start(&ctx.sources[i]) != 0) {
            ctx.stop = 1;
        }
    }
    if (ctx.stop) {
        goto cleanup;
    }

    if (pthread_create(&command_thread, NULL, pipeline_command_thread, &ctx) != 0) {
        LOGE("command pthread_create failed");
        ctx.stop = 1;
        goto cleanup;
    }

    last_stream_source = ctx.stream_source;
    while (!ctx.stop) {
        pipeline_source_id_t current_stream_source;

        pthread_mutex_lock(&ctx.state_lock);
        current_stream_source = ctx.stream_source;
        for (i = 0; i < PIPELINE_SOURCE_COUNT; ++i) {
            if (last_record[i] != ctx.record_enabled[i]) {
                LOGI("%s record %s", ctx.sources[i].name, ctx.record_enabled[i] ? "enabled" : "disabled");
                last_record[i] = ctx.record_enabled[i];
            }
        }
        pthread_mutex_unlock(&ctx.state_lock);

        if (current_stream_source != last_stream_source) {
            LOGI("stream route switched to %s", pipeline_source_name(current_stream_source));
            last_stream_source = current_stream_source;
        }

        for (i = 0; i < PIPELINE_SOURCE_COUNT; ++i) {
            pipeline_source_ctx_t *src = &ctx.sources[i];

            pthread_mutex_lock(&src->lock);
            if (src->latest_ready && src->frame_seq != last_seq[i]) {
                if ((pipeline_source_id_t)i == current_stream_source) {
                    LOGD("stream source=%s seq=%lu nv12=%dx%d",
                         src->name,
                         src->frame_seq,
                         src->latest_frame != NULL ? src->latest_frame->width : 0,
                         src->latest_frame != NULL ? src->latest_frame->height : 0);
                }
                if (last_record[i]) {
                    LOGD("record source=%s seq=%lu nv12=%dx%d",
                         src->name,
                         src->frame_seq,
                         src->latest_frame != NULL ? src->latest_frame->width : 0,
                         src->latest_frame != NULL ? src->latest_frame->height : 0);
                }
                last_seq[i] = src->frame_seq;
            }
            pthread_mutex_unlock(&src->lock);
        }

        usleep(10000);
    }

    pthread_join(command_thread, NULL);

cleanup:
    for (i = 0; i < PIPELINE_SOURCE_COUNT; ++i) {
        pipeline_source_deinit(&ctx.sources[i]);
    }
    pthread_mutex_destroy(&ctx.state_lock);
    avformat_network_deinit();
    return ctx.stop ? EXIT_SUCCESS : EXIT_FAILURE;
}
