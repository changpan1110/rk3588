#ifndef HIGH_SPEED_CAMERA_TEST_H
#define HIGH_SPEED_CAMERA_TEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 高速相机按键板独立测试菜单（测试代码）。 */
int high_speed_camera_test_menu(const char *device, uint32_t baudrate);
int high_speed_camera_test_self_test(void);
void high_speed_camera_test_request_stop(void);

#ifdef __cplusplus
}
#endif

#endif
