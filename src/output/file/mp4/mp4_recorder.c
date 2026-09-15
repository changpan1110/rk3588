#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "mp4_recorder.c"

#include "output/file/mp4/mp4_recorder.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libavformat/avformat.h>

#include "common/debug.h"
#include "input/video/input_csi.h"
#include "input/video/input_hdmi_in.h"
#include "input/video/input_usb.h"
#include "process/video/video_frame_convert.h"

#define MP4_RECORDER_QUEUE_SIZE 8
#define MP4_RECORDER_DEFAULT_BITRATE 4000000
#define MP4_RECORDER_DEFAULT_DIR "/tmp"

struct mp4_recorder_ctx {
    mp4_recorder_config_t cfg;
    char encoder_name[32];
    char storage_dir[APP_PATH_MAX_LEN];
    union {
        input_usb_ctx_t usb;
        input_csi_ctx_t csi;
        input_hdmi_in_ctx_t hdmi_in;
    } input;

    /* 采集线程 -> 编码线程 的帧队列 */
    pthread_t capture_thread;
    pthread_t encode_thread;
    pthread_mutex_t queue_lock;
    pthread_cond_t queue_cond;
    AVFrame *queue_frames[MP4_RECORDER_QUEUE_SIZE];
    int queue_head;
    int queue_count;

    /* 运行状态(queue_lock 保护) */
    int stop_requested;
    int capture_done;
    int recording;
    int record_start_request;
    int record_stop_request;
    char pending_record_path[APP_PATH_MAX_LEN];

    /* 拍照用的最新帧(独立锁,不阻塞编码路径) */
    pthread_mutex_t latest_lock;
    AVFrame *latest_frame;
    pthread_mutex_t snapshot_lock;

    /* 编码器 / mp4 封装(仅编码线程访问) */
    video_frame_convert_ctx_t nv12_convert;
    const AVCodec *codec;
    AVCodecContext *codec_ctx;
    AVPacket *packet;
    AVFormatContext *ofmt_ctx;
    AVStream *video_stream;
    int header_written;
    char record_path[APP_PATH_MAX_LEN];
    int64_t record_start_us;

    int captured_frames;
    int encoded_frames;
    int encoded_packets;
    int dropped_frames;
};

static void mp4_recorder_emit(mp4_recorder_ctx_t *ctx, mp4_recorder_event_t event, const char *path) {
    if (ctx->cfg.event_cb != NULL) {
        ctx->cfg.event_cb(event, path, ctx->cfg.event_user_data);
    }
}

/* 递归创建目录(类似 mkdir -p),已存在不算错误 */
static int mp4_recorder_mkdir_p(const char *dir) {
    char tmp[APP_PATH_MAX_LEN];
    char *p;
    size_t len;

    if (dir == NULL || dir[0] == '\0') {
        return -1;
    }

    snprintf(tmp, sizeof(tmp), "%s", dir);
    len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (p = tmp + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* 在配置的存储目录下生成 prefix_时间戳.ext 的完整路径 */
static void mp4_recorder_make_auto_path(mp4_recorder_ctx_t *ctx,
                                           char *out,
                                           size_t out_size,
                                           const char *prefix,
                                           const char *ext) {
    time_t now = time(NULL);
    struct tm tm_now;
    char stamp[32];

    localtime_r(&now, &tm_now);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_now);
    snprintf(out, out_size, "%s/%s_%s%s", ctx->storage_dir, prefix, stamp, ext);
}

static app_status_t mp4_recorder_open_input(mp4_recorder_ctx_t *ctx) {
    switch (ctx->cfg.input.source_type) {
        case VIDEO_SOURCE_USB:
            return input_usb_open_hw(&ctx->input.usb, &ctx->cfg.input);
        case VIDEO_SOURCE_CSI0:
        case VIDEO_SOURCE_CSI1:
            return input_csi_open_hw(&ctx->input.csi, &ctx->cfg.input);
        case VIDEO_SOURCE_HDMI_IN:
            return input_hdmi_in_open(&ctx->input.hdmi_in, &ctx->cfg.input);
        default:
            return APP_ERR_PARAM;
    }
}

static app_status_t mp4_recorder_read_input(mp4_recorder_ctx_t *ctx, video_frame_t *frame) {
    switch (ctx->cfg.input.source_type) {
        case VIDEO_SOURCE_USB:
            return input_usb_read(&ctx->input.usb, frame);
        case VIDEO_SOURCE_CSI0:
        case VIDEO_SOURCE_CSI1:
            return input_csi_read(&ctx->input.csi, frame);
        case VIDEO_SOURCE_HDMI_IN:
            return input_hdmi_in_read(&ctx->input.hdmi_in, frame);
        default:
            return APP_ERR_PARAM;
    }
}

static void mp4_recorder_close_input(mp4_recorder_ctx_t *ctx) {
    switch (ctx->cfg.input.source_type) {
        case VIDEO_SOURCE_USB:
            input_usb_close(&ctx->input.usb);
            break;
        case VIDEO_SOURCE_CSI0:
        case VIDEO_SOURCE_CSI1:
            input_csi_close(&ctx->input.csi);
            break;
        case VIDEO_SOURCE_HDMI_IN:
            input_hdmi_in_close(&ctx->input.hdmi_in);
            break;
        default:
            break;
    }
}

static app_status_t mp4_recorder_queue_push(mp4_recorder_ctx_t *ctx, const AVFrame *frame) {
    AVFrame *dst_frame;
    int slot;
    int ret;

    pthread_mutex_lock(&ctx->queue_lock);
    if (ctx->queue_count == MP4_RECORDER_QUEUE_SIZE) {
        /* 队列满丢最老的一帧,保实时性 */
        slot = ctx->queue_head;
        if (ctx->queue_frames[slot] != NULL) {
            av_frame_unref(ctx->queue_frames[slot]);
        }
        ctx->queue_head = (ctx->queue_head + 1) % MP4_RECORDER_QUEUE_SIZE;
        ctx->queue_count--;
        ctx->dropped_frames++;
    }

    slot = (ctx->queue_head + ctx->queue_count) % MP4_RECORDER_QUEUE_SIZE;
    if (ctx->queue_frames[slot] == NULL) {
        ctx->queue_frames[slot] = av_frame_alloc();
        if (ctx->queue_frames[slot] == NULL) {
            pthread_mutex_unlock(&ctx->queue_lock);
            return APP_ERR_NOMEM;
        }
    } else {
        av_frame_unref(ctx->queue_frames[slot]);
    }

    dst_frame = ctx->queue_frames[slot];
    ret = av_frame_ref(dst_frame, frame);
    if (ret < 0) {
        LOGE("recorder queue av_frame_ref failed: %d", ret);
        pthread_mutex_unlock(&ctx->queue_lock);
        return APP_ERR_FFMPEG;
    }

    ctx->queue_count++;
    ctx->captured_frames++;
    pthread_cond_signal(&ctx->queue_cond);
    pthread_mutex_unlock(&ctx->queue_lock);
    return APP_OK;
}

/*
 * 队列空时等待;record/stop/deinit 请求或采集结束都会唤醒。
 * 返回 APP_ERR_EOF 表示当前无帧可取,编码线程去处理状态变化。
 */
static app_status_t mp4_recorder_queue_pop(mp4_recorder_ctx_t *ctx, AVFrame *dst_frame) {
    AVFrame *src_frame;
    int ret;

    pthread_mutex_lock(&ctx->queue_lock);
    while (ctx->queue_count == 0 &&
           !ctx->capture_done &&
           !ctx->stop_requested &&
           !ctx->record_start_request &&
           !ctx->record_stop_request) {
        pthread_cond_wait(&ctx->queue_cond, &ctx->queue_lock);
    }

    if (ctx->queue_count == 0) {
        pthread_mutex_unlock(&ctx->queue_lock);
        return APP_ERR_EOF;
    }

    src_frame = ctx->queue_frames[ctx->queue_head];
    av_frame_unref(dst_frame);
    ret = av_frame_ref(dst_frame, src_frame);
    if (ret < 0) {
        LOGE("recorder queue pop av_frame_ref failed: %d", ret);
        pthread_mutex_unlock(&ctx->queue_lock);
        return APP_ERR_FFMPEG;
    }

    av_frame_unref(src_frame);
    ctx->queue_head = (ctx->queue_head + 1) % MP4_RECORDER_QUEUE_SIZE;
    ctx->queue_count--;
    pthread_mutex_unlock(&ctx->queue_lock);
    return APP_OK;
}

static const AVCodec *mp4_recorder_find_encoder(const char *preferred_name) {
    const AVCodec *codec;

    codec = avcodec_find_encoder_by_name(preferred_name);
    if (codec != NULL) {
        return codec;
    }

    codec = avcodec_find_encoder_by_name("h264_v4l2m2m");
    if (codec != NULL) {
        LOGW("encoder %s not found, fallback to h264_v4l2m2m", preferred_name);
        return codec;
    }

    return NULL;
}

static app_status_t mp4_recorder_open_output(mp4_recorder_ctx_t *ctx, int width, int height, int fps) {
    int ret;

    ctx->codec = mp4_recorder_find_encoder(ctx->encoder_name);
    if (ctx->codec == NULL) {
        LOGE("hardware encoder not found: %s", ctx->encoder_name);
        return APP_ERR_UNSUPPORTED;
    }

    ret = avformat_alloc_output_context2(&ctx->ofmt_ctx, NULL, "mp4", ctx->record_path);
    if (ret < 0 || ctx->ofmt_ctx == NULL) {
        LOGE("avformat_alloc_output_context2 failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    ctx->codec_ctx = avcodec_alloc_context3(ctx->codec);
    if (ctx->codec_ctx == NULL) {
        return APP_ERR_NOMEM;
    }

    ctx->codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    ctx->codec_ctx->codec_id = AV_CODEC_ID_H264;
    ctx->codec_ctx->width = width;
    ctx->codec_ctx->height = height;
    ctx->codec_ctx->pix_fmt = AV_PIX_FMT_NV12;
    ctx->codec_ctx->time_base = (AVRational){1, 1000000};
    ctx->codec_ctx->framerate = (AVRational){fps > 0 ? fps : 30, 1};
    ctx->codec_ctx->bit_rate = ctx->cfg.bitrate;
    ctx->codec_ctx->gop_size = ctx->cfg.gop;
    ctx->codec_ctx->max_b_frames = 0;
    if ((ctx->ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        ctx->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    ret = avcodec_open2(ctx->codec_ctx, ctx->codec, NULL);
    if (ret < 0) {
        LOGE("avcodec_open2 failed for %s: %d", ctx->codec->name, ret);
        return APP_ERR_FFMPEG;
    }

    ctx->video_stream = avformat_new_stream(ctx->ofmt_ctx, NULL);
    if (ctx->video_stream == NULL) {
        return APP_ERR_NOMEM;
    }
    ctx->video_stream->time_base = ctx->codec_ctx->time_base;
    ctx->video_stream->avg_frame_rate = ctx->codec_ctx->framerate;

    ret = avcodec_parameters_from_context(ctx->video_stream->codecpar, ctx->codec_ctx);
    if (ret < 0) {
        LOGE("avcodec_parameters_from_context failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    if ((ctx->ofmt_ctx->oformat->flags & AVFMT_NOFILE) == 0) {
        ret = avio_open(&ctx->ofmt_ctx->pb, ctx->record_path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOGE("avio_open failed: %d path=%s", ret, ctx->record_path);
            return APP_ERR_IO;
        }
    }

    ret = avformat_write_header(ctx->ofmt_ctx, NULL);
    if (ret < 0) {
        LOGE("avformat_write_header failed: %d", ret);
        return APP_ERR_FFMPEG;
    }
    ctx->header_written = 1;

    ctx->packet = av_packet_alloc();
    if (ctx->packet == NULL) {
        return APP_ERR_NOMEM;
    }

    LOGD("recorder output opened encoder=%s %dx%d@%d bitrate=%d path=%s",
         ctx->codec->name,
         width,
         height,
         fps > 0 ? fps : 30,
         ctx->cfg.bitrate,
         ctx->record_path);
    return APP_OK;
}

static app_status_t mp4_recorder_write_packets(mp4_recorder_ctx_t *ctx) {
    int ret;

    while ((ret = avcodec_receive_packet(ctx->codec_ctx, ctx->packet)) >= 0) {
        av_packet_rescale_ts(ctx->packet, ctx->codec_ctx->time_base, ctx->video_stream->time_base);
        ctx->packet->stream_index = ctx->video_stream->index;
        ret = av_interleaved_write_frame(ctx->ofmt_ctx, ctx->packet);
        if (ret < 0) {
            LOGE("av_interleaved_write_frame failed: %d", ret);
            av_packet_unref(ctx->packet);
            return APP_ERR_IO;
        }
        ctx->encoded_packets++;
        av_packet_unref(ctx->packet);
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return APP_OK;
    }

    LOGE("avcodec_receive_packet failed: %d", ret);
    return APP_ERR_FFMPEG;
}

static app_status_t mp4_recorder_encode_frame(mp4_recorder_ctx_t *ctx, AVFrame *src_frame) {
    const AVFrame *encode_frame;
    int converted = 0;
    int ret;

    if (ctx->nv12_convert.dst_width != src_frame->width ||
        ctx->nv12_convert.dst_height != src_frame->height ||
        ctx->nv12_convert.dst_fmt != AV_PIX_FMT_NV12) {
        video_frame_convert_deinit(&ctx->nv12_convert);
        ret = video_frame_convert_init(&ctx->nv12_convert,
                                         src_frame->width,
                                         src_frame->height,
                                         AV_PIX_FMT_NV12);
        if (ret != APP_OK) {
            return ret;
        }
    }

    encode_frame = video_frame_convert_prepare(&ctx->nv12_convert, src_frame, &converted);
    if (encode_frame == NULL) {
        LOGE("recorder prepare nv12 frame failed src=%dx%d fmt=%d",
             src_frame->width,
             src_frame->height,
             src_frame->format);
        return APP_ERR_FFMPEG;
    }

    ret = avcodec_send_frame(ctx->codec_ctx, encode_frame);
    if (ret < 0) {
        LOGE("avcodec_send_frame failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    return mp4_recorder_write_packets(ctx);
}

static app_status_t mp4_recorder_flush_encoder(mp4_recorder_ctx_t *ctx) {
    int ret;

    ret = avcodec_send_frame(ctx->codec_ctx, NULL);
    if (ret < 0) {
        LOGE("avcodec_send_frame flush failed: %d", ret);
        return APP_ERR_FFMPEG;
    }
    return mp4_recorder_write_packets(ctx);
}

/*
 * 录像收尾:flush 编码器 -> 写 trailer(moov)-> 关闭释放。
 * 只有经过这里,mp4 才是完整可播的文件。
 */
static void mp4_recorder_finalize_record(mp4_recorder_ctx_t *ctx) {
    int64_t duration_us;

    if (!ctx->recording) {
        return;
    }

    if (ctx->codec_ctx != NULL) {
        if (mp4_recorder_flush_encoder(ctx) != APP_OK) {
            LOGW("recorder flush encoder failed on finalize");
        }
    }
    if (ctx->ofmt_ctx != NULL && ctx->header_written) {
        av_write_trailer(ctx->ofmt_ctx);
    }
    if (ctx->packet != NULL) {
        av_packet_free(&ctx->packet);
    }
    if (ctx->codec_ctx != NULL) {
        avcodec_free_context(&ctx->codec_ctx);
    }
    if (ctx->ofmt_ctx != NULL) {
        if ((ctx->ofmt_ctx->oformat->flags & AVFMT_NOFILE) == 0 && ctx->ofmt_ctx->pb != NULL) {
            avio_closep(&ctx->ofmt_ctx->pb);
        }
        avformat_free_context(ctx->ofmt_ctx);
        ctx->ofmt_ctx = NULL;
    }
    ctx->codec = NULL;
    ctx->video_stream = NULL;
    ctx->header_written = 0;

    duration_us = app_get_time_us() - ctx->record_start_us;
    ctx->recording = 0;
    ctx->record_stop_request = 0;

    if (ctx->encoded_frames > 0) {
        LOGI("record stop path=%s frames=%d packets=%d duration=%.2fs",
             ctx->record_path,
             ctx->encoded_frames,
             ctx->encoded_packets,
             (double)duration_us / 1000000.0);
        mp4_recorder_emit(ctx, MP4_RECORDER_EVENT_RECORD_FINISHED, ctx->record_path);
    } else {
        LOGI("record canceled: no frames encoded, no file written");
        mp4_recorder_emit(ctx, MP4_RECORDER_EVENT_RECORD_CANCELED, NULL);
    }
}

static void *mp4_recorder_capture_thread(void *opaque) {
    mp4_recorder_ctx_t *ctx = (mp4_recorder_ctx_t *)opaque;
    video_frame_t frame;
    app_status_t status;
    int want_push;

    memset(&frame, 0, sizeof(frame));
    frame.av_frame = av_frame_alloc();
    if (frame.av_frame == NULL) {
        LOGE("recorder capture av_frame_alloc failed");
        pthread_mutex_lock(&ctx->queue_lock);
        ctx->capture_done = 1;
        pthread_cond_broadcast(&ctx->queue_cond);
        pthread_mutex_unlock(&ctx->queue_lock);
        return NULL;
    }

    while (!ctx->stop_requested) {
        av_frame_unref(frame.av_frame);
        status = mp4_recorder_read_input(ctx, &frame);
        if (status != APP_OK) {
            if (status != APP_ERR_EOF) {
                LOGW("recorder capture read failed: %s", app_status_str(status));
            }
            usleep(10000);
            continue;
        }

        /* 拍照用:永远保留最新一帧 */
        pthread_mutex_lock(&ctx->latest_lock);
        if (ctx->latest_frame == NULL) {
            ctx->latest_frame = av_frame_alloc();
        }
        if (ctx->latest_frame != NULL) {
            av_frame_unref(ctx->latest_frame);
            av_frame_ref(ctx->latest_frame, frame.av_frame);
        }
        pthread_mutex_unlock(&ctx->latest_lock);

        /* 只有录像中且未请求停止时才进编码队列 */
        pthread_mutex_lock(&ctx->queue_lock);
        want_push = ctx->recording && !ctx->record_stop_request && !ctx->stop_requested;
        pthread_mutex_unlock(&ctx->queue_lock);
        if (want_push) {
            status = mp4_recorder_queue_push(ctx, frame.av_frame);
            if (status != APP_OK) {
                LOGW("recorder queue push failed: %s", app_status_str(status));
            }
        }
    }

    av_frame_free(&frame.av_frame);
    pthread_mutex_lock(&ctx->queue_lock);
    ctx->capture_done = 1;
    pthread_cond_broadcast(&ctx->queue_cond);
    pthread_mutex_unlock(&ctx->queue_lock);
    return NULL;
}

static void *mp4_recorder_encode_thread(void *opaque) {
    mp4_recorder_ctx_t *ctx = (mp4_recorder_ctx_t *)opaque;
    AVFrame *encode_frame;
    app_status_t status;
    int fps;
    int finalize;
    int is_recording;
    int emit_started;
    char started_path[APP_PATH_MAX_LEN];

    encode_frame = av_frame_alloc();
    if (encode_frame == NULL) {
        LOGE("recorder encode av_frame_alloc failed");
        return NULL;
    }

    fps = ctx->cfg.input.fps > 0 ? ctx->cfg.input.fps : 30;

    while (1) {
        /* record_start 请求:置录像状态,编码器等第一帧到了再按实际尺寸打开 */
        emit_started = 0;
        pthread_mutex_lock(&ctx->queue_lock);
        if (ctx->record_start_request) {
            if (!ctx->recording) {
                ctx->recording = 1;
                ctx->record_start_us = app_get_time_us();
                ctx->encoded_frames = 0;
                ctx->encoded_packets = 0;
                strncpy(ctx->record_path, ctx->pending_record_path, sizeof(ctx->record_path) - 1);
                ctx->record_path[sizeof(ctx->record_path) - 1] = '\0';
                LOGI("record start -> %s", ctx->record_path);
                emit_started = 1;
                strncpy(started_path, ctx->record_path, sizeof(started_path) - 1);
                started_path[sizeof(started_path) - 1] = '\0';
            }
            ctx->record_start_request = 0;
        }
        /* deinit 时如果还在录像,按 stop 流程正常收尾 */
        if (ctx->stop_requested && ctx->recording && !ctx->record_stop_request) {
            ctx->record_stop_request = 1;
            pthread_cond_broadcast(&ctx->queue_cond);
        }
        pthread_mutex_unlock(&ctx->queue_lock);
        /* 回调放在锁外,避免用户在回调里再调 API 造成死锁 */
        if (emit_started) {
            mp4_recorder_emit(ctx, MP4_RECORDER_EVENT_RECORD_STARTED, started_path);
        }

        status = mp4_recorder_queue_pop(ctx, encode_frame);
        if (status == APP_OK) {
            pthread_mutex_lock(&ctx->queue_lock);
            is_recording = ctx->recording;
            pthread_mutex_unlock(&ctx->queue_lock);

            if (is_recording) {
                if (ctx->codec_ctx == NULL) {
                    status = mp4_recorder_open_output(ctx,
                                                         encode_frame->width,
                                                         encode_frame->height,
                                                         fps);
                    if (status != APP_OK) {
                        LOGE("recorder open output failed: %s", app_status_str(status));
                        mp4_recorder_emit(ctx, MP4_RECORDER_EVENT_ERROR, ctx->record_path);
                        pthread_mutex_lock(&ctx->queue_lock);
                        ctx->record_stop_request = 1;
                        pthread_mutex_unlock(&ctx->queue_lock);
                        continue;
                    }
                }

                encode_frame->pts = app_get_time_us() - ctx->record_start_us;
                status = mp4_recorder_encode_frame(ctx, encode_frame);
                if (status != APP_OK) {
                    LOGW("recorder encode frame failed: %s", app_status_str(status));
                }
                ctx->encoded_frames++;
            }
            continue;
        }

        /* 队列空:检查是否要收尾录像 / 退出 */
        pthread_mutex_lock(&ctx->queue_lock);
        finalize = ctx->recording && ctx->record_stop_request && ctx->queue_count == 0;
        pthread_mutex_unlock(&ctx->queue_lock);

        if (finalize) {
            mp4_recorder_finalize_record(ctx);
        }

        pthread_mutex_lock(&ctx->queue_lock);
        is_recording = ctx->recording;
        pthread_mutex_unlock(&ctx->queue_lock);
        if (ctx->stop_requested && !is_recording) {
            break;
        }
    }

    av_frame_free(&encode_frame);
    return NULL;
}

app_status_t mp4_recorder_init(mp4_recorder_ctx_t **out_ctx, const mp4_recorder_config_t *cfg) {
    mp4_recorder_ctx_t *ctx;
    app_status_t status;

    if (out_ctx == NULL || cfg == NULL) {
        return APP_ERR_PARAM;
    }

    ctx = (mp4_recorder_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return APP_ERR_NOMEM;
    }

    ctx->cfg = *cfg;
    strncpy(ctx->encoder_name,
            cfg->encoder_name != NULL ? cfg->encoder_name : "h264_rkmpp",
            sizeof(ctx->encoder_name) - 1);
    if (cfg->storage_dir != NULL && cfg->storage_dir[0] != '\0') {
        strncpy(ctx->storage_dir, cfg->storage_dir, sizeof(ctx->storage_dir) - 1);
    } else {
        strncpy(ctx->storage_dir, MP4_RECORDER_DEFAULT_DIR, sizeof(ctx->storage_dir) - 1);
    }
    if (mp4_recorder_mkdir_p(ctx->storage_dir) != 0) {
        LOGW("recorder storage dir %s create failed: %s (record/snapshot may fail)",
             ctx->storage_dir,
             strerror(errno));
    }
    if (ctx->cfg.bitrate <= 0) {
        ctx->cfg.bitrate = MP4_RECORDER_DEFAULT_BITRATE;
    }
    if (ctx->cfg.gop <= 0) {
        ctx->cfg.gop = cfg->input.fps > 0 ? cfg->input.fps : 30;
    }

    status = mp4_recorder_open_input(ctx);
    if (status != APP_OK) {
        LOGE("recorder open input %s failed: %s", cfg->input.device, app_status_str(status));
        free(ctx);
        return status;
    }

    if (pthread_mutex_init(&ctx->queue_lock, NULL) != 0 ||
        pthread_cond_init(&ctx->queue_cond, NULL) != 0 ||
        pthread_mutex_init(&ctx->latest_lock, NULL) != 0 ||
        pthread_mutex_init(&ctx->snapshot_lock, NULL) != 0) {
        LOGE("recorder pthread init failed");
        mp4_recorder_close_input(ctx);
        free(ctx);
        return APP_ERR_IO;
    }

    if (pthread_create(&ctx->capture_thread, NULL, mp4_recorder_capture_thread, ctx) != 0) {
        LOGE("recorder pthread_create capture failed");
        mp4_recorder_close_input(ctx);
        free(ctx);
        return APP_ERR_IO;
    }
    if (pthread_create(&ctx->encode_thread, NULL, mp4_recorder_encode_thread, ctx) != 0) {
        LOGE("recorder pthread_create encode failed");
        pthread_mutex_lock(&ctx->queue_lock);
        ctx->stop_requested = 1;
        pthread_cond_broadcast(&ctx->queue_cond);
        pthread_mutex_unlock(&ctx->queue_lock);
        pthread_join(ctx->capture_thread, NULL);
        mp4_recorder_close_input(ctx);
        free(ctx);
        return APP_ERR_IO;
    }

    LOGI("recorder init ok: %s %dx%d@%d fmt=%s storage=%s",
         cfg->input.device,
         cfg->input.width,
         cfg->input.height,
         cfg->input.fps,
         cfg->input.input_format,
         ctx->storage_dir);
    *out_ctx = ctx;
    return APP_OK;
}

app_status_t mp4_recorder_record_start(mp4_recorder_ctx_t *ctx, const char *mp4_path) {
    if (ctx == NULL) {
        return APP_ERR_PARAM;
    }

    pthread_mutex_lock(&ctx->queue_lock);
    if (ctx->stop_requested) {
        pthread_mutex_unlock(&ctx->queue_lock);
        return APP_ERR_IO;
    }
    if (ctx->recording || ctx->record_start_request) {
        pthread_mutex_unlock(&ctx->queue_lock);
        LOGW("record start ignored: already recording %s", ctx->record_path);
        return APP_ERR_BUSY;
    }

    if (mp4_path != NULL && mp4_path[0] != '\0') {
        strncpy(ctx->pending_record_path, mp4_path, sizeof(ctx->pending_record_path) - 1);
        ctx->pending_record_path[sizeof(ctx->pending_record_path) - 1] = '\0';
    } else {
        mp4_recorder_make_auto_path(ctx, ctx->pending_record_path, sizeof(ctx->pending_record_path), "record", ".mp4");
    }
    ctx->record_start_request = 1;
    pthread_cond_broadcast(&ctx->queue_cond);
    pthread_mutex_unlock(&ctx->queue_lock);
    return APP_OK;
}

app_status_t mp4_recorder_record_stop(mp4_recorder_ctx_t *ctx) {
    if (ctx == NULL) {
        return APP_ERR_PARAM;
    }

    pthread_mutex_lock(&ctx->queue_lock);
    if (!ctx->recording) {
        pthread_mutex_unlock(&ctx->queue_lock);
        LOGW("record stop ignored: not recording");
        return APP_ERR_IO;
    }
    ctx->record_stop_request = 1;
    pthread_cond_broadcast(&ctx->queue_cond);
    pthread_mutex_unlock(&ctx->queue_lock);
    return APP_OK;
}

app_status_t mp4_recorder_write_jpeg(const AVFrame *src_frame, const char *path) {
    video_frame_convert_ctx_t hw_to_nv12;
    video_frame_convert_ctx_t jpeg_convert;
    const AVFrame *jpeg_source;
    const AVFrame *yuvj_frame;
    const AVCodec *codec;
    AVCodecContext *enc = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    FILE *fp = NULL;
    app_status_t result = APP_OK;
    int converted = 0;
    int ret;

    if (src_frame == NULL || path == NULL) {
        return APP_ERR_PARAM;
    }

    frame = av_frame_clone(src_frame);
    if (frame == NULL) {
        return APP_ERR_FFMPEG;
    }
    frame->pts = 0;

    memset(&hw_to_nv12, 0, sizeof(hw_to_nv12));
    memset(&jpeg_convert, 0, sizeof(jpeg_convert));
    jpeg_source = frame;

    /*
     * External V4L2/RKMPP DRM frames cannot always be downloaded through
     * av_hwframe_transfer_data().  Let RKRGA consume the DMA-BUF and produce
     * a CPU NV12 frame first, then use swscale for the JPEG-only conversion.
     */
    if (frame->format == AV_PIX_FMT_DRM_PRIME) {
        ret = video_frame_convert_init(&hw_to_nv12,
                                         frame->width,
                                         frame->height,
                                         AV_PIX_FMT_NV12);
        if (ret != APP_OK) {
            result = ret;
            goto out;
        }
        jpeg_source = video_frame_convert_prepare(&hw_to_nv12, frame, &converted);
        if (jpeg_source == NULL || jpeg_source->format != AV_PIX_FMT_NV12) {
            LOGE("snapshot DRM to NV12 conversion failed src=%dx%d fmt=%d",
                 frame->width,
                 frame->height,
                 frame->format);
            result = APP_ERR_FFMPEG;
            goto out;
        }
        LOGD("snapshot hardware frame downloaded through RKRGA: %dx%d nv12",
             jpeg_source->width,
             jpeg_source->height);
    }

    /* MJPEG accepts YUVJ420P; CPU NV12/YUV input is converted by swscale. */
    converted = 0;
    ret = video_frame_convert_init(&jpeg_convert,
                                     jpeg_source->width,
                                     jpeg_source->height,
                                     AV_PIX_FMT_YUVJ420P);
    if (ret != APP_OK) {
        result = ret;
        goto out;
    }
    yuvj_frame = video_frame_convert_prepare(&jpeg_convert, jpeg_source, &converted);
    if (yuvj_frame == NULL) {
        LOGE("snapshot convert to yuvj420p failed src=%dx%d fmt=%d",
             jpeg_source->width,
             jpeg_source->height,
             jpeg_source->format);
        result = APP_ERR_FFMPEG;
        goto out;
    }

    codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (codec == NULL) {
        LOGE("snapshot mjpeg encoder not found");
        result = APP_ERR_UNSUPPORTED;
        goto out;
    }

    enc = avcodec_alloc_context3(codec);
    packet = av_packet_alloc();
    fp = fopen(path, "wb");
    if (enc == NULL || packet == NULL || fp == NULL) {
        LOGE("snapshot alloc/open failed path=%s", path);
        result = APP_ERR_IO;
        goto out;
    }

    enc->width = yuvj_frame->width;
    enc->height = yuvj_frame->height;
    enc->pix_fmt = AV_PIX_FMT_YUVJ420P;
    enc->color_range = AVCOL_RANGE_JPEG;
    enc->colorspace = yuvj_frame->colorspace;
    enc->color_primaries = yuvj_frame->color_primaries;
    enc->color_trc = yuvj_frame->color_trc;
    enc->time_base = (AVRational){1, 30};
    enc->flags |= AV_CODEC_FLAG_QSCALE;
    enc->global_quality = 2 * FF_QP2LAMBDA;

    ret = avcodec_open2(enc, codec, NULL);
    if (ret < 0) {
        LOGE("snapshot avcodec_open2 failed: %d", ret);
        result = APP_ERR_FFMPEG;
        goto out;
    }

    ret = avcodec_send_frame(enc, yuvj_frame);
    while (ret >= 0) {
        ret = avcodec_receive_packet(enc, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            LOGE("snapshot encode failed: %d", ret);
            result = APP_ERR_FFMPEG;
            break;
        }
        if (fwrite(packet->data, 1, (size_t)packet->size, fp) != (size_t)packet->size) {
            LOGE("snapshot write failed: %s", path);
            result = APP_ERR_IO;
        }
        av_packet_unref(packet);
        ret = 0;
    }

out:
    if (fp != NULL) {
        fclose(fp);
    }
    if (packet != NULL) {
        av_packet_free(&packet);
    }
    if (enc != NULL) {
        avcodec_free_context(&enc);
    }
    video_frame_convert_deinit(&jpeg_convert);
    video_frame_convert_deinit(&hw_to_nv12);
    av_frame_free(&frame);
    return result;
}

app_status_t mp4_recorder_snapshot(mp4_recorder_ctx_t *ctx, const char *jpg_path) {
    AVFrame *frame = NULL;
    app_status_t result;
    char path[APP_PATH_MAX_LEN];
    int ret;

    if (ctx == NULL) {
        return APP_ERR_PARAM;
    }

    if (jpg_path != NULL && jpg_path[0] != '\0') {
        strncpy(path, jpg_path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        mp4_recorder_make_auto_path(ctx, path, sizeof(path), "snapshot", ".jpg");
    }

    /* 串行化多次拍照调用 */
    pthread_mutex_lock(&ctx->snapshot_lock);

    frame = av_frame_alloc();
    if (frame == NULL) {
        pthread_mutex_unlock(&ctx->snapshot_lock);
        return APP_ERR_NOMEM;
    }

    pthread_mutex_lock(&ctx->latest_lock);
    if (ctx->latest_frame == NULL) {
        pthread_mutex_unlock(&ctx->latest_lock);
        pthread_mutex_unlock(&ctx->snapshot_lock);
        av_frame_free(&frame);
        LOGW("snapshot failed: no frame captured yet");
        return APP_ERR_IO;
    }
    ret = av_frame_ref(frame, ctx->latest_frame);
    pthread_mutex_unlock(&ctx->latest_lock);
    if (ret < 0) {
        pthread_mutex_unlock(&ctx->snapshot_lock);
        av_frame_free(&frame);
        return APP_ERR_FFMPEG;
    }

    result = mp4_recorder_write_jpeg(frame, path);
    av_frame_free(&frame);
    pthread_mutex_unlock(&ctx->snapshot_lock);

    if (result == APP_OK) {
        LOGI("snapshot saved: %s", path);
        mp4_recorder_emit(ctx, MP4_RECORDER_EVENT_SNAPSHOT_SAVED, path);
    } else {
        mp4_recorder_emit(ctx, MP4_RECORDER_EVENT_ERROR, path);
    }
    return result;
}

int mp4_recorder_is_recording(mp4_recorder_ctx_t *ctx) {
    int recording;

    if (ctx == NULL) {
        return 0;
    }
    pthread_mutex_lock(&ctx->queue_lock);
    recording = ctx->recording;
    pthread_mutex_unlock(&ctx->queue_lock);
    return recording;
}

void mp4_recorder_deinit(mp4_recorder_ctx_t *ctx) {
    int i;

    if (ctx == NULL) {
        return;
    }

    pthread_mutex_lock(&ctx->queue_lock);
    ctx->stop_requested = 1;
    pthread_cond_broadcast(&ctx->queue_cond);
    pthread_mutex_unlock(&ctx->queue_lock);

    /* 采集线程先停,编码线程排空队列并写完 trailer 后退出 */
    if (ctx->capture_thread != 0) {
        pthread_join(ctx->capture_thread, NULL);
        ctx->capture_thread = 0;
    }
    if (ctx->encode_thread != 0) {
        pthread_join(ctx->encode_thread, NULL);
        ctx->encode_thread = 0;
    }

    if (ctx->latest_frame != NULL) {
        av_frame_free(&ctx->latest_frame);
    }
    video_frame_convert_deinit(&ctx->nv12_convert);
    for (i = 0; i < MP4_RECORDER_QUEUE_SIZE; ++i) {
        if (ctx->queue_frames[i] != NULL) {
            av_frame_free(&ctx->queue_frames[i]);
        }
    }
    mp4_recorder_close_input(ctx);
    pthread_cond_destroy(&ctx->queue_cond);
    pthread_mutex_destroy(&ctx->queue_lock);
    pthread_mutex_destroy(&ctx->latest_lock);
    pthread_mutex_destroy(&ctx->snapshot_lock);
    LOGD("recorder deinit captured=%d dropped=%d", ctx->captured_frames, ctx->dropped_frames);
    free(ctx);
}
