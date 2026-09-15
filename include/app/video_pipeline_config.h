#ifndef APP_VIDEO_PIPELINE_CONFIG_H
#define APP_VIDEO_PIPELINE_CONFIG_H

#include <stddef.h>

#include "process/video/video_pipeline.h"

#define VP_MAIN_MAX_CHANNELS 16

/* 串口设备配置，从 JSON 的 "serial" 对象读取，未提供的字段用默认值。 */
typedef struct {
    char sbus_device[APP_PATH_MAX_LEN];
    char high_speed_camera_device[APP_PATH_MAX_LEN];
    uint32_t high_speed_camera_baudrate;
    char thermal_camera_device[APP_PATH_MAX_LEN];
    uint32_t thermal_camera_baudrate;
    char laser_device[APP_PATH_MAX_LEN];
    uint32_t laser_baudrate;
    char visca_4k_camera_device[APP_PATH_MAX_LEN];
    uint32_t visca_4k_camera_baudrate;
    char motor_device[APP_PATH_MAX_LEN];
    uint32_t motor_baudrate;
    uint32_t motor_slave_addr;
    char mavlink_config_path[APP_PATH_MAX_LEN];
} vp_main_serial_config_t;

/* Owns all strings referenced by channels[]. */
typedef struct {
    vp_channel_config_t channels[VP_MAIN_MAX_CHANNELS];
    char storage_dir[APP_PATH_MAX_LEN];
    char record_dirs[VP_MAIN_MAX_CHANNELS][APP_PATH_MAX_LEN];
    char snapshot_dirs[VP_MAIN_MAX_CHANNELS][APP_PATH_MAX_LEN];
    char osd_labels[VP_MAIN_MAX_CHANNELS][APP_PATH_MAX_LEN];
    vp_main_serial_config_t serial;
    int channel_count;
} vp_main_channels_config_t;

int vp_main_channels_config_load_json(vp_main_channels_config_t *config,
                                      const char *path,
                                      char *error,
                                      size_t error_size);

#endif
