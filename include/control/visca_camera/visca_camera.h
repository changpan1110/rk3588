#ifndef VISCA_CAMERA_H
#define VISCA_CAMERA_H

#include "input/serial/uart_base.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VISCA_PACKET_MAX_SIZE 16U
#define VISCA_TITLE_LINE_COUNT 11U
#define VISCA_TITLE_CHARS_PER_LINE 20U

typedef enum {
    VISCA_OK = 0,
    VISCA_ERR_PARAM = -1,
    VISCA_ERR_UART = -2,
    VISCA_ERR_TIMEOUT = -3,
    VISCA_ERR_PROTOCOL = -4,
    VISCA_ERR_MESSAGE_LENGTH = -5,
    VISCA_ERR_SYNTAX = -6,
    VISCA_ERR_BUFFER_FULL = -7,
    VISCA_ERR_CANCELLED = -8,
    VISCA_ERR_NO_SOCKET = -9,
    VISCA_ERR_NOT_EXECUTABLE = -10,
    VISCA_ERR_CAMERA = -11
} visca_status_t;

typedef enum {
    VISCA_ADJUST_RESET = 0,
    VISCA_ADJUST_UP,
    VISCA_ADJUST_DOWN
} visca_adjust_t;

typedef enum {
    VISCA_DZOOM_OFF = 0,
    VISCA_DZOOM_ON,
    VISCA_DZOOM_SUPER_RESOLUTION
} visca_dzoom_mode_t;

typedef enum {
    VISCA_FOCUS_AUTO = 0,
    VISCA_FOCUS_MANUAL,
    VISCA_FOCUS_TOGGLE
} visca_focus_mode_t;

typedef enum {
    VISCA_AF_NORMAL = 0,
    VISCA_AF_INTERVAL,
    VISCA_AF_ZOOM_TRIGGER
} visca_af_mode_t;

typedef enum {
    VISCA_AF_SENSITIVITY_NORMAL = 0,
    VISCA_AF_SENSITIVITY_LOW
} visca_af_sensitivity_t;

typedef enum {
    VISCA_IR_CORRECTION_STANDARD = 0,
    VISCA_IR_CORRECTION_LIGHT
} visca_ir_correction_t;

typedef enum {
    VISCA_WB_AUTO = 0x00,
    VISCA_WB_INDOOR = 0x01,
    VISCA_WB_OUTDOOR = 0x02,
    VISCA_WB_ONE_PUSH = 0x03,
    VISCA_WB_ATW = 0x04,
    VISCA_WB_MANUAL = 0x05,
    VISCA_WB_OUTDOOR_AUTO = 0x06,
    VISCA_WB_SODIUM_LAMP_AUTO = 0x07,
    VISCA_WB_SODIUM_LAMP = 0x08,
    VISCA_WB_SODIUM_LAMP_OUTDOOR_AUTO = 0x09
} visca_wb_mode_t;

typedef enum {
    VISCA_AE_FULL_AUTO = 0x00,
    VISCA_AE_MANUAL = 0x03,
    VISCA_AE_SHUTTER_PRIORITY = 0x0A,
    VISCA_AE_IRIS_PRIORITY = 0x0B,
    VISCA_AE_BRIGHT = 0x0D,
    VISCA_AE_GAIN_PRIORITY = 0x0E
} visca_ae_mode_t;

typedef enum {
    VISCA_GAMMA_STANDARD = 0,
    VISCA_GAMMA_STRAIGHT = 1,
    VISCA_GAMMA_PATTERN = 2
} visca_gamma_mode_t;

typedef enum {
    VISCA_COLOR_BAR_OFF = 0,
    VISCA_COLOR_BAR_8_COLORS = 1,
    VISCA_COLOR_BAR_7_COLORS = 2,
    VISCA_COLOR_BAR_GRAYSCALE = 3
} visca_color_bar_mode_t;

typedef enum {
    VISCA_BLOCK_LENS = 0,
    VISCA_BLOCK_CAMERA_CONTROL = 1,
    VISCA_BLOCK_OTHER = 2,
    VISCA_BLOCK_EXTENDED_1 = 3,
    VISCA_BLOCK_EXTENDED_2 = 4,
    VISCA_BLOCK_EXTENDED_3 = 5
} visca_block_query_t;

typedef struct {
    uart_config_t uart;
    uint8_t address;
    uint8_t last_socket;
    uint8_t last_camera_error;
    int response_timeout_ms;
} visca_camera_t;

typedef struct {
    uint16_t model_code;
    uint32_t rom_version;
    uint8_t socket_count;
    uint8_t raw[7];
} visca_version_t;

typedef struct {
    uint8_t display_brightness;
    uint8_t brightness_compensation;
    uint8_t compensation_level;
} visca_ve_params_t;

/* Transport and raw access. */
visca_status_t visca_camera_open(visca_camera_t *camera,
                                 const char *device,
                                 uint32_t baudrate,
                                 uint8_t address);
void visca_camera_close(visca_camera_t *camera);
void visca_camera_set_timeout(visca_camera_t *camera, int timeout_ms);
visca_status_t visca_camera_send_raw(visca_camera_t *camera,
                                     const uint8_t *packet,
                                     size_t packet_size);
visca_status_t visca_camera_read_packet(visca_camera_t *camera,
                                        uint8_t *packet,
                                        size_t packet_capacity,
                                        int timeout_ms,
                                        size_t *packet_size);
visca_status_t visca_camera_command_raw(visca_camera_t *camera,
                                        const uint8_t *command,
                                        size_t command_size);
visca_status_t visca_camera_command_raw_async(visca_camera_t *camera,
                                              const uint8_t *command,
                                              size_t command_size);
visca_status_t visca_camera_inquiry_raw(visca_camera_t *camera,
                                        const uint8_t *inquiry,
                                        size_t inquiry_size,
                                        uint8_t *reply_data,
                                        size_t reply_capacity,
                                        size_t *reply_size);
const char *visca_camera_status_string(visca_status_t status);

/* Interface and system management commands. */
visca_status_t visca_camera_address_set(visca_camera_t *camera,
                                        uint8_t requested_address);
visca_status_t visca_camera_if_clear(visca_camera_t *camera);
visca_status_t visca_camera_if_clear_broadcast(visca_camera_t *camera);
visca_status_t visca_camera_cancel(visca_camera_t *camera, uint8_t socket);
visca_status_t visca_camera_set_power(visca_camera_t *camera, int on);
visca_status_t visca_camera_initialize_lens(visca_camera_t *camera);
visca_status_t visca_camera_reset(visca_camera_t *camera);
visca_status_t visca_camera_set_id(visca_camera_t *camera, uint16_t id);

/* Zoom commands. Continuous movement functions wait for the camera ACK
   before returning, so a following stop command sees a clean socket state. */
visca_status_t visca_camera_zoom_stop(visca_camera_t *camera);
visca_status_t visca_camera_zoom_tele(visca_camera_t *camera, uint8_t speed);
visca_status_t visca_camera_zoom_wide(visca_camera_t *camera, uint8_t speed);
visca_status_t visca_camera_zoom_direct(visca_camera_t *camera,
                                        uint16_t position);
visca_status_t visca_camera_zoom_optical_ratio(visca_camera_t *camera,
                                               uint8_t ratio);
visca_status_t visca_camera_set_dzoom_mode(visca_camera_t *camera,
                                           visca_dzoom_mode_t mode);
visca_status_t visca_camera_set_dzoom_combine(visca_camera_t *camera,
                                              int combine);
visca_status_t visca_camera_dzoom_stop(visca_camera_t *camera);
visca_status_t visca_camera_dzoom_tele(visca_camera_t *camera, uint8_t speed);
visca_status_t visca_camera_dzoom_wide(visca_camera_t *camera, uint8_t speed);
visca_status_t visca_camera_dzoom_toggle_x1_max(visca_camera_t *camera);
visca_status_t visca_camera_dzoom_direct(visca_camera_t *camera,
                                         uint8_t position);
visca_status_t visca_camera_zoom_focus_direct(visca_camera_t *camera,
                                              uint16_t zoom_position,
                                              uint16_t focus_position);
visca_status_t visca_camera_set_continuous_zoom_reply(visca_camera_t *camera,
                                                     int enabled);
visca_status_t visca_camera_set_zoom_reply_interval(visca_camera_t *camera,
                                                   uint8_t v_cycles);

/* Focus commands. */
visca_status_t visca_camera_set_focus_mode(visca_camera_t *camera,
                                          visca_focus_mode_t mode);
visca_status_t visca_camera_focus_stop(visca_camera_t *camera);
visca_status_t visca_camera_focus_far(visca_camera_t *camera, uint8_t speed);
visca_status_t visca_camera_focus_near(visca_camera_t *camera, uint8_t speed);
visca_status_t visca_camera_focus_direct(visca_camera_t *camera,
                                         uint16_t position);
visca_status_t visca_camera_focus_one_push(visca_camera_t *camera);
visca_status_t visca_camera_set_focus_near_limit(visca_camera_t *camera,
                                                uint16_t position);
visca_status_t visca_camera_set_af_mode(visca_camera_t *camera,
                                       visca_af_mode_t mode);
visca_status_t visca_camera_set_af_time(visca_camera_t *camera,
                                       uint8_t active_seconds,
                                       uint8_t interval_seconds);
visca_status_t visca_camera_set_af_sensitivity(visca_camera_t *camera,
                                              visca_af_sensitivity_t sensitivity);
visca_status_t visca_camera_set_spot_focus(visca_camera_t *camera, int enabled);
visca_status_t visca_camera_set_spot_focus_position(visca_camera_t *camera,
                                                   uint8_t x,
                                                   uint8_t y);
visca_status_t visca_camera_set_ir_correction(visca_camera_t *camera,
                                             visca_ir_correction_t mode);
visca_status_t visca_camera_set_continuous_focus_reply(visca_camera_t *camera,
                                                      int enabled);
visca_status_t visca_camera_set_focus_reply_interval(visca_camera_t *camera,
                                                    uint8_t v_cycles);

/* White balance commands. */
visca_status_t visca_camera_set_wb_mode(visca_camera_t *camera,
                                       visca_wb_mode_t mode);
visca_status_t visca_camera_wb_one_push(visca_camera_t *camera);
visca_status_t visca_camera_adjust_r_gain(visca_camera_t *camera,
                                         visca_adjust_t adjustment);
visca_status_t visca_camera_set_r_gain(visca_camera_t *camera, uint8_t value);
visca_status_t visca_camera_adjust_b_gain(visca_camera_t *camera,
                                         visca_adjust_t adjustment);
visca_status_t visca_camera_set_b_gain(visca_camera_t *camera, uint8_t value);

/* Exposure commands. */
visca_status_t visca_camera_set_ae_mode(visca_camera_t *camera,
                                       visca_ae_mode_t mode);
visca_status_t visca_camera_set_low_light_basis(visca_camera_t *camera,
                                               int enabled);
visca_status_t visca_camera_set_low_light_basis_position(visca_camera_t *camera,
                                                        uint8_t position);
visca_status_t visca_camera_adjust_shutter(visca_camera_t *camera,
                                          visca_adjust_t adjustment);
visca_status_t visca_camera_set_shutter(visca_camera_t *camera, uint8_t position);
visca_status_t visca_camera_set_max_shutter_limit(visca_camera_t *camera,
                                                 uint8_t position);
visca_status_t visca_camera_set_min_shutter_limit(visca_camera_t *camera,
                                                 uint8_t position);
visca_status_t visca_camera_set_slow_shutter(visca_camera_t *camera, int enabled);
visca_status_t visca_camera_set_slow_shutter_limit(visca_camera_t *camera,
                                                  uint8_t position);
visca_status_t visca_camera_adjust_iris(visca_camera_t *camera,
                                       visca_adjust_t adjustment);
visca_status_t visca_camera_set_iris(visca_camera_t *camera, uint8_t position);
visca_status_t visca_camera_adjust_gain(visca_camera_t *camera,
                                       visca_adjust_t adjustment);
visca_status_t visca_camera_set_gain(visca_camera_t *camera, uint8_t position);
visca_status_t visca_camera_set_gain_limit(visca_camera_t *camera, uint8_t limit);
visca_status_t visca_camera_set_gain_point(visca_camera_t *camera,
                                          uint8_t position);
visca_status_t visca_camera_enable_gain_point(visca_camera_t *camera, int enabled);
visca_status_t visca_camera_adjust_bright(visca_camera_t *camera,
                                         visca_adjust_t adjustment);
visca_status_t visca_camera_set_bright(visca_camera_t *camera, uint8_t position);
visca_status_t visca_camera_enable_exposure_compensation(visca_camera_t *camera,
                                                        int enabled);
visca_status_t visca_camera_adjust_exposure_compensation(
    visca_camera_t *camera,
    visca_adjust_t adjustment);
visca_status_t visca_camera_set_exposure_compensation(visca_camera_t *camera,
                                                     uint8_t position);
visca_status_t visca_camera_set_backlight(visca_camera_t *camera, int enabled);
visca_status_t visca_camera_set_spot_ae(visca_camera_t *camera, int enabled);
visca_status_t visca_camera_set_spot_ae_position(visca_camera_t *camera,
                                                uint8_t x,
                                                uint8_t y);
visca_status_t visca_camera_set_ae_response(visca_camera_t *camera,
                                           uint8_t response);
visca_status_t visca_camera_set_ve(visca_camera_t *camera, int enabled);
visca_status_t visca_camera_set_ve_parameters(visca_camera_t *camera,
                                             const visca_ve_params_t *params);
visca_status_t visca_camera_set_defog(visca_camera_t *camera,
                                     int enabled,
                                     uint8_t level);

/* Image processing commands. */
visca_status_t visca_camera_adjust_aperture(visca_camera_t *camera,
                                           visca_adjust_t adjustment);
visca_status_t visca_camera_set_aperture_level(visca_camera_t *camera,
                                              uint8_t level);
visca_status_t visca_camera_set_aperture_mode(visca_camera_t *camera,
                                             int manual);
visca_status_t visca_camera_set_aperture_bandwidth(visca_camera_t *camera,
                                                  uint8_t bandwidth);
visca_status_t visca_camera_set_aperture_crispening(visca_camera_t *camera,
                                                   uint8_t value);
visca_status_t visca_camera_set_aperture_hv_balance(visca_camera_t *camera,
                                                   uint8_t value);
visca_status_t visca_camera_set_aperture_bw_balance(visca_camera_t *camera,
                                                   uint8_t value);
visca_status_t visca_camera_set_aperture_limit(visca_camera_t *camera,
                                              uint8_t value);
visca_status_t visca_camera_set_aperture_highlight_detail(visca_camera_t *camera,
                                                         uint8_t value);
visca_status_t visca_camera_set_aperture_super_low(visca_camera_t *camera,
                                                  uint8_t value);
visca_status_t visca_camera_set_high_resolution(visca_camera_t *camera,
                                               int enabled);
visca_status_t visca_camera_set_noise_reduction(visca_camera_t *camera,
                                               uint8_t level);
visca_status_t visca_camera_set_noise_reduction_2d_3d(visca_camera_t *camera,
                                                     uint8_t level_2d,
                                                     uint8_t level_3d);
visca_status_t visca_camera_set_stabilizer(visca_camera_t *camera, int enabled);
visca_status_t visca_camera_set_gamma(visca_camera_t *camera,
                                     visca_gamma_mode_t mode);
visca_status_t visca_camera_set_gamma_pattern(visca_camera_t *camera,
                                             uint16_t pattern);
visca_status_t visca_camera_set_gamma_offset(visca_camera_t *camera,
                                            int negative,
                                            uint8_t offset);
visca_status_t visca_camera_set_high_sensitivity(visca_camera_t *camera,
                                                int enabled);
visca_status_t visca_camera_set_lr_reverse(visca_camera_t *camera, int enabled);
visca_status_t visca_camera_set_freeze(visca_camera_t *camera, int enabled);
visca_status_t visca_camera_set_black_white(visca_camera_t *camera, int enabled);
visca_status_t visca_camera_set_e_flip(visca_camera_t *camera, int enabled);

/* ICR/day-night commands. */
visca_status_t visca_camera_set_icr(visca_camera_t *camera, int enabled);
visca_status_t visca_camera_set_auto_icr(visca_camera_t *camera, int enabled);
visca_status_t visca_camera_set_auto_icr_threshold(visca_camera_t *camera,
                                                  uint8_t threshold);
visca_status_t visca_camera_set_auto_icr_alarm(visca_camera_t *camera,
                                              int enabled);

/* Memory, display, title and privacy commands. */
visca_status_t visca_camera_memory_reset(visca_camera_t *camera, uint8_t number);
visca_status_t visca_camera_memory_set(visca_camera_t *camera, uint8_t number);
visca_status_t visca_camera_memory_recall(visca_camera_t *camera, uint8_t number);
visca_status_t visca_camera_custom_reset(visca_camera_t *camera);
visca_status_t visca_camera_custom_set(visca_camera_t *camera);
visca_status_t visca_camera_custom_recall(visca_camera_t *camera);
visca_status_t visca_camera_user_memory_write(visca_camera_t *camera,
                                             uint8_t address,
                                             uint16_t value);
visca_status_t visca_camera_set_display(visca_camera_t *camera, int enabled);
visca_status_t visca_camera_toggle_display(visca_camera_t *camera);
visca_status_t visca_camera_title_set_style(visca_camera_t *camera,
                                           uint8_t line,
                                           uint8_t horizontal_position,
                                           uint8_t color,
                                           int blink);
visca_status_t visca_camera_title_set_characters(visca_camera_t *camera,
                                                uint8_t line,
                                                uint8_t start_index,
                                                const uint8_t *character_codes,
                                                size_t character_count);
visca_status_t visca_camera_title_clear(visca_camera_t *camera, uint8_t line);
visca_status_t visca_camera_title_enable(visca_camera_t *camera,
                                        uint8_t line,
                                        int enabled);
visca_status_t visca_camera_set_mute(visca_camera_t *camera, int enabled);
visca_status_t visca_camera_toggle_mute(visca_camera_t *camera);
visca_status_t visca_camera_privacy_set_mask(visca_camera_t *camera,
                                            uint8_t mask,
                                            int new_position,
                                            uint8_t half_width,
                                            uint8_t half_height);
visca_status_t visca_camera_privacy_set_table(visca_camera_t *camera,
                                             uint8_t table);
visca_status_t visca_camera_privacy_set_display(visca_camera_t *camera,
                                               uint32_t mask_bits);
visca_status_t visca_camera_privacy_set_color(visca_camera_t *camera,
                                             uint32_t color_select_bits,
                                             uint8_t color_zero,
                                             uint8_t color_one);
visca_status_t visca_camera_privacy_set_pan_tilt_angle(visca_camera_t *camera,
                                                     uint16_t pan,
                                                     uint16_t tilt);
visca_status_t visca_camera_privacy_set_ptz_mask(visca_camera_t *camera,
                                                uint8_t mask,
                                                uint16_t pan,
                                                uint16_t tilt,
                                                uint16_t zoom);
visca_status_t visca_camera_privacy_set_non_interlock_mask(
    visca_camera_t *camera,
    uint8_t mask,
    uint8_t x,
    uint8_t y,
    uint8_t half_width,
    uint8_t half_height);
visca_status_t visca_camera_set_center_line(visca_camera_t *camera, int enabled);

/* Register, color, extended, test-pattern and 4K ePT commands. */
visca_status_t visca_camera_register_write(visca_camera_t *camera,
                                          uint8_t register_number,
                                          uint8_t value);
visca_status_t visca_camera_set_color_enhancement_parameters(
    visca_camera_t *camera,
    uint8_t threshold,
    uint8_t high_luminance_color,
    uint8_t low_luminance_color);
visca_status_t visca_camera_set_color_enhancement(visca_camera_t *camera,
                                                 int enabled);
visca_status_t visca_camera_set_chroma_suppress(visca_camera_t *camera,
                                               uint8_t level);
visca_status_t visca_camera_set_color_gain(visca_camera_t *camera, uint8_t level);
visca_status_t visca_camera_set_color_hue(visca_camera_t *camera, uint8_t level);
visca_status_t visca_camera_extended_adjust_exposure_compensation(
    visca_camera_t *camera,
    visca_adjust_t adjustment,
    uint8_t steps);
visca_status_t visca_camera_extended_set_exposure_compensation(
    visca_camera_t *camera,
    uint8_t level);
visca_status_t visca_camera_extended_adjust_aperture(visca_camera_t *camera,
                                                    visca_adjust_t adjustment,
                                                    uint8_t steps);
visca_status_t visca_camera_extended_set_aperture(visca_camera_t *camera,
                                                 uint8_t level);
visca_status_t visca_camera_extended_set_auto_icr_threshold(
    visca_camera_t *camera,
    uint8_t threshold);
visca_status_t visca_camera_extended_set_auto_icr_on_level(visca_camera_t *camera,
                                                         uint8_t level);
visca_status_t visca_camera_extended_set_color_gain(visca_camera_t *camera,
                                                   uint8_t level);
visca_status_t visca_camera_extended_set_color_hue(visca_camera_t *camera,
                                                  uint8_t level);
visca_status_t visca_camera_set_hlc(visca_camera_t *camera,
                                   uint8_t level,
                                   uint8_t mask_level);
visca_status_t visca_camera_set_color_bar(visca_camera_t *camera,
                                         visca_color_bar_mode_t mode);
visca_status_t visca_camera_set_ept(visca_camera_t *camera, int enabled);
visca_status_t visca_camera_set_ept_position(visca_camera_t *camera,
                                            uint16_t pan,
                                            uint16_t tilt);
visca_status_t visca_camera_enter_maintenance_mode(visca_camera_t *camera);

/* Inquiry commands. */
visca_status_t visca_camera_get_power(visca_camera_t *camera, int *on);
visca_status_t visca_camera_get_zoom_position(visca_camera_t *camera,
                                             uint16_t *position);
visca_status_t visca_camera_get_dzoom_mode(visca_camera_t *camera,
                                          visca_dzoom_mode_t *mode);
visca_status_t visca_camera_get_dzoom_combine(visca_camera_t *camera,
                                             int *combine);
visca_status_t visca_camera_get_dzoom_position(visca_camera_t *camera,
                                              uint8_t *position);
visca_status_t visca_camera_get_focus_mode(visca_camera_t *camera,
                                          visca_focus_mode_t *mode);
visca_status_t visca_camera_get_focus_position(visca_camera_t *camera,
                                              uint16_t *position);
visca_status_t visca_camera_get_focus_near_limit(visca_camera_t *camera,
                                                uint16_t *position);
visca_status_t visca_camera_get_spot_focus(visca_camera_t *camera, int *enabled);
visca_status_t visca_camera_get_spot_focus_position(visca_camera_t *camera,
                                                   uint8_t *x,
                                                   uint8_t *y);
visca_status_t visca_camera_get_af_sensitivity(visca_camera_t *camera,
                                              visca_af_sensitivity_t *sensitivity);
visca_status_t visca_camera_get_af_mode(visca_camera_t *camera,
                                       visca_af_mode_t *mode);
visca_status_t visca_camera_get_af_time(visca_camera_t *camera,
                                       uint8_t *active_seconds,
                                       uint8_t *interval_seconds);
visca_status_t visca_camera_get_low_light_basis(visca_camera_t *camera,
                                               int *enabled);
visca_status_t visca_camera_get_low_light_basis_position(visca_camera_t *camera,
                                                        uint8_t *position);
visca_status_t visca_camera_get_ir_correction(visca_camera_t *camera,
                                             visca_ir_correction_t *mode);
visca_status_t visca_camera_get_wb_mode(visca_camera_t *camera,
                                       visca_wb_mode_t *mode);
visca_status_t visca_camera_get_r_gain(visca_camera_t *camera, uint8_t *value);
visca_status_t visca_camera_get_b_gain(visca_camera_t *camera, uint8_t *value);
visca_status_t visca_camera_get_ae_mode(visca_camera_t *camera,
                                       visca_ae_mode_t *mode);
visca_status_t visca_camera_get_shutter(visca_camera_t *camera, uint8_t *position);
visca_status_t visca_camera_get_max_shutter_limit(visca_camera_t *camera,
                                                 uint8_t *position);
visca_status_t visca_camera_get_min_shutter_limit(visca_camera_t *camera,
                                                 uint8_t *position);
visca_status_t visca_camera_get_slow_shutter(visca_camera_t *camera,
                                            int *enabled);
visca_status_t visca_camera_get_slow_shutter_limit(visca_camera_t *camera,
                                                  uint8_t *position);
visca_status_t visca_camera_get_iris(visca_camera_t *camera, uint8_t *position);
visca_status_t visca_camera_get_gain(visca_camera_t *camera, uint8_t *position);
visca_status_t visca_camera_get_gain_limit(visca_camera_t *camera, uint8_t *limit);
visca_status_t visca_camera_get_gain_point(visca_camera_t *camera,
                                          uint8_t *position);
visca_status_t visca_camera_get_gain_point_enabled(visca_camera_t *camera,
                                                  int *enabled);
visca_status_t visca_camera_get_bright(visca_camera_t *camera, uint8_t *position);
visca_status_t visca_camera_get_exposure_compensation_enabled(
    visca_camera_t *camera,
    int *enabled);
visca_status_t visca_camera_get_exposure_compensation(visca_camera_t *camera,
                                                     uint8_t *position);
visca_status_t visca_camera_get_backlight(visca_camera_t *camera, int *enabled);
visca_status_t visca_camera_get_spot_ae(visca_camera_t *camera, int *enabled);
visca_status_t visca_camera_get_spot_ae_position(visca_camera_t *camera,
                                                uint8_t *x,
                                                uint8_t *y);
visca_status_t visca_camera_get_ve(visca_camera_t *camera, int *enabled);
visca_status_t visca_camera_get_ve_parameters(visca_camera_t *camera,
                                             visca_ve_params_t *params);
visca_status_t visca_camera_get_ae_response(visca_camera_t *camera,
                                           uint8_t *response);
visca_status_t visca_camera_get_defog(visca_camera_t *camera,
                                     int *enabled,
                                     uint8_t *level);
visca_status_t visca_camera_get_aperture_level(visca_camera_t *camera,
                                              uint8_t *level);
visca_status_t visca_camera_get_aperture_mode(visca_camera_t *camera,
                                             int *manual);
visca_status_t visca_camera_get_aperture_bandwidth(visca_camera_t *camera,
                                                  uint8_t *value);
visca_status_t visca_camera_get_aperture_crispening(visca_camera_t *camera,
                                                   uint8_t *value);
visca_status_t visca_camera_get_aperture_hv_balance(visca_camera_t *camera,
                                                   uint8_t *value);
visca_status_t visca_camera_get_aperture_bw_balance(visca_camera_t *camera,
                                                   uint8_t *value);
visca_status_t visca_camera_get_aperture_limit(visca_camera_t *camera,
                                              uint8_t *value);
visca_status_t visca_camera_get_aperture_highlight_detail(visca_camera_t *camera,
                                                         uint8_t *value);
visca_status_t visca_camera_get_aperture_super_low(visca_camera_t *camera,
                                                  uint8_t *value);
visca_status_t visca_camera_get_high_resolution(visca_camera_t *camera,
                                               int *enabled);
visca_status_t visca_camera_get_noise_reduction(visca_camera_t *camera,
                                               uint8_t *level);
visca_status_t visca_camera_get_noise_reduction_2d_3d(visca_camera_t *camera,
                                                     uint8_t *level_2d,
                                                     uint8_t *level_3d);
visca_status_t visca_camera_get_stabilizer(visca_camera_t *camera, int *enabled);
visca_status_t visca_camera_get_gamma(visca_camera_t *camera,
                                     visca_gamma_mode_t *mode);
visca_status_t visca_camera_get_gamma_pattern(visca_camera_t *camera,
                                             uint16_t *pattern);
visca_status_t visca_camera_get_gamma_offset(visca_camera_t *camera,
                                            int *negative,
                                            uint8_t *offset);
visca_status_t visca_camera_get_high_sensitivity(visca_camera_t *camera,
                                                int *enabled);
visca_status_t visca_camera_get_lr_reverse(visca_camera_t *camera, int *enabled);
visca_status_t visca_camera_get_freeze(visca_camera_t *camera, int *enabled);
visca_status_t visca_camera_get_black_white(visca_camera_t *camera, int *enabled);
visca_status_t visca_camera_get_e_flip(visca_camera_t *camera, int *enabled);
visca_status_t visca_camera_get_icr(visca_camera_t *camera, int *enabled);
visca_status_t visca_camera_get_auto_icr(visca_camera_t *camera, int *enabled);
visca_status_t visca_camera_get_auto_icr_threshold(visca_camera_t *camera,
                                                  uint8_t *threshold);
visca_status_t visca_camera_get_auto_icr_alarm(visca_camera_t *camera,
                                              int *enabled);
visca_status_t visca_camera_get_last_memory(visca_camera_t *camera,
                                           uint8_t *number);
visca_status_t visca_camera_user_memory_read(visca_camera_t *camera,
                                            uint8_t address,
                                            uint16_t *value);
visca_status_t visca_camera_get_display(visca_camera_t *camera, int *enabled);
visca_status_t visca_camera_get_mute(visca_camera_t *camera, int *enabled);
visca_status_t visca_camera_get_privacy_table(visca_camera_t *camera,
                                             uint8_t *table);
visca_status_t visca_camera_get_privacy_display(visca_camera_t *camera,
                                               uint32_t *mask_bits);
visca_status_t visca_camera_get_privacy_pan_tilt(visca_camera_t *camera,
                                                uint16_t *pan,
                                                uint16_t *tilt);
visca_status_t visca_camera_get_privacy_ptz(visca_camera_t *camera,
                                           uint8_t mask,
                                           uint16_t *pan,
                                           uint16_t *tilt,
                                           uint16_t *zoom);
visca_status_t visca_camera_get_privacy_monitor(visca_camera_t *camera,
                                               uint32_t *mask_bits);
visca_status_t visca_camera_get_id(visca_camera_t *camera, uint16_t *id);
visca_status_t visca_camera_get_version(visca_camera_t *camera,
                                       visca_version_t *version);
visca_status_t visca_camera_get_continuous_zoom_reply(visca_camera_t *camera,
                                                     int *enabled);
visca_status_t visca_camera_get_zoom_reply_interval(visca_camera_t *camera,
                                                   uint8_t *v_cycles);
visca_status_t visca_camera_get_continuous_focus_reply(visca_camera_t *camera,
                                                      int *enabled);
visca_status_t visca_camera_get_focus_reply_interval(visca_camera_t *camera,
                                                    uint8_t *v_cycles);
visca_status_t visca_camera_get_extended_auto_icr_on_level(visca_camera_t *camera,
                                                         uint8_t *level);
visca_status_t visca_camera_get_minimum_shutter_enabled(visca_camera_t *camera,
                                                      int *enabled);
visca_status_t visca_camera_get_minimum_shutter_position(visca_camera_t *camera,
                                                       uint8_t *position);
visca_status_t visca_camera_get_hlc(visca_camera_t *camera,
                                   uint8_t *level,
                                   uint8_t *mask_level);
visca_status_t visca_camera_register_read(visca_camera_t *camera,
                                         uint8_t register_number,
                                         uint8_t *value);
visca_status_t visca_camera_get_color_bar(visca_camera_t *camera,
                                         visca_color_bar_mode_t *mode);
visca_status_t visca_camera_get_ept(visca_camera_t *camera, int *enabled);
visca_status_t visca_camera_get_ept_position(visca_camera_t *camera,
                                            uint16_t *pan,
                                            uint16_t *tilt);
visca_status_t visca_camera_get_temperature(visca_camera_t *camera,
                                           int8_t *raw_temperature);
visca_status_t visca_camera_get_block(visca_camera_t *camera,
                                     visca_block_query_t block,
                                     uint8_t *data,
                                     size_t data_capacity,
                                     size_t *data_size);

#ifdef __cplusplus
}
#endif

#endif
