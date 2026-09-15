#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "debug.c"

#include "common/debug.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static atomic_int g_log_level = ATOMIC_VAR_INIT(LOG_LEVEL_DEBUG);
static pthread_once_t g_log_level_once = PTHREAD_ONCE_INIT;

static void log_init_level_from_env(void) {
    const char *value = getenv("RK3588_LOG_LEVEL");
    log_level_t level = LOG_LEVEL_DEBUG;

    if (value == NULL || value[0] == '\0') {
        return;
    }
    if (strcasecmp(value, "TRACE") == 0) {
        level = LOG_LEVEL_TRACE;
    } else if (strcasecmp(value, "DEBUG") == 0) {
        level = LOG_LEVEL_DEBUG;
    } else if (strcasecmp(value, "INFO") == 0) {
        level = LOG_LEVEL_INFO;
    } else if (strcasecmp(value, "WARN") == 0 ||
               strcasecmp(value, "WARNING") == 0) {
        level = LOG_LEVEL_WARN;
    } else if (strcasecmp(value, "ERROR") == 0) {
        level = LOG_LEVEL_ERROR;
    } else if (strcasecmp(value, "OFF") == 0) {
        level = LOG_LEVEL_OFF;
    } else {
        return;
    }
    atomic_store_explicit(&g_log_level, level, memory_order_relaxed);
}

void log_set_level(log_level_t level) {
    pthread_once(&g_log_level_once, log_init_level_from_env);
    if (level < LOG_LEVEL_TRACE || level > LOG_LEVEL_OFF) {
        return;
    }
    atomic_store_explicit(&g_log_level, level, memory_order_relaxed);
}

log_level_t log_get_level(void) {
    pthread_once(&g_log_level_once, log_init_level_from_env);
    return (log_level_t)atomic_load_explicit(&g_log_level, memory_order_relaxed);
}

int log_is_enabled(log_level_t level) {
    return level >= LOG_LEVEL_TRACE &&
           level < LOG_LEVEL_OFF &&
           level >= log_get_level();
}

static const char *log_level_name(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_TRACE: return "TRACE";
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO: return "INFO ";
        case LOG_LEVEL_WARN: return "WARN ";
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_OFF: return "OFF  ";
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
        case LOG_LEVEL_OFF: return "\x1b[0m";
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

    if (!log_is_enabled(level)) {
        return;
    }

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
