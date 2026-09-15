#ifndef CONTROL_DJI_RSDK_H
#define CONTROL_DJI_RSDK_H

#include "input/can/can_socket.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DJI_RSDK_DEFAULT_CAN_INTERFACE "can0"
#define DJI_RSDK_CAN_BITRATE 1000000U
#define DJI_RSDK_PC_TX_CAN_ID 0x223U
#define DJI_RSDK_PC_RX_CAN_ID 0x222U
#define DJI_RSDK_GIMBAL_COMMAND_SET 0x0EU
#define DJI_RSDK_CAMERA_COMMAND_SET 0x0DU
#define DJI_RSDK_MAX_PACKET_SIZE 1023U
#define DJI_RSDK_MAX_COMMAND_DATA_SIZE 1005U

typedef enum {
    DJI_RSDK_OK = 0,
    DJI_RSDK_NO_DATA = 1,
    DJI_RSDK_ERR_PARAM = -1,
    DJI_RSDK_ERR_STATE = -2,
    DJI_RSDK_ERR_CAN = -3,
    DJI_RSDK_ERR_TIMEOUT = -4,
    DJI_RSDK_ERR_CRC = -5,
    DJI_RSDK_ERR_PROTOCOL = -6,
    DJI_RSDK_ERR_REMOTE = -7,
    DJI_RSDK_ERR_RANGE = -8,
    DJI_RSDK_ERR_BUFFER = -9
} dji_rsdk_status_t;

typedef enum {
    DJI_RSDK_ACK_NONE = 0,
    DJI_RSDK_ACK_OPTIONAL = 1,
    DJI_RSDK_ACK_REQUIRED = 3
} dji_rsdk_ack_policy_t;

typedef enum {
    DJI_RSDK_ANGLE_NONE = 0x00,
    DJI_RSDK_ANGLE_ATTITUDE = 0x01,
    DJI_RSDK_ANGLE_JOINT = 0x02
} dji_rsdk_angle_type_t;

typedef enum {
    DJI_RSDK_FOLLOW_LOCK = 0x00,
    DJI_RSDK_FOLLOW_YAW = 0x02,
    DJI_RSDK_FOLLOW_SPORT = 0x03
} dji_rsdk_follow_mode_t;

typedef enum {
    DJI_RSDK_ORIENTATION_UNCHANGED = 0x00,
    DJI_RSDK_ORIENTATION_LANDSCAPE_0 = 0x01,
    DJI_RSDK_ORIENTATION_LANDSCAPE_180 = 0x02,
    DJI_RSDK_ORIENTATION_PORTRAIT_90 = 0x03,
    DJI_RSDK_ORIENTATION_PORTRAIT_NEGATIVE_90 = 0x04,
    DJI_RSDK_ORIENTATION_TOGGLE = 0x05,
    DJI_RSDK_ORIENTATION_DEFAULT = 0xFF
} dji_rsdk_orientation_t;

typedef enum {
    DJI_RSDK_FOCUS_CALIBRATION_NONE = 0x00,
    DJI_RSDK_FOCUS_CALIBRATION_AUTO = 0x01,
    DJI_RSDK_FOCUS_CALIBRATION_MANUAL = 0x02,
    DJI_RSDK_FOCUS_CALIBRATION_SET_MIN = 0x04,
    DJI_RSDK_FOCUS_CALIBRATION_SET_MAX = 0x05,
    DJI_RSDK_FOCUS_CALIBRATION_STOP = 0x06
} dji_rsdk_focus_calibration_command_t;

typedef enum {
    DJI_RSDK_FOCUS_UNCALIBRATED = 0x01,
    DJI_RSDK_FOCUS_CALIBRATING = 0x02,
    DJI_RSDK_FOCUS_CALIBRATED = 0x03
} dji_rsdk_focus_calibration_state_t;

typedef enum {
    DJI_RSDK_CAMERA_PHOTO_START = 0x0001,
    DJI_RSDK_CAMERA_PHOTO_STOP = 0x0002,
    DJI_RSDK_CAMERA_RECORD_START = 0x0003,
    DJI_RSDK_CAMERA_RECORD_STOP = 0x0004,
    DJI_RSDK_CAMERA_CENTER_FOCUS_START = 0x0005,
    DJI_RSDK_CAMERA_CENTER_FOCUS_STOP = 0x000B
} dji_rsdk_camera_action_t;

typedef struct {
    uint16_t sequence;
    uint8_t command_type;
    uint8_t is_ack;
    uint8_t command_set;
    uint8_t command_id;
    uint8_t data[DJI_RSDK_MAX_COMMAND_DATA_SIZE];
    size_t data_size;
} dji_rsdk_frame_t;

typedef struct {
    dji_rsdk_angle_type_t type;
    int16_t yaw_tenths_degree;
    int16_t roll_tenths_degree;
    int16_t pitch_tenths_degree;
} dji_rsdk_angles_t;

typedef struct {
    uint8_t pitch_max_degree;
    uint8_t pitch_min_degree;
    uint8_t yaw_max_degree;
    uint8_t yaw_min_degree;
    uint8_t roll_max_degree;
    uint8_t roll_min_degree;
} dji_rsdk_angle_limits_t;

typedef struct {
    uint8_t pitch;
    uint8_t yaw;
    uint8_t roll;
} dji_rsdk_motor_stiffness_t;

typedef struct {
    uint8_t valid_flags;
    int16_t yaw_attitude_tenths_degree;
    int16_t roll_attitude_tenths_degree;
    int16_t pitch_attitude_tenths_degree;
    int16_t yaw_joint_tenths_degree;
    int16_t roll_joint_tenths_degree;
    int16_t pitch_joint_tenths_degree;
    dji_rsdk_angle_limits_t limits;
    dji_rsdk_motor_stiffness_t stiffness;
} dji_rsdk_gimbal_push_t;

typedef struct {
    uint32_t device_id;
    uint32_t version;
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    uint8_t build;
} dji_rsdk_version_t;

typedef struct {
    uint8_t status;
    uint8_t progress_percent;
    uint32_t error_status;
} dji_rsdk_auto_calibration_status_t;

typedef struct {
    dji_rsdk_focus_calibration_state_t calibration_state;
    uint32_t position;
} dji_rsdk_focus_position_t;

typedef void (*dji_rsdk_unsolicited_callback_t)(const dji_rsdk_frame_t *frame,
                                                 void *user_data);

typedef struct {
    can_socket_t can;
    uint16_t next_sequence;
    uint8_t rx_buffer[DJI_RSDK_MAX_PACKET_SIZE * 2U];
    size_t rx_size;
    int initialized;
    int debug_enabled;
    int loghex_enabled;
    dji_rsdk_unsolicited_callback_t unsolicited_callback;
    void *unsolicited_user_data;
} dji_rsdk_t;

void dji_rsdk_init(dji_rsdk_t *rsdk);
dji_rsdk_status_t dji_rsdk_open(dji_rsdk_t *rsdk,
                                const char *can_interface);
void dji_rsdk_close(dji_rsdk_t *rsdk);
int dji_rsdk_is_open(const dji_rsdk_t *rsdk);
void dji_rsdk_set_debug(dji_rsdk_t *rsdk, int enabled);
void dji_rsdk_set_loghex(dji_rsdk_t *rsdk, int enabled);
void dji_rsdk_set_unsolicited_callback(
    dji_rsdk_t *rsdk,
    dji_rsdk_unsolicited_callback_t callback,
    void *user_data);

dji_rsdk_status_t dji_rsdk_send_command(
    dji_rsdk_t *rsdk,
    uint8_t command_set,
    uint8_t command_id,
    const uint8_t *data,
    size_t data_size,
    dji_rsdk_ack_policy_t ack_policy,
    uint16_t *sequence);
dji_rsdk_status_t dji_rsdk_receive_frame(dji_rsdk_t *rsdk,
                                         int timeout_ms,
                                         dji_rsdk_frame_t *frame);
dji_rsdk_status_t dji_rsdk_request(dji_rsdk_t *rsdk,
                                   uint8_t command_set,
                                   uint8_t command_id,
                                   const uint8_t *request_data,
                                   size_t request_size,
                                   int timeout_ms,
                                   uint8_t *response_data,
                                   size_t response_capacity,
                                   size_t *response_size,
                                   uint8_t *remote_return_code);

dji_rsdk_status_t dji_rsdk_set_position(
    dji_rsdk_t *rsdk,
    int16_t yaw_tenths_degree,
    int16_t roll_tenths_degree,
    int16_t pitch_tenths_degree,
    int absolute_control,
    int yaw_enabled,
    int roll_enabled,
    int pitch_enabled,
    uint8_t action_time_tenths_second,
    int timeout_ms);
dji_rsdk_status_t dji_rsdk_set_speed(
    dji_rsdk_t *rsdk,
    int16_t yaw_tenths_degree_per_second,
    int16_t roll_tenths_degree_per_second,
    int16_t pitch_tenths_degree_per_second,
    int take_control,
    int ignore_focal_length,
    int timeout_ms);
dji_rsdk_status_t dji_rsdk_get_angles(dji_rsdk_t *rsdk,
                                      dji_rsdk_angle_type_t type,
                                      int timeout_ms,
                                      dji_rsdk_angles_t *angles);
dji_rsdk_status_t dji_rsdk_set_angle_limits(
    dji_rsdk_t *rsdk,
    const dji_rsdk_angle_limits_t *limits,
    int timeout_ms);
dji_rsdk_status_t dji_rsdk_get_angle_limits(
    dji_rsdk_t *rsdk,
    int timeout_ms,
    dji_rsdk_angle_limits_t *limits);
dji_rsdk_status_t dji_rsdk_set_motor_stiffness(
    dji_rsdk_t *rsdk,
    const dji_rsdk_motor_stiffness_t *stiffness,
    int timeout_ms);
dji_rsdk_status_t dji_rsdk_get_motor_stiffness(
    dji_rsdk_t *rsdk,
    int timeout_ms,
    dji_rsdk_motor_stiffness_t *stiffness);
dji_rsdk_status_t dji_rsdk_set_parameter_push(
    dji_rsdk_t *rsdk,
    int enabled,
    int timeout_ms);
dji_rsdk_status_t dji_rsdk_parse_gimbal_push(
    const dji_rsdk_frame_t *frame,
    dji_rsdk_gimbal_push_t *push);

dji_rsdk_status_t dji_rsdk_get_version(dji_rsdk_t *rsdk,
                                       uint32_t device_id,
                                       int timeout_ms,
                                       dji_rsdk_version_t *version);
dji_rsdk_status_t dji_rsdk_push_external_version(dji_rsdk_t *rsdk,
                                                 uint32_t device_id,
                                                 uint32_t version);
dji_rsdk_status_t dji_rsdk_send_joystick(dji_rsdk_t *rsdk,
                                         int16_t pitch,
                                         int16_t roll,
                                         int16_t yaw);
dji_rsdk_status_t dji_rsdk_send_dial(dji_rsdk_t *rsdk,
                                     int16_t speed);

dji_rsdk_status_t dji_rsdk_get_user_parameters(
    dji_rsdk_t *rsdk,
    const uint8_t *parameter_ids,
    size_t parameter_count,
    int timeout_ms,
    uint8_t *tlv_data,
    size_t tlv_capacity,
    size_t *tlv_size);
dji_rsdk_status_t dji_rsdk_set_user_parameters(
    dji_rsdk_t *rsdk,
    const uint8_t *tlv_data,
    size_t tlv_size,
    int timeout_ms,
    uint8_t *response_tlv,
    size_t response_capacity,
    size_t *response_size);
dji_rsdk_status_t dji_rsdk_set_work_mode(dji_rsdk_t *rsdk,
                                         uint8_t work_mode,
                                         dji_rsdk_orientation_t orientation,
                                         int timeout_ms);
dji_rsdk_status_t dji_rsdk_recenter(dji_rsdk_t *rsdk, int timeout_ms);
dji_rsdk_status_t dji_rsdk_selfie(dji_rsdk_t *rsdk, int timeout_ms);
dji_rsdk_status_t dji_rsdk_set_follow_mode(dji_rsdk_t *rsdk,
                                           dji_rsdk_follow_mode_t mode,
                                           int timeout_ms);
dji_rsdk_status_t dji_rsdk_set_auto_calibration(dji_rsdk_t *rsdk,
                                                int enabled,
                                                int single_pose_mode,
                                                int timeout_ms);
dji_rsdk_status_t dji_rsdk_parse_auto_calibration_push(
    const dji_rsdk_frame_t *frame,
    dji_rsdk_auto_calibration_status_t *status);
dji_rsdk_status_t dji_rsdk_toggle_smart_tracking(dji_rsdk_t *rsdk);

dji_rsdk_status_t dji_rsdk_set_focus_position(dji_rsdk_t *rsdk,
                                              uint16_t position);
dji_rsdk_status_t dji_rsdk_calibrate_focus_motor(
    dji_rsdk_t *rsdk,
    dji_rsdk_focus_calibration_command_t command,
    int timeout_ms);
dji_rsdk_status_t dji_rsdk_get_focus_position(
    dji_rsdk_t *rsdk,
    int timeout_ms,
    dji_rsdk_focus_position_t *position);

dji_rsdk_status_t dji_rsdk_camera_action(dji_rsdk_t *rsdk,
                                         dji_rsdk_camera_action_t action,
                                         int timeout_ms);
dji_rsdk_status_t dji_rsdk_camera_get_recording(dji_rsdk_t *rsdk,
                                                int timeout_ms,
                                                int *recording);

dji_rsdk_status_t dji_rsdk_protocol_self_test(void);
const char *dji_rsdk_status_string(dji_rsdk_status_t status);
const char *dji_rsdk_remote_code_string(uint8_t return_code);

#ifdef __cplusplus
}
#endif

#endif
