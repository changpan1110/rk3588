#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "mp4_encode_test.c"

/*
 * 录像 / 停止 / 拍照 交互式验证工具。
 *
 * 这个程序只是 output_recorder API 的一个命令行外壳:
 * stdin 输入命令,内部全部走 output_recorder_record_start / stop / snapshot。
 * 按键通知、串口命令等外部线程要接入时,照这个文件的方式调 API 即可。
 *
 * 命令:
 *   record [file.mp4]   开始录像(不带文件名则存到 storage_dir 下按时间戳命名)
 *   stop                停止录像(异步,写完回调里打印 record saved)
 *   snapshot [file.jpg] 拍照(同上,可省略文件名)
 *   quit                退出(录像中会先正常写完 mp4 再退出)
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>

#include "common/common.h"
#include "common/debug.h"
#include "output/output_recorder.h"

static volatile sig_atomic_t g_stop_requested = 0;

static void mp4_encode_test_handle_signal(int signo) {
    (void)signo;
    g_stop_requested = 1;
}

static void mp4_encode_test_print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [<usb|csi0|csi1|hdmi_in> <device> <format> <width> <height> <fps> [storage_dir]]\n"
            "Default:\n"
            "  %s -> hdmi_in /dev/video40 driver-default, storage /tmp\n"
            "Examples:\n"
            "  %s usb /dev/video41 mjpeg 1280 720 30\n"
            "  %s csi0 /dev/video22 nv12 3840 2160 30 /media/sd\n",
            prog, prog, prog, prog);
}

static void mp4_encode_test_print_help(void) {
    printf("commands:\n"
           "  record [file.mp4]    start recording (auto name in storage dir if omitted)\n"
           "  stop                 stop recording (mp4 is finalized asynchronously)\n"
           "  snapshot [file.jpg]  save current frame as JPEG\n"
           "  quit                 exit (finalizes an active recording first)\n");
    fflush(stdout);
}

static void mp4_encode_test_on_event(output_recorder_event_t event, const char *path, void *user_data) {
    (void)user_data;

    switch (event) {
        case OUTPUT_RECORDER_EVENT_RECORD_STARTED:
            printf("recording -> %s\n", path);
            break;
        case OUTPUT_RECORDER_EVENT_RECORD_FINISHED:
            printf("record saved: %s\n", path);
            break;
        case OUTPUT_RECORDER_EVENT_RECORD_CANCELED:
            printf("record canceled: no frames, no file written\n");
            break;
        case OUTPUT_RECORDER_EVENT_SNAPSHOT_SAVED:
            printf("snapshot saved: %s\n", path);
            break;
        case OUTPUT_RECORDER_EVENT_ERROR:
            printf("recorder error%s%s\n", path != NULL ? ": " : "", path != NULL ? path : "");
            break;
        default:
            break;
    }
    fflush(stdout);
}

static app_status_t mp4_encode_test_parse_args(output_recorder_config_t *cfg, int argc, char **argv) {
    if (cfg == NULL || argv == NULL) {
        return APP_ERR_PARAM;
    }
    if (argc != 1 && argc != 7 && argc != 8) {
        mp4_encode_test_print_usage(argv[0]);
        return APP_ERR_PARAM;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->encoder_name = "h264_rkmpp";
    cfg->bitrate = 4000000;
    cfg->storage_dir = "/tmp";

    if (argc == 1) {
        strncpy(cfg->input.name, "hdmi_in0", sizeof(cfg->input.name) - 1);
        strncpy(cfg->input.device, "/dev/video40", sizeof(cfg->input.device) - 1);
        cfg->input.source_type = VIDEO_SOURCE_HDMI_IN;
        return APP_OK;
    }

    if (strcmp(argv[1], "usb") == 0) {
        cfg->input.source_type = VIDEO_SOURCE_USB;
        strncpy(cfg->input.name, "usb0", sizeof(cfg->input.name) - 1);
    } else if (strcmp(argv[1], "csi0") == 0) {
        cfg->input.source_type = VIDEO_SOURCE_CSI0;
        strncpy(cfg->input.name, "csi0", sizeof(cfg->input.name) - 1);
    } else if (strcmp(argv[1], "csi1") == 0) {
        cfg->input.source_type = VIDEO_SOURCE_CSI1;
        strncpy(cfg->input.name, "csi1", sizeof(cfg->input.name) - 1);
    } else if (strcmp(argv[1], "hdmi_in") == 0) {
        cfg->input.source_type = VIDEO_SOURCE_HDMI_IN;
        strncpy(cfg->input.name, "hdmi_in0", sizeof(cfg->input.name) - 1);
    } else {
        mp4_encode_test_print_usage(argv[0]);
        return APP_ERR_PARAM;
    }

    strncpy(cfg->input.device, argv[2], sizeof(cfg->input.device) - 1);
    strncpy(cfg->input.input_format, argv[3], sizeof(cfg->input.input_format) - 1);
    cfg->input.width = atoi(argv[4]);
    cfg->input.height = atoi(argv[5]);
    cfg->input.fps = atoi(argv[6]);
    if (argc == 8) {
        cfg->storage_dir = argv[7];
    }
    return APP_OK;
}

int main(int argc, char **argv) {
    output_recorder_config_t cfg;
    output_recorder_ctx_t *recorder = NULL;
    app_status_t status;
    char line[256];
    char *cmd;
    char *arg;

    status = mp4_encode_test_parse_args(&cfg, argc, argv);
    if (status != APP_OK) {
        return EXIT_FAILURE;
    }
    cfg.event_cb = mp4_encode_test_on_event;

    signal(SIGINT, mp4_encode_test_handle_signal);
    signal(SIGTERM, mp4_encode_test_handle_signal);

#ifdef HAVE_LIBAVDEVICE
    extern void avdevice_register_all(void);
    avdevice_register_all();
#endif
    avformat_network_init();

    status = output_recorder_init(&recorder, &cfg);
    if (status != APP_OK) {
        LOGE("output_recorder_init failed: %s", app_status_str(status));
        avformat_network_deinit();
        return EXIT_FAILURE;
    }

    mp4_encode_test_print_help();
    while (!g_stop_requested && fgets(line, sizeof(line), stdin) != NULL) {
        cmd = strtok(line, " \t\r\n");
        if (cmd == NULL) {
            continue;
        }
        arg = strtok(NULL, " \t\r\n");

        if (strcmp(cmd, "record") == 0) {
            status = output_recorder_record_start(recorder, arg);
            if (status == APP_ERR_BUSY) {
                printf("already recording, use stop first\n");
                fflush(stdout);
            } else if (status != APP_OK) {
                printf("record start failed: %s\n", app_status_str(status));
                fflush(stdout);
            }
            continue;
        }

        if (strcmp(cmd, "stop") == 0) {
            status = output_recorder_record_stop(recorder);
            if (status == APP_ERR_IO) {
                printf("not recording\n");
                fflush(stdout);
            }
            continue;
        }

        if (strcmp(cmd, "snapshot") == 0) {
            status = output_recorder_snapshot(recorder, arg);
            if (status != APP_OK) {
                printf("snapshot failed: %s\n", app_status_str(status));
                fflush(stdout);
            }
            continue;
        }

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            break;
        }

        if (strcmp(cmd, "help") == 0) {
            mp4_encode_test_print_help();
            continue;
        }

        printf("unknown command: %s\n", cmd);
        fflush(stdout);
    }

    output_recorder_deinit(recorder);
    avformat_network_deinit();
    return EXIT_SUCCESS;
}
