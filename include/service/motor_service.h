#ifndef MOTOR_SERVICE_H
#define MOTOR_SERVICE_H

#include <stdint.h>

#include "control/modbus/modbus.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 电机 + 光耦继电器 Modbus 后台服务（生产代码）。
 *
 * 通过一个串口线程接收/发送 Modbus RTU 数据：
 *   - 电机对焦 + / -（保持寄存器 0x100，含速度）
 *   - 光耦触发（线圈 0x100，写 0x05）
 *
 * 生命周期：init() -> start() -> ... -> stop()。
 */

/* 电机方向 */
typedef enum {
    MOTOR_DIRECTION_STOP = 0,
    MOTOR_DIRECTION_FORWARD = 1,   /* 对焦+ / 正转 */
    MOTOR_DIRECTION_REVERSE = -1   /* 对焦- / 反转 */
} motor_direction_t;

modbus_status_t motor_service_init(const char *device,
                                   uint32_t baudrate,
                                   uint8_t slave_addr);
modbus_status_t motor_service_start(void);
void motor_service_stop(void);

/* 电机移动：direction 正/负，speed 0(慢)~255(快) */
modbus_status_t motor_service_request_move(int direction, uint8_t speed);
/* 电机停止 */
modbus_status_t motor_service_request_stop(void);
/* 光耦触发：吸合一短脉冲（开 -> 延时 -> 关） */
modbus_status_t motor_service_request_trigger(void);

const char *motor_service_status_string(modbus_status_t status);

#ifdef __cplusplus
}
#endif

#endif
