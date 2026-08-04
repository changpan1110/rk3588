#ifndef OUTPUT_STORE_MP4_H
#define OUTPUT_STORE_MP4_H

#include "common/common.h"

typedef struct {
    char path[APP_PATH_MAX_LEN];
} output_store_mp4_ctx_t;

app_status_t output_store_mp4_init(output_store_mp4_ctx_t *ctx, const char *path);
app_status_t output_store_mp4_write(output_store_mp4_ctx_t *ctx, const uint8_t *data, size_t size);
void output_store_mp4_deinit(output_store_mp4_ctx_t *ctx);

#endif
