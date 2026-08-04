#include "output/output_stream_rtmp.h"

#include <string.h>

app_status_t output_stream_rtmp_init(output_stream_rtmp_ctx_t *ctx, const char *url) {
    if (ctx == NULL || url == NULL) {
        return APP_ERR_PARAM;
    }
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->url, url, sizeof(ctx->url) - 1);
    return APP_OK;
}

app_status_t output_stream_rtmp_send(output_stream_rtmp_ctx_t *ctx, const uint8_t *data, size_t size) {
    (void)ctx;
    (void)data;
    (void)size;
    return APP_OK;
}

void output_stream_rtmp_deinit(output_stream_rtmp_ctx_t *ctx) {
    (void)ctx;
}
