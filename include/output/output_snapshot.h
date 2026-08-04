#ifndef OUTPUT_SNAPSHOT_H
#define OUTPUT_SNAPSHOT_H

#include "common/common.h"

typedef struct {
    char output_dir[APP_PATH_MAX_LEN];
} output_snapshot_ctx_t;

typedef struct {
    const char *path;
} output_snapshot_params_t;

app_status_t output_snapshot_init(output_snapshot_ctx_t *ctx, const char *output_dir);
app_status_t output_snapshot_take(output_snapshot_ctx_t *ctx,
                                  const output_snapshot_params_t *params,
                                  const video_frame_t *frame);
void output_snapshot_deinit(output_snapshot_ctx_t *ctx);

#endif
