#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "output_encode_common.c"

#include "output/output_encode_common.h"

#include <pthread.h>
#include <string.h>

#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

#include "common/debug.h"

#ifdef HAVE_LIBRGA
#include <rga/RgaApi.h>
#include <rga/im2d.h>
#endif

#ifdef HAVE_LIBRGA
static pthread_mutex_t g_output_encode_rga_lock = PTHREAD_MUTEX_INITIALIZER;

static IM_STATUS output_encode_rga_resize_on_core(rga_buffer_t src,
                                                  rga_buffer_t dst,
                                                  IM_SCHEDULER_CORE core) {
    IM_STATUS config_status;
    IM_STATUS status;

    pthread_mutex_lock(&g_output_encode_rga_lock);
    config_status = imconfig(IM_CONFIG_SCHEDULER_CORE, core);
    status = (config_status == IM_STATUS_SUCCESS ||
              config_status == IM_STATUS_NOERROR)
                 ? imresize(src, dst)
                 : config_status;
    imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_DEFAULT);
    pthread_mutex_unlock(&g_output_encode_rga_lock);
    return status;
}

static IM_STATUS output_encode_rga_cvtcolor_on_core(rga_buffer_t src,
                                                    rga_buffer_t dst,
                                                    int src_fmt,
                                                    int dst_fmt,
                                                    int color_space_mode,
                                                    IM_SCHEDULER_CORE core) {
    IM_STATUS config_status;
    IM_STATUS status;

    pthread_mutex_lock(&g_output_encode_rga_lock);
    config_status = imconfig(IM_CONFIG_SCHEDULER_CORE, core);
    status = (config_status == IM_STATUS_SUCCESS ||
              config_status == IM_STATUS_NOERROR)
                 ? imcvtcolor(src, dst, src_fmt, dst_fmt, color_space_mode)
                 : config_status;
    imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_DEFAULT);
    pthread_mutex_unlock(&g_output_encode_rga_lock);
    return status;
}
#endif

static int output_encode_sws_colorspace(const AVFrame *frame) {
    switch (frame->colorspace) {
        case AVCOL_SPC_BT709:
            return SWS_CS_ITU709;
        case AVCOL_SPC_FCC:
            return SWS_CS_FCC;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            return SWS_CS_ITU601;
        case AVCOL_SPC_SMPTE240M:
            return SWS_CS_SMPTE240M;
        default:
            return frame->width >= 1280 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

static int output_encode_frame_is_rgb(const AVFrame *frame) {
    const AVPixFmtDescriptor *desc =
        av_pix_fmt_desc_get((enum AVPixelFormat)frame->format);

    return desc != NULL && (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
}

static int output_encode_dst_is_limited_bt709(const output_encode_convert_ctx_t *ctx) {
    return ctx->dst_fmt == AV_PIX_FMT_NV12 ||
           ctx->dst_fmt == AV_PIX_FMT_NV21 ||
           ctx->dst_fmt == AV_PIX_FMT_NV16;
}

static int output_encode_frame_is_hw(const AVFrame *frame) {
    if (frame == NULL || frame->hw_frames_ctx == NULL) {
        return 0;
    }
    return frame->format == AV_PIX_FMT_DRM_PRIME;
}

static const char *output_encode_pix_fmt_name(enum AVPixelFormat fmt) {
    const char *name = av_get_pix_fmt_name(fmt);

    return name != NULL ? name : "unknown";
}

static void output_encode_reset_rkrga_filter(output_encode_convert_ctx_t *ctx) {
    output_encode_rkrga_filter_destroy(&ctx->rkrga_filter);
    av_buffer_unref(&ctx->rkrga_src_hw_frames_ctx);
    ctx->rkrga_src_width = 0;
    ctx->rkrga_src_height = 0;
    ctx->rkrga_src_fmt = AV_PIX_FMT_NONE;
}

static int output_encode_rkrga_source_matches(const output_encode_convert_ctx_t *ctx,
                                               const AVFrame *src_frame,
                                               int input_is_hw) {
    if (ctx->rkrga_src_width != src_frame->width ||
        ctx->rkrga_src_height != src_frame->height ||
        ctx->rkrga_src_fmt != src_frame->format) {
        return 0;
    }
    if (!input_is_hw) {
        return 1;
    }
    return ctx->rkrga_src_hw_frames_ctx != NULL &&
           src_frame->hw_frames_ctx != NULL &&
           ctx->rkrga_src_hw_frames_ctx->data == src_frame->hw_frames_ctx->data;
}

static int output_encode_remember_rkrga_source(output_encode_convert_ctx_t *ctx,
                                                const AVFrame *src_frame,
                                                int input_is_hw) {
    av_buffer_unref(&ctx->rkrga_src_hw_frames_ctx);
    ctx->rkrga_src_width = src_frame->width;
    ctx->rkrga_src_height = src_frame->height;
    ctx->rkrga_src_fmt = (enum AVPixelFormat)src_frame->format;
    if (input_is_hw) {
        ctx->rkrga_src_hw_frames_ctx = av_buffer_ref(src_frame->hw_frames_ctx);
        if (ctx->rkrga_src_hw_frames_ctx == NULL) {
            ctx->rkrga_src_width = 0;
            ctx->rkrga_src_height = 0;
            ctx->rkrga_src_fmt = AV_PIX_FMT_NONE;
            return 0;
        }
    }
    return 1;
}

static const AVFrame *output_encode_transfer_hw_frame(output_encode_convert_ctx_t *ctx,
                                                       const AVFrame *src_frame) {
    AVHWFramesContext *frames_ctx;
    int ret;

    if (!output_encode_frame_is_hw(src_frame)) {
        return src_frame;
    }
    if (ctx->hw_transfer_frame == NULL) {
        ctx->hw_transfer_frame = av_frame_alloc();
        if (ctx->hw_transfer_frame == NULL) {
            return NULL;
        }
    }

    av_frame_unref(ctx->hw_transfer_frame);
    ret = av_hwframe_transfer_data(ctx->hw_transfer_frame, src_frame, 0);
    if (ret < 0) {
        LOGE("encode hardware frame download failed: %d", ret);
        return NULL;
    }
    ret = av_frame_copy_props(ctx->hw_transfer_frame, src_frame);
    if (ret < 0) {
        LOGE("encode hardware frame property copy failed: %d", ret);
        return NULL;
    }

    if (!ctx->hw_transfer_path_logged) {
        frames_ctx = (AVHWFramesContext *)src_frame->hw_frames_ctx->data;
        LOGW("encode fallback path=hardware-download src=%s sw_fmt=%s",
             output_encode_pix_fmt_name((enum AVPixelFormat)src_frame->format),
             frames_ctx != NULL
                 ? output_encode_pix_fmt_name(frames_ctx->sw_format)
                 : "unknown");
        ctx->hw_transfer_path_logged = 1;
    }
    return ctx->hw_transfer_frame;
}

static const AVFrame *output_encode_rkrga_filter_convert(
    output_encode_convert_ctx_t *ctx,
    const AVFrame *src_frame,
    int *converted) {
    const AVFrame *output_frame;
    app_status_t status;

    int input_is_hw;

    input_is_hw = output_encode_frame_is_hw(src_frame);
    if (ctx->dst_fmt != AV_PIX_FMT_NV12 ||
        (!input_is_hw && src_frame->format != AV_PIX_FMT_BGR24)) {
        return NULL;
    }

    if (ctx->rkrga_filter_failed) {
        if (output_encode_rkrga_source_matches(ctx, src_frame, input_is_hw)) {
            return NULL;
        }
        output_encode_reset_rkrga_filter(ctx);
        ctx->rkrga_filter_failed = 0;
        ctx->rkrga_filter_path_logged = 0;
    } else if (ctx->rkrga_filter != NULL &&
               !output_encode_rkrga_source_matches(ctx, src_frame, input_is_hw)) {
        output_encode_reset_rkrga_filter(ctx);
        ctx->rkrga_filter_path_logged = 0;
    }
    if (ctx->rkrga_filter == NULL) {
        if (!output_encode_remember_rkrga_source(ctx, src_frame, input_is_hw)) {
            return NULL;
        }
        if (input_is_hw) {
            status = output_encode_rkrga_filter_create_hw(&ctx->rkrga_filter,
                                                          src_frame->width,
                                                          src_frame->height,
                                                          src_frame->hw_frames_ctx,
                                                          ctx->dst_width,
                                                          ctx->dst_height,
                                                          ctx->dst_fmt,
                                                          ctx->output_is_hw);
        } else {
            status = output_encode_rkrga_filter_create(&ctx->rkrga_filter,
                                                       src_frame->width,
                                                       src_frame->height,
                                                       AV_PIX_FMT_BGR24,
                                                       ctx->dst_width,
                                                       ctx->dst_height,
                                                       ctx->dst_fmt,
                                                       ctx->output_is_hw);
        }
        if (status != APP_OK) {
            ctx->rkrga_filter_failed = 1;
            if (ctx->output_is_hw) {
                LOGE("RKRGA filter unavailable for required hardware output");
            } else {
                LOGW("RKRGA filter unavailable, fallback to hardware download/direct conversion");
            }
            return NULL;
        }
    }

    output_frame = output_encode_rkrga_filter_process(ctx->rkrga_filter, src_frame);
    if (output_frame == NULL) {
        ctx->rkrga_filter_failed = 1;
        output_encode_rkrga_filter_destroy(&ctx->rkrga_filter);
        if (ctx->output_is_hw) {
            LOGE("RKRGA filter processing failed for required hardware output");
        } else {
            LOGW("RKRGA filter processing failed, fallback to hardware download/direct conversion");
        }
        return NULL;
    }
    if (!ctx->rkrga_filter_path_logged) {
        LOGI("encode convert path=%s src=%dx%d fmt=%s "
             "dst=%dx%d fmt=%s",
             input_is_hw ? "FFmpeg-RKRGA-hardware-frame"
                         : "FFmpeg-RKRGA-DMABUF",
             src_frame->width,
             src_frame->height,
             output_encode_pix_fmt_name((enum AVPixelFormat)src_frame->format),
             ctx->dst_width,
             ctx->dst_height,
             ctx->output_is_hw ? "drm_prime(nv12)"
                               : output_encode_pix_fmt_name(ctx->dst_fmt));
        ctx->rkrga_filter_path_logged = 1;
    }
    if (converted != NULL) {
        *converted = 1;
    }
    return output_frame;
}

static int output_encode_configure_sws(output_encode_convert_ctx_t *ctx,
                                       const AVFrame *src_frame) {
    const int *src_coefficients;
    const int *dst_coefficients;
    int src_range;

    if (!output_encode_dst_is_limited_bt709(ctx)) {
        return 0;
    }

    src_coefficients = sws_getCoefficients(output_encode_sws_colorspace(src_frame));
    dst_coefficients = sws_getCoefficients(SWS_CS_ITU709);
    src_range = output_encode_frame_is_rgb(src_frame) ||
                src_frame->color_range == AVCOL_RANGE_JPEG;
    return sws_setColorspaceDetails(ctx->sws,
                                    src_coefficients,
                                    src_range,
                                    dst_coefficients,
                                    0,
                                    0,
                                    1 << 16,
                                    1 << 16);
}

static app_status_t output_encode_alloc_work_frame(output_encode_convert_ctx_t *ctx) {
    AVBufferRef *buffer;
    size_t luma_size;
    size_t chroma_size;
    int chroma_height;
    int linesize;
    int first_alloc;
    int ret;

    first_alloc = ctx->work_frame == NULL;
    if (ctx->work_frame == NULL) {
        ctx->work_frame = av_frame_alloc();
        if (ctx->work_frame == NULL) {
            return APP_ERR_NOMEM;
        }
    } else {
        av_frame_unref(ctx->work_frame);
    }

    ctx->work_frame->format = ctx->dst_fmt;
    ctx->work_frame->width = ctx->dst_width;
    ctx->work_frame->height = ctx->dst_height;

    if (ctx->dst_fmt == AV_PIX_FMT_NV12 ||
        ctx->dst_fmt == AV_PIX_FMT_NV21 ||
        ctx->dst_fmt == AV_PIX_FMT_NV16) {
        linesize = (ctx->dst_width + 63) & ~63;
        chroma_height = ctx->dst_fmt == AV_PIX_FMT_NV16
                            ? ctx->dst_height
                            : (ctx->dst_height + 1) / 2;
        luma_size = (size_t)linesize * (size_t)ctx->dst_height;
        chroma_size = (size_t)linesize * (size_t)chroma_height;
        buffer = av_buffer_alloc(luma_size + chroma_size);
        if (buffer == NULL) {
            return APP_ERR_NOMEM;
        }

        ctx->work_frame->buf[0] = buffer;
        ctx->work_frame->data[0] = buffer->data;
        ctx->work_frame->data[1] = buffer->data + luma_size;
        ctx->work_frame->linesize[0] = linesize;
        ctx->work_frame->linesize[1] = linesize;
        ctx->work_frame->extended_data = ctx->work_frame->data;
        if (first_alloc) {
            LOGI("encode contiguous %s buffer: size=%zu linesize=%d uv_offset=%zu",
                 ctx->dst_fmt == AV_PIX_FMT_NV12
                     ? "NV12"
                     : (ctx->dst_fmt == AV_PIX_FMT_NV21 ? "NV21" : "NV16"),
                 luma_size + chroma_size,
                 linesize,
                 luma_size);
        }
    } else {
        ret = av_frame_get_buffer(ctx->work_frame, 64);
        if (ret < 0) {
            LOGE("encode work frame get buffer failed: %d", ret);
            return APP_ERR_FFMPEG;
        }
    }

    ret = av_frame_make_writable(ctx->work_frame);
    if (ret < 0) {
        LOGE("encode work frame make writable failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    return APP_OK;
}

#ifdef HAVE_LIBRGA
static app_status_t output_encode_alloc_rga_resize_frame(output_encode_convert_ctx_t *ctx) {
    im_handle_param_t param;
    int ret;

    if (ctx->rga_resize_frame == NULL) {
        ctx->rga_resize_frame = av_frame_alloc();
        if (ctx->rga_resize_frame == NULL) {
            return APP_ERR_NOMEM;
        }
        ctx->rga_resize_frame->format = AV_PIX_FMT_BGR24;
        ctx->rga_resize_frame->width = ctx->dst_width;
        ctx->rga_resize_frame->height = ctx->dst_height;
        ret = av_frame_get_buffer(ctx->rga_resize_frame, 64);
        if (ret < 0) {
            LOGE("rga resize frame get buffer failed: %d", ret);
            return APP_ERR_FFMPEG;
        }

        memset(&param, 0, sizeof(param));
        param.width = (uint32_t)(ctx->rga_resize_frame->linesize[0] / 3);
        param.height = (uint32_t)ctx->dst_height;
        param.format = RK_FORMAT_BGR_888;
        ctx->rga_resize_handle = importbuffer_virtualaddr(ctx->rga_resize_frame->data[0], &param);
        if (ctx->rga_resize_handle == 0) {
            LOGE("rga resize frame import failed");
            return APP_ERR_FFMPEG;
        }
    }

    ret = av_frame_make_writable(ctx->rga_resize_frame);
    if (ret < 0) {
        LOGE("rga resize frame make writable failed: %d", ret);
        return APP_ERR_FFMPEG;
    }
    return APP_OK;
}

static int output_encode_rga_status_ok(IM_STATUS status) {
    return status == IM_STATUS_SUCCESS || status == IM_STATUS_NOERROR;
}

static int output_encode_rga_pix_fmt(enum AVPixelFormat fmt) {
    switch (fmt) {
        case AV_PIX_FMT_NV12:    return RK_FORMAT_YCbCr_420_SP;
        case AV_PIX_FMT_NV21:    return RK_FORMAT_YCrCb_420_SP;
        case AV_PIX_FMT_YUV420P: return RK_FORMAT_YCbCr_420_P;
        case AV_PIX_FMT_NV16:    return RK_FORMAT_YCbCr_422_SP;
        case AV_PIX_FMT_YUV422P: return RK_FORMAT_YCbCr_422_P;
        case AV_PIX_FMT_YUYV422: return RK_FORMAT_YUYV_422;
        case AV_PIX_FMT_UYVY422: return RK_FORMAT_UYVY_422;
        case AV_PIX_FMT_BGR24:   return RK_FORMAT_BGR_888;
        default:                 return RK_FORMAT_UNKNOWN;
    }
}

/*
 * RGA 要求所有平面在一块连续内存里。
 * FFmpeg 解码/v4l2 出来的帧一般都满足,这里显式校验,不满足就走 swscale。
 */
static int output_encode_rga_frame_usable(const AVFrame *frame) {
    const uint8_t *expected;

    if (frame->data[0] == NULL || frame->linesize[0] <= 0) {
        return 0;
    }

    switch ((enum AVPixelFormat)frame->format) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21:
        case AV_PIX_FMT_NV16:
            if (frame->data[1] == NULL) {
                return 0;
            }
            expected = frame->data[0] + (ptrdiff_t)frame->linesize[0] * frame->height;
            return frame->data[1] == expected;
        case AV_PIX_FMT_YUV420P:
            if (frame->data[1] == NULL || frame->data[2] == NULL) {
                return 0;
            }
            expected = frame->data[0] + (ptrdiff_t)frame->linesize[0] * frame->height;
            if (frame->data[1] != expected) {
                return 0;
            }
            expected = frame->data[1] + (ptrdiff_t)frame->linesize[1] * ((frame->height + 1) / 2);
            return frame->data[2] == expected;
        case AV_PIX_FMT_YUV422P:
            if (frame->data[1] == NULL || frame->data[2] == NULL) {
                return 0;
            }
            expected = frame->data[0] + (ptrdiff_t)frame->linesize[0] * frame->height;
            if (frame->data[1] != expected) {
                return 0;
            }
            expected = frame->data[1] + (ptrdiff_t)frame->linesize[1] * frame->height;
            return frame->data[2] == expected;
        case AV_PIX_FMT_YUYV422:
        case AV_PIX_FMT_UYVY422:
        case AV_PIX_FMT_BGR24:
            return 1;
        default:
            return 0;
    }
}

/* 第一平面的字节/像素,用于把 linesize(字节)换算成 RGA 的 wstride(像素) */
static int output_encode_rga_plane_bpp(enum AVPixelFormat fmt) {
    switch (fmt) {
        case AV_PIX_FMT_YUYV422:
        case AV_PIX_FMT_UYVY422:
            return 2;
        case AV_PIX_FMT_BGR24:
            return 3;
        default:
            return 1;
    }
}

/*
 * 用 RGA 硬件做缩放 + 格式转换(一步完成)。
 * 返回 NULL 表示这一路不能用 RGA,调用方回落 swscale。
 * 注意:RGA 不区分 yuvj(全范围),USB MJPEG 软解帧经此转换颜色范围按 limited 处理,
 * 与原先 swscale 默认行为一致。
 */
static const AVFrame *output_encode_rga_convert(output_encode_convert_ctx_t *ctx,
                                                const AVFrame *src_frame,
                                                int *converted) {
    enum AVPixelFormat src_av_fmt = (enum AVPixelFormat)src_frame->format;
    int src_rga_fmt;
    int dst_rga_fmt;
    rga_buffer_t src_img;
    rga_buffer_t dst_img;
    rga_buffer_t resize_img;
    IM_STATUS status;
    int rgb_to_nv12;
    rga_buffer_handle_t src_handle = 0;
    rga_buffer_handle_t dst_handle = 0;

    ctx->rga_resize_valid = 0;
    if (ctx->rga_failed) {
        return NULL;
    }

    src_rga_fmt = output_encode_rga_pix_fmt(src_av_fmt);
    dst_rga_fmt = output_encode_rga_pix_fmt(ctx->dst_fmt);
    if (src_rga_fmt == RK_FORMAT_UNKNOWN || dst_rga_fmt == RK_FORMAT_UNKNOWN) {
        return NULL;
    }
    if (!output_encode_rga_frame_usable(src_frame)) {
        return NULL;
    }
    if (output_encode_alloc_work_frame(ctx) != APP_OK) {
        return NULL;
    }

    src_img = wrapbuffer_virtualaddr(src_frame->data[0],
                                     src_frame->width,
                                     src_frame->height,
                                     src_rga_fmt,
                                     src_frame->linesize[0] / output_encode_rga_plane_bpp(src_av_fmt),
                                     src_frame->height);
    dst_img = wrapbuffer_virtualaddr(ctx->work_frame->data[0],
                                     ctx->dst_width,
                                     ctx->dst_height,
                                     dst_rga_fmt,
                                     ctx->work_frame->linesize[0] / output_encode_rga_plane_bpp(ctx->dst_fmt),
                                     ctx->dst_height);

    rgb_to_nv12 = src_av_fmt == AV_PIX_FMT_BGR24 && ctx->dst_fmt == AV_PIX_FMT_NV12;
    if (rgb_to_nv12 &&
        (src_frame->width != ctx->dst_width || src_frame->height != ctx->dst_height)) {
        im_handle_param_t src_param;
        im_handle_param_t dst_param;

        if (output_encode_alloc_rga_resize_frame(ctx) != APP_OK) {
            return NULL;
        }

        memset(&src_param, 0, sizeof(src_param));
        src_param.width = (uint32_t)(src_frame->linesize[0] / 3);
        src_param.height = (uint32_t)src_frame->height;
        src_param.format = (uint32_t)src_rga_fmt;
        src_handle = importbuffer_virtualaddr(src_frame->data[0], &src_param);

        memset(&dst_param, 0, sizeof(dst_param));
        dst_param.width = (uint32_t)ctx->work_frame->linesize[0];
        dst_param.height = (uint32_t)ctx->dst_height;
        dst_param.format = (uint32_t)dst_rga_fmt;
        dst_handle = importbuffer_virtualaddr(ctx->work_frame->data[0], &dst_param);
        if (src_handle == 0 || dst_handle == 0) {
            if (src_handle != 0) {
                releasebuffer_handle(src_handle);
            }
            if (dst_handle != 0) {
                releasebuffer_handle(dst_handle);
            }
            LOGE("rga source/destination handle import failed");
            return NULL;
        }

        src_img = wrapbuffer_handle(src_handle,
                                    src_frame->width,
                                    src_frame->height,
                                    src_rga_fmt,
                                    src_frame->linesize[0] / 3,
                                    src_frame->height);
        dst_img = wrapbuffer_handle(dst_handle,
                                    ctx->dst_width,
                                    ctx->dst_height,
                                    dst_rga_fmt,
                                    ctx->work_frame->linesize[0],
                                    ctx->dst_height);
        resize_img = wrapbuffer_handle(ctx->rga_resize_handle,
                                       ctx->dst_width,
                                       ctx->dst_height,
                                       RK_FORMAT_BGR_888,
                                       ctx->rga_resize_frame->linesize[0] / 3,
                                       ctx->dst_height);
        status = output_encode_rga_resize_on_core(src_img,
                                                  resize_img,
                                                  IM_SCHEDULER_RGA3_CORE0);
        if (output_encode_rga_status_ok(status)) {
            ctx->rga_resize_frame->pts = src_frame->pts;
            ctx->rga_resize_frame->color_range = src_frame->color_range;
            ctx->rga_resize_frame->colorspace = src_frame->colorspace;
            ctx->rga_resize_frame->color_primaries = src_frame->color_primaries;
            ctx->rga_resize_frame->color_trc = src_frame->color_trc;
            ctx->rga_resize_valid = 1;
            if (ctx->rga_color_failed) {
                releasebuffer_handle(src_handle);
                releasebuffer_handle(dst_handle);
                return NULL;
            }
            status = output_encode_rga_cvtcolor_on_core(
                resize_img,
                dst_img,
                RK_FORMAT_BGR_888,
                dst_rga_fmt,
                IM_RGB_TO_YUV_BT709_LIMIT,
                IM_SCHEDULER_RGA2_CORE0);
        }
        releasebuffer_handle(src_handle);
        releasebuffer_handle(dst_handle);
    } else if (src_frame->width == ctx->dst_width && src_frame->height == ctx->dst_height) {
        status = output_encode_rga_cvtcolor_on_core(
            src_img,
            dst_img,
            src_rga_fmt,
            dst_rga_fmt,
            rgb_to_nv12 ? IM_RGB_TO_YUV_BT709_LIMIT : IM_COLOR_SPACE_DEFAULT,
            rgb_to_nv12 ? IM_SCHEDULER_RGA2_CORE0 : IM_SCHEDULER_DEFAULT);
    } else {
        status = output_encode_rga_resize_on_core(src_img,
                                                  dst_img,
                                                  IM_SCHEDULER_DEFAULT);
    }

    if (!output_encode_rga_status_ok(status)) {
        ctx->rga_error_count++;
        if (ctx->rga_resize_valid) {
            ctx->rga_color_failed = 1;
            LOGW("rga BGR-to-NV12 conversion disabled for this encoder; "
                 "keep RGA resize and use swscale for 1080p color conversion");
            return NULL;
        }
        if (ctx->rga_error_count <= 3 || ctx->rga_error_count % 30 == 0) {
            LOGW("rga convert failed (%s) src=%dx%d fmt=%d dst=%dx%d fmt=%d, "
                 "retry next frame",
                 imStrError(status),
                 src_frame->width,
                 src_frame->height,
                 src_av_fmt,
                 ctx->dst_width,
                 ctx->dst_height,
                 ctx->dst_fmt);
        }
        return NULL;
    }

    ctx->rga_error_count = 0;

    if (!ctx->rga_path_logged) {
        LOGI("encode convert path=%s src=%dx%d fmt=%d dst=%dx%d fmt=%d",
             rgb_to_nv12 &&
                     (src_frame->width != ctx->dst_width || src_frame->height != ctx->dst_height)
                 ? "RGA-two-pass-BT709"
                 : "RGA",
             src_frame->width,
             src_frame->height,
             src_av_fmt,
             ctx->dst_width,
             ctx->dst_height,
             ctx->dst_fmt);
        ctx->rga_path_logged = 1;
    }
    ctx->work_frame->color_range = AVCOL_RANGE_MPEG;
    ctx->work_frame->colorspace = AVCOL_SPC_BT709;
    ctx->work_frame->color_primaries = AVCOL_PRI_BT709;
    ctx->work_frame->color_trc = AVCOL_TRC_BT709;
    ctx->work_frame->pts = src_frame->pts;
    if (converted != NULL) {
        *converted = 1;
    }
    return ctx->work_frame;
}
#endif /* HAVE_LIBRGA */

app_status_t output_encode_convert_init(output_encode_convert_ctx_t *ctx,
                                        int dst_width,
                                        int dst_height,
                                        enum AVPixelFormat dst_fmt) {
    if (ctx == NULL || dst_width <= 0 || dst_height <= 0) {
        return APP_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->dst_width = dst_width;
    ctx->dst_height = dst_height;
    ctx->dst_fmt = dst_fmt;
    ctx->src_fmt = AV_PIX_FMT_NONE;
    return APP_OK;
}

app_status_t output_encode_convert_init_hw(output_encode_convert_ctx_t *ctx,
                                           int dst_width,
                                           int dst_height,
                                           enum AVPixelFormat dst_sw_fmt) {
    app_status_t status;

    if (dst_sw_fmt != AV_PIX_FMT_NV12) {
        return APP_ERR_UNSUPPORTED;
    }
    status = output_encode_convert_init(ctx, dst_width, dst_height, dst_sw_fmt);
    if (status != APP_OK) {
        return status;
    }
    ctx->output_is_hw = 1;
    return APP_OK;
}

const AVFrame *output_encode_prepare_rga_bgr_resize(output_encode_convert_ctx_t *ctx,
                                                    const AVFrame *src_frame) {
#ifdef HAVE_LIBRGA
    im_handle_param_t src_param;
    rga_buffer_handle_t src_handle;
    rga_buffer_t src_img;
    rga_buffer_t dst_img;
    IM_STATUS status;

    if (ctx == NULL || src_frame == NULL ||
        src_frame->format != AV_PIX_FMT_BGR24 ||
        !output_encode_rga_frame_usable(src_frame) ||
        ctx->rga_failed) {
        return NULL;
    }
    if (output_encode_alloc_rga_resize_frame(ctx) != APP_OK) {
        return NULL;
    }

    memset(&src_param, 0, sizeof(src_param));
    src_param.width = (uint32_t)(src_frame->linesize[0] / 3);
    src_param.height = (uint32_t)src_frame->height;
    src_param.format = RK_FORMAT_BGR_888;
    src_handle = importbuffer_virtualaddr(src_frame->data[0], &src_param);
    if (src_handle == 0) {
        LOGE("rga BGR resize source handle import failed");
        ctx->rga_failed = 1;
        return NULL;
    }

    src_img = wrapbuffer_handle(src_handle,
                                src_frame->width,
                                src_frame->height,
                                RK_FORMAT_BGR_888,
                                src_frame->linesize[0] / 3,
                                src_frame->height);
    dst_img = wrapbuffer_handle(ctx->rga_resize_handle,
                                ctx->dst_width,
                                ctx->dst_height,
                                RK_FORMAT_BGR_888,
                                ctx->rga_resize_frame->linesize[0] / 3,
                                ctx->dst_height);
    status = output_encode_rga_resize_on_core(src_img,
                                              dst_img,
                                              IM_SCHEDULER_RGA3_CORE0);
    releasebuffer_handle(src_handle);
    if (!output_encode_rga_status_ok(status)) {
        LOGE("rga BGR resize failed (%s) src=%dx%d dst=%dx%d",
             imStrError(status),
             src_frame->width,
             src_frame->height,
             ctx->dst_width,
             ctx->dst_height);
        ctx->rga_failed = 1;
        return NULL;
    }

    if (!ctx->rga_path_logged) {
        LOGI("compare resize path=RGA-BGR src=%dx%d dst=%dx%d",
             src_frame->width,
             src_frame->height,
             ctx->dst_width,
             ctx->dst_height);
        ctx->rga_path_logged = 1;
    }
    ctx->rga_resize_frame->color_range = src_frame->color_range;
    ctx->rga_resize_frame->colorspace = src_frame->colorspace;
    ctx->rga_resize_frame->color_primaries = src_frame->color_primaries;
    ctx->rga_resize_frame->color_trc = src_frame->color_trc;
    ctx->rga_resize_frame->pts = src_frame->pts;
    return ctx->rga_resize_frame;
#else
    (void)ctx;
    (void)src_frame;
    return NULL;
#endif
}

const AVFrame *output_encode_prepare_frame(output_encode_convert_ctx_t *ctx,
                                           const AVFrame *src_frame,
                                           int *converted) {
    const AVFrame *convert_src;
    app_status_t status;
    int ret;
    enum AVPixelFormat src_fmt;

    if (converted != NULL) {
        *converted = 0;
    }
    if (ctx == NULL || src_frame == NULL) {
        return NULL;
    }

    convert_src = src_frame;
    src_fmt = (enum AVPixelFormat)src_frame->format;
    if (!ctx->output_is_hw &&
        src_frame->width == ctx->dst_width &&
        src_frame->height == ctx->dst_height &&
        src_fmt == ctx->dst_fmt) {
        return src_frame;
    }

#ifdef HAVE_LIBRGA
    {
        const AVFrame *filter_frame =
            output_encode_rkrga_filter_convert(ctx, src_frame, converted);
        if (filter_frame != NULL) {
            return filter_frame;
        }
    }
#endif

    if (ctx->output_is_hw) {
        LOGE("encode hardware output conversion failed src=%dx%d fmt=%s dst=%dx%d nv12",
             src_frame->width,
             src_frame->height,
             output_encode_pix_fmt_name((enum AVPixelFormat)src_frame->format),
             ctx->dst_width,
             ctx->dst_height);
        return NULL;
    }

    if (output_encode_frame_is_hw(src_frame)) {
        convert_src = output_encode_transfer_hw_frame(ctx, src_frame);
        if (convert_src == NULL) {
            return NULL;
        }
        src_fmt = (enum AVPixelFormat)convert_src->format;
        if (convert_src->width == ctx->dst_width &&
            convert_src->height == ctx->dst_height &&
            src_fmt == ctx->dst_fmt) {
            if (converted != NULL) {
                *converted = 1;
            }
            return convert_src;
        }
    }

#ifdef HAVE_LIBRGA
    {
        const AVFrame *rga_input = convert_src;
        const AVFrame *rga_frame = output_encode_rga_convert(ctx, convert_src, converted);
        if (rga_frame != NULL) {
            return rga_frame;
        }
        if (ctx->rga_resize_valid && ctx->rga_resize_frame != NULL) {
            convert_src = ctx->rga_resize_frame;
            src_fmt = (enum AVPixelFormat)convert_src->format;
            if (!ctx->rga_partial_fallback_logged) {
                LOGW("encode fallback path=RGA-resize+swscale-color src=%dx%d fmt=%d "
                     "intermediate=%dx%d fmt=%d dst=%dx%d fmt=%d",
                     rga_input->width,
                     rga_input->height,
                     rga_input->format,
                     convert_src->width,
                     convert_src->height,
                     convert_src->format,
                     ctx->dst_width,
                     ctx->dst_height,
                     ctx->dst_fmt);
                ctx->rga_partial_fallback_logged = 1;
            }
        }
    }
#endif

    if (ctx->sws == NULL ||
        ctx->src_fmt != src_fmt ||
        ctx->src_width != convert_src->width ||
        ctx->src_height != convert_src->height) {
        ctx->sws = sws_getCachedContext(ctx->sws,
                                        convert_src->width,
                                        convert_src->height,
                                        src_fmt,
                                        ctx->dst_width,
                                        ctx->dst_height,
                                        ctx->dst_fmt,
                                        SWS_BILINEAR,
                                        NULL,
                                        NULL,
                                        NULL);
        if (ctx->sws == NULL) {
            LOGE("encode sws_getCachedContext failed src=%dx%d fmt=%d dst=%dx%d fmt=%d",
                 convert_src->width,
                 convert_src->height,
                 src_fmt,
                 ctx->dst_width,
                 ctx->dst_height,
                 ctx->dst_fmt);
            return NULL;
        }
        ctx->src_fmt = src_fmt;
        ctx->src_width = convert_src->width;
        ctx->src_height = convert_src->height;
    }

    status = output_encode_alloc_work_frame(ctx);
    if (status != APP_OK) {
        return NULL;
    }

    ret = output_encode_configure_sws(ctx, convert_src);
    if (ret < 0) {
        LOGE("encode sws_setColorspaceDetails failed: %d", ret);
        return NULL;
    }

    ret = sws_scale(ctx->sws,
                    (const uint8_t * const *)convert_src->data,
                    convert_src->linesize,
                    0,
                    convert_src->height,
                    ctx->work_frame->data,
                    ctx->work_frame->linesize);
    if (ret <= 0) {
        LOGE("encode sws_scale failed: %d", ret);
        return NULL;
    }

    if (!ctx->sws_path_logged) {
        LOGI("encode convert path=swscale src=%dx%d fmt=%s(%d) "
             "dst=%dx%d fmt=%s(%d)",
             convert_src->width,
             convert_src->height,
             output_encode_pix_fmt_name(src_fmt),
             src_fmt,
             ctx->dst_width,
             ctx->dst_height,
             output_encode_pix_fmt_name(ctx->dst_fmt),
             ctx->dst_fmt);
        ctx->sws_path_logged = 1;
    }
    if (output_encode_dst_is_limited_bt709(ctx)) {
        ctx->work_frame->color_range = AVCOL_RANGE_MPEG;
        ctx->work_frame->colorspace = AVCOL_SPC_BT709;
        ctx->work_frame->color_primaries = AVCOL_PRI_BT709;
        ctx->work_frame->color_trc = AVCOL_TRC_BT709;
    }
    ctx->work_frame->pts = src_frame->pts;
    if (converted != NULL) {
        *converted = 1;
    }
    return ctx->work_frame;
}

void output_encode_convert_deinit(output_encode_convert_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    if (ctx->work_frame != NULL) {
        av_frame_free(&ctx->work_frame);
    }
    if (ctx->hw_transfer_frame != NULL) {
        av_frame_free(&ctx->hw_transfer_frame);
    }
    output_encode_reset_rkrga_filter(ctx);
#ifdef HAVE_LIBRGA
    if (ctx->rga_resize_handle != 0) {
        releasebuffer_handle(ctx->rga_resize_handle);
    }
#endif
    if (ctx->rga_resize_frame != NULL) {
        av_frame_free(&ctx->rga_resize_frame);
    }
    sws_freeContext(ctx->sws);
    memset(ctx, 0, sizeof(*ctx));
}
