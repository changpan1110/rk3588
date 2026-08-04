#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>

typedef enum {
    LOG_LEVEL_TRACE = 0,
    LOG_LEVEL_DEBUG = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_WARN = 3,
    LOG_LEVEL_ERROR = 4
} log_level_t;

#ifndef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL LOG_LEVEL_INFO
#endif

#ifndef LOG_FILE_NAME
#define LOG_FILE_NAME __FILE__
#endif

void log_write(log_level_t level, const char *file, int line, const char *func, const char *fmt, ...);

#define LOGT(fmt, ...) do { if (LOG_LOCAL_LEVEL <= LOG_LEVEL_TRACE) log_write(LOG_LEVEL_TRACE, LOG_FILE_NAME, __LINE__, __func__, fmt, ##__VA_ARGS__); } while (0)
#define LOGD(fmt, ...) do { if (LOG_LOCAL_LEVEL <= LOG_LEVEL_DEBUG) log_write(LOG_LEVEL_DEBUG, LOG_FILE_NAME, __LINE__, __func__, fmt, ##__VA_ARGS__); } while (0)
#define LOGI(fmt, ...) do { if (LOG_LOCAL_LEVEL <= LOG_LEVEL_INFO)  log_write(LOG_LEVEL_INFO,  LOG_FILE_NAME, __LINE__, __func__, fmt, ##__VA_ARGS__); } while (0)
#define LOGW(fmt, ...) do { if (LOG_LOCAL_LEVEL <= LOG_LEVEL_WARN)  log_write(LOG_LEVEL_WARN,  LOG_FILE_NAME, __LINE__, __func__, fmt, ##__VA_ARGS__); } while (0)
#define LOGE(fmt, ...) do { if (LOG_LOCAL_LEVEL <= LOG_LEVEL_ERROR) log_write(LOG_LEVEL_ERROR, LOG_FILE_NAME, __LINE__, __func__, fmt, ##__VA_ARGS__); } while (0)

#endif
