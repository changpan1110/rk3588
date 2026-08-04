#include "output/output_store_mp4.h"

#include <string.h>

app_status_t output_store_mp4_init(output_store_mp4_ctx_t *ctx, const char *path) {
    if (ctx == NULL || path == NULL) {
        return APP_ERR_PARAM;
    }
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->path, path, sizeof(ctx->path) - 1);
    return APP_OK;
}

app_status_t output_store_mp4_write(output_store_mp4_ctx_t *ctx, const uint8_t *data, size_t size) {
    (void)ctx;
    (void)data;
    (void)size;
    return APP_OK;
}

void output_store_mp4_deinit(output_store_mp4_ctx_t *ctx) {
    (void)ctx;
}
