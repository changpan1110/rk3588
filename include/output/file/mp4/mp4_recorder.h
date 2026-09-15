#ifndef MP4_RECORDER_H
#define MP4_RECORDER_H

#include "common/common.h"

/*
 * 录像 / 停止 / 拍照 API。
 *
 * 模块内部自带:采集线程 + 编码队列 + h264_rkmpp 编码 + mp4 封装,
 * 外部线程(按键通知、串口命令、UI 事件等)只调函数,不需要自己实现线程:
 *
 *   mp4_recorder_init(&rec, &cfg);
 *   mp4_recorder_record_start(rec, NULL);   // 开始录像,NULL=按时间戳自动命名
 *   mp4_recorder_record_stop(rec);          // 停止录像(异步,写完回调 RECORD_FINISHED)
 *   mp4_recorder_snapshot(rec, NULL);       // 拍照存 JPEG
 *   mp4_recorder_deinit(rec);
 *
 * 说明:
 * - 所有 API 线程安全,可在任意线程调用;
 * - record_start / record_stop 是非阻塞的,真正开始/写完通过 event_cb 回调通知;
 * - snapshot 是同步的(当前帧转 JPEG,几十~几百 ms 取决于分辨率);
 * - 任何时候 deinit / 进程退出都会把 mp4 trailer 写完,不会产生缺 moov 的废文件;
 * - 需要几路同时录,就创建几个实例。
 */

typedef struct mp4_recorder_ctx mp4_recorder_ctx_t;

typedef enum {
    MP4_RECORDER_EVENT_RECORD_STARTED = 0,  /* 开始录像,path=mp4 路径 */
    MP4_RECORDER_EVENT_RECORD_FINISHED,     /* 录像文件已完整写完可播,path=mp4 路径 */
    MP4_RECORDER_EVENT_RECORD_CANCELED,     /* 一帧都没录到,未生成文件 */
    MP4_RECORDER_EVENT_SNAPSHOT_SAVED,      /* 拍照完成,path=jpg 路径 */
    MP4_RECORDER_EVENT_ERROR                /* 出错 */
} mp4_recorder_event_t;

typedef void (*mp4_recorder_event_cb)(mp4_recorder_event_t event, const char *path, void *user_data);

typedef struct {
    video_input_config_t input;          /* 采集源:device/input_format/width/height/fps/source_type */
    const char *encoder_name;            /* NULL -> "h264_rkmpp" */
    int bitrate;                         /* <=0 -> 4000000 */
    int gop;                             /* <=0 -> 输入 fps(或 30) */
    const char *storage_dir;             /* 录像/拍照默认存储目录,NULL 或 "" -> "/tmp",不存在会自动创建 */
    mp4_recorder_event_cb event_cb;   /* 事件回调(在内部线程上下文里被调用),可为 NULL */
    void *event_user_data;
} mp4_recorder_config_t;

/*
 * mp4_path / jpg_path 传 NULL 或 "" 时,
 * 自动落到 cfg.storage_dir 下,按 record_时间戳.mp4 / snapshot_时间戳.jpg 命名;
 * 传具体路径(绝对或相对)时按传入路径存储。
 */
app_status_t mp4_recorder_init(mp4_recorder_ctx_t **out_ctx, const mp4_recorder_config_t *cfg);
app_status_t mp4_recorder_record_start(mp4_recorder_ctx_t *ctx, const char *mp4_path);
app_status_t mp4_recorder_record_stop(mp4_recorder_ctx_t *ctx);
app_status_t mp4_recorder_snapshot(mp4_recorder_ctx_t *ctx, const char *jpg_path);
int mp4_recorder_is_recording(mp4_recorder_ctx_t *ctx);
void mp4_recorder_deinit(mp4_recorder_ctx_t *ctx);

/* 独立工具:把任意格式的一帧图像转成 JPEG 存盘(不依赖 recorder 实例) */
app_status_t mp4_recorder_write_jpeg(const AVFrame *frame, const char *path);

#endif
