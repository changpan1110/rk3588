#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "video_pipeline_record.c"

#include "video_pipeline_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>

#include "common/debug.h"

static int64_t vp_record_next_frame_pts(const vp_channel_t *ch) {
    AVRational frame_duration;

    if (ch == NULL || ch->rec_enc == NULL) {
        return 0;
    }
    frame_duration = ch->rec_enc->framerate.num > 0 &&
                             ch->rec_enc->framerate.den > 0
                         ? av_inv_q(ch->rec_enc->framerate)
                         : (AVRational){1, 30};
    return av_rescale_q((int64_t)ch->rec_frames,
                        frame_duration,
                        ch->rec_enc->time_base);
}

static enum AVPixelFormat vp_record_hw_sw_format(const AVFrame *frame) {
    const AVHWFramesContext *frames_ctx;

    if (frame == NULL || frame->format != AV_PIX_FMT_DRM_PRIME ||
        frame->hw_frames_ctx == NULL || frame->hw_frames_ctx->data == NULL) {
        return AV_PIX_FMT_NONE;
    }
    frames_ctx = (const AVHWFramesContext *)frame->hw_frames_ctx->data;
    return frames_ctx->sw_format;
}

static int vp_record_can_direct_hw(const AVFrame *frame,
                                   int width,
                                   int height) {
    enum AVPixelFormat sw_format;

    if (frame == NULL || frame->width != width || frame->height != height) {
        return 0;
    }
    sw_format = vp_record_hw_sw_format(frame);
    return vp_h264_rkmpp_supports_sw_format(sw_format);
}

static int vp_record_direct_frame_matches(const vp_channel_t *ch,
                                          const AVFrame *frame) {
    return ch != NULL && ch->rec_direct_hw && frame != NULL &&
           frame->width == ch->rec_enc->width &&
           frame->height == ch->rec_enc->height &&
           vp_record_hw_sw_format(frame) == ch->rec_direct_sw_fmt;
}

static app_status_t vp_record_open_output(vp_channel_t *ch,
                                          int width,
                                          int height,
                                          int fps,
                                          const AVFrame *warmup_frame) {
    const AVFrame *hw_encode_frame = NULL;
    enum AVPixelFormat direct_sw_fmt = AV_PIX_FMT_NONE;
    int converted = 0;
    int convert_to_hw;
    int direct_hw;
    int ret;

    if (ch->rec_bitrate <= 0) {
        ch->rec_bitrate = (width >= 3000) ? VP_RECORD_4K_BITRATE : VP_RECORD_HD_BITRATE;
    }

    ret = avformat_alloc_output_context2(&ch->rec_fmt, NULL, "mp4", ch->rec_path);
    if (ret < 0 || ch->rec_fmt == NULL) {
        LOGE("%s record alloc mp4 failed: %d", ch->name, ret);
        return APP_ERR_FFMPEG;
    }

    direct_hw = vp_record_can_direct_hw(warmup_frame, width, height);
    if (direct_hw) {
        direct_sw_fmt = vp_record_hw_sw_format(warmup_frame);
        hw_encode_frame = warmup_frame;
        ch->rec_direct_hw = 1;
        ch->rec_direct_sw_fmt = direct_sw_fmt;
    } else {
        ch->rec_direct_hw = 0;
        ch->rec_direct_sw_fmt = AV_PIX_FMT_NONE;
    }
    convert_to_hw = !direct_hw &&
                    ch->input.source_type == VIDEO_SOURCE_HDMI_IN &&
                    warmup_frame != NULL;
    if (ch->input.source_type == VIDEO_SOURCE_HDMI_IN && warmup_frame == NULL) {
        LOGW("%s DRM record optimization skipped: no source frame available for hardware context",
             ch->name);
    }
    if (!direct_hw) {
        if ((convert_to_hw
                 ? video_frame_convert_init_hw(&ch->rec_convert,
                                                 width,
                                                 height,
                                                 AV_PIX_FMT_NV12)
                 : video_frame_convert_init(&ch->rec_convert,
                                              width,
                                              height,
                                              AV_PIX_FMT_NV12)) != APP_OK) {
            return APP_ERR_FFMPEG;
        }
        if (convert_to_hw) {
            hw_encode_frame = video_frame_convert_prepare(&ch->rec_convert,
                                                          warmup_frame,
                                                          &converted);
            if (hw_encode_frame == NULL ||
                hw_encode_frame->format != AV_PIX_FMT_DRM_PRIME ||
                hw_encode_frame->hw_frames_ctx == NULL) {
                LOGE("%s record hardware conversion prewarm failed", ch->name);
                return APP_ERR_FFMPEG;
            }
        }
    }
    if (vp_open_h264_encoder(&ch->rec_enc,
                             width,
                             height,
                             fps,
                             ch->rec_bitrate,
                             fps,
                             (direct_hw || convert_to_hw)
                                 ? AV_PIX_FMT_DRM_PRIME
                                 : AV_PIX_FMT_NV12,
                             hw_encode_frame != NULL
                                 ? hw_encode_frame->hw_frames_ctx
                                 : NULL,
                             1) != APP_OK) {
        return APP_ERR_FFMPEG;
    }

    ch->rec_stream = avformat_new_stream(ch->rec_fmt, NULL);
    if (ch->rec_stream == NULL) {
        return APP_ERR_NOMEM;
    }
    ch->rec_stream->time_base = ch->rec_enc->time_base;
    ch->rec_stream->avg_frame_rate = ch->rec_enc->framerate;

    ret = avcodec_parameters_from_context(ch->rec_stream->codecpar, ch->rec_enc);
    if (ret < 0) {
        LOGE("%s record parameters_from_context failed: %d", ch->name, ret);
        return APP_ERR_FFMPEG;
    }

    if ((ch->rec_fmt->oformat->flags & AVFMT_NOFILE) == 0) {
        ret = avio_open(&ch->rec_fmt->pb, ch->rec_path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOGE("%s record avio_open failed: %d path=%s", ch->name, ret, ch->rec_path);
            return APP_ERR_IO;
        }
    }

    ret = avformat_write_header(ch->rec_fmt, NULL);
    if (ret < 0) {
        LOGE("%s record write_header failed: %d", ch->name, ret);
        return APP_ERR_FFMPEG;
    }
    ch->rec_header_written = 1;

    ch->rec_pkt = av_packet_alloc();
    if (ch->rec_pkt == NULL) {
        return APP_ERR_NOMEM;
    }

    LOGD("%s record output opened %dx%d@%d bitrate=%d input=%s path=%s",
         ch->name,
         width,
         height,
         fps,
         ch->rec_bitrate,
         direct_hw
             ? (av_get_pix_fmt_name(direct_sw_fmt) != NULL
                    ? av_get_pix_fmt_name(direct_sw_fmt)
                    : "drm_prime/unknown")
             : (convert_to_hw ? "drm_prime/nv12 converted" : "nv12 converted"),
         ch->rec_path);
    return APP_OK;
}

static app_status_t vp_record_encode_frame(vp_channel_t *ch,
                                           AVFrame *src_frame,
                                           int64_t *out_convert_us,
                                           int64_t *out_codec_mux_us) {
    const AVFrame *enc_frame;
    int64_t codec_started_us;
    int64_t convert_started_us;
    int converted = 0;
    int ret;

    if (out_convert_us != NULL) {
        *out_convert_us = 0;
    }
    if (out_codec_mux_us != NULL) {
        *out_codec_mux_us = 0;
    }
    convert_started_us = app_get_time_us();
    if (ch->rec_direct_hw) {
        if (!vp_record_direct_frame_matches(ch, src_frame)) {
            LOGE("%s record direct frame mismatch src=%dx%d fmt=%s expected=%dx%d %s",
                 ch->name,
                 src_frame->width,
                 src_frame->height,
                 av_get_pix_fmt_name(vp_record_hw_sw_format(src_frame)) != NULL
                     ? av_get_pix_fmt_name(vp_record_hw_sw_format(src_frame))
                     : "unknown",
                 ch->rec_enc->width,
                 ch->rec_enc->height,
                 av_get_pix_fmt_name(ch->rec_direct_sw_fmt) != NULL
                     ? av_get_pix_fmt_name(ch->rec_direct_sw_fmt)
                     : "unknown");
            return APP_ERR_UNSUPPORTED;
        }
        enc_frame = src_frame;
    } else {
        enc_frame = video_frame_convert_prepare(&ch->rec_convert,
                                                src_frame,
                                                &converted);
    }
    if (out_convert_us != NULL) {
        *out_convert_us = app_get_time_us() - convert_started_us;
    }
    if (enc_frame == NULL) {
        LOGE("%s record convert failed src=%dx%d fmt=%d",
             ch->name, src_frame->width, src_frame->height, src_frame->format);
        return APP_ERR_FFMPEG;
    }

    codec_started_us = app_get_time_us();
    ret = avcodec_send_frame(ch->rec_enc, (AVFrame *)enc_frame);
    if (ret < 0) {
        if (out_codec_mux_us != NULL) {
            *out_codec_mux_us = app_get_time_us() - codec_started_us;
        }
        LOGE("%s record send_frame failed: %d", ch->name, ret);
        return APP_ERR_FFMPEG;
    }

    while ((ret = avcodec_receive_packet(ch->rec_enc, ch->rec_pkt)) >= 0) {
        av_packet_rescale_ts(ch->rec_pkt, ch->rec_enc->time_base, ch->rec_stream->time_base);
        ch->rec_pkt->stream_index = ch->rec_stream->index;
        ret = av_interleaved_write_frame(ch->rec_fmt, ch->rec_pkt);
        av_packet_unref(ch->rec_pkt);
        if (ret < 0) {
            if (out_codec_mux_us != NULL) {
                *out_codec_mux_us = app_get_time_us() - codec_started_us;
            }
            LOGE("%s record write frame failed: %d", ch->name, ret);
            return APP_ERR_IO;
        }
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        if (out_codec_mux_us != NULL) {
            *out_codec_mux_us = app_get_time_us() - codec_started_us;
        }
        return APP_OK;
    }
    if (out_codec_mux_us != NULL) {
        *out_codec_mux_us = app_get_time_us() - codec_started_us;
    }
    LOGE("%s record receive_packet failed: %d", ch->name, ret);
    return APP_ERR_FFMPEG;
}

static void vp_record_close_output(vp_channel_t *ch) {
    int ret;

    if (ch->rec_enc != NULL && ch->rec_pkt != NULL &&
        ch->rec_stream != NULL && ch->rec_fmt != NULL) {
        ret = avcodec_send_frame(ch->rec_enc, NULL);
        if (ret >= 0) {
            while ((ret = avcodec_receive_packet(ch->rec_enc, ch->rec_pkt)) >= 0) {
                av_packet_rescale_ts(ch->rec_pkt, ch->rec_enc->time_base, ch->rec_stream->time_base);
                ch->rec_pkt->stream_index = ch->rec_stream->index;
                av_interleaved_write_frame(ch->rec_fmt, ch->rec_pkt);
                av_packet_unref(ch->rec_pkt);
            }
        }
    }
    if (ch->rec_fmt != NULL && ch->rec_header_written) {
        av_write_trailer(ch->rec_fmt);
    }
    if (ch->rec_pkt != NULL) {
        av_packet_free(&ch->rec_pkt);
    }
    if (ch->rec_enc != NULL) {
        avcodec_free_context(&ch->rec_enc);
    }
    if (ch->rec_fmt != NULL) {
        if ((ch->rec_fmt->oformat->flags & AVFMT_NOFILE) == 0 && ch->rec_fmt->pb != NULL) {
            avio_closep(&ch->rec_fmt->pb);
        }
        avformat_free_context(ch->rec_fmt);
        ch->rec_fmt = NULL;
    }
    ch->rec_stream = NULL;
    ch->rec_header_written = 0;
    ch->rec_direct_hw = 0;
    ch->rec_direct_sw_fmt = AV_PIX_FMT_NONE;
    video_frame_convert_deinit(&ch->rec_convert);
}

static void vp_record_fill_stats(const vp_channel_t *ch, vp_record_stats_t *out_stats) {
    double duration_s;

    if (out_stats == NULL) {
        return;
    }

    memset(out_stats, 0, sizeof(*out_stats));
    snprintf(out_stats->channel, sizeof(out_stats->channel), "%s", ch->name);
    snprintf(out_stats->path, sizeof(out_stats->path), "%s", ch->rec_path);
    out_stats->start_time_us = ch->rec_start_us;
    out_stats->end_time_us = ch->rec_end_us;
    if (ch->rec_end_us > ch->rec_start_us) {
        out_stats->duration_us = ch->rec_end_us - ch->rec_start_us;
    }
    out_stats->total_frames = (uint64_t)ch->rec_frames;
    out_stats->dropped_frames = ch->rec_q.dropped;
    out_stats->queue_peak = ch->rec_q.max_count;
    out_stats->queue_capacity = VP_QUEUE_SIZE;
    duration_s = (double)out_stats->duration_us / 1000000.0;
    if (duration_s > 0.0) {
        out_stats->average_fps = (double)out_stats->total_frames / duration_s;
    }
}

static void *vp_record_thread(void *opaque) {
    vp_channel_t *ch = (vp_channel_t *)opaque;
    vp_ctx_t *p = ch->owner;
    AVFrame *frame;
    app_status_t status;
    int64_t timing_started_us = app_get_time_us();
    int64_t codec_mux_sum_us = 0;
    int timing_frames = 0;
    int last_reported_dropped = 0;

    frame = av_frame_alloc();
    if (frame == NULL) {
        LOGE("%s record av_frame_alloc failed", ch->name);
        return NULL;
    }
    while (1) {
        pthread_mutex_lock(&p->lock);
        while (ch->rec_q.count == 0 && ch->record_wanted && !p->stop) {
            pthread_cond_wait(&ch->rec_cond, &p->lock);
        }
        if (ch->rec_q.count == 0) {
            pthread_mutex_unlock(&p->lock);
            break;
        }
        vp_queue_pop_locked(&ch->rec_q, frame);
        pthread_mutex_unlock(&p->lock);

        {
            int64_t codec_mux_us = 0;

            frame->pts = vp_record_next_frame_pts(ch);
            status = vp_record_encode_frame(ch,
                                            frame,
                                            NULL,
                                            &codec_mux_us);
            codec_mux_sum_us += codec_mux_us;
            timing_frames++;
        }
        if (status != APP_OK) {
            LOGW("%s record encode failed: %s", ch->name, app_status_str(status));
        } else {
            ch->rec_frames++;
        }

        if (app_get_time_us() - timing_started_us >= 1000000) {
            int queue_count;
            int queue_peak;
            int dropped;

            pthread_mutex_lock(&p->lock);
            queue_count = ch->rec_q.count;
            queue_peak = ch->rec_q.max_count;
            dropped = ch->rec_q.dropped;
            ch->record_encode_ms = timing_frames > 0
                                       ? (double)codec_mux_sum_us /
                                             timing_frames / 1000.0
                                       : 0.0;
            ch->record_queue = queue_count;
            ch->record_queue_peak = queue_peak;
            ch->record_dropped = dropped;
            pthread_mutex_unlock(&p->lock);
            if (dropped > last_reported_dropped) {
                LOGW("%s record queue dropped %d frame(s) (total=%d peak=%d capacity=%d)",
                     ch->name,
                     dropped - last_reported_dropped,
                     dropped,
                     queue_peak,
                     VP_QUEUE_SIZE);
                last_reported_dropped = dropped;
            }
            timing_started_us = app_get_time_us();
            codec_mux_sum_us = 0;
            timing_frames = 0;
        }
    }

    vp_record_close_output(ch);

    pthread_mutex_lock(&p->lock);
    if (ch->rec_end_us <= ch->rec_start_us) {
        ch->rec_end_us = app_get_time_us();
    }
    ch->recording = 0;
    pthread_mutex_unlock(&p->lock);

    if (ch->rec_frames > 0) {
        double duration_s = (double)(ch->rec_end_us - ch->rec_start_us) / 1000000.0;
        double average_fps = duration_s > 0.0 ? (double)ch->rec_frames / duration_s : 0.0;

        LOGI("%s record saved: %s duration=%.3fs frames=%d avg_fps=%.2f "
             "dropped=%d queue_peak=%d",
             ch->name,
             ch->rec_path,
             duration_s,
             ch->rec_frames,
             average_fps,
             ch->rec_q.dropped,
             ch->rec_q.max_count);
    } else {
        LOGI("%s record canceled: no frames, no file written", ch->name);
    }
    av_frame_free(&frame);
    return NULL;
}

app_status_t vp_record_start(vp_ctx_t *p, vp_channel_id_t id, const char *mp4_path) {
    vp_channel_t *ch;
    AVFrame *warmup_frame = NULL;
    const AVFrame *prepared_frame;
    app_status_t status;
    int converted = 0;
    int warmup_encoded = 0;
    int64_t warmup_started_us;
    int width;
    int height;
    int fps;

    if (p == NULL || id < 0 || id >= p->channel_count) {
        return APP_ERR_PARAM;
    }
    ch = &p->ch[id];
    if (!ch->is_opened) {
        LOGE("%s record start failed: channel not opened", ch->name);
        return APP_ERR_IO;
    }

    pthread_mutex_lock(&p->lock);
    if (ch->input.source_type == VIDEO_SOURCE_HDMI_IN && !ch->input_has_signal) {
        pthread_mutex_unlock(&p->lock);
        LOGW("%s record start skipped: HDMI has no valid input signal", ch->name);
        return APP_ERR_IO;
    }
    if (ch->record_initializing || ch->record_wanted || ch->recording) {
        pthread_mutex_unlock(&p->lock);
        LOGW("%s record start ignored: already recording", ch->name);
        return APP_ERR_BUSY;
    }

    if (mp4_path != NULL && mp4_path[0] != '\0') {
        strncpy(ch->rec_path, mp4_path, sizeof(ch->rec_path) - 1);
        ch->rec_path[sizeof(ch->rec_path) - 1] = '\0';
    } else {
        if (vp_make_dated_path(ch->record_dir,
                               ch->rec_path,
                               sizeof(ch->rec_path),
                               ".mp4",
                               time(NULL)) != 0) {
            pthread_mutex_unlock(&p->lock);
            LOGE("%s record path create failed: %s", ch->name, ch->record_dir);
            return APP_ERR_IO;
        }
    }
    width = ch->record_output.width > 0
                ? ch->record_output.width
                : (ch->latest != NULL ? ch->latest->width : ch->input.width);
    height = ch->record_output.height > 0
                 ? ch->record_output.height
                 : (ch->latest != NULL ? ch->latest->height : ch->input.height);
    fps = ch->record_output.fps > 0
              ? ch->record_output.fps
              : (ch->input.fps > 0 ? ch->input.fps : 30);
    if (ch->latest != NULL) {
        warmup_frame = av_frame_clone(ch->latest);
        if (warmup_frame == NULL) {
            pthread_mutex_unlock(&p->lock);
            return APP_ERR_NOMEM;
        }
    }
    ch->rec_bitrate = ch->record_output.bitrate;
    ch->record_initializing = 1;
    pthread_mutex_unlock(&p->lock);

    if (width <= 0 || height <= 0) {
        pthread_mutex_lock(&p->lock);
        ch->record_initializing = 0;
        pthread_mutex_unlock(&p->lock);
        av_frame_free(&warmup_frame);
        LOGE("%s record start failed: invalid output size %dx%d", ch->name, width, height);
        return APP_ERR_PARAM;
    }

    LOGD("%s record preparing %dx%d@%d", ch->name, width, height, fps);
    status = vp_record_open_output(ch, width, height, fps, warmup_frame);
    if (status != APP_OK) {
        vp_record_close_output(ch);
        av_frame_free(&warmup_frame);
        pthread_mutex_lock(&p->lock);
        ch->record_initializing = 0;
        pthread_mutex_unlock(&p->lock);
        LOGE("%s record prepare output failed", ch->name);
        return status;
    }

    if (warmup_frame != NULL) {
        warmup_started_us = app_get_time_us();
        prepared_frame = ch->rec_direct_hw
                             ? warmup_frame
                             : video_frame_convert_prepare(&ch->rec_convert,
                                                           warmup_frame,
                                                           &converted);
        if (prepared_frame == NULL) {
            vp_record_close_output(ch);
            av_frame_free(&warmup_frame);
            pthread_mutex_lock(&p->lock);
            ch->record_initializing = 0;
            pthread_mutex_unlock(&p->lock);
            LOGE("%s record conversion warmup failed", ch->name);
            return APP_ERR_FFMPEG;
        }
        warmup_frame->pts = 0;
        status = vp_record_encode_frame(ch, warmup_frame, NULL, NULL);
        if (status != APP_OK) {
            vp_record_close_output(ch);
            av_frame_free(&warmup_frame);
            pthread_mutex_lock(&p->lock);
            ch->record_initializing = 0;
            pthread_mutex_unlock(&p->lock);
            LOGE("%s record encoder warmup failed", ch->name);
            return status;
        }
        warmup_encoded = 1;
        LOGD("%s record %s+encoder warmup complete in %.3f ms",
             ch->name,
             ch->rec_direct_hw ? "direct" : "conversion",
             (double)(app_get_time_us() - warmup_started_us) / 1000.0);
    } else {
        LOGW("%s record conversion warmup skipped: no source frame yet", ch->name);
    }
    av_frame_free(&warmup_frame);

    pthread_mutex_lock(&p->lock);
    if (p->stop) {
        ch->record_initializing = 0;
        pthread_mutex_unlock(&p->lock);
        vp_record_close_output(ch);
        return APP_ERR_BUSY;
    }
    ch->rec_start_us = app_get_time_us();
    ch->rec_end_us = 0;
    ch->rec_frames = warmup_encoded;
    vp_queue_clear_locked(&ch->rec_q);
    ch->rec_q.dropped = 0;
    ch->rec_q.max_count = 0;
    ch->record_encode_ms = 0.0;
    ch->record_queue = 0;
    ch->record_queue_peak = 0;
    ch->record_dropped = 0;
    ch->record_wanted = 1;
    if (pthread_create(&ch->rec_thread, NULL, vp_record_thread, ch) != 0) {
        ch->record_wanted = 0;
        ch->record_initializing = 0;
        pthread_mutex_unlock(&p->lock);
        vp_record_close_output(ch);
        LOGE("%s record thread create failed", ch->name);
        return APP_ERR_IO;
    }
    ch->recording = 1;
    ch->record_initializing = 0;
    pthread_mutex_unlock(&p->lock);

    LOGI("%s record start -> %s", ch->name, ch->rec_path);
    return APP_OK;
}

app_status_t vp_record_start_all(vp_ctx_t *p, const char *dir) {
    char path[APP_PATH_MAX_LEN];
    char channel_dir[APP_PATH_MAX_LEN];
    time_t started_at;
    int started = 0;
    int i;

    if (p == NULL) {
        return APP_ERR_PARAM;
    }

    started_at = time(NULL);

    for (i = 0; i < p->channel_count; ++i) {
        if (!p->ch[i].is_opened) {
            continue;
        }
        if (dir != NULL && dir[0] != '\0') {
            int written = snprintf(channel_dir,
                                   sizeof(channel_dir),
                                   "%s/%s",
                                   dir,
                                   p->ch[i].name);
            if (written < 0 || (size_t)written >= sizeof(channel_dir)) {
                LOGE("%s record directory path is too long", p->ch[i].name);
                continue;
            }
        } else {
            snprintf(channel_dir, sizeof(channel_dir), "%s", p->ch[i].record_dir);
        }
        if (vp_make_dated_path(channel_dir, path, sizeof(path), ".mp4", started_at) != 0) {
            LOGE("%s record path create failed: %s", p->ch[i].name, channel_dir);
            continue;
        }
        if (vp_record_start(p, (vp_channel_id_t)i, path) == APP_OK) {
            started++;
        }
    }

    LOGI("record start all: %d/%d channels", started, p->channel_count);
    return started > 0 ? APP_OK : APP_ERR_IO;
}

app_status_t vp_record_stop_ex(vp_ctx_t *p,
                               vp_channel_id_t id,
                               vp_record_stats_t *out_stats) {
    vp_channel_t *ch;

    if (p == NULL || id < 0 || id >= p->channel_count) {
        return APP_ERR_PARAM;
    }
    ch = &p->ch[id];

    pthread_mutex_lock(&p->lock);
    if (ch->record_initializing) {
        pthread_mutex_unlock(&p->lock);
        LOGW("%s record stop ignored: output is still initializing", ch->name);
        return APP_ERR_BUSY;
    }
    if (!ch->record_wanted && !ch->recording) {
        pthread_mutex_unlock(&p->lock);
        LOGW("%s record stop ignored: not recording", ch->name);
        return APP_ERR_IO;
    }
    ch->record_wanted = 0;
    if (ch->rec_end_us <= ch->rec_start_us) {
        ch->rec_end_us = app_get_time_us();
    }
    pthread_cond_broadcast(&ch->rec_cond);
    pthread_mutex_unlock(&p->lock);

    pthread_join(ch->rec_thread, NULL);
    ch->rec_thread = 0;
    pthread_mutex_lock(&p->lock);
    vp_record_fill_stats(ch, out_stats);
    pthread_mutex_unlock(&p->lock);
    return APP_OK;
}

app_status_t vp_record_stop(vp_ctx_t *p, vp_channel_id_t id) {
    return vp_record_stop_ex(p, id, NULL);
}

app_status_t vp_record_stop_all_ex(vp_ctx_t *p,
                                   vp_record_stats_t *stats,
                                   int stats_capacity,
                                   int *out_count) {
    int stats_count = 0;
    int i;

    if (p == NULL || stats_capacity < 0 ||
        (stats_capacity > 0 && stats == NULL)) {
        return APP_ERR_PARAM;
    }
    /* Signal every recording thread before waiting on any one of them. */
    for (i = 0; i < p->channel_count; ++i) {
        pthread_mutex_lock(&p->lock);
        if (!p->ch[i].record_initializing &&
            (p->ch[i].record_wanted || p->ch[i].recording)) {
            p->ch[i].record_wanted = 0;
            if (p->ch[i].rec_end_us <= p->ch[i].rec_start_us) {
                p->ch[i].rec_end_us = app_get_time_us();
            }
            pthread_cond_broadcast(&p->ch[i].rec_cond);
        }
        pthread_mutex_unlock(&p->lock);
    }

    for (i = 0; i < p->channel_count; ++i) {
        vp_record_stats_t channel_stats;
        int thread_started;

        pthread_mutex_lock(&p->lock);
        thread_started = p->ch[i].rec_thread != 0;
        pthread_mutex_unlock(&p->lock);
        if (!thread_started) {
            continue;
        }
        {
            struct timespec timeout;
            int join_ret;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += 3;
            join_ret = pthread_timedjoin_np(p->ch[i].rec_thread, NULL, &timeout);
            if (join_ret != 0) {
                LOGW("%s record thread did not stop within 3s; continuing", p->ch[i].name);
                continue;
            }
        }
        p->ch[i].rec_thread = 0;
        pthread_mutex_lock(&p->lock);
        vp_record_fill_stats(&p->ch[i], &channel_stats);
        pthread_mutex_unlock(&p->lock);
        if (stats_count < stats_capacity) {
            stats[stats_count++] = channel_stats;
        }
    }
    if (out_count != NULL) {
        *out_count = stats_count;
    }
    return APP_OK;
}

app_status_t vp_record_stop_all(vp_ctx_t *p) {
    return vp_record_stop_all_ex(p, NULL, 0, NULL);
}

int vp_is_recording(vp_ctx_t *p, vp_channel_id_t id) {
    int recording;

    if (p == NULL || id < 0 || id >= p->channel_count) {
        return 0;
    }
    pthread_mutex_lock(&p->lock);
    recording = p->ch[id].record_initializing || p->ch[id].recording;
    pthread_mutex_unlock(&p->lock);
    return recording;
}

app_status_t vp_record_get_path(vp_ctx_t *p,
                                vp_channel_id_t id,
                                char *out_path,
                                size_t out_size) {
    if (p == NULL || id < 0 || id >= p->channel_count ||
        out_path == NULL || out_size == 0) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&p->lock);
    snprintf(out_path, out_size, "%s", p->ch[id].rec_path);
    pthread_mutex_unlock(&p->lock);
    return APP_OK;
}
