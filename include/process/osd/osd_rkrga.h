#ifndef OSD_RKRGA_H
#define OSD_RKRGA_H

#include <stdint.h>

#include <libavutil/frame.h>

#include "common/common.h"

typedef struct osd_rkrga osd_rkrga_t;

app_status_t osd_rkrga_init(osd_rkrga_t **out,
                                   int width,
                                   int height,
                                   int overlay_width,
                                   int overlay_height,
                                   int overlay_x,
                                   int overlay_y);

const AVFrame *osd_rkrga_process(osd_rkrga_t *ctx,
                                        const AVFrame *main_frame,
                                        const AVFrame *rgba_overlay,
                                        uint64_t overlay_revision);

void osd_rkrga_deinit(osd_rkrga_t **ctx);

#endif
