#ifndef THERMAL_CAMERA_SERVICE_H
#define THERMAL_CAMERA_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 热成像相机后台服务（生产代码，供主程序调用）。
 * 生命周期：init() -> start() -> ... -> stop()。
 */
int thermal_camera_service_init(const char *device, uint32_t baudrate);
int thermal_camera_service_start(void);
void thermal_camera_service_stop(void);
int thermal_camera_service_request_pseudocolor(uint8_t mode);

#ifdef __cplusplus
}
#endif

#endif
