#include "process/process_decode.h"

app_status_t process_decode_frame(video_frame_t *frame) {
    if (frame == NULL || frame->av_frame == NULL) {
        return APP_ERR_PARAM;
    }
    return APP_OK;
}
