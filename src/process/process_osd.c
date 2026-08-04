#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "process_osd.c"

#include "process/process_osd.h"

#include "common/debug.h"

app_status_t process_osd_apply_laser(video_frame_t *frame, const laser_data_t *laser) {
    if (frame == NULL || laser == NULL || frame->av_frame == NULL) {
        return APP_ERR_PARAM;
    }

    LOGD("apply laser osd distance=%.3f signal=%d", laser->distance_m, laser->signal_level);
    return APP_OK;
}
