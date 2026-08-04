#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "output_record.c"

#include "output/output_record.h"

#include <string.h>

#include "common/debug.h"

app_status_t output_record_init(output_record_ctx_t *ctx) {
    if (ctx == NULL) {
        return APP_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));
    return APP_OK;
}

app_status_t output_record_start(output_record_ctx_t *ctx, const output_record_params_t *params) {
    app_status_t status;

    if (ctx == NULL || params == NULL || params->path == NULL) {
        return APP_ERR_PARAM;
    }

    if (ctx->is_recording) {
        return APP_OK;
    }

    memset(ctx->path, 0, sizeof(ctx->path));
    strncpy(ctx->path, params->path, sizeof(ctx->path) - 1);
    ctx->encode = params->encode;

    status = output_store_mp4_init(&ctx->mp4, ctx->path);
    if (status != APP_OK) {
        return status;
    }

    ctx->is_recording = 1;
    LOGI("record start path=%s %dx%d@%d bitrate=%d",
         ctx->path,
         ctx->encode.width,
         ctx->encode.height,
         ctx->encode.fps,
         ctx->encode.bitrate);
    return APP_OK;
}

app_status_t output_record_write(output_record_ctx_t *ctx, const encoded_packet_t *packet) {
    if (ctx == NULL || packet == NULL) {
        return APP_ERR_PARAM;
    }
    if (!ctx->is_recording) {
        return APP_ERR_IO;
    }

    return output_store_mp4_write(&ctx->mp4, packet->data, packet->size);
}

app_status_t output_record_stop(output_record_ctx_t *ctx) {
    if (ctx == NULL) {
        return APP_ERR_PARAM;
    }
    if (!ctx->is_recording) {
        return APP_OK;
    }

    output_store_mp4_deinit(&ctx->mp4);
    ctx->is_recording = 0;
    LOGI("record stop path=%s", ctx->path);
    return APP_OK;
}

int output_record_is_running(const output_record_ctx_t *ctx) {
    if (ctx == NULL) {
        return 0;
    }
    return ctx->is_recording;
}

void output_record_deinit(output_record_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    output_record_stop(ctx);
}
