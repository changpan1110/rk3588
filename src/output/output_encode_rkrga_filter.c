#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "output_encode_rkrga_filter.c"

#include "output/output_encode_rkrga_filter.h"

#include <stdio.h>
#include <stdlib.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avstring.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#include "common/debug.h"

struct output_encode_rkrga_filter {
    AVFilterGraph *graph;
    AVFilterContext *source;
    AVFilterContext *sink;
    AVBufferRef *hw_device;
    AVBufferRef *src_hw_frames_ctx;
    AVFrame *output_frame;
    int src_width;
    int src_height;
    enum AVPixelFormat src_fmt;
    enum AVPixelFormat src_sw_fmt;
    int input_is_hw;
    int output_is_hw;
    int dst_width;
    int dst_height;
    enum AVPixelFormat dst_fmt;
};

static void output_encode_rkrga_filter_log_error(const char *operation, int error) {
    char message[AV_ERROR_MAX_STRING_SIZE];

    av_strerror(error, message, sizeof(message));
    LOGE("%s failed: %s (%d)", operation, message, error);
}

static const char *output_encode_rkrga_pix_fmt_name(enum AVPixelFormat fmt) {
    const char *name = av_get_pix_fmt_name(fmt);

    return name != NULL ? name : "unknown";
}

static app_status_t output_encode_rkrga_filter_build(output_encode_rkrga_filter_t *ctx) {
    const AVFilter *buffer_filter;
    const AVFilter *buffersink_filter;
    AVBufferSrcParameters *source_params = NULL;
    AVHWFramesContext *src_frames_ctx = NULL;
    AVFilterInOut *inputs = NULL;
    AVFilterInOut *outputs = NULL;
    char source_args[256];
    char filter_desc[512];
    const char *output_suffix;
    int same_size;
    unsigned int i;
    int ret;

    buffer_filter = avfilter_get_by_name("buffer");
    buffersink_filter = avfilter_get_by_name("buffersink");
    if (buffer_filter == NULL || buffersink_filter == NULL ||
        avfilter_get_by_name("scale_rkrga") == NULL) {
        LOGE("required FFmpeg buffer/scale_rkrga filters are unavailable");
        return APP_ERR_FFMPEG;
    }

    ctx->graph = avfilter_graph_alloc();
    if (ctx->graph == NULL) {
        return APP_ERR_NOMEM;
    }

    if (ctx->input_is_hw) {
        if (ctx->src_hw_frames_ctx == NULL ||
            ctx->src_hw_frames_ctx->data == NULL) {
            return APP_ERR_PARAM;
        }
        src_frames_ctx = (AVHWFramesContext *)ctx->src_hw_frames_ctx->data;
        ctx->src_sw_fmt = src_frames_ctx->sw_format;
        ctx->hw_device = av_buffer_ref(src_frames_ctx->device_ref);
        if (ctx->hw_device == NULL) {
            return APP_ERR_NOMEM;
        }
    } else {
        ret = av_hwdevice_ctx_create(&ctx->hw_device,
                                     AV_HWDEVICE_TYPE_RKMPP,
                                     NULL,
                                     NULL,
                                     0);
        if (ret < 0) {
            output_encode_rkrga_filter_log_error("create RKMPP hardware device", ret);
            return APP_ERR_FFMPEG;
        }
    }

    snprintf(source_args,
             sizeof(source_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/1000000:pixel_aspect=1/1",
             ctx->src_width,
             ctx->src_height,
             ctx->src_fmt);
    ret = avfilter_graph_create_filter(&ctx->source,
                                       buffer_filter,
                                       "rkrga_input",
                                       source_args,
                                       NULL,
                                       ctx->graph);
    if (ret < 0) {
        output_encode_rkrga_filter_log_error("create RKRGA buffer source", ret);
        return APP_ERR_FFMPEG;
    }
    if (ctx->input_is_hw) {
        source_params = av_buffersrc_parameters_alloc();
        if (source_params == NULL) {
            return APP_ERR_NOMEM;
        }
        source_params->hw_frames_ctx = av_buffer_ref(ctx->src_hw_frames_ctx);
        if (source_params->hw_frames_ctx == NULL) {
            av_free(source_params);
            return APP_ERR_NOMEM;
        }
        ret = av_buffersrc_parameters_set(ctx->source, source_params);
        av_buffer_unref(&source_params->hw_frames_ctx);
        av_free(source_params);
        source_params = NULL;
        if (ret < 0) {
            output_encode_rkrga_filter_log_error("set RKRGA hardware source parameters", ret);
            return APP_ERR_FFMPEG;
        }
    }
    ret = avfilter_graph_create_filter(&ctx->sink,
                                       buffersink_filter,
                                       "rkrga_output",
                                       NULL,
                                       NULL,
                                       ctx->graph);
    if (ret < 0) {
        output_encode_rkrga_filter_log_error("create RKRGA buffer sink", ret);
        return APP_ERR_FFMPEG;
    }

    same_size = ctx->src_width == ctx->dst_width &&
                ctx->src_height == ctx->dst_height;
    output_suffix = ctx->output_is_hw
                        ? ""
                        : ",hwdownload,format=pix_fmts=nv12";
    if (ctx->input_is_hw && ctx->src_sw_fmt == AV_PIX_FMT_NV12 && same_size) {
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "%s",
                 ctx->output_is_hw ? "null"
                                   : "hwdownload,format=pix_fmts=nv12");
    } else if (ctx->input_is_hw && ctx->src_sw_fmt == AV_PIX_FMT_NV12) {
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "scale_rkrga=w=%d:h=%d:format=nv12:core=rga3_core0:async_depth=0%s",
                 ctx->dst_width,
                 ctx->dst_height,
                 output_suffix);
    } else if (ctx->input_is_hw && ctx->src_sw_fmt == AV_PIX_FMT_NV16 && same_size) {
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "scale_rkrga=w=%d:h=%d:format=nv12:core=rga2_core0:async_depth=0%s",
                 ctx->dst_width,
                 ctx->dst_height,
                 output_suffix);
    } else if (ctx->input_is_hw && ctx->src_sw_fmt == AV_PIX_FMT_NV16) {
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "scale_rkrga=w=%d:h=%d:format=nv16:core=rga3_core0:async_depth=0,"
                 "scale_rkrga=w=%d:h=%d:format=nv12:core=rga2_core0:async_depth=0%s",
                 ctx->dst_width,
                 ctx->dst_height,
                 ctx->dst_width,
                 ctx->dst_height,
                 output_suffix);
    } else if (ctx->input_is_hw && ctx->src_sw_fmt == AV_PIX_FMT_BGR24 && same_size) {
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "scale_rkrga=w=%d:h=%d:format=nv12:core=rga2_core0:async_depth=0%s",
                 ctx->dst_width,
                 ctx->dst_height,
                 output_suffix);
    } else if (ctx->input_is_hw && ctx->src_sw_fmt == AV_PIX_FMT_BGR24) {
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "scale_rkrga=w=%d:h=%d:format=bgr24:core=rga3_core0:async_depth=0,"
                 "scale_rkrga=w=%d:h=%d:format=nv12:core=rga2_core0:async_depth=0%s",
                 ctx->dst_width,
                 ctx->dst_height,
                 ctx->dst_width,
                 ctx->dst_height,
                 output_suffix);
    } else if (ctx->input_is_hw) {
        LOGE("unsupported RKRGA hardware source software format: %s",
             output_encode_rkrga_pix_fmt_name(ctx->src_sw_fmt));
        return APP_ERR_UNSUPPORTED;
    } else if (same_size) {
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "format=pix_fmts=bgr24,hwupload,"
                 "scale_rkrga=w=%d:h=%d:format=nv12:core=rga2_core0:async_depth=0%s",
                 ctx->dst_width,
                 ctx->dst_height,
                 output_suffix);
    } else {
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "format=pix_fmts=bgr24,hwupload,"
                 "scale_rkrga=w=%d:h=%d:format=bgr24:core=rga3_core0:async_depth=0,"
                 "scale_rkrga=w=%d:h=%d:format=nv12:core=rga2_core0:async_depth=0%s",
                 ctx->dst_width,
                 ctx->dst_height,
                 ctx->dst_width,
                 ctx->dst_height,
                 output_suffix);
    }

    outputs = avfilter_inout_alloc();
    inputs = avfilter_inout_alloc();
    if (outputs == NULL || inputs == NULL) {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        return APP_ERR_NOMEM;
    }
    outputs->name = av_strdup("in");
    outputs->filter_ctx = ctx->source;
    outputs->pad_idx = 0;
    outputs->next = NULL;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = ctx->sink;
    inputs->pad_idx = 0;
    inputs->next = NULL;
    if (outputs->name == NULL || inputs->name == NULL) {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        return APP_ERR_NOMEM;
    }

    ret = avfilter_graph_parse_ptr(ctx->graph,
                                   filter_desc,
                                   &inputs,
                                   &outputs,
                                   NULL);
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    if (ret < 0) {
        output_encode_rkrga_filter_log_error("parse RKRGA filter graph", ret);
        return APP_ERR_FFMPEG;
    }

    for (i = 0; i < ctx->graph->nb_filters; ++i) {
        AVFilterContext *filter = ctx->graph->filters[i];

        if (filter->hw_device_ctx == NULL) {
            filter->hw_device_ctx = av_buffer_ref(ctx->hw_device);
            if (filter->hw_device_ctx == NULL) {
                return APP_ERR_NOMEM;
            }
        }
    }

    ret = avfilter_graph_config(ctx->graph, NULL);
    if (ret < 0) {
        output_encode_rkrga_filter_log_error("configure RKRGA filter graph", ret);
        return APP_ERR_FFMPEG;
    }

    ctx->output_frame = av_frame_alloc();
    if (ctx->output_frame == NULL) {
        return APP_ERR_NOMEM;
    }
    LOGI("RKRGA filter opened: %dx%d %s%s -> %dx%d %s%s (%s%s, %s)",
         ctx->src_width,
         ctx->src_height,
         ctx->input_is_hw ? output_encode_rkrga_pix_fmt_name(ctx->src_sw_fmt)
                          : output_encode_rkrga_pix_fmt_name(ctx->src_fmt),
         ctx->input_is_hw ? " DRM" : "",
         ctx->dst_width,
         ctx->dst_height,
         output_encode_rkrga_pix_fmt_name(ctx->dst_fmt),
         ctx->output_is_hw ? " DRM" : "",
         ctx->input_is_hw ? "" : "hwupload, ",
         ctx->src_sw_fmt == AV_PIX_FMT_NV12
             ? (same_size ? "direct" : "RGA3 resize")
             : (same_size ? "RGA2 color" : "RGA3 resize + RGA2 color"),
         ctx->output_is_hw ? "hardware output" : "hwdownload");
    return APP_OK;
}

app_status_t output_encode_rkrga_filter_create(output_encode_rkrga_filter_t **out,
                                               int src_width,
                                               int src_height,
                                               enum AVPixelFormat src_fmt,
                                               int dst_width,
                                               int dst_height,
                                               enum AVPixelFormat dst_fmt,
                                               int output_is_hw) {
    output_encode_rkrga_filter_t *ctx;
    app_status_t status;

    if (out == NULL || src_width <= 0 || src_height <= 0 ||
        dst_width <= 0 || dst_height <= 0 ||
        src_fmt != AV_PIX_FMT_BGR24 || dst_fmt != AV_PIX_FMT_NV12) {
        return APP_ERR_PARAM;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return APP_ERR_NOMEM;
    }
    ctx->src_width = src_width;
    ctx->src_height = src_height;
    ctx->src_fmt = src_fmt;
    ctx->dst_width = dst_width;
    ctx->dst_height = dst_height;
    ctx->dst_fmt = dst_fmt;
    ctx->output_is_hw = output_is_hw != 0;

    status = output_encode_rkrga_filter_build(ctx);
    if (status != APP_OK) {
        output_encode_rkrga_filter_destroy(&ctx);
        return status;
    }
    *out = ctx;
    return APP_OK;
}

app_status_t output_encode_rkrga_filter_create_hw(output_encode_rkrga_filter_t **out,
                                                  int src_width,
                                                  int src_height,
                                                  AVBufferRef *src_hw_frames_ctx,
                                                  int dst_width,
                                                  int dst_height,
                                                  enum AVPixelFormat dst_fmt,
                                                  int output_is_hw) {
    output_encode_rkrga_filter_t *ctx;
    app_status_t status;

    if (out == NULL || src_width <= 0 || src_height <= 0 ||
        src_hw_frames_ctx == NULL || dst_width <= 0 || dst_height <= 0 ||
        dst_fmt != AV_PIX_FMT_NV12) {
        return APP_ERR_PARAM;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return APP_ERR_NOMEM;
    }
    ctx->src_width = src_width;
    ctx->src_height = src_height;
    ctx->src_fmt = AV_PIX_FMT_DRM_PRIME;
    ctx->dst_width = dst_width;
    ctx->dst_height = dst_height;
    ctx->dst_fmt = dst_fmt;
    ctx->input_is_hw = 1;
    ctx->output_is_hw = output_is_hw != 0;
    ctx->src_hw_frames_ctx = av_buffer_ref(src_hw_frames_ctx);
    if (ctx->src_hw_frames_ctx == NULL) {
        free(ctx);
        return APP_ERR_NOMEM;
    }

    status = output_encode_rkrga_filter_build(ctx);
    if (status != APP_OK) {
        output_encode_rkrga_filter_destroy(&ctx);
        return status;
    }
    *out = ctx;
    return APP_OK;
}

const AVFrame *output_encode_rkrga_filter_process(output_encode_rkrga_filter_t *ctx,
                                                  const AVFrame *src_frame) {
    AVFrame *input_frame;
    int ret;

    if (ctx == NULL || src_frame == NULL ||
        src_frame->width != ctx->src_width ||
        src_frame->height != ctx->src_height ||
        src_frame->format != ctx->src_fmt ||
        (ctx->input_is_hw && src_frame->hw_frames_ctx == NULL)) {
        return NULL;
    }

    av_frame_unref(ctx->output_frame);
    input_frame = av_frame_clone(src_frame);
    if (input_frame == NULL) {
        return NULL;
    }
    input_frame->color_range = AVCOL_RANGE_JPEG;
    input_frame->colorspace = AVCOL_SPC_BT709;
    input_frame->color_primaries = AVCOL_PRI_BT709;
    input_frame->color_trc = AVCOL_TRC_BT709;

    ret = av_buffersrc_add_frame_flags(ctx->source,
                                       input_frame,
                                       AV_BUFFERSRC_FLAG_KEEP_REF);
    av_frame_free(&input_frame);
    if (ret < 0) {
        output_encode_rkrga_filter_log_error("submit RKRGA input frame", ret);
        return NULL;
    }
    ret = av_buffersink_get_frame(ctx->sink, ctx->output_frame);
    if (ret < 0) {
        output_encode_rkrga_filter_log_error("receive RKRGA output frame", ret);
        return NULL;
    }
    if ((ctx->output_is_hw &&
         (ctx->output_frame->format != AV_PIX_FMT_DRM_PRIME ||
          ctx->output_frame->hw_frames_ctx == NULL)) ||
        (!ctx->output_is_hw && ctx->output_frame->format != ctx->dst_fmt)) {
        LOGE("RKRGA output format mismatch: expected=%s actual=%s hw_ctx=%p",
             ctx->output_is_hw ? "drm_prime" : output_encode_rkrga_pix_fmt_name(ctx->dst_fmt),
             output_encode_rkrga_pix_fmt_name(
                 (enum AVPixelFormat)ctx->output_frame->format),
             (void *)ctx->output_frame->hw_frames_ctx);
        av_frame_unref(ctx->output_frame);
        return NULL;
    }

    ctx->output_frame->color_range = AVCOL_RANGE_MPEG;
    ctx->output_frame->colorspace = AVCOL_SPC_BT709;
    ctx->output_frame->color_primaries = AVCOL_PRI_BT709;
    ctx->output_frame->color_trc = AVCOL_TRC_BT709;
    ctx->output_frame->pts = src_frame->pts;
    return ctx->output_frame;
}

void output_encode_rkrga_filter_destroy(output_encode_rkrga_filter_t **ctx_ptr) {
    output_encode_rkrga_filter_t *ctx;

    if (ctx_ptr == NULL || *ctx_ptr == NULL) {
        return;
    }
    ctx = *ctx_ptr;
    av_frame_free(&ctx->output_frame);
    avfilter_graph_free(&ctx->graph);
    av_buffer_unref(&ctx->src_hw_frames_ctx);
    av_buffer_unref(&ctx->hw_device);
    free(ctx);
    *ctx_ptr = NULL;
}
