#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "output_snapshot.c"

#include "output/output_snapshot.h"

#include <stdio.h>
#include <string.h>

#include "common/debug.h"

app_status_t output_snapshot_init(output_snapshot_ctx_t *ctx, const char *output_dir) {
    if (ctx == NULL || output_dir == NULL) {
        return APP_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->output_dir, output_dir, sizeof(ctx->output_dir) - 1);
    return APP_OK;
}

app_status_t output_snapshot_take(output_snapshot_ctx_t *ctx,
                                  const output_snapshot_params_t *params,
                                  const video_frame_t *frame) {
    FILE *fp;
    const char *path;
    static const uint8_t jpeg_stub[] = {0xFF, 0xD8, 0xFF, 0xD9};

    if (ctx == NULL || params == NULL || params->path == NULL || frame == NULL || frame->av_frame == NULL) {
        return APP_ERR_PARAM;
    }

    path = params->path;
    fp = fopen(path, "wb");
    if (fp == NULL) {
        LOGE("snapshot open failed: %s", path);
        return APP_ERR_IO;
    }

    /* Placeholder snapshot flow: later this should encode the current frame to JPEG. */
    if (fwrite(jpeg_stub, 1, sizeof(jpeg_stub), fp) != sizeof(jpeg_stub)) {
        fclose(fp);
        LOGE("snapshot write failed: %s", path);
        return APP_ERR_IO;
    }

    fclose(fp);
    LOGI("snapshot saved path=%s source=%s", path, frame->source_name);
    return APP_OK;
}

void output_snapshot_deinit(output_snapshot_ctx_t *ctx) {
    (void)ctx;
}
