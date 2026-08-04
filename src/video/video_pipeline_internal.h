#ifndef VIDEO_PIPELINE_INTERNAL_H
#define VIDEO_PIPELINE_INTERNAL_H

#include "video/video_pipeline.h"

#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>

#include "input/input_csi0.h"
#include "input/input_csi1.h"
#include "input/input_hdmi_in.h"
#include "input/input_usb.h"
#include "output/output_encode_common.h"
#include "output/output_osd_rkrga.h"

#define VP_QUEUE_SIZE 8
#define VP_RTP_MTU 1400
#define VP_DEFAULT_DIR "/tmp"
#define VP_DEFAULT_RTSP_URL "rtsp://127.0.0.1:8554/live"
#define VP_STREAM_DEFAULT_BITRATE 4000000
#define VP_RECORD_4K_BITRATE 16000000
#define VP_RECORD_HD_BITRATE 8000000

typedef struct {
    AVFrame *frames[VP_QUEUE_SIZE];
    int head;
    int count;
    int max_count;
    int dropped;
} vp_frame_queue_t;

typedef struct vp_channel {
    struct vp_ctx *owner;
    vp_channel_id_t id;
    char name[APP_NAME_MAX_LEN];
    char osd_label[128];
    video_input_config_t input;
    video_encode_params_t record_output;
    char record_dir[APP_PATH_MAX_LEN];
    char snapshot_dir[APP_PATH_MAX_LEN];
    union {
        input_usb_ctx_t usb;
        input_csi0_ctx_t csi0;
        input_csi1_ctx_t csi1;
        input_hdmi_in_ctx_t hdmi_in;
    } in;
    int is_opened;

    pthread_t cap_thread;
    uint64_t cap_frames;
    AVFrame *latest;

    pthread_t rec_thread;
    int record_initializing;
    int record_wanted;
    int recording;
    char rec_path[APP_PATH_MAX_LEN];
    int64_t rec_start_us;
    int64_t rec_end_us;
    vp_frame_queue_t rec_q;
    pthread_cond_t rec_cond;

    output_encode_convert_ctx_t rec_convert;
    AVCodecContext *rec_enc;
    AVFormatContext *rec_fmt;
    AVStream *rec_stream;
    AVPacket *rec_pkt;
    int rec_header_written;
    int rec_frames;
    int rec_bitrate;
} vp_channel_t;

struct vp_ctx {
    vp_config_t cfg;
    char storage_dir[APP_PATH_MAX_LEN];

    pthread_mutex_t lock;
    int stop;
    int channel_count;
    vp_channel_t *ch;

    pthread_t stream_thread;
    vp_frame_queue_t stream_q;
    pthread_cond_t stream_cond;
    int stream_enabled;
    vp_channel_id_t stream_ch;
    int switch_pending;
    uint64_t stream_frames;

    pthread_mutex_t osd_lock;
    vp_osd_params_t osd;

    output_encode_convert_ctx_t stream_convert;
    output_osd_rkrga_t *stream_osd_rkrga;
    AVFrame *stream_osd_rgba;
    uint64_t stream_osd_revision;
    output_osd_rkrga_t *stream_label_rkrga;
    AVFrame *stream_label_rgba;
    vp_channel_id_t stream_label_channel;
    uint64_t stream_label_revision;
    AVCodecContext *stream_enc;
    AVPacket *stream_pkt;
    int64_t stream_epoch_us;

    AVFormatContext *rtsp_fmt;
    AVStream *rtsp_stream;
    int rtsp_header_written;

    int udp_fd;
    struct sockaddr_in udp_addr;
    uint16_t rtp_seq;
    uint32_t rtp_ssrc;
};

void vp_queue_push_locked(vp_frame_queue_t *q, const AVFrame *frame);
void vp_queue_push_latest_locked(vp_frame_queue_t *q, const AVFrame *frame);
int vp_queue_pop_locked(vp_frame_queue_t *q, AVFrame *dst);
void vp_queue_clear_locked(vp_frame_queue_t *q);
void vp_queue_free(vp_frame_queue_t *q);

app_status_t vp_channel_open(vp_channel_t *ch);
app_status_t vp_channel_read(vp_channel_t *ch, video_frame_t *frame);
void vp_channel_close(vp_channel_t *ch);
void *vp_capture_thread(void *opaque);

app_status_t vp_open_h264_encoder(AVCodecContext **out_enc,
                                  int width,
                                  int height,
                                  int fps,
                                  int bitrate,
                                  int gop,
                                  enum AVPixelFormat pix_fmt,
                                  AVBufferRef *hw_frames_ctx,
                                  int global_header);

int vp_osd_needed(vp_ctx_t *p);
void vp_osd_draw(vp_ctx_t *p, AVFrame *f);
void vp_osd_get_snapshot(vp_ctx_t *p, vp_osd_params_t *out);
app_status_t vp_osd_render_rgba(const vp_osd_params_t *params,
                                uint64_t recording_seconds,
                                int recording,
                                int blink_on,
                                int snapshot_flash_on,
                                AVFrame *rgba_frame);
app_status_t vp_osd_render_source_label(const char *text,
                                        const char *font_path,
                                        int font_size,
                                        AVFrame *rgba_frame);

int vp_udp_open(vp_ctx_t *p, const char *ip, int port);
void vp_rtp_send_packet(vp_ctx_t *p, const uint8_t *data, int size, int64_t pts_us);
app_status_t vp_rtsp_open(vp_ctx_t *p, const char *url);
app_status_t vp_rtsp_write_packet(vp_ctx_t *p, AVPacket *pkt);
void vp_rtsp_close(vp_ctx_t *p);
void *vp_stream_thread(void *opaque);

int vp_mkdir_p(const char *dir);
int vp_make_dated_path(const char *base_dir,
                       char *out,
                       size_t out_size,
                       const char *ext,
                       time_t now);

#endif
