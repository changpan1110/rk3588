#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "video_pipeline_encode.c"

#include "video_pipeline_internal.h"

#include <string.h>

#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>

#include "common/debug.h"

int vp_h264_rkmpp_supports_sw_format(enum AVPixelFormat sw_format) {
    switch (sw_format) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV16:
        case AV_PIX_FMT_NV24:
        case AV_PIX_FMT_BGR24:
            return 1;
        default:
            return 0;
    }
}

app_status_t vp_open_h264_encoder(AVCodecContext **out_enc,
                                  int width,
                                  int height,
                                  int fps,
                                  int bitrate,
                                  int gop,
                                  enum AVPixelFormat pix_fmt,
                                  AVBufferRef *hw_frames_ctx,
                                  int global_header) {
    const AVCodec *codec;
    AVCodecContext *enc;
    AVHWFramesContext *frames_ctx = NULL;
    AVDictionary *opts = NULL;
    int ret;

    codec = avcodec_find_encoder_by_name("h264_rkmpp");
    if (codec == NULL) {
        if (pix_fmt == AV_PIX_FMT_DRM_PRIME) {
            LOGE("h264_rkmpp is required for DRM PRIME input");
            return APP_ERR_UNSUPPORTED;
        }
        codec = avcodec_find_encoder_by_name("h264_v4l2m2m");
        LOGW("h264_rkmpp not found, fallback to h264_v4l2m2m");
    }
    if (codec == NULL) {
        LOGE("no h264 encoder available");
        return APP_ERR_UNSUPPORTED;
    }

    enc = avcodec_alloc_context3(codec);
    if (enc == NULL) {
        return APP_ERR_NOMEM;
    }

    enc->codec_type = AVMEDIA_TYPE_VIDEO;
    enc->codec_id = AV_CODEC_ID_H264;
    enc->width = width;
    enc->height = height;
    enc->pix_fmt = pix_fmt;
    if (pix_fmt == AV_PIX_FMT_DRM_PRIME) {
        if (hw_frames_ctx == NULL || hw_frames_ctx->data == NULL) {
            LOGE("DRM PRIME encoder input requires hw_frames_ctx");
            avcodec_free_context(&enc);
            return APP_ERR_PARAM;
        }
        frames_ctx = (AVHWFramesContext *)hw_frames_ctx->data;
        if (!vp_h264_rkmpp_supports_sw_format(frames_ctx->sw_format)) {
            LOGE("DRM PRIME encoder does not support hardware frame format %s",
                 av_get_pix_fmt_name(frames_ctx->sw_format) != NULL
                     ? av_get_pix_fmt_name(frames_ctx->sw_format)
                     : "unknown");
            avcodec_free_context(&enc);
            return APP_ERR_UNSUPPORTED;
        }
        enc->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);
        if (enc->hw_frames_ctx == NULL) {
            avcodec_free_context(&enc);
            return APP_ERR_NOMEM;
        }
        enc->sw_pix_fmt = frames_ctx->sw_format;
    }
    enc->time_base = (AVRational){1, 1000000};
    enc->framerate = (AVRational){fps > 0 ? fps : 30, 1};
    enc->bit_rate = bitrate;
    enc->gop_size = gop > 0 ? gop : (fps > 0 ? fps : 30);
    enc->max_b_frames = 0;
    enc->color_range = AVCOL_RANGE_MPEG;
    enc->colorspace = AVCOL_SPC_BT709;
    enc->color_primaries = AVCOL_PRI_BT709;
    enc->color_trc = AVCOL_TRC_BT709;
    enc->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (global_header) {
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (strcmp(codec->name, "h264_rkmpp") == 0) {
        av_dict_set(&opts, "rc_mode", "CBR", 0);
    }

    ret = avcodec_open2(enc, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        LOGE("avcodec_open2 %s failed: %d", codec->name, ret);
        avcodec_free_context(&enc);
        return APP_ERR_FFMPEG;
    }

    LOGI("h264 encoder opened: %s %dx%d@%d bitrate=%d pix_fmt=%s sw_fmt=%s "
         "low_delay=%d max_b_frames=%d has_b_frames=%d delay=%d threads=%d",
         codec->name,
         width,
         height,
         fps,
         bitrate,
         av_get_pix_fmt_name(pix_fmt) != NULL
             ? av_get_pix_fmt_name(pix_fmt)
             : "unknown",
         frames_ctx != NULL
             ? (av_get_pix_fmt_name(frames_ctx->sw_format) != NULL
                    ? av_get_pix_fmt_name(frames_ctx->sw_format)
                    : "unknown")
             : "none",
         (enc->flags & AV_CODEC_FLAG_LOW_DELAY) != 0,
         enc->max_b_frames,
         enc->has_b_frames,
         enc->delay,
         enc->thread_count);
    *out_enc = enc;
    return APP_OK;
}
