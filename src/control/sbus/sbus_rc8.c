// #define LOG_FILE_NAME "sbus_rc8.c"

// #include "control/sbus/sbus_rc8.h"

// #include <errno.h>
// #include <pthread.h>
// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <time.h>
// #include <unistd.h>

// #include "common/debug.h"

// #define SBUS_RC8_RETRY_US 10000
// #define SBUS_RC8_RETRY_COUNT 100
// #define SBUS_RC8_DEVICE_PATH_MAX 128
// #define SBUS_RC8_TIMING_LOG_US 1000000LL
// #define SBUS_RC8_STALL_LOG_US 500000LL
// #define SBUS_RC8_ANALOG_LOG_THRESHOLD 32

// typedef struct {
//     uint16_t latest_value;
//     pthread_cond_t cond;
//     int pending;
// } sbus_rc8_channel_state_t;

// struct sbus_rc8 {
//     pthread_t receive_thread;
//     pthread_mutex_t lock;
//     char device[SBUS_RC8_DEVICE_PATH_MAX];
//     sbus_rc8_channel_state_t channels[SBUS_RC8_CHANNEL_COUNT];
//     int stop_requested;
//     int receive_thread_initialized;
// };

// typedef struct {
//     int64_t opened_us;
//     int64_t last_frame_us;
//     int64_t window_start_us;
//     int64_t last_stall_log_us;
//     int64_t period_sum_us;
//     int64_t period_min_us;
//     int64_t period_max_us;
//     uint64_t window_frames;
//     uint64_t period_samples;
//     uint64_t frame_lost_count;
//     uint64_t failsafe_count;
//     uint16_t latest_channels[SBUS_RC8_CHANNEL_COUNT];
//     uint16_t last_logged_channels[SBUS_RC8_CHANNEL_COUNT];
//     int channels_initialized;
// } sbus_rc8_timing_t;

// static int64_t sbus_rc8_monotonic_us(void) {
//     struct timespec now;

//     if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
//         return 0;
//     }
//     return (int64_t)now.tv_sec * 1000000LL + now.tv_nsec / 1000LL;
// }

// static void sbus_rc8_timing_reset(sbus_rc8_timing_t *timing) {
//     int64_t now_us = sbus_rc8_monotonic_us();

//     memset(timing, 0, sizeof(*timing));
//     timing->opened_us = now_us;
//     timing->window_start_us = now_us;
//     timing->period_min_us = INT64_MAX;
// }

// static void sbus_rc8_log_channel_changes(sbus_rc8_timing_t *timing,
//                                          const sbus_frame_t *frame,
//                                          int64_t now_us) {
//     sbus_controls_t controls;
//     unsigned int channel;

//     sbus_channels_decode(frame, &controls);
//     if (!timing->channels_initialized) {
//         memcpy(timing->last_logged_channels,
//                frame->channels,
//                sizeof(timing->last_logged_channels));
//         timing->channels_initialized = 1;
//         LOGD("SBUS RX first frame mono_us=%lld "
//              "CH1=%u CH2=%u CH3=%u CH4=%u CH5=%u CH6=%u CH7=%u CH8=%u",
//              (long long)now_us,
//              frame->channels[0], frame->channels[1],
//              frame->channels[2], frame->channels[3],
//              frame->channels[4], frame->channels[5],
//              frame->channels[6], frame->channels[7]);
//         return;
//     }

//     for (channel = 0; channel < SBUS_RC8_CHANNEL_COUNT; ++channel) {
//         uint16_t previous = timing->last_logged_channels[channel];
//         uint16_t current = frame->channels[channel];

//         if (channel < 4U) {
//             int difference = (int)current - (int)previous;
//             int mapped = channel < 2U
//                              ? controls.joystick[channel]
//                              : controls.knob[channel - 2U];

//             if (difference < 0) {
//                 difference = -difference;
//             }
//             if (difference < SBUS_RC8_ANALOG_LOG_THRESHOLD) {
//                 continue;
//             }
//             LOGD("SBUS RX change mono_us=%lld CH%u raw=%u mapped=%d",
//                  (long long)now_us,
//                  channel + 1U,
//                  current,
//                  mapped);
//         } else {
//             int previous_pressed = previous >= SBUS_CHANNEL_CENTER;
//             int current_pressed = current >= SBUS_CHANNEL_CENTER;

//             if (previous_pressed == current_pressed) {
//                 continue;
//             }
//             LOGD("SBUS RX change mono_us=%lld CH%u raw=%u pressed=%d",
//                  (long long)now_us,
//                  channel + 1U,
//                  current,
//                  current_pressed);
//         }
//         timing->last_logged_channels[channel] = current;
//     }
// }

// static void sbus_rc8_timing_frame(sbus_rc8_timing_t *timing,
//                                   const sbus_frame_t *frame) {
//     int64_t now_us = sbus_rc8_monotonic_us();
//     int64_t window_us;
//     unsigned int channel;

//     if (timing->last_frame_us > 0 && now_us > timing->last_frame_us) {
//         int64_t period_us = now_us - timing->last_frame_us;

//         timing->period_sum_us += period_us;
//         timing->period_samples++;
//         if (period_us < timing->period_min_us) {
//             timing->period_min_us = period_us;
//         }
//         if (period_us > timing->period_max_us) {
//             timing->period_max_us = period_us;
//         }
//     }
//     timing->last_frame_us = now_us;
//     timing->window_frames++;
//     timing->frame_lost_count += frame->frame_lost != 0;
//     timing->failsafe_count += frame->failsafe != 0;
//     for (channel = 0; channel < SBUS_RC8_CHANNEL_COUNT; ++channel) {
//         timing->latest_channels[channel] = frame->channels[channel];
//     }

//     if (!frame->frame_lost && !frame->failsafe) {
//         sbus_rc8_log_channel_changes(timing, frame, now_us);
//     }

//     window_us = now_us - timing->window_start_us;
//     if (window_us < SBUS_RC8_TIMING_LOG_US) {
//         return;
//     }

//     LOGD("SBUS RX timing frames=%llu rate=%.1fHz period_ms "
//          "avg=%.3f min=%.3f max=%.3f lost=%llu failsafe=%llu "
//          "CH1=%u CH2=%u CH3=%u CH4=%u CH5=%u CH6=%u CH7=%u CH8=%u",
//          (unsigned long long)timing->window_frames,
//          window_us > 0
//              ? (double)timing->window_frames * 1000000.0 / (double)window_us
//              : 0.0,
//          timing->period_samples > 0
//              ? (double)timing->period_sum_us /
//                    (double)timing->period_samples / 1000.0
//              : 0.0,
//          timing->period_samples > 0
//              ? (double)timing->period_min_us / 1000.0
//              : 0.0,
//          timing->period_samples > 0
//              ? (double)timing->period_max_us / 1000.0
//              : 0.0,
//          (unsigned long long)timing->frame_lost_count,
//          (unsigned long long)timing->failsafe_count,
//          timing->latest_channels[0], timing->latest_channels[1],
//          timing->latest_channels[2], timing->latest_channels[3],
//          timing->latest_channels[4], timing->latest_channels[5],
//          timing->latest_channels[6], timing->latest_channels[7]);

//     timing->window_start_us = now_us;
//     timing->window_frames = 0;
//     timing->period_sum_us = 0;
//     timing->period_min_us = INT64_MAX;
//     timing->period_max_us = 0;
//     timing->period_samples = 0;
//     timing->frame_lost_count = 0;
//     timing->failsafe_count = 0;
// }

// static void sbus_rc8_timing_no_frame(sbus_rc8_timing_t *timing) {
//     int64_t now_us = sbus_rc8_monotonic_us();
//     int64_t reference_us = timing->last_frame_us > 0
//                                ? timing->last_frame_us
//                                : timing->opened_us;
//     int64_t stalled_us = now_us - reference_us;

//     if (stalled_us < SBUS_RC8_STALL_LOG_US ||
//         now_us - timing->last_stall_log_us < SBUS_RC8_TIMING_LOG_US) {
//         return;
//     }
//     timing->last_stall_log_us = now_us;
//     LOGW("SBUS RX stalled mono_us=%lld no_complete_frame_ms=%.1f",
//          (long long)now_us,
//          (double)stalled_us / 1000.0);
// }

// static int sbus_rc8_should_stop(sbus_rc8_t *rc8) {
//     int stop_requested;

//     pthread_mutex_lock(&rc8->lock);
//     stop_requested = rc8->stop_requested;
//     pthread_mutex_unlock(&rc8->lock);
//     return stop_requested;
// }

// static void sbus_rc8_retry_wait(sbus_rc8_t *rc8) {
//     int count;

//     for (count = 0; count < SBUS_RC8_RETRY_COUNT; ++count) {
//         if (sbus_rc8_should_stop(rc8)) {
//             break;
//         }
//         usleep(SBUS_RC8_RETRY_US);
//     }
// }

// static void *sbus_rc8_receive_thread(void *opaque) {
//     sbus_rc8_t *rc8 = (sbus_rc8_t *)opaque;
//     sbus_data_t data;
//     sbus_frame_t frame;
//     sbus_status_t status;
//     sbus_rc8_timing_t timing;

//     uart_base_config_init(&data.uart);
//     sbus_parser_init(&data.parser);
//     while (!sbus_rc8_should_stop(rc8)) {
//         status = sbus_data_init(&data, rc8->device);
//         if (status != SBUS_OK) {
//             LOGW("SBUS receiver init %s failed: %s; retrying",
//                  rc8->device,
//                  sbus_status_string(status));
//             sbus_rc8_retry_wait(rc8);
//             continue;
//         }

//         sbus_rc8_timing_reset(&timing);
//         LOGI("SBUS receive initialized: %s baud=%d format=8E2",
//              rc8->device,
//              SBUS_BAUDRATE);
//         while (!sbus_rc8_should_stop(rc8)) {
//             status = sbus_data_receive(&data, &frame);
//             if (status == SBUS_OK) {
//                 unsigned int channel;

//                 sbus_rc8_timing_frame(&timing, &frame);
//                 if (frame.frame_lost || frame.failsafe) {
//                     continue;
//                 }
//                 pthread_mutex_lock(&rc8->lock);
//                 for (channel = 0;
//                      channel < SBUS_RC8_CHANNEL_COUNT;
//                      ++channel) {
//                     sbus_rc8_channel_state_t *channel_state =
//                         &rc8->channels[channel];

//                     channel_state->latest_value = frame.channels[channel];
//                     channel_state->pending = 1;
//                     pthread_cond_signal(&channel_state->cond);
//                 }
//                 pthread_mutex_unlock(&rc8->lock);
//             } else if (status == SBUS_NO_FRAME) {
//                 sbus_rc8_timing_no_frame(&timing);
//             } else {
//                 LOGW("SBUS receive %s failed: %s; reinitializing",
//                      rc8->device,
//                      sbus_status_string(status));
//                 break;
//             }
//         }

//         sbus_data_deinit(&data);
//         if (!sbus_rc8_should_stop(rc8)) {
//             sbus_rc8_retry_wait(rc8);
//         }
//     }

//     sbus_data_deinit(&data);
//     return NULL;
// }

// sbus_status_t sbus_rc8_init(sbus_rc8_t **out_rc8,
//                             const char *device) {
//     sbus_rc8_t *rc8;
//     unsigned int channel;

//     if (out_rc8 == NULL) {
//         return SBUS_ERR_PARAM;
//     }
//     *out_rc8 = NULL;
//     if (device == NULL || device[0] == '\0' ||
//         strlen(device) >= SBUS_RC8_DEVICE_PATH_MAX) {
//         return SBUS_ERR_PARAM;
//     }

//     rc8 = (sbus_rc8_t *)calloc(1, sizeof(*rc8));
//     if (rc8 == NULL) {
//         return SBUS_ERR_NOMEM;
//     }
//     snprintf(rc8->device, sizeof(rc8->device), "%s", device);

//     if (pthread_mutex_init(&rc8->lock, NULL) != 0) {
//         free(rc8);
//         return SBUS_ERR_IO;
//     }
//     for (channel = 0; channel < SBUS_RC8_CHANNEL_COUNT; ++channel) {
//         if (pthread_cond_init(&rc8->channels[channel].cond, NULL) != 0) {
//             while (channel > 0) {
//                 channel--;
//                 pthread_cond_destroy(&rc8->channels[channel].cond);
//             }
//             pthread_mutex_destroy(&rc8->lock);
//             free(rc8);
//             return SBUS_ERR_IO;
//         }
//     }
//     if (pthread_create(&rc8->receive_thread,
//                        NULL,
//                        sbus_rc8_receive_thread,
//                        rc8) != 0) {
//         for (channel = 0; channel < SBUS_RC8_CHANNEL_COUNT; ++channel) {
//             pthread_cond_destroy(&rc8->channels[channel].cond);
//         }
//         pthread_mutex_destroy(&rc8->lock);
//         free(rc8);
//         return SBUS_ERR_IO;
//     }
//     rc8->receive_thread_initialized = 1;
//     *out_rc8 = rc8;
//     return SBUS_OK;
// }

// sbus_status_t sbus_rc8_channel_receive(
//     sbus_rc8_t *rc8,
//     unsigned int channel_number,
//     uint16_t *channel_value,
//     int timeout_ms) {
//     sbus_rc8_channel_state_t *channel_state;
//     struct timespec deadline;
//     int wait_status = 0;

//     if (rc8 == NULL || channel_value == NULL ||
//         channel_number == 0 || channel_number > SBUS_RC8_CHANNEL_COUNT ||
//         timeout_ms < 0) {
//         return SBUS_ERR_PARAM;
//     }
//     channel_state = &rc8->channels[channel_number - 1U];

//     if (timeout_ms > 0) {
//         if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
//             return SBUS_ERR_IO;
//         }
//         deadline.tv_sec += timeout_ms / 1000;
//         deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
//         if (deadline.tv_nsec >= 1000000000L) {
//             deadline.tv_sec++;
//             deadline.tv_nsec -= 1000000000L;
//         }
//     }

//     pthread_mutex_lock(&rc8->lock);
//     while (!channel_state->pending && !rc8->stop_requested) {
//         if (timeout_ms == 0) {
//             pthread_mutex_unlock(&rc8->lock);
//             return SBUS_NO_FRAME;
//         }
//         wait_status = pthread_cond_timedwait(&channel_state->cond,
//                                              &rc8->lock,
//                                              &deadline);
//         if (wait_status == ETIMEDOUT) {
//             pthread_mutex_unlock(&rc8->lock);
//             return SBUS_NO_FRAME;
//         }
//         if (wait_status != 0) {
//             pthread_mutex_unlock(&rc8->lock);
//             return SBUS_ERR_IO;
//         }
//     }

//     if (!channel_state->pending) {
//         pthread_mutex_unlock(&rc8->lock);
//         return SBUS_NO_FRAME;
//     }
//     *channel_value = channel_state->latest_value;
//     channel_state->pending = 0;
//     pthread_mutex_unlock(&rc8->lock);
//     return SBUS_OK;
// }

// void sbus_rc8_deinit(sbus_rc8_t *rc8) {
//     unsigned int channel;

//     if (rc8 == NULL) {
//         return;
//     }
//     if (rc8->receive_thread_initialized) {
//         pthread_mutex_lock(&rc8->lock);
//         rc8->stop_requested = 1;
//         for (channel = 0; channel < SBUS_RC8_CHANNEL_COUNT; ++channel) {
//             pthread_cond_broadcast(&rc8->channels[channel].cond);
//         }
//         pthread_mutex_unlock(&rc8->lock);
//         pthread_join(rc8->receive_thread, NULL);
//         rc8->receive_thread_initialized = 0;
//     }
//     for (channel = 0; channel < SBUS_RC8_CHANNEL_COUNT; ++channel) {
//         pthread_cond_destroy(&rc8->channels[channel].cond);
//     }
//     pthread_mutex_destroy(&rc8->lock);
//     free(rc8);
// }
