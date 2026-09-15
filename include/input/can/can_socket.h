#ifndef INPUT_CAN_SOCKET_H
#define INPUT_CAN_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_SOCKET_INTERFACE_NAME_MAX 16U
#define CAN_SOCKET_CLASSIC_DATA_MAX 8U

typedef enum {
    CAN_SOCKET_OK = 0,
    CAN_SOCKET_NO_DATA = 1,
    CAN_SOCKET_ERR_PARAM = -1,
    CAN_SOCKET_ERR_STATE = -2,
    CAN_SOCKET_ERR_SYSTEM = -3,
    CAN_SOCKET_ERR_TIMEOUT = -4,
    CAN_SOCKET_ERR_PROTOCOL = -5
} can_socket_status_t;

typedef struct {
    int fd;
    uint32_t tx_id;
    uint32_t rx_id;
    int last_system_error;
    int log_frames;
    char interface_name[CAN_SOCKET_INTERFACE_NAME_MAX];
} can_socket_t;

void can_socket_init(can_socket_t *socket_can);
can_socket_status_t can_socket_open(can_socket_t *socket_can,
                                    const char *interface_name,
                                    uint32_t tx_id,
                                    uint32_t rx_id);
void can_socket_close(can_socket_t *socket_can);
int can_socket_is_open(const can_socket_t *socket_can);
void can_socket_set_log_frames(can_socket_t *socket_can, int enabled);
int can_socket_last_system_error(const can_socket_t *socket_can);

can_socket_status_t can_socket_send(can_socket_t *socket_can,
                                    const uint8_t *data,
                                    size_t size,
                                    int timeout_ms);
can_socket_status_t can_socket_receive(can_socket_t *socket_can,
                                       uint8_t data[CAN_SOCKET_CLASSIC_DATA_MAX],
                                       size_t *size,
                                       int timeout_ms);

const char *can_socket_status_string(can_socket_status_t status);

#ifdef __cplusplus
}
#endif

#endif
