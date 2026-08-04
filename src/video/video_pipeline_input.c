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

app_status_t vp_channel_open(vp_channel_t *ch) {
    switch (ch->input.source_type) {
        case VIDEO_SOURCE_USB:
            return input_usb_open_hw(&ch->in.usb, &ch->input);
        case VIDEO_SOURCE_CSI0:
            return input_csi0_open(&ch->in.csi0, &ch->input);
        case VIDEO_SOURCE_CSI1:
            return input_csi1_open(&ch->in.csi1, &ch->input);
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
            return input_csi0_read(&ch->in.csi0, frame);
        case VIDEO_SOURCE_CSI1:
            return input_csi1_read(&ch->in.csi1, frame);
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
            input_csi0_close(&ch->in.csi0);
            break;
        case VIDEO_SOURCE_CSI1:
            input_csi1_close(&ch->in.csi1);
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
    int timing_frames = 0;

    memset(&frame, 0, sizeof(frame));
    frame.av_frame = av_frame_alloc();
    if (frame.av_frame == NULL) {
        LOGE("%s capture av_frame_alloc failed", ch->name);
        return NULL;
    }
    frame_interval_us = ch->input.fps > 0 ? 1000000LL / ch->input.fps : 0;
    if (frame_interval_us > 0) {
        LOGI("%s pipeline frame limit: %d fps", ch->name, ch->input.fps);
    }
    timing_started_us = app_get_time_us();
    timing_cpu_started_us = vp_capture_thread_cpu_us();

    while (!p->stop) {
        av_frame_unref(frame.av_frame);
        status = vp_channel_read(ch, &frame);
        if (status != APP_OK) {
            if (status != APP_ERR_EOF) {
                LOGW("%s capture read failed: %s", ch->name, app_status_str(status));
            }
            usleep(10000);
            continue;
        }
        now_us = frame.pts_us > 0 ? frame.pts_us : app_get_time_us();
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
        ch->cap_frames++;
        if (ch->latest == NULL) {
            ch->latest = av_frame_alloc();
        }
        if (ch->latest != NULL) {
            av_frame_unref(ch->latest);
            av_frame_ref(ch->latest, frame.av_frame);
        }
        if (ch->record_wanted) {
            vp_queue_push_locked(&ch->rec_q, frame.av_frame);
            pthread_cond_signal(&ch->rec_cond);
        }
        if (p->stream_enabled && ch->id == p->stream_ch && !p->switch_pending) {
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

            LOGI("%s capture timing frames=%d thread_cpu=%.3fms "
                 "cpu_per_frame=%.3fms cpu_load=%.1f%%",
                 ch->name,
                 timing_frames,
                 (double)cpu_us / 1000.0,
                 timing_frames > 0 ? (double)cpu_us / timing_frames / 1000.0 : 0.0,
                 wall_us > 0 ? (double)cpu_us * 100.0 / wall_us : 0.0);
            timing_started_us = timing_ended_us;
            timing_cpu_started_us = timing_cpu_ended_us;
            timing_frames = 0;
        }
    }

    av_frame_free(&frame.av_frame);
    return NULL;
}
