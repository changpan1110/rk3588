#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "video_pipeline_input.c"

#include "video_pipeline_internal.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/debug.h"

static int64_t vp_capture_thread_cpu_us(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static int vp_capture_get_hdmi_placeholder(vp_channel_t *ch,
                                            AVFrame **out_placeholder) {
    static const uint8_t bars[8][3] = {
        {235, 128, 128}, /* white */
        {210,  16, 146}, /* yellow */
        {170, 166,  16}, /* cyan */
        {145,  54,  34}, /* green */
        {106, 202, 222}, /* magenta */
        { 81,  90, 240}, /* red */
        { 41, 240, 110}, /* blue */
        { 16, 128, 128}  /* black */
    };
    AVFrame *placeholder;
    int width;
    int height;
    int x;
    int y;

    if (ch == NULL || out_placeholder == NULL) {
        return APP_ERR_PARAM;
    }
    if (*out_placeholder != NULL) {
        return APP_OK;
    }

    width = ch->input.width > 0 ? ch->input.width : 1920;
    height = ch->input.height > 0 ? ch->input.height : 1080;
    placeholder = av_frame_alloc();
    if (placeholder == NULL) {
        return APP_ERR_NOMEM;
    }
    placeholder->format = AV_PIX_FMT_NV12;
    placeholder->width = width;
    placeholder->height = height;
    placeholder->color_range = AVCOL_RANGE_MPEG;
    placeholder->colorspace = AVCOL_SPC_BT709;
    placeholder->color_primaries = AVCOL_PRI_BT709;
    placeholder->color_trc = AVCOL_TRC_BT709;
    if (av_frame_get_buffer(placeholder, 32) < 0) {
        av_frame_free(&placeholder);
        return APP_ERR_NOMEM;
    }

    /* Keep aligned row padding deterministic for DMA/RGA consumers. */
    memset(placeholder->data[0], 16,
           (size_t)placeholder->linesize[0] * (size_t)height);
    memset(placeholder->data[1], 128,
           (size_t)placeholder->linesize[1] * (size_t)((height + 1) / 2));

    for (y = 0; y < height; ++y) {
        uint8_t *line = placeholder->data[0] + y * placeholder->linesize[0];
        for (x = 0; x < width; ++x) {
            int bar = (x * 8) / width;
            line[x] = bars[bar][0];
        }
    }
    for (y = 0; y < (height + 1) / 2; ++y) {
        uint8_t *line = placeholder->data[1] + y * placeholder->linesize[1];
        for (x = 0; x < width; x += 2) {
            int bar = (x * 8) / width;
            line[x] = bars[bar][1];
            if (x + 1 < width) {
                line[x + 1] = bars[bar][2];
            }
        }
    }
    *out_placeholder = placeholder;
    LOGI("%s HDMI no-signal color bars enabled: %dx%d",
         ch->name,
         width,
         height);
    return APP_OK;
}

app_status_t vp_channel_open(vp_channel_t *ch) {
    switch (ch->input.source_type) {
        case VIDEO_SOURCE_USB:
            return input_usb_open_hw(&ch->in.usb, &ch->input);
        case VIDEO_SOURCE_CSI0:
        case VIDEO_SOURCE_CSI1:
            return input_csi_open_hw(&ch->in.csi, &ch->input);
        case VIDEO_SOURCE_HDMI_IN:
            return input_hdmi_in_open(&ch->in.hdmi_in, &ch->input);
        default:
            return APP_ERR_PARAM;
    }
}

app_status_t vp_channel_read(vp_channel_t *ch, video_frame_t *frame) {
    switch (ch->input.source_type) {
        case VIDEO_SOURCE_USB:
            return input_usb_read(&ch->in.usb, frame);
        case VIDEO_SOURCE_CSI0:
        case VIDEO_SOURCE_CSI1:
            return input_csi_read(&ch->in.csi, frame);
        case VIDEO_SOURCE_HDMI_IN:
            return input_hdmi_in_read(&ch->in.hdmi_in, frame);
        default:
            return APP_ERR_PARAM;
    }
}

void vp_channel_close(vp_channel_t *ch) {
    switch (ch->input.source_type) {
        case VIDEO_SOURCE_USB:
            input_usb_close(&ch->in.usb);
            break;
        case VIDEO_SOURCE_CSI0:
        case VIDEO_SOURCE_CSI1:
            input_csi_close(&ch->in.csi);
            break;
        case VIDEO_SOURCE_HDMI_IN:
            input_hdmi_in_close(&ch->in.hdmi_in);
            break;
        default:
            break;
    }
}

void *vp_capture_thread(void *opaque) {
    vp_channel_t *ch = (vp_channel_t *)opaque;
    vp_ctx_t *p = ch->owner;
    video_frame_t frame;
    app_status_t status;
    int64_t frame_interval_us;
    int64_t next_frame_us = 0;
    int64_t now_us;
    int64_t timing_started_us;
    int64_t timing_cpu_started_us;
    int64_t read_started_us;
    int64_t read_cpu_started_us;
    int64_t timing_read_sum_us = 0;
    int64_t timing_decode_cpu_sum_us = 0;
    int64_t next_reconnect_us = 0;
    int timing_frames = 0;
    AVFrame *placeholder_frame = NULL;

    memset(&frame, 0, sizeof(frame));
    frame.av_frame = av_frame_alloc();
    if (frame.av_frame == NULL) {
        LOGE("%s capture av_frame_alloc failed", ch->name);
        return NULL;
    }
    frame_interval_us = ch->input.fps > 0 ? 1000000LL / ch->input.fps : 0;
    if (frame_interval_us > 0) {
        LOGD("%s pipeline frame limit: %d fps", ch->name, ch->input.fps);
    }
    timing_started_us = app_get_time_us();
    timing_cpu_started_us = vp_capture_thread_cpu_us();
    read_cpu_started_us = 0;

    while (!p->stop) {
        int frame_is_real = 1;

        av_frame_unref(frame.av_frame);
        if (!ch->is_opened) {
            if (!ch->capture_reconnect) {
                break;
            }
            now_us = app_get_time_us();
            if (now_us >= next_reconnect_us) {
                status = vp_channel_open(ch);
                if (status == APP_OK) {
                    ch->is_opened = 1;
                    next_reconnect_us = 0;
                    LOGI("%s input reconnected: %s", ch->name, ch->input.device);
                    next_frame_us = 0;
                    timing_started_us = now_us;
                    timing_cpu_started_us = vp_capture_thread_cpu_us();
                } else {
                    next_reconnect_us = now_us + 1000000LL;
                }
            }

            if (!ch->is_opened) {
                if (ch->input.source_type != VIDEO_SOURCE_HDMI_IN) {
                    usleep(10000);
                    continue;
                }
                if (vp_capture_get_hdmi_placeholder(ch, &placeholder_frame) != APP_OK ||
                    av_frame_ref(frame.av_frame, placeholder_frame) < 0) {
                    usleep(10000);
                    continue;
                }
                frame_is_real = 0;
                now_us = app_get_time_us();
                frame.pts_us = now_us;
            }
        }

        if (frame_is_real) {
            read_started_us = app_get_time_us();
            read_cpu_started_us = vp_capture_thread_cpu_us();
            status = vp_channel_read(ch, &frame);
            if (status == APP_OK) {
                timing_read_sum_us += app_get_time_us() - read_started_us;
                timing_decode_cpu_sum_us +=
                    vp_capture_thread_cpu_us() - read_cpu_started_us;
            }
        }
        if (frame_is_real && status == APP_ERR_AGAIN) {
            usleep(10000);
            continue;
        }
        if (frame_is_real && status != APP_OK) {
            if (ch->capture_reconnect && status == APP_ERR_EOF) {
                av_frame_unref(frame.av_frame);
                if (vp_capture_get_hdmi_placeholder(ch, &placeholder_frame) == APP_OK &&
                    av_frame_ref(frame.av_frame, placeholder_frame) == 0) {
                    vp_channel_close(ch);
                    ch->is_opened = 0;
                    next_reconnect_us = app_get_time_us() + 1000000LL;
                    frame_is_real = 0;
                    now_us = app_get_time_us();
                    frame.pts_us = now_us;
                    LOGW("%s input timed out; showing color bars and reconnecting",
                         ch->name);
                }
            }
            if (!frame_is_real) {
                /* Continue below and publish the placeholder frame. */
            } else {
                if (ch->capture_reconnect && status == APP_ERR_IO) {
                    vp_channel_close(ch);
                    ch->is_opened = 0;
                    next_reconnect_us = app_get_time_us() + 1000000LL;
                    pthread_mutex_lock(&p->lock);
                    ch->input_has_signal = 0;
                    if (ch->record_wanted) {
                        ch->record_wanted = 0;
                        ch->rec_end_us = app_get_time_us();
                        pthread_cond_broadcast(&ch->rec_cond);
                    }
                    pthread_mutex_unlock(&p->lock);
                    LOGW("%s input disconnected; entering reconnect mode", ch->name);
                }
                if (status != APP_ERR_EOF) {
                    LOGW("%s capture read failed: %s", ch->name, app_status_str(status));
                }
                usleep(10000);
                continue;
            }
        }
        if (frame_is_real) {
            now_us = frame.pts_us > 0 ? frame.pts_us : app_get_time_us();
        }
        if (frame_is_real == 0) {
            pthread_mutex_lock(&p->lock);
            ch->input_has_signal = 0;
            if (ch->record_wanted) {
                ch->record_wanted = 0;
                ch->rec_end_us = app_get_time_us();
                pthread_cond_broadcast(&ch->rec_cond);
            }
            pthread_mutex_unlock(&p->lock);
        }
        if (frame_interval_us > 0) {
            if (next_frame_us == 0) {
                next_frame_us = now_us;
            }
            if (now_us + frame_interval_us / 4 < next_frame_us) {
                continue;
            }
            if (now_us > next_frame_us + frame_interval_us * 2) {
                next_frame_us = now_us;
            }
            next_frame_us += frame_interval_us;
        }
        frame.av_frame->pts = now_us;

        pthread_mutex_lock(&p->lock);
        if (frame_is_real) {
            ch->cap_frames++;
            ch->input_has_signal = 1;
        }
        if (ch->latest == NULL) {
            ch->latest = av_frame_alloc();
        }
        if (ch->latest != NULL) {
            av_frame_unref(ch->latest);
            av_frame_ref(ch->latest, frame.av_frame);
        }
        if (ch->record_wanted && frame_is_real) {
            vp_queue_push_locked(&ch->rec_q, frame.av_frame);
            pthread_cond_signal(&ch->rec_cond);
        }
        if (p->stream_enabled && ch->id == p->stream_ch &&
            !p->switch_pending &&
            frame.av_frame->pts >= p->stream_generation_start_us) {
            vp_queue_push_latest_locked(&p->stream_q, frame.av_frame);
            pthread_cond_signal(&p->stream_cond);
        }
        pthread_mutex_unlock(&p->lock);
        timing_frames++;

        if (app_get_time_us() - timing_started_us >= 1000000) {
            int64_t timing_ended_us = app_get_time_us();
            int64_t timing_cpu_ended_us = vp_capture_thread_cpu_us();
            int64_t wall_us = timing_ended_us - timing_started_us;
            int64_t cpu_us = timing_cpu_ended_us - timing_cpu_started_us;

            pthread_mutex_lock(&p->lock);
            ch->capture_cpu_ms = (double)cpu_us / 1000.0;
            ch->capture_cpu_load =
                wall_us > 0 ? (double)cpu_us * 100.0 / wall_us : 0.0;
            ch->read_ms = timing_frames > 0
                              ? (double)timing_read_sum_us /
                                    timing_frames / 1000.0
                              : 0.0;
            ch->decode_ms = timing_frames > 0
                                ? (double)timing_decode_cpu_sum_us /
                                      timing_frames / 1000.0
                                : 0.0;
            pthread_mutex_unlock(&p->lock);
            timing_started_us = timing_ended_us;
            timing_cpu_started_us = timing_cpu_ended_us;
            timing_read_sum_us = 0;
            timing_decode_cpu_sum_us = 0;
            timing_frames = 0;
        }
    }

    av_frame_free(&frame.av_frame);
    av_frame_free(&placeholder_frame);
    return NULL;
}
