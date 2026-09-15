#define _POSIX_C_SOURCE 200809L

#include "service/high_speed_camera_service.h"

#include "common/debug.h"
#include "control/high_speed_camera/high_speed_camera.h"

#include <pthread.h>
#include <time.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_t thread;
    int thread_started;
    int stop_requested;
    int unlock_pending;
    int running;
} high_speed_camera_service_context_t;

static high_speed_camera_service_context_t g_high_speed_camera_service = {
    .lock = PTHREAD_MUTEX_INITIALIZER
};

static void *high_speed_camera_service_thread(void *opaque) {
    high_speed_camera_service_context_t *service =
        (high_speed_camera_service_context_t *)opaque;

    for (;;) {
        int stop_requested;
        int unlock_pending;

        pthread_mutex_lock(&service->lock);
        stop_requested = service->stop_requested;
        unlock_pending = service->unlock_pending;
        service->unlock_pending = 0;
        pthread_mutex_unlock(&service->lock);

        if (stop_requested) {
            break;
        }

        if (unlock_pending) {
            high_speed_camera_status_t status = high_speed_camera_send_key(
                HIGH_SPEED_CAMERA_KEY_UNLOCK);
            if (status != HIGH_SPEED_CAMERA_OK) {
                LOGW("high-speed-camera unlock send failed: %s",
                     high_speed_camera_status_string(status));
            } else {
                LOGI("high-speed-camera unlock request sent");
            }
        }

        /* Keep receive and transmit on one thread. The finite wait also lets
         * service_stop join without closing a UART that is still in poll(). */
        {
            high_speed_camera_status_t status = high_speed_camera_process_once(
                100, NULL, NULL, NULL);
            if (status != HIGH_SPEED_CAMERA_OK &&
                status != HIGH_SPEED_CAMERA_NO_DATA &&
                status != HIGH_SPEED_CAMERA_ERR_PROTOCOL &&
                status != HIGH_SPEED_CAMERA_ERR_CRC) {
                LOGW("high-speed-camera receive failed: %s",
                     high_speed_camera_status_string(status));
                {
                    const struct timespec retry_delay = {0, 100000000L};
                    (void)nanosleep(&retry_delay, NULL);
                }
            }
        }
    }

    high_speed_camera_close();
    pthread_mutex_lock(&service->lock);
    service->running = 0;
    service->thread_started = 0;
    pthread_mutex_unlock(&service->lock);
    return NULL;
}

int high_speed_camera_service_init(const char *device, uint32_t baudrate) {
    high_speed_camera_status_t status;

    if (device == NULL || device[0] == '\0' || baudrate == 0U) {
        return HIGH_SPEED_CAMERA_ERR_PARAM;
    }

    pthread_mutex_lock(&g_high_speed_camera_service.lock);
    if (g_high_speed_camera_service.running) {
        pthread_mutex_unlock(&g_high_speed_camera_service.lock);
        return HIGH_SPEED_CAMERA_OK;
    }
    g_high_speed_camera_service.stop_requested = 0;
    g_high_speed_camera_service.unlock_pending = 0;
    pthread_mutex_unlock(&g_high_speed_camera_service.lock);

    (void)high_speed_camera_init();
    high_speed_camera_set_debug(1);
    high_speed_camera_set_loghex(0);
    status = high_speed_camera_open(device, baudrate);
    if (status != HIGH_SPEED_CAMERA_OK) {
        LOGW("high-speed-camera open %s failed: %s",
             device,
             high_speed_camera_status_string(status));
        return status;
    }
    LOGI("high-speed-camera initialized: %s baud=%u",
         device,
         (unsigned)baudrate);
    return HIGH_SPEED_CAMERA_OK;
}

int high_speed_camera_service_start(void) {
    pthread_mutex_lock(&g_high_speed_camera_service.lock);
    if (g_high_speed_camera_service.running) {
        pthread_mutex_unlock(&g_high_speed_camera_service.lock);
        return HIGH_SPEED_CAMERA_OK;
    }
    if (!high_speed_camera_is_open()) {
        pthread_mutex_unlock(&g_high_speed_camera_service.lock);
        return HIGH_SPEED_CAMERA_ERR_STATE;
    }
    g_high_speed_camera_service.running = 1;
    if (pthread_create(&g_high_speed_camera_service.thread,
                       NULL,
                       high_speed_camera_service_thread,
                       &g_high_speed_camera_service) != 0) {
        g_high_speed_camera_service.running = 0;
        pthread_mutex_unlock(&g_high_speed_camera_service.lock);
        high_speed_camera_close();
        return HIGH_SPEED_CAMERA_ERR_UART;
    }
    g_high_speed_camera_service.thread_started = 1;
    pthread_mutex_unlock(&g_high_speed_camera_service.lock);
    LOGI("high-speed-camera service started");
    return HIGH_SPEED_CAMERA_OK;
}

void high_speed_camera_service_stop(void) {
    pthread_t thread;
    int thread_started;

    pthread_mutex_lock(&g_high_speed_camera_service.lock);
    thread_started = g_high_speed_camera_service.thread_started;
    thread = g_high_speed_camera_service.thread;
    g_high_speed_camera_service.stop_requested = 1;
    pthread_mutex_unlock(&g_high_speed_camera_service.lock);

    if (thread_started) {
        (void)pthread_join(thread, NULL);
    } else {
        high_speed_camera_close();
        pthread_mutex_lock(&g_high_speed_camera_service.lock);
        g_high_speed_camera_service.running = 0;
        g_high_speed_camera_service.unlock_pending = 0;
        pthread_mutex_unlock(&g_high_speed_camera_service.lock);
    }
}

int high_speed_camera_service_request_unlock(void) {
    int running;

    pthread_mutex_lock(&g_high_speed_camera_service.lock);
    running = g_high_speed_camera_service.running;
    if (running) {
        /* Coalesce repeated press/hold notifications into one frame. */
        g_high_speed_camera_service.unlock_pending = 1;
    }
    pthread_mutex_unlock(&g_high_speed_camera_service.lock);
    return running ? HIGH_SPEED_CAMERA_OK : HIGH_SPEED_CAMERA_ERR_STATE;
}
