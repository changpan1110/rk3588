#include "control/mavlink/mavlink_web_control.h"

#include <math.h>
#include <string.h>

static int mavlink_web_float_to_u16(float value, uint16_t *out) {
    float rounded;

    if (out == NULL || !isfinite(value) || value < 0.0f || value > 65535.0f) {
        return 0;
    }

    rounded = floorf(value);
    if (rounded != value) {
        return 0;
    }

    *out = (uint16_t)rounded;
    return 1;
}

static int mavlink_web_action_is_hold(mavlink_web_action_t action) {
    return action == MAVLINK_WEB_ACTION_ZOOM_IN ||
           action == MAVLINK_WEB_ACTION_ZOOM_OUT ||
           action == MAVLINK_WEB_ACTION_FOCUS_IN ||
           action == MAVLINK_WEB_ACTION_FOCUS_OUT ||
           action == MAVLINK_WEB_ACTION_GIMBAL_UP ||
           action == MAVLINK_WEB_ACTION_GIMBAL_DOWN ||
           action == MAVLINK_WEB_ACTION_GIMBAL_LEFT ||
           action == MAVLINK_WEB_ACTION_GIMBAL_RIGHT;
}

static int mavlink_web_decode_command(
    const mavlink_message_t *message,
    uint8_t local_system,
    uint8_t local_component,
    mavlink_web_command_t *out) {
    mavlink_command_long_t payload;
    uint16_t values[7];

    if (message == NULL || out == NULL || message->msgid != MAVLINK_MSG_ID_COMMAND_LONG) {
        return 0;
    }

    mavlink_msg_command_long_decode(message, &payload);
    if (payload.command != MAV_CMD_USER_1) {
        return 0;
    }

    if (payload.target_system != 0U && payload.target_system != local_system) {
        return 0;
    }
    if (payload.target_component != 0U && payload.target_component != local_component) {
        return 0;
    }

    if (!mavlink_web_float_to_u16(payload.param1, &values[0]) ||
        !mavlink_web_float_to_u16(payload.param2, &values[1]) ||
        !mavlink_web_float_to_u16(payload.param3, &values[2]) ||
        !mavlink_web_float_to_u16(payload.param4, &values[3]) ||
        !mavlink_web_float_to_u16(payload.param5, &values[4]) ||
        !mavlink_web_float_to_u16(payload.param6, &values[5]) ||
        !mavlink_web_float_to_u16(payload.param7, &values[6])) {
        return 0;
    }

    memset(out, 0, sizeof(*out));
    {
        uint16_t control_word = values[0];
        uint16_t action;
        uint16_t event;
        uint16_t value;

        if ((control_word & 0x8000U) != 0U &&
            (control_word & 0x0060U) != 0U) {
            action = control_word & 0x001FU;
            event = (control_word >> 5U) & 0x0003U;
            value = (control_word >> 7U) & 0x00FFU;
        } else {
            action = control_word & 0x00FFU;
            event = (control_word >> 8U) & 0x0003U;
            value = (control_word >> 10U) & 0x003FU;
        }

        if (action < MAVLINK_WEB_ACTION_VIDEO_SWITCH ||
            action > MAVLINK_WEB_ACTION_GIMBAL_CENTER ||
            event > MAVLINK_WEB_EVENT_RELEASE) {
            return 0;
        }
        out->action = (mavlink_web_action_t)action;
        out->event = (mavlink_web_event_t)event;
        out->value = (uint8_t)value;
    }

    if (mavlink_web_action_is_hold(out->action) &&
        out->event == MAVLINK_WEB_EVENT_CLICK) {
        return 0;
    }
    if (!mavlink_web_action_is_hold(out->action) &&
        out->event != MAVLINK_WEB_EVENT_CLICK) {
        return 0;
    }

    out->source_system = message->sysid;
    out->source_component = message->compid;
    out->sequence = (uint32_t)values[1] | ((uint32_t)values[2] << 16);
    out->command_uid = (uint64_t)values[3] |
                       ((uint64_t)values[4] << 16) |
                       ((uint64_t)values[5] << 32) |
                       ((uint64_t)values[6] << 48);
    return 1;
}

static int mavlink_web_find_cached_result(
    const mavlink_web_control_t *control,
    const mavlink_web_command_t *command,
    uint8_t *result) {
    size_t i;

    for (i = 0; i < MAVLINK_WEB_CONTROL_CACHE_SIZE; ++i) {
        const mavlink_web_dedupe_entry_t *entry = &control->cache[i];
        if (entry->valid &&
            entry->source_system == command->source_system &&
            entry->source_component == command->source_component &&
            entry->command_uid == command->command_uid) {
            *result = entry->result;
            return 1;
        }
    }

    return 0;
}

static void mavlink_web_cache_result(
    mavlink_web_control_t *control,
    const mavlink_web_command_t *command,
    uint8_t result) {
    mavlink_web_dedupe_entry_t *entry = &control->cache[control->next_cache_index];

    entry->valid = 1U;
    entry->source_system = command->source_system;
    entry->source_component = command->source_component;
    entry->result = result;
    entry->command_uid = command->command_uid;
    control->next_cache_index =
        (control->next_cache_index + 1U) % MAVLINK_WEB_CONTROL_CACHE_SIZE;
}

static void mavlink_web_make_ack(
    const mavlink_web_command_t *command,
    uint8_t local_system,
    uint8_t local_component,
    uint8_t result,
    mavlink_message_t *ack_out) {
    mavlink_msg_command_ack_pack(
        local_system,
        local_component,
        ack_out,
        MAV_CMD_USER_1,
        result,
        result == MAV_RESULT_ACCEPTED ? 100U : 0U,
        (int32_t)(command->command_uid & 0xFFFFFFFFU),
        command->source_system,
        command->source_component);
}

void mavlink_web_control_init(mavlink_web_control_t *control) {
    if (control != NULL) {
        memset(control, 0, sizeof(*control));
    }
}

int mavlink_web_control_handle_ex(
    mavlink_web_control_t *control,
    const mavlink_message_t *message,
    uint8_t local_system,
    uint8_t local_component,
    mavlink_web_command_handler_t handler,
    void *user_data,
    mavlink_message_t *ack_out,
    mavlink_web_handle_info_t *info_out) {
    mavlink_web_command_t command;
    uint8_t result;
    int duplicate;

    if (control == NULL || handler == NULL || ack_out == NULL) {
        return 0;
    }

    if (!mavlink_web_decode_command(message, local_system, local_component, &command)) {
        return 0;
    }

    duplicate = mavlink_web_find_cached_result(control, &command, &result);
    if (!duplicate) {
        result = handler(&command, user_data);
        mavlink_web_cache_result(control, &command, result);
    }

    mavlink_web_make_ack(&command, local_system, local_component, result, ack_out);
    if (info_out != NULL) {
        info_out->command = command;
        info_out->result = result;
        info_out->duplicate = duplicate ? 1U : 0U;
    }
    return 1;
}

int mavlink_web_control_handle(
    mavlink_web_control_t *control,
    const mavlink_message_t *message,
    uint8_t local_system,
    uint8_t local_component,
    mavlink_web_command_handler_t handler,
    void *user_data,
    mavlink_message_t *ack_out) {
    return mavlink_web_control_handle_ex(
        control,
        message,
        local_system,
        local_component,
        handler,
        user_data,
        ack_out,
        NULL);
}
