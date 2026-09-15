#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "video_pipeline_main.c"

/*
 * 多路视频管线主程序，默认配置为 USB1 + USB2 + HDMI 常采，
 * 支持录像、拍照、推流切换和 OSD。
 *
 * 用法：
 *   rk3588_video_pipeline_main [/path/to/video_pipeline.json]
 * 每次启动都从 JSON 文件读取通道参数，推流协议和地址由
 * g_pipeline_default_config.stream_output 配置。
 *
 * 控制：
 *   SBUS CH5              点动循环切换 JSON 中前 3 个通道
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
#include "control/high_speed_camera/high_speed_camera.h"
#include "control/sbus/sbus_rc8.h"
#include "control/sfl0603_laser/sfl0603_laser.h"
#include "control/thermal_camera/thermal_camera.h"
#include "control/visca_4k_camera/visca_4k_camera.h"
#include "process/video/video_pipeline.h"
#include "control/video_pipeline/video_pipeline_control.h"
#include "app/sbus_rc8_command.h"
#include "app/video_pipeline_config.h"
#include "service/dji_rsdk_service.h"
#include "service/high_speed_camera_service.h"
#include "service/motor_service.h"
#include "service/sfl0603_laser_service.h"
#include "service/thermal_camera_service.h"
#ifdef HAVE_MAVLINK_CONTROL
#include "control/mavlink/mavlink_control_actions.h"
#include "control/mavlink/mavlink_control_service.h"
#endif

#define VP_MAIN_DEFAULT_DIR "/home/cat/11223344/pipeline_rec"
#define VP_MAIN_DEFAULT_CONFIG_PATH "/home/cat/rk3588/config/video_pipeline.json"

/* 串口设备地址现在从 config/video_pipeline.json 的 "serial" 对象读取，
 * 未在 JSON 中提供的字段使用 video_pipeline_config.c 里的默认值。 */

static volatile sig_atomic_t g_stop_requested = 0;

static const vp_config_t g_pipeline_default_config = {
    .channel_count = 0,
    .channels = NULL,
    .stream_output = {
        .video = {
            .width = 1920,
            .height = 1080,
            .fps = 30,
            .bitrate = 20000000
        },
        /* Only this field selects the active stream output. */
        .transport = VP_STREAM_OUTPUT_RTSP,

        /* Used only when transport == VP_STREAM_OUTPUT_UDP. */
        .udp_dest_ip = "192.168.31.21",
        .udp_dest_port = 5000,

        /* Used only when transport == VP_STREAM_OUTPUT_RTSP. */
        .rtsp_url = "rtsp://127.0.0.1:8554/drone1",
        .rtsp_transport = "udp",

        /* Used only when transport == VP_STREAM_OUTPUT_SRT. */
        .srt_url =
            "srt://192.168.31.21:8890?mode=caller&"
            "streamid=publish:drone1&pkt_size=1316&latency=80000",
        .srt_passphrase_file = ""
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

static void vp_main_handle_signal(int signo) {
    (void)signo;
    g_stop_requested = 1;
}

static int vp_main_parse_port_environment(const char *name,
                                          int *destination) {
    const char *value = getenv(name);
    char *end = NULL;
    long port;

    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    port = strtol(value, &end, 10);
    if (end == value || *end != '\0' || port <= 0 || port > 65535) {
        LOGE("invalid %s=%s", name, value);
        return -1;
    }
    *destination = (int)port;
    return 0;
}

static int vp_main_apply_stream_environment(vp_config_t *cfg) {
    const char *transport;
    const char *value;

    if (cfg == NULL) {
        return -1;
    }
    transport = getenv("RK3588_STREAM_TRANSPORT");
    if (transport != NULL && transport[0] != '\0') {
        if (strcmp(transport, "udp") == 0 || strcmp(transport, "rtp") == 0) {
            cfg->stream_output.transport = VP_STREAM_OUTPUT_UDP;
        } else if (strcmp(transport, "rtsp") == 0) {
            cfg->stream_output.transport = VP_STREAM_OUTPUT_RTSP;
        } else if (strcmp(transport, "srt") == 0) {
            cfg->stream_output.transport = VP_STREAM_OUTPUT_SRT;
        } else {
            LOGE("invalid RK3588_STREAM_TRANSPORT=%s", transport);
            return -1;
        }
    }

    value = getenv("RK3588_UDP_DEST_IP");
    if (value != NULL && value[0] != '\0') {
        snprintf(cfg->stream_output.udp_dest_ip,
                 sizeof(cfg->stream_output.udp_dest_ip),
                 "%s",
                 value);
    }
    if (vp_main_parse_port_environment("RK3588_UDP_DEST_PORT",
                                       &cfg->stream_output.udp_dest_port) != 0) {
        return -1;
    }
    value = getenv("RK3588_RTSP_URL");
    if (value != NULL && value[0] != '\0') {
        snprintf(cfg->stream_output.rtsp_url,
                 sizeof(cfg->stream_output.rtsp_url),
                 "%s",
                 value);
    }
    value = getenv("RK3588_RTSP_TRANSPORT");
    if (value != NULL && value[0] != '\0') {
        if (strcmp(value, "tcp") != 0 && strcmp(value, "udp") != 0) {
            LOGE("invalid RK3588_RTSP_TRANSPORT=%s", value);
            return -1;
        }
        snprintf(cfg->stream_output.rtsp_transport,
                 sizeof(cfg->stream_output.rtsp_transport),
                 "%s",
                 value);
    }
    value = getenv("RK3588_SRT_URL");
    if (value != NULL && value[0] != '\0') {
        snprintf(cfg->stream_output.srt_url,
                 sizeof(cfg->stream_output.srt_url),
                 "%s",
                 value);
    }
    value = getenv("RK3588_SRT_PASSPHRASE_FILE");
    if (value != NULL && value[0] != '\0') {
        snprintf(cfg->stream_output.srt_passphrase_file,
                 sizeof(cfg->stream_output.srt_passphrase_file),
                 "%s",
                 value);
    }
    return 0;
}

static void vp_main_print_help(vp_ctx_t *p, const vp_config_t *cfg) {
    int i;

    printf("console command:\n"
           "  quit                 exit\n"
           "  stream <channel>     select configured channel\n"
           "  record start <channel|all> [path]\n"
           "  record stop <channel|all>\n"
           "SBUS RC8 control:\n"
           "  CH5 press           cycle first 3 configured channels\n"
           "  CH6 press           toggle start/stop all recordings\n"
           "  CH7 rising edge      snapshot selected stream channel\n");
    printf("channels:");
    for (i = 0; i < vp_channel_count(p); ++i) {
        printf(" %s", vp_channel_name(p, (vp_channel_id_t)i));
    }
    printf("\n");
    printf("auto storage:\n");
    for (i = 0; i < vp_channel_count(p); ++i) {
        char record_dir[APP_PATH_MAX_LEN];
        char snapshot_dir[APP_PATH_MAX_LEN];

        if (cfg->channels[i].record_dir != NULL) {
            snprintf(record_dir,
                     sizeof(record_dir),
                     "%s",
                     cfg->channels[i].record_dir);
        } else {
            snprintf(record_dir,
                     sizeof(record_dir),
                     "%s/record/%s",
                     cfg->storage_dir,
                     cfg->channels[i].input.name);
        }
        if (cfg->channels[i].snapshot_dir != NULL) {
            snprintf(snapshot_dir,
                     sizeof(snapshot_dir),
                     "%s",
                     cfg->channels[i].snapshot_dir);
        } else {
            snprintf(snapshot_dir,
                     sizeof(snapshot_dir),
                     "%s/snapshot/%s",
                     cfg->storage_dir,
                     cfg->channels[i].input.name);
        }
        printf("  %-4s record=%s snapshot=%s\n",
               cfg->channels[i].input.name,
               record_dir,
               snapshot_dir);
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

    /* timestamp_us is CLOCK_MONOTONIC; convert it to wall-clock time by
     * subtracting the elapsed monotonic time from the current wall time. */
    seconds = time(NULL) - (time_t)((app_get_time_us() - timestamp_us) / 1000000LL);
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
    uint64_t last_stream_frames = 0;

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
        vp_stream_stats_t stream_stats;
        uint64_t stream_frames;
        uint64_t stream_fps;
        int i;

        sleep(1);
        for (i = 0; i < count; ++i) {
            vp_get_channel_stats(p, (vp_channel_id_t)i, &st);
            fps[i] = st.captured - last[i];
            last[i] = st.captured;
            rec_flag[i] = st.recording ? "REC" : "-";
        }
        vp_get_stream_stats(p, &stream_stats);
        stream_frames = vp_get_stream_frames(p);
        stream_fps = stream_frames - last_stream_frames;
        last_stream_frames = stream_frames;
        printf("\r\033[K");
        for (i = 0; i < count; ++i) {
            printf("%s%s %2llufps %s",
                   i == 0 ? "" : " | ",
                   vp_channel_name(p, (vp_channel_id_t)i),
                   (unsigned long long)fps[i],
                   rec_flag[i]);
            vp_get_channel_stats(p, (vp_channel_id_t)i, &st);
            printf(" cpu=%4.1f%%/%5.1fms read=%5.2fms dec_cpu=%4.2fms",
                   st.capture_cpu_load,
                   st.capture_cpu_ms,
                   st.read_ms,
                   st.decode_ms);
            if (st.recording) {
                printf(" rec_enc=%4.2fms rec_q=%d rec_drop=%d",
                       st.record_encode_ms,
                       st.record_queue,
                       st.record_dropped);
            }
        }
        printf(" | stream=%s %2llufps cpu=%4.1f%%/%5.1fms "
               "q=%4.2fms conv=%4.2fms osd=%4.2fms enc=%4.2fms send=%4.2fms "
               "age=%4.1f/%4.1fms drop=%d   ",
               vp_channel_name(p, vp_stream_current(p)),
               (unsigned long long)stream_fps,
               stream_stats.cpu_load,
               stream_stats.cpu_ms,
               stream_stats.queue_ms,
               stream_stats.convert_ms,
               stream_stats.overlay_ms,
               stream_stats.encode_ms,
               stream_stats.send_ms,
               stream_stats.packet_age_ms,
               stream_stats.packet_age_max_ms,
               stream_stats.queue_drop);
        fflush(stdout);
    }
    printf("\n");
    free(last);
    free(fps);
    free(rec_flag);
    return NULL;
}

int main(int argc, char **argv) {
    vp_config_t cfg = g_pipeline_default_config;
    vp_main_channels_config_t channels_config;
    vp_ctx_t *p = NULL;
    vp_control_t *control = NULL;
#ifdef HAVE_MAVLINK_CONTROL
    mavlink_control_service_t *mavlink_control = NULL;
    mavlink_control_actions_t *mavlink_actions = NULL;
#endif
    app_status_t status;
    // sbus_status_t sbus_status;
    pthread_t status_thread;
    int status_thread_started = 0;
    int dji_rsdk_service_started = 0;
    int high_speed_camera_service_started = 0;
    int thermal_camera_service_started = 0;
    int sfl0603_laser_started = 0;
    int motor_service_started = 0;
    int visca_4k_camera_started = 0;
    // sbus_rc8_t *sbus_rc8 = NULL;
    // sbus_rc8_command_t *sbus_command = NULL;
    // const char *sbus_cycle_channels[3] = {NULL, NULL, NULL};
    const char *config_path = VP_MAIN_DEFAULT_CONFIG_PATH;
    char config_error[256];
    char line[256];
    char *cmd;
    int check_config_only = 0;
    int i;

    if (argc == 1) {
        /* Use the board's default JSON file. */
    } else if (argc == 2) {
        if (strcmp(argv[1], "--check-config") == 0) {
            check_config_only = 1;
        } else {
            config_path = argv[1];
        }
    } else if (argc == 3 &&
               (strcmp(argv[1], "--config") == 0 ||
                strcmp(argv[1], "--check-config") == 0)) {
        check_config_only = strcmp(argv[1], "--check-config") == 0;
        config_path = argv[2];
    } else {
        fprintf(stderr,
                "usage: %s [config.json]\n"
                "       %s --config config.json\n"
                "       %s --check-config [config.json]\n",
                argv[0],
                argv[0],
                argv[0]);
        return EXIT_FAILURE;
    }

    if (vp_main_channels_config_load_json(&channels_config,
                                          config_path,
                                          config_error,
                                          sizeof(config_error)) != 0) {
        LOGE("channel config load failed: %s", config_error);
        return EXIT_FAILURE;
    }
    cfg.channel_count = channels_config.channel_count;
    cfg.channels = channels_config.channels;
    if (channels_config.storage_dir[0] != '\0') {
        cfg.storage_dir = channels_config.storage_dir;
    }
    if (vp_main_apply_stream_environment(&cfg) != 0) {
        return EXIT_FAILURE;
    }
    // SBUS RC8 is disabled on this board; keep channel cycling code
    // commented out together with its type declarations above.
    // for (i = 0; i < 3 && i < cfg.channel_count; ++i) {
    //     sbus_cycle_channels[i] = cfg.channels[i].input.name;
    // }
    LOGI("loaded %d channel(s) from %s", cfg.channel_count, config_path);
    if (check_config_only) {
        printf("config OK: %s\n", config_path);
        printf("  storage_dir=%s\n", cfg.storage_dir);
        for (i = 0; i < cfg.channel_count; ++i) {
            printf("  %s device=%s format=%s input=%dx%d@%d "
                   "record=%dx%d@%d bitrate=%d\n",
                   cfg.channels[i].input.name,
                   cfg.channels[i].input.device,
                   cfg.channels[i].input.input_format,
                   cfg.channels[i].input.width,
                   cfg.channels[i].input.height,
                   cfg.channels[i].input.fps,
                   cfg.channels[i].record_output.width,
                   cfg.channels[i].record_output.height,
                   cfg.channels[i].record_output.fps,
                   cfg.channels[i].record_output.bitrate);
        }
        return EXIT_SUCCESS;
    }

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

    {
        visca_status_t visca_status = visca_4k_camera_init(
            channels_config.serial.visca_4k_camera_device,
            channels_config.serial.visca_4k_camera_baudrate);
        if (visca_status == VISCA_OK) {
            visca_4k_camera_started = 1;
        } else {
            LOGW("VISCA 4K camera disabled: %s",
                 visca_camera_status_string(visca_status));
            visca_4k_camera_deinit();
        }
    }

    {
        int high_speed_status = high_speed_camera_service_init(
            channels_config.serial.high_speed_camera_device,
            channels_config.serial.high_speed_camera_baudrate);
        if (high_speed_status == HIGH_SPEED_CAMERA_OK) {
            high_speed_status = high_speed_camera_service_start();
        }
        if (high_speed_status == HIGH_SPEED_CAMERA_OK) {
            high_speed_camera_service_started = 1;
        } else {
            LOGW("high-speed-camera service disabled: %s",
                 high_speed_camera_status_string(
                     (high_speed_camera_status_t)high_speed_status));
        }
    }

    {
        int thermal_status = thermal_camera_service_init(
            channels_config.serial.thermal_camera_device,
            channels_config.serial.thermal_camera_baudrate);
        if (thermal_status == THERMAL_CAMERA_OK) {
            thermal_status = thermal_camera_service_start();
        }
        if (thermal_status == THERMAL_CAMERA_OK) {
            thermal_camera_service_started = 1;
        } else {
            LOGW("thermal camera service disabled: %s",
                 thermal_camera_status_string(
                     (thermal_camera_status_t)thermal_status));
        }
    }

    {
        sfl0603_laser_status_t laser_status = sfl0603_laser_service_init(
            channels_config.serial.laser_device,
            channels_config.serial.laser_baudrate);
        if (laser_status == SFL0603_LASER_OK) {
            sfl0603_laser_service_set_osd_control(control);
            laser_status = sfl0603_laser_service_start();
        }
        if (laser_status == SFL0603_LASER_OK) {
            sfl0603_laser_started = 1;
        } else {
            LOGW("sfl0603 laser service disabled: %s",
                 sfl0603_laser_status_string(laser_status));
        }
    }

    {
        modbus_status_t motor_status = motor_service_init(
            channels_config.serial.motor_device,
            channels_config.serial.motor_baudrate,
            (uint8_t)channels_config.serial.motor_slave_addr);
        if (motor_status == MODBUS_OK) {
            motor_status = motor_service_start();
        }
        if (motor_status == MODBUS_OK) {
            motor_service_started = 1;
        } else {
            LOGW("motor service disabled: %s",
                 modbus_status_string(motor_status));
        }
    }

    status = vp_control_osd_set_enabled_async(control, 1, NULL);
    if (status != APP_OK) {
        LOGW("initial OSD enable queue failed: %s", app_status_str(status));
    }

#ifdef HAVE_MAVLINK_CONTROL
    status = mavlink_control_actions_init(
        &mavlink_actions,
        p,
        control);
    if (status != APP_OK) {
        LOGE("MAVLink action dispatcher init failed: %s", app_status_str(status));
        if (motor_service_started) {
            motor_service_stop();
        }
        if (sfl0603_laser_started) {
            sfl0603_laser_service_stop();
        }
        if (thermal_camera_service_started) {
            thermal_camera_service_stop();
        }
        if (visca_4k_camera_started) {
            visca_4k_camera_stop();
            visca_4k_camera_deinit();
        }
        if (high_speed_camera_service_started) {
            high_speed_camera_service_stop();
        }
        vp_control_deinit(control);
        vp_deinit(p);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }
    status = mavlink_control_service_init(
        &mavlink_control,
        channels_config.serial.mavlink_config_path,
        mavlink_control_actions_handle,
        mavlink_actions);
    if (status != APP_OK) {
        LOGE("MAVLink control init failed: %s", app_status_str(status));
        mavlink_control_actions_deinit(mavlink_actions);
        if (motor_service_started) {
            motor_service_stop();
        }
        if (sfl0603_laser_started) {
            sfl0603_laser_service_stop();
        }
        if (thermal_camera_service_started) {
            thermal_camera_service_stop();
        }
        if (visca_4k_camera_started) {
            visca_4k_camera_stop();
            visca_4k_camera_deinit();
        }
        if (high_speed_camera_service_started) {
            high_speed_camera_service_stop();
        }
        vp_control_deinit(control);
        vp_deinit(p);
        avformat_network_deinit();
        return EXIT_FAILURE;
    }
#endif

    // sbus_status = sbus_rc8_init(&sbus_rc8, VP_MAIN_SBUS_DEVICE);
    // if (sbus_status != SBUS_OK) {
    //     LOGE("SBUS RC8 init failed: %s",
    //          sbus_status_string(sbus_status));
    // } else {
    //     if (cfg.channel_count < 3) {
    //         LOGW("SBUS RC8 CH5 cycle disabled: at least 3 channels are required");
    //     } else {
    //         status = sbus_rc8_command_init(&sbus_command,
    //                                        sbus_rc8,
    //                                        p,
    //                                        control,
    //                                        sbus_cycle_channels);
    //         if (status != APP_OK) {
    //             LOGE("SBUS RC8 control init failed: %s",
    //                  app_status_str(status));
    //         }
    //     }
    // }

    {
        int dji_status = dji_rsdk_service_init(control);
        if (dji_status == DJI_RSDK_OK) {
            dji_status = dji_rsdk_service_start();
        }
        if (dji_status == DJI_RSDK_OK) {
            dji_rsdk_service_started = 1;
        } else {
            LOGW("DJI RSDK telemetry service disabled: status=%d",
                 dji_status);
        }
    }

    if (pthread_create(&status_thread, NULL, vp_main_status_thread, p) == 0) {
        status_thread_started = 1;
    }

    vp_main_print_help(p, &cfg);
    while (!g_stop_requested && fgets(line, sizeof(line), stdin) != NULL) {
        cmd = strtok(line, " \t\r\n");
        if (cmd == NULL) {
            continue;
        }

        if (strcmp(cmd, "quit") == 0) {
            break;
        }

        if (strcmp(cmd, "stream") == 0) {
            char *channel = strtok(NULL, " \t\r\n");
            uint64_t request_id = 0;

            if (channel == NULL) {
                printf("\nusage: stream <channel>\n");
            } else {
                status = vp_control_stream_select_async(control,
                                                        channel,
                                                        &request_id);
                if (status != APP_OK) {
                    printf("\nstream switch queue failed: %s\n",
                           app_status_str(status));
                } else {
                    printf("\nstream switch queued: %s request=%llu\n",
                           channel,
                           (unsigned long long)request_id);
                }
            }
            fflush(stdout);
            continue;
        }

        if (strcmp(cmd, "record") == 0) {
            char *action = strtok(NULL, " \t\r\n");
            char *channel = strtok(NULL, " \t\r\n");
            char *path = strtok(NULL, " \t\r\n");
            uint64_t request_id = 0;

            if (action == NULL || channel == NULL ||
                (strcmp(action, "start") != 0 && strcmp(action, "stop") != 0)) {
                printf("\nusage: record <start|stop> <channel|all> [path]\n");
            } else if (strcmp(action, "start") == 0) {
                status = strcmp(channel, "all") == 0
                             ? vp_control_record_start_all_async(control,
                                                                 path,
                                                                 &request_id)
                             : vp_control_record_start_async(control,
                                                             channel,
                                                             path,
                                                             &request_id);
                printf("\nrecord start %s: %s request=%llu\n",
                       channel,
                       app_status_str(status),
                       (unsigned long long)request_id);
            } else {
                status = strcmp(channel, "all") == 0
                             ? vp_control_record_stop_all_async(control,
                                                                &request_id)
                             : vp_control_record_stop_async(control,
                                                            channel,
                                                            &request_id);
                printf("\nrecord stop %s: %s request=%llu\n",
                       channel,
                       app_status_str(status),
                       (unsigned long long)request_id);
            }
            fflush(stdout);
            continue;
        }

        printf("\nunknown command; use quit, stream, or record\n");
        fflush(stdout);
    }

    g_stop_requested = 1;
    // sbus_rc8_command_deinit(sbus_command);
    // sbus_rc8_deinit(sbus_rc8);
    if (dji_rsdk_service_started) {
        dji_rsdk_service_stop();
    }
    if (status_thread_started) {
        pthread_join(status_thread, NULL);
    }
#ifdef HAVE_MAVLINK_CONTROL
    mavlink_control_service_deinit(mavlink_control);
    mavlink_control_actions_deinit(mavlink_actions);
#endif
    if (motor_service_started) {
        motor_service_stop();
    }
    if (sfl0603_laser_started) {
        sfl0603_laser_service_stop();
    }
    if (thermal_camera_service_started) {
        thermal_camera_service_stop();
    }
    if (visca_4k_camera_started) {
        visca_4k_camera_stop();
        visca_4k_camera_deinit();
    }
    if (high_speed_camera_service_started) {
        high_speed_camera_service_stop();
    }
    vp_control_deinit(control);
    vp_deinit(p);
    avformat_network_deinit();
    return EXIT_SUCCESS;
}
