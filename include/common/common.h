#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>

#define APP_NAME_MAX_LEN 32
#define APP_PATH_MAX_LEN 128
#define APP_FMT_MAX_LEN 32

typedef enum {
    APP_OK = 0,
    APP_ERR_PARAM = -1,
    APP_ERR_NOMEM = -2,
    APP_ERR_FFMPEG = -3,
    APP_ERR_IO = -4,
    APP_ERR_EOF = -5,
    APP_ERR_UNSUPPORTED = -6,
    APP_ERR_BUSY = -7
} app_status_t;

typedef enum {
    VIDEO_SOURCE_USB = 0,
    VIDEO_SOURCE_CSI0,
    VIDEO_SOURCE_CSI1,
    VIDEO_SOURCE_HDMI_IN
} video_source_type_t;

typedef enum {
    ENCODER_MODE_SINGLE = 0,
    ENCODER_MODE_DUAL
} encoder_mode_t;

typedef struct {
    char name[APP_NAME_MAX_LEN];
    char device[APP_PATH_MAX_LEN];
    char input_format[APP_FMT_MAX_LEN];
    int width;
    int height;
    int fps;
    video_source_type_t source_type;
    int use_native_v4l2; /* HDMI: use the V4L2 DMA-BUF/DRM PRIME input path. */
} video_input_config_t;

typedef struct {
    AVFrame *av_frame;
    int64_t pts_us;
    video_source_type_t source_type;
    char source_name[APP_NAME_MAX_LEN];
} video_frame_t;

typedef struct {
    int width;
    int height;
    int fps;
    int bitrate;
} video_encode_params_t;

typedef struct {
    int enable_record;
    int enable_stream;
    video_encode_params_t record;
    video_encode_params_t stream;
} camera_output_config_t;

typedef struct {
    uint8_t *data;
    size_t size;
    int64_t pts_us;
    int is_key_frame;
    int width;
    int height;
} encoded_packet_t;

typedef struct {
    char device[APP_PATH_MAX_LEN];
    int baudrate;
    int is_rs485;
    int fd;
} uart_config_t;

const char *app_status_str(app_status_t status);
const char *video_source_type_str(video_source_type_t source_type);
int64_t app_get_time_us(void);

#endif
