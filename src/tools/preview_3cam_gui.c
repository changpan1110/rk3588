#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "preview_3cam_gui.c"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "common/common.h"
#include "common/debug.h"
#include "input/input_csi0.h"
#include "input/input_csi1.h"
#include "input/input_hdmi_in.h"
#include "input/input_usb.h"

#define PREVIEW_WINDOW_W 1600
#define PREVIEW_WINDOW_H 900
#define PREVIEW_FPS 30

typedef struct {
    const char *name;
    video_input_config_t cfg;
    video_source_type_t source_type;
    union {
        input_usb_ctx_t usb;
        input_csi0_ctx_t csi0;
        input_csi1_ctx_t csi1;
        input_hdmi_in_ctx_t hdmi_in;
    } input;
    int opened;
    int stop;
    int read_failures;
    pthread_t thread;
    pthread_mutex_t lock;
    struct SwsContext *sws;
    uint8_t *rgba_data;
    int rgba_linesize;
    int rgba_buf_size;
    int frame_width;
    int frame_height;
    enum AVPixelFormat frame_fmt;
    int frame_ready;
} preview_source_t;

typedef struct {
    preview_source_t source;
    SDL_Texture *texture;
} preview_slot_t;

static app_status_t preview_source_open(preview_source_t *src) {
    switch (src->source_type) {
        case VIDEO_SOURCE_USB:
            return input_usb_open(&src->input.usb, &src->cfg);
        case VIDEO_SOURCE_CSI0:
            return input_csi0_open(&src->input.csi0, &src->cfg);
        case VIDEO_SOURCE_CSI1:
            return input_csi1_open(&src->input.csi1, &src->cfg);
        case VIDEO_SOURCE_HDMI_IN:
            return input_hdmi_in_open(&src->input.hdmi_in, &src->cfg);
        default:
            return APP_ERR_PARAM;
    }
}

static app_status_t preview_source_read(preview_source_t *src, video_frame_t *frame) {
    switch (src->source_type) {
        case VIDEO_SOURCE_USB:
            return input_usb_read(&src->input.usb, frame);
        case VIDEO_SOURCE_CSI0:
            return input_csi0_read(&src->input.csi0, frame);
        case VIDEO_SOURCE_CSI1:
            return input_csi1_read(&src->input.csi1, frame);
        case VIDEO_SOURCE_HDMI_IN:
            return input_hdmi_in_read(&src->input.hdmi_in, frame);
        default:
            return APP_ERR_PARAM;
    }
}

static void preview_source_close(preview_source_t *src) {
    switch (src->source_type) {
        case VIDEO_SOURCE_USB:
            input_usb_close(&src->input.usb);
            break;
        case VIDEO_SOURCE_CSI0:
            input_csi0_close(&src->input.csi0);
            break;
        case VIDEO_SOURCE_CSI1:
            input_csi1_close(&src->input.csi1);
            break;
        case VIDEO_SOURCE_HDMI_IN:
            input_hdmi_in_close(&src->input.hdmi_in);
            break;
        default:
            break;
    }
}

static int preview_source_prepare_convert(preview_source_t *src, const AVFrame *frame) {
    enum AVPixelFormat src_fmt = (enum AVPixelFormat)frame->format;
    struct SwsContext *sws;
    int rgba_linesize[4] = {0};
    int rgba_buf_size;
    uint8_t *rgba_data;
    uint8_t *rgba_planes[4] = {0};

    if (src->rgba_data != NULL &&
        src->frame_width == frame->width &&
        src->frame_height == frame->height &&
        src->frame_fmt == src_fmt) {
        return 0;
    }

    sws = sws_getCachedContext(src->sws,
                               frame->width,
                               frame->height,
                               src_fmt,
                               frame->width,
                               frame->height,
                               AV_PIX_FMT_BGRA,
                               SWS_BILINEAR,
                               NULL,
                               NULL,
                               NULL);
    if (sws == NULL) {
        LOGE("%s sws_getCachedContext failed", src->name);
        return -1;
    }

    rgba_buf_size = av_image_get_buffer_size(AV_PIX_FMT_BGRA, frame->width, frame->height, 1);
    if (rgba_buf_size <= 0) {
        LOGE("%s av_image_get_buffer_size failed", src->name);
        return -1;
    }

    rgba_data = realloc(src->rgba_data, (size_t)rgba_buf_size);
    if (rgba_data == NULL) {
        LOGE("%s realloc rgba buffer failed", src->name);
        return -1;
    }

    if (av_image_fill_arrays(rgba_planes,
                             rgba_linesize,
                             rgba_data,
                             AV_PIX_FMT_BGRA,
                             frame->width,
                             frame->height,
                             1) < 0) {
        LOGE("%s av_image_fill_arrays failed", src->name);
        return -1;
    }

    src->sws = sws;
    src->rgba_data = rgba_data;
    src->rgba_linesize = rgba_linesize[0];
    src->rgba_buf_size = rgba_buf_size;
    src->frame_width = frame->width;
    src->frame_height = frame->height;
    src->frame_fmt = src_fmt;
    return 0;
}

static void *preview_source_thread(void *opaque) {
    preview_source_t *src = (preview_source_t *)opaque;
    video_frame_t frame;
    app_status_t status;
    uint8_t *dst_planes[4] = {0};
    int dst_linesize[4] = {0};

    memset(&frame, 0, sizeof(frame));
    frame.av_frame = av_frame_alloc();
    if (frame.av_frame == NULL) {
        LOGE("%s av_frame_alloc failed", src->name);
        return NULL;
    }

    while (!src->stop) {
        av_frame_unref(frame.av_frame);
        status = preview_source_read(src, &frame);
        if (status != APP_OK) {
            src->read_failures++;
            if (src->read_failures % 100 == 1) {
                LOGW("%s read failed: %s", src->name, app_status_str(status));
            }
            usleep(10000);
            continue;
        }
        src->read_failures = 0;

        pthread_mutex_lock(&src->lock);
        if (preview_source_prepare_convert(src, frame.av_frame) == 0) {
            dst_planes[0] = src->rgba_data;
            dst_linesize[0] = src->rgba_linesize;

            sws_scale(src->sws,
                      (const uint8_t * const *)frame.av_frame->data,
                      frame.av_frame->linesize,
                      0,
                      frame.av_frame->height,
                      dst_planes,
                      dst_linesize);
            src->frame_ready = 1;
        }
        pthread_mutex_unlock(&src->lock);
    }

    av_frame_free(&frame.av_frame);
    return NULL;
}

static void preview_source_deinit(preview_source_t *src) {
    if (src == NULL) {
        return;
    }

    src->stop = 1;
    if (src->opened) {
        pthread_join(src->thread, NULL);
        preview_source_close(src);
        src->opened = 0;
    }
    sws_freeContext(src->sws);
    free(src->rgba_data);
    pthread_mutex_destroy(&src->lock);
    memset(src, 0, sizeof(*src));
}

static void preview_source_init(preview_source_t *src,
                                const char *name,
                                const char *device,
                                const char *input_format,
                                int width,
                                int height,
                                int fps,
                                video_source_type_t source_type) {
    memset(src, 0, sizeof(*src));
    src->name = name;
    src->source_type = source_type;
    strncpy(src->cfg.name, name, sizeof(src->cfg.name) - 1);
    strncpy(src->cfg.device, device, sizeof(src->cfg.device) - 1);
    strncpy(src->cfg.input_format, input_format, sizeof(src->cfg.input_format) - 1);
    src->cfg.width = width;
    src->cfg.height = height;
    src->cfg.fps = fps;
    src->cfg.source_type = source_type;
    pthread_mutex_init(&src->lock, NULL);
}

static void preview_source_apply_cli(preview_source_t *src, int argc, char **argv) {
    int i;
    char option[32];

    snprintf(option, sizeof(option), "--%s-dev", src->name);
    for (i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], option) == 0) {
            strncpy(src->cfg.device, argv[i + 1], sizeof(src->cfg.device) - 1);
            src->cfg.device[sizeof(src->cfg.device) - 1] = '\0';
        }
    }

    snprintf(option, sizeof(option), "--%s-fmt", src->name);
    for (i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], option) == 0) {
            strncpy(src->cfg.input_format, argv[i + 1], sizeof(src->cfg.input_format) - 1);
            src->cfg.input_format[sizeof(src->cfg.input_format) - 1] = '\0';
        }
    }
}

static int preview_source_start(preview_source_t *src) {
    app_status_t status = preview_source_open(src);
    if (status != APP_OK) {
        LOGW("%s open failed: %s", src->name, app_status_str(status));
        return -1;
    }

    src->opened = 1;
    if (pthread_create(&src->thread, NULL, preview_source_thread, src) != 0) {
        LOGE("%s pthread_create failed", src->name);
        preview_source_close(src);
        src->opened = 0;
        return -1;
    }
    return 0;
}

static SDL_Texture *ensure_texture(SDL_Renderer *renderer, preview_slot_t *slot) {
    SDL_Texture *texture;
    preview_source_t *src = &slot->source;

    if (!src->frame_ready) {
        return slot->texture;
    }
    if (slot->texture != NULL) {
        return slot->texture;
    }

    texture = SDL_CreateTexture(renderer,
                                SDL_PIXELFORMAT_BGRA32,
                                SDL_TEXTUREACCESS_STREAMING,
                                src->frame_width,
                                src->frame_height);
    if (texture == NULL) {
        LOGE("%s SDL_CreateTexture failed: %s", src->name, SDL_GetError());
        return NULL;
    }

    slot->texture = texture;
    return slot->texture;
}

static void render_source(SDL_Renderer *renderer, preview_slot_t *slot, const SDL_Rect *dst) {
    preview_source_t *src = &slot->source;
    SDL_Texture *texture = ensure_texture(renderer, slot);

    if (!src->frame_ready || texture == NULL) {
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderFillRect(renderer, dst);
        return;
    }

    pthread_mutex_lock(&src->lock);
    if (src->rgba_data != NULL &&
        SDL_UpdateTexture(texture, NULL, src->rgba_data, src->rgba_linesize) == 0) {
        SDL_RenderCopy(renderer, texture, NULL, dst);
    }
    pthread_mutex_unlock(&src->lock);
}

int main(int argc, char **argv) {
    preview_slot_t slots[3];
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Rect rects[3];
    int running = 1;
    int i;

#ifdef HAVE_LIBAVDEVICE
    extern void avdevice_register_all(void);
    avdevice_register_all();
#endif
    avformat_network_init();

    memset(slots, 0, sizeof(slots));
    preview_source_init(&slots[0].source, "usb0", "/dev/video41", "mjpeg", 1280, 720, 30, VIDEO_SOURCE_USB);
    preview_source_init(&slots[1].source, "usb1", "/dev/video43", "mjpeg", 640, 480, 30, VIDEO_SOURCE_USB);
    preview_source_init(&slots[2].source, "hdmi_in", "/dev/video40", "", 0, 0, 0, VIDEO_SOURCE_HDMI_IN);

    for (i = 0; i < 3; ++i) {
        preview_source_apply_cli(&slots[i].source, argc, argv);
    }

    if (getenv("DISPLAY") == NULL) {
        LOGW("DISPLAY is not set. Run this GUI test from the board desktop session.");
    }
    if (getenv("XDG_RUNTIME_DIR") == NULL) {
        LOGW("XDG_RUNTIME_DIR is not set. Avoid sudo for GUI preview.");
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOGE("SDL_Init failed: %s", SDL_GetError());
        return EXIT_FAILURE;
    }

    window = SDL_CreateWindow("RK3588 3-Camera Preview",
                              SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED,
                              PREVIEW_WINDOW_W,
                              PREVIEW_WINDOW_H,
                              0);
    if (window == NULL) {
        LOGE("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
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
            return EXIT_FAILURE;
        }
    }

    rects[0] = (SDL_Rect){0, 0, PREVIEW_WINDOW_W / 2, PREVIEW_WINDOW_H / 2};
    rects[1] = (SDL_Rect){PREVIEW_WINDOW_W / 2, 0, PREVIEW_WINDOW_W / 2, PREVIEW_WINDOW_H / 2};
    rects[2] = (SDL_Rect){PREVIEW_WINDOW_W / 4, PREVIEW_WINDOW_H / 2, PREVIEW_WINDOW_W / 2, PREVIEW_WINDOW_H / 2};

    for (i = 0; i < 3; ++i) {
        preview_source_start(&slots[i].source);
    }

    while (running) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        for (i = 0; i < 3; ++i) {
            render_source(renderer, &slots[i], &rects[i]);
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(1000 / PREVIEW_FPS);
    }

    for (i = 0; i < 3; ++i) {
        if (slots[i].texture != NULL) {
            SDL_DestroyTexture(slots[i].texture);
        }
        preview_source_deinit(&slots[i].source);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    avformat_network_deinit();
    return EXIT_SUCCESS;
}
