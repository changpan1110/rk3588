#ifndef SFL0603_LASER_H
#define SFL0603_LASER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SFL0603_LASER_DEFAULT_BAUDRATE 115200U
#define SFL0603_LASER_MAX_DATA_SIZE 10U

typedef enum {
    SFL0603_LASER_OK = 0,
    SFL0603_LASER_NO_DATA = 1,
    SFL0603_LASER_ERR_PARAM = -1,
    SFL0603_LASER_ERR_STATE = -2,
    SFL0603_LASER_ERR_UART = -3,
    SFL0603_LASER_ERR_TIMEOUT = -4,
    SFL0603_LASER_ERR_CHECKSUM = -5,
    SFL0603_LASER_ERR_PROTOCOL = -6
} sfl0603_laser_status_t;

typedef enum {
    SFL0603_LASER_CMD_STANDBY = 0x00,
    SFL0603_LASER_CMD_SINGLE_MEASURE = 0x01,
    SFL0603_LASER_CMD_CONTINUOUS_MEASURE = 0x02,
    SFL0603_LASER_CMD_SELF_CHECK = 0x03,
    SFL0603_LASER_CMD_MIN_DISTANCE = 0x04,
    SFL0603_LASER_CMD_EMISSION_COUNT = 0x06,
    SFL0603_LASER_CMD_TARGET_MODE = 0x22,
    SFL0603_LASER_CMD_BAUDRATE = 0x26
} sfl0603_laser_command_t;

typedef enum {
    SFL0603_LASER_TARGET_SINGLE = 0x0000,
    SFL0603_LASER_TARGET_THREE = 0x0001,
    SFL0603_LASER_TARGET_FIRST_LAST = 0x0010
} sfl0603_laser_target_mode_t;

typedef struct {
    uint8_t command;
    uint8_t data_length;
    uint8_t data[SFL0603_LASER_MAX_DATA_SIZE];
} sfl0603_laser_frame_t;

typedef struct {
    uint8_t flags;
    uint32_t distance_dm[3];
    int main_wave_present;
    int echo_present;
    int laser_ok;
    int timing_ok;
    int apd_ok;
    int front_target_present;
    int rear_target_present;
} sfl0603_laser_measurement_t;

typedef struct {
    uint16_t negative_5v_centi_volts;
    uint16_t blind_zone_m;
    uint8_t apd_high_voltage_v;
    int8_t apd_temperature_c;
    uint16_t positive_5v_centi_volts;
} sfl0603_laser_self_check_t;

sfl0603_laser_status_t sfl0603_laser_init(void);
sfl0603_laser_status_t sfl0603_laser_open(const char *device,
                                          uint32_t baudrate);
void sfl0603_laser_close(void);
int sfl0603_laser_is_open(void);
uint32_t sfl0603_laser_get_baudrate(void);

void sfl0603_laser_set_debug(int enabled);
void sfl0603_laser_set_loghex(int enabled);
int sfl0603_laser_get_debug(void);
int sfl0603_laser_get_loghex(void);

sfl0603_laser_status_t sfl0603_laser_send_command(
    sfl0603_laser_command_t command,
    const uint8_t *data,
    size_t data_length);
sfl0603_laser_status_t sfl0603_laser_read_frame(
    int timeout_ms,
    sfl0603_laser_frame_t *frame);

sfl0603_laser_status_t sfl0603_laser_standby(int timeout_ms);
sfl0603_laser_status_t sfl0603_laser_measure_once(
    int timeout_ms,
    sfl0603_laser_measurement_t *measurement);
sfl0603_laser_status_t sfl0603_laser_start_continuous(
    uint16_t period_ms,
    int timeout_ms,
    sfl0603_laser_measurement_t *first_measurement);
sfl0603_laser_status_t sfl0603_laser_read_measurement(
    int timeout_ms,
    sfl0603_laser_measurement_t *measurement);
sfl0603_laser_status_t sfl0603_laser_run_self_check(
    int timeout_ms,
    sfl0603_laser_self_check_t *result);
sfl0603_laser_status_t sfl0603_laser_set_min_distance(
    uint16_t distance_m,
    int timeout_ms);
sfl0603_laser_status_t sfl0603_laser_query_emission_count(
    int timeout_ms,
    uint32_t *count);
sfl0603_laser_status_t sfl0603_laser_set_target_mode(
    sfl0603_laser_target_mode_t mode,
    int timeout_ms);
/* The module changes the host UART only after receiving the device echo. */
sfl0603_laser_status_t sfl0603_laser_change_baudrate(
    uint32_t new_baudrate,
    int timeout_ms);

double sfl0603_laser_distance_m(uint32_t distance_dm);
sfl0603_laser_status_t sfl0603_laser_self_test(void);
const char *sfl0603_laser_command_name(uint8_t command);
const char *sfl0603_laser_status_string(sfl0603_laser_status_t status);

#ifdef __cplusplus
}
#endif

#endif
