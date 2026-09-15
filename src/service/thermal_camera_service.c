#define _POSIX_C_SOURCE 200809L

#include "service/thermal_camera_service.h"

#include "common/debug.h"
#include "control/thermal_camera/thermal_camera.h"

#include <pthread.h>

#define THERMAL_CAMERA_SERVICE_QUEUE_CAPACITY 16U

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    pthread_t thread;
    thermal_camera_t camera;
    uint8_t modes[THERMAL_CAMERA_SERVICE_QUEUE_CAPACITY];
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
    int thread_started;
    int running;
    int stop_requested;
} thermal_camera_service_context_t;

static thermal_camera_service_context_t g_thermal_camera_service = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER
};

static void *thermal_camera_service_thread(void *opaque) {
    thermal_camera_service_context_t *service =
        (thermal_camera_service_context_t *)opaque;

    for (;;) {
        uint8_t mode;
        thermal_camera_status_t status;

        pthread_mutex_lock(&service->lock);
        while (service->queue_count == 0U && !service->stop_requested) {
            pthread_cond_wait(&service->condition, &service->lock);
        }
        if (service->stop_requested) {
            pthread_mutex_unlock(&service->lock);
            break;
        }
        mode = service->modes[service->queue_head];
        service->queue_head =
            (service->queue_head + 1U) % THERMAL_CAMERA_SERVICE_QUEUE_CAPACITY;
        --service->queue_count;
        pthread_mutex_unlock(&service->lock);

        status = thermal_camera_set_pseudocolor(
            &service->camera, (thermal_camera_pseudocolor_t)mode);
        if (status == THERMAL_CAMERA_OK) {
            LOGI("thermal pseudocolor applied: %u(%s)",
                 (unsigned)mode,
                 thermal_camera_pseudocolor_name(mode));
        } else {
            LOGW("thermal pseudocolor %u(%s) failed: %s",
                 (unsigned)mode,
                 thermal_camera_pseudocolor_name(mode),
                 thermal_camera_status_string(status));
        }
    }

    thermal_camera_close(&service->camera);
    pthread_mutex_lock(&service->lock);
    service->running = 0;
    service->thread_started = 0;
    service->queue_head = 0U;
    service->queue_tail = 0U;
    service->queue_count = 0U;
    pthread_mutex_unlock(&service->lock);
    return NULL;
}

int thermal_camera_service_init(const char *device, uint32_t baudrate) {
    thermal_camera_status_t status;

    if (device == NULL || device[0] == '\0' || baudrate == 0U) {
        return THERMAL_CAMERA_ERR_PARAM;
    }

    pthread_mutex_lock(&g_thermal_camera_service.lock);
    if (g_thermal_camera_service.running) {
        pthread_mutex_unlock(&g_thermal_camera_service.lock);
        return THERMAL_CAMERA_OK;
    }
    g_thermal_camera_service.queue_head = 0U;
    g_thermal_camera_service.queue_tail = 0U;
    g_thermal_camera_service.queue_count = 0U;
    g_thermal_camera_service.stop_requested = 0;
    pthread_mutex_unlock(&g_thermal_camera_service.lock);

    thermal_camera_init(&g_thermal_camera_service.camera);
    thermal_camera_set_debug(&g_thermal_camera_service.camera, 1);
    thermal_camera_set_loghex(&g_thermal_camera_service.camera, 0);
    status = thermal_camera_open(&g_thermal_camera_service.camera,
                                 device,
                                 baudrate);
    if (status != THERMAL_CAMERA_OK) {
        LOGW("thermal camera open %s failed: %s",
             device,
             thermal_camera_status_string(status));
        return status;
    }
    LOGI("thermal camera initialized: %s baud=%u",
         device,
         (unsigned)baudrate);
    return THERMAL_CAMERA_OK;
}

int thermal_camera_service_start(void) {
    pthread_mutex_lock(&g_thermal_camera_service.lock);
    if (g_thermal_camera_service.running) {
        pthread_mutex_unlock(&g_thermal_camera_service.lock);
        return THERMAL_CAMERA_OK;
    }
    if (!thermal_camera_is_open(&g_thermal_camera_service.camera)) {
        pthread_mutex_unlock(&g_thermal_camera_service.lock);
        return THERMAL_CAMERA_ERR_STATE;
    }
    g_thermal_camera_service.running = 1;
    if (pthread_create(&g_thermal_camera_service.thread,
                       NULL,
                       thermal_camera_service_thread,
                       &g_thermal_camera_service) != 0) {
        g_thermal_camera_service.running = 0;
        pthread_mutex_unlock(&g_thermal_camera_service.lock);
        thermal_camera_close(&g_thermal_camera_service.camera);
        return THERMAL_CAMERA_ERR_UART;
    }
    g_thermal_camera_service.thread_started = 1;
    pthread_mutex_unlock(&g_thermal_camera_service.lock);
    LOGI("thermal camera service started");
    return THERMAL_CAMERA_OK;
}

void thermal_camera_service_stop(void) {
    pthread_t thread;
    int thread_started;

    pthread_mutex_lock(&g_thermal_camera_service.lock);
    thread_started = g_thermal_camera_service.thread_started;
    thread = g_thermal_camera_service.thread;
    g_thermal_camera_service.stop_requested = 1;
    pthread_cond_signal(&g_thermal_camera_service.condition);
    pthread_mutex_unlock(&g_thermal_camera_service.lock);

    if (thread_started) {
        (void)pthread_join(thread, NULL);
    } else {
        thermal_camera_close(&g_thermal_camera_service.camera);
        pthread_mutex_lock(&g_thermal_camera_service.lock);
        g_thermal_camera_service.running = 0;
        g_thermal_camera_service.queue_head = 0U;
        g_thermal_camera_service.queue_tail = 0U;
        g_thermal_camera_service.queue_count = 0U;
        pthread_mutex_unlock(&g_thermal_camera_service.lock);
    }
}

int thermal_camera_service_request_pseudocolor(uint8_t mode) {
    int result = THERMAL_CAMERA_OK;

    if (mode > THERMAL_CAMERA_PSEUDOCOLOR_DEEP_BLUE) {
        return THERMAL_CAMERA_ERR_RANGE;
    }

    pthread_mutex_lock(&g_thermal_camera_service.lock);
    if (!g_thermal_camera_service.running ||
        g_thermal_camera_service.stop_requested) {
        result = THERMAL_CAMERA_ERR_STATE;
    } else if (g_thermal_camera_service.queue_count >=
               THERMAL_CAMERA_SERVICE_QUEUE_CAPACITY) {
        result = THERMAL_CAMERA_ERR_BUSY;
    } else {
        g_thermal_camera_service.modes[g_thermal_camera_service.queue_tail] =
            mode;
        g_thermal_camera_service.queue_tail =
            (g_thermal_camera_service.queue_tail + 1U) %
            THERMAL_CAMERA_SERVICE_QUEUE_CAPACITY;
        ++g_thermal_camera_service.queue_count;
        pthread_cond_signal(&g_thermal_camera_service.condition);
    }
    pthread_mutex_unlock(&g_thermal_camera_service.lock);
    return result;
}
