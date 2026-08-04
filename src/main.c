#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "main.c"

#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#include "common/common.h"
#include "common/debug.h"
#include "app/app_manager.h"

#ifdef HAVE_LIBAVDEVICE
extern void avdevice_register_all(void);
#endif

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_stop_signal(int signo) {
    (void)signo;
    g_stop_requested = 1;
}

static video_app_config_t build_video_app_config(void) {
    video_app_config_t cfg = {
        .input = {
            .usb = {
                .name = "usb0",
                .device = "/dev/video41",
                .input_format = "mjpeg",
                .width = 1280,
                .height = 720,
                .fps = 10,
                .source_type = VIDEO_SOURCE_USB
            },
            .csi0 = {
                .name = "csi0",
                .device = "/dev/video22",
                .input_format = "nv12",
                .width = 3840,
                .height = 2160,
                .fps = 30,
                .source_type = VIDEO_SOURCE_CSI0
            },
            .csi1 = {
                .name = "csi1",
                .device = "/dev/video31",
                .input_format = "uyvy422",
                .width = 1632,
                .height = 1224,
                .fps = 30,
                .source_type = VIDEO_SOURCE_CSI1
            },
            .hdmi_in = {
                .name = "hdmi_in0",
                .device = "/dev/video40",
                .input_format = "bgr3",
                .width = 3840,
                .height = 2160,
                .fps = 30,
                .source_type = VIDEO_SOURCE_HDMI_IN
            }
        }
    };

    return cfg;
}

int main(void) {
    video_app_ctx_t app;
    video_app_config_t config;
    app_status_t status;

    signal(SIGINT, handle_stop_signal);
    signal(SIGTERM, handle_stop_signal);

#ifdef HAVE_LIBAVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    config = build_video_app_config();

    status = video_app_init(&app, &config);
    if (status != APP_OK) {
        LOGE("video_app_init failed: %s", app_status_str(status));
        return EXIT_FAILURE;
    }

    while (video_app_is_running(&app)) {
        if (g_stop_requested) {
            video_app_request_stop(&app);
            break;
        }
        status = video_app_run_once(&app);
        if (status != APP_OK && status != APP_ERR_EOF) {
            LOGW("video_app_run_once returned: %s", app_status_str(status));
            break;
        }
        usleep((useconds_t)video_app_get_loop_delay_ms(&app) * 1000U);
    }

    video_app_deinit(&app);
    avformat_network_deinit();
    return EXIT_SUCCESS;
}
