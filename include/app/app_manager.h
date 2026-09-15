#ifndef APP_MANAGER_H
#define APP_MANAGER_H

#include "input/video/input_usb.h"
#include "input/video/input_csi.h"
#include "input/video/input_hdmi_in.h"
#include "process/video/process_common.h"
#include "output/display/display_gui.h"

typedef struct {
    video_input_config_t usb;
    video_input_config_t csi0;
    video_input_config_t csi1;
    video_input_config_t hdmi_in;
} video_input_group_t;

typedef struct {
    video_input_group_t input;
} video_app_config_t;

typedef struct {
    video_app_config_t config;
    input_usb_ctx_t usb;
    input_csi_ctx_t csi0;
    input_csi_ctx_t csi1;
    input_hdmi_in_ctx_t hdmi_in;
    process_common_ctx_t process;
    display_gui_ctx_t gui_display;
    int running;
    int loop_delay_ms;
} video_app_ctx_t;

app_status_t video_app_init(video_app_ctx_t *ctx, const video_app_config_t *cfg);
app_status_t video_app_run_once(video_app_ctx_t *ctx);
int video_app_is_running(const video_app_ctx_t *ctx);
int video_app_get_loop_delay_ms(const video_app_ctx_t *ctx);
void video_app_request_stop(video_app_ctx_t *ctx);
void video_app_deinit(video_app_ctx_t *ctx);

#endif
