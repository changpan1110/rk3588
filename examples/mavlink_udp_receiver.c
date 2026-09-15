/*
 * Standalone RK3588 MAVLink UDP receiver example.
 *
 * This uses the official MAVLink c_library_v2 parser and message definitions.
 * The application-specific mapping of COMMAND_LONG parameters is documented
 * in WEB_MAVLINK_IMPLEMENTATION_GUIDE.md.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mavlink/common/mavlink.h"

#define DEFAULT_UDP_PORT 14550
#define LOCAL_SYSTEM_ID 1U
#define LOCAL_COMPONENT_ID MAV_COMP_ID_ONBOARD_COMPUTER
#define DEDUPE_CACHE_SIZE 64U

typedef enum {
    WEB_ACTION_VIDEO_SWITCH = 1,
    WEB_ACTION_SNAPSHOT = 2,
    WEB_ACTION_RECORD_TOGGLE = 3,
    WEB_ACTION_ZOOM_IN = 4,
    WEB_ACTION_ZOOM_OUT = 5,
    WEB_ACTION_TRIGGER = 6
} web_action_t;

typedef struct {
    uint8_t source_system;
    uint8_t source_component;
    web_action_t action;
    uint32_t sequence;
    uint64_t command_uid;
} web_command_t;

typedef struct {
    int valid;
    uint8_t source_system;
    uint8_t source_component;
    uint8_t result;
    uint64_t command_uid;
} dedupe_entry_t;

static volatile sig_atomic_t g_running = 1;
static dedupe_entry_t g_dedupe_cache[DEDUPE_CACHE_SIZE];
static size_t g_next_cache_entry = 0U;

static void handle_signal(int signal_number) {
    (void)signal_number;
    g_running = 0;
}

static const char *action_name(web_action_t action) {
    switch (action) {
    case WEB_ACTION_VIDEO_SWITCH:
        return "video.switch";
    case WEB_ACTION_SNAPSHOT:
        return "camera.snapshot";
    case WEB_ACTION_RECORD_TOGGLE:
        return "record.toggle";
    case WEB_ACTION_ZOOM_IN:
        return "camera.zoom_in";
    case WEB_ACTION_ZOOM_OUT:
        return "camera.zoom_out";
    case WEB_ACTION_TRIGGER:
        return "device.trigger";
    default:
        return "unknown";
    }
}

static int float_to_u16(float value, uint16_t *out_value) {
    if (out_value == NULL || !isfinite(value) || value < 0.0f || value > 65535.0f) {
        return 0;
    }
    if (floorf(value) != value) {
        return 0;
    }
    *out_value = (uint16_t)value;
    return 1;
}

/*
 * Return values:
 *   0: not an RK3588 web-control message
 *   1: valid command decoded
 *  -1: matching command with invalid application parameters
 */
static int decode_web_command(const mavlink_message_t *message, web_command_t *out_command) {
    mavlink_command_long_t payload;
    uint16_t values[7];

    if (message == NULL || out_command == NULL ||
        message->msgid != MAVLINK_MSG_ID_COMMAND_LONG) {
        return 0;
    }

    mavlink_msg_command_long_decode(message, &payload);
    if (payload.command != MAV_CMD_USER_1) {
        return 0;
    }
    if (payload.target_system != 0U && payload.target_system != LOCAL_SYSTEM_ID) {
        return 0;
    }
    if (payload.target_component != 0U &&
        payload.target_component != LOCAL_COMPONENT_ID) {
        return 0;
    }

    memset(out_command, 0, sizeof(*out_command));
    out_command->source_system = message->sysid;
    out_command->source_component = message->compid;

    if (!float_to_u16(payload.param1, &values[0]) ||
        !float_to_u16(payload.param2, &values[1]) ||
        !float_to_u16(payload.param3, &values[2]) ||
        !float_to_u16(payload.param4, &values[3]) ||
        !float_to_u16(payload.param5, &values[4]) ||
        !float_to_u16(payload.param6, &values[5]) ||
        !float_to_u16(payload.param7, &values[6])) {
        return -1;
    }

    if (values[0] < WEB_ACTION_VIDEO_SWITCH || values[0] > WEB_ACTION_TRIGGER) {
        return -1;
    }

    out_command->action = (web_action_t)values[0];
    out_command->sequence =
        (uint32_t)values[1] | ((uint32_t)values[2] << 16);
    out_command->command_uid =
        (uint64_t)values[3] |
        ((uint64_t)values[4] << 16) |
        ((uint64_t)values[5] << 32) |
        ((uint64_t)values[6] << 48);
    return 1;
}

static int find_cached_result(const web_command_t *command, uint8_t *out_result) {
    size_t index;

    for (index = 0U; index < DEDUPE_CACHE_SIZE; ++index) {
        const dedupe_entry_t *entry = &g_dedupe_cache[index];
        if (entry->valid &&
            entry->source_system == command->source_system &&
            entry->source_component == command->source_component &&
            entry->command_uid == command->command_uid) {
            *out_result = entry->result;
            return 1;
        }
    }
    return 0;
}

static void cache_result(const web_command_t *command, uint8_t result) {
    dedupe_entry_t *entry = &g_dedupe_cache[g_next_cache_entry];

    entry->valid = 1;
    entry->source_system = command->source_system;
    entry->source_component = command->source_component;
    entry->command_uid = command->command_uid;
    entry->result = result;
    g_next_cache_entry = (g_next_cache_entry + 1U) % DEDUPE_CACHE_SIZE;
}

static uint8_t execute_command(const web_command_t *command) {
    /*
     * Replace this switch with calls into the real video application, for
     * example vp_control_snapshot_async() or vp_control_stream_select_async().
     */
    switch (command->action) {
    case WEB_ACTION_VIDEO_SWITCH:
    case WEB_ACTION_SNAPSHOT:
    case WEB_ACTION_RECORD_TOGGLE:
    case WEB_ACTION_ZOOM_IN:
    case WEB_ACTION_ZOOM_OUT:
    case WEB_ACTION_TRIGGER:
        printf(
            "EXECUTE action=%s sequence=%" PRIu32 " uid=%016" PRIx64 "\n",
            action_name(command->action),
            command->sequence,
            command->command_uid);
        return MAV_RESULT_ACCEPTED;
    default:
        return MAV_RESULT_UNSUPPORTED;
    }
}

static int send_command_ack(
    int socket_fd,
    const struct sockaddr_in *peer_address,
    socklen_t peer_length,
    const web_command_t *command,
    uint8_t result) {
    mavlink_message_t ack_message;
    uint8_t send_buffer[MAVLINK_MAX_PACKET_LEN];
    uint16_t send_length;
    ssize_t written;

    mavlink_msg_command_ack_pack(
        LOCAL_SYSTEM_ID,
        LOCAL_COMPONENT_ID,
        &ack_message,
        MAV_CMD_USER_1,
        result,
        result == MAV_RESULT_ACCEPTED ? 100U : 0U,
        0,
        command->source_system,
        command->source_component);

    send_length = mavlink_msg_to_send_buffer(send_buffer, &ack_message);
    written = sendto(
        socket_fd,
        send_buffer,
        send_length,
        0,
        (const struct sockaddr *)peer_address,
        peer_length);
    if (written != (ssize_t)send_length) {
        perror("sendto COMMAND_ACK");
        return -1;
    }
    return 0;
}

static void process_message(
    int socket_fd,
    const struct sockaddr_in *peer_address,
    socklen_t peer_length,
    const mavlink_message_t *message) {
    web_command_t command;
    uint8_t result;
    int duplicate = 0;
    int decode_result = decode_web_command(message, &command);

    if (decode_result == 0) {
        return;
    }

    if (decode_result < 0) {
        fprintf(
            stderr,
            "Rejected invalid MAV_CMD_USER_1 from system=%u component=%u\n",
            command.source_system,
            command.source_component);
        result = MAV_RESULT_FAILED;
    } else if (find_cached_result(&command, &result)) {
        duplicate = 1;
    } else {
        result = execute_command(&command);
        cache_result(&command, result);
    }

    printf(
        "RECEIVED source=%u/%u action=%s sequence=%" PRIu32
        " uid=%016" PRIx64 " duplicate=%d result=%u\n",
        command.source_system,
        command.source_component,
        action_name(command.action),
        command.sequence,
        command.command_uid,
        duplicate,
        result);

    (void)send_command_ack(
        socket_fd,
        peer_address,
        peer_length,
        &command,
        result);
}

int main(int argc, char **argv) {
    int socket_fd;
    int reuse_address = 1;
    int udp_port = DEFAULT_UDP_PORT;
    struct sockaddr_in local_address;
    mavlink_message_t message;
    mavlink_status_t parser_status;
    uint8_t receive_buffer[2048];

    if (argc > 1) {
        udp_port = atoi(argv[1]);
        if (udp_port < 1 || udp_port > 65535) {
            fprintf(stderr, "Invalid UDP port: %s\n", argv[1]);
            return EXIT_FAILURE;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    setvbuf(stdout, NULL, _IOLBF, 0);
    memset(&parser_status, 0, sizeof(parser_status));

    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    if (setsockopt(
            socket_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(socket_fd);
        return EXIT_FAILURE;
    }

    memset(&local_address, 0, sizeof(local_address));
    local_address.sin_family = AF_INET;
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    local_address.sin_port = htons((uint16_t)udp_port);

    if (bind(
            socket_fd,
            (const struct sockaddr *)&local_address,
            sizeof(local_address)) < 0) {
        perror("bind");
        close(socket_fd);
        return EXIT_FAILURE;
    }

    printf(
        "Listening for MAVLink UDP on 0.0.0.0:%d as system=%u component=%u\n",
        udp_port,
        LOCAL_SYSTEM_ID,
        LOCAL_COMPONENT_ID);

    while (g_running) {
        struct sockaddr_in peer_address;
        socklen_t peer_length = sizeof(peer_address);
        ssize_t received;
        ssize_t index;

        received = recvfrom(
            socket_fd,
            receive_buffer,
            sizeof(receive_buffer),
            0,
            (struct sockaddr *)&peer_address,
            &peer_length);
        if (received < 0) {
            if (errno == EINTR && !g_running) {
                break;
            }
            perror("recvfrom");
            continue;
        }

        for (index = 0; index < received; ++index) {
            if (mavlink_parse_char(
                    MAVLINK_COMM_0,
                    receive_buffer[index],
                    &message,
                    &parser_status)) {
                process_message(
                    socket_fd,
                    &peer_address,
                    peer_length,
                    &message);
            }
        }
    }

    close(socket_fd);
    printf("Receiver stopped.\n");
    return EXIT_SUCCESS;
}
