#define crc_t dji_rsdk_custom_crc32_t
#define crc_init dji_rsdk_custom_crc32_init
#define crc_update dji_rsdk_custom_crc32_update
#define crc_finalize dji_rsdk_custom_crc32_finalize

#include "custom_crc32.c"

#include "control/dji_rsdk/dji_rsdk_crc.h"

uint32_t dji_rsdk_crc32_calculate(const uint8_t *data, size_t size) {
    dji_rsdk_custom_crc32_t crc;

    if (data == NULL && size > 0U) {
        return 0U;
    }
    crc = dji_rsdk_custom_crc32_init();
    crc = dji_rsdk_custom_crc32_update(crc, data, size);
    crc = dji_rsdk_custom_crc32_finalize(crc);
    return (uint32_t)crc;
}
