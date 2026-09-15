#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "video_rkrga_filter.c"

#include "process/video/video_rkrga_filter.h"

#include <errno.h>
#include <linux/dma-buf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avstring.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#include "common/debug.h"

struct video_rkrga_filter {
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
    int software_map_input;
    int output_is_hw;
    int dst_width;
    int dst_height;
    enum AVPixelFormat dst_fmt;
};

typedef struct {
    void *address[AV_NUM_DATA_POINTERS];
    size_t size[AV_NUM_DATA_POINTERS];
    int fd[AV_NUM_DATA_POINTERS];
    int sync_started[AV_NUM_DATA_POINTERS];
    int object_count;
    AVBufferRef *source_ref;
} video_frame_convert_drm_map_t;

static void video_rkrga_filter_log_error(const char *operation, int error) {
    char message[AV_ERROR_MAX_STRING_SIZE];

    av_strerror(error, message, sizeof(message));
    LOGE("%s failed: %s (%d)", operation, message, error);
}

static const char *video_frame_convert_rkrga_pix_fmt_name(enum AVPixelFormat fmt) {
    const char *name = av_get_pix_fmt_name(fmt);

    return name != NULL ? name : "unknown";
}

static int video_rkrga_filter_format_is_yuvj(enum AVPixelFormat fmt) {
    return fmt == AV_PIX_FMT_YUVJ420P ||
           fmt == AV_PIX_FMT_YUVJ422P ||
           fmt == AV_PIX_FMT_YUVJ444P ||
           fmt == AV_PIX_FMT_YUVJ440P;
}

static void video_rkrga_filter_reset_graph(video_rkrga_filter_t *ctx) {
    av_frame_free(&ctx->output_frame);
    avfilter_graph_free(&ctx->graph);
    av_buffer_unref(&ctx->hw_device);
    ctx->source = NULL;
    ctx->sink = NULL;
}

static void video_frame_convert_drm_map_release(void *opaque, uint8_t *data) {
    video_frame_convert_drm_map_t *map = (video_frame_convert_drm_map_t *)opaque;
    int i;

    (void)data;
    if (map == NULL) {
        return;
    }
    for (i = 0; i < map->object_count; ++i) {
        if (map->sync_started[i]) {
            struct dma_buf_sync sync = {
                .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ
            };

            ioctl(map->fd[i], DMA_BUF_IOCTL_SYNC, &sync);
        }
        if (map->address[i] != MAP_FAILED && map->address[i] != NULL) {
            munmap(map->address[i], map->size[i]);
        }
    }
    av_buffer_unref(&map->source_ref);
    free(map);
}

static AVFrame *video_frame_convert_map_drm_frame(const AVFrame *src_frame,
                                            enum AVPixelFormat sw_format) {
    const AVDRMFrameDescriptor *desc;
    const AVDRMLayerDescriptor *layer;
    video_frame_convert_drm_map_t *map;
    AVFrame *mapped_frame;
    AVBufferRef *buffer_ref;
    int i;
    int ret;

    if (src_frame == NULL || src_frame->format != AV_PIX_FMT_DRM_PRIME ||
        src_frame->data[0] == NULL || src_frame->buf[0] == NULL) {
        return NULL;
    }
    desc = (const AVDRMFrameDescriptor *)src_frame->data[0];
    if (desc->nb_objects != 1 ||
        desc->nb_layers != 1) {
        return NULL;
    }
    layer = &desc->layers[0];
    if (layer->nb_planes <= 0 || layer->nb_planes > AV_NUM_DATA_POINTERS) {
        return NULL;
    }

    map = (video_frame_convert_drm_map_t *)calloc(1, sizeof(*map));
    if (map == NULL) {
        return NULL;
    }
    map->object_count = desc->nb_objects;
    for (i = 0; i < map->object_count; ++i) {
        map->address[i] = MAP_FAILED;
        map->size[i] = desc->objects[i].size;
        map->fd[i] = desc->objects[i].fd;
        if (desc->objects[i].format_modifier != 0 || map->size[i] == 0) {
            video_frame_convert_drm_map_release(map, NULL);
            return NULL;
        }
        map->address[i] = mmap(NULL,
                               map->size[i],
                               PROT_READ,
                               MAP_SHARED,
                               desc->objects[i].fd,
                               0);
        if (map->address[i] == MAP_FAILED) {
            LOGE("map external DRM object %d failed: %s", i, strerror(errno));
            video_frame_convert_drm_map_release(map, NULL);
            return NULL;
        }
        {
            struct dma_buf_sync sync = {
                .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ
            };

            if (ioctl(map->fd[i], DMA_BUF_IOCTL_SYNC, &sync) == 0) {
                map->sync_started[i] = 1;
            } else if (errno != ENOTTY) {
                LOGW("start DRM object %d CPU access sync failed: %s",
                     i,
                     strerror(errno));
            }
        }
    }
    map->source_ref = av_buffer_ref(src_frame->buf[0]);
    if (map->source_ref == NULL) {
        video_frame_convert_drm_map_release(map, NULL);
        return NULL;
    }

    mapped_frame = av_frame_alloc();
    if (mapped_frame == NULL) {
        video_frame_convert_drm_map_release(map, NULL);
        return NULL;
    }
    buffer_ref = av_buffer_create((uint8_t *)map->address[0],
                                  map->size[0],
                                  video_frame_convert_drm_map_release,
                                  map,
                                  AV_BUFFER_FLAG_READONLY);
    if (buffer_ref == NULL) {
        av_frame_free(&mapped_frame);
        video_frame_convert_drm_map_release(map, NULL);
        return NULL;
    }
    mapped_frame->buf[0] = buffer_ref;
    mapped_frame->format = sw_format;
    mapped_frame->width = src_frame->width;
    mapped_frame->height = src_frame->height;
    for (i = 0; i < layer->nb_planes; ++i) {
        unsigned int object_index = layer->planes[i].object_index;
        size_t offset = layer->planes[i].offset;
        unsigned int plane_height = i == 1 && sw_format == AV_PIX_FMT_NV12
                                        ? (src_frame->height + 1) / 2
                                        : src_frame->height;
        uint64_t plane_end = (uint64_t)offset +
                             (uint64_t)layer->planes[i].pitch * plane_height;

        if (object_index >= (unsigned int)map->object_count ||
            offset >= map->size[object_index] ||
            layer->planes[i].pitch == 0 ||
            plane_end > map->size[object_index]) {
            av_frame_free(&mapped_frame);
            return NULL;
        }
        mapped_frame->data[i] = (uint8_t *)map->address[object_index] + offset;
        mapped_frame->linesize[i] = (int)layer->planes[i].pitch;
    }
    mapped_frame->extended_data = mapped_frame->data;
    ret = av_frame_copy_props(mapped_frame, src_frame);
    if (ret < 0) {
        av_frame_free(&mapped_frame);
        return NULL;
    }
    return mapped_frame;
}

static app_status_t video_rkrga_filter_build(video_rkrga_filter_t *ctx) {
    const AVFilter *buffer_filter;
    const AVFilter *buffersink_filter;
    AVBufferSrcParameters *source_params = NULL;
    AVHWFramesContext *src_frames_ctx = NULL;
    AVFilterInOut *inputs = NULL;
    AVFilterInOut *outputs = NULL;
    char source_args[256];
    char filter_desc[512];
    const char *output_suffix;
    enum AVPixelFormat source_fmt;
    int source_is_hw;
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
    if ((!ctx->input_is_hw || ctx->software_map_input) &&
        (avfilter_get_by_name("scale") == NULL ||
         avfilter_get_by_name("format") == NULL ||
         avfilter_get_by_name("hwupload") == NULL)) {
        LOGE("required FFmpeg scale/format/hwupload filters are unavailable");
        return APP_ERR_FFMPEG;
    }

    ctx->graph = avfilter_graph_alloc();
    if (ctx->graph == NULL) {
        return APP_ERR_NOMEM;
    }

    source_is_hw = ctx->input_is_hw && !ctx->software_map_input;
    source_fmt = ctx->software_map_input ? ctx->src_sw_fmt : ctx->src_fmt;
    if (source_is_hw) {
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
    } else if (ctx->input_is_hw && ctx->src_hw_frames_ctx != NULL &&
               ctx->src_hw_frames_ctx->data != NULL) {
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
            video_rkrga_filter_log_error("create RKMPP hardware device", ret);
            return APP_ERR_FFMPEG;
        }
    }

    if (source_is_hw) {
        /* args 里先用软件格式 src_sw_fmt 让 buffersrc init_video 通过，
         * 真正的硬件格式 drm_prime + hw_frames_ctx 由下面的
         * av_buffersrc_parameters_set 设置 */
        snprintf(source_args,
                 sizeof(source_args),
                 "video_size=%dx%d:pix_fmt=%d:time_base=1/1000000:pixel_aspect=1/1",
                 ctx->src_width,
                 ctx->src_height,
                 ctx->src_sw_fmt);
    } else {
        snprintf(source_args,
                 sizeof(source_args),
                 "video_size=%dx%d:pix_fmt=%d:time_base=1/1000000:pixel_aspect=1/1",
                 ctx->src_width,
                 ctx->src_height,
                 source_fmt);
    }
    ret = avfilter_graph_create_filter(&ctx->source,
                                       buffer_filter,
                                       "rkrga_input",
                                       source_args,
                                       NULL,
                                       ctx->graph);
    if (ret < 0) {
        video_rkrga_filter_log_error("create RKRGA buffer source", ret);
        return APP_ERR_FFMPEG;
    }
    if (source_is_hw) {
        source_params = av_buffersrc_parameters_alloc();
        if (source_params == NULL) {
            return APP_ERR_NOMEM;
        }
        source_params->format = source_fmt;
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
            video_rkrga_filter_log_error("set RKRGA hardware source parameters", ret);
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
        video_rkrga_filter_log_error("create RKRGA buffer sink", ret);
        return APP_ERR_FFMPEG;
    }

    same_size = ctx->src_width == ctx->dst_width &&
                ctx->src_height == ctx->dst_height;
    output_suffix = ctx->output_is_hw
                        ? ""
                        : ",hwdownload,format=pix_fmts=nv12";
    if (ctx->software_map_input) {
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "scale=w=%d:h=%d:flags=fast_bilinear,"
                 "format=pix_fmts=nv12,hwupload%s",
                 ctx->dst_width,
                  ctx->dst_height,
                  output_suffix);
    } else if (!ctx->input_is_hw && ctx->src_fmt != AV_PIX_FMT_BGR24) {
        if (video_rkrga_filter_format_is_yuvj(ctx->src_fmt)) {
            snprintf(filter_desc,
                     sizeof(filter_desc),
                     "scale=w=%d:h=%d:flags=fast_bilinear:"
                     "in_range=full:out_range=tv:out_color_matrix=bt709,"
                     "format=pix_fmts=nv12,hwupload%s",
                     ctx->dst_width,
                     ctx->dst_height,
                     output_suffix);
        } else {
            snprintf(filter_desc,
                     sizeof(filter_desc),
                     "scale=w=%d:h=%d:flags=fast_bilinear:"
                     "out_range=tv:out_color_matrix=bt709,"
                     "format=pix_fmts=nv12,hwupload%s",
                     ctx->dst_width,
                     ctx->dst_height,
                     output_suffix);
        }
    } else if (ctx->input_is_hw && ctx->src_sw_fmt == AV_PIX_FMT_NV12 && same_size) {
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
    } else if (ctx->input_is_hw &&
               ctx->src_sw_fmt == AV_PIX_FMT_NV16 && same_size) {
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "scale_rkrga=w=%d:h=%d:format=nv12:core=rga3_core0:async_depth=0%s",
                 ctx->dst_width,
                 ctx->dst_height,
                 output_suffix);
    } else if (ctx->input_is_hw && ctx->src_sw_fmt == AV_PIX_FMT_NV24) {
        /* NV24 is supported by RGA2-Pro only; resize and convert in one pass. */
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "scale_rkrga=w=%d:h=%d:format=nv12:async_depth=0%s",
                 ctx->dst_width,
                 ctx->dst_height,
                 output_suffix);
    } else if (ctx->input_is_hw && ctx->src_sw_fmt == AV_PIX_FMT_NV16) {
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "scale_rkrga=w=%d:h=%d:format=nv12:core=rga3_core0:async_depth=0%s",
                 ctx->dst_width,
                 ctx->dst_height,
                 output_suffix);
    } else if (ctx->input_is_hw && ctx->src_sw_fmt == AV_PIX_FMT_BGR24 && same_size) {
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "scale_rkrga=w=%d:h=%d:format=nv12:core=rga3_core0:async_depth=0%s",
                 ctx->dst_width,
                 ctx->dst_height,
                 output_suffix);
    } else if (ctx->input_is_hw && ctx->src_sw_fmt == AV_PIX_FMT_BGR24) {
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "scale_rkrga=w=%d:h=%d:format=bgr24:core=rga3_core0:async_depth=0,"
                 "scale_rkrga=w=%d:h=%d:format=nv12:core=rga3_core0:async_depth=0%s",
                 ctx->dst_width,
                 ctx->dst_height,
                 ctx->dst_width,
                 ctx->dst_height,
                 output_suffix);
    } else if (ctx->input_is_hw) {
        LOGE("unsupported RKRGA hardware source software format: %s",
             video_frame_convert_rkrga_pix_fmt_name(ctx->src_sw_fmt));
        return APP_ERR_UNSUPPORTED;
    } else if (same_size) {
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "format=pix_fmts=bgr24,hwupload,"
                 "scale_rkrga=w=%d:h=%d:format=nv12:core=rga3_core0:async_depth=0%s",
                 ctx->dst_width,
                 ctx->dst_height,
                 output_suffix);
    } else {
        snprintf(filter_desc,
                 sizeof(filter_desc),
                 "format=pix_fmts=bgr24,hwupload,"
                 "scale_rkrga=w=%d:h=%d:format=bgr24:core=rga3_core0:async_depth=0,"
                 "scale_rkrga=w=%d:h=%d:format=nv12:core=rga3_core0:async_depth=0%s",
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
        video_rkrga_filter_log_error("parse RKRGA filter graph", ret);
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
        video_rkrga_filter_log_error("configure RKRGA filter graph", ret);
        return APP_ERR_FFMPEG;
    }

    ctx->output_frame = av_frame_alloc();
    if (ctx->output_frame == NULL) {
        return APP_ERR_NOMEM;
    }
    LOGD("RKRGA filter opened: %dx%d %s%s -> %dx%d %s%s (%s%s, %s)",
         ctx->src_width,
         ctx->src_height,
         ctx->input_is_hw ? video_frame_convert_rkrga_pix_fmt_name(ctx->src_sw_fmt)
                          : video_frame_convert_rkrga_pix_fmt_name(ctx->src_fmt),
         ctx->input_is_hw ? " DRM" : "",
         ctx->dst_width,
         ctx->dst_height,
         video_frame_convert_rkrga_pix_fmt_name(ctx->dst_fmt),
         ctx->output_is_hw ? " DRM" : "",
         ctx->software_map_input ? "DMA-BUF mmap, "
                                 : (ctx->input_is_hw ? "" : "hwupload, "),
         ctx->software_map_input
             ? "software NV24 to NV12 + hwupload"
             : (!ctx->input_is_hw && ctx->src_fmt != AV_PIX_FMT_BGR24
                    ? "software scale/color to NV12 + hwupload"
             : (ctx->src_sw_fmt == AV_PIX_FMT_NV12
                    ? (same_size ? "direct" : "RGA3 resize")
                    : (ctx->src_sw_fmt == AV_PIX_FMT_NV24
                           ? "RGA2-Pro resize + color"
                           : (ctx->src_sw_fmt == AV_PIX_FMT_NV16
                                  ? (same_size ? "RGA2 color"
                                               : "RGA2 resize + color")
                                   : (same_size ? "RGA2 color"
                                                : "RGA3 resize + RGA2 color"))))),
         ctx->output_is_hw ? "hardware output" : "hwdownload");
    return APP_OK;
}

app_status_t video_rkrga_filter_create(video_rkrga_filter_t **out,
                                               int src_width,
                                               int src_height,
                                               enum AVPixelFormat src_fmt,
                                               int dst_width,
                                               int dst_height,
                                               enum AVPixelFormat dst_fmt,
                                               int output_is_hw) {
    video_rkrga_filter_t *ctx;
    app_status_t status;

    if (out == NULL || src_width <= 0 || src_height <= 0 ||
        dst_width <= 0 || dst_height <= 0 ||
        src_fmt == AV_PIX_FMT_NONE || src_fmt == AV_PIX_FMT_DRM_PRIME ||
        av_pix_fmt_desc_get(src_fmt) == NULL || dst_fmt != AV_PIX_FMT_NV12) {
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

    status = video_rkrga_filter_build(ctx);
    if (status != APP_OK) {
        video_rkrga_filter_destroy(&ctx);
        return status;
    }
    *out = ctx;
    return APP_OK;
}

app_status_t video_rkrga_filter_create_hw(video_rkrga_filter_t **out,
                                                  int src_width,
                                                  int src_height,
                                                  AVBufferRef *src_hw_frames_ctx,
                                                  int dst_width,
                                                  int dst_height,
                                                  enum AVPixelFormat dst_fmt,
                                                  int output_is_hw) {
    video_rkrga_filter_t *ctx;
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

    status = video_rkrga_filter_build(ctx);
    if (status != APP_OK && ctx->src_sw_fmt == AV_PIX_FMT_NV24) {
        LOGW("NV24 RGA2-Pro path unavailable; fallback to DMA-BUF mmap + "
             "software NV24-to-NV12 conversion");
        video_rkrga_filter_reset_graph(ctx);
        ctx->software_map_input = 1;
        status = video_rkrga_filter_build(ctx);
    }
    if (status != APP_OK) {
        video_rkrga_filter_destroy(&ctx);
        return status;
    }
    *out = ctx;
    return APP_OK;
}

const AVFrame *video_rkrga_filter_process(video_rkrga_filter_t *ctx,
                                                  const AVFrame *src_frame) {
    AVFrame *input_frame;
    enum AVPixelFormat input_sw_fmt;
    int ret;

    if (ctx == NULL || src_frame == NULL ||
        src_frame->width != ctx->src_width ||
        src_frame->height != ctx->src_height ||
        src_frame->format != ctx->src_fmt ||
        (ctx->input_is_hw && src_frame->hw_frames_ctx == NULL)) {
        return NULL;
    }

    av_frame_unref(ctx->output_frame);
    input_frame = ctx->software_map_input
                      ? video_frame_convert_map_drm_frame(src_frame, ctx->src_sw_fmt)
                      : av_frame_clone(src_frame);
    if (input_frame == NULL) {
        return NULL;
    }
    input_sw_fmt = ctx->software_map_input
                       ? ctx->src_sw_fmt
                       : (ctx->input_is_hw ? ctx->src_sw_fmt : ctx->src_fmt);
    if (input_frame->color_range == AVCOL_RANGE_UNSPECIFIED) {
        input_frame->color_range =
            input_sw_fmt == AV_PIX_FMT_BGR24 ||
                    video_rkrga_filter_format_is_yuvj(input_sw_fmt)
                ? AVCOL_RANGE_JPEG
                : AVCOL_RANGE_MPEG;
    }
    if (input_frame->colorspace == AVCOL_SPC_UNSPECIFIED) {
        input_frame->colorspace = input_sw_fmt == AV_PIX_FMT_BGR24
                                      ? AVCOL_SPC_RGB
                                      : AVCOL_SPC_BT709;
    }
    if (input_frame->color_primaries == AVCOL_PRI_UNSPECIFIED) {
        input_frame->color_primaries = AVCOL_PRI_BT709;
    }
    if (input_frame->color_trc == AVCOL_TRC_UNSPECIFIED) {
        input_frame->color_trc = AVCOL_TRC_BT709;
    }

    ret = av_buffersrc_add_frame_flags(ctx->source,
                                       input_frame,
                                       AV_BUFFERSRC_FLAG_KEEP_REF);
    av_frame_free(&input_frame);
    if (ret < 0) {
        video_rkrga_filter_log_error("submit RKRGA input frame", ret);
        return NULL;
    }
    ret = av_buffersink_get_frame(ctx->sink, ctx->output_frame);
    if (ret < 0) {
        video_rkrga_filter_log_error("receive RKRGA output frame", ret);
        return NULL;
    }
    if ((ctx->output_is_hw &&
         (ctx->output_frame->format != AV_PIX_FMT_DRM_PRIME ||
          ctx->output_frame->hw_frames_ctx == NULL)) ||
        (!ctx->output_is_hw && ctx->output_frame->format != ctx->dst_fmt)) {
        LOGE("RKRGA output format mismatch: expected=%s actual=%s hw_ctx=%p",
             ctx->output_is_hw ? "drm_prime" : video_frame_convert_rkrga_pix_fmt_name(ctx->dst_fmt),
             video_frame_convert_rkrga_pix_fmt_name(
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

void video_rkrga_filter_destroy(video_rkrga_filter_t **ctx_ptr) {
    video_rkrga_filter_t *ctx;

    if (ctx_ptr == NULL || *ctx_ptr == NULL) {
        return;
    }
    ctx = *ctx_ptr;
    video_rkrga_filter_reset_graph(ctx);
    av_buffer_unref(&ctx->src_hw_frames_ctx);
    free(ctx);
    *ctx_ptr = NULL;
}
