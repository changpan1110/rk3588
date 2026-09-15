#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "video_pipeline_stream.c"

#include "video_pipeline_internal.h"

#include <stdint.h>
#include <time.h>

#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>

#include "common/debug.h"

static const char *vp_stream_transport_name(vp_stream_output_t transport) {
    switch (transport) {
        case VP_STREAM_OUTPUT_UDP: return "UDP/RTP";
        case VP_STREAM_OUTPUT_RTSP: return "RTSP";
        case VP_STREAM_OUTPUT_SRT: return "SRT";
        default: return "unknown";
    }
}

static void vp_stream_close_network_output(vp_ctx_t *p) {
    if (p == NULL) {
        return;
    }
    rtsp_output_deinit(&p->rtsp_output);
    srt_output_deinit(&p->srt_output);
    udp_output_deinit(&p->udp_output);
}

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
    if (p->cfg.stream_output.transport == VP_STREAM_OUTPUT_UDP &&
        !udp_output_is_open(&p->udp_output)) {
        const char *destination_ip =
            p->cfg.stream_output.udp_dest_ip[0] != '\0'
                ? p->cfg.stream_output.udp_dest_ip
                : p->cfg.stream_output.rtp_dest_ip;
        int destination_port = p->cfg.stream_output.udp_dest_port > 0
                                   ? p->cfg.stream_output.udp_dest_port
                                   : p->cfg.stream_output.rtp_dest_port;

        status = udp_output_init(&p->udp_output,
                                 destination_ip,
                                 destination_port);
        if (status != APP_OK) {
            return status;
        }
    }
    if (p->cfg.stream_output.transport == VP_STREAM_OUTPUT_RTSP &&
        !rtsp_output_is_open(&p->rtsp_output)) {
        const char *url = p->cfg.stream_output.rtsp_url[0] != '\0'
                              ? p->cfg.stream_output.rtsp_url
                              : VP_DEFAULT_RTSP_URL;
        const char *transport =
            p->cfg.stream_output.rtsp_transport[0] != '\0'
                ? p->cfg.stream_output.rtsp_transport
                : VP_DEFAULT_RTSP_TRANSPORT;

        status = rtsp_output_init(&p->rtsp_output,
                                  url,
                                  transport,
                                  p->stream_enc);
        if (status != APP_OK) {
            return status;
        }
    }
    if (p->cfg.stream_output.transport == VP_STREAM_OUTPUT_SRT &&
        !srt_output_is_open(&p->srt_output)) {
        const char *url = p->cfg.stream_output.srt_url[0] != '\0'
                              ? p->cfg.stream_output.srt_url
                              : VP_DEFAULT_SRT_URL;
        const char *passphrase_file =
            p->cfg.stream_output.srt_passphrase_file[0] != '\0'
                ? p->cfg.stream_output.srt_passphrase_file
                : VP_DEFAULT_SRT_PASSPHRASE_FILE;

        status = srt_output_init(&p->srt_output,
                                 url,
                                 passphrase_file,
                                 p->stream_enc);
        if (status != APP_OK) {
            return status;
        }
    }
    return APP_OK;
}

static void vp_stream_reset_encoder(vp_ctx_t *p) {
    if (p == NULL) {
        return;
    }

    /* Keep RTSP/SRT publishing alive so existing viewers survive a source switch. */
    if (p->stream_pkt != NULL) {
        av_packet_unref(p->stream_pkt);
    }
    if (p->stream_enc != NULL) {
        avcodec_free_context(&p->stream_enc);
    }
}

static int64_t vp_stream_thread_cpu_us(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

void *vp_stream_thread(void *opaque) {
    vp_ctx_t *p = (vp_ctx_t *)opaque;
    AVFrame *frame;
    const AVFrame *converted_frame;
    const AVFrame *enc_frame;
    AVFrame *enc_mut;
    int64_t timing_start_us = app_get_time_us();
    int64_t timing_cpu_start_us = vp_stream_thread_cpu_us();
    int64_t queue_sum_us = 0;
    int64_t convert_sum_us = 0;
    int64_t overlay_sum_us = 0;
    int64_t encode_sum_us = 0;
    int64_t output_sum_us = 0;
    int64_t packet_age_sum_us = 0;
    int64_t packet_age_max_us = 0;
    int timing_frames = 0;
    int timing_encode_calls = 0;
    int timing_packets = 0;
    int converted = 0;
    int force_idr = 0;
    int wait_for_keyframe = 0;
    int encode_fail_count = 0;
    int64_t next_output_retry_us = 0;
    int64_t next_encode_retry_us = 0;
    int ret;

    frame = av_frame_alloc();
    if (frame == NULL) {
        LOGE("stream av_frame_alloc failed");
        return NULL;
    }

    while (!p->stop) {
        uint64_t frame_generation;
        uint64_t reset_generation = 0;
        int reset_encoder = 0;

        pthread_mutex_lock(&p->lock);
        if (p->switch_pending) {
            vp_queue_clear_locked(&p->stream_q);
            p->switch_pending = 0;
            force_idr = 1;
            wait_for_keyframe = 1;
            reset_encoder = 1;
            reset_generation = p->stream_generation;
        }
        if (reset_encoder) {
            pthread_mutex_unlock(&p->lock);
            vp_stream_reset_encoder(p);
            next_output_retry_us = 0;
            LOGI("stream encoder reset for source generation=%llu; "
                 "network publisher kept alive",
                 (unsigned long long)reset_generation);
            continue;
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
        frame_generation = p->stream_generation;
        vp_queue_pop_locked(&p->stream_q, frame);
        pthread_mutex_unlock(&p->lock);

        {
            int64_t convert_start_us = app_get_time_us();
            int64_t capture_us = frame->pts;

            if (capture_us > 0 && convert_start_us >= capture_us) {
                queue_sum_us += convert_start_us - capture_us;
            }
            converted_frame = video_frame_convert_prepare(&p->stream_convert,
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
            enc_frame = osd_rkrga_process(p->stream_label_rkrga,
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
            enc_frame = osd_rkrga_process(p->stream_osd_rkrga,
                                                  enc_frame,
                                                  p->stream_osd_rgba,
                                                  revision);
            overlay_sum_us += app_get_time_us() - overlay_start_us;
        }
        if (enc_frame == NULL) {
            continue;
        }
        {
            int64_t now_us = app_get_time_us();
            app_status_t prepare_status;

            if (next_output_retry_us > now_us) {
                continue;
            }
            prepare_status = vp_stream_prepare_output(p, enc_frame);
            if (prepare_status != APP_OK) {
                LOGW("%s output prepare failed (%s); retrying in %.1fs",
                     vp_stream_transport_name(p->cfg.stream_output.transport),
                     app_status_str(prepare_status),
                     (double)VP_STREAM_RECONNECT_DELAY_US / 1000000.0);
                vp_stream_close_network_output(p);
                next_output_retry_us = now_us + VP_STREAM_RECONNECT_DELAY_US;
                force_idr = 1;
                wait_for_keyframe = 1;
                continue;
            }
            if (next_output_retry_us != 0) {
                LOGI("%s output reconnected",
                     vp_stream_transport_name(p->cfg.stream_output.transport));
                next_output_retry_us = 0;
                force_idr = 1;
                wait_for_keyframe = 1;
            }
        }

        pthread_mutex_lock(&p->lock);
        if (p->switch_pending || frame_generation != p->stream_generation) {
            pthread_mutex_unlock(&p->lock);
            LOGD("discard stale stream frame generation=%llu current=%llu",
                 (unsigned long long)frame_generation,
                 (unsigned long long)p->stream_generation);
            continue;
        }
        pthread_mutex_unlock(&p->lock);

        enc_mut = (AVFrame *)enc_frame;
        enc_mut->pts = frame->pts - p->stream_epoch_us;
        if (force_idr) {
            enc_mut->pict_type = AV_PICTURE_TYPE_I;
        } else {
            enc_mut->pict_type = AV_PICTURE_TYPE_NONE;
        }

        {
            int64_t encode_start_us = app_get_time_us();
            int64_t frame_output_us = 0;

            if (next_encode_retry_us > encode_start_us) {
                continue;
            }

            ret = avcodec_send_frame(p->stream_enc, enc_mut);
            if (ret < 0) {
                encode_fail_count++;
                LOGW("stream send_frame failed: %d (consecutive=%d)",
                     ret,
                     encode_fail_count);
                if (encode_fail_count >= 10) {
                    LOGW("stream encoder failed repeatedly; resetting encoder");
                    vp_stream_reset_encoder(p);
                    encode_fail_count = 0;
                    force_idr = 1;
                    wait_for_keyframe = 1;
                    next_encode_retry_us = encode_start_us + 100000;
                } else {
                    next_encode_retry_us = encode_start_us + 10000;
                }
                continue;
            }
            encode_fail_count = 0;
            next_encode_retry_us = 0;
            timing_encode_calls++;
            while ((ret = avcodec_receive_packet(p->stream_enc,
                                                  p->stream_pkt)) == 0) {
                int64_t packet_capture_us =
                    p->stream_pkt->pts != AV_NOPTS_VALUE
                        ? p->stream_pkt->pts + p->stream_epoch_us
                        : frame->pts;
                int64_t packet_age_us;
                int64_t output_start_us;
                app_status_t write_status = APP_OK;

                if (wait_for_keyframe &&
                    (p->stream_pkt->flags & AV_PKT_FLAG_KEY) == 0) {
                    LOGW("drop non-key packet while waiting for switched source IDR");
                    av_packet_unref(p->stream_pkt);
                    continue;
                }
                if (wait_for_keyframe) {
                    LOGD("stream switch IDR ready generation=%llu size=%d",
                         (unsigned long long)frame_generation,
                         p->stream_pkt->size);
                    wait_for_keyframe = 0;
                    force_idr = 0;
                }

                output_start_us = app_get_time_us();
                if (p->cfg.stream_output.transport == VP_STREAM_OUTPUT_RTSP) {
                    write_status = rtsp_output_send(&p->rtsp_output,
                                                    p->stream_pkt);
                } else if (p->cfg.stream_output.transport ==
                           VP_STREAM_OUTPUT_SRT) {
                    write_status = srt_output_send(&p->srt_output,
                                                   p->stream_pkt);
                } else {
                    int64_t packet_pts_us =
                        p->stream_pkt->pts != AV_NOPTS_VALUE
                            ? av_rescale_q(p->stream_pkt->pts,
                                           p->stream_enc->time_base,
                                           (AVRational){1, 1000000})
                            : app_get_time_us() - p->stream_epoch_us;

                    write_status = udp_output_send(
                        &p->udp_output,
                        p->stream_pkt->data,
                        (size_t)p->stream_pkt->size,
                        packet_pts_us);
                }
                frame_output_us += app_get_time_us() - output_start_us;
                if (write_status != APP_OK) {
                    av_packet_unref(p->stream_pkt);
                    LOGW("%s output failed; reconnecting in %.1fs",
                         vp_stream_transport_name(
                             p->cfg.stream_output.transport),
                         (double)VP_STREAM_RECONNECT_DELAY_US / 1000000.0);
                    vp_stream_close_network_output(p);
                    next_output_retry_us =
                        app_get_time_us() + VP_STREAM_RECONNECT_DELAY_US;
                    force_idr = 1;
                    wait_for_keyframe = 1;
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
            encode_sum_us += app_get_time_us() - encode_start_us - frame_output_us;
            output_sum_us += frame_output_us;
        }
        p->stream_frames++;

        if (app_get_time_us() - timing_start_us >= 1000000) {
            int stream_dropped;
            int64_t timing_end_us = app_get_time_us();
            int64_t timing_cpu_end_us = vp_stream_thread_cpu_us();
            vp_stream_stats_t stats;

            pthread_mutex_lock(&p->lock);
            stream_dropped = p->stream_q.dropped;
            stats.frames = (uint64_t)timing_frames;
            stats.cpu_ms = (double)(timing_cpu_end_us - timing_cpu_start_us) / 1000.0;
            stats.cpu_load = timing_end_us > timing_start_us
                                 ? (double)(timing_cpu_end_us - timing_cpu_start_us) *
                                       100.0 / (double)(timing_end_us - timing_start_us)
                                 : 0.0;
            stats.queue_ms = timing_frames > 0
                                 ? (double)queue_sum_us / timing_frames / 1000.0
                                 : 0.0;
            stats.convert_ms = timing_frames > 0
                                   ? (double)convert_sum_us / timing_frames / 1000.0
                                   : 0.0;
            stats.overlay_ms = timing_frames > 0
                                   ? (double)overlay_sum_us / timing_frames / 1000.0
                                   : 0.0;
            stats.encode_ms = timing_encode_calls > 0
                                  ? (double)encode_sum_us / timing_encode_calls / 1000.0
                                  : 0.0;
            stats.send_ms = timing_packets > 0
                                ? (double)output_sum_us / timing_packets / 1000.0
                                : 0.0;
            stats.packet_age_ms = timing_packets > 0
                                      ? (double)packet_age_sum_us / timing_packets / 1000.0
                                      : 0.0;
            stats.packet_age_max_ms = (double)packet_age_max_us / 1000.0;
            stats.queue_drop = stream_dropped;
            p->stream_stats = stats;
            pthread_mutex_unlock(&p->lock);

            timing_start_us = app_get_time_us();
            timing_cpu_start_us = vp_stream_thread_cpu_us();
            queue_sum_us = 0;
            convert_sum_us = 0;
            overlay_sum_us = 0;
            encode_sum_us = 0;
            output_sum_us = 0;
            packet_age_sum_us = 0;
            packet_age_max_us = 0;
            timing_frames = 0;
            timing_encode_calls = 0;
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
    if (!p->ch[id].is_opened && !p->ch[id].capture_reconnect) {
        LOGE("stream select failed: %s not opened", vp_channel_name(p, id));
        return APP_ERR_IO;
    }

    pthread_mutex_lock(&p->lock);
    if (id != p->stream_ch) {
        p->stream_ch = id;
        p->stream_generation++;
        p->stream_generation_start_us = app_get_time_us();
        p->switch_pending = 1;
        pthread_cond_broadcast(&p->stream_cond);
        LOGI("stream switch -> %s generation=%llu (encoder reset + IDR)",
             vp_channel_name(p, id),
             (unsigned long long)p->stream_generation);
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
    const char *udp_destination_ip;
    int udp_destination_port;
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

    udp_destination_ip = p->cfg.stream_output.udp_dest_ip[0] != '\0'
                             ? p->cfg.stream_output.udp_dest_ip
                             : p->cfg.stream_output.rtp_dest_ip;
    udp_destination_port = p->cfg.stream_output.udp_dest_port > 0
                               ? p->cfg.stream_output.udp_dest_port
                               : p->cfg.stream_output.rtp_dest_port;
    if (new_enabled &&
        p->cfg.stream_output.transport == VP_STREAM_OUTPUT_UDP &&
        !udp_output_is_open(&p->udp_output)) {
        status = udp_output_init(&p->udp_output,
                                 udp_destination_ip,
                                 udp_destination_port);
        if (status != APP_OK) {
            LOGE("stream enable failed: RTP/UDP output open failed");
            return status;
        }
    }

    pthread_mutex_lock(&p->lock);
    if (p->stream_enabled != new_enabled) {
        p->stream_enabled = new_enabled;
        vp_queue_clear_locked(&p->stream_q);
        p->stream_generation++;
        p->stream_generation_start_us = app_get_time_us();
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
