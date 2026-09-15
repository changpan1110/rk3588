#define crc_t dji_rsdk_custom_crc16_t
#define crc_init dji_rsdk_custom_crc16_init
#define crc_update dji_rsdk_custom_crc16_update
#define crc_finalize dji_rsdk_custom_crc16_finalize

#include "custom_crc16.c"

#include "control/dji_rsdk/dji_rsdk_crc.h"

uint16_t dji_rsdk_crc16_calculate(const uint8_t *data, size_t size) {
    dji_rsdk_custom_crc16_t crc;

    if (data == NULL && size > 0U) {
        return 0U;
    }
    crc = dji_rsdk_custom_crc16_init();
    crc = dji_rsdk_custom_crc16_update(crc, data, size);
    crc = dji_rsdk_custom_crc16_finalize(crc);
    return (uint16_t)crc;
}
