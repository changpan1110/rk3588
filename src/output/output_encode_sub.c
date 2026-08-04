#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "output_encode_sub.c"

#include "output/output_encode_sub.h"

#include <string.h>

#include "common/debug.h"

app_status_t output_encode_sub_init(output_encode_sub_ctx_t *ctx,
                                    encoder_mode_t mode,
                                    const video_encode_params_t *params,
                                    int enabled) {
    app_status_t status;

    if (ctx == NULL || params == NULL) {
        return APP_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->mode = mode;
    ctx->params = *params;
    ctx->enabled = enabled;
    LOGI("sub encoder init mode=%d enabled=%d %dx%d@%d bitrate=%d",
         mode,
         enabled,
         params->width,
         params->height,
         params->fps,
         params->bitrate);

    status = output_encode_convert_init(&ctx->convert,
                                        params->width,
                                        params->height,
                                        AV_PIX_FMT_NV12);
    if (status != APP_OK) {
        return status;
    }

    return APP_OK;
}

app_status_t output_encode_sub_push(output_encode_sub_ctx_t *ctx, const video_frame_t *frame) {
    const AVFrame *encode_frame;
    int converted;

    if (ctx == NULL || frame == NULL || frame->av_frame == NULL) {
        return APP_ERR_PARAM;
    }
    if (!ctx->enabled) {
        return APP_OK;
    }

    encode_frame = output_encode_prepare_frame(&ctx->convert, frame->av_frame, &converted);
    if (encode_frame == NULL) {
        LOGE("sub encode prepare frame failed source=%s src=%dx%d fmt=%d",
             frame->source_name,
             frame->av_frame->width,
             frame->av_frame->height,
             frame->av_frame->format);
        return APP_ERR_FFMPEG;
    }

    LOGD("sub encode frame source=%s -> %dx%d dst_fmt=NV12 converted=%d src=%dx%d fmt=%d",
         frame->source_name,
         ctx->params.width,
         ctx->params.height,
         converted,
         frame->av_frame->width,
         frame->av_frame->height,
         frame->av_frame->format);
    return APP_OK;
}

void output_encode_sub_deinit(output_encode_sub_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    output_encode_convert_deinit(&ctx->convert);
}
