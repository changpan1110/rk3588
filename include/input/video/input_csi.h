#ifndef INPUT_CSI_H
#define INPUT_CSI_H

#include "common/common.h"
#include "input/video/input_common.h"

typedef struct input_csi_native_ctx input_csi_native_ctx_t;

typedef struct {
    video_input_config_t cfg;
    ffmpeg_input_ctx_t decoder;
    input_csi_native_ctx_t *native;
    int use_native;
    int is_opened;
} input_csi_ctx_t;

/*
 * FFmpeg v4l2 软件帧路径(兼容旧的 input_csi0/1_open 行为)。
 */
app_status_t input_csi_open(input_csi_ctx_t *ctx, const video_input_config_t *cfg);

/*
 * 原生 V4L2 DMA-BUF/DRM PRIME 零拷贝路径。
 * 成功时输出 AV_PIX_FMT_DRM_PRIME 硬件帧;失败时回退到 FFmpeg 路径。
 */
app_status_t input_csi_open_hw(input_csi_ctx_t *ctx, const video_input_config_t *cfg);

app_status_t input_csi_read(input_csi_ctx_t *ctx, video_frame_t *frame);
void input_csi_close(input_csi_ctx_t *ctx);

#endif
