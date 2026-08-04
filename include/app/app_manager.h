#ifndef APP_MANAGER_H
#define APP_MANAGER_H

#include "input/input_usb.h"
#include "input/input_csi0.h"
#include "input/input_csi1.h"
#include "input/input_hdmi_in.h"
#include "process/process_common.h"
#include "output/output_encode_main.h"
#include "output/output_encode_sub.h"
#include "output/output_display_gui.h"
#include "output/output_display_hdmi.h"
#include "output/output_record.h"
#include "output/output_snapshot.h"

typedef struct {
    encoder_mode_t encoder_mode;
    camera_output_config_t output;
} video_output_config_t;

typedef struct {
    video_input_config_t usb;
    video_input_config_t csi0;
    video_input_config_t csi1;
    video_input_config_t hdmi_in;
} video_input_group_t;

typedef struct {
    video_input_group_t input;
    video_output_config_t output;
} video_app_config_t;

typedef struct {
    video_app_config_t config;
    input_usb_ctx_t usb;
    input_csi0_ctx_t csi0;
    input_csi1_ctx_t csi1;
    input_hdmi_in_ctx_t hdmi_in;
    process_common_ctx_t process;
    video_output_config_t output;
    output_encode_main_ctx_t main_encoder;
    output_encode_sub_ctx_t sub_encoder;
    output_display_gui_ctx_t gui_display;
    output_display_hdmi_ctx_t hdmi_display;
    output_record_ctx_t recorder;
    output_snapshot_ctx_t snapshot;
    int running;
    int loop_delay_ms;
} video_app_ctx_t;

void video_output_prepare_default(video_output_config_t *cfg);
app_status_t video_app_init(video_app_ctx_t *ctx, const video_app_config_t *cfg);
app_status_t video_app_run_once(video_app_ctx_t *ctx);
int video_app_is_running(const video_app_ctx_t *ctx);
int video_app_get_loop_delay_ms(const video_app_ctx_t *ctx);
void video_app_request_stop(video_app_ctx_t *ctx);
app_status_t video_app_start_record(video_app_ctx_t *ctx, const char *path);
app_status_t video_app_stop_record(video_app_ctx_t *ctx);
app_status_t video_app_take_snapshot(video_app_ctx_t *ctx, const char *path, const video_frame_t *frame);
void video_app_deinit(video_app_ctx_t *ctx);

#endif
