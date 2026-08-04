#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "video_pipeline.c"

#include "video_pipeline_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "common/debug.h"
#include "output/output_recorder.h"

#define VP_STREAM_OSD_WIDTH 400
#define VP_STREAM_OSD_HEIGHT 96
#define VP_STREAM_OSD_MARGIN 32
#define VP_SOURCE_LABEL_DEFAULT_X 32
#define VP_SOURCE_LABEL_DEFAULT_Y 32
#define VP_SOURCE_LABEL_DEFAULT_WIDTH 360
#define VP_SOURCE_LABEL_DEFAULT_HEIGHT 72
#define VP_SOURCE_LABEL_DEFAULT_FONT_SIZE 38
#define VP_SOURCE_LABEL_DEFAULT_FONT_PATH \
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"

int vp_channel_count(vp_ctx_t *p) {
    return p != NULL ? p->channel_count : 0;
}

const char *vp_channel_name(vp_ctx_t *p, vp_channel_id_t id) {
    if (p == NULL || id < 0 || id >= p->channel_count) {
        return "unknown";
    }
    return p->ch[id].name;
}

int vp_channel_find(vp_ctx_t *p, const char *name, vp_channel_id_t *out) {
    int i;

    if (p == NULL || name == NULL || out == NULL) {
        return -1;
    }
    for (i = 0; i < p->channel_count; ++i) {
        if (strcmp(p->ch[i].name, name) == 0) {
            *out = (vp_channel_id_t)i;
            return 0;
        }
    }
    return -1;
}

app_status_t vp_init(vp_ctx_t **out_ctx, const vp_config_t *cfg) {
    vp_ctx_t *p;
    app_status_t status;
    int i;
    int stream_w;
    int stream_h;
    int stream_fps;
    vp_stream_output_t stream_output;

    if (out_ctx == NULL || cfg == NULL || cfg->channels == NULL || cfg->channel_count <= 0) {
        return APP_ERR_PARAM;
    }

    p = (vp_ctx_t *)calloc(1, sizeof(*p));
    if (p == NULL) {
        return APP_ERR_NOMEM;
    }
    p->cfg = *cfg;
    p->channel_count = cfg->channel_count;
    p->ch = (vp_channel_t *)calloc((size_t)p->channel_count, sizeof(*p->ch));
    if (p->ch == NULL) {
        free(p);
        return APP_ERR_NOMEM;
    }
    p->udp_output.socket_fd = -1;
    p->stream_enabled = 1;
    p->stream_ch = VP_CHANNEL_INVALID;

    if (cfg->storage_dir != NULL && cfg->storage_dir[0] != '\0') {
        strncpy(p->storage_dir, cfg->storage_dir, sizeof(p->storage_dir) - 1);
    } else {
        strncpy(p->storage_dir, VP_DEFAULT_DIR, sizeof(p->storage_dir) - 1);
    }
    if (vp_mkdir_p(p->storage_dir) != 0) {
        LOGW("storage dir %s create failed: %s", p->storage_dir, strerror(errno));
    }

    pthread_mutex_init(&p->lock, NULL);
    pthread_mutex_init(&p->osd_lock, NULL);
    pthread_cond_init(&p->stream_cond, NULL);

    for (i = 0; i < p->channel_count; ++i) {
        vp_channel_t *ch = &p->ch[i];

        ch->owner = p;
        ch->id = (vp_channel_id_t)i;
        ch->input = cfg->channels[i].input;
        ch->record_output = cfg->channels[i].record_output;
        ch->rec_bitrate = ch->record_output.bitrate;
        if (ch->input.name[0] != '\0') {
            strncpy(ch->name, ch->input.name, sizeof(ch->name) - 1);
        } else {
            snprintf(ch->name, sizeof(ch->name), "channel%d", i);
        }
        snprintf(ch->osd_label,
                 sizeof(ch->osd_label),
                 "%s",
                 cfg->channels[i].osd_label != NULL &&
                         cfg->channels[i].osd_label[0] != '\0'
                     ? cfg->channels[i].osd_label
                     : ch->name);
        if (cfg->channels[i].record_dir != NULL && cfg->channels[i].record_dir[0] != '\0') {
            snprintf(ch->record_dir, sizeof(ch->record_dir), "%s", cfg->channels[i].record_dir);
        } else {
            snprintf(ch->record_dir, sizeof(ch->record_dir),
                     "%s/record/%s", p->storage_dir, ch->name);
        }
        if (cfg->channels[i].snapshot_dir != NULL && cfg->channels[i].snapshot_dir[0] != '\0') {
            snprintf(ch->snapshot_dir, sizeof(ch->snapshot_dir), "%s", cfg->channels[i].snapshot_dir);
        } else {
            snprintf(ch->snapshot_dir, sizeof(ch->snapshot_dir),
                     "%s/snapshot/%s", p->storage_dir, ch->name);
        }
        if (vp_mkdir_p(ch->record_dir) != 0) {
            LOGW("%s record dir %s create failed: %s", ch->name, ch->record_dir, strerror(errno));
        }
        if (vp_mkdir_p(ch->snapshot_dir) != 0) {
            LOGW("%s snapshot dir %s create failed: %s", ch->name, ch->snapshot_dir, strerror(errno));
        }
        pthread_cond_init(&ch->rec_cond, NULL);

        status = vp_channel_open(ch);
        if (status != APP_OK) {
            LOGE("%s input open failed: %s (channel disabled)", ch->name, app_status_str(status));
            ch->is_opened = 0;
            continue;
        }
        ch->is_opened = 1;

        if (pthread_create(&ch->cap_thread, NULL, vp_capture_thread, ch) != 0) {
            LOGE("%s capture thread create failed", ch->name);
            vp_channel_close(ch);
            ch->is_opened = 0;
            continue;
        }
        LOGI("%s capture started: %s %dx%d@%d fmt=%s",
             ch->name,
             ch->input.device,
             ch->input.width,
             ch->input.height,
             ch->input.fps,
             ch->input.input_format);
    }

    for (i = 0; i < p->channel_count; ++i) {
        if (p->ch[i].is_opened) {
            p->stream_ch = (vp_channel_id_t)i;
            break;
        }
    }
    if (p->stream_ch == VP_CHANNEL_INVALID) {
        LOGE("no channel available");
        vp_deinit(p);
        return APP_ERR_IO;
    }

    stream_w = cfg->stream_output.video.width > 0 ? cfg->stream_output.video.width : 1920;
    stream_h = cfg->stream_output.video.height > 0 ? cfg->stream_output.video.height : 1080;
    stream_fps = cfg->stream_output.video.fps > 0 ? cfg->stream_output.video.fps : 30;
    stream_output = cfg->stream_output.transport;
    if (stream_output != VP_STREAM_OUTPUT_RTP && stream_output != VP_STREAM_OUTPUT_RTSP) {
        stream_output = VP_STREAM_OUTPUT_RTP;
    }
    p->cfg.stream_output.transport = stream_output;
    if (p->cfg.source_label.enabled) {
        vp_source_label_config_t *label = &p->cfg.source_label;

        if (label->x < 0) {
            label->x = VP_SOURCE_LABEL_DEFAULT_X;
        }
        if (label->y < 0) {
            label->y = VP_SOURCE_LABEL_DEFAULT_Y;
        }
        if (label->width <= 0) {
            label->width = VP_SOURCE_LABEL_DEFAULT_WIDTH;
        }
        if (label->height <= 0) {
            label->height = VP_SOURCE_LABEL_DEFAULT_HEIGHT;
        }
        if (label->font_size <= 0) {
            label->font_size = VP_SOURCE_LABEL_DEFAULT_FONT_SIZE;
        }
        if (label->font_path == NULL || label->font_path[0] == '\0') {
            label->font_path = VP_SOURCE_LABEL_DEFAULT_FONT_PATH;
        }
        if (label->x + label->width > stream_w ||
            label->y + label->height > stream_h) {
            LOGE("source label area (%d,%d %dx%d) exceeds stream %dx%d",
                 label->x,
                 label->y,
                 label->width,
                 label->height,
                 stream_w,
                 stream_h);
            vp_deinit(p);
            return APP_ERR_PARAM;
        }
    }
    status = output_encode_convert_init_hw(&p->stream_convert,
                                           stream_w,
                                           stream_h,
                                           AV_PIX_FMT_NV12);
    if (status != APP_OK) {
        vp_deinit(p);
        return status;
    }
    status = output_osd_rkrga_init(&p->stream_osd_rkrga,
                                   stream_w,
                                   stream_h,
                                   VP_STREAM_OSD_WIDTH,
                                   VP_STREAM_OSD_HEIGHT,
                                   stream_w - VP_STREAM_OSD_WIDTH -
                                       VP_STREAM_OSD_MARGIN,
                                   VP_STREAM_OSD_MARGIN);
    if (status != APP_OK) {
        vp_deinit(p);
        return status;
    }
    p->stream_osd_rgba = av_frame_alloc();
    if (p->stream_osd_rgba == NULL) {
        vp_deinit(p);
        return APP_ERR_NOMEM;
    }
    p->stream_osd_rgba->format = AV_PIX_FMT_RGBA;
    p->stream_osd_rgba->width = VP_STREAM_OSD_WIDTH;
    p->stream_osd_rgba->height = VP_STREAM_OSD_HEIGHT;
    if (av_frame_get_buffer(p->stream_osd_rgba, 64) < 0) {
        vp_deinit(p);
        return APP_ERR_NOMEM;
    }
    p->stream_osd_revision = UINT64_MAX;

    if (p->cfg.source_label.enabled) {
        const vp_source_label_config_t *label = &p->cfg.source_label;

        status = output_osd_rkrga_init(&p->stream_label_rkrga,
                                       stream_w,
                                       stream_h,
                                       label->width,
                                       label->height,
                                       label->x,
                                       label->y);
        if (status != APP_OK) {
            vp_deinit(p);
            return status;
        }
        p->stream_label_rgba = av_frame_alloc();
        if (p->stream_label_rgba == NULL) {
            vp_deinit(p);
            return APP_ERR_NOMEM;
        }
        p->stream_label_rgba->format = AV_PIX_FMT_RGBA;
        p->stream_label_rgba->width = label->width;
        p->stream_label_rgba->height = label->height;
        if (av_frame_get_buffer(p->stream_label_rgba, 64) < 0) {
            vp_deinit(p);
            return APP_ERR_NOMEM;
        }
        p->stream_label_channel = VP_CHANNEL_INVALID;
        p->stream_label_revision = 0;
        LOGI("source label enabled: position=(%d,%d) size=%dx%d font=%s %dpx",
             label->x,
             label->y,
             label->width,
             label->height,
             label->font_path,
             label->font_size);
    }

    if (stream_output == VP_STREAM_OUTPUT_RTP &&
        cfg->stream_output.rtp_dest_port > 0 &&
        cfg->stream_output.rtp_dest_ip[0] != '\0') {
        if (output_stream_udp_init(&p->udp_output,
                                   cfg->stream_output.rtp_dest_ip,
                                   cfg->stream_output.rtp_dest_port) != APP_OK) {
            LOGW("udp open failed, streaming disabled");
            p->stream_enabled = 0;
        }
    } else if (stream_output == VP_STREAM_OUTPUT_RTP) {
        LOGW("stream dest not configured, streaming disabled");
        p->stream_enabled = 0;
    } else {
        LOGI("RTSP publish will open after the first hardware OSD frame");
    }

    p->stream_epoch_us = app_get_time_us();
    if (pthread_create(&p->stream_thread, NULL, vp_stream_thread, p) != 0) {
        LOGE("stream thread create failed");
        vp_deinit(p);
        return APP_ERR_IO;
    }

    LOGI("stream queue mode=latest-frame (stale frames dropped)");
    LOGI("pipeline init ok, channels=%d stream source=%s",
         p->channel_count, vp_channel_name(p, p->stream_ch));
    *out_ctx = p;
    return APP_OK;
}

app_status_t vp_snapshot_ex(vp_ctx_t *p,
                            vp_channel_id_t id,
                            const char *jpg_path,
                            char *out_path,
                            size_t out_size) {
    vp_channel_t *ch;
    AVFrame *frame;
    app_status_t status;
    char path[APP_PATH_MAX_LEN];
    int ret;

    if (out_path != NULL && out_size > 0) {
        out_path[0] = '\0';
    }
    if (p == NULL || id < 0 || id >= p->channel_count ||
        (out_path != NULL && out_size == 0)) {
        return APP_ERR_PARAM;
    }
    ch = &p->ch[id];

    if (jpg_path != NULL && jpg_path[0] != '\0') {
        strncpy(path, jpg_path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        if (vp_make_dated_path(ch->snapshot_dir, path, sizeof(path), ".jpg", time(NULL)) != 0) {
            LOGE("%s snapshot path create failed: %s", ch->name, ch->snapshot_dir);
            return APP_ERR_IO;
        }
    }
    if (out_path != NULL) {
        snprintf(out_path, out_size, "%s", path);
    }

    frame = av_frame_alloc();
    if (frame == NULL) {
        return APP_ERR_NOMEM;
    }

    pthread_mutex_lock(&p->lock);
    if (ch->latest == NULL) {
        pthread_mutex_unlock(&p->lock);
        av_frame_free(&frame);
        LOGW("%s snapshot failed: no frame yet", ch->name);
        return APP_ERR_IO;
    }
    ret = av_frame_ref(frame, ch->latest);
    pthread_mutex_unlock(&p->lock);
    if (ret < 0) {
        LOGE("%s snapshot frame reference failed: %d", ch->name, ret);
        av_frame_free(&frame);
        return APP_ERR_FFMPEG;
    }

    status = output_recorder_write_jpeg(frame, path);
    av_frame_free(&frame);

    if (status == APP_OK) {
        LOGI("%s snapshot saved: %s", ch->name, path);
    }
    return status;
}

app_status_t vp_snapshot(vp_ctx_t *p, vp_channel_id_t id, const char *jpg_path) {
    return vp_snapshot_ex(p, id, jpg_path, NULL, 0);
}

void vp_get_channel_stats(vp_ctx_t *p, vp_channel_id_t id, vp_channel_stats_t *out) {
    if (p == NULL || out == NULL || id < 0 || id >= p->channel_count) {
        return;
    }
    pthread_mutex_lock(&p->lock);
    out->captured = p->ch[id].cap_frames;
    out->recording = p->ch[id].record_initializing || p->ch[id].recording;
    out->rec_frames = p->ch[id].rec_frames;
    out->rec_dropped = p->ch[id].rec_q.dropped;
    pthread_mutex_unlock(&p->lock);
}

uint64_t vp_get_stream_frames(vp_ctx_t *p) {
    uint64_t frames;

    if (p == NULL) {
        return 0;
    }
    pthread_mutex_lock(&p->lock);
    frames = p->stream_frames;
    pthread_mutex_unlock(&p->lock);
    return frames;
}

void vp_deinit(vp_ctx_t *p) {
    int i;

    if (p == NULL) {
        return;
    }

    pthread_mutex_lock(&p->lock);
    p->stop = 1;
    for (i = 0; i < p->channel_count; ++i) {
        pthread_cond_broadcast(&p->ch[i].rec_cond);
    }
    pthread_cond_broadcast(&p->stream_cond);
    pthread_mutex_unlock(&p->lock);

    for (i = 0; i < p->channel_count; ++i) {
        vp_channel_t *ch = &p->ch[i];

        if (ch->record_wanted || ch->recording) {
            vp_record_stop(p, (vp_channel_id_t)i);
        }
        if (ch->is_opened && ch->cap_thread != 0) {
            pthread_join(ch->cap_thread, NULL);
            ch->cap_thread = 0;
        }
    }
    if (p->stream_thread != 0) {
        pthread_join(p->stream_thread, NULL);
        p->stream_thread = 0;
    }
    output_stream_rtsp_deinit(&p->rtsp_output);
    if (p->stream_enc != NULL) {
        avcodec_free_context(&p->stream_enc);
    }
    if (p->stream_pkt != NULL) {
        av_packet_free(&p->stream_pkt);
    }
    output_osd_rkrga_deinit(&p->stream_osd_rkrga);
    if (p->stream_osd_rgba != NULL) {
        av_frame_free(&p->stream_osd_rgba);
    }
    output_osd_rkrga_deinit(&p->stream_label_rkrga);
    if (p->stream_label_rgba != NULL) {
        av_frame_free(&p->stream_label_rgba);
    }
    output_encode_convert_deinit(&p->stream_convert);
    vp_queue_free(&p->stream_q);

    for (i = 0; i < p->channel_count; ++i) {
        vp_channel_t *ch = &p->ch[i];

        if (ch->latest != NULL) {
            av_frame_free(&ch->latest);
        }
        vp_queue_free(&ch->rec_q);
        if (ch->is_opened) {
            vp_channel_close(ch);
        }
        pthread_cond_destroy(&ch->rec_cond);
    }

    output_stream_udp_deinit(&p->udp_output);
    pthread_cond_destroy(&p->stream_cond);
    pthread_mutex_destroy(&p->lock);
    pthread_mutex_destroy(&p->osd_lock);
    LOGI("pipeline deinit");
    free(p->ch);
    free(p);
}
