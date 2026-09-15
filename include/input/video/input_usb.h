#ifndef INPUT_USB_H
#define INPUT_USB_H

#include "common/common.h"
#include "input/video/input_common.h"

typedef struct {
    video_input_config_t cfg;
    ffmpeg_input_ctx_t decoder;
    AVFrame *sw_frame;
    uint64_t hw_frame_count;
    uint64_t hw_transfer_count;
    uint64_t decode_timing_frames;
    int64_t decode_timing_sum_us;
    int64_t decode_timing_max_us;
    int retain_hw_frames;
    int is_opened;
} input_usb_ctx_t;

app_status_t input_usb_open(input_usb_ctx_t *ctx, const video_input_config_t *cfg);
app_status_t input_usb_open_hw(input_usb_ctx_t *ctx, const video_input_config_t *cfg);
app_status_t input_usb_read(input_usb_ctx_t *ctx, video_frame_t *frame);
void input_usb_close(input_usb_ctx_t *ctx);

#endif
