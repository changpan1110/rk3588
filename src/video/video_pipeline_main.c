#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "video_pipeline_main.c"

/*
 * 多路视频管线主程序，默认配置为 USB1 + USB2 + HDMI 常采，
 * 支持录像、拍照、推流切换和 OSD。
 *
 * 用法：
 *   rk3588_video_pipeline_main
 * 推流协议和地址统一由 g_pipeline_default_config.stream_output 配置。
 *
 * 控制：
 *   SBUS CH5              点动循环切换 usb1/usb2/hdmi
 *   SBUS CH6              按键上升沿切换全部录像/停止
 *   SBUS CH7              上升沿拍摄当前选择通道
 *   quit                  退出
 */

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavformat/avformat.h>

#include "common/common.h"
#include "common/debug.h"
#include "serial/sbus_rc8.h"
#include "video/video_pipeline.h"
#include "video/video_pipeline_control.h"
#include "app/sbus_rc8_command.h"

#define VP_MAIN_DEFAULT_DIR "/tmp/pipeline_rec"
#define VP_MAIN_USB1_RECORD_DIR VP_MAIN_DEFAULT_DIR "/record/usb1"
#define VP_MAIN_USB2_RECORD_DIR VP_MAIN_DEFAULT_DIR "/record/usb2"
#define VP_MAIN_HDMI_RECORD_DIR VP_MAIN_DEFAULT_DIR "/record/hdmi"
#define VP_MAIN_USB1_SNAPSHOT_DIR VP_MAIN_DEFAULT_DIR "/snapshot/usb1"
#define VP_MAIN_USB2_SNAPSHOT_DIR VP_MAIN_DEFAULT_DIR "/snapshot/usb2"
#define VP_MAIN_HDMI_SNAPSHOT_DIR VP_MAIN_DEFAULT_DIR "/snapshot/hdmi"
#define VP_MAIN_USB1_DEVICE "/dev/video41"
#define VP_MAIN_USB2_DEVICE "/dev/video43"
#define VP_MAIN_HDMI_DEVICE "/dev/video40"
#define VP_MAIN_SBUS_DEVICE "/dev/ttyACM0"

static volatile sig_atomic_t g_stop_requested = 0;

static const vp_channel_config_t g_pipeline_default_channels[] = {
    {
        .input = {
            .name = "usb1",
            .device = VP_MAIN_USB1_DEVICE,
            .input_format = "mjpeg",
            .width = 1920,
            .height = 1080,
            .fps = 30,
            .source_type = VIDEO_SOURCE_USB
        },
        .record_output = {
            .width = 0,
            .height = 0,
            .fps = 30,
            .bitrate = 8000000
        },
        .record_dir = VP_MAIN_USB1_RECORD_DIR,
        .snapshot_dir = VP_MAIN_USB1_SNAPSHOT_DIR,
        .osd_label = "USB1 摄像头"
    },
    {
        /* Both configured USB cameras capture MJPEG at 1080p. */
        .input = {
            .name = "usb2",
            .device = VP_MAIN_USB2_DEVICE,
            .input_format = "mjpeg",
            .width = 1920,
            .height = 1080,
            .fps = 30,
            .source_type = VIDEO_SOURCE_USB
        },
        .record_output = {
            .width = 0,
            .height = 0,
            .fps = 30,
            .bitrate = 8000000
        },
        .record_dir = VP_MAIN_USB2_RECORD_DIR,
        .snapshot_dir = VP_MAIN_USB2_SNAPSHOT_DIR,
        .osd_label = "USB2 摄像头"
    },
    {
        .input = {
            .name = "hdmi",
            .device = VP_MAIN_HDMI_DEVICE,
            .input_format = "",
            .width = 0,
            .height = 0,
            .fps = 30,
            .source_type = VIDEO_SOURCE_HDMI_IN,
            .use_native_v4l2 = 1
        },
        .record_output = {
            .width = 0,
            .height = 0,
            .fps = 30,
            .bitrate = 16000000
        },
        .record_dir = VP_MAIN_HDMI_RECORD_DIR,
        .snapshot_dir = VP_MAIN_HDMI_SNAPSHOT_DIR,
        .osd_label = "HDMI 输入"
    }
};

static const vp_config_t g_pipeline_default_config = {
    .channel_count = (int)(sizeof(g_pipeline_default_channels) /
                           sizeof(g_pipeline_default_channels[0])),
    .channels = g_pipeline_default_channels,
    .stream_output = {
        .video = {
            .width = 1920,
            .height = 1080,
            .fps = 30,
            .bitrate = 4000000
        },
        .transport = VP_STREAM_OUTPUT_RTSP,
        .rtp_dest_ip = "192.168.31.100",
        .rtp_dest_port = 5000,
        .rtsp_url = "rtsp://127.0.0.1:8554/live"
    },
    .source_label = {
        .enabled = 1,
        .x = 32,
        .y = 32,
        .width = 360,
        .height = 72,
        .font_size = 38,
        .font_path = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
    },
    .storage_dir = VP_MAIN_DEFAULT_DIR
};

static const char *const g_sbus_rc8_cycle_channels[3] = {
    "usb1",
    "usb2",
    "hdmi"
};

static void vp_main_handle_signal(int signo) {
    (void)signo;
    g_stop_requested = 1;
}

static void vp_main_print_help(vp_ctx_t *p) {
    int i;

    printf("console command:\n"
           "  quit                 exit\n"
           "SBUS RC8 control:\n"
           "  CH5 press           cycle usb1/usb2/hdmi\n"
           "  CH6 press           toggle start/stop all recordings\n"
           "  CH7 rising edge      snapshot selected stream channel\n");
    printf("channels:");
    for (i = 0; i < vp_channel_count(p); ++i) {
        printf(" %s", vp_channel_name(p, (vp_channel_id_t)i));
    }
    printf("\n");
    printf("auto storage:\n");
    for (i = 0; i < vp_channel_count(p); ++i) {
        printf("  %-4s record=%s snapshot=%s\n",
               g_pipeline_default_channels[i].input.name,
               g_pipeline_default_channels[i].record_dir,
               g_pipeline_default_channels[i].snapshot_dir);
    }
    fflush(stdout);
}

static void vp_main_format_record_time(int64_t timestamp_us, char *out, size_t out_size) {
    time_t seconds;
    struct tm local_time;
    char date_time[32];

    if (out == NULL || out_size == 0) {
        return;
    }
    if (timestamp_us <= 0) {
        snprintf(out, out_size, "unknown");
        return;
    }

    seconds = (time_t)(timestamp_us / 1000000LL);
    if (localtime_r(&seconds, &local_time) == NULL ||
        strftime(date_time, sizeof(date_time), "%Y-%m-%d %H:%M:%S", &local_time) == 0) {
        snprintf(out, out_size, "unknown");
        return;
    }
    snprintf(out,
             out_size,
             "%s.%03lld",
             date_time,
             (long long)((timestamp_us / 1000LL) % 1000LL));
}

static void vp_main_print_record_stats(const vp_record_stats_t *stats) {
    char start_time[48];
    char end_time[48];
    int64_t total_ms;
    long long hours;
    long long minutes;
    long long seconds;
    long long millis;

    if (stats == NULL) {
        return;
    }

    vp_main_format_record_time(stats->start_time_us, start_time, sizeof(start_time));
    vp_main_format_record_time(stats->end_time_us, end_time, sizeof(end_time));
    total_ms = stats->duration_us > 0 ? stats->duration_us / 1000LL : 0;
    hours = (long long)(total_ms / 3600000LL);
    minutes = (long long)((total_ms / 60000LL) % 60LL);
    seconds = (long long)((total_ms / 1000LL) % 60LL);
    millis = (long long)(total_ms % 1000LL);

    printf("\nrecord stopped: %s\n"
           "  start:     %s\n"
           "  end:       %s\n"
           "  duration:  %02lld:%02lld:%02lld.%03lld\n"
           "  frames:    %llu\n"
           "  avg fps:   %.2f\n"
           "  dropped:   %d\n"
           "  queue peak:%d/%d\n"
           "  file:      %s\n",
           stats->channel,
           start_time,
           end_time,
           hours,
           minutes,
           seconds,
           millis,
           (unsigned long long)stats->total_frames,
           stats->average_fps,
           stats->dropped_frames,
           stats->queue_peak,
           stats->queue_capacity,
           stats->path);
}

static void vp_main_control_event(const vp_control_event_t *event, void *opaque) {
    (void)opaque;

    if (event == NULL) {
        return;
    }
    switch (event->type) {
        case VP_CONTROL_EVENT_RECORD_START_COMPLETE:
            if (event->status == APP_OK) {
                printf("\n[event %llu] record started: %s -> %s\n",
                       (unsigned long long)event->request_id,
                       event->channel,
                       event->path);
            } else {
                printf("\n[event %llu] record start failed: %s (%s)\n",
                       (unsigned long long)event->request_id,
                       event->channel,
                       app_status_str(event->status));
            }
            break;
        case VP_CONTROL_EVENT_RECORD_START_ALL_COMPLETE:
            printf("\n[event %llu] record all %s: active=%d root=%s\n",
                   (unsigned long long)event->request_id,
                   event->status == APP_OK ? "complete" : "failed",
                   event->completed_count,
                   event->path[0] != '\0' ? event->path : "configured directories");
            break;
        case VP_CONTROL_EVENT_RECORD_STOP_COMPLETE:
            if (event->status == APP_OK) {
                vp_main_print_record_stats(&event->record_stats);
            } else {
                printf("\n[event %llu] record stop failed: %s (%s)\n",
                       (unsigned long long)event->request_id,
                       event->channel,
                       app_status_str(event->status));
            }
            break;
        case VP_CONTROL_EVENT_RECORD_STOP_ALL_COMPLETE:
            printf("\n[event %llu] stop all %s: stopped=%d\n",
                   (unsigned long long)event->request_id,
                   event->status == APP_OK ? "complete" : "failed",
                   event->completed_count);
            break;
        case VP_CONTROL_EVENT_SNAPSHOT_COMPLETE:
            printf("\n[event %llu] snapshot %s: %s%s%s\n",
                   (unsigned long long)event->request_id,
                   event->status == APP_OK ? "saved" : "failed",
                   event->channel,
                   event->status == APP_OK ? " -> " : " ",
                   event->status == APP_OK ? event->path : app_status_str(event->status));
            break;
        case VP_CONTROL_EVENT_STREAM_SELECT_COMPLETE:
            printf("\n[event %llu] stream switch %s: %s\n",
                   (unsigned long long)event->request_id,
                   event->status == APP_OK ? "complete" : "failed",
                   event->status == APP_OK ? event->channel : app_status_str(event->status));
            break;
        case VP_CONTROL_EVENT_STREAM_ENABLE_COMPLETE:
            printf("\n[event %llu] stream %s: %s\n",
                   (unsigned long long)event->request_id,
                   event->enabled ? "on" : "off",
                   app_status_str(event->status));
            break;
        case VP_CONTROL_EVENT_OSD_ENABLE_COMPLETE:
            printf("\n[event %llu] OSD %s: %s\n",
                   (unsigned long long)event->request_id,
                   event->enabled ? "on" : "off",
                   app_status_str(event->status));
            break;
        case VP_CONTROL_EVENT_OSD_POSITION_COMPLETE:
            printf("\n[event %llu] OSD position (%d,%d): %s\n",
                   (unsigned long long)event->request_id,
                   event->x,
                   event->y,
                   app_status_str(event->status));
            break;
        default:
            break;
    }
    fflush(stdout);
}

static void *vp_main_status_thread(void *opaque) {
    vp_ctx_t *p = (vp_ctx_t *)opaque;
    int count = vp_channel_count(p);
    uint64_t *last;
    uint64_t *fps;
    const char **rec_flag;

    last = (uint64_t *)calloc((size_t)count, sizeof(*last));
    fps = (uint64_t *)calloc((size_t)count, sizeof(*fps));
    rec_flag = (const char **)calloc((size_t)count, sizeof(*rec_flag));
    if (last == NULL || fps == NULL || rec_flag == NULL) {
        free(last);
        free(fps);
        free(rec_flag);
        return NULL;
    }

    while (!g_stop_requested) {
        vp_channel_stats_t st;
        int i;

        sleep(1);
        for (i = 0; i < count; ++i) {
            vp_get_channel_stats(p, (vp_channel_id_t)i, &st);
            fps[i] = st.captured - last[i];
            last[i] = st.captured;
            rec_flag[i] = st.recording ? "REC" : "-";
        }
        printf("\r\033[K");
        for (i = 0; i < count; ++i) {
            printf("%s%s %2llufps %s",
                   i == 0 ? "" : " | ",
                   vp_channel_name(p, (vp_channel_id_t)i),
                   (unsigned long long)fps[i],
                   rec_flag[i]);
        }
        printf(" | stream=%s enc=%llu   ",
               vp_channel_name(p, vp_stream_current(p)),
               (unsigned long long)vp_get_stream_frames(p));
        fflush(stdout);
    }
    printf("\n");
    free(last);
    free(fps);
    free(rec_flag);
    return NULL;
}

int main(void) {
    vp_config_t cfg = g_pipeline_default_config;
    vp_ctx_t *p = NULL;
    vp_control_t *control = NULL;
    app_status_t status;
    sbus_status_t sbus_status;
    pthread_t status_thread;
    int status_thread_started = 0;
    sbus_rc8_t *sbus_rc8 = NULL;
    sbus_rc8_command_t *sbus_command = NULL;
    char line[256];
    char *cmd;

    signal(SIGINT, vp_main_handle_signal);
    signal(SIGTERM, vp_main_handle_signal);

#ifdef HAVE_LIBAVDEVICE
    extern void avdevice_register_all(void);
    avdevice_register_all();
#endif
    avformat_network_init();

    status = vp_init(&p, &cfg);
    if (status != APP_OK) {
        LOGE("vp_init failed: %s", app_status_str(status));
        avformat_network_deinit();
        return EXIT_FAILURE;
    }

    status = vp_control_init(&control, p);
    if (status != APP_OK) {
        LOGE("vp_control_init failed: %s", app_status_str(status));
        vp_deinit(p);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }
    status = vp_control_set_event_callback(control, vp_main_control_event, NULL);
    if (status != APP_OK) {
        LOGE("control event callback setup failed: %s", app_status_str(status));
        vp_control_deinit(control);
        vp_deinit(p);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }
    status = vp_control_osd_set_enabled_async(control, 1, NULL);
    if (status != APP_OK) {
        LOGW("initial OSD enable queue failed: %s", app_status_str(status));
    }

    sbus_status = sbus_rc8_init(&sbus_rc8, VP_MAIN_SBUS_DEVICE);
    if (sbus_status != SBUS_OK) {
        LOGE("SBUS RC8 init failed: %s",
             sbus_status_string(sbus_status));
    } else {
        status = sbus_rc8_command_init(&sbus_command,
                                       sbus_rc8,
                                       p,
                                       control,
                                       g_sbus_rc8_cycle_channels);
        if (status != APP_OK) {
            LOGE("SBUS RC8 control init failed: %s",
                 app_status_str(status));
        }
    }

    if (pthread_create(&status_thread, NULL, vp_main_status_thread, p) == 0) {
        status_thread_started = 1;
    }

    vp_main_print_help(p);
    while (!g_stop_requested && fgets(line, sizeof(line), stdin) != NULL) {
        cmd = strtok(line, " \t\r\n");
        if (cmd == NULL) {
            continue;
        }

        if (strcmp(cmd, "quit") == 0) {
            break;
        }

        printf("\nonly quit is accepted; SBUS RC8 uses CH5/CH6/CH7\n");
        fflush(stdout);
    }

    g_stop_requested = 1;
    sbus_rc8_command_deinit(sbus_command);
    sbus_rc8_deinit(sbus_rc8);
    if (status_thread_started) {
        pthread_join(status_thread, NULL);
    }
    vp_control_deinit(control);
    vp_deinit(p);
    avformat_network_deinit();
    return EXIT_SUCCESS;
}
