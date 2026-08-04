#include "video_pipeline_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

int vp_mkdir_p(const char *dir) {
    char tmp[APP_PATH_MAX_LEN];
    char *s;
    size_t len;

    if (dir == NULL || dir[0] == '\0') {
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s", dir);
    len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (s = tmp + 1; *s != '\0'; ++s) {
        if (*s == '/') {
            *s = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *s = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

int vp_make_dated_path(const char *base_dir,
                       char *out,
                       size_t out_size,
                       const char *ext,
                       time_t now) {
    struct tm tm_now;
    char date[16];
    char stamp[32];
    char dated_dir[APP_PATH_MAX_LEN];
    int written;

    if (base_dir == NULL || base_dir[0] == '\0' || out == NULL || out_size == 0 ||
        ext == NULL || ext[0] == '\0') {
        return -1;
    }
    localtime_r(&now, &tm_now);
    strftime(date, sizeof(date), "%Y%m%d", &tm_now);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_now);
    written = snprintf(dated_dir, sizeof(dated_dir), "%s/%s", base_dir, date);
    if (written < 0 || (size_t)written >= sizeof(dated_dir) || vp_mkdir_p(dated_dir) != 0) {
        return -1;
    }
    written = snprintf(out, out_size, "%s/%s%s", dated_dir, stamp, ext);
    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }
    return 0;
}
