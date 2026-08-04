#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "process_pip.c"

#include "process/process_pip.h"

#include "common/debug.h"

app_status_t process_pip_frame(process_common_ctx_t *ctx, video_frame_t *frame) {
    if (ctx == NULL || frame == NULL || frame->av_frame == NULL) {
        return APP_ERR_PARAM;
    }

    LOGD("process frame source=%s pts_us=%lld size=%dx%d fmt=%d",
        frame->source_name,
        (long long)frame->pts_us,
        frame->av_frame->width,
        frame->av_frame->height,
        frame->av_frame->format);
    return APP_OK;
}
