#ifndef THERMAL_CAMERA_TEST_H
#define THERMAL_CAMERA_TEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 热成像相机独立测试菜单（测试代码）。 */
int thermal_camera_test_menu(const char *device, uint32_t baudrate);

#ifdef __cplusplus
}
#endif

#endif
