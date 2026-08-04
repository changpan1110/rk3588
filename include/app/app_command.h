#ifndef APP_COMMAND_H
#define APP_COMMAND_H

#include "common/common.h"
#include "process/process_common.h"

app_status_t app_command_handle_line(process_common_ctx_t *ctx, const char *line);

#endif
