#ifndef CONTROL_DJI_RSDK_CRC_H
#define CONTROL_DJI_RSDK_CRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DJI R SDK Protocol and User Interface v2.5, section 3.2. */
#define DJI_RSDK_CRC16_POLYNOMIAL 0x8005U
#define DJI_RSDK_CRC16_INITIAL 0xC55CU
#define DJI_RSDK_CRC16_XOR_OUT 0x0000U

#define DJI_RSDK_CRC32_POLYNOMIAL 0x04C11DB7U
#define DJI_RSDK_CRC32_INITIAL 0xC55C0000U
#define DJI_RSDK_CRC32_XOR_OUT 0x00000000U

uint16_t dji_rsdk_crc16_calculate(const uint8_t *data, size_t size);
uint32_t dji_rsdk_crc32_calculate(const uint8_t *data, size_t size);

int dji_rsdk_crc16_verify(const uint8_t *data,
                          size_t size,
                          uint16_t expected_crc);
int dji_rsdk_crc32_verify(const uint8_t *data,
                          size_t size,
                          uint32_t expected_crc);

int dji_rsdk_crc_self_test(void);

#ifdef __cplusplus
}
#endif

#endif
