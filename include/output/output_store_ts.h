#ifndef OUTPUT_STORE_TS_H
#define OUTPUT_STORE_TS_H

#include "common/common.h"

typedef struct {
    char path[APP_PATH_MAX_LEN];
} output_store_ts_ctx_t;

app_status_t output_store_ts_init(output_store_ts_ctx_t *ctx, const char *path);
app_status_t output_store_ts_write(output_store_ts_ctx_t *ctx, const uint8_t *data, size_t size);
void output_store_ts_deinit(output_store_ts_ctx_t *ctx);

#endif
