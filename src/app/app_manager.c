#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "app_manager.c"

#include "app/app_manager.h"

#include <string.h>
#include <unistd.h>

#include "common/debug.h"
#include "process/process_pip.h"

/*
 * Manual GUI preview source selection.
 * Change this one line to switch the decoded preview between USB / CSI0 / CSI1 / HDMI_IN.
 */
static video_source_type_t g_gui_preview_source = VIDEO_SOURCE_CSI0;

/*
 * Low-latency preview mode:
 * only read decoded frames and show them on GUI.
 * Skip PIP / HDMI display / encode / record to isolate delay sources.
 */
static int g_gui_preview_only = 1;

static int video_app_get_preview_fps(const video_app_ctx_t *ctx) {
    if (ctx == NULL) {
        return 30;
    }

    switch (g_gui_preview_source) {
        case VIDEO_SOURCE_USB:
            return ctx->config.input.usb.fps > 0 ? ctx->config.input.usb.fps : 30;
        case VIDEO_SOURCE_CSI0:
            return ctx->config.input.csi0.fps > 0 ? ctx->config.input.csi0.fps : 30;
        case VIDEO_SOURCE_CSI1:
            return ctx->config.input.csi1.fps > 0 ? ctx->config.input.csi1.fps : 30;
        case VIDEO_SOURCE_HDMI_IN:
            return ctx->config.input.hdmi_in.fps > 0 ? ctx->config.input.hdmi_in.fps : 30;
        default:
            return 30;
    }
}

void video_output_prepare_default(video_output_config_t *cfg) {
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->output.enable_record = 1;
    cfg->output.enable_stream = 1;

    cfg->output.record.width = 1920;
    cfg->output.record.height = 1080;
    cfg->output.record.fps = 30;
    cfg->output.record.bitrate = 4000000;

    cfg->output.stream.width = 1280;
    cfg->output.stream.height = 720;
    cfg->output.stream.fps = 30;
    cfg->output.stream.bitrate = 1000000;

    if (cfg->output.record.width == cfg->output.stream.width &&
        cfg->output.record.height == cfg->output.stream.height &&
        cfg->output.record.fps == cfg->output.stream.fps &&
        cfg->output.record.bitrate == cfg->output.stream.bitrate) {
        cfg->encoder_mode = ENCODER_MODE_SINGLE;
    } else {
        cfg->encoder_mode = ENCODER_MODE_DUAL;
    }
}

static app_status_t video_app_open_inputs(video_app_ctx_t *ctx) {
    app_status_t status;

    if (g_gui_preview_only) {
        switch (g_gui_preview_source) {
            case VIDEO_SOURCE_USB:
                status = input_usb_open(&ctx->usb, &ctx->config.input.usb);
                if (status != APP_OK) {
                    LOGW("usb input open failed: %s", app_status_str(status));
                }
                return status;
            case VIDEO_SOURCE_CSI0:
                status = input_csi0_open(&ctx->csi0, &ctx->config.input.csi0);
                if (status != APP_OK) {
                    LOGW("csi0 input open failed: %s", app_status_str(status));
                }
                return status;
            case VIDEO_SOURCE_CSI1:
                status = input_csi1_open(&ctx->csi1, &ctx->config.input.csi1);
                if (status != APP_OK) {
                    LOGW("csi1 input open failed: %s", app_status_str(status));
                }
                return status;
            case VIDEO_SOURCE_HDMI_IN:
                status = input_hdmi_in_open(&ctx->hdmi_in, &ctx->config.input.hdmi_in);
                if (status != APP_OK) {
                    LOGW("hdmi_in input open failed: %s", app_status_str(status));
                }
                return status;
            default:
                return APP_ERR_PARAM;
        }
    }

    status = input_usb_open(&ctx->usb, &ctx->config.input.usb);
    if (status != APP_OK) {
        LOGW("usb input open failed: %s", app_status_str(status));
    }

    status = input_csi0_open(&ctx->csi0, &ctx->config.input.csi0);
    if (status != APP_OK) {
        LOGW("csi0 input open failed: %s", app_status_str(status));
    }

    status = input_csi1_open(&ctx->csi1, &ctx->config.input.csi1);
    if (status != APP_OK) {
        LOGW("csi1 input open failed: %s", app_status_str(status));
    }

    status = input_hdmi_in_open(&ctx->hdmi_in, &ctx->config.input.hdmi_in);
    if (status != APP_OK) {
        LOGW("hdmi_in input open failed: %s", app_status_str(status));
    }

    return APP_OK;
}

static app_status_t video_app_read_frame(video_app_ctx_t *ctx, video_frame_t *frame) {
    switch (g_gui_preview_source) {
        case VIDEO_SOURCE_USB:
            if (ctx->usb.is_opened) {
                return input_usb_read(&ctx->usb, frame);
            }
            break;
        case VIDEO_SOURCE_CSI0:
            if (ctx->csi0.is_opened) {
                return input_csi0_read(&ctx->csi0, frame);
            }
            break;
        case VIDEO_SOURCE_CSI1:
            if (ctx->csi1.is_opened) {
                return input_csi1_read(&ctx->csi1, frame);
            }
            break;
        case VIDEO_SOURCE_HDMI_IN:
            if (ctx->hdmi_in.is_opened) {
                return input_hdmi_in_read(&ctx->hdmi_in, frame);
            }
            break;
        default:
            break;
    }

    LOGE("selected gui preview source is not opened");
    return APP_ERR_IO;
}

app_status_t video_app_init(video_app_ctx_t *ctx, const video_app_config_t *cfg) {
    app_status_t status;

    if (ctx == NULL || cfg == NULL) {
        return APP_ERR_PARAM;
    }

    *ctx = (video_app_ctx_t){0};
    ctx->config = *cfg;
    ctx->output = cfg->output;
    ctx->running = 1;
    ctx->loop_delay_ms = g_gui_preview_only ? 1 : (1000 / video_app_get_preview_fps(ctx));

    status = process_common_init(&ctx->process);
    if (status != APP_OK) {
        return status;
    }

    status = output_display_gui_init(&ctx->gui_display, 1, 1280, 720);
    if (status != APP_OK) {
        LOGW("gui display init failed: %s", app_status_str(status));
    }

    if (!g_gui_preview_only) {
        status = output_encode_main_init(&ctx->main_encoder,
                                         ctx->output.encoder_mode,
                                         &ctx->output.output.record);
        if (status != APP_OK) {
            return status;
        }

        status = output_encode_sub_init(&ctx->sub_encoder,
                                        ctx->output.encoder_mode,
                                        &ctx->output.output.stream,
                                        ctx->output.encoder_mode == ENCODER_MODE_DUAL);
        if (status != APP_OK) {
            return status;
        }

        status = output_display_hdmi_init(&ctx->hdmi_display, 207);
        if (status != APP_OK) {
            return status;
        }

        status = output_record_init(&ctx->recorder);
        if (status != APP_OK) {
            return status;
        }

        status = output_snapshot_init(&ctx->snapshot, "/tmp");
        if (status != APP_OK) {
            return status;
        }
    }

    return video_app_open_inputs(ctx);
}

app_status_t video_app_run_once(video_app_ctx_t *ctx) {
    video_frame_t frame;
    app_status_t status;

    if (ctx == NULL) {
        return APP_ERR_PARAM;
    }

    memset(&frame, 0, sizeof(frame));
    frame.av_frame = av_frame_alloc();
    if (frame.av_frame == NULL) {
        return APP_ERR_NOMEM;
    }

    status = video_app_read_frame(ctx, &frame);

    if (status == APP_OK) {
        /*
         * 预览模式下不做 CPU 格式转换:
         * NV12 / UYVY / YUV420P 由 GUI 直接上传 SDL 纹理,缩放交给显示侧,
         * 只有 yuvj420p 这类特殊格式才走 GUI 内部的 swscale 兜底。
         */
        if (!g_gui_preview_only || frame.av_frame->format == AV_PIX_FMT_NV16) {
            status = process_common_convert_to_yuv420(&ctx->process, &frame);
            if (status != APP_OK) {
                LOGW("process_common_convert_to_yuv420 failed: %s", app_status_str(status));
                av_frame_free(&frame.av_frame);
                return status;
            }
        }

        if (output_display_gui_poll_quit(&ctx->gui_display)) {
            ctx->gui_display.enabled = 0;
            ctx->running = 0;
        }
        output_display_gui_show(&ctx->gui_display, &frame);
        if (!g_gui_preview_only) {
            process_pip_frame(&ctx->process, &frame);
            output_display_hdmi_show(&ctx->hdmi_display, &frame);
            output_encode_main_push(&ctx->main_encoder, &frame);
            output_encode_sub_push(&ctx->sub_encoder, &frame);
        }
    } else if (status != APP_ERR_EOF) {
        LOGW("video_app_run_once read failed: %s", app_status_str(status));
    } else {
        LOGD("video_app_run_once read retry: %s", app_status_str(status));
    }

    av_frame_free(&frame.av_frame);
    return status;
}

int video_app_is_running(const video_app_ctx_t *ctx) {
    if (ctx == NULL) {
        return 0;
    }
    return ctx->running;
}

int video_app_get_loop_delay_ms(const video_app_ctx_t *ctx) {
    if (ctx == NULL) {
        return 0;
    }
    if (ctx->loop_delay_ms <= 0) {
        return 0;
    }
    return ctx->loop_delay_ms;
}

void video_app_request_stop(video_app_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    ctx->running = 0;
}

app_status_t video_app_start_record(video_app_ctx_t *ctx, const char *path) {
    output_record_params_t params;

    if (ctx == NULL || path == NULL) {
        return APP_ERR_PARAM;
    }

    params.path = path;
    params.encode = ctx->output.output.record;
    return output_record_start(&ctx->recorder, &params);
}

app_status_t video_app_stop_record(video_app_ctx_t *ctx) {
    if (ctx == NULL) {
        return APP_ERR_PARAM;
    }
    return output_record_stop(&ctx->recorder);
}

app_status_t video_app_take_snapshot(video_app_ctx_t *ctx, const char *path, const video_frame_t *frame) {
    output_snapshot_params_t params;

    if (ctx == NULL || path == NULL || frame == NULL) {
        return APP_ERR_PARAM;
    }

    params.path = path;
    return output_snapshot_take(&ctx->snapshot, &params, frame);
}

void video_app_deinit(video_app_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    input_usb_close(&ctx->usb);
    input_csi0_close(&ctx->csi0);
    input_csi1_close(&ctx->csi1);
    input_hdmi_in_close(&ctx->hdmi_in);
    output_display_gui_deinit(&ctx->gui_display);
    output_record_deinit(&ctx->recorder);
    output_snapshot_deinit(&ctx->snapshot);
    output_display_hdmi_deinit(&ctx->hdmi_display);
    output_encode_main_deinit(&ctx->main_encoder);
    output_encode_sub_deinit(&ctx->sub_encoder);
    process_common_deinit(&ctx->process);
}
