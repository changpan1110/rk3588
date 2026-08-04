#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "debug.c"

#include "common/debug.h"

#include <stdarg.h>
#include <string.h>
#include <time.h>

static const char *log_level_name(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_TRACE: return "TRACE";
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO: return "INFO ";
        case LOG_LEVEL_WARN: return "WARN ";
        case LOG_LEVEL_ERROR: return "ERROR";
        default: return "UNKWN";
    }
}

static const char *log_level_color(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_TRACE: return "\x1b[90m";
        case LOG_LEVEL_DEBUG: return "\x1b[36m";
        case LOG_LEVEL_INFO: return "\x1b[32m";
        case LOG_LEVEL_WARN: return "\x1b[33m";
        case LOG_LEVEL_ERROR: return "\x1b[31m";
        default: return "\x1b[0m";
    }
}

static const char *log_file_basename(const char *file) {
    const char *slash;
    const char *backslash;

    if (file == NULL) {
        return "unknown";
    }

    slash = strrchr(file, '/');
    backslash = strrchr(file, '\\');

    if (slash == NULL && backslash == NULL) {
        return file;
    }
    if (slash == NULL) {
        return backslash + 1;
    }
    if (backslash == NULL) {
        return slash + 1;
    }
    return (slash > backslash ? slash : backslash) + 1;
}

void log_write(log_level_t level, const char *file, int line, const char *func, const char *fmt, ...) {
    char time_buf[32];
    time_t now;
    struct tm tm_now;
    va_list ap;
    const char *short_file;

    now = time(NULL);
    localtime_r(&now, &tm_now);
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm_now);
    short_file = log_file_basename(file);

    fprintf(stderr, "%s[%s][%s][%s:%d][%s] ",
        log_level_color(level),
        time_buf,
        log_level_name(level),
        short_file,
        line,
        func);

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\x1b[0m\n");
}
