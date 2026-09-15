#ifndef PROCESS_VIDEO_PIPELINE_H
#define PROCESS_VIDEO_PIPELINE_H

#include "common/common.h"

/*
 * 多路视频管线，每路输入和录像输出参数独立配置。
 *
 * 编码拓扑:
 *   HDMI: 录像编码器(4K,record 时才启动)+ 推流编码器(1080p,独立常驻)
 *   CSI:  录像编码器(1080p,record 时才启动)
 *   USB:  录像编码器(1080p,record 时才启动)
 *   推流: 独立 1080p 编码器,任意时刻只推选中的一路;
 *         切换只换输入源 + 强制 IDR,码流参数连续 -> 播放器无感(无缝切换);
 *         OSD 只画在推流这一路,录像干净。
 *
 * 外部线程(按键/串口)只需要调下面几个函数,全部线程安全。
 */

#define VP_CHANNEL_INVALID (-1)
#define VP_OSD_YAW_MIN_DEG (-140.0f)
#define VP_OSD_YAW_MAX_DEG (140.0f)
#define VP_OSD_PITCH_MIN_DEG (-90.0f)
#define VP_OSD_PITCH_MAX_DEG (30.0f)

typedef int vp_channel_id_t;

typedef enum {
    VP_STREAM_OUTPUT_UDP = 0,
    VP_STREAM_OUTPUT_RTP = VP_STREAM_OUTPUT_UDP,
    VP_STREAM_OUTPUT_RTSP = 1,
    VP_STREAM_OUTPUT_SRT = 2
} vp_stream_output_t;

/* OSD 参数:串口线程拿到新数据就调 vp_osd_update,推流画面实时更新 */
typedef struct {
    int show_crosshair;     /* 显示无人机式十字准星 */
    int cross_x;            /* 十字中心 x,-1 = 画面中心 */
    int cross_y;            /* 十字中心 y,-1 = 画面中心 */
    float yaw_deg;          /* Clamped to VP_OSD_YAW_MIN/MAX_DEG for display. */
    float pitch_deg;        /* Clamped to VP_OSD_PITCH_MIN/MAX_DEG for display. */
    int yaw_valid;          /* Non-zero when yaw_deg contains live data. */
    int pitch_valid;        /* Non-zero when pitch_deg contains live data. */
    float distance_m;       /* 激光测距(米),供后续文本叠加 */
    int laser_valid;        /* Non-zero when distance_m contains live data. */
    int signal_level;       /* 信号强度,供后续文本叠加 */
    int laser_continuous;   /* 连续测量开启时原点红绿闪烁 */
    int laser_measuring;    /* 单次测量进行中,原点红绿快闪 */
    int laser_error;        /* 测量失败时黄点提示 */
    char text[64];          /* 自定义文本(文本渲染下一步加) */
    int64_t snapshot_flash_until_us; /* successful snapshot indicator deadline */
    uint32_t seq;           /* 更新序号,vp_osd_update 自动 +1 */
} vp_osd_params_t;

typedef struct {
    video_input_config_t input;
    video_encode_params_t record_output;   /* width/height=0:保持输入尺寸; fps<=0:跟随输入; bitrate<=0:自动 */
    const char *record_dir;                /* NULL uses storage_dir/record/<name> */
    const char *snapshot_dir;              /* NULL uses storage_dir/snapshot/<name> */
    const char *osd_label;                 /* UTF-8 source label; NULL uses input.name */
    int enabled;                           /* 0 = skip this channel (JSON enabled=false) */
} vp_channel_config_t;

typedef struct {
    int enabled;
    int x;
    int y;
    int width;
    int height;
    int font_size;
    const char *font_path;
} vp_source_label_config_t;

typedef struct {
    video_encode_params_t video;           /* 推流编码输出参数 */
    vp_stream_output_t transport;          /* UDP/RTP, RTSP or SRT */
    char udp_dest_ip[64];                  /* UDP/RTP destination IP */
    int udp_dest_port;                     /* UDP/RTP destination port */
    char rtp_dest_ip[64];                  /* Legacy compatibility; use udp_dest_ip */
    int rtp_dest_port;                     /* Legacy compatibility; use udp_dest_port */
    char rtsp_url[APP_PATH_MAX_LEN];        /* RTSP publish URL */
    char rtsp_transport[8];                /* "tcp" (default) or "udp" */
    char srt_url[APP_PATH_MAX_LEN];         /* MPEG-TS over SRT URL */
    char srt_passphrase_file[APP_PATH_MAX_LEN]; /* Local passphrase file */
} vp_stream_config_t;

typedef struct {
    int channel_count;                     /* 运行时通道数量，>0 */
    const vp_channel_config_t *channels;   /* 指向 channel_count 个通道配置 */
    vp_stream_config_t stream_output;
    vp_source_label_config_t source_label;
    const char *storage_dir;               /* 录像/拍照默认目录,NULL = "/tmp",自动创建 */
} vp_config_t;

typedef struct vp_ctx vp_ctx_t;

app_status_t vp_init(vp_ctx_t **out_ctx, const vp_config_t *cfg);

/* path=NULL: <record_dir>/YYYYMMDD/YYYYMMDD_HHMMSS.mp4 */
app_status_t vp_record_start(vp_ctx_t *ctx, vp_channel_id_t ch, const char *mp4_path);
/* dir!=NULL: <dir>/<channel>/YYYYMMDD/YYYYMMDD_HHMMSS.mp4 */
app_status_t vp_record_start_all(vp_ctx_t *ctx, const char *dir);

typedef struct {
    char channel[APP_NAME_MAX_LEN];
    char path[APP_PATH_MAX_LEN];
    int64_t start_time_us;
    int64_t end_time_us;
    int64_t duration_us;
    uint64_t total_frames;
    int dropped_frames;
    int queue_peak;
    int queue_capacity;
    double average_fps;
} vp_record_stats_t;

app_status_t vp_record_stop(vp_ctx_t *ctx, vp_channel_id_t ch);   /* 返回时文件已写完,可直接用 */
app_status_t vp_record_stop_all(vp_ctx_t *ctx);
app_status_t vp_record_stop_ex(vp_ctx_t *ctx,
                               vp_channel_id_t ch,
                               vp_record_stats_t *out_stats);
app_status_t vp_record_stop_all_ex(vp_ctx_t *ctx,
                                   vp_record_stats_t *stats,
                                   int stats_capacity,
                                   int *out_count);
int vp_is_recording(vp_ctx_t *ctx, vp_channel_id_t ch);
app_status_t vp_record_get_path(vp_ctx_t *ctx,
                                vp_channel_id_t ch,
                                char *out_path,
                                size_t out_size);

/* path=NULL: <snapshot_dir>/YYYYMMDD/YYYYMMDD_HHMMSS.jpg */
app_status_t vp_snapshot(vp_ctx_t *ctx, vp_channel_id_t ch, const char *jpg_path);
app_status_t vp_snapshot_ex(vp_ctx_t *ctx,
                            vp_channel_id_t ch,
                            const char *jpg_path,
                            char *out_path,
                            size_t out_size);

/* 推流切换:无缝(连续码流 + 切换点强制关键帧) */
app_status_t vp_stream_select(vp_ctx_t *ctx, vp_channel_id_t ch);
vp_channel_id_t vp_stream_current(vp_ctx_t *ctx);
app_status_t vp_stream_set_enabled(vp_ctx_t *ctx, int enabled);
int vp_stream_is_enabled(vp_ctx_t *ctx);

/* OSD 参数更新(串口线程调用),只影响推流,不进录像 */
app_status_t vp_osd_update(vp_ctx_t *ctx, const vp_osd_params_t *params);
app_status_t vp_osd_get(vp_ctx_t *ctx, vp_osd_params_t *out_params);
/* Frequent telemetry updates. These modify only their own OSD fields. */
app_status_t vp_osd_set_gimbal_angles(vp_ctx_t *ctx,
                                      float yaw_deg,
                                      float pitch_deg,
                                      int valid);
app_status_t vp_osd_set_laser_distance(vp_ctx_t *ctx,
                                       float distance_m,
                                       int signal_level,
                                       int valid);
app_status_t vp_osd_set_laser_continuous(vp_ctx_t *ctx, int active);
app_status_t vp_osd_set_laser_measuring(vp_ctx_t *ctx, int measuring);
app_status_t vp_osd_set_laser_error(vp_ctx_t *ctx, int error);
app_status_t vp_osd_notify_snapshot(vp_ctx_t *ctx);

int vp_channel_count(vp_ctx_t *ctx);
const char *vp_channel_name(vp_ctx_t *ctx, vp_channel_id_t ch);
int vp_channel_find(vp_ctx_t *ctx, const char *name, vp_channel_id_t *out);

/* 状态统计:给一行刷新式的状态显示用 */
typedef struct {
    uint64_t captured;      /* 累计采集帧数 */
    int recording;          /* 是否录像中 */
    int rec_frames;         /* 本次录像已编码帧数 */
    int rec_dropped;        /* 录像队列丢弃帧数 */
    double capture_cpu_ms;
    double capture_cpu_load;
    double read_ms;
    double decode_ms;
    double record_encode_ms;
    int record_queue;
    int record_queue_peak;
    int record_dropped;
} vp_channel_stats_t;

void vp_get_channel_stats(vp_ctx_t *ctx, vp_channel_id_t ch, vp_channel_stats_t *out);

typedef struct {
    uint64_t frames;
    double cpu_ms;
    double cpu_load;
    double queue_ms;
    double convert_ms;
    double overlay_ms;
    double encode_ms;
    double send_ms;
    double packet_age_ms;
    double packet_age_max_ms;
    int queue_drop;
} vp_stream_stats_t;

void vp_get_stream_stats(vp_ctx_t *ctx, vp_stream_stats_t *out);
uint64_t vp_get_stream_frames(vp_ctx_t *ctx);

void vp_deinit(vp_ctx_t *ctx);

#endif
