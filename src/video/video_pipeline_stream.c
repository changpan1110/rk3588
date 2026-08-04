#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "video_pipeline_stream.c"

#include "video_pipeline_internal.h"

#include <stdint.h>

#include <libavutil/hwcontext.h>

#include "common/debug.h"

static app_status_t vp_stream_prepare_output(vp_ctx_t *p,
                                             const AVFrame *hw_frame) {
    int width;
    int height;
    int fps;
    int bitrate;
    app_status_t status;

    if (p == NULL || hw_frame == NULL ||
        hw_frame->format != AV_PIX_FMT_DRM_PRIME ||
        hw_frame->hw_frames_ctx == NULL) {
        return APP_ERR_PARAM;
    }

    width = p->cfg.stream_output.video.width > 0
                ? p->cfg.stream_output.video.width
                : 1920;
    height = p->cfg.stream_output.video.height > 0
                 ? p->cfg.stream_output.video.height
                 : 1080;
    fps = p->cfg.stream_output.video.fps > 0
              ? p->cfg.stream_output.video.fps
              : 30;
    bitrate = p->cfg.stream_output.video.bitrate > 0
                  ? p->cfg.stream_output.video.bitrate
                  : VP_STREAM_DEFAULT_BITRATE;

    if (p->stream_enc == NULL) {
        status = vp_open_h264_encoder(&p->stream_enc,
                                      width,
                                      height,
                                      fps,
                                      bitrate,
                                      fps,
                                      AV_PIX_FMT_DRM_PRIME,
                                      hw_frame->hw_frames_ctx,
                                      p->cfg.stream_output.transport ==
                                          VP_STREAM_OUTPUT_RTSP);
        if (status != APP_OK) {
            return status;
        }
    }
    if (p->stream_pkt == NULL) {
        p->stream_pkt = av_packet_alloc();
        if (p->stream_pkt == NULL) {
            return APP_ERR_NOMEM;
        }
    }
    if (p->cfg.stream_output.transport == VP_STREAM_OUTPUT_RTSP &&
        p->rtsp_fmt == NULL) {
        const char *url = p->cfg.stream_output.rtsp_url[0] != '\0'
                              ? p->cfg.stream_output.rtsp_url
                              : VP_DEFAULT_RTSP_URL;

        status = vp_rtsp_open(p, url);
        if (status != APP_OK) {
            return status;
        }
    }
    return APP_OK;
}

void *vp_stream_thread(void *opaque) {
    vp_ctx_t *p = (vp_ctx_t *)opaque;
    AVFrame *frame;
    const AVFrame *converted_frame;
    const AVFrame *enc_frame;
    AVFrame *enc_mut;
    int64_t timing_start_us = app_get_time_us();
    int64_t queue_sum_us = 0;
    int64_t convert_sum_us = 0;
    int64_t overlay_sum_us = 0;
    int64_t packet_age_sum_us = 0;
    int64_t packet_age_max_us = 0;
    int timing_frames = 0;
    int timing_packets = 0;
    int converted = 0;
    int force_idr = 0;
    int ret;

    frame = av_frame_alloc();
    if (frame == NULL) {
        LOGE("stream av_frame_alloc failed");
        return NULL;
    }

    while (!p->stop) {
        pthread_mutex_lock(&p->lock);
        if (p->switch_pending) {
            vp_queue_clear_locked(&p->stream_q);
            p->switch_pending = 0;
            force_idr = 1;
        }
        while ((p->stream_q.count == 0 || !p->stream_enabled) &&
               !p->stop && !p->switch_pending) {
            pthread_cond_wait(&p->stream_cond, &p->lock);
        }
        if (p->stop || p->switch_pending) {
            pthread_mutex_unlock(&p->lock);
            continue;
        }
        if (!p->stream_enabled) {
            pthread_mutex_unlock(&p->lock);
            continue;
        }
        vp_queue_pop_locked(&p->stream_q, frame);
        pthread_mutex_unlock(&p->lock);

        {
            int64_t convert_start_us = app_get_time_us();
            int64_t capture_us = frame->pts;

            if (capture_us > 0 && convert_start_us >= capture_us) {
                queue_sum_us += convert_start_us - capture_us;
            }
            converted_frame = output_encode_prepare_frame(&p->stream_convert,
                                                           frame,
                                                           &converted);
            convert_sum_us += app_get_time_us() - convert_start_us;
            timing_frames++;
        }
        if (converted_frame == NULL) {
            continue;
        }

        enc_frame = converted_frame;
        if (p->cfg.source_label.enabled) {
            const vp_source_label_config_t *label = &p->cfg.source_label;
            vp_channel_id_t label_channel = vp_stream_current(p);

            if (label_channel < 0 || label_channel >= p->channel_count) {
                continue;
            }
            if (p->stream_label_channel != label_channel) {
                if (vp_osd_render_source_label(
                        p->ch[label_channel].osd_label,
                        label->font_path,
                        label->font_size,
                        p->stream_label_rgba) != APP_OK) {
                    LOGW("source label render failed: channel=%s text=%s",
                         vp_channel_name(p, label_channel),
                         p->ch[label_channel].osd_label);
                    continue;
                }
                p->stream_label_channel = label_channel;
                p->stream_label_revision++;
                LOGI("source label updated: channel=%s text=%s position=(%d,%d)",
                     vp_channel_name(p, label_channel),
                     p->ch[label_channel].osd_label,
                     label->x,
                     label->y);
            }
            enc_frame = output_osd_rkrga_process(p->stream_label_rkrga,
                                                  enc_frame,
                                                  p->stream_label_rgba,
                                                  p->stream_label_revision);
            if (enc_frame == NULL) {
                continue;
            }
        }

        {
            vp_osd_params_t osd;
            uint64_t recording_seconds;
            uint64_t revision;
            int64_t now_us;
            int64_t recording_start_us = 0;
            vp_channel_id_t current_channel;
            int recording;
            int blink_on;
            int snapshot_flash_on;
            int64_t overlay_start_us = app_get_time_us();

            vp_osd_get_snapshot(p, &osd);
            now_us = app_get_time_us();
            current_channel = vp_stream_current(p);
            pthread_mutex_lock(&p->lock);
            recording = osd.show_crosshair && current_channel >= 0 &&
                        current_channel < p->channel_count &&
                        p->ch[current_channel].record_wanted &&
                        p->ch[current_channel].recording;
            if (recording) {
                recording_start_us = p->ch[current_channel].rec_start_us;
            }
            pthread_mutex_unlock(&p->lock);
            blink_on = recording && ((now_us / 500000LL) & 1LL) == 0;
            recording_seconds = recording && now_us > recording_start_us
                                    ? (uint64_t)((now_us - recording_start_us) /
                                                 1000000LL)
                                    : 0;
            snapshot_flash_on = osd.show_crosshair &&
                                now_us < osd.snapshot_flash_until_us &&
                                ((now_us / 150000LL) & 1LL) == 0;
            revision = ((uint64_t)osd.seq << 32) |
                       (recording_seconds & 0x1fffffffULL) |
                       ((uint64_t)(recording != 0) << 29) |
                       ((uint64_t)(blink_on != 0) << 30) |
                       ((uint64_t)(snapshot_flash_on != 0) << 31);
            if (p->stream_osd_revision != revision) {
                if (vp_osd_render_rgba(&osd,
                                       recording_seconds,
                                       recording,
                                       blink_on,
                                       snapshot_flash_on,
                                       p->stream_osd_rgba) != APP_OK) {
                    LOGW("stream OSD RGBA render failed");
                    continue;
                }
                p->stream_osd_revision = revision;
            }
            enc_frame = output_osd_rkrga_process(p->stream_osd_rkrga,
                                                  enc_frame,
                                                  p->stream_osd_rgba,
                                                  revision);
            overlay_sum_us += app_get_time_us() - overlay_start_us;
        }
        if (enc_frame == NULL) {
            continue;
        }
        if (vp_stream_prepare_output(p, enc_frame) != APP_OK) {
            LOGW("stream hardware encoder/output prepare failed");
            vp_stream_set_enabled(p, 0);
            continue;
        }

        enc_mut = (AVFrame *)enc_frame;
        enc_mut->pts = frame->pts - p->stream_epoch_us;
        if (force_idr) {
            enc_mut->pict_type = AV_PICTURE_TYPE_I;
            force_idr = 0;
        }

        ret = avcodec_send_frame(p->stream_enc, enc_mut);
        if (ret < 0) {
            LOGW("stream send_frame failed: %d", ret);
            continue;
        }
        while ((ret = avcodec_receive_packet(p->stream_enc, p->stream_pkt)) == 0) {
            int64_t packet_capture_us = p->stream_pkt->pts != AV_NOPTS_VALUE
                                            ? p->stream_pkt->pts + p->stream_epoch_us
                                            : frame->pts;
            int64_t packet_age_us;
            app_status_t write_status = APP_OK;

            if (p->cfg.stream_output.transport == VP_STREAM_OUTPUT_RTSP) {
                write_status = vp_rtsp_write_packet(p, p->stream_pkt);
            } else {
                vp_rtp_send_packet(p, p->stream_pkt->data, p->stream_pkt->size, p->stream_pkt->pts);
            }
            if (write_status != APP_OK) {
                av_packet_unref(p->stream_pkt);
                LOGW("RTSP connection lost; stream encoding paused, use 'stream on' to reconnect");
                vp_rtsp_close(p);
                vp_stream_set_enabled(p, 0);
                break;
            }
            packet_age_us = app_get_time_us() - packet_capture_us;
            if (packet_age_us >= 0) {
                packet_age_sum_us += packet_age_us;
                if (packet_age_us > packet_age_max_us) {
                    packet_age_max_us = packet_age_us;
                }
            }
            timing_packets++;
            av_packet_unref(p->stream_pkt);
        }
        p->stream_frames++;

        if (app_get_time_us() - timing_start_us >= 1000000) {
            vp_channel_id_t current = vp_stream_current(p);

            LOGI("stream timing source=%s frames=%d packets=%d queue=%.2fms "
                 "convert=%.2fms overlay=%.2fms packet_age=%.2fms max=%.2fms",
                 vp_channel_name(p, current),
                 timing_frames,
                 timing_packets,
                 timing_frames > 0
                     ? (double)queue_sum_us / timing_frames / 1000.0
                     : 0.0,
                 timing_frames > 0
                     ? (double)convert_sum_us / timing_frames / 1000.0
                     : 0.0,
                 timing_frames > 0
                     ? (double)overlay_sum_us / timing_frames / 1000.0
                     : 0.0,
                 timing_packets > 0
                     ? (double)packet_age_sum_us / timing_packets / 1000.0
                     : 0.0,
                 (double)packet_age_max_us / 1000.0);
            timing_start_us = app_get_time_us();
            queue_sum_us = 0;
            convert_sum_us = 0;
            overlay_sum_us = 0;
            packet_age_sum_us = 0;
            packet_age_max_us = 0;
            timing_frames = 0;
            timing_packets = 0;
        }
    }

    av_frame_free(&frame);
    return NULL;
}

app_status_t vp_stream_select(vp_ctx_t *p, vp_channel_id_t id) {
    if (p == NULL || id < 0 || id >= p->channel_count) {
        return APP_ERR_PARAM;
    }
    if (!p->ch[id].is_opened) {
        LOGE("stream select failed: %s not opened", vp_channel_name(p, id));
        return APP_ERR_IO;
    }

    pthread_mutex_lock(&p->lock);
    if (id != p->stream_ch) {
        p->stream_ch = id;
        p->switch_pending = 1;
        pthread_cond_broadcast(&p->stream_cond);
        LOGI("stream switch -> %s (IDR forced)", vp_channel_name(p, id));
    }
    pthread_mutex_unlock(&p->lock);
    return APP_OK;
}

vp_channel_id_t vp_stream_current(vp_ctx_t *p) {
    vp_channel_id_t cur;

    if (p == NULL) {
        return VP_CHANNEL_INVALID;
    }
    pthread_mutex_lock(&p->lock);
    cur = p->stream_ch;
    pthread_mutex_unlock(&p->lock);
    return cur;
}

app_status_t vp_stream_set_enabled(vp_ctx_t *p, int enabled) {
    app_status_t status;
    const char *url;
    int current_enabled;
    int new_enabled;

    if (p == NULL) {
        return APP_ERR_PARAM;
    }
    new_enabled = enabled != 0;

    pthread_mutex_lock(&p->lock);
    current_enabled = p->stream_enabled;
    pthread_mutex_unlock(&p->lock);
    if (current_enabled == new_enabled) {
        return APP_OK;
    }

    if (new_enabled &&
        p->cfg.stream_output.transport == VP_STREAM_OUTPUT_RTSP &&
        p->rtsp_fmt == NULL &&
        p->stream_enc != NULL) {
        url = p->cfg.stream_output.rtsp_url[0] != '\0'
                  ? p->cfg.stream_output.rtsp_url
                  : VP_DEFAULT_RTSP_URL;
        status = vp_rtsp_open(p, url);
        if (status != APP_OK) {
            LOGE("stream enable failed: RTSP reconnect failed");
            return status;
        }
    }

    pthread_mutex_lock(&p->lock);
    if (p->stream_enabled != new_enabled) {
        p->stream_enabled = new_enabled;
        vp_queue_clear_locked(&p->stream_q);
        p->switch_pending = 1;
        pthread_cond_broadcast(&p->stream_cond);
    }
    pthread_mutex_unlock(&p->lock);
    LOGI("stream %s", new_enabled ? "enabled" : "disabled");
    return APP_OK;
}

int vp_stream_is_enabled(vp_ctx_t *p) {
    int enabled;

    if (p == NULL) {
        return 0;
    }
    pthread_mutex_lock(&p->lock);
    enabled = p->stream_enabled;
    pthread_mutex_unlock(&p->lock);
    return enabled;
}
