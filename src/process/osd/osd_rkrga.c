#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "osd_rkrga.c"

#include "process/osd/osd_rkrga.h"

#include <stdint.h>
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

struct osd_rkrga {
    AVFilterGraph *graph;
    AVFilterContext *main_source;
    AVFilterContext *overlay_source;
    AVFilterContext *sink;
    AVBufferRef *hw_device;
    AVBufferRef *main_hw_frames_ctx;
    AVFrame *output_frame;
    int width;
    int height;
    int overlay_width;
    int overlay_height;
    int overlay_x;
    int overlay_y;
    uint64_t overlay_revision;
    int overlay_submitted;
};

static void osd_rkrga_log_error(const char *operation, int error) {
    char message[AV_ERROR_MAX_STRING_SIZE];

    av_strerror(error, message, sizeof(message));
    LOGE("%s failed: %s (%d)", operation, message, error);
}

static void osd_rkrga_reset_graph(osd_rkrga_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    if (ctx->output_frame != NULL) {
        av_frame_free(&ctx->output_frame);
    }
    avfilter_graph_free(&ctx->graph);
    av_buffer_unref(&ctx->main_hw_frames_ctx);
    av_buffer_unref(&ctx->hw_device);
    ctx->main_source = NULL;
    ctx->overlay_source = NULL;
    ctx->sink = NULL;
    ctx->overlay_revision = UINT64_MAX;
    ctx->overlay_submitted = 0;
}

static int osd_rkrga_main_matches(const osd_rkrga_t *ctx,
                                         const AVFrame *main_frame) {
    return ctx->graph != NULL &&
           ctx->main_hw_frames_ctx != NULL &&
           main_frame->hw_frames_ctx != NULL &&
           ctx->main_hw_frames_ctx->data == main_frame->hw_frames_ctx->data;
}

static app_status_t osd_rkrga_build(osd_rkrga_t *ctx,
                                           const AVFrame *main_frame) {
    const AVFilter *buffer_filter;
    const AVFilter *buffersink_filter;
    AVHWFramesContext *main_frames;
    AVBufferSrcParameters *main_params = NULL;
    AVFilterInOut *main_output = NULL;
    AVFilterInOut *overlay_output = NULL;
    AVFilterInOut *graph_input = NULL;
    char main_args[256];
    char overlay_args[256];
    char filter_desc[512];
    unsigned int i;
    int ret;

    if (ctx == NULL || main_frame == NULL ||
        main_frame->format != AV_PIX_FMT_DRM_PRIME ||
        main_frame->hw_frames_ctx == NULL ||
        main_frame->width != ctx->width ||
        main_frame->height != ctx->height) {
        return APP_ERR_PARAM;
    }

    main_frames = (AVHWFramesContext *)main_frame->hw_frames_ctx->data;
    if (main_frames == NULL || main_frames->sw_format != AV_PIX_FMT_NV12) {
        LOGE("OSD main frame must be NV12 DRM PRIME");
        return APP_ERR_UNSUPPORTED;
    }

    osd_rkrga_reset_graph(ctx);
    buffer_filter = avfilter_get_by_name("buffer");
    buffersink_filter = avfilter_get_by_name("buffersink");
    if (buffer_filter == NULL || buffersink_filter == NULL ||
        avfilter_get_by_name("hwupload") == NULL ||
        avfilter_get_by_name("overlay_rkrga") == NULL) {
        LOGE("required buffer/hwupload/overlay_rkrga filters are unavailable");
        return APP_ERR_UNSUPPORTED;
    }

    ctx->graph = avfilter_graph_alloc();
    if (ctx->graph == NULL) {
        return APP_ERR_NOMEM;
    }
    ctx->main_hw_frames_ctx = av_buffer_ref(main_frame->hw_frames_ctx);
    ctx->hw_device = av_buffer_ref(main_frames->device_ref);
    if (ctx->main_hw_frames_ctx == NULL || ctx->hw_device == NULL) {
        osd_rkrga_reset_graph(ctx);
        return APP_ERR_NOMEM;
    }

    snprintf(main_args,
             sizeof(main_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/1000000:pixel_aspect=1/1",
             ctx->width,
             ctx->height,
             main_frames->sw_format);
    snprintf(overlay_args,
             sizeof(overlay_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/1000000:pixel_aspect=1/1",
             ctx->overlay_width,
             ctx->overlay_height,
             AV_PIX_FMT_RGBA);
    snprintf(filter_desc,
             sizeof(filter_desc),
             "[overlay]hwupload[overlay_hw];"
             "[main][overlay_hw]overlay_rkrga="
             "x=%d:y=%d:alpha=255:alpha_format=straight:format=nv12:"
             "repeatlast=1:shortest=0:core=rga3_core0:async_depth=0[out]",
             ctx->overlay_x,
             ctx->overlay_y);

    ret = avfilter_graph_create_filter(&ctx->main_source,
                                       buffer_filter,
                                       "osd_main",
                                       main_args,
                                       NULL,
                                       ctx->graph);
    if (ret < 0) {
        osd_rkrga_log_error("create OSD main source", ret);
        goto fail;
    }
    main_params = av_buffersrc_parameters_alloc();
    if (main_params == NULL) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    main_params->format = AV_PIX_FMT_DRM_PRIME;
    main_params->hw_frames_ctx = av_buffer_ref(ctx->main_hw_frames_ctx);
    if (main_params->hw_frames_ctx == NULL) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ret = av_buffersrc_parameters_set(ctx->main_source, main_params);
    av_buffer_unref(&main_params->hw_frames_ctx);
    av_free(main_params);
    main_params = NULL;
    if (ret < 0) {
        osd_rkrga_log_error("set OSD main hardware parameters", ret);
        goto fail;
    }

    ret = avfilter_graph_create_filter(&ctx->overlay_source,
                                       buffer_filter,
                                       "osd_overlay",
                                       overlay_args,
                                       NULL,
                                       ctx->graph);
    if (ret < 0) {
        osd_rkrga_log_error("create OSD overlay source", ret);
        goto fail;
    }
    ret = avfilter_graph_create_filter(&ctx->sink,
                                       buffersink_filter,
                                       "osd_output",
                                       NULL,
                                       NULL,
                                       ctx->graph);
    if (ret < 0) {
        osd_rkrga_log_error("create OSD sink", ret);
        goto fail;
    }

    main_output = avfilter_inout_alloc();
    overlay_output = avfilter_inout_alloc();
    graph_input = avfilter_inout_alloc();
    if (main_output == NULL || overlay_output == NULL || graph_input == NULL) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    main_output->name = av_strdup("main");
    main_output->filter_ctx = ctx->main_source;
    main_output->pad_idx = 0;
    main_output->next = overlay_output;
    overlay_output->name = av_strdup("overlay");
    overlay_output->filter_ctx = ctx->overlay_source;
    overlay_output->pad_idx = 0;
    overlay_output->next = NULL;
    graph_input->name = av_strdup("out");
    graph_input->filter_ctx = ctx->sink;
    graph_input->pad_idx = 0;
    graph_input->next = NULL;
    if (main_output->name == NULL || overlay_output->name == NULL ||
        graph_input->name == NULL) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = avfilter_graph_parse_ptr(ctx->graph,
                                   filter_desc,
                                   &graph_input,
                                   &main_output,
                                   NULL);
    if (ret < 0) {
        osd_rkrga_log_error("parse OSD RKRGA graph", ret);
        goto fail;
    }
    avfilter_inout_free(&main_output);
    avfilter_inout_free(&graph_input);

    for (i = 0; i < ctx->graph->nb_filters; ++i) {
        AVFilterContext *filter = ctx->graph->filters[i];

        if (filter->hw_device_ctx == NULL) {
            filter->hw_device_ctx = av_buffer_ref(ctx->hw_device);
            if (filter->hw_device_ctx == NULL) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        }
    }

    ret = avfilter_graph_config(ctx->graph, NULL);
    if (ret < 0) {
        osd_rkrga_log_error("configure OSD RKRGA graph", ret);
        goto fail;
    }
    ctx->output_frame = av_frame_alloc();
    if (ctx->output_frame == NULL) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    LOGI("hardware OSD opened: main=%dx%d nv12 DRM overlay=%dx%d rgba "
         "output=nv12 DRM filter=overlay_rkrga",
         ctx->width,
         ctx->height,
         ctx->overlay_width,
         ctx->overlay_height);
    return APP_OK;

fail:
    if (main_params != NULL) {
        av_buffer_unref(&main_params->hw_frames_ctx);
        av_free(main_params);
    }
    avfilter_inout_free(&main_output);
    avfilter_inout_free(&graph_input);
    osd_rkrga_reset_graph(ctx);
    return ret == AVERROR(ENOMEM) ? APP_ERR_NOMEM : APP_ERR_FFMPEG;
}

app_status_t osd_rkrga_init(osd_rkrga_t **out,
                                   int width,
                                   int height,
                                   int overlay_width,
                                   int overlay_height,
                                   int overlay_x,
                                   int overlay_y) {
    osd_rkrga_t *ctx;

    if (out == NULL || width <= 0 || height <= 0 ||
        overlay_width <= 0 || overlay_height <= 0 ||
        overlay_x < 0 || overlay_y < 0 ||
        overlay_x + overlay_width > width ||
        overlay_y + overlay_height > height) {
        return APP_ERR_PARAM;
    }
    *out = NULL;
    ctx = (osd_rkrga_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return APP_ERR_NOMEM;
    }
    ctx->width = width;
    ctx->height = height;
    ctx->overlay_width = overlay_width;
    ctx->overlay_height = overlay_height;
    ctx->overlay_x = overlay_x;
    ctx->overlay_y = overlay_y;
    ctx->overlay_revision = UINT64_MAX;
    *out = ctx;
    return APP_OK;
}

const AVFrame *osd_rkrga_process(osd_rkrga_t *ctx,
                                        const AVFrame *main_frame,
                                        const AVFrame *rgba_overlay,
                                        uint64_t overlay_revision) {
    AVFrame *submitted;
    int ret;

    if (ctx == NULL || main_frame == NULL || rgba_overlay == NULL ||
        rgba_overlay->format != AV_PIX_FMT_RGBA ||
        rgba_overlay->width != ctx->overlay_width ||
        rgba_overlay->height != ctx->overlay_height) {
        return NULL;
    }
    if (!osd_rkrga_main_matches(ctx, main_frame)) {
        if (osd_rkrga_build(ctx, main_frame) != APP_OK) {
            return NULL;
        }
    }

    submitted = av_frame_clone(rgba_overlay);
    if (submitted == NULL) {
        return NULL;
    }
    submitted->pts = main_frame->pts;
    ret = av_buffersrc_add_frame_flags(ctx->overlay_source,
                                       submitted,
                                       AV_BUFFERSRC_FLAG_KEEP_REF);
    av_frame_free(&submitted);
    if (ret < 0) {
        osd_rkrga_log_error("submit OSD overlay", ret);
        return NULL;
    }
    ctx->overlay_revision = overlay_revision;
    ctx->overlay_submitted = 1;

    submitted = av_frame_clone(main_frame);
    if (submitted == NULL) {
        return NULL;
    }
    ret = av_buffersrc_add_frame_flags(ctx->main_source,
                                       submitted,
                                       AV_BUFFERSRC_FLAG_KEEP_REF);
    av_frame_free(&submitted);
    if (ret < 0) {
        osd_rkrga_log_error("submit OSD main frame", ret);
        return NULL;
    }

    av_frame_unref(ctx->output_frame);
    ret = av_buffersink_get_frame(ctx->sink, ctx->output_frame);
    if (ret < 0) {
        osd_rkrga_log_error("receive OSD output frame", ret);
        return NULL;
    }
    if (ctx->output_frame->format != AV_PIX_FMT_DRM_PRIME ||
        ctx->output_frame->hw_frames_ctx == NULL) {
        LOGE("hardware OSD returned non-DRM frame fmt=%s hw_ctx=%p",
             av_get_pix_fmt_name((enum AVPixelFormat)ctx->output_frame->format),
             (void *)ctx->output_frame->hw_frames_ctx);
        av_frame_unref(ctx->output_frame);
        return NULL;
    }
    ctx->output_frame->pts = main_frame->pts;
    ctx->output_frame->color_range = AVCOL_RANGE_MPEG;
    ctx->output_frame->colorspace = AVCOL_SPC_BT709;
    ctx->output_frame->color_primaries = AVCOL_PRI_BT709;
    ctx->output_frame->color_trc = AVCOL_TRC_BT709;
    return ctx->output_frame;
}

void osd_rkrga_deinit(osd_rkrga_t **ctx_ptr) {
    osd_rkrga_t *ctx;

    if (ctx_ptr == NULL || *ctx_ptr == NULL) {
        return;
    }
    ctx = *ctx_ptr;
    osd_rkrga_reset_graph(ctx);
    free(ctx);
    *ctx_ptr = NULL;
}
