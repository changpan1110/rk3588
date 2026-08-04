#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "output_display_gui.c"

#include "output/output_display_gui.h"

#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "common/debug.h"

static int output_display_gui_get_direct_texture_format(enum AVPixelFormat fmt, Uint32 *texture_fmt) {
    if (texture_fmt == NULL) {
        return 0;
    }

    switch (fmt) {
        case AV_PIX_FMT_UYVY422:
            *texture_fmt = SDL_PIXELFORMAT_UYVY;
            return 1;
        case AV_PIX_FMT_YUYV422:
            *texture_fmt = SDL_PIXELFORMAT_YUY2;
            return 1;
        default:
            return 0;
    }
}

static int output_display_gui_get_nv_texture_format(enum AVPixelFormat fmt, Uint32 *texture_fmt) {
    if (texture_fmt == NULL) {
        return 0;
    }

    switch (fmt) {
        case AV_PIX_FMT_NV12:
            *texture_fmt = SDL_PIXELFORMAT_NV12;
            return 1;
        case AV_PIX_FMT_NV21:
            *texture_fmt = SDL_PIXELFORMAT_NV21;
            return 1;
        default:
            return 0;
    }
}

static int output_display_gui_get_yuv_texture_format(const AVFrame *frame, Uint32 *texture_fmt) {
    if (frame == NULL || texture_fmt == NULL) {
        return 0;
    }

    /*
     * SDL 的 IYUV 纹理按 BT.601 limited range 渲染。
     * MJPEG 软解输出的是全范围 yuvj420p,留给 sws 路径处理以保证颜色正确。
     */
    if ((enum AVPixelFormat)frame->format == AV_PIX_FMT_YUV420P &&
        frame->color_range != AVCOL_RANGE_JPEG) {
        *texture_fmt = SDL_PIXELFORMAT_IYUV;
        return 1;
    }
    return 0;
}

static int output_display_gui_guess_colorspace(const AVFrame *frame) {
    if (frame->colorspace != AVCOL_SPC_UNSPECIFIED) {
        return (int)frame->colorspace;
    }

    if (frame->width >= 1280 || frame->height >= 720) {
        return SWS_CS_ITU709;
    }
    return SWS_CS_SMPTE170M;
}

static app_status_t output_display_gui_prepare(output_display_gui_ctx_t *ctx, const AVFrame *frame) {
    uint8_t *planes[4] = {0};
    int linesize[4] = {0};
    enum AVPixelFormat src_fmt = (enum AVPixelFormat)frame->format;
    struct SwsContext *sws;
    uint8_t *rgba_data;
    int rgba_buf_size;
    SDL_Texture *texture;
    Uint32 direct_texture_fmt = SDL_PIXELFORMAT_UNKNOWN;
    Uint32 nv_texture_fmt = SDL_PIXELFORMAT_UNKNOWN;
    Uint32 yuv_texture_fmt = SDL_PIXELFORMAT_UNKNOWN;
    int ww;
    int wh;
    int src_cs;
    const int *inv_table;
    const int *table;
    int src_range;
    int dst_range;
    int brightness;
    int contrast;
    int saturation;

    SDL_GetWindowSize((SDL_Window *)ctx->window, &ww, &wh);
    if (ww <= 0) {
        ww = ctx->window_width > 0 ? ctx->window_width : frame->width;
    }
    if (wh <= 0) {
        wh = ctx->window_height > 0 ? ctx->window_height : frame->height;
    }

    if (ctx->texture != NULL &&
        ctx->frame_width == frame->width &&
        ctx->frame_height == frame->height &&
        (ctx->use_direct_upload ||
         ctx->use_nv_upload ||
         ctx->use_yuv_upload ||
         (ctx->texture_width == ww && ctx->texture_height == wh)) &&
        ctx->frame_fmt == src_fmt) {
        return APP_OK;
    }

    ctx->use_direct_upload = output_display_gui_get_direct_texture_format(src_fmt, &direct_texture_fmt);
    ctx->use_nv_upload = output_display_gui_get_nv_texture_format(src_fmt, &nv_texture_fmt);
    ctx->use_yuv_upload = output_display_gui_get_yuv_texture_format(frame, &yuv_texture_fmt);

    if (ctx->texture != NULL) {
        SDL_DestroyTexture((SDL_Texture *)ctx->texture);
        ctx->texture = NULL;
    }

    if (ctx->use_direct_upload) {
        texture = SDL_CreateTexture((SDL_Renderer *)ctx->renderer,
                                    direct_texture_fmt,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    frame->width,
                                    frame->height);
        if (texture == NULL) {
            LOGE("gui SDL_CreateTexture direct failed: %s", SDL_GetError());
            return APP_ERR_IO;
        }

        sws_freeContext(ctx->sws);
        ctx->sws = NULL;
        free(ctx->rgba_data);
        ctx->rgba_data = NULL;
        ctx->rgba_linesize = 0;
        ctx->rgba_buf_size = 0;
        ctx->frame_width = frame->width;
        ctx->frame_height = frame->height;
        ctx->texture_width = frame->width;
        ctx->texture_height = frame->height;
        ctx->frame_fmt = src_fmt;
        ctx->texture = texture;
        ctx->texture_format = direct_texture_fmt;
        LOGI("gui direct upload src=%dx%d fmt=%d texture_fmt=%u",
             frame->width,
             frame->height,
             src_fmt,
             direct_texture_fmt);
        return APP_OK;
    }

    if (ctx->use_nv_upload) {
        texture = SDL_CreateTexture((SDL_Renderer *)ctx->renderer,
                                    nv_texture_fmt,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    frame->width,
                                    frame->height);
        if (texture == NULL) {
            LOGE("gui SDL_CreateTexture nv failed: %s", SDL_GetError());
            return APP_ERR_IO;
        }

        sws_freeContext(ctx->sws);
        ctx->sws = NULL;
        free(ctx->rgba_data);
        ctx->rgba_data = NULL;
        ctx->rgba_linesize = 0;
        ctx->rgba_buf_size = 0;
        ctx->frame_width = frame->width;
        ctx->frame_height = frame->height;
        ctx->texture_width = frame->width;
        ctx->texture_height = frame->height;
        ctx->frame_fmt = src_fmt;
        ctx->texture = texture;
        ctx->texture_format = nv_texture_fmt;
        LOGI("gui nv upload src=%dx%d fmt=%d texture_fmt=%u",
             frame->width,
             frame->height,
             src_fmt,
             nv_texture_fmt);
        return APP_OK;
    }

    if (ctx->use_yuv_upload) {
        texture = SDL_CreateTexture((SDL_Renderer *)ctx->renderer,
                                    yuv_texture_fmt,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    frame->width,
                                    frame->height);
        if (texture == NULL) {
            LOGE("gui SDL_CreateTexture yuv failed: %s", SDL_GetError());
            return APP_ERR_IO;
        }

        sws_freeContext(ctx->sws);
        ctx->sws = NULL;
        free(ctx->rgba_data);
        ctx->rgba_data = NULL;
        ctx->rgba_linesize = 0;
        ctx->rgba_buf_size = 0;
        ctx->frame_width = frame->width;
        ctx->frame_height = frame->height;
        ctx->texture_width = frame->width;
        ctx->texture_height = frame->height;
        ctx->frame_fmt = src_fmt;
        ctx->texture = texture;
        ctx->texture_format = yuv_texture_fmt;
        LOGI("gui yuv upload src=%dx%d fmt=%d texture_fmt=%u",
             frame->width,
             frame->height,
             src_fmt,
             yuv_texture_fmt);
        return APP_OK;
    }

    sws = sws_getCachedContext(ctx->sws,
                               frame->width,
                               frame->height,
                               src_fmt,
                               ww,
                               wh,
                               AV_PIX_FMT_BGRA,
                               SWS_FAST_BILINEAR,
                               NULL,
                               NULL,
                               NULL);
    if (sws == NULL) {
        LOGE("gui sws_getCachedContext failed");
        return APP_ERR_FFMPEG;
    }

    src_cs = output_display_gui_guess_colorspace(frame);
    inv_table = sws_getCoefficients(src_cs);
    table = sws_getCoefficients(SWS_CS_DEFAULT);
    src_range = frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
    dst_range = 1;
    brightness = 0;
    contrast = 1 << 16;
    saturation = 1 << 16;
    sws_setColorspaceDetails(sws,
                             inv_table,
                             src_range,
                             table,
                             dst_range,
                             brightness,
                             contrast,
                             saturation);

    rgba_buf_size = av_image_get_buffer_size(AV_PIX_FMT_BGRA, ww, wh, 1);
    if (rgba_buf_size <= 0) {
        LOGE("gui av_image_get_buffer_size failed");
        return APP_ERR_FFMPEG;
    }

    rgba_data = realloc(ctx->rgba_data, (size_t)rgba_buf_size);
    if (rgba_data == NULL) {
        LOGE("gui realloc rgba buffer failed");
        return APP_ERR_NOMEM;
    }

    if (av_image_fill_arrays(planes,
                             linesize,
                             rgba_data,
                             AV_PIX_FMT_BGRA,
                             ww,
                             wh,
                             1) < 0) {
        LOGE("gui av_image_fill_arrays failed");
        return APP_ERR_FFMPEG;
    }

    texture = SDL_CreateTexture((SDL_Renderer *)ctx->renderer,
                                SDL_PIXELFORMAT_BGRA32,
                                SDL_TEXTUREACCESS_STREAMING,
                                ww,
                                wh);
    if (texture == NULL) {
        LOGE("gui SDL_CreateTexture failed: %s", SDL_GetError());
        return APP_ERR_IO;
    }

    ctx->sws = sws;
    ctx->rgba_data = rgba_data;
    ctx->rgba_linesize = linesize[0];
    ctx->rgba_buf_size = rgba_buf_size;
    ctx->frame_width = frame->width;
    ctx->frame_height = frame->height;
    ctx->texture_width = ww;
    ctx->texture_height = wh;
    ctx->frame_fmt = src_fmt;
    ctx->texture = texture;
    ctx->texture_format = SDL_PIXELFORMAT_BGRA32;
    LOGI("gui prepare src=%dx%d fmt=%d -> dst=%dx%d colorspace=%d range=%d",
         frame->width,
         frame->height,
         src_fmt,
         ww,
         wh,
         src_cs,
         src_range);
    return APP_OK;
}

app_status_t output_display_gui_init(output_display_gui_ctx_t *ctx, int enabled, int window_width, int window_height) {
    SDL_Renderer *renderer;

    if (ctx == NULL) {
        return APP_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->enabled = enabled;
    ctx->window_width = window_width;
    ctx->window_height = window_height;

    if (!enabled) {
        return APP_OK;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOGE("SDL_Init failed: %s", SDL_GetError());
        return APP_ERR_IO;
    }

    ctx->window = SDL_CreateWindow("rk3588_video_app decode preview",
                                   SDL_WINDOWPOS_CENTERED,
                                   SDL_WINDOWPOS_CENTERED,
                                   window_width,
                                   window_height,
                                   SDL_WINDOW_RESIZABLE);
    if (ctx->window == NULL) {
        LOGE("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return APP_ERR_IO;
    }

    renderer = SDL_CreateRenderer((SDL_Window *)ctx->window,
                                  -1,
                                  SDL_RENDERER_ACCELERATED);
    if (renderer == NULL) {
        LOGW("SDL accelerated renderer failed: %s", SDL_GetError());
        renderer = SDL_CreateRenderer((SDL_Window *)ctx->window, -1, SDL_RENDERER_SOFTWARE);
        if (renderer == NULL) {
            LOGE("SDL_CreateRenderer failed: %s", SDL_GetError());
            SDL_DestroyWindow((SDL_Window *)ctx->window);
            SDL_Quit();
            ctx->window = NULL;
            return APP_ERR_IO;
        }
    }

    ctx->renderer = renderer;
    ctx->initialized = 1;
    return APP_OK;
}

app_status_t output_display_gui_show(output_display_gui_ctx_t *ctx, const video_frame_t *frame) {
    app_status_t status;
    SDL_Rect dst = {0, 0, 0, 0};
    int ww;
    int wh;

    if (ctx == NULL || frame == NULL || frame->av_frame == NULL) {
        return APP_ERR_PARAM;
    }
    if (!ctx->enabled || !ctx->initialized) {
        return APP_OK;
    }

    status = output_display_gui_prepare(ctx, frame->av_frame);
    if (status != APP_OK) {
        return status;
    }

    if (ctx->use_direct_upload) {
        if (SDL_UpdateTexture((SDL_Texture *)ctx->texture,
                              NULL,
                              frame->av_frame->data[0],
                              frame->av_frame->linesize[0]) != 0) {
            LOGE("SDL_UpdateTexture direct failed: %s", SDL_GetError());
            return APP_ERR_IO;
        }
    } else if (ctx->use_nv_upload) {
        if (SDL_UpdateNVTexture((SDL_Texture *)ctx->texture,
                                NULL,
                                frame->av_frame->data[0],
                                frame->av_frame->linesize[0],
                                frame->av_frame->data[1],
                                frame->av_frame->linesize[1]) != 0) {
            LOGE("SDL_UpdateNVTexture failed: %s", SDL_GetError());
            return APP_ERR_IO;
        }
    } else if (ctx->use_yuv_upload) {
        if (SDL_UpdateYUVTexture((SDL_Texture *)ctx->texture,
                                 NULL,
                                 frame->av_frame->data[0],
                                 frame->av_frame->linesize[0],
                                 frame->av_frame->data[1],
                                 frame->av_frame->linesize[1],
                                 frame->av_frame->data[2],
                                 frame->av_frame->linesize[2]) != 0) {
            LOGE("SDL_UpdateYUVTexture failed: %s", SDL_GetError());
            return APP_ERR_IO;
        }
    } else {
        sws_scale(ctx->sws,
                  (const uint8_t * const *)frame->av_frame->data,
                  frame->av_frame->linesize,
                  0,
                  frame->av_frame->height,
                  &ctx->rgba_data,
                  &ctx->rgba_linesize);

        if (SDL_UpdateTexture((SDL_Texture *)ctx->texture, NULL, ctx->rgba_data, ctx->rgba_linesize) != 0) {
            LOGE("SDL_UpdateTexture failed: %s", SDL_GetError());
            return APP_ERR_IO;
        }
    }

    SDL_GetWindowSize((SDL_Window *)ctx->window, &ww, &wh);
    dst.w = ww;
    dst.h = wh;

    SDL_SetRenderDrawColor((SDL_Renderer *)ctx->renderer, 0, 0, 0, 255);
    SDL_RenderClear((SDL_Renderer *)ctx->renderer);
    SDL_RenderCopy((SDL_Renderer *)ctx->renderer, (SDL_Texture *)ctx->texture, NULL, &dst);
    SDL_RenderPresent((SDL_Renderer *)ctx->renderer);
    return APP_OK;
}

int output_display_gui_poll_quit(output_display_gui_ctx_t *ctx) {
    SDL_Event event;

    if (ctx == NULL || !ctx->enabled || !ctx->initialized) {
        return 0;
    }

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            return 1;
        }
    }
    return 0;
}

void output_display_gui_deinit(output_display_gui_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    if (ctx->texture != NULL) {
        SDL_DestroyTexture((SDL_Texture *)ctx->texture);
    }
    if (ctx->renderer != NULL) {
        SDL_DestroyRenderer((SDL_Renderer *)ctx->renderer);
    }
    if (ctx->window != NULL) {
        SDL_DestroyWindow((SDL_Window *)ctx->window);
    }
    if (ctx->initialized) {
        SDL_Quit();
    }
    sws_freeContext(ctx->sws);
    free(ctx->rgba_data);
    memset(ctx, 0, sizeof(*ctx));
}
