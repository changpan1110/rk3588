#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "hdmi_color_compare.c"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <libavutil/pixdesc.h>

#include "common/common.h"
#include "common/debug.h"
#include "input/input_hdmi_in.h"
#include "output/output_encode_common.h"

#define COMPARE_WINDOW_WIDTH 1600
#define COMPARE_WINDOW_HEIGHT 500

static volatile sig_atomic_t g_stop_requested = 0;

static void compare_handle_signal(int signo) {
    (void)signo;
    g_stop_requested = 1;
}

static SDL_Rect compare_fit_rect(int pane_x,
                                 int pane_width,
                                 int pane_height,
                                 int frame_width,
                                 int frame_height) {
    SDL_Rect rect;
    int height = (int)((int64_t)pane_width * frame_height / frame_width);
    int width = pane_width;

    if (height > pane_height) {
        height = pane_height;
        width = (int)((int64_t)height * frame_width / frame_height);
    }
    rect.x = pane_x + (pane_width - width) / 2;
    rect.y = (pane_height - height) / 2;
    rect.w = width;
    rect.h = height;
    return rect;
}

int main(void) {
    video_input_config_t input_cfg = {
        .name = "hdmi_in0",
        .device = "/dev/video40",
        .input_format = "",
        .width = 0,
        .height = 0,
        .fps = 30,
        .source_type = VIDEO_SOURCE_HDMI_IN
    };
    input_hdmi_in_ctx_t input;
    output_encode_convert_ctx_t convert;
    video_frame_t raw_frame;
    const AVFrame *resize_frame;
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *raw_texture = NULL;
    SDL_Texture *scaled_texture = NULL;
    SDL_Event event;
    SDL_Rect raw_rect;
    SDL_Rect scaled_rect;
    app_status_t status;
    int running = 1;
    int window_width;
    int window_height;
    int initialized_input = 0;
    int initialized_sdl = 0;
    int frame_info_logged = 0;
    int fps_frames = 0;
    int64_t fps_start_us = app_get_time_us();
    int exit_code = EXIT_FAILURE;

    memset(&input, 0, sizeof(input));
    memset(&convert, 0, sizeof(convert));
    memset(&raw_frame, 0, sizeof(raw_frame));

    signal(SIGINT, compare_handle_signal);
    signal(SIGTERM, compare_handle_signal);

#ifdef HAVE_LIBAVDEVICE
    {
        extern void avdevice_register_all(void);
        avdevice_register_all();
    }
#endif
    avformat_network_init();

    if (getenv("DISPLAY") == NULL) {
        LOGW("DISPLAY is not set. Run this tool from the RK3588 desktop session.");
    }
    if (getenv("XDG_RUNTIME_DIR") == NULL) {
        LOGW("XDG_RUNTIME_DIR is not set. Avoid sudo when starting the GUI.");
    }

    status = input_hdmi_in_open(&input, &input_cfg);
    if (status != APP_OK) {
        LOGE("open HDMI input failed: %s", app_status_str(status));
        goto cleanup;
    }
    initialized_input = 1;

    status = output_encode_convert_init(&convert, 1920, 1080, AV_PIX_FMT_BGR24);
    if (status != APP_OK) {
        LOGE("init RGA compare conversion failed: %s", app_status_str(status));
        goto cleanup;
    }

    raw_frame.av_frame = av_frame_alloc();
    if (raw_frame.av_frame == NULL) {
        LOGE("allocate HDMI frame failed");
        goto cleanup;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOGE("SDL_Init failed: %s", SDL_GetError());
        goto cleanup;
    }
    initialized_sdl = 1;

    window = SDL_CreateWindow("Left: live HDMI raw-color preview | Right: live RGA-scaled 1080P",
                              SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED,
                              COMPARE_WINDOW_WIDTH,
                              COMPARE_WINDOW_HEIGHT,
                              SDL_WINDOW_RESIZABLE);
    if (window == NULL) {
        LOGE("SDL_CreateWindow failed: %s", SDL_GetError());
        goto cleanup;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (renderer == NULL) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (renderer == NULL) {
        LOGE("SDL_CreateRenderer failed: %s", SDL_GetError());
        goto cleanup;
    }

    exit_code = EXIT_SUCCESS;
    LOGI("HDMI color compare started: left=raw BGR24, right=RGA-scaled BGR24");
    while (running && !g_stop_requested) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running = 0;
                }
            }
        }
        if (!running) {
            break;
        }

        av_frame_unref(raw_frame.av_frame);
        status = input_hdmi_in_read(&input, &raw_frame);
        if (status != APP_OK) {
            if (status != APP_ERR_EOF) {
                LOGW("read HDMI frame failed: %s", app_status_str(status));
            }
            SDL_Delay(10);
            continue;
        }

        resize_frame = output_encode_prepare_rga_bgr_resize(&convert, raw_frame.av_frame);
        if (resize_frame == NULL) {
            LOGE("RGA resize HDMI frame failed");
            exit_code = EXIT_FAILURE;
            break;
        }
        if (raw_frame.av_frame->format != AV_PIX_FMT_BGR24 ||
            resize_frame->format != AV_PIX_FMT_BGR24) {
            LOGE("direct display requires BGR24: raw=%d resize=%d",
                 raw_frame.av_frame->format,
                 resize_frame->format);
            exit_code = EXIT_FAILURE;
            break;
        }

        if (raw_texture == NULL) {
            raw_texture = SDL_CreateTexture(renderer,
                                            SDL_PIXELFORMAT_BGR24,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            raw_frame.av_frame->width,
                                            raw_frame.av_frame->height);
            scaled_texture = SDL_CreateTexture(renderer,
                                               SDL_PIXELFORMAT_BGR24,
                                               SDL_TEXTUREACCESS_STREAMING,
                                               resize_frame->width,
                                               resize_frame->height);
            if (raw_texture == NULL || scaled_texture == NULL) {
                LOGE("SDL_CreateTexture failed: %s", SDL_GetError());
                exit_code = EXIT_FAILURE;
                break;
            }
        }

        if (!frame_info_logged) {
            LOGI("compare frames: raw=%dx%d fmt=%s; resize=%dx%d fmt=%s",
                 raw_frame.av_frame->width,
                 raw_frame.av_frame->height,
                 av_get_pix_fmt_name((enum AVPixelFormat)raw_frame.av_frame->format),
                 resize_frame->width,
                 resize_frame->height,
                 av_get_pix_fmt_name((enum AVPixelFormat)resize_frame->format));
            frame_info_logged = 1;
        }

        if (SDL_UpdateTexture(raw_texture,
                              NULL,
                              raw_frame.av_frame->data[0],
                              raw_frame.av_frame->linesize[0]) != 0 ||
            SDL_UpdateTexture(scaled_texture,
                              NULL,
                              resize_frame->data[0],
                              resize_frame->linesize[0]) != 0) {
            LOGE("SDL_UpdateTexture failed: %s", SDL_GetError());
            exit_code = EXIT_FAILURE;
            break;
        }

        SDL_GetWindowSize(window, &window_width, &window_height);
        raw_rect = compare_fit_rect(0,
                                    window_width / 2,
                                    window_height,
                                    raw_frame.av_frame->width,
                                    raw_frame.av_frame->height);
        scaled_rect = compare_fit_rect(window_width / 2,
                                       window_width - window_width / 2,
                                       window_height,
                                       resize_frame->width,
                                       resize_frame->height);

        SDL_SetRenderDrawColor(renderer, 16, 16, 16, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, raw_texture, NULL, &raw_rect);
        SDL_RenderCopy(renderer, scaled_texture, NULL, &scaled_rect);
        SDL_SetRenderDrawColor(renderer, 220, 220, 220, 255);
        SDL_RenderDrawLine(renderer, window_width / 2, 0, window_width / 2, window_height);
        SDL_RenderPresent(renderer);

        fps_frames++;
        if (app_get_time_us() - fps_start_us >= 1000000) {
            int64_t elapsed_us = app_get_time_us() - fps_start_us;
            LOGI("compare display fps=%d",
                 (int)((int64_t)fps_frames * 1000000 / elapsed_us));
            fps_frames = 0;
            fps_start_us = app_get_time_us();
        }
    }

cleanup:
    if (raw_texture != NULL) {
        SDL_DestroyTexture(raw_texture);
    }
    if (scaled_texture != NULL) {
        SDL_DestroyTexture(scaled_texture);
    }
    if (renderer != NULL) {
        SDL_DestroyRenderer(renderer);
    }
    if (window != NULL) {
        SDL_DestroyWindow(window);
    }
    if (initialized_sdl) {
        SDL_Quit();
    }
    av_frame_free(&raw_frame.av_frame);
    output_encode_convert_deinit(&convert);
    if (initialized_input) {
        input_hdmi_in_close(&input);
    }
    avformat_network_deinit();
    return exit_code;
}
