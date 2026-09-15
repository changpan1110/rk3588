/*
 * Copy the relevant parts into the RK3588 UDP receive thread after adding
 * c_library_v2 to the include path. This file is an integration example and
 * is intentionally not added to the current CMake targets.
 */

#include "control/mavlink/mavlink_web_control.h"
#include "control/video_pipeline/video_pipeline_control.h"

typedef struct {
    vp_control_t *video_control;
    const char *selected_channel;
} air_control_context_t;

static uint8_t air_dispatch_web_command(
    const mavlink_web_command_t *command,
    void *user_data) {
    air_control_context_t *ctx = (air_control_context_t *)user_data;
    app_status_t status = APP_ERR_UNSUPPORTED;

    switch (command->action) {
    case MAVLINK_WEB_ACTION_SNAPSHOT:
        status = vp_control_snapshot_async(
            ctx->video_control,
            ctx->selected_channel,
            NULL,
            NULL);
        break;

    case MAVLINK_WEB_ACTION_RECORD_TOGGLE:
        /* Resolve to explicit start/stop from the current recording state. */
        status = APP_ERR_UNSUPPORTED;
        break;

    case MAVLINK_WEB_ACTION_VIDEO_SWITCH:
    case MAVLINK_WEB_ACTION_ZOOM_IN:
    case MAVLINK_WEB_ACTION_ZOOM_OUT:
    case MAVLINK_WEB_ACTION_TRIGGER:
        /* Connect these actions to the existing application command layer. */
        status = APP_ERR_UNSUPPORTED;
        break;

    default:
        status = APP_ERR_PARAM;
        break;
    }

    return status == APP_OK ? MAV_RESULT_ACCEPTED : MAV_RESULT_FAILED;
}

/*
 * Typical receive-loop usage:
 *
 * mavlink_message_t message;
 * mavlink_status_t parser_status;
 * mavlink_message_t ack;
 * mavlink_web_control_t web_control;
 *
 * mavlink_web_control_init(&web_control);
 *
 * for each received UDP byte:
 *   if (mavlink_parse_char(MAVLINK_COMM_0, byte, &message, &parser_status)) {
 *       if (mavlink_web_control_handle(&web_control,
 *                                      &message,
 *                                      1,
 *                                      MAV_COMP_ID_ONBOARD_COMPUTER,
 *                                      air_dispatch_web_command,
 *                                      &context,
 *                                      &ack)) {
 *           uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
 *           uint16_t length = mavlink_msg_to_send_buffer(buffer, &ack);
 *           sendto(socket_fd, buffer, length, 0, peer_address, peer_length);
 *       }
 *   }
 */
