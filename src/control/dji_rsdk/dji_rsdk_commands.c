#include "control/dji_rsdk/dji_rsdk.h"

#include <string.h>

#define DJI_RSDK_CMD_POSITION 0x00U
#define DJI_RSDK_CMD_SPEED 0x01U
#define DJI_RSDK_CMD_ANGLES 0x02U
#define DJI_RSDK_CMD_SET_LIMITS 0x03U
#define DJI_RSDK_CMD_GET_LIMITS 0x04U
#define DJI_RSDK_CMD_SET_STIFFNESS 0x05U
#define DJI_RSDK_CMD_GET_STIFFNESS 0x06U
#define DJI_RSDK_CMD_PUSH_CONTROL 0x07U
#define DJI_RSDK_CMD_PARAMETER_PUSH 0x08U
#define DJI_RSDK_CMD_VERSION 0x09U
#define DJI_RSDK_CMD_EXTERNAL_CONTROL 0x0AU
#define DJI_RSDK_CMD_GET_USER_PARAMETERS 0x0BU
#define DJI_RSDK_CMD_SET_USER_PARAMETERS 0x0CU
#define DJI_RSDK_CMD_WORK_MODE 0x0DU
#define DJI_RSDK_CMD_CENTER_SELFIE_FOLLOW 0x0EU
#define DJI_RSDK_CMD_AUTO_CALIBRATION 0x0FU
#define DJI_RSDK_CMD_AUTO_CALIBRATION_PUSH 0x10U
#define DJI_RSDK_CMD_SMART_TRACKING 0x11U
#define DJI_RSDK_CMD_FOCUS_MOTOR 0x12U
#define DJI_RSDK_CMD_CAMERA_ACTION 0x00U
#define DJI_RSDK_CMD_CAMERA_STATUS 0x01U

#define DJI_RSDK_COMMAND_READ_U16_LE(data) \
    ((uint16_t)((uint16_t)(data)[0] | ((uint16_t)(data)[1] << 8)))

#define DJI_RSDK_COMMAND_READ_U32_LE(data) \
    ((uint32_t)(data)[0] | ((uint32_t)(data)[1] << 8) | \
     ((uint32_t)(data)[2] << 16) | ((uint32_t)(data)[3] << 24))

#define DJI_RSDK_COMMAND_WRITE_U16_LE(data, value)         \
    do {                                                   \
        uint8_t *dji_data_ = (data);                       \
        uint16_t dji_value_ = (uint16_t)(value);           \
        dji_data_[0] = (uint8_t)(dji_value_ & 0xFFU);      \
        dji_data_[1] = (uint8_t)(dji_value_ >> 8);         \
    } while (0)

#define DJI_RSDK_COMMAND_WRITE_U32_LE(data, value)             \
    do {                                                       \
        uint8_t *dji_data_ = (data);                           \
        uint32_t dji_value_ = (uint32_t)(value);               \
        dji_data_[0] = (uint8_t)(dji_value_ & 0xFFU);          \
        dji_data_[1] = (uint8_t)((dji_value_ >> 8) & 0xFFU);   \
        dji_data_[2] = (uint8_t)((dji_value_ >> 16) & 0xFFU);  \
        dji_data_[3] = (uint8_t)(dji_value_ >> 24);            \
    } while (0)

static dji_rsdk_status_t dji_rsdk_commands_ack(dji_rsdk_t *rsdk,
                                               uint8_t command_set,
                                               uint8_t command_id,
                                               const uint8_t *data,
                                               size_t data_size,
                                               int timeout_ms) {
    uint8_t ignored[16];
    size_t ignored_size = 0U;

    return dji_rsdk_request(rsdk,
                            command_set,
                            command_id,
                            data,
                            data_size,
                            timeout_ms,
                            ignored,
                            sizeof(ignored),
                            &ignored_size,
                            NULL);
}

static int dji_rsdk_commands_limits_valid(
    const dji_rsdk_angle_limits_t *limits) {
    return limits != NULL &&
           limits->pitch_max_degree <= 145U &&
           limits->pitch_min_degree <= 55U &&
           limits->yaw_max_degree <= 179U &&
           limits->yaw_min_degree <= 179U &&
           limits->roll_max_degree <= 30U &&
           limits->roll_min_degree <= 30U;
}

static int dji_rsdk_commands_stiffness_valid(
    const dji_rsdk_motor_stiffness_t *stiffness) {
    return stiffness != NULL && stiffness->pitch <= 100U &&
           stiffness->yaw <= 100U && stiffness->roll <= 100U;
}

static int dji_rsdk_commands_tlv_valid(const uint8_t *data, size_t size) {
    size_t offset = 0U;

    if (data == NULL || size < 2U) {
        return 0;
    }
    while (offset < size) {
        size_t value_size;

        if (size - offset < 2U) {
            return 0;
        }
        value_size = data[offset + 1U];
        offset += 2U;
        if (value_size > size - offset) {
            return 0;
        }
        offset += value_size;
    }
    return 1;
}

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
    int timeout_ms) {
    uint8_t data[8];

    if (yaw_tenths_degree < -1800 || yaw_tenths_degree > 1800 ||
        roll_tenths_degree < -300 || roll_tenths_degree > 300 ||
        pitch_tenths_degree < -560 || pitch_tenths_degree > 1460) {
        return DJI_RSDK_ERR_RANGE;
    }
    DJI_RSDK_COMMAND_WRITE_U16_LE(data, yaw_tenths_degree);
    DJI_RSDK_COMMAND_WRITE_U16_LE(data + 2U, roll_tenths_degree);
    DJI_RSDK_COMMAND_WRITE_U16_LE(data + 4U, pitch_tenths_degree);
    data[6] = (uint8_t)((absolute_control ? 0x01U : 0U) |
                        (yaw_enabled ? 0U : 0x02U) |
                        (roll_enabled ? 0U : 0x04U) |
                        (pitch_enabled ? 0U : 0x08U));
    data[7] = action_time_tenths_second;
    return dji_rsdk_commands_ack(rsdk,
                                 DJI_RSDK_GIMBAL_COMMAND_SET,
                                 DJI_RSDK_CMD_POSITION,
                                 data,
                                 sizeof(data),
                                 timeout_ms);
}

dji_rsdk_status_t dji_rsdk_set_speed(
    dji_rsdk_t *rsdk,
    int16_t yaw_tenths_degree_per_second,
    int16_t roll_tenths_degree_per_second,
    int16_t pitch_tenths_degree_per_second,
    int take_control,
    int ignore_focal_length,
    int timeout_ms) {
    uint8_t data[7];

    if (yaw_tenths_degree_per_second < -3600 ||
        yaw_tenths_degree_per_second > 3600 ||
        roll_tenths_degree_per_second < -3600 ||
        roll_tenths_degree_per_second > 3600 ||
        pitch_tenths_degree_per_second < -3600 ||
        pitch_tenths_degree_per_second > 3600) {
        return DJI_RSDK_ERR_RANGE;
    }
    DJI_RSDK_COMMAND_WRITE_U16_LE(
        data, (uint16_t)yaw_tenths_degree_per_second);
    DJI_RSDK_COMMAND_WRITE_U16_LE(
        data + 2U, (uint16_t)roll_tenths_degree_per_second);
    DJI_RSDK_COMMAND_WRITE_U16_LE(
        data + 4U, (uint16_t)pitch_tenths_degree_per_second);
    data[6] = (uint8_t)((take_control ? 0x80U : 0U) |
                        (ignore_focal_length ? 0x08U : 0U));
    return dji_rsdk_commands_ack(rsdk,
                                 DJI_RSDK_GIMBAL_COMMAND_SET,
                                 DJI_RSDK_CMD_SPEED,
                                 data,
                                 sizeof(data),
                                 timeout_ms);
}

dji_rsdk_status_t dji_rsdk_get_angles(dji_rsdk_t *rsdk,
                                      dji_rsdk_angle_type_t type,
                                      int timeout_ms,
                                      dji_rsdk_angles_t *angles) {
    uint8_t request = (uint8_t)type;
    uint8_t response[7];
    size_t response_size = 0U;
    dji_rsdk_status_t status;

    if (angles == NULL ||
        (type != DJI_RSDK_ANGLE_ATTITUDE && type != DJI_RSDK_ANGLE_JOINT)) {
        return DJI_RSDK_ERR_PARAM;
    }
    status = dji_rsdk_request(rsdk,
                              DJI_RSDK_GIMBAL_COMMAND_SET,
                              DJI_RSDK_CMD_ANGLES,
                              &request,
                              sizeof(request),
                              timeout_ms,
                              response,
                              sizeof(response),
                              &response_size,
                              NULL);
    if (status != DJI_RSDK_OK) {
        return status;
    }
    if (response_size != sizeof(response) || response[0] > 0x02U) {
        return DJI_RSDK_ERR_PROTOCOL;
    }
    angles->type = (dji_rsdk_angle_type_t)response[0];
    angles->yaw_tenths_degree =
        (int16_t)DJI_RSDK_COMMAND_READ_U16_LE(response + 1U);
    angles->roll_tenths_degree =
        (int16_t)DJI_RSDK_COMMAND_READ_U16_LE(response + 3U);
    angles->pitch_tenths_degree =
        (int16_t)DJI_RSDK_COMMAND_READ_U16_LE(response + 5U);
    return DJI_RSDK_OK;
}

dji_rsdk_status_t dji_rsdk_set_angle_limits(
    dji_rsdk_t *rsdk,
    const dji_rsdk_angle_limits_t *limits,
    int timeout_ms) {
    uint8_t data[7];

    if (!dji_rsdk_commands_limits_valid(limits)) {
        return limits == NULL ? DJI_RSDK_ERR_PARAM : DJI_RSDK_ERR_RANGE;
    }
    data[0] = 0x01U;
    data[1] = limits->pitch_max_degree;
    data[2] = limits->pitch_min_degree;
    data[3] = limits->yaw_max_degree;
    data[4] = limits->yaw_min_degree;
    data[5] = limits->roll_max_degree;
    data[6] = limits->roll_min_degree;
    return dji_rsdk_commands_ack(rsdk,
                                 DJI_RSDK_GIMBAL_COMMAND_SET,
                                 DJI_RSDK_CMD_SET_LIMITS,
                                 data,
                                 sizeof(data),
                                 timeout_ms);
}

dji_rsdk_status_t dji_rsdk_get_angle_limits(
    dji_rsdk_t *rsdk,
    int timeout_ms,
    dji_rsdk_angle_limits_t *limits) {
    uint8_t request = 0x01U;
    uint8_t response[6];
    size_t response_size = 0U;
    dji_rsdk_status_t status;

    if (limits == NULL) {
        return DJI_RSDK_ERR_PARAM;
    }
    status = dji_rsdk_request(rsdk,
                              DJI_RSDK_GIMBAL_COMMAND_SET,
                              DJI_RSDK_CMD_GET_LIMITS,
                              &request,
                              sizeof(request),
                              timeout_ms,
                              response,
                              sizeof(response),
                              &response_size,
                              NULL);
    if (status != DJI_RSDK_OK) {
        return status;
    }
    if (response_size != sizeof(response)) {
        return DJI_RSDK_ERR_PROTOCOL;
    }
    limits->pitch_max_degree = response[0];
    limits->pitch_min_degree = response[1];
    limits->yaw_max_degree = response[2];
    limits->yaw_min_degree = response[3];
    limits->roll_max_degree = response[4];
    limits->roll_min_degree = response[5];
    return dji_rsdk_commands_limits_valid(limits) ? DJI_RSDK_OK
                                                   : DJI_RSDK_ERR_PROTOCOL;
}

dji_rsdk_status_t dji_rsdk_set_motor_stiffness(
    dji_rsdk_t *rsdk,
    const dji_rsdk_motor_stiffness_t *stiffness,
    int timeout_ms) {
    uint8_t data[4];

    if (!dji_rsdk_commands_stiffness_valid(stiffness)) {
        return stiffness == NULL ? DJI_RSDK_ERR_PARAM : DJI_RSDK_ERR_RANGE;
    }
    data[0] = 0x01U;
    data[1] = stiffness->pitch;
    data[2] = stiffness->roll;
    data[3] = stiffness->yaw;
    return dji_rsdk_commands_ack(rsdk,
                                 DJI_RSDK_GIMBAL_COMMAND_SET,
                                 DJI_RSDK_CMD_SET_STIFFNESS,
                                 data,
                                 sizeof(data),
                                 timeout_ms);
}

dji_rsdk_status_t dji_rsdk_get_motor_stiffness(
    dji_rsdk_t *rsdk,
    int timeout_ms,
    dji_rsdk_motor_stiffness_t *stiffness) {
    uint8_t request = 0x01U;
    uint8_t response[3];
    size_t response_size = 0U;
    dji_rsdk_status_t status;

    if (stiffness == NULL) {
        return DJI_RSDK_ERR_PARAM;
    }
    status = dji_rsdk_request(rsdk,
                              DJI_RSDK_GIMBAL_COMMAND_SET,
                              DJI_RSDK_CMD_GET_STIFFNESS,
                              &request,
                              sizeof(request),
                              timeout_ms,
                              response,
                              sizeof(response),
                              &response_size,
                              NULL);
    if (status != DJI_RSDK_OK) {
        return status;
    }
    if (response_size != sizeof(response)) {
        return DJI_RSDK_ERR_PROTOCOL;
    }
    stiffness->pitch = response[0];
    stiffness->yaw = response[1];
    stiffness->roll = response[2];
    return dji_rsdk_commands_stiffness_valid(stiffness)
               ? DJI_RSDK_OK
               : DJI_RSDK_ERR_PROTOCOL;
}

dji_rsdk_status_t dji_rsdk_set_parameter_push(
    dji_rsdk_t *rsdk,
    int enabled,
    int timeout_ms) {
    uint8_t request = enabled ? 0x01U : 0x02U;

    return dji_rsdk_commands_ack(rsdk,
                                 DJI_RSDK_GIMBAL_COMMAND_SET,
                                 DJI_RSDK_CMD_PUSH_CONTROL,
                                 &request,
                                 sizeof(request),
                                 timeout_ms);
}

dji_rsdk_status_t dji_rsdk_parse_gimbal_push(
    const dji_rsdk_frame_t *frame,
    dji_rsdk_gimbal_push_t *push) {
    const uint8_t *data;

    if (frame == NULL || push == NULL) {
        return DJI_RSDK_ERR_PARAM;
    }
    if (frame->is_ack || frame->command_set != DJI_RSDK_GIMBAL_COMMAND_SET ||
        frame->command_id != DJI_RSDK_CMD_PARAMETER_PUSH ||
        frame->data_size != 22U) {
        return DJI_RSDK_ERR_PROTOCOL;
    }
    data = frame->data;
    memset(push, 0, sizeof(*push));
    push->valid_flags = data[0];
    push->yaw_attitude_tenths_degree =
        (int16_t)DJI_RSDK_COMMAND_READ_U16_LE(data + 1U);
    push->roll_attitude_tenths_degree =
        (int16_t)DJI_RSDK_COMMAND_READ_U16_LE(data + 3U);
    push->pitch_attitude_tenths_degree =
        (int16_t)DJI_RSDK_COMMAND_READ_U16_LE(data + 5U);
    push->yaw_joint_tenths_degree =
        (int16_t)DJI_RSDK_COMMAND_READ_U16_LE(data + 7U);
    push->roll_joint_tenths_degree =
        (int16_t)DJI_RSDK_COMMAND_READ_U16_LE(data + 9U);
    push->pitch_joint_tenths_degree =
        (int16_t)DJI_RSDK_COMMAND_READ_U16_LE(data + 11U);
    push->limits.pitch_max_degree = data[13];
    push->limits.pitch_min_degree = data[14];
    push->limits.yaw_max_degree = data[15];
    push->limits.yaw_min_degree = data[16];
    push->limits.roll_max_degree = data[17];
    push->limits.roll_min_degree = data[18];
    push->stiffness.pitch = data[19];
    push->stiffness.yaw = data[20];
    push->stiffness.roll = data[21];
    return DJI_RSDK_OK;
}

dji_rsdk_status_t dji_rsdk_get_version(dji_rsdk_t *rsdk,
                                       uint32_t device_id,
                                       int timeout_ms,
                                       dji_rsdk_version_t *version) {
    uint8_t request[4];
    uint8_t response[8];
    size_t response_size = 0U;
    dji_rsdk_status_t status;

    if (version == NULL) {
        return DJI_RSDK_ERR_PARAM;
    }
    DJI_RSDK_COMMAND_WRITE_U32_LE(request, device_id);
    status = dji_rsdk_request(rsdk,
                              DJI_RSDK_GIMBAL_COMMAND_SET,
                              DJI_RSDK_CMD_VERSION,
                              request,
                              sizeof(request),
                              timeout_ms,
                              response,
                              sizeof(response),
                              &response_size,
                              NULL);
    if (status != DJI_RSDK_OK) {
        return status;
    }
    if (response_size != sizeof(response)) {
        return DJI_RSDK_ERR_PROTOCOL;
    }
    version->device_id = DJI_RSDK_COMMAND_READ_U32_LE(response);
    version->version = DJI_RSDK_COMMAND_READ_U32_LE(response + 4U);
    version->major = (uint8_t)(version->version >> 24);
    version->minor = (uint8_t)(version->version >> 16);
    version->patch = (uint8_t)(version->version >> 8);
    version->build = (uint8_t)version->version;
    return DJI_RSDK_OK;
}

dji_rsdk_status_t dji_rsdk_push_external_version(dji_rsdk_t *rsdk,
                                                 uint32_t device_id,
                                                 uint32_t version) {
    uint8_t data[8];

    DJI_RSDK_COMMAND_WRITE_U32_LE(data, device_id);
    DJI_RSDK_COMMAND_WRITE_U32_LE(data + 4U, version);
    return dji_rsdk_send_command(rsdk,
                                 DJI_RSDK_GIMBAL_COMMAND_SET,
                                 DJI_RSDK_CMD_VERSION,
                                 data,
                                 sizeof(data),
                                 DJI_RSDK_ACK_NONE,
                                 NULL);
}

dji_rsdk_status_t dji_rsdk_send_joystick(dji_rsdk_t *rsdk,
                                         int16_t pitch,
                                         int16_t roll,
                                         int16_t yaw) {
    uint8_t data[7];

    if (pitch < -15000 || pitch > 15000 || roll < -15000 ||
        roll > 15000 || yaw < -15000 || yaw > 15000) {
        return DJI_RSDK_ERR_RANGE;
    }
    data[0] = 0x01U;
    DJI_RSDK_COMMAND_WRITE_U16_LE(data + 1U, pitch);
    DJI_RSDK_COMMAND_WRITE_U16_LE(data + 3U, roll);
    DJI_RSDK_COMMAND_WRITE_U16_LE(data + 5U, yaw);
    return dji_rsdk_send_command(rsdk,
                                 DJI_RSDK_GIMBAL_COMMAND_SET,
                                 DJI_RSDK_CMD_EXTERNAL_CONTROL,
                                 data,
                                 sizeof(data),
                                 DJI_RSDK_ACK_NONE,
                                 NULL);
}

dji_rsdk_status_t dji_rsdk_send_dial(dji_rsdk_t *rsdk,
                                     int16_t speed) {
    uint8_t data[3];

    if (speed < -2048 || speed > 2048) {
        return DJI_RSDK_ERR_RANGE;
    }
    data[0] = 0x02U;
    DJI_RSDK_COMMAND_WRITE_U16_LE(data + 1U, speed);
    return dji_rsdk_send_command(rsdk,
                                 DJI_RSDK_GIMBAL_COMMAND_SET,
                                 DJI_RSDK_CMD_EXTERNAL_CONTROL,
                                 data,
                                 sizeof(data),
                                 DJI_RSDK_ACK_NONE,
                                 NULL);
}

dji_rsdk_status_t dji_rsdk_get_user_parameters(
    dji_rsdk_t *rsdk,
    const uint8_t *parameter_ids,
    size_t parameter_count,
    int timeout_ms,
    uint8_t *tlv_data,
    size_t tlv_capacity,
    size_t *tlv_size) {
    dji_rsdk_status_t status;

    if (parameter_ids == NULL || parameter_count == 0U ||
        parameter_count > DJI_RSDK_MAX_COMMAND_DATA_SIZE ||
        tlv_data == NULL || tlv_size == NULL) {
        return DJI_RSDK_ERR_PARAM;
    }
    status = dji_rsdk_request(rsdk,
                              DJI_RSDK_GIMBAL_COMMAND_SET,
                              DJI_RSDK_CMD_GET_USER_PARAMETERS,
                              parameter_ids,
                              parameter_count,
                              timeout_ms,
                              tlv_data,
                              tlv_capacity,
                              tlv_size,
                              NULL);
    if (status != DJI_RSDK_OK) {
        return status;
    }
    return *tlv_size == 0U || dji_rsdk_commands_tlv_valid(tlv_data, *tlv_size)
               ? DJI_RSDK_OK
               : DJI_RSDK_ERR_PROTOCOL;
}

dji_rsdk_status_t dji_rsdk_set_user_parameters(
    dji_rsdk_t *rsdk,
    const uint8_t *tlv_data,
    size_t tlv_size,
    int timeout_ms,
    uint8_t *response_tlv,
    size_t response_capacity,
    size_t *response_size) {
    dji_rsdk_status_t status;

    if (!dji_rsdk_commands_tlv_valid(tlv_data, tlv_size) ||
        response_size == NULL ||
        (response_capacity > 0U && response_tlv == NULL)) {
        return DJI_RSDK_ERR_PARAM;
    }
    status = dji_rsdk_request(rsdk,
                              DJI_RSDK_GIMBAL_COMMAND_SET,
                              DJI_RSDK_CMD_SET_USER_PARAMETERS,
                              tlv_data,
                              tlv_size,
                              timeout_ms,
                              response_tlv,
                              response_capacity,
                              response_size,
                              NULL);
    if (status != DJI_RSDK_OK) {
        return status;
    }
    return *response_size == 0U ||
                   dji_rsdk_commands_tlv_valid(response_tlv, *response_size)
               ? DJI_RSDK_OK
               : DJI_RSDK_ERR_PROTOCOL;
}

dji_rsdk_status_t dji_rsdk_set_work_mode(dji_rsdk_t *rsdk,
                                         uint8_t work_mode,
                                         dji_rsdk_orientation_t orientation,
                                         int timeout_ms) {
    uint8_t data[2];

    if (work_mode != 0xFEU ||
        (orientation != DJI_RSDK_ORIENTATION_UNCHANGED &&
         orientation != DJI_RSDK_ORIENTATION_LANDSCAPE_0 &&
         orientation != DJI_RSDK_ORIENTATION_LANDSCAPE_180 &&
         orientation != DJI_RSDK_ORIENTATION_PORTRAIT_90 &&
         orientation != DJI_RSDK_ORIENTATION_PORTRAIT_NEGATIVE_90 &&
         orientation != DJI_RSDK_ORIENTATION_TOGGLE &&
         orientation != DJI_RSDK_ORIENTATION_DEFAULT)) {
        return DJI_RSDK_ERR_PARAM;
    }
    data[0] = work_mode;
    data[1] = (uint8_t)orientation;
    return dji_rsdk_commands_ack(rsdk,
                                 DJI_RSDK_GIMBAL_COMMAND_SET,
                                 DJI_RSDK_CMD_WORK_MODE,
                                 data,
                                 sizeof(data),
                                 timeout_ms);
}

static dji_rsdk_status_t dji_rsdk_commands_center_selfie(
    dji_rsdk_t *rsdk,
    uint8_t command,
    int timeout_ms) {
    uint8_t data[2] = {0xFEU, command};

    return dji_rsdk_commands_ack(rsdk,
                                 DJI_RSDK_GIMBAL_COMMAND_SET,
                                 DJI_RSDK_CMD_CENTER_SELFIE_FOLLOW,
                                 data,
                                 sizeof(data),
                                 timeout_ms);
}

dji_rsdk_status_t dji_rsdk_recenter(dji_rsdk_t *rsdk, int timeout_ms) {
    return dji_rsdk_commands_center_selfie(rsdk, 0x01U, timeout_ms);
}

dji_rsdk_status_t dji_rsdk_selfie(dji_rsdk_t *rsdk, int timeout_ms) {
    return dji_rsdk_commands_center_selfie(rsdk, 0x02U, timeout_ms);
}

dji_rsdk_status_t dji_rsdk_set_follow_mode(dji_rsdk_t *rsdk,
                                           dji_rsdk_follow_mode_t mode,
                                           int timeout_ms) {
    uint8_t data[2];

    if (mode != DJI_RSDK_FOLLOW_LOCK && mode != DJI_RSDK_FOLLOW_YAW &&
        mode != DJI_RSDK_FOLLOW_SPORT) {
        return DJI_RSDK_ERR_PARAM;
    }
    data[0] = (uint8_t)mode;
    data[1] = 0x00U;
    return dji_rsdk_commands_ack(rsdk,
                                 DJI_RSDK_GIMBAL_COMMAND_SET,
                                 DJI_RSDK_CMD_CENTER_SELFIE_FOLLOW,
                                 data,
                                 sizeof(data),
                                 timeout_ms);
}

dji_rsdk_status_t dji_rsdk_set_auto_calibration(dji_rsdk_t *rsdk,
                                                int enabled,
                                                int single_pose_mode,
                                                int timeout_ms) {
    uint8_t tlv[3];

    tlv[0] = 0x00U;
    tlv[1] = 0x01U;
    tlv[2] = (uint8_t)((enabled ? 0x01U : 0U) |
                       (single_pose_mode ? 0x02U : 0U));
    return dji_rsdk_commands_ack(rsdk,
                                 DJI_RSDK_GIMBAL_COMMAND_SET,
                                 DJI_RSDK_CMD_AUTO_CALIBRATION,
                                 tlv,
                                 sizeof(tlv),
                                 timeout_ms);
}

dji_rsdk_status_t dji_rsdk_parse_auto_calibration_push(
    const dji_rsdk_frame_t *frame,
    dji_rsdk_auto_calibration_status_t *status) {
    size_t offset = 0U;

    if (frame == NULL || status == NULL) {
        return DJI_RSDK_ERR_PARAM;
    }
    if (frame->is_ack || frame->command_set != DJI_RSDK_GIMBAL_COMMAND_SET ||
        frame->command_id != DJI_RSDK_CMD_AUTO_CALIBRATION_PUSH) {
        return DJI_RSDK_ERR_PROTOCOL;
    }
    while (offset + 2U <= frame->data_size) {
        uint8_t type = frame->data[offset];
        size_t length = frame->data[offset + 1U];

        offset += 2U;
        if (length > frame->data_size - offset) {
            return DJI_RSDK_ERR_PROTOCOL;
        }
        if (type == 0x00U) {
            if (length != 6U) {
                return DJI_RSDK_ERR_PROTOCOL;
            }
            status->status = frame->data[offset];
            status->progress_percent = frame->data[offset + 1U];
            status->error_status =
                DJI_RSDK_COMMAND_READ_U32_LE(frame->data + offset + 2U);
            return status->progress_percent <= 100U ? DJI_RSDK_OK
                                                    : DJI_RSDK_ERR_PROTOCOL;
        }
        offset += length;
    }
    return DJI_RSDK_ERR_PROTOCOL;
}

dji_rsdk_status_t dji_rsdk_toggle_smart_tracking(dji_rsdk_t *rsdk) {
    uint8_t data = 0x03U;

    return dji_rsdk_send_command(rsdk,
                                 DJI_RSDK_GIMBAL_COMMAND_SET,
                                 DJI_RSDK_CMD_SMART_TRACKING,
                                 &data,
                                 sizeof(data),
                                 DJI_RSDK_ACK_NONE,
                                 NULL);
}

dji_rsdk_status_t dji_rsdk_set_focus_position(dji_rsdk_t *rsdk,
                                              uint16_t position) {
    uint8_t data[5];

    if (position > 4095U) {
        return DJI_RSDK_ERR_RANGE;
    }
    data[0] = 0x01U;
    data[1] = 0x00U;
    data[2] = 0x02U;
    DJI_RSDK_COMMAND_WRITE_U16_LE(data + 3U, position);
    return dji_rsdk_send_command(rsdk,
                                 DJI_RSDK_GIMBAL_COMMAND_SET,
                                 DJI_RSDK_CMD_FOCUS_MOTOR,
                                 data,
                                 sizeof(data),
                                 DJI_RSDK_ACK_NONE,
                                 NULL);
}

dji_rsdk_status_t dji_rsdk_calibrate_focus_motor(
    dji_rsdk_t *rsdk,
    dji_rsdk_focus_calibration_command_t command,
    int timeout_ms) {
    uint8_t request[3];
    uint8_t response[3];
    size_t response_size = 0U;
    dji_rsdk_status_t status;

    if (command != DJI_RSDK_FOCUS_CALIBRATION_NONE &&
        command != DJI_RSDK_FOCUS_CALIBRATION_AUTO &&
        command != DJI_RSDK_FOCUS_CALIBRATION_MANUAL &&
        command != DJI_RSDK_FOCUS_CALIBRATION_SET_MIN &&
        command != DJI_RSDK_FOCUS_CALIBRATION_SET_MAX &&
        command != DJI_RSDK_FOCUS_CALIBRATION_STOP) {
        return DJI_RSDK_ERR_PARAM;
    }
    request[0] = 0x02U;
    request[1] = 0x00U;
    request[2] = (uint8_t)command;
    status = dji_rsdk_request(rsdk,
                              DJI_RSDK_GIMBAL_COMMAND_SET,
                              DJI_RSDK_CMD_FOCUS_MOTOR,
                              request,
                              sizeof(request),
                              timeout_ms,
                              response,
                              sizeof(response),
                              &response_size,
                              NULL);
    if (status != DJI_RSDK_OK) {
        return status;
    }
    if (response_size != sizeof(response) || response[0] != 0x02U ||
        response[1] != 0x00U) {
        return DJI_RSDK_ERR_PROTOCOL;
    }
    return response[2] == 0x00U ? DJI_RSDK_OK : DJI_RSDK_ERR_REMOTE;
}

dji_rsdk_status_t dji_rsdk_get_focus_position(
    dji_rsdk_t *rsdk,
    int timeout_ms,
    dji_rsdk_focus_position_t *position) {
    uint8_t request[2] = {0x15U, 0x00U};
    uint8_t response[7];
    size_t response_size = 0U;
    dji_rsdk_status_t status;

    if (position == NULL) {
        return DJI_RSDK_ERR_PARAM;
    }
    status = dji_rsdk_request(rsdk,
                              DJI_RSDK_GIMBAL_COMMAND_SET,
                              DJI_RSDK_CMD_FOCUS_MOTOR,
                              request,
                              sizeof(request),
                              timeout_ms,
                              response,
                              sizeof(response),
                              &response_size,
                              NULL);
    if (status != DJI_RSDK_OK) {
        return status;
    }
    if (response_size != sizeof(response) || response[0] != 0x15U ||
        response[1] != 0x00U || response[2] < 0x01U || response[2] > 0x03U) {
        return DJI_RSDK_ERR_PROTOCOL;
    }
    position->calibration_state =
        (dji_rsdk_focus_calibration_state_t)response[2];
    position->position = DJI_RSDK_COMMAND_READ_U32_LE(response + 3U);
    return position->position <= 4095U ? DJI_RSDK_OK
                                       : DJI_RSDK_ERR_PROTOCOL;
}

dji_rsdk_status_t dji_rsdk_camera_action(dji_rsdk_t *rsdk,
                                         dji_rsdk_camera_action_t action,
                                         int timeout_ms) {
    uint8_t data[2];

    if (action != DJI_RSDK_CAMERA_PHOTO_START &&
        action != DJI_RSDK_CAMERA_PHOTO_STOP &&
        action != DJI_RSDK_CAMERA_RECORD_START &&
        action != DJI_RSDK_CAMERA_RECORD_STOP &&
        action != DJI_RSDK_CAMERA_CENTER_FOCUS_START &&
        action != DJI_RSDK_CAMERA_CENTER_FOCUS_STOP) {
        return DJI_RSDK_ERR_PARAM;
    }
    DJI_RSDK_COMMAND_WRITE_U16_LE(data, action);
    return dji_rsdk_commands_ack(rsdk,
                                 DJI_RSDK_CAMERA_COMMAND_SET,
                                 DJI_RSDK_CMD_CAMERA_ACTION,
                                 data,
                                 sizeof(data),
                                 timeout_ms);
}

dji_rsdk_status_t dji_rsdk_camera_get_recording(dji_rsdk_t *rsdk,
                                                int timeout_ms,
                                                int *recording) {
    uint8_t request = 0x01U;
    uint8_t response[1];
    size_t response_size = 0U;
    dji_rsdk_status_t status;

    if (recording == NULL) {
        return DJI_RSDK_ERR_PARAM;
    }
    status = dji_rsdk_request(rsdk,
                              DJI_RSDK_CAMERA_COMMAND_SET,
                              DJI_RSDK_CMD_CAMERA_STATUS,
                              &request,
                              sizeof(request),
                              timeout_ms,
                              response,
                              sizeof(response),
                              &response_size,
                              NULL);
    if (status != DJI_RSDK_OK) {
        return status;
    }
    if (response_size != sizeof(response) ||
        (response[0] != 0x00U && response[0] != 0x02U)) {
        return DJI_RSDK_ERR_PROTOCOL;
    }
    *recording = response[0] == 0x02U;
    return DJI_RSDK_OK;
}
