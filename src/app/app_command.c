#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "app_command.c"

#include "app/app_command.h"

#include <string.h>

#include "common/debug.h"

app_status_t app_command_handle_line(process_common_ctx_t *ctx, const char *line) {
    if (ctx == NULL || line == NULL) {
        return APP_ERR_PARAM;
    }

    if (strcmp(line, "pip on") == 0) {
        ctx->pip_enabled = 1;
        LOGI("command: pip enabled");
        return APP_OK;
    }
    if (strcmp(line, "pip off") == 0) {
        ctx->pip_enabled = 0;
        LOGI("command: pip disabled");
        return APP_OK;
    }

    LOGW("unknown command: %s", line);
    return APP_ERR_UNSUPPORTED;
}
