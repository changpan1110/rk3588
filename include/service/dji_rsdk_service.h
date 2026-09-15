#ifndef DJI_RSDK_SERVICE_H
#define DJI_RSDK_SERVICE_H

#include <stdint.h>

#include "control/dji_rsdk/dji_rsdk.h"
#include "control/video_pipeline/video_pipeline_control.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DJI RSDK 云台后台服务（生产代码，供主程序调用）。
 * 生命周期：init() -> start() -> ... -> stop()。
 * 服务内部用独立线程通过 SocketCAN 读取云台角度并写入 OSD。
 */

typedef enum {
    DJI_RSDK_GIMBAL_DIRECTION_UP = 0,
    DJI_RSDK_GIMBAL_DIRECTION_DOWN,
    DJI_RSDK_GIMBAL_DIRECTION_LEFT,
    DJI_RSDK_GIMBAL_DIRECTION_RIGHT
} dji_rsdk_gimbal_direction_t;

dji_rsdk_status_t dji_rsdk_service_init(vp_control_t *control);
dji_rsdk_status_t dji_rsdk_service_start(void);
void dji_rsdk_service_stop(void);
dji_rsdk_status_t dji_rsdk_service_set_gimbal_direction(
    dji_rsdk_gimbal_direction_t direction,
    int pressed,
    uint8_t speed_tenths_degree_per_second);
dji_rsdk_status_t dji_rsdk_service_center_gimbal(void);

#ifdef __cplusplus
}
#endif

#endif
