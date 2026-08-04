#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "process_common.c"

#include "process/process_common.h"

#include <string.h>

#include "common/debug.h"

app_status_t process_common_init(process_common_ctx_t *ctx) {
    if (ctx == NULL) {
        return APP_ERR_PARAM;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->main_index = 0;
    ctx->sub_index = 1;
    return APP_OK;
}

app_status_t process_common_convert_to_yuv420(process_common_ctx_t *ctx, video_frame_t *frame) {
    const AVFrame *converted_frame;
    int converted = 0;

    if (ctx == NULL || frame == NULL || frame->av_frame == NULL) {
        return APP_ERR_PARAM;
    }

    if (ctx->yuv420_convert.dst_width != frame->av_frame->width ||
        ctx->yuv420_convert.dst_height != frame->av_frame->height ||
        ctx->yuv420_convert.dst_fmt != AV_PIX_FMT_YUV420P) {
        output_encode_convert_deinit(&ctx->yuv420_convert);
        if (output_encode_convert_init(&ctx->yuv420_convert,
                                       frame->av_frame->width,
                                       frame->av_frame->height,
                                       AV_PIX_FMT_YUV420P) != APP_OK) {
            LOGE("process yuv420 convert init failed source=%s size=%dx%d",
                 frame->source_name,
                 frame->av_frame->width,
                 frame->av_frame->height);
            return APP_ERR_FFMPEG;
        }
    }

    converted_frame = output_encode_prepare_frame(&ctx->yuv420_convert, frame->av_frame, &converted);
    if (converted_frame == NULL) {
        LOGE("process convert to yuv420 failed source=%s src=%dx%d fmt=%d",
             frame->source_name,
             frame->av_frame->width,
             frame->av_frame->height,
             frame->av_frame->format);
        return APP_ERR_FFMPEG;
    }

    if (converted) {
        av_frame_unref(frame->av_frame);
        av_frame_move_ref(frame->av_frame, ctx->yuv420_convert.work_frame);
        LOGD("process convert frame source=%s -> yuv420p size=%dx%d",
             frame->source_name,
             frame->av_frame->width,
             frame->av_frame->height);
    }

    return APP_OK;
}

void process_common_deinit(process_common_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    output_encode_convert_deinit(&ctx->yuv420_convert);
}
