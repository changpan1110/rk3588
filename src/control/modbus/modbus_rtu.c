#include "control/modbus/modbus.h"

#include <stdio.h>
#include <string.h>

static modbus_status_t modbus_rtu_read_byte(modbus_rtu_t *ctx,
                                            uint8_t *byte,
                                            int timeout_ms) {
    size_t got = 0U;
    uart_status_t status =
        uart_base_read_data(&ctx->uart, byte, 1U, 1U, timeout_ms, &got);

    if (status == UART_NO_DATA || (status == UART_OK && got == 0U)) {
        return MODBUS_ERR_TIMEOUT;
    }
    if (status != UART_OK || got != 1U) {
        return MODBUS_ERR_IO;
    }
    return MODBUS_OK;
}

/* 主机接收一帧响应，按功能码计算期望长度 */
static modbus_status_t modbus_master_receive(modbus_rtu_t *ctx,
                                             size_t *out_len,
                                             int timeout_ms) {
    agile_modbus_t *mb = &ctx->rtu._ctx;
    uint8_t *buf = mb->read_buf;
    size_t len = 0U;
    size_t expected = 0U;
    uint8_t function;

    if (modbus_rtu_read_byte(ctx, &buf[len++], timeout_ms) != MODBUS_OK) {
        return MODBUS_ERR_TIMEOUT;
    }
    if (modbus_rtu_read_byte(ctx, &buf[len++], timeout_ms) != MODBUS_OK) {
        return MODBUS_ERR_TIMEOUT;
    }

    function = buf[1];
    if ((function & 0x80U) != 0U) {
        expected = 5U; /* slave + function + exception + crc2 */
    } else {
        switch (function) {
        case AGILE_MODBUS_FC_READ_COILS:
        case AGILE_MODBUS_FC_READ_DISCRETE_INPUTS:
        case AGILE_MODBUS_FC_READ_HOLDING_REGISTERS:
        case AGILE_MODBUS_FC_READ_INPUT_REGISTERS:
        case AGILE_MODBUS_FC_WRITE_AND_READ_REGISTERS:
            if (modbus_rtu_read_byte(ctx, &buf[len++], timeout_ms) !=
                MODBUS_OK) {
                return MODBUS_ERR_TIMEOUT;
            }
            expected = 3U + (size_t)buf[2] + 2U;
            break;
        case AGILE_MODBUS_FC_WRITE_SINGLE_COIL:
        case AGILE_MODBUS_FC_WRITE_SINGLE_REGISTER:
        case AGILE_MODBUS_FC_WRITE_MULTIPLE_COILS:
        case AGILE_MODBUS_FC_WRITE_MULTIPLE_REGISTERS:
            expected = 8U;
            break;
        default:
            return MODBUS_ERR_EXCEPTION;
        }
    }

    while (len < expected) {
        if (modbus_rtu_read_byte(ctx, &buf[len++], timeout_ms) != MODBUS_OK) {
            return MODBUS_ERR_TIMEOUT;
        }
    }
    *out_len = len;
    return MODBUS_OK;
}

/* 从机接收一帧请求，按功能码计算期望长度 */
static modbus_status_t modbus_slave_receive(modbus_rtu_t *ctx,
                                            size_t *out_len,
                                            int timeout_ms) {
    agile_modbus_t *mb = &ctx->rtu._ctx;
    uint8_t *buf = mb->read_buf;
    size_t len = 0U;
    size_t expected = 0U;
    uint8_t function;

    if (modbus_rtu_read_byte(ctx, &buf[len++], timeout_ms) != MODBUS_OK) {
        return MODBUS_ERR_TIMEOUT;
    }
    if (modbus_rtu_read_byte(ctx, &buf[len++], timeout_ms) != MODBUS_OK) {
        return MODBUS_ERR_TIMEOUT;
    }

    function = buf[1];
    switch (function) {
    case AGILE_MODBUS_FC_READ_COILS:
    case AGILE_MODBUS_FC_READ_DISCRETE_INPUTS:
    case AGILE_MODBUS_FC_READ_HOLDING_REGISTERS:
    case AGILE_MODBUS_FC_READ_INPUT_REGISTERS:
    case AGILE_MODBUS_FC_WRITE_SINGLE_COIL:
    case AGILE_MODBUS_FC_WRITE_SINGLE_REGISTER:
        expected = 8U;
        break;
    case AGILE_MODBUS_FC_WRITE_MULTIPLE_COILS:
    case AGILE_MODBUS_FC_WRITE_MULTIPLE_REGISTERS: {
        size_t i;

        for (i = 0U; i < 5U; ++i) { /* addr2 + quantity2 + byte_count1 */
            if (modbus_rtu_read_byte(ctx, &buf[len++], timeout_ms) !=
                MODBUS_OK) {
                return MODBUS_ERR_TIMEOUT;
            }
        }
        expected = 9U + (size_t)buf[6] + 2U;
        break;
    }
    default:
        return MODBUS_ERR_EXCEPTION;
    }

    while (len < expected) {
        if (modbus_rtu_read_byte(ctx, &buf[len++], timeout_ms) != MODBUS_OK) {
            return MODBUS_ERR_TIMEOUT;
        }
    }
    *out_len = len;
    return MODBUS_OK;
}

modbus_status_t modbus_rtu_init(modbus_rtu_t *ctx,
                                const char *device,
                                uint32_t baudrate,
                                uart_parity_t parity,
                                uint8_t data_bits,
                                uint8_t stop_bits) {
    if (ctx == NULL || device == NULL || device[0] == '\0' || baudrate == 0U) {
        return MODBUS_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));
    uart_base_config_init(&ctx->uart);
    snprintf(ctx->uart.device, sizeof(ctx->uart.device), "%s", device);
    ctx->uart.baudrate = baudrate;
    ctx->uart.data_bits = data_bits;
    ctx->uart.parity = parity;
    ctx->uart.stop_bits = stop_bits;
    if (uart_base_open_port(&ctx->uart) != UART_OK) {
        return MODBUS_ERR_IO;
    }
    (void)uart_base_flush(&ctx->uart, UART_FLUSH_BOTH);

    agile_modbus_rtu_init(&ctx->rtu,
                          ctx->send_buf,
                          (int)sizeof(ctx->send_buf),
                          ctx->read_buf,
                          (int)sizeof(ctx->read_buf));
    ctx->opened = 1;
    return MODBUS_OK;
}

void modbus_rtu_close(modbus_rtu_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    if (ctx->opened) {
        uart_base_close_port(&ctx->uart);
    }
    ctx->opened = 0;
}

int modbus_rtu_is_open(const modbus_rtu_t *ctx) {
    return ctx != NULL && ctx->opened;
}

modbus_status_t modbus_master_read_holding_registers(modbus_rtu_t *ctx,
                                                     uint8_t slave,
                                                     uint16_t addr,
                                                     uint16_t nb,
                                                     uint16_t *dest,
                                                     int timeout_ms) {
    agile_modbus_t *mb;
    size_t read_len = 0U;
    size_t written = 0U;
    int send_len;
    modbus_status_t status;

    if (ctx == NULL || !ctx->opened || dest == NULL || nb == 0U) {
        return MODBUS_ERR_PARAM;
    }
    mb = &ctx->rtu._ctx;
    agile_modbus_set_slave(mb, slave);
    send_len = agile_modbus_serialize_read_registers(mb, addr, nb);
    if (send_len <= 0) {
        return MODBUS_ERR_PARAM;
    }

    (void)uart_base_flush(&ctx->uart, UART_FLUSH_BOTH);
    if (uart_base_write_data(&ctx->uart,
                             mb->send_buf,
                             (size_t)send_len,
                             &written) != UART_OK ||
        written != (size_t)send_len) {
        return MODBUS_ERR_IO;
    }

    status = modbus_master_receive(ctx, &read_len, timeout_ms);
    if (status != MODBUS_OK) {
        return status;
    }
    if (agile_modbus_deserialize_read_registers(mb, (int)read_len, dest) < 0) {
        return MODBUS_ERR_EXCEPTION;
    }
    return MODBUS_OK;
}

modbus_status_t modbus_master_read_input_registers(modbus_rtu_t *ctx,
                                                   uint8_t slave,
                                                   uint16_t addr,
                                                   uint16_t nb,
                                                   uint16_t *dest,
                                                   int timeout_ms) {
    agile_modbus_t *mb;
    size_t read_len = 0U;
    size_t written = 0U;
    int send_len;
    modbus_status_t status;

    if (ctx == NULL || !ctx->opened || dest == NULL || nb == 0U) {
        return MODBUS_ERR_PARAM;
    }
    mb = &ctx->rtu._ctx;
    agile_modbus_set_slave(mb, slave);
    send_len = agile_modbus_serialize_read_input_registers(mb, addr, nb);
    if (send_len <= 0) {
        return MODBUS_ERR_PARAM;
    }

    (void)uart_base_flush(&ctx->uart, UART_FLUSH_BOTH);
    if (uart_base_write_data(&ctx->uart,
                             mb->send_buf,
                             (size_t)send_len,
                             &written) != UART_OK ||
        written != (size_t)send_len) {
        return MODBUS_ERR_IO;
    }

    status = modbus_master_receive(ctx, &read_len, timeout_ms);
    if (status != MODBUS_OK) {
        return status;
    }
    if (agile_modbus_deserialize_read_input_registers(mb,
                                                      (int)read_len,
                                                      dest) < 0) {
        return MODBUS_ERR_EXCEPTION;
    }
    return MODBUS_OK;
}

modbus_status_t modbus_master_write_single_register(modbus_rtu_t *ctx,
                                                    uint8_t slave,
                                                    uint16_t addr,
                                                    uint16_t value,
                                                    int timeout_ms) {
    agile_modbus_t *mb;
    size_t read_len = 0U;
    size_t written = 0U;
    int send_len;
    modbus_status_t status;

    if (ctx == NULL || !ctx->opened) {
        return MODBUS_ERR_PARAM;
    }
    mb = &ctx->rtu._ctx;
    agile_modbus_set_slave(mb, slave);
    send_len = agile_modbus_serialize_write_register(mb, addr, value);
    if (send_len <= 0) {
        return MODBUS_ERR_PARAM;
    }

    (void)uart_base_flush(&ctx->uart, UART_FLUSH_BOTH);
    if (uart_base_write_data(&ctx->uart,
                             mb->send_buf,
                             (size_t)send_len,
                             &written) != UART_OK ||
        written != (size_t)send_len) {
        return MODBUS_ERR_IO;
    }

    status = modbus_master_receive(ctx, &read_len, timeout_ms);
    if (status != MODBUS_OK) {
        return status;
    }
    if (agile_modbus_deserialize_write_register(mb, (int)read_len) < 0) {
        return MODBUS_ERR_EXCEPTION;
    }
    return MODBUS_OK;
}

modbus_status_t modbus_master_write_multiple_registers(modbus_rtu_t *ctx,
                                                       uint8_t slave,
                                                       uint16_t addr,
                                                       uint16_t nb,
                                                       const uint16_t *src,
                                                       int timeout_ms) {
    agile_modbus_t *mb;
    size_t read_len = 0U;
    size_t written = 0U;
    int send_len;
    modbus_status_t status;

    if (ctx == NULL || !ctx->opened || src == NULL || nb == 0U) {
        return MODBUS_ERR_PARAM;
    }
    mb = &ctx->rtu._ctx;
    agile_modbus_set_slave(mb, slave);
    send_len = agile_modbus_serialize_write_registers(mb, addr, nb, src);
    if (send_len <= 0) {
        return MODBUS_ERR_PARAM;
    }

    (void)uart_base_flush(&ctx->uart, UART_FLUSH_BOTH);
    if (uart_base_write_data(&ctx->uart,
                             mb->send_buf,
                             (size_t)send_len,
                             &written) != UART_OK ||
        written != (size_t)send_len) {
        return MODBUS_ERR_IO;
    }

    status = modbus_master_receive(ctx, &read_len, timeout_ms);
    if (status != MODBUS_OK) {
        return status;
    }
    if (agile_modbus_deserialize_write_registers(mb, (int)read_len) < 0) {
        return MODBUS_ERR_EXCEPTION;
    }
    return MODBUS_OK;
}

modbus_status_t modbus_master_write_single_coil(modbus_rtu_t *ctx,
                                                uint8_t slave,
                                                uint16_t addr,
                                                int value,
                                                int timeout_ms) {
    agile_modbus_t *mb;
    size_t read_len = 0U;
    size_t written = 0U;
    int send_len;
    modbus_status_t status;

    if (ctx == NULL || !ctx->opened) {
        return MODBUS_ERR_PARAM;
    }
    mb = &ctx->rtu._ctx;
    agile_modbus_set_slave(mb, slave);
    send_len = agile_modbus_serialize_write_bit(mb, addr, value ? 1 : 0);
    if (send_len <= 0) {
        return MODBUS_ERR_PARAM;
    }

    (void)uart_base_flush(&ctx->uart, UART_FLUSH_BOTH);
    if (uart_base_write_data(&ctx->uart,
                             mb->send_buf,
                             (size_t)send_len,
                             &written) != UART_OK ||
        written != (size_t)send_len) {
        return MODBUS_ERR_IO;
    }

    status = modbus_master_receive(ctx, &read_len, timeout_ms);
    if (status != MODBUS_OK) {
        return status;
    }
    if (agile_modbus_deserialize_write_bit(mb, (int)read_len) < 0) {
        return MODBUS_ERR_EXCEPTION;
    }
    return MODBUS_OK;
}

int modbus_slave_poll(modbus_rtu_t *ctx,
                      uint8_t slave,
                      uint8_t slave_strict,
                      agile_modbus_slave_callback_t callback,
                      const void *data,
                      int timeout_ms) {
    agile_modbus_t *mb;
    size_t req_len = 0U;
    int send_len;
    int frame_length = 0;

    if (ctx == NULL || !ctx->opened || callback == NULL) {
        return -1;
    }
    mb = &ctx->rtu._ctx;
    agile_modbus_set_slave(mb, slave);

    if (modbus_slave_receive(ctx, &req_len, timeout_ms) != MODBUS_OK) {
        return -1;
    }

    send_len = agile_modbus_slave_handle(mb,
                                         (int)req_len,
                                         slave_strict,
                                         callback,
                                         data,
                                         &frame_length);
    if (send_len > 0) {
        size_t written = 0U;

        if (uart_base_write_data(&ctx->uart,
                                 mb->send_buf,
                                 (size_t)send_len,
                                 &written) != UART_OK ||
            written != (size_t)send_len) {
            return -1;
        }
    }
    return frame_length > 0 ? frame_length : -1;
}

const char *modbus_status_string(modbus_status_t status) {
    switch (status) {
    case MODBUS_OK:
        return "ok";
    case MODBUS_ERR_PARAM:
        return "invalid parameter";
    case MODBUS_ERR_IO:
        return "UART error";
    case MODBUS_ERR_TIMEOUT:
        return "response timeout";
    case MODBUS_ERR_EXCEPTION:
        return "modbus exception";
    default:
        return "unknown";
    }
}
