#define _POSIX_C_SOURCE 200809L

#include "control/thermal_camera/thermal_camera.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define THERMAL_CAMERA_CLASS_ISP 0x78U
#define THERMAL_CAMERA_CLASS_SYSTEM 0x74U
#define THERMAL_CAMERA_CLASS_CALIBRATION 0x7CU
#define THERMAL_CAMERA_CLASS_DISPLAY 0x70U

static uint8_t thermal_camera_checksum(const uint8_t *frame, size_t size) {
    uint32_t sum = 0U;
    size_t i;

    for (i = 2U; i + 2U < size; ++i) {
        sum += frame[i];
    }
    return (uint8_t)sum;
}

static void thermal_camera_log_frame(const thermal_camera_t *camera,
                                     const char *direction,
                                     const uint8_t *frame,
                                     size_t size) {
    size_t i;

    if (camera == NULL || !camera->loghex_enabled) {
        return;
    }
    fprintf(stderr, "[THERMAL_CAMERA][%s]", direction);
    for (i = 0U; i < size; ++i) {
        fprintf(stderr, " %02X", (unsigned)frame[i]);
    }
    fputc('\n', stderr);
}

void thermal_camera_init(thermal_camera_t *camera) {
    if (camera == NULL) {
        return;
    }
    memset(camera, 0, sizeof(*camera));
    uart_base_config_init(&camera->uart);
    camera->uart.baudrate = THERMAL_CAMERA_DEFAULT_BAUDRATE;
    camera->response_timeout_ms = 500;
}

thermal_camera_status_t thermal_camera_open(thermal_camera_t *camera,
                                            const char *device,
                                            uint32_t baudrate) {
    size_t length;
    uart_status_t status;

    if (camera == NULL || device == NULL || device[0] == '\0' ||
        baudrate == 0U) {
        return THERMAL_CAMERA_ERR_PARAM;
    }
    length = strlen(device);
    if (length >= sizeof(camera->uart.device)) {
        return THERMAL_CAMERA_ERR_PARAM;
    }
    thermal_camera_close(camera);
    uart_base_config_init(&camera->uart);
    memcpy(camera->uart.device, device, length + 1U);
    camera->uart.baudrate = baudrate;
    camera->uart.data_bits = 8U;
    camera->uart.parity = UART_PARITY_NONE;
    camera->uart.stop_bits = 1U;
    camera->uart.nonblocking = 1U;
    status = uart_base_open_port(&camera->uart);
    if (status != UART_OK) {
        return THERMAL_CAMERA_ERR_UART;
    }
    (void)uart_base_flush(&camera->uart, UART_FLUSH_BOTH);
    camera->opened = 1;
    return THERMAL_CAMERA_OK;
}

void thermal_camera_close(thermal_camera_t *camera) {
    if (camera == NULL) {
        return;
    }
    if (camera->uart.fd >= 0) {
        uart_base_close_port(&camera->uart);
    }
    camera->opened = 0;
}

int thermal_camera_is_open(const thermal_camera_t *camera) {
    return camera != NULL && camera->opened && camera->uart.fd >= 0;
}

void thermal_camera_set_debug(thermal_camera_t *camera, int enabled) {
    if (camera != NULL) {
        camera->debug_enabled = enabled ? 1 : 0;
    }
}

void thermal_camera_set_loghex(thermal_camera_t *camera, int enabled) {
    if (camera != NULL) {
        camera->loghex_enabled = enabled ? 1 : 0;
    }
}

static thermal_camera_status_t thermal_camera_write_frame(
    thermal_camera_t *camera, const uint8_t *frame, size_t size) {
    size_t total = 0U;

    while (total < size) {
        size_t written = 0U;
        uart_status_t status = uart_base_write_data(&camera->uart,
                                                     frame + total,
                                                     size - total,
                                                     &written);
        if (status != UART_OK || written == 0U) {
            return THERMAL_CAMERA_ERR_UART;
        }
        total += written;
    }
    thermal_camera_log_frame(camera, "TX", frame, size);
    return THERMAL_CAMERA_OK;
}

static thermal_camera_status_t thermal_camera_read_exact(thermal_camera_t *camera,
                                                         uint8_t *data,
                                                         size_t size,
                                                         int timeout_ms) {
    size_t total = 0U;

    while (total < size) {
        size_t received = 0U;
        uart_status_t status = uart_base_read_data(&camera->uart,
                                                    data + total,
                                                    size - total,
                                                    1U,
                                                    timeout_ms,
                                                    &received);
        if (status == UART_NO_DATA || received == 0U) {
            return THERMAL_CAMERA_ERR_TIMEOUT;
        }
        if (status != UART_OK) {
            return THERMAL_CAMERA_ERR_UART;
        }
        total += received;
        timeout_ms = camera->response_timeout_ms;
    }
    return THERMAL_CAMERA_OK;
}

static thermal_camera_status_t thermal_camera_receive_response(
    thermal_camera_t *camera,
    uint8_t expected_class,
    uint8_t expected_subcommand,
    uint8_t *data,
    size_t capacity,
    size_t *size,
    uint8_t *flag) {
    uint8_t frame[THERMAL_CAMERA_MAX_FRAME_SIZE];
    size_t frame_size;
    size_t data_size;
    thermal_camera_status_t status;

    status = thermal_camera_read_exact(camera, frame, 2U,
                                       camera->response_timeout_ms);
    if (status != THERMAL_CAMERA_OK) {
        return status;
    }
    if (frame[0] != THERMAL_CAMERA_FRAME_BEGIN || frame[1] < 4U ||
        frame[1] > THERMAL_CAMERA_MAX_DATA_SIZE + 4U) {
        return THERMAL_CAMERA_ERR_PROTOCOL;
    }
    frame_size = (size_t)frame[1] + 4U;
    status = thermal_camera_read_exact(camera, frame + 2U, frame_size - 2U,
                                       camera->response_timeout_ms);
    if (status != THERMAL_CAMERA_OK) {
        return status;
    }
    thermal_camera_log_frame(camera, "RX", frame, frame_size);
    if (frame[frame_size - 1U] != THERMAL_CAMERA_FRAME_END) {
        return THERMAL_CAMERA_ERR_PROTOCOL;
    }
    if (thermal_camera_checksum(frame, frame_size) != frame[frame_size - 2U]) {
        return THERMAL_CAMERA_ERR_CHECKSUM;
    }
    if (frame[2] != THERMAL_CAMERA_DEVICE_ADDRESS || frame[3] != expected_class ||
        frame[4] != expected_subcommand) {
        return THERMAL_CAMERA_ERR_PROTOCOL;
    }
    data_size = (size_t)frame[1] - 4U;
    if (data_size > capacity || (data_size > 0U && data == NULL)) {
        return THERMAL_CAMERA_ERR_PARAM;
    }
    if (data_size > 0U) {
        memcpy(data, frame + 6U, data_size);
    }
    if (size != NULL) {
        *size = data_size;
    }
    if (flag != NULL) {
        *flag = frame[5];
    }
    if (camera->debug_enabled) {
        fprintf(stderr, "[THERMAL_CAMERA] response flag=0x%02X data=%zu\n",
                (unsigned)frame[5], data_size);
    }
    return frame[5] == 0x04U ? THERMAL_CAMERA_ERR_REMOTE
                             : (frame[5] == 0x03U ? THERMAL_CAMERA_OK
                                                  : THERMAL_CAMERA_ERR_PROTOCOL);
}

thermal_camera_status_t thermal_camera_command(thermal_camera_t *camera,
                                               uint8_t class_id,
                                               uint8_t subcommand,
                                               uint8_t read_write,
                                               const uint8_t *data,
                                               size_t data_size,
                                               uint8_t *response_data,
                                               size_t response_capacity,
                                               size_t *response_size,
                                               uint8_t *response_flag) {
    uint8_t frame[THERMAL_CAMERA_MAX_FRAME_SIZE];
    size_t frame_size;
    size_t i;
    thermal_camera_status_t status;

    if (camera == NULL || !thermal_camera_is_open(camera) ||
        (data_size > 0U && data == NULL) || data_size > THERMAL_CAMERA_MAX_DATA_SIZE ||
        (read_write != 0U && read_write != 1U) ||
        (response_capacity > 0U && response_data == NULL)) {
        return camera == NULL || !thermal_camera_is_open(camera)
                   ? THERMAL_CAMERA_ERR_STATE
                   : THERMAL_CAMERA_ERR_PARAM;
    }
    frame_size = data_size + 8U;
    frame[0] = THERMAL_CAMERA_FRAME_BEGIN;
    frame[1] = (uint8_t)(data_size + 4U);
    frame[2] = THERMAL_CAMERA_DEVICE_ADDRESS;
    frame[3] = class_id;
    frame[4] = subcommand;
    frame[5] = read_write;
    for (i = 0U; i < data_size; ++i) {
        frame[6U + i] = data[i];
    }
    frame[6U + data_size] = thermal_camera_checksum(frame, frame_size);
    frame[7U + data_size] = THERMAL_CAMERA_FRAME_END;
    status = thermal_camera_write_frame(camera, frame, frame_size);
    if (status != THERMAL_CAMERA_OK) {
        return status;
    }
    return thermal_camera_receive_response(camera, class_id, subcommand,
                                           response_data, response_capacity,
                                           response_size, response_flag);
}

static thermal_camera_status_t thermal_camera_one_byte_set(
    thermal_camera_t *camera, uint8_t class_id, uint8_t subcommand, uint8_t value,
    uint8_t read_write) {
    uint8_t response[THERMAL_CAMERA_MAX_DATA_SIZE];
    size_t response_size = 0U;
    return thermal_camera_command(camera, class_id, subcommand, read_write,
                                  &value, 1U, response, sizeof(response),
                                  &response_size, NULL);
}

static thermal_camera_status_t thermal_camera_one_byte_get(
    thermal_camera_t *camera, uint8_t class_id, uint8_t subcommand, uint8_t *value) {
    uint8_t request = 0U;
    uint8_t response[THERMAL_CAMERA_MAX_DATA_SIZE];
    size_t response_size = 0U;
    thermal_camera_status_t status;

    if (value == NULL) {
        return THERMAL_CAMERA_ERR_PARAM;
    }
    status = thermal_camera_command(camera, class_id, subcommand, 1U,
                                    &request, 1U, response, sizeof(response),
                                    &response_size, NULL);
    if (status == THERMAL_CAMERA_OK) {
        if (response_size < 1U) {
            return THERMAL_CAMERA_ERR_PROTOCOL;
        }
        *value = response[0];
    }
    return status;
}

thermal_camera_status_t thermal_camera_set_pseudocolor(
    thermal_camera_t *camera, thermal_camera_pseudocolor_t mode) {
    return mode <= THERMAL_CAMERA_PSEUDOCOLOR_DEEP_BLUE
               ? thermal_camera_one_byte_set(camera, THERMAL_CAMERA_CLASS_ISP,
                                             0x20U, (uint8_t)mode, 0U)
               : THERMAL_CAMERA_ERR_RANGE;
}

thermal_camera_status_t thermal_camera_get_pseudocolor(
    thermal_camera_t *camera, thermal_camera_pseudocolor_t *mode) {
    uint8_t value;
    if (mode == NULL) return THERMAL_CAMERA_ERR_PARAM;
    thermal_camera_status_t status = thermal_camera_one_byte_get(
        camera, THERMAL_CAMERA_CLASS_ISP, 0x20U, &value);
    if (status == THERMAL_CAMERA_OK) {
        if (value > THERMAL_CAMERA_PSEUDOCOLOR_DEEP_BLUE) {
            return THERMAL_CAMERA_ERR_PROTOCOL;
        }
        *mode = (thermal_camera_pseudocolor_t)value;
    }
    return status;
}

static thermal_camera_status_t thermal_camera_set_level(thermal_camera_t *camera,
                                                        uint8_t subcommand,
                                                        uint8_t value) {
    return value <= 100U ? thermal_camera_one_byte_set(
                               camera, THERMAL_CAMERA_CLASS_ISP, subcommand,
                               value, 0U)
                         : THERMAL_CAMERA_ERR_RANGE;
}

static thermal_camera_status_t thermal_camera_get_level(thermal_camera_t *camera,
                                                        uint8_t subcommand,
                                                        uint8_t *value) {
    thermal_camera_status_t status = thermal_camera_one_byte_get(
        camera, THERMAL_CAMERA_CLASS_ISP, subcommand, value);
    if (status == THERMAL_CAMERA_OK && *value > 100U) {
        return THERMAL_CAMERA_ERR_PROTOCOL;
    }
    return status;
}

thermal_camera_status_t thermal_camera_set_brightness(thermal_camera_t *camera, uint8_t value) { return thermal_camera_set_level(camera, 0x02U, value); }
thermal_camera_status_t thermal_camera_get_brightness(thermal_camera_t *camera, uint8_t *value) { return thermal_camera_get_level(camera, 0x02U, value); }
thermal_camera_status_t thermal_camera_set_contrast(thermal_camera_t *camera, uint8_t value) { return thermal_camera_set_level(camera, 0x03U, value); }
thermal_camera_status_t thermal_camera_get_contrast(thermal_camera_t *camera, uint8_t *value) { return thermal_camera_get_level(camera, 0x03U, value); }
#if 0
thermal_camera_status_t thermal_camera_set_detail_enhancement(thermal_camera_t *camera, uint8_t value) { return thermal_camera_set_level(camera, 0x10U, value); }
thermal_camera_status_t thermal_camera_get_detail_enhancement(thermal_camera_t *camera, uint8_t *value) { return thermal_camera_get_level(camera, 0x10U, value); }
thermal_camera_status_t thermal_camera_set_spatial_noise_reduction(thermal_camera_t *camera, uint8_t value) { return thermal_camera_set_level(camera, 0x15U, value); }
thermal_camera_status_t thermal_camera_get_spatial_noise_reduction(thermal_camera_t *camera, uint8_t *value) { return thermal_camera_get_level(camera, 0x15U, value); }
thermal_camera_status_t thermal_camera_set_temporal_noise_reduction(thermal_camera_t *camera, uint8_t value) { return thermal_camera_set_level(camera, 0x16U, value); }
thermal_camera_status_t thermal_camera_get_temporal_noise_reduction(thermal_camera_t *camera, uint8_t *value) { return thermal_camera_get_level(camera, 0x16U, value); }

static thermal_camera_status_t thermal_camera_action(thermal_camera_t *camera,
                                                     uint8_t class_id,
                                                     uint8_t subcommand,
                                                     uint8_t value) {
    return thermal_camera_one_byte_set(camera, class_id, subcommand, value, 0U);
}

thermal_camera_status_t thermal_camera_save_settings(thermal_camera_t *camera) { return thermal_camera_action(camera, THERMAL_CAMERA_CLASS_SYSTEM, 0x10U, 0U); }
thermal_camera_status_t thermal_camera_factory_reset(thermal_camera_t *camera) { return thermal_camera_action(camera, THERMAL_CAMERA_CLASS_SYSTEM, 0x0FU, 0U); }
thermal_camera_status_t thermal_camera_manual_shutter_correction(thermal_camera_t *camera) { return thermal_camera_action(camera, THERMAL_CAMERA_CLASS_CALIBRATION, 0x02U, 0U); }
thermal_camera_status_t thermal_camera_manual_background_correction(thermal_camera_t *camera) { return thermal_camera_action(camera, THERMAL_CAMERA_CLASS_CALIBRATION, 0x03U, 0U); }
thermal_camera_status_t thermal_camera_vignetting_correction(thermal_camera_t *camera) { return thermal_camera_action(camera, THERMAL_CAMERA_CLASS_CALIBRATION, 0x0CU, 0x02U); }

thermal_camera_status_t thermal_camera_set_shutter_auto_mode(thermal_camera_t *camera, thermal_camera_shutter_auto_mode_t mode) {
    return mode <= THERMAL_CAMERA_SHUTTER_AUTO_FULL ? thermal_camera_action(camera, THERMAL_CAMERA_CLASS_CALIBRATION, 0x04U, (uint8_t)mode) : THERMAL_CAMERA_ERR_RANGE;
}

thermal_camera_status_t thermal_camera_get_shutter_auto_mode(thermal_camera_t *camera, thermal_camera_shutter_auto_mode_t *mode) {
    uint8_t value;
    if (mode == NULL) return THERMAL_CAMERA_ERR_PARAM;
    thermal_camera_status_t status = thermal_camera_one_byte_get(camera, THERMAL_CAMERA_CLASS_CALIBRATION, 0x04U, &value);
    if (status == THERMAL_CAMERA_OK) {
        if (value > THERMAL_CAMERA_SHUTTER_AUTO_FULL) return THERMAL_CAMERA_ERR_PROTOCOL;
        *mode = (thermal_camera_shutter_auto_mode_t)value;
    }
    return status;
}

thermal_camera_status_t thermal_camera_set_shutter_interval_minutes(thermal_camera_t *camera, uint16_t minutes) {
    uint8_t data[2] = {(uint8_t)(minutes >> 8), (uint8_t)minutes};
    uint8_t response[THERMAL_CAMERA_MAX_DATA_SIZE];
    size_t response_size;
    return thermal_camera_command(camera, THERMAL_CAMERA_CLASS_CALIBRATION, 0x05U, 0U, data, sizeof(data), response, sizeof(response), &response_size, NULL);
}

thermal_camera_status_t thermal_camera_get_shutter_interval_minutes(thermal_camera_t *camera, uint16_t *minutes) {
    uint8_t request = 0U;
    uint8_t response[THERMAL_CAMERA_MAX_DATA_SIZE];
    size_t response_size;
    thermal_camera_status_t status;
    if (minutes == NULL) return THERMAL_CAMERA_ERR_PARAM;
    status = thermal_camera_command(camera, THERMAL_CAMERA_CLASS_CALIBRATION, 0x05U, 1U, &request, 1U, response, sizeof(response), &response_size, NULL);
    if (status == THERMAL_CAMERA_OK) {
        if (response_size < 2U) return THERMAL_CAMERA_ERR_PROTOCOL;
        *minutes = (uint16_t)(((uint16_t)response[0] << 8) | response[1]);
    }
    return status;
}

thermal_camera_status_t thermal_camera_set_mirror_mode(thermal_camera_t *camera, thermal_camera_mirror_mode_t mode) {
    return mode <= THERMAL_CAMERA_MIRROR_UP_DOWN ? thermal_camera_action(camera, THERMAL_CAMERA_CLASS_DISPLAY, 0x11U, (uint8_t)mode) : THERMAL_CAMERA_ERR_RANGE;
}

thermal_camera_status_t thermal_camera_get_mirror_mode(thermal_camera_t *camera, thermal_camera_mirror_mode_t *mode) {
    uint8_t value;
    if (mode == NULL) return THERMAL_CAMERA_ERR_PARAM;
    thermal_camera_status_t status = thermal_camera_one_byte_get(camera, THERMAL_CAMERA_CLASS_DISPLAY, 0x11U, &value);
    if (status == THERMAL_CAMERA_OK) {
        if (value > THERMAL_CAMERA_MIRROR_UP_DOWN) return THERMAL_CAMERA_ERR_PROTOCOL;
        *mode = (thermal_camera_mirror_mode_t)value;
    }
    return status;
}

thermal_camera_status_t thermal_camera_set_zoom(thermal_camera_t *camera, thermal_camera_zoom_t zoom) {
    return zoom <= THERMAL_CAMERA_ZOOM_X8 ? thermal_camera_action(camera, THERMAL_CAMERA_CLASS_DISPLAY, 0x12U, (uint8_t)zoom) : THERMAL_CAMERA_ERR_RANGE;
}

thermal_camera_status_t thermal_camera_get_zoom(thermal_camera_t *camera, thermal_camera_zoom_t *zoom) {
    uint8_t value;
    if (zoom == NULL) return THERMAL_CAMERA_ERR_PARAM;
    thermal_camera_status_t status = thermal_camera_one_byte_get(camera, THERMAL_CAMERA_CLASS_DISPLAY, 0x12U, &value);
    if (status == THERMAL_CAMERA_OK) {
        if (value > THERMAL_CAMERA_ZOOM_X8) return THERMAL_CAMERA_ERR_PROTOCOL;
        *zoom = (thermal_camera_zoom_t)value;
    }
    return status;
}

thermal_camera_status_t thermal_camera_cursor_control(thermal_camera_t *camera, uint8_t command) {
    if (command == 0x00U || command == 0x0FU ||
        (command >= 0x02U && command <= 0x06U) || command == 0x0DU ||
        command == 0x0EU || ((command & 0xF0U) >= 0x20U &&
                             (command & 0xF0U) <= 0x50U &&
                             (command & 0x0FU) != 0U)) {
        return thermal_camera_action(camera, THERMAL_CAMERA_CLASS_ISP, 0x1AU, command);
    }
    return THERMAL_CAMERA_ERR_RANGE;
}
#endif

thermal_camera_status_t thermal_camera_query(thermal_camera_t *camera,
                                             uint8_t class_id,
                                             uint8_t subcommand,
                                             uint8_t *data,
                                             size_t capacity,
                                             size_t *size) {
    uint8_t request = 0U;
    return thermal_camera_command(camera, class_id, subcommand, 1U, &request, 1U,
                                  data, capacity, size, NULL);
}

const char *thermal_camera_status_string(thermal_camera_status_t status) {
    switch (status) {
        case THERMAL_CAMERA_OK: return "成功";
        case THERMAL_CAMERA_NO_DATA: return "无数据";
        case THERMAL_CAMERA_ERR_PARAM: return "参数错误";
        case THERMAL_CAMERA_ERR_STATE: return "串口未打开";
        case THERMAL_CAMERA_ERR_UART: return "串口通信错误";
        case THERMAL_CAMERA_ERR_TIMEOUT: return "等待响应超时";
        case THERMAL_CAMERA_ERR_PROTOCOL: return "协议格式错误";
        case THERMAL_CAMERA_ERR_CHECKSUM: return "校验和错误";
        case THERMAL_CAMERA_ERR_REMOTE: return "机芯返回错误";
        case THERMAL_CAMERA_ERR_RANGE: return "数值超出范围";
        case THERMAL_CAMERA_ERR_BUSY: return "命令队列已满";
        default: return "未知错误";
    }
}

const char *thermal_camera_pseudocolor_name(uint8_t mode) {
    static const char *names[] = {"白热", "黑热", "融合1", "彩虹", "融合2", "铁红1", "铁红2", "深褐色", "色彩1", "色彩2", "冰火", "雨", "绿热", "红热", "深蓝色"};
    return mode <= THERMAL_CAMERA_PSEUDOCOLOR_DEEP_BLUE ? names[mode] : "未知伪彩";
}

#if 0
const char *thermal_camera_shutter_mode_name(uint8_t mode) {
    static const char *names[] = {"关闭", "定时控制", "温差控制", "全自动"};
    return mode <= THERMAL_CAMERA_SHUTTER_AUTO_FULL ? names[mode] : "未知快门模式";
}

const char *thermal_camera_mirror_name(uint8_t mode) {
    static const char *names[] = {"无镜像", "中心镜像", "左右镜像", "上下镜像"};
    return mode <= THERMAL_CAMERA_MIRROR_UP_DOWN ? names[mode] : "未知镜像模式";
}

const char *thermal_camera_zoom_name(uint8_t zoom) {
    static const char *names[] = {"X1", "X2", "X4", "X8"};
    return zoom <= THERMAL_CAMERA_ZOOM_X8 ? names[zoom] : "未知放大倍数";
}
#endif
