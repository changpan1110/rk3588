#define LOG_LOCAL_LEVEL LOG_LEVEL_TRACE
#define LOG_FILE_NAME "app_manager.c"

#include "app/app_manager.h"

#include <string.h>
#include <unistd.h>

#include "common/debug.h"

/*
 * Manual GUI preview source selection.
 * Change this one line to switch the decoded preview between USB / CSI0 / CSI1 / HDMI_IN.
 */
static video_source_type_t g_gui_preview_source = VIDEO_SOURCE_CSI0;

static app_status_t video_app_open_inputs(video_app_ctx_t *ctx) {
    app_status_t status;

    switch (g_gui_preview_source) {
        case VIDEO_SOURCE_USB:
            status = input_usb_open(&ctx->usb, &ctx->config.input.usb);
            break;
        case VIDEO_SOURCE_CSI0:
            status = input_csi_open(&ctx->csi0, &ctx->config.input.csi0);
            break;
        case VIDEO_SOURCE_CSI1:
            status = input_csi_open(&ctx->csi1, &ctx->config.input.csi1);
            break;
        case VIDEO_SOURCE_HDMI_IN:
            status = input_hdmi_in_open(&ctx->hdmi_in, &ctx->config.input.hdmi_in);
            break;
        default:
            return APP_ERR_PARAM;
    }

    if (status != APP_OK) {
        LOGW("preview input open failed: %s", app_status_str(status));
    }
    return status;
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
                return input_csi_read(&ctx->csi0, frame);
            }
            break;
        case VIDEO_SOURCE_CSI1:
            if (ctx->csi1.is_opened) {
                return input_csi_read(&ctx->csi1, frame);
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
    ctx->running = 1;
    ctx->loop_delay_ms = 1;

    status = process_common_init(&ctx->process);
    if (status != APP_OK) {
        return status;
    }

    status = display_gui_init(&ctx->gui_display, 1, 1280, 720);
    if (status != APP_OK) {
        LOGW("gui display init failed: %s", app_status_str(status));
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
        if (frame.av_frame->format == AV_PIX_FMT_NV16) {
            status = process_common_convert_to_yuv420(&ctx->process, &frame);
            if (status != APP_OK) {
                LOGW("process_common_convert_to_yuv420 failed: %s", app_status_str(status));
                av_frame_free(&frame.av_frame);
                return status;
            }
        }

        if (display_gui_poll_quit(&ctx->gui_display)) {
            ctx->gui_display.enabled = 0;
            ctx->running = 0;
        }
        display_gui_show(&ctx->gui_display, &frame);
    } else if (status != APP_ERR_EOF) {
        LOGW("video_app_run_once read failed: %s", app_status_str(status));
    } else {
        LOGT("video_app_run_once read retry: %s", app_status_str(status));
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

void video_app_deinit(video_app_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    input_usb_close(&ctx->usb);
    input_csi_close(&ctx->csi0);
    input_csi_close(&ctx->csi1);
    input_hdmi_in_close(&ctx->hdmi_in);
    display_gui_deinit(&ctx->gui_display);
    process_common_deinit(&ctx->process);
}
