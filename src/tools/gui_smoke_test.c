#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "gui_smoke_test.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/frame.h>
#include <libavutil/imgutils.h>

#include "common/common.h"
#include "common/debug.h"
#include "output/output_display_gui.h"

static app_status_t fill_test_frame(AVFrame *frame, int tick) {
    int x;
    int y;
    int ret;

    if (frame == NULL) {
        return APP_ERR_PARAM;
    }

    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 640;
    frame->height = 360;

    ret = av_frame_get_buffer(frame, 32);
    if (ret < 0) {
        LOGE("av_frame_get_buffer failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    ret = av_frame_make_writable(frame);
    if (ret < 0) {
        LOGE("av_frame_make_writable failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    for (y = 0; y < frame->height; ++y) {
        for (x = 0; x < frame->width; ++x) {
            frame->data[0][y * frame->linesize[0] + x] = (uint8_t)((x + tick) & 0xff);
        }
    }

    for (y = 0; y < frame->height / 2; ++y) {
        for (x = 0; x < frame->width / 2; ++x) {
            frame->data[1][y * frame->linesize[1] + x] = (uint8_t)((64 + tick * 2) & 0xff);
            frame->data[2][y * frame->linesize[2] + x] = (uint8_t)((192 + tick * 3) & 0xff);
        }
    }

    return APP_OK;
}

int main(void) {
    output_display_gui_ctx_t gui;
    video_frame_t frame;
    app_status_t status;
    int tick = 0;

    memset(&gui, 0, sizeof(gui));
    memset(&frame, 0, sizeof(frame));

    status = output_display_gui_init(&gui, 1, 960, 540);
    if (status != APP_OK) {
        LOGE("output_display_gui_init failed: %s", app_status_str(status));
        return EXIT_FAILURE;
    }

    LOGI("gui smoke test started");
    LOGI("close the SDL window to exit");

    while (!output_display_gui_poll_quit(&gui)) {
        frame.av_frame = av_frame_alloc();
        if (frame.av_frame == NULL) {
            LOGE("av_frame_alloc failed");
            output_display_gui_deinit(&gui);
            return EXIT_FAILURE;
        }

        status = fill_test_frame(frame.av_frame, tick);
        if (status != APP_OK) {
            av_frame_free(&frame.av_frame);
            output_display_gui_deinit(&gui);
            return EXIT_FAILURE;
        }

        frame.pts_us = app_get_time_us();
        frame.source_type = VIDEO_SOURCE_USB;
        snprintf(frame.source_name, sizeof(frame.source_name), "%s", "gui-test");

        status = output_display_gui_show(&gui, &frame);
        av_frame_free(&frame.av_frame);
        if (status != APP_OK) {
            LOGE("output_display_gui_show failed: %s", app_status_str(status));
            output_display_gui_deinit(&gui);
            return EXIT_FAILURE;
        }

        ++tick;
    }

    output_display_gui_deinit(&gui);
    LOGI("gui smoke test finished");
    return EXIT_SUCCESS;
}
