#ifndef VIDEO_FRAME_CONVERT_H
#define VIDEO_FRAME_CONVERT_H

#include <libswscale/swscale.h>

#include "common/common.h"
#include "process/video/video_rkrga_filter.h"

typedef struct {
    int dst_width;
    int dst_height;
    enum AVPixelFormat dst_fmt;
    int output_is_hw;
    enum AVPixelFormat src_fmt;
    int src_width;
    int src_height;
    int rkrga_src_width;
    int rkrga_src_height;
    enum AVPixelFormat rkrga_src_fmt;
    struct SwsContext *sws;
    AVFrame *work_frame;
    AVFrame *rga_resize_frame;
    AVFrame *hw_transfer_frame;
    video_rkrga_filter_t *rkrga_filter;
    AVBufferRef *rkrga_src_hw_frames_ctx;
    uint32_t rga_resize_handle;
    /* RGA runtime 失败后置 1,该上下文永久回落 swscale */
    int rga_failed;
    int rkrga_filter_failed;
    int rkrga_filter_path_logged;
    int rga_color_failed;
    int rga_resize_valid;
    int rga_error_count;
    int rga_partial_fallback_logged;
    int rga_path_logged;
    int sws_path_logged;
    int hw_transfer_path_logged;
} video_frame_convert_ctx_t;

app_status_t video_frame_convert_init(video_frame_convert_ctx_t *ctx,
                                        int dst_width,
                                        int dst_height,
                                        enum AVPixelFormat dst_fmt);
app_status_t video_frame_convert_init_hw(video_frame_convert_ctx_t *ctx,
                                           int dst_width,
                                           int dst_height,
                                           enum AVPixelFormat dst_sw_fmt);
const AVFrame *video_frame_convert_prepare_rga_bgr_resize(video_frame_convert_ctx_t *ctx,
                                                    const AVFrame *src_frame);
const AVFrame *video_frame_convert_prepare(video_frame_convert_ctx_t *ctx,
                                           const AVFrame *src_frame,
                                           int *converted);
void video_frame_convert_deinit(video_frame_convert_ctx_t *ctx);

#endif
