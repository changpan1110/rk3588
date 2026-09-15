#define _POSIX_C_SOURCE 200809L
#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "dji_rsdk_service.c"

#include "service/dji_rsdk_service.h"

#include "common/common.h"
#include "common/debug.h"
#include "control/dji_rsdk/dji_rsdk.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DJI_RSDK_OSD_CAN_INTERFACE "can0"
#define DJI_RSDK_OSD_UPDATE_HZ 10U
#define DJI_RSDK_OSD_REQUEST_TIMEOUT_MS \
    ((1000U + DJI_RSDK_OSD_UPDATE_HZ - 1U) / DJI_RSDK_OSD_UPDATE_HZ)
#define DJI_RSDK_NANOSECONDS_PER_SECOND 1000000000L
#define DJI_RSDK_OSD_UPDATE_PERIOD_NS \
    (DJI_RSDK_NANOSECONDS_PER_SECOND / DJI_RSDK_OSD_UPDATE_HZ)
#define DJI_RSDK_GIMBAL_COMMAND_TIMEOUT_MS 300
#define DJI_RSDK_GIMBAL_WATCHDOG_MS 1200

#if DJI_RSDK_OSD_UPDATE_HZ == 0U
#error "DJI_RSDK_OSD_UPDATE_HZ must be greater than zero"
#endif

typedef struct {
    pthread_mutex_t lock;
    pthread_t thread;
    int thread_started;
    int stop_requested;
    int initialized;
    dji_rsdk_t rsdk;
    vp_control_t *control;
    int direction_pressed[4];
    uint8_t speed_tenths_degree_per_second;
    uint64_t control_generation;
    uint64_t applied_generation;
    int center_requested;
    struct timespec last_control_update;
} dji_rsdk_osd_service_context_t;

static dji_rsdk_osd_service_context_t g_dji_rsdk_osd_service = {
    .lock = PTHREAD_MUTEX_INITIALIZER
};

static int dji_rsdk_osd_service_stop_requested(
    dji_rsdk_osd_service_context_t *service) {
    int stop_requested;

    pthread_mutex_lock(&service->lock);
    stop_requested = service->stop_requested;
    pthread_mutex_unlock(&service->lock);
    return stop_requested;
}

static void dji_rsdk_osd_timespec_add_ns(struct timespec *value,
                                         long nanoseconds) {
    value->tv_nsec += nanoseconds;
    while (value->tv_nsec >= DJI_RSDK_NANOSECONDS_PER_SECOND) {
        value->tv_nsec -= DJI_RSDK_NANOSECONDS_PER_SECOND;
        ++value->tv_sec;
    }
}

static long long dji_rsdk_elapsed_ms(const struct timespec *from,
                                     const struct timespec *to) {
    return (long long)(to->tv_sec - from->tv_sec) * 1000LL +
           (long long)(to->tv_nsec - from->tv_nsec) / 1000000LL;
}

static int dji_rsdk_any_direction_pressed(
    const dji_rsdk_osd_service_context_t *service) {
    int direction;

    for (direction = 0; direction < 4; ++direction) {
        if (service->direction_pressed[direction]) {
            return 1;
        }
    }
    return 0;
}

static void dji_rsdk_apply_pending_gimbal_control(
    dji_rsdk_osd_service_context_t *service) {
    struct timespec now;
    uint64_t generation;
    int center_requested;
    int up;
    int down;
    int left;
    int right;
    uint8_t speed;
    int watchdog_stop = 0;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return;
    }

    pthread_mutex_lock(&service->lock);
    if (dji_rsdk_any_direction_pressed(service) &&
        dji_rsdk_elapsed_ms(&service->last_control_update, &now) >
            DJI_RSDK_GIMBAL_WATCHDOG_MS) {
        memset(service->direction_pressed, 0, sizeof(service->direction_pressed));
        service->control_generation++;
        watchdog_stop = 1;
    }
    generation = service->control_generation;
    if (generation == service->applied_generation) {
        pthread_mutex_unlock(&service->lock);
        return;
    }
    service->applied_generation = generation;
    center_requested = service->center_requested;
    service->center_requested = 0;
    up = service->direction_pressed[DJI_RSDK_GIMBAL_DIRECTION_UP];
    down = service->direction_pressed[DJI_RSDK_GIMBAL_DIRECTION_DOWN];
    left = service->direction_pressed[DJI_RSDK_GIMBAL_DIRECTION_LEFT];
    right = service->direction_pressed[DJI_RSDK_GIMBAL_DIRECTION_RIGHT];
    speed = service->speed_tenths_degree_per_second;
    pthread_mutex_unlock(&service->lock);

    if (center_requested) {
        dji_rsdk_status_t status = dji_rsdk_set_position(
            &service->rsdk,
            0,
            0,
            0,
            1,
            1,
            1,
            1,
            10,
            DJI_RSDK_GIMBAL_COMMAND_TIMEOUT_MS);
        if (status != DJI_RSDK_OK) {
            LOGW("DJI RSDK gimbal center failed: %s",
                 dji_rsdk_status_string(status));
        } else {
            LOGI("DJI RSDK gimbal center position yaw=0 roll=0 pitch=0");
        }
        return;
    }

    {
        int16_t yaw_speed = (int16_t)((right - left) * (int)speed);
        int16_t pitch_speed = (int16_t)((up - down) * (int)speed);
        dji_rsdk_status_t status = dji_rsdk_set_speed(
            &service->rsdk,
            yaw_speed,
            0,
            pitch_speed,
            1,
            1,
            DJI_RSDK_GIMBAL_COMMAND_TIMEOUT_MS);
        if (status != DJI_RSDK_OK) {
            LOGW("DJI RSDK gimbal speed failed yaw=%d pitch=%d: %s",
                 yaw_speed,
                 pitch_speed,
                 dji_rsdk_status_string(status));
        } else if (watchdog_stop) {
            LOGW("DJI RSDK gimbal watchdog stopped motion after %u ms",
                 (unsigned)DJI_RSDK_GIMBAL_WATCHDOG_MS);
        } else {
            LOGD("DJI RSDK gimbal speed yaw=%d pitch=%d", yaw_speed, pitch_speed);
        }
    }
}

static void *dji_rsdk_osd_service_thread(void *opaque) {
    dji_rsdk_osd_service_context_t *service =
        (dji_rsdk_osd_service_context_t *)opaque;
    dji_rsdk_status_t previous_status = DJI_RSDK_NO_DATA;
    struct timespec next_update;

    if (clock_gettime(CLOCK_MONOTONIC, &next_update) != 0) {
        LOGE("DJI RSDK telemetry clock initialization failed: %s",
             strerror(errno));
        vp_control_osd_set_gimbal_angles(service->control, 0.0f, 0.0f, 0);
        dji_rsdk_close(&service->rsdk);
        return NULL;
    }

    while (!dji_rsdk_osd_service_stop_requested(service)) {
        dji_rsdk_angles_t angles;
        dji_rsdk_status_t rsdk_status;
        app_status_t osd_status;

        dji_rsdk_apply_pending_gimbal_control(service);
        rsdk_status = dji_rsdk_get_angles(
            &service->rsdk,
            DJI_RSDK_ANGLE_ATTITUDE,
            (int)DJI_RSDK_OSD_REQUEST_TIMEOUT_MS,
            &angles);
        if (rsdk_status == DJI_RSDK_OK) {
            osd_status = vp_control_osd_set_gimbal_angles(
                service->control,
                angles.yaw_tenths_degree / 10.0f,
                angles.pitch_tenths_degree / 10.0f,
                1);
            if (osd_status != APP_OK) {
                LOGW("DJI RSDK OSD update failed: %s",
                     app_status_str(osd_status));
            }
            if (previous_status != DJI_RSDK_OK) {
                LOGI("DJI RSDK angle telemetry available on %s",
                     DJI_RSDK_OSD_CAN_INTERFACE);
            }
        } else {
            osd_status = vp_control_osd_set_gimbal_angles(
                service->control,
                0.0f,
                0.0f,
                0);
            if (osd_status != APP_OK) {
                LOGW("DJI RSDK OSD invalidation failed: %s",
                     app_status_str(osd_status));
            }
            if (rsdk_status != previous_status) {
                LOGW("DJI RSDK angle read failed on %s: %s",
                     DJI_RSDK_OSD_CAN_INTERFACE,
                     dji_rsdk_status_string(rsdk_status));
            }
        }
        previous_status = rsdk_status;

        dji_rsdk_osd_timespec_add_ns(&next_update,
                                     DJI_RSDK_OSD_UPDATE_PERIOD_NS);
        while (!dji_rsdk_osd_service_stop_requested(service) &&
               clock_nanosleep(CLOCK_MONOTONIC,
                               TIMER_ABSTIME,
                               &next_update,
                               NULL) == EINTR) {
        }
    }

    (void)dji_rsdk_set_speed(
        &service->rsdk,
        0,
        0,
        0,
        1,
        1,
        DJI_RSDK_GIMBAL_COMMAND_TIMEOUT_MS);
    vp_control_osd_set_gimbal_angles(service->control, 0.0f, 0.0f, 0);
    dji_rsdk_close(&service->rsdk);
    return NULL;
}

dji_rsdk_status_t dji_rsdk_service_init(vp_control_t *control) {
    dji_rsdk_osd_service_context_t *service = &g_dji_rsdk_osd_service;
    dji_rsdk_status_t status;

    if (control == NULL) {
        return DJI_RSDK_ERR_PARAM;
    }

    pthread_mutex_lock(&service->lock);
    if (service->thread_started) {
        pthread_mutex_unlock(&service->lock);
        return DJI_RSDK_ERR_STATE;
    }
    service->control = control;
    service->stop_requested = 0;
    memset(service->direction_pressed, 0, sizeof(service->direction_pressed));
    service->speed_tenths_degree_per_second = 200U;
    service->control_generation = 0U;
    service->applied_generation = 0U;
    service->center_requested = 0;
    (void)clock_gettime(CLOCK_MONOTONIC, &service->last_control_update);
    dji_rsdk_init(&service->rsdk);
    dji_rsdk_set_debug(&service->rsdk, 0);
    dji_rsdk_set_loghex(&service->rsdk, 0);
    status = dji_rsdk_open(&service->rsdk, DJI_RSDK_OSD_CAN_INTERFACE);
    if (status != DJI_RSDK_OK) {
        service->control = NULL;
        service->initialized = 0;
        pthread_mutex_unlock(&service->lock);
        vp_control_osd_set_gimbal_angles(control, 0.0f, 0.0f, 0);
        return status;
    }
    service->initialized = 1;
    pthread_mutex_unlock(&service->lock);
    LOGI("DJI RSDK telemetry initialized on %s",
         DJI_RSDK_OSD_CAN_INTERFACE);
    return DJI_RSDK_OK;
}

dji_rsdk_status_t dji_rsdk_service_start(void) {
    dji_rsdk_osd_service_context_t *service = &g_dji_rsdk_osd_service;
    int thread_status;

    pthread_mutex_lock(&service->lock);
    if (service->thread_started) {
        pthread_mutex_unlock(&service->lock);
        return DJI_RSDK_OK;
    }
    if (!service->initialized) {
        pthread_mutex_unlock(&service->lock);
        return DJI_RSDK_ERR_STATE;
    }
    thread_status = pthread_create(&service->thread,
                                   NULL,
                                   dji_rsdk_osd_service_thread,
                                   service);
    if (thread_status != 0) {
        vp_control_t *control = service->control;

        pthread_mutex_unlock(&service->lock);
        dji_rsdk_close(&service->rsdk);
        pthread_mutex_lock(&service->lock);
        service->control = NULL;
        service->initialized = 0;
        pthread_mutex_unlock(&service->lock);
        vp_control_osd_set_gimbal_angles(control, 0.0f, 0.0f, 0);
        LOGE("DJI RSDK telemetry thread creation failed: %s",
             strerror(thread_status));
        return DJI_RSDK_ERR_STATE;
    }
    service->thread_started = 1;
    pthread_mutex_unlock(&service->lock);

    LOGI("DJI RSDK telemetry started on %s at %u Hz",
         DJI_RSDK_OSD_CAN_INTERFACE,
         (unsigned)DJI_RSDK_OSD_UPDATE_HZ);
    return DJI_RSDK_OK;
}

void dji_rsdk_service_stop(void) {
    dji_rsdk_osd_service_context_t *service = &g_dji_rsdk_osd_service;
    pthread_t thread;
    int thread_started;
    int initialized;

    pthread_mutex_lock(&service->lock);
    thread_started = service->thread_started;
    thread = service->thread;
    initialized = service->initialized;
    service->stop_requested = 1;
    pthread_mutex_unlock(&service->lock);

    if (thread_started) {
        pthread_join(thread, NULL);
        pthread_mutex_lock(&service->lock);
        service->thread_started = 0;
        service->stop_requested = 0;
        service->control = NULL;
        service->initialized = 0;
        pthread_mutex_unlock(&service->lock);
    } else if (initialized) {
        dji_rsdk_close(&service->rsdk);
        pthread_mutex_lock(&service->lock);
        service->initialized = 0;
        service->control = NULL;
        pthread_mutex_unlock(&service->lock);
    }
    LOGI("DJI RSDK telemetry stopped");
}

dji_rsdk_status_t dji_rsdk_service_set_gimbal_direction(
    dji_rsdk_gimbal_direction_t direction,
    int pressed,
    uint8_t speed_tenths_degree_per_second) {
    dji_rsdk_osd_service_context_t *service = &g_dji_rsdk_osd_service;

    if (direction < DJI_RSDK_GIMBAL_DIRECTION_UP ||
        direction > DJI_RSDK_GIMBAL_DIRECTION_RIGHT) {
        return DJI_RSDK_ERR_PARAM;
    }

    pthread_mutex_lock(&service->lock);
    if (!service->thread_started) {
        pthread_mutex_unlock(&service->lock);
        return DJI_RSDK_ERR_STATE;
    }
    service->direction_pressed[direction] = pressed ? 1 : 0;
    service->speed_tenths_degree_per_second = speed_tenths_degree_per_second;
    service->center_requested = 0;
    service->control_generation++;
    (void)clock_gettime(CLOCK_MONOTONIC, &service->last_control_update);
    pthread_mutex_unlock(&service->lock);
    return DJI_RSDK_OK;
}

dji_rsdk_status_t dji_rsdk_service_center_gimbal(void) {
    dji_rsdk_osd_service_context_t *service = &g_dji_rsdk_osd_service;

    pthread_mutex_lock(&service->lock);
    if (!service->thread_started) {
        pthread_mutex_unlock(&service->lock);
        return DJI_RSDK_ERR_STATE;
    }
    memset(service->direction_pressed, 0, sizeof(service->direction_pressed));
    service->center_requested = 1;
    service->control_generation++;
    (void)clock_gettime(CLOCK_MONOTONIC, &service->last_control_update);
    pthread_mutex_unlock(&service->lock);
    return DJI_RSDK_OK;
}
