#define _POSIX_C_SOURCE 200809L

#include "service/sfl0603_laser_service.h"

#include "common/debug.h"
#include "control/sfl0603_laser/sfl0603_laser.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define SFL0603_LASER_SERVICE_QUEUE_CAPACITY 8U
#define SFL0603_LASER_SERVICE_RESPONSE_TIMEOUT_MS 2000
#define SFL0603_LASER_SERVICE_CONTINUOUS_READ_TIMEOUT_MS 200

typedef enum {
    SFL0603_LASER_SERVICE_CMD_SINGLE = 0,
    SFL0603_LASER_SERVICE_CMD_CONTINUOUS_START,
    SFL0603_LASER_SERVICE_CMD_CONTINUOUS_STOP
} sfl0603_laser_service_command_t;

typedef struct {
    sfl0603_laser_service_command_t command;
    uint16_t period_ms;
} sfl0603_laser_service_request_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    pthread_t thread;
    vp_control_t *osd_control;
    sfl0603_laser_service_request_t requests[SFL0603_LASER_SERVICE_QUEUE_CAPACITY];
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
    int thread_started;
    int running;
    int stop_requested;
} sfl0603_laser_service_context_t;

static sfl0603_laser_service_context_t g_sfl0603_laser_service = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER
};

static void sfl0603_laser_service_push_osd(
    const sfl0603_laser_measurement_t *measurement) {
    vp_control_t *control = g_sfl0603_laser_service.osd_control;

    if (control == NULL || measurement == NULL) {
        return;
    }
    (void)vp_control_osd_set_laser_distance(
        control,
        (float)sfl0603_laser_distance_m(measurement->distance_dm[0]),
        measurement->front_target_present ? 1 : 0,
        1);
    (void)vp_control_osd_set_laser_measuring(control, 0);
    (void)vp_control_osd_set_laser_error(control, 0);
}

static void sfl0603_laser_service_set_measuring_osd(int measuring) {
    vp_control_t *control = g_sfl0603_laser_service.osd_control;

    if (control == NULL) {
        return;
    }
    (void)vp_control_osd_set_laser_measuring(control, measuring);
}

static void sfl0603_laser_service_set_error_osd(int error) {
    vp_control_t *control = g_sfl0603_laser_service.osd_control;

    if (control == NULL) {
        return;
    }
    (void)vp_control_osd_set_laser_measuring(control, 0);
    (void)vp_control_osd_set_laser_error(control, error);
}

static void sfl0603_laser_service_clear_osd(void) {
    vp_control_t *control = g_sfl0603_laser_service.osd_control;

    if (control == NULL) {
        return;
    }
    (void)vp_control_osd_set_laser_distance(control, 0.0f, 0, 0);
}

static sfl0603_laser_status_t sfl0603_laser_service_push_request(
    sfl0603_laser_service_command_t command,
    uint16_t period_ms) {
    sfl0603_laser_status_t result = SFL0603_LASER_OK;

    pthread_mutex_lock(&g_sfl0603_laser_service.lock);
    if (!g_sfl0603_laser_service.running ||
        g_sfl0603_laser_service.stop_requested) {
        result = SFL0603_LASER_ERR_STATE;
    } else if (g_sfl0603_laser_service.queue_count >=
               SFL0603_LASER_SERVICE_QUEUE_CAPACITY) {
        result = SFL0603_LASER_ERR_STATE;
    } else {
        g_sfl0603_laser_service.requests[g_sfl0603_laser_service.queue_tail]
            .command = command;
        g_sfl0603_laser_service.requests[g_sfl0603_laser_service.queue_tail]
            .period_ms = period_ms;
        g_sfl0603_laser_service.queue_tail =
            (g_sfl0603_laser_service.queue_tail + 1U) %
            SFL0603_LASER_SERVICE_QUEUE_CAPACITY;
        ++g_sfl0603_laser_service.queue_count;
        pthread_cond_signal(&g_sfl0603_laser_service.condition);
    }
    pthread_mutex_unlock(&g_sfl0603_laser_service.lock);
    return result;
}

static void sfl0603_laser_service_handle_request(
    const sfl0603_laser_service_request_t *request,
    int *continuous_active) {
    sfl0603_laser_measurement_t measurement;
    sfl0603_laser_status_t status;

    switch (request->command) {
    case SFL0603_LASER_SERVICE_CMD_SINGLE:
        status = sfl0603_laser_measure_once(
            SFL0603_LASER_SERVICE_RESPONSE_TIMEOUT_MS, &measurement);
        if (status == SFL0603_LASER_OK) {
            sfl0603_laser_service_push_osd(&measurement);
            LOGI("sfl0603 single measure: %.1f m",
                 sfl0603_laser_distance_m(measurement.distance_dm[0]));
        } else {
            sfl0603_laser_service_set_error_osd(1);
            LOGW("sfl0603 single measure failed: %s",
                 sfl0603_laser_status_string(status));
        }
        break;

    case SFL0603_LASER_SERVICE_CMD_CONTINUOUS_START:
        status = sfl0603_laser_start_continuous(
            request->period_ms,
            SFL0603_LASER_SERVICE_RESPONSE_TIMEOUT_MS,
            &measurement);
        if (status == SFL0603_LASER_OK) {
            sfl0603_laser_service_push_osd(&measurement);
            *continuous_active = 1;
            LOGI("sfl0603 continuous measurement started period_ms=%u",
                 (unsigned)request->period_ms);
        } else {
            LOGW("sfl0603 continuous measurement start failed: %s",
                 sfl0603_laser_status_string(status));
        }
        break;

    case SFL0603_LASER_SERVICE_CMD_CONTINUOUS_STOP:
        status = sfl0603_laser_standby(SFL0603_LASER_SERVICE_RESPONSE_TIMEOUT_MS);
        *continuous_active = 0;
        sfl0603_laser_service_clear_osd();
        if (status == SFL0603_LASER_OK) {
            LOGI("sfl0603 continuous measurement stopped");
        } else {
            LOGW("sfl0603 continuous measurement stop failed: %s",
                 sfl0603_laser_status_string(status));
        }
        break;

    default:
        break;
    }
}

static void *sfl0603_laser_service_thread(void *opaque) {
    sfl0603_laser_service_context_t *service =
        (sfl0603_laser_service_context_t *)opaque;
    int continuous_active = 0;

    for (;;) {
        sfl0603_laser_service_request_t request;
        int have_request = 0;

        pthread_mutex_lock(&service->lock);
        if (continuous_active && service->queue_count == 0U &&
            !service->stop_requested) {
            struct timespec deadline;

            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec +=
                (long)SFL0603_LASER_SERVICE_CONTINUOUS_READ_TIMEOUT_MS *
                1000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec += deadline.tv_nsec / 1000000000L;
                deadline.tv_nsec %= 1000000000L;
            }
            (void)pthread_cond_timedwait(&service->condition,
                                         &service->lock,
                                         &deadline);
        } else {
            while (service->queue_count == 0U && !service->stop_requested) {
                pthread_cond_wait(&service->condition, &service->lock);
            }
        }
        if (service->stop_requested) {
            pthread_mutex_unlock(&service->lock);
            break;
        }
        if (service->queue_count > 0U) {
            request = service->requests[service->queue_head];
            service->queue_head =
                (service->queue_head + 1U) %
                SFL0603_LASER_SERVICE_QUEUE_CAPACITY;
            --service->queue_count;
            have_request = 1;
        }
        pthread_mutex_unlock(&service->lock);

        if (have_request) {
            sfl0603_laser_service_handle_request(&request, &continuous_active);
        } else if (continuous_active) {
            sfl0603_laser_measurement_t measurement;
            sfl0603_laser_status_t status = sfl0603_laser_read_measurement(
                SFL0603_LASER_SERVICE_CONTINUOUS_READ_TIMEOUT_MS,
                &measurement);

            if (status == SFL0603_LASER_OK) {
                sfl0603_laser_service_push_osd(&measurement);
            } else if (status != SFL0603_LASER_ERR_TIMEOUT &&
                       status != SFL0603_LASER_NO_DATA) {
                LOGW("sfl0603 continuous read failed: %s",
                     sfl0603_laser_status_string(status));
                continuous_active = 0;
            }
        }
    }

    if (continuous_active) {
        (void)sfl0603_laser_standby(SFL0603_LASER_SERVICE_RESPONSE_TIMEOUT_MS);
    }
    sfl0603_laser_close();

    pthread_mutex_lock(&service->lock);
    service->running = 0;
    service->thread_started = 0;
    service->queue_head = 0U;
    service->queue_tail = 0U;
    service->queue_count = 0U;
    pthread_mutex_unlock(&service->lock);
    return NULL;
}

sfl0603_laser_status_t sfl0603_laser_service_init(const char *device,
                                                  uint32_t baudrate) {
    sfl0603_laser_status_t status;

    if (device == NULL || device[0] == '\0' || baudrate == 0U) {
        return SFL0603_LASER_ERR_PARAM;
    }

    pthread_mutex_lock(&g_sfl0603_laser_service.lock);
    if (g_sfl0603_laser_service.running) {
        pthread_mutex_unlock(&g_sfl0603_laser_service.lock);
        return SFL0603_LASER_OK;
    }
    g_sfl0603_laser_service.queue_head = 0U;
    g_sfl0603_laser_service.queue_tail = 0U;
    g_sfl0603_laser_service.queue_count = 0U;
    g_sfl0603_laser_service.stop_requested = 0;
    pthread_mutex_unlock(&g_sfl0603_laser_service.lock);

    status = sfl0603_laser_init();
    if (status != SFL0603_LASER_OK) {
        return status;
    }
    sfl0603_laser_set_debug(1);
    sfl0603_laser_set_loghex(0);
    status = sfl0603_laser_open(device, baudrate);
    if (status != SFL0603_LASER_OK) {
        LOGW("sfl0603 laser open %s failed: %s",
             device,
             sfl0603_laser_status_string(status));
        return status;
    }
    LOGI("sfl0603 laser initialized: %s baud=%u",
         device,
         (unsigned)baudrate);
    return SFL0603_LASER_OK;
}

sfl0603_laser_status_t sfl0603_laser_service_start(void) {
    pthread_mutex_lock(&g_sfl0603_laser_service.lock);
    if (g_sfl0603_laser_service.running) {
        pthread_mutex_unlock(&g_sfl0603_laser_service.lock);
        return SFL0603_LASER_OK;
    }
    if (!sfl0603_laser_is_open()) {
        pthread_mutex_unlock(&g_sfl0603_laser_service.lock);
        return SFL0603_LASER_ERR_STATE;
    }
    g_sfl0603_laser_service.running = 1;
    if (pthread_create(&g_sfl0603_laser_service.thread,
                       NULL,
                       sfl0603_laser_service_thread,
                       &g_sfl0603_laser_service) != 0) {
        g_sfl0603_laser_service.running = 0;
        pthread_mutex_unlock(&g_sfl0603_laser_service.lock);
        sfl0603_laser_close();
        return SFL0603_LASER_ERR_UART;
    }
    g_sfl0603_laser_service.thread_started = 1;
    pthread_mutex_unlock(&g_sfl0603_laser_service.lock);
    LOGI("sfl0603 laser service started");
    return SFL0603_LASER_OK;
}

void sfl0603_laser_service_stop(void) {
    pthread_t thread;
    int thread_started;

    pthread_mutex_lock(&g_sfl0603_laser_service.lock);
    thread_started = g_sfl0603_laser_service.thread_started;
    thread = g_sfl0603_laser_service.thread;
    g_sfl0603_laser_service.stop_requested = 1;
    pthread_cond_signal(&g_sfl0603_laser_service.condition);
    pthread_mutex_unlock(&g_sfl0603_laser_service.lock);

    if (thread_started) {
        (void)pthread_join(thread, NULL);
    } else {
        sfl0603_laser_close();
        pthread_mutex_lock(&g_sfl0603_laser_service.lock);
        g_sfl0603_laser_service.running = 0;
        g_sfl0603_laser_service.queue_head = 0U;
        g_sfl0603_laser_service.queue_tail = 0U;
        g_sfl0603_laser_service.queue_count = 0U;
        pthread_mutex_unlock(&g_sfl0603_laser_service.lock);
    }
}

void sfl0603_laser_service_set_osd_control(vp_control_t *control) {
    pthread_mutex_lock(&g_sfl0603_laser_service.lock);
    g_sfl0603_laser_service.osd_control = control;
    pthread_mutex_unlock(&g_sfl0603_laser_service.lock);
}

sfl0603_laser_status_t sfl0603_laser_service_request_single_measure(void) {
    sfl0603_laser_status_t status = sfl0603_laser_service_push_request(
        SFL0603_LASER_SERVICE_CMD_SINGLE, 0U);
    if (status == SFL0603_LASER_OK) {
        sfl0603_laser_service_set_measuring_osd(1);
    }
    return status;
}

sfl0603_laser_status_t sfl0603_laser_service_request_continuous(
    uint16_t period_ms,
    int enable) {
    if (enable) {
        if (period_ms == 0U) {
            return SFL0603_LASER_ERR_PARAM;
        }
        return sfl0603_laser_service_push_request(
            SFL0603_LASER_SERVICE_CMD_CONTINUOUS_START, period_ms);
    }
    return sfl0603_laser_service_push_request(
        SFL0603_LASER_SERVICE_CMD_CONTINUOUS_STOP, 0U);
}
