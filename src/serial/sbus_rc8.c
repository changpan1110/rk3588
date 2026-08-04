#define LOG_FILE_NAME "sbus_rc8.c"

#include "serial/sbus_rc8.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/debug.h"

#define SBUS_RC8_IDLE_US 1000
#define SBUS_RC8_RETRY_US 10000
#define SBUS_RC8_RETRY_COUNT 100
#define SBUS_RC8_DEVICE_PATH_MAX 128
#define SBUS_RC8_CHANNEL_QUEUE_SIZE 32

typedef struct {
    uint16_t data[SBUS_RC8_CHANNEL_QUEUE_SIZE];
    pthread_cond_t cond;
    int head;
    int count;
    int active;
    uint64_t dropped;
} sbus_rc8_channel_queue_t;

struct sbus_rc8 {
    pthread_t receive_thread;
    pthread_mutex_t lock;
    char device[SBUS_RC8_DEVICE_PATH_MAX];
    sbus_rc8_channel_queue_t channels[SBUS_RC8_CHANNEL_COUNT];
    int stop_requested;
    int receive_thread_initialized;
};

static int sbus_rc8_should_stop(sbus_rc8_t *rc8) {
    int stop_requested;

    pthread_mutex_lock(&rc8->lock);
    stop_requested = rc8->stop_requested;
    pthread_mutex_unlock(&rc8->lock);
    return stop_requested;
}

static void sbus_rc8_retry_wait(sbus_rc8_t *rc8) {
    int count;

    for (count = 0; count < SBUS_RC8_RETRY_COUNT; ++count) {
        if (sbus_rc8_should_stop(rc8)) {
            break;
        }
        usleep(SBUS_RC8_RETRY_US);
    }
}

static void *sbus_rc8_receive_thread(void *opaque) {
    sbus_rc8_t *rc8 = (sbus_rc8_t *)opaque;
    sbus_data_t data;
    sbus_frame_t frame;
    sbus_status_t status;

    data.fd = -1;
    while (!sbus_rc8_should_stop(rc8)) {
        status = sbus_data_init(&data, rc8->device);
        if (status != SBUS_OK) {
            LOGW("SBUS receiver init %s failed: %s; retrying",
                 rc8->device,
                 sbus_status_string(status));
            sbus_rc8_retry_wait(rc8);
            continue;
        }

        LOGI("SBUS receive initialized: %s baud=%d format=8E2",
             rc8->device,
             SBUS_BAUDRATE);
        while (!sbus_rc8_should_stop(rc8)) {
            status = sbus_data_receive(&data, &frame);
            if (status == SBUS_OK) {
                unsigned int channel;

                if (frame.frame_lost || frame.failsafe) {
                    continue;
                }
                pthread_mutex_lock(&rc8->lock);
                for (channel = 0;
                     channel < SBUS_RC8_CHANNEL_COUNT;
                     ++channel) {
                    sbus_rc8_channel_queue_t *queue =
                        &rc8->channels[channel];
                    int tail;

                    if (!queue->active) {
                        continue;
                    }
                    if (queue->count == SBUS_RC8_CHANNEL_QUEUE_SIZE) {
                        queue->head =
                            (queue->head + 1) % SBUS_RC8_CHANNEL_QUEUE_SIZE;
                        queue->count--;
                        queue->dropped++;
                    }
                    tail = (queue->head + queue->count) %
                           SBUS_RC8_CHANNEL_QUEUE_SIZE;
                    queue->data[tail] = frame.channels[channel];
                    queue->count++;
                    pthread_cond_signal(&queue->cond);
                }
                pthread_mutex_unlock(&rc8->lock);
            } else if (status == SBUS_NO_FRAME) {
                usleep(SBUS_RC8_IDLE_US);
            } else {
                LOGW("SBUS receive %s failed: %s; reinitializing",
                     rc8->device,
                     sbus_status_string(status));
                break;
            }
        }

        sbus_data_deinit(&data);
        if (!sbus_rc8_should_stop(rc8)) {
            sbus_rc8_retry_wait(rc8);
        }
    }

    sbus_data_deinit(&data);
    return NULL;
}

sbus_status_t sbus_rc8_init(sbus_rc8_t **out_rc8,
                            const char *device) {
    sbus_rc8_t *rc8;
    unsigned int channel;

    if (out_rc8 == NULL) {
        return SBUS_ERR_PARAM;
    }
    *out_rc8 = NULL;
    if (device == NULL || device[0] == '\0' ||
        strlen(device) >= SBUS_RC8_DEVICE_PATH_MAX) {
        return SBUS_ERR_PARAM;
    }

    rc8 = (sbus_rc8_t *)calloc(1, sizeof(*rc8));
    if (rc8 == NULL) {
        return SBUS_ERR_NOMEM;
    }
    snprintf(rc8->device, sizeof(rc8->device), "%s", device);

    if (pthread_mutex_init(&rc8->lock, NULL) != 0) {
        free(rc8);
        return SBUS_ERR_IO;
    }
    for (channel = 0; channel < SBUS_RC8_CHANNEL_COUNT; ++channel) {
        if (pthread_cond_init(&rc8->channels[channel].cond, NULL) != 0) {
            while (channel > 0) {
                channel--;
                pthread_cond_destroy(&rc8->channels[channel].cond);
            }
            pthread_mutex_destroy(&rc8->lock);
            free(rc8);
            return SBUS_ERR_IO;
        }
    }
    if (pthread_create(&rc8->receive_thread,
                       NULL,
                       sbus_rc8_receive_thread,
                       rc8) != 0) {
        for (channel = 0; channel < SBUS_RC8_CHANNEL_COUNT; ++channel) {
            pthread_cond_destroy(&rc8->channels[channel].cond);
        }
        pthread_mutex_destroy(&rc8->lock);
        free(rc8);
        return SBUS_ERR_IO;
    }
    rc8->receive_thread_initialized = 1;
    *out_rc8 = rc8;
    return SBUS_OK;
}

sbus_status_t sbus_rc8_channel_receive(
    sbus_rc8_t *rc8,
    unsigned int channel_number,
    uint16_t *channel_value,
    int timeout_ms) {
    sbus_rc8_channel_queue_t *queue;
    struct timespec deadline;
    int wait_status = 0;

    if (rc8 == NULL || channel_value == NULL ||
        channel_number == 0 || channel_number > SBUS_RC8_CHANNEL_COUNT ||
        timeout_ms < 0) {
        return SBUS_ERR_PARAM;
    }
    queue = &rc8->channels[channel_number - 1U];

    if (timeout_ms > 0) {
        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
            return SBUS_ERR_IO;
        }
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    pthread_mutex_lock(&rc8->lock);
    queue->active = 1;
    while (queue->count == 0 && !rc8->stop_requested) {
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&rc8->lock);
            return SBUS_NO_FRAME;
        }
        wait_status = pthread_cond_timedwait(&queue->cond,
                                             &rc8->lock,
                                             &deadline);
        if (wait_status == ETIMEDOUT) {
            pthread_mutex_unlock(&rc8->lock);
            return SBUS_NO_FRAME;
        }
        if (wait_status != 0) {
            pthread_mutex_unlock(&rc8->lock);
            return SBUS_ERR_IO;
        }
    }

    if (queue->count == 0) {
        pthread_mutex_unlock(&rc8->lock);
        return SBUS_NO_FRAME;
    }
    *channel_value = queue->data[queue->head];
    queue->head = (queue->head + 1) % SBUS_RC8_CHANNEL_QUEUE_SIZE;
    queue->count--;
    pthread_mutex_unlock(&rc8->lock);
    return SBUS_OK;
}

void sbus_rc8_deinit(sbus_rc8_t *rc8) {
    unsigned int channel;

    if (rc8 == NULL) {
        return;
    }
    if (rc8->receive_thread_initialized) {
        pthread_mutex_lock(&rc8->lock);
        rc8->stop_requested = 1;
        for (channel = 0; channel < SBUS_RC8_CHANNEL_COUNT; ++channel) {
            pthread_cond_broadcast(&rc8->channels[channel].cond);
        }
        pthread_mutex_unlock(&rc8->lock);
        pthread_join(rc8->receive_thread, NULL);
        rc8->receive_thread_initialized = 0;
    }
    for (channel = 0; channel < SBUS_RC8_CHANNEL_COUNT; ++channel) {
        pthread_cond_destroy(&rc8->channels[channel].cond);
    }
    pthread_mutex_destroy(&rc8->lock);
    free(rc8);
}
