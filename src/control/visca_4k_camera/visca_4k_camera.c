#define _POSIX_C_SOURCE 200809L
#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "visca_4k_camera.c"

#include "control/visca_4k_camera/visca_4k_camera.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "common/debug.h"

#define VISCA_4K_CAMERA_EVENT_PRESS 1U
#define VISCA_4K_CAMERA_EVENT_RELEASE 2U
#define VISCA_4K_CAMERA_REPEAT_MS 100L
#define VISCA_4K_CAMERA_ADDRESS 1U

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    pthread_t thread;
    visca_camera_t camera;
    visca_4k_camera_action_t action;
    uint8_t speed;
    int running;
    int thread_started;
    int stop_requested;
    int initialized;
} visca_4k_camera_context_t;

static visca_4k_camera_context_t g_visca_4k_camera = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
    .action = VISCA_4K_CAMERA_ACTION_ZOOM_TELE,
    .speed = 7U
};

static void visca_4k_camera_stop_motion(visca_4k_camera_action_t action) {
    if (action == VISCA_4K_CAMERA_ACTION_FOCUS_FAR ||
        action == VISCA_4K_CAMERA_ACTION_FOCUS_NEAR) {
        (void)visca_camera_focus_stop(&g_visca_4k_camera.camera);
    } else {
        (void)visca_camera_zoom_stop(&g_visca_4k_camera.camera);
    }
}

static visca_status_t visca_4k_camera_send_motion(
    visca_4k_camera_action_t action,
    uint8_t speed) {
    switch (action) {
    case VISCA_4K_CAMERA_ACTION_ZOOM_TELE:
        return visca_camera_zoom_tele(&g_visca_4k_camera.camera, speed);
    case VISCA_4K_CAMERA_ACTION_ZOOM_WIDE:
        return visca_camera_zoom_wide(&g_visca_4k_camera.camera, speed);
    case VISCA_4K_CAMERA_ACTION_FOCUS_FAR:
        return visca_camera_focus_far(&g_visca_4k_camera.camera, speed);
    case VISCA_4K_CAMERA_ACTION_FOCUS_NEAR:
        return visca_camera_focus_near(&g_visca_4k_camera.camera, speed);
    default:
        return VISCA_ERR_PARAM;
    }
}

static void *visca_4k_camera_thread(void *opaque) {
    visca_4k_camera_context_t *context =
        (visca_4k_camera_context_t *)opaque;
    visca_4k_camera_action_t last_action =
        VISCA_4K_CAMERA_ACTION_ZOOM_TELE;
    int motion_active = 0;

    for (;;) {
        visca_4k_camera_action_t action;
        uint8_t speed;
        struct timespec deadline;
        int wait_result;

        pthread_mutex_lock(&context->lock);
        while (!context->stop_requested && !context->running) {
            pthread_cond_wait(&context->condition, &context->lock);
        }
        if (context->stop_requested) {
            pthread_mutex_unlock(&context->lock);
            break;
        }
        action = context->action;
        speed = context->speed;
        pthread_mutex_unlock(&context->lock);

        if (!motion_active || action != last_action) {
            if (motion_active) {
                visca_4k_camera_stop_motion(last_action);
            }
            last_action = action;
            motion_active = 1;
        }

        if (visca_4k_camera_send_motion(action, speed) != VISCA_OK) {
            LOGW("VISCA 4K action failed action=%d speed=%u",
                 (int)action,
                 (unsigned int)speed);
        }

        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += VISCA_4K_CAMERA_REPEAT_MS * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_mutex_lock(&context->lock);
        wait_result = 0;
        while (!context->stop_requested && context->running && wait_result == 0) {
            wait_result = pthread_cond_timedwait(
                &context->condition, &context->lock, &deadline);
        }
        if (!context->running && motion_active) {
            pthread_mutex_unlock(&context->lock);
            visca_4k_camera_stop_motion(last_action);
            motion_active = 0;
        } else {
            pthread_mutex_unlock(&context->lock);
        }
    }

    if (motion_active) {
        visca_4k_camera_stop_motion(last_action);
    }
    return NULL;
}

static void visca_4k_camera_reset_state(void) {
    memset(&g_visca_4k_camera.camera, 0, sizeof(g_visca_4k_camera.camera));
    g_visca_4k_camera.action = VISCA_4K_CAMERA_ACTION_ZOOM_TELE;
    g_visca_4k_camera.speed = 7U;
    g_visca_4k_camera.running = 0;
    g_visca_4k_camera.stop_requested = 0;
}

visca_status_t visca_4k_camera_init(const char *device, uint32_t baudrate) {
    visca_status_t status;

    if (device == NULL || device[0] == '\0' || baudrate == 0U) {
        return VISCA_ERR_PARAM;
    }

    pthread_mutex_lock(&g_visca_4k_camera.lock);
    if (g_visca_4k_camera.thread_started) {
        pthread_mutex_unlock(&g_visca_4k_camera.lock);
        return VISCA_OK;
    }
    visca_4k_camera_reset_state();
    g_visca_4k_camera.initialized = 1;
    pthread_mutex_unlock(&g_visca_4k_camera.lock);

    status = visca_camera_open(&g_visca_4k_camera.camera,
                               device,
                               baudrate,
                               VISCA_4K_CAMERA_ADDRESS);
    if (status != VISCA_OK) {
        return status;
    }
    status = visca_camera_set_focus_mode(&g_visca_4k_camera.camera,
                                         VISCA_FOCUS_MANUAL);
    if (status != VISCA_OK) {
        LOGW("VISCA 4K focus manual mode setup failed: %s",
             visca_camera_status_string(status));
    }

    pthread_mutex_lock(&g_visca_4k_camera.lock);
    g_visca_4k_camera.stop_requested = 0;
    g_visca_4k_camera.running = 0;
    g_visca_4k_camera.speed = 7U;
    if (pthread_create(&g_visca_4k_camera.thread,
                       NULL,
                       visca_4k_camera_thread,
                       &g_visca_4k_camera) != 0) {
        pthread_mutex_unlock(&g_visca_4k_camera.lock);
        visca_camera_close(&g_visca_4k_camera.camera);
        return VISCA_ERR_UART;
    }
    g_visca_4k_camera.thread_started = 1;
    pthread_mutex_unlock(&g_visca_4k_camera.lock);
    LOGI("VISCA 4K camera initialized device=%s baud=%u address=%u",
         device,
         (unsigned int)baudrate,
         (unsigned int)VISCA_4K_CAMERA_ADDRESS);
    return VISCA_OK;
}

void visca_4k_camera_stop(void) {
    pthread_t thread;
    int thread_started;

    pthread_mutex_lock(&g_visca_4k_camera.lock);
    thread_started = g_visca_4k_camera.thread_started;
    thread = g_visca_4k_camera.thread;
    g_visca_4k_camera.stop_requested = 1;
    g_visca_4k_camera.running = 0;
    pthread_cond_signal(&g_visca_4k_camera.condition);
    pthread_mutex_unlock(&g_visca_4k_camera.lock);

    if (thread_started) {
        pthread_join(thread, NULL);
    }
    visca_camera_close(&g_visca_4k_camera.camera);
    pthread_mutex_lock(&g_visca_4k_camera.lock);
    g_visca_4k_camera.thread_started = 0;
    g_visca_4k_camera.stop_requested = 0;
    pthread_mutex_unlock(&g_visca_4k_camera.lock);
}

void visca_4k_camera_deinit(void) {
    pthread_mutex_lock(&g_visca_4k_camera.lock);
    if (g_visca_4k_camera.thread_started || g_visca_4k_camera.running) {
        pthread_mutex_unlock(&g_visca_4k_camera.lock);
        LOGW("VISCA 4K camera deinit requested before stop");
        return;
    }
    visca_4k_camera_reset_state();
    g_visca_4k_camera.initialized = 0;
    pthread_mutex_unlock(&g_visca_4k_camera.lock);
}

int visca_4k_camera_is_running(void) {
    int started;
    pthread_mutex_lock(&g_visca_4k_camera.lock);
    started = g_visca_4k_camera.thread_started;
    pthread_mutex_unlock(&g_visca_4k_camera.lock);
    return started;
}

visca_status_t visca_4k_camera_action_request(
    visca_4k_camera_action_t action,
    uint8_t event,
    uint8_t speed) {
    if (action > VISCA_4K_CAMERA_ACTION_FOCUS_NEAR) {
        return VISCA_ERR_PARAM;
    }
    if (event == VISCA_4K_CAMERA_EVENT_PRESS && speed > 7U) {
        return VISCA_ERR_PARAM;
    }
    pthread_mutex_lock(&g_visca_4k_camera.lock);
    if (!g_visca_4k_camera.thread_started) {
        pthread_mutex_unlock(&g_visca_4k_camera.lock);
        return VISCA_ERR_UART;
    }
    if (event == VISCA_4K_CAMERA_EVENT_PRESS) {
        g_visca_4k_camera.action = action;
        g_visca_4k_camera.speed = speed;
        g_visca_4k_camera.running = 1;
    } else if (event == VISCA_4K_CAMERA_EVENT_RELEASE) {
        g_visca_4k_camera.running = 0;
    } else {
        pthread_mutex_unlock(&g_visca_4k_camera.lock);
        return VISCA_ERR_PARAM;
    }
    pthread_cond_signal(&g_visca_4k_camera.condition);
    pthread_mutex_unlock(&g_visca_4k_camera.lock);
    return VISCA_OK;
}
