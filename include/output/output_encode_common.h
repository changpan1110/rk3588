#ifndef OUTPUT_ENCODE_COMMON_H
#define OUTPUT_ENCODE_COMMON_H

#include <libswscale/swscale.h>

#include "common/common.h"
#include "output/output_encode_rkrga_filter.h"

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
    output_encode_rkrga_filter_t *rkrga_filter;
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
} output_encode_convert_ctx_t;

app_status_t output_encode_convert_init(output_encode_convert_ctx_t *ctx,
                                        int dst_width,
                                        int dst_height,
                                        enum AVPixelFormat dst_fmt);
app_status_t output_encode_convert_init_hw(output_encode_convert_ctx_t *ctx,
                                           int dst_width,
                                           int dst_height,
                                           enum AVPixelFormat dst_sw_fmt);
const AVFrame *output_encode_prepare_rga_bgr_resize(output_encode_convert_ctx_t *ctx,
                                                    const AVFrame *src_frame);
const AVFrame *output_encode_prepare_frame(output_encode_convert_ctx_t *ctx,
                                           const AVFrame *src_frame,
                                           int *converted);
void output_encode_convert_deinit(output_encode_convert_ctx_t *ctx);

#endif
