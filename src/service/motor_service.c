#define _POSIX_C_SOURCE 200809L

#include "service/motor_service.h"

#include "common/debug.h"

#include <pthread.h>
#include <time.h>

/* 电机保持寄存器（含速度）：500=停止, 正转=500-speed, 反转=500+speed（web speed 0~255） */
#define MOTOR_REGISTER_ADDR 0x100U
#define MOTOR_STOP_VALUE 500U
/* 光耦继电器线圈 */
#define MOTOR_TRIGGER_COIL_ADDR 0x100U
#define MOTOR_TRIGGER_PULSE_MS 1000
#define MOTOR_RESPONSE_TIMEOUT_MS 1000

#define MOTOR_SERVICE_QUEUE_CAPACITY 8U

typedef enum {
    MOTOR_SERVICE_CMD_MOVE = 0,
    MOTOR_SERVICE_CMD_STOP,
    MOTOR_SERVICE_CMD_TRIGGER
} motor_service_command_t;

typedef struct {
    motor_service_command_t command;
    int direction;
    uint8_t speed;
} motor_service_request_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    pthread_t thread;
    modbus_rtu_t modbus;
    uint8_t slave_addr;
    motor_service_request_t requests[MOTOR_SERVICE_QUEUE_CAPACITY];
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
    int thread_started;
    int running;
    int stop_requested;
} motor_service_context_t;

static motor_service_context_t g_motor_service = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER
};

static uint16_t motor_speed_to_register(int direction, uint8_t speed) {
    if (direction > 0) {
        /* 正转：500 - speed（0=停止, 255=最快=245） */
        return 500U - (uint16_t)speed;
    }
    /* 反转：500 + speed（0=停止, 255=最快=755） */
    return 500U + (uint16_t)speed;
}

static modbus_status_t motor_service_push_request(motor_service_command_t command,
                                                  int direction,
                                                  uint8_t speed) {
    modbus_status_t result = MODBUS_OK;

    pthread_mutex_lock(&g_motor_service.lock);
    if (!g_motor_service.running || g_motor_service.stop_requested) {
        result = MODBUS_ERR_IO;
    } else if (g_motor_service.queue_count >= MOTOR_SERVICE_QUEUE_CAPACITY) {
        result = MODBUS_ERR_IO;
    } else {
        g_motor_service.requests[g_motor_service.queue_tail].command = command;
        g_motor_service.requests[g_motor_service.queue_tail].direction = direction;
        g_motor_service.requests[g_motor_service.queue_tail].speed = speed;
        g_motor_service.queue_tail =
            (g_motor_service.queue_tail + 1U) % MOTOR_SERVICE_QUEUE_CAPACITY;
        ++g_motor_service.queue_count;
        pthread_cond_signal(&g_motor_service.condition);
    }
    pthread_mutex_unlock(&g_motor_service.lock);
    return result;
}

static void motor_service_handle_request(const motor_service_request_t *request) {
    modbus_status_t status;

    switch (request->command) {
    case MOTOR_SERVICE_CMD_MOVE: {
        uint16_t value = motor_speed_to_register(request->direction,
                                                 request->speed);

        status = modbus_master_write_single_register(&g_motor_service.modbus,
                                                     g_motor_service.slave_addr,
                                                     MOTOR_REGISTER_ADDR,
                                                     value,
                                                     MOTOR_RESPONSE_TIMEOUT_MS);
        if (status == MODBUS_OK) {
            LOGI("motor move direction=%d speed=%u reg=0x%04X",
                 request->direction,
                 (unsigned)request->speed,
                 (unsigned)value);
        } else {
            LOGW("motor move failed: %s", modbus_status_string(status));
        }
        break;
    }

    case MOTOR_SERVICE_CMD_STOP:
        status = modbus_master_write_single_register(&g_motor_service.modbus,
                                                     g_motor_service.slave_addr,
                                                     MOTOR_REGISTER_ADDR,
                                                     MOTOR_STOP_VALUE,
                                                     MOTOR_RESPONSE_TIMEOUT_MS);
        if (status == MODBUS_OK) {
            LOGI("motor stop");
        } else {
            LOGW("motor stop failed: %s", modbus_status_string(status));
        }
        break;

    case MOTOR_SERVICE_CMD_TRIGGER: {
        const struct timespec pulse_delay = {
            MOTOR_TRIGGER_PULSE_MS / 1000,
            (long)(MOTOR_TRIGGER_PULSE_MS % 1000) * 1000000L
        };

        status = modbus_master_write_single_coil(&g_motor_service.modbus,
                                                 g_motor_service.slave_addr,
                                                 MOTOR_TRIGGER_COIL_ADDR,
                                                 1,
                                                 MOTOR_RESPONSE_TIMEOUT_MS);
        if (status == MODBUS_OK) {
            (void)nanosleep(&pulse_delay, NULL);
            status = modbus_master_write_single_coil(&g_motor_service.modbus,
                                                     g_motor_service.slave_addr,
                                                     MOTOR_TRIGGER_COIL_ADDR,
                                                     0,
                                                     MOTOR_RESPONSE_TIMEOUT_MS);
        }
        if (status == MODBUS_OK) {
            LOGI("optocoupler trigger pulse");
        } else {
            LOGW("optocoupler trigger failed: %s", modbus_status_string(status));
        }
        break;
    }

    default:
        break;
    }
}

static void *motor_service_thread(void *opaque) {
    motor_service_context_t *service = (motor_service_context_t *)opaque;

    for (;;) {
        motor_service_request_t request;

        pthread_mutex_lock(&service->lock);
        while (service->queue_count == 0U && !service->stop_requested) {
            pthread_cond_wait(&service->condition, &service->lock);
        }
        if (service->stop_requested) {
            pthread_mutex_unlock(&service->lock);
            break;
        }
        request = service->requests[service->queue_head];
        service->queue_head =
            (service->queue_head + 1U) % MOTOR_SERVICE_QUEUE_CAPACITY;
        --service->queue_count;
        pthread_mutex_unlock(&service->lock);

        motor_service_handle_request(&request);
    }

    modbus_rtu_close(&service->modbus);
    pthread_mutex_lock(&service->lock);
    service->running = 0;
    service->thread_started = 0;
    service->queue_head = 0U;
    service->queue_tail = 0U;
    service->queue_count = 0U;
    pthread_mutex_unlock(&service->lock);
    return NULL;
}

modbus_status_t motor_service_init(const char *device,
                                   uint32_t baudrate,
                                   uint8_t slave_addr) {
    modbus_status_t status;

    if (device == NULL || device[0] == '\0' || baudrate == 0U || slave_addr == 0U) {
        return MODBUS_ERR_PARAM;
    }

    pthread_mutex_lock(&g_motor_service.lock);
    if (g_motor_service.running) {
        pthread_mutex_unlock(&g_motor_service.lock);
        return MODBUS_OK;
    }
    g_motor_service.queue_head = 0U;
    g_motor_service.queue_tail = 0U;
    g_motor_service.queue_count = 0U;
    g_motor_service.stop_requested = 0;
    g_motor_service.slave_addr = slave_addr;
    pthread_mutex_unlock(&g_motor_service.lock);

    status = modbus_rtu_init(&g_motor_service.modbus,
                             device,
                             baudrate,
                             UART_PARITY_NONE,
                             8U,
                             1U);
    if (status != MODBUS_OK) {
        LOGW("motor modbus open %s failed: %s",
             device,
             modbus_status_string(status));
        return status;
    }
    LOGI("motor modbus initialized: %s baud=%u slave=%u",
         device,
         (unsigned)baudrate,
         (unsigned)slave_addr);
    return MODBUS_OK;
}

modbus_status_t motor_service_start(void) {
    pthread_mutex_lock(&g_motor_service.lock);
    if (g_motor_service.running) {
        pthread_mutex_unlock(&g_motor_service.lock);
        return MODBUS_OK;
    }
    if (!modbus_rtu_is_open(&g_motor_service.modbus)) {
        pthread_mutex_unlock(&g_motor_service.lock);
        return MODBUS_ERR_IO;
    }
    g_motor_service.running = 1;
    if (pthread_create(&g_motor_service.thread,
                       NULL,
                       motor_service_thread,
                       &g_motor_service) != 0) {
        g_motor_service.running = 0;
        pthread_mutex_unlock(&g_motor_service.lock);
        modbus_rtu_close(&g_motor_service.modbus);
        return MODBUS_ERR_IO;
    }
    g_motor_service.thread_started = 1;
    pthread_mutex_unlock(&g_motor_service.lock);
    LOGI("motor service started");
    return MODBUS_OK;
}

void motor_service_stop(void) {
    pthread_t thread;
    int thread_started;

    pthread_mutex_lock(&g_motor_service.lock);
    thread_started = g_motor_service.thread_started;
    thread = g_motor_service.thread;
    g_motor_service.stop_requested = 1;
    pthread_cond_signal(&g_motor_service.condition);
    pthread_mutex_unlock(&g_motor_service.lock);

    if (thread_started) {
        (void)pthread_join(thread, NULL);
    } else {
        modbus_rtu_close(&g_motor_service.modbus);
        pthread_mutex_lock(&g_motor_service.lock);
        g_motor_service.running = 0;
        g_motor_service.queue_head = 0U;
        g_motor_service.queue_tail = 0U;
        g_motor_service.queue_count = 0U;
        pthread_mutex_unlock(&g_motor_service.lock);
    }
}

modbus_status_t motor_service_request_move(int direction, uint8_t speed) {
    if (direction == 0) {
        return motor_service_request_stop();
    }
    return motor_service_push_request(MOTOR_SERVICE_CMD_MOVE, direction, speed);
}

modbus_status_t motor_service_request_stop(void) {
    return motor_service_push_request(MOTOR_SERVICE_CMD_STOP, 0, 0U);
}

modbus_status_t motor_service_request_trigger(void) {
    return motor_service_push_request(MOTOR_SERVICE_CMD_TRIGGER, 0, 0U);
}

const char *motor_service_status_string(modbus_status_t status) {
    return modbus_status_string(status);
}
