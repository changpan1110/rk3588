#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "mavlink_transport.c"

#include "control/mavlink/mavlink_transport.h"

#include "common/debug.h"

int mavlink_transport_run(
    const mavlink_control_config_t *config,
    const mavlink_transport_runtime_t *runtime) {
    if (config == NULL || runtime == NULL ||
        runtime->should_stop == NULL || runtime->on_data == NULL) {
        return -1;
    }

    switch (config->transport) {
    case MAVLINK_CONTROL_TRANSPORT_UDP:
        LOGD("MAVLink transport entry -> UDP");
        return mavlink_transport_udp_run(config, runtime);
    case MAVLINK_CONTROL_TRANSPORT_TCP:
        LOGD("MAVLink transport entry -> TCP");
        return mavlink_transport_tcp_run(config, runtime);
    case MAVLINK_CONTROL_TRANSPORT_SERIAL:
        LOGD("MAVLink transport entry -> serial");
        return mavlink_transport_serial_run(config, runtime);
    default:
        LOGE("unknown MAVLink transport: %d", (int)config->transport);
        return -1;
    }
}
