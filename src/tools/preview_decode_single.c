#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "preview_decode_single.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "common/common.h"
#include "common/debug.h"
#include "input/input_csi0.h"
#include "input/input_csi1.h"
#include "input/input_hdmi_in.h"
#include "input/input_usb.h"

#define PREVIEW_WINDOW_W 1280
#define PREVIEW_WINDOW_H 720

typedef struct {
    video_source_type_t source_type;
    video_input_config_t cfg;
    union {
        input_usb_ctx_t usb;
        input_csi0_ctx_t csi0;
        input_csi1_ctx_t csi1;
        input_hdmi_in_ctx_t hdmi_in;
    } input;
} preview_ctx_t;

static void preview_print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s <usb|csi0|csi1|hdmi_in> <device> <format> <width> <height> <fps>\n"
            "Example:\n"
            "  %s usb /dev/video41 mjpeg 1280 720 30\n"
            "  %s csi0 /dev/video22 uyvy422 3840 2160 30\n"
            "  %s csi1 /dev/video31 uyvy422 1632 1224 30\n"
            "  %s hdmi_in /dev/video40 uyvy422 1920 1080 30\n",
            prog, prog, prog, prog, prog);
}

static app_status_t preview_open(preview_ctx_t *ctx) {
    switch (ctx->source_type) {
        case VIDEO_SOURCE_USB:
            return input_usb_open(&ctx->input.usb, &ctx->cfg);
        case VIDEO_SOURCE_CSI0:
            return input_csi0_open(&ctx->input.csi0, &ctx->cfg);
        case VIDEO_SOURCE_CSI1:
            return input_csi1_open(&ctx->input.csi1, &ctx->cfg);
        case VIDEO_SOURCE_HDMI_IN:
            return input_hdmi_in_open(&ctx->input.hdmi_in, &ctx->cfg);
        default:
            return APP_ERR_PARAM;
    }
}

static app_status_t preview_read(preview_ctx_t *ctx, video_frame_t *frame) {
    switch (ctx->source_type) {
        case VIDEO_SOURCE_USB:
            return input_usb_read(&ctx->input.usb, frame);
        case VIDEO_SOURCE_CSI0:
            return input_csi0_read(&ctx->input.csi0, frame);
        case VIDEO_SOURCE_CSI1:
            return input_csi1_read(&ctx->input.csi1, frame);
        case VIDEO_SOURCE_HDMI_IN:
            return input_hdmi_in_read(&ctx->input.hdmi_in, frame);
        default:
            return APP_ERR_PARAM;
    }
}

static void preview_close(preview_ctx_t *ctx) {
    switch (ctx->source_type) {
        case VIDEO_SOURCE_USB:
            input_usb_close(&ctx->input.usb);
            break;
        case VIDEO_SOURCE_CSI0:
            input_csi0_close(&ctx->input.csi0);
            break;
        case VIDEO_SOURCE_CSI1:
            input_csi1_close(&ctx->input.csi1);
            break;
        case VIDEO_SOURCE_HDMI_IN:
            input_hdmi_in_close(&ctx->input.hdmi_in);
            break;
        default:
            break;
    }
}

static app_status_t preview_parse_args(preview_ctx_t *ctx, int argc, char **argv) {
    if (argc != 7) {
        preview_print_usage(argv[0]);
        return APP_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));
    if (strcmp(argv[1], "usb") == 0) {
        ctx->source_type = VIDEO_SOURCE_USB;
        strncpy(ctx->cfg.name, "usb0", sizeof(ctx->cfg.name) - 1);
    } else if (strcmp(argv[1], "csi0") == 0) {
        ctx->source_type = VIDEO_SOURCE_CSI0;
        strncpy(ctx->cfg.name, "csi0", sizeof(ctx->cfg.name) - 1);
    } else if (strcmp(argv[1], "csi1") == 0) {
        ctx->source_type = VIDEO_SOURCE_CSI1;
        strncpy(ctx->cfg.name, "csi1", sizeof(ctx->cfg.name) - 1);
    } else if (strcmp(argv[1], "hdmi_in") == 0) {
        ctx->source_type = VIDEO_SOURCE_HDMI_IN;
        strncpy(ctx->cfg.name, "hdmi_in0", sizeof(ctx->cfg.name) - 1);
    } else {
        preview_print_usage(argv[0]);
        return APP_ERR_PARAM;
    }

    strncpy(ctx->cfg.device, argv[2], sizeof(ctx->cfg.device) - 1);
    strncpy(ctx->cfg.input_format, argv[3], sizeof(ctx->cfg.input_format) - 1);
    ctx->cfg.width = atoi(argv[4]);
    ctx->cfg.height = atoi(argv[5]);
    ctx->cfg.fps = atoi(argv[6]);
    ctx->cfg.source_type = ctx->source_type;

    return APP_OK;
}

int main(int argc, char **argv) {
    preview_ctx_t ctx;
    video_frame_t frame;
    app_status_t status;
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    struct SwsContext *sws = NULL;
    uint8_t *rgba_buf = NULL;
    int rgba_size = 0;
    int rgba_linesize[4] = {0};
    uint8_t *rgba_planes[4] = {0};
    int running = 1;
    int frame_width = 0;
    int frame_height = 0;
    enum AVPixelFormat frame_fmt = AV_PIX_FMT_NONE;

    status = preview_parse_args(&ctx, argc, argv);
    if (status != APP_OK) {
        return EXIT_FAILURE;
    }

#ifdef HAVE_LIBAVDEVICE
    extern void avdevice_register_all(void);
    avdevice_register_all();
#endif
    avformat_network_init();

    status = preview_open(&ctx);
    if (status != APP_OK) {
        LOGE("preview_open failed: %s", app_status_str(status));
        avformat_network_deinit();
        return EXIT_FAILURE;
    }

    if (getenv("DISPLAY") == NULL) {
        LOGW("DISPLAY is not set. Run this preview from the board desktop session.");
    }
    if (getenv("XDG_RUNTIME_DIR") == NULL) {
        LOGW("XDG_RUNTIME_DIR is not set. Avoid sudo for GUI preview.");
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOGE("SDL_Init failed: %s", SDL_GetError());
        preview_close(&ctx);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }

    window = SDL_CreateWindow(ctx.cfg.name,
                              SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED,
                              PREVIEW_WINDOW_W,
                              PREVIEW_WINDOW_H,
                              SDL_WINDOW_RESIZABLE);
    if (window == NULL) {
        LOGE("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        preview_close(&ctx);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == NULL) {
        LOGW("SDL accelerated renderer failed: %s", SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (renderer == NULL) {
            LOGE("SDL_CreateRenderer failed: %s", SDL_GetError());
            SDL_DestroyWindow(window);
            SDL_Quit();
            preview_close(&ctx);
            avformat_network_deinit();
            return EXIT_FAILURE;
        }
    }

    memset(&frame, 0, sizeof(frame));
    frame.av_frame = av_frame_alloc();
    if (frame.av_frame == NULL) {
        LOGE("av_frame_alloc failed");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        preview_close(&ctx);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }

    while (running) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
            }
        }

        av_frame_unref(frame.av_frame);
        status = preview_read(&ctx, &frame);
        if (status != APP_OK) {
            LOGW("preview_read failed: %s", app_status_str(status));
            SDL_Delay(20);
            continue;
        }

        if (frame_width != frame.av_frame->width ||
            frame_height != frame.av_frame->height ||
            frame_fmt != (enum AVPixelFormat)frame.av_frame->format) {
            sws_freeContext(sws);
            sws = sws_getCachedContext(NULL,
                                       frame.av_frame->width,
                                       frame.av_frame->height,
                                       (enum AVPixelFormat)frame.av_frame->format,
                                       frame.av_frame->width,
                                       frame.av_frame->height,
                                       AV_PIX_FMT_BGRA,
                                       SWS_BILINEAR,
                                       NULL,
                                       NULL,
                                       NULL);
            if (sws == NULL) {
                LOGE("sws_getCachedContext failed");
                break;
            }

            free(rgba_buf);
            rgba_size = av_image_get_buffer_size(AV_PIX_FMT_BGRA,
                                                 frame.av_frame->width,
                                                 frame.av_frame->height,
                                                 1);
            if (rgba_size <= 0) {
                LOGE("av_image_get_buffer_size failed");
                break;
            }

            rgba_buf = malloc((size_t)rgba_size);
            if (rgba_buf == NULL) {
                LOGE("malloc rgba buffer failed");
                break;
            }

            if (av_image_fill_arrays(rgba_planes,
                                     rgba_linesize,
                                     rgba_buf,
                                     AV_PIX_FMT_BGRA,
                                     frame.av_frame->width,
                                     frame.av_frame->height,
                                     1) < 0) {
                LOGE("av_image_fill_arrays failed");
                break;
            }

            if (texture != NULL) {
                SDL_DestroyTexture(texture);
            }
            texture = SDL_CreateTexture(renderer,
                                        SDL_PIXELFORMAT_BGRA32,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        frame.av_frame->width,
                                        frame.av_frame->height);
            if (texture == NULL) {
                LOGE("SDL_CreateTexture failed: %s", SDL_GetError());
                break;
            }

            frame_width = frame.av_frame->width;
            frame_height = frame.av_frame->height;
            frame_fmt = (enum AVPixelFormat)frame.av_frame->format;
        }

        sws_scale(sws,
                  (const uint8_t * const *)frame.av_frame->data,
                  frame.av_frame->linesize,
                  0,
                  frame.av_frame->height,
                  rgba_planes,
                  rgba_linesize);

        if (SDL_UpdateTexture(texture, NULL, rgba_buf, rgba_linesize[0]) != 0) {
            LOGE("SDL_UpdateTexture failed: %s", SDL_GetError());
            break;
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }

    av_frame_free(&frame.av_frame);
    if (texture != NULL) {
        SDL_DestroyTexture(texture);
    }
    sws_freeContext(sws);
    free(rgba_buf);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    preview_close(&ctx);
    avformat_network_deinit();
    return EXIT_SUCCESS;
}
