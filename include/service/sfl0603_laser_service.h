#ifndef SFL0603_LASER_SERVICE_H
#define SFL0603_LASER_SERVICE_H

#include <stdint.h>

#include "control/sfl0603_laser/sfl0603_laser.h"
#include "control/video_pipeline/video_pipeline_control.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SFL0603 激光测距后台服务（生产代码，供主程序调用）。
 *
 * 服务内部用独立线程串行收发激光数据，并把距离推到 OSD。
 * 生命周期：init() -> start() -> ... -> stop()。
 * 按钮触发：request_single_measure() / request_continuous()。
 */
sfl0603_laser_status_t sfl0603_laser_service_init(const char *device,
                                                  uint32_t baudrate);
sfl0603_laser_status_t sfl0603_laser_service_start(void);
void sfl0603_laser_service_stop(void);

/* 设置 OSD 输出目标。在 init() 之后、start() 之前调用一次。 */
void sfl0603_laser_service_set_osd_control(vp_control_t *control);

/* 触发一次单次测距。 */
sfl0603_laser_status_t sfl0603_laser_service_request_single_measure(void);

/* enable != 0 启动连续测距，enable == 0 停止连续测距。 */
sfl0603_laser_status_t sfl0603_laser_service_request_continuous(uint16_t period_ms,
                                                                int enable);

#ifdef __cplusplus
}
#endif

#endif
