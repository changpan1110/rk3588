#ifndef OUTPUT_ENCODE_RKRGA_FILTER_H
#define OUTPUT_ENCODE_RKRGA_FILTER_H

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include "common/common.h"

typedef struct output_encode_rkrga_filter output_encode_rkrga_filter_t;

app_status_t output_encode_rkrga_filter_create(output_encode_rkrga_filter_t **out,
                                               int src_width,
                                               int src_height,
                                               enum AVPixelFormat src_fmt,
                                               int dst_width,
                                               int dst_height,
                                               enum AVPixelFormat dst_fmt,
                                               int output_is_hw);
app_status_t output_encode_rkrga_filter_create_hw(output_encode_rkrga_filter_t **out,
                                                  int src_width,
                                                  int src_height,
                                                  AVBufferRef *src_hw_frames_ctx,
                                                  int dst_width,
                                                  int dst_height,
                                                  enum AVPixelFormat dst_fmt,
                                                  int output_is_hw);
const AVFrame *output_encode_rkrga_filter_process(output_encode_rkrga_filter_t *ctx,
                                                  const AVFrame *src_frame);
void output_encode_rkrga_filter_destroy(output_encode_rkrga_filter_t **ctx);

#endif
