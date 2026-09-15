#ifndef SFL0603_LASER_TEST_H
#define SFL0603_LASER_TEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int sfl0603_laser_test_menu(const char *device, uint32_t baudrate);
int sfl0603_laser_test_self_test(void);
void sfl0603_laser_test_request_stop(void);

#ifdef __cplusplus
}
#endif

#endif
