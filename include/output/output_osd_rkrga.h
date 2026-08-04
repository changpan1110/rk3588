#ifndef OUTPUT_OSD_RKRGA_H
#define OUTPUT_OSD_RKRGA_H

#include <stdint.h>

#include <libavutil/frame.h>

#include "common/common.h"

typedef struct output_osd_rkrga output_osd_rkrga_t;

app_status_t output_osd_rkrga_init(output_osd_rkrga_t **out,
                                   int width,
                                   int height,
                                   int overlay_width,
                                   int overlay_height,
                                   int overlay_x,
                                   int overlay_y);

const AVFrame *output_osd_rkrga_process(output_osd_rkrga_t *ctx,
                                        const AVFrame *main_frame,
                                        const AVFrame *rgba_overlay,
                                        uint64_t overlay_revision);

void output_osd_rkrga_deinit(output_osd_rkrga_t **ctx);

#endif
