#ifndef HIGH_SPEED_CAMERA_SERVICE_H
#define HIGH_SPEED_CAMERA_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 高速相机按键板后台服务（生产代码，供主程序调用）。
 * 生命周期：init() -> start() -> ... -> stop()。
 */
int high_speed_camera_service_init(const char *device, uint32_t baudrate);
int high_speed_camera_service_start(void);
void high_speed_camera_service_stop(void);
int high_speed_camera_service_request_unlock(void);

#ifdef __cplusplus
}
#endif

#endif
