#ifndef VIDEO_RKRGA_FILTER_H
#define VIDEO_RKRGA_FILTER_H

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include "common/common.h"

typedef struct video_rkrga_filter video_rkrga_filter_t;

app_status_t video_rkrga_filter_create(video_rkrga_filter_t **out,
                                               int src_width,
                                               int src_height,
                                               enum AVPixelFormat src_fmt,
                                               int dst_width,
                                               int dst_height,
                                               enum AVPixelFormat dst_fmt,
                                               int output_is_hw);
app_status_t video_rkrga_filter_create_hw(video_rkrga_filter_t **out,
                                                  int src_width,
                                                  int src_height,
                                                  AVBufferRef *src_hw_frames_ctx,
                                                  int dst_width,
                                                  int dst_height,
                                                  enum AVPixelFormat dst_fmt,
                                                  int output_is_hw);
const AVFrame *video_rkrga_filter_process(video_rkrga_filter_t *ctx,
                                                  const AVFrame *src_frame);
void video_rkrga_filter_destroy(video_rkrga_filter_t **ctx);

#endif
