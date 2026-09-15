#ifndef DISPLAY_GUI_H
#define DISPLAY_GUI_H

#include "common/common.h"

typedef struct {
    int enabled;
    int initialized;
    int window_width;
    int window_height;
    int frame_width;
    int frame_height;
    int texture_width;
    int texture_height;
    enum AVPixelFormat frame_fmt;
    struct SwsContext *sws;
    void *window;
    void *renderer;
    void *texture;
    unsigned int texture_format;
    int use_direct_upload;
    int use_nv_upload;
    int use_yuv_upload;
    uint8_t *rgba_data;
    int rgba_linesize;
    int rgba_buf_size;
} display_gui_ctx_t;

app_status_t display_gui_init(display_gui_ctx_t *ctx, int enabled, int window_width, int window_height);
app_status_t display_gui_show(display_gui_ctx_t *ctx, const video_frame_t *frame);
int display_gui_poll_quit(display_gui_ctx_t *ctx);
void display_gui_deinit(display_gui_ctx_t *ctx);

#endif
