#include "common/common.h"

#include <string.h>
#include <sys/time.h>

const char *app_status_str(app_status_t status) {
    switch (status) {
        case APP_OK: return "APP_OK";
        case APP_ERR_PARAM: return "APP_ERR_PARAM";
        case APP_ERR_NOMEM: return "APP_ERR_NOMEM";
        case APP_ERR_FFMPEG: return "APP_ERR_FFMPEG";
        case APP_ERR_IO: return "APP_ERR_IO";
        case APP_ERR_EOF: return "APP_ERR_EOF";
        case APP_ERR_UNSUPPORTED: return "APP_ERR_UNSUPPORTED";
        case APP_ERR_BUSY: return "APP_ERR_BUSY";
        default: return "APP_ERR_UNKNOWN";
    }
}

const char *video_source_type_str(video_source_type_t source_type) {
    switch (source_type) {
        case VIDEO_SOURCE_USB: return "usb";
        case VIDEO_SOURCE_CSI0: return "csi0";
        case VIDEO_SOURCE_CSI1: return "csi1";
        case VIDEO_SOURCE_HDMI_IN: return "hdmi_in";
        default: return "unknown";
    }
}

int64_t app_get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000000LL) + tv.tv_usec;
}
