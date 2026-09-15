#ifndef CONTROL_MODBUS_MODBUS_H
#define CONTROL_MODBUS_MODBUS_H

#include <stddef.h>
#include <stdint.h>

#include "agile_modbus/agile_modbus.h"
#include "input/serial/uart_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RTU 收发缓冲区大小（RTU 最大 ADU 256 字节） */
#define MODBUS_RTU_BUF_SIZE 256U

typedef enum {
    MODBUS_OK = 0,
    MODBUS_ERR_PARAM = -1,
    MODBUS_ERR_IO = -2,
    MODBUS_ERR_TIMEOUT = -3,
    MODBUS_ERR_EXCEPTION = -4
} modbus_status_t;

/*
 * Modbus RTU 句柄：串口 + agile_modbus RTU 上下文。
 * 使用方式：init -> (master 读写 / slave 轮询) -> close。
 */
typedef struct {
    uart_config_t uart;
    agile_modbus_rtu_t rtu;
    uint8_t send_buf[MODBUS_RTU_BUF_SIZE];
    uint8_t read_buf[MODBUS_RTU_BUF_SIZE];
    int opened;
} modbus_rtu_t;

/* 打开串口并初始化 agile_modbus RTU 上下文 */
modbus_status_t modbus_rtu_init(modbus_rtu_t *ctx,
                                const char *device,
                                uint32_t baudrate,
                                uart_parity_t parity,
                                uint8_t data_bits,
                                uint8_t stop_bits);
void modbus_rtu_close(modbus_rtu_t *ctx);
int modbus_rtu_is_open(const modbus_rtu_t *ctx);

/* ---- 主机(master)接口 ---- */
modbus_status_t modbus_master_read_holding_registers(modbus_rtu_t *ctx,
                                                     uint8_t slave,
                                                     uint16_t addr,
                                                     uint16_t nb,
                                                     uint16_t *dest,
                                                     int timeout_ms);
modbus_status_t modbus_master_read_input_registers(modbus_rtu_t *ctx,
                                                   uint8_t slave,
                                                   uint16_t addr,
                                                   uint16_t nb,
                                                   uint16_t *dest,
                                                   int timeout_ms);
modbus_status_t modbus_master_write_single_register(modbus_rtu_t *ctx,
                                                    uint8_t slave,
                                                    uint16_t addr,
                                                    uint16_t value,
                                                    int timeout_ms);
modbus_status_t modbus_master_write_multiple_registers(modbus_rtu_t *ctx,
                                                       uint8_t slave,
                                                       uint16_t addr,
                                                       uint16_t nb,
                                                       const uint16_t *src,
                                                       int timeout_ms);
modbus_status_t modbus_master_write_single_coil(modbus_rtu_t *ctx,
                                                uint8_t slave,
                                                uint16_t addr,
                                                int value,
                                                int timeout_ms);

/* ---- 从机(slave)接口 ---- */
/*
 * 读一帧请求，调用 agile_modbus_slave_handle 处理并把响应发回串口。
 * 返回 >=0 表示成功处理的请求长度，<0 表示失败。
 */
int modbus_slave_poll(modbus_rtu_t *ctx,
                      uint8_t slave,
                      uint8_t slave_strict,
                      agile_modbus_slave_callback_t callback,
                      const void *data,
                      int timeout_ms);

const char *modbus_status_string(modbus_status_t status);

#ifdef __cplusplus
}
#endif

#endif
