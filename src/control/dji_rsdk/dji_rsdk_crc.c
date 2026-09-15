#include "control/dji_rsdk/dji_rsdk_crc.h"

int dji_rsdk_crc16_verify(const uint8_t *data,
                          size_t size,
                          uint16_t expected_crc) {
    return (data != NULL || size == 0U) &&
           dji_rsdk_crc16_calculate(data, size) == expected_crc;
}

int dji_rsdk_crc32_verify(const uint8_t *data,
                          size_t size,
                          uint32_t expected_crc) {
    return (data != NULL || size == 0U) &&
           dji_rsdk_crc32_calculate(data, size) == expected_crc;
}

int dji_rsdk_crc_self_test(void) {
    static const uint8_t official_packet_without_crc32[] = {
        0xAA, 0x1A, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
        0x22, 0x11, 0xA2, 0x42, 0x0E, 0x00, 0x20, 0x00,
        0x30, 0x00, 0x40, 0x00, 0x01, 0x14
    };

    return dji_rsdk_crc16_verify(official_packet_without_crc32,
                                 10U,
                                 0x42A2U) &&
           dji_rsdk_crc32_verify(official_packet_without_crc32,
                                 sizeof(official_packet_without_crc32),
                                 0xBE97407BU);
}
