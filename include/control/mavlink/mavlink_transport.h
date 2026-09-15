#ifndef CONTROL_MAVLINK_TRANSPORT_H
#define CONTROL_MAVLINK_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#define MAVLINK_CONTROL_POLL_MS 250
#define MAVLINK_CONTROL_RECONNECT_MS 1000
#define MAVLINK_CONTROL_HOST_MAX 64
#define MAVLINK_CONTROL_DEVICE_MAX 128

typedef enum {
    MAVLINK_CONTROL_TRANSPORT_UDP = 0,
    MAVLINK_CONTROL_TRANSPORT_TCP,
    MAVLINK_CONTROL_TRANSPORT_SERIAL
} mavlink_control_transport_t;

typedef enum {
    MAVLINK_CONTROL_TCP_SERVER = 0,
    MAVLINK_CONTROL_TCP_CLIENT
} mavlink_control_tcp_mode_t;

typedef struct {
    int enabled;
    int debug;
    mavlink_control_transport_t transport;
    uint8_t system_id;
    uint8_t component_id;
    char udp_bind_ip[MAVLINK_CONTROL_HOST_MAX];
    int udp_port;
    mavlink_control_tcp_mode_t tcp_mode;
    char tcp_host[MAVLINK_CONTROL_HOST_MAX];
    int tcp_port;
    char serial_device[MAVLINK_CONTROL_DEVICE_MAX];
    int serial_baud;
} mavlink_control_config_t;

typedef int (*mavlink_transport_stop_fn)(void *opaque);
typedef int (*mavlink_transport_send_fn)(
    void *opaque,
    const uint8_t *data,
    size_t size);
typedef int (*mavlink_transport_data_fn)(
    void *opaque,
    const uint8_t *data,
    size_t size,
    mavlink_transport_send_fn send_fn,
    void *send_opaque);

typedef struct {
    mavlink_transport_stop_fn should_stop;
    mavlink_transport_data_fn on_data;
    void *opaque;
} mavlink_transport_runtime_t;

int mavlink_control_config_load(
    mavlink_control_config_t *config,
    const char *config_path);
const char *mavlink_control_transport_name(
    mavlink_control_transport_t transport);

/* Single transport entry point used by the MAVLink service thread. */
int mavlink_transport_run(
    const mavlink_control_config_t *config,
    const mavlink_transport_runtime_t *runtime);

int mavlink_transport_udp_run(
    const mavlink_control_config_t *config,
    const mavlink_transport_runtime_t *runtime);
int mavlink_transport_tcp_run(
    const mavlink_control_config_t *config,
    const mavlink_transport_runtime_t *runtime);
int mavlink_transport_serial_run(
    const mavlink_control_config_t *config,
    const mavlink_transport_runtime_t *runtime);

#endif
