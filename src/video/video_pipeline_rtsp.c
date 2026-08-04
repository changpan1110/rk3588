#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "video_pipeline_rtsp.c"

#include "video_pipeline_internal.h"

#include <string.h>

#include <libavutil/opt.h>

#include "common/debug.h"

app_status_t vp_rtsp_open(vp_ctx_t *p, const char *url) {
    AVDictionary *opts = NULL;
    const char *publish_url;
    int ret;

    if (p == NULL || p->stream_enc == NULL) {
        return APP_ERR_PARAM;
    }

    publish_url = (url != NULL && url[0] != '\0') ? url : VP_DEFAULT_RTSP_URL;

    ret = avformat_alloc_output_context2(&p->rtsp_fmt, NULL, "rtsp", publish_url);
    if (ret < 0 || p->rtsp_fmt == NULL) {
        LOGE("rtsp alloc output failed: %d url=%s", ret, publish_url);
        vp_rtsp_close(p);
        return APP_ERR_FFMPEG;
    }
    p->rtsp_fmt->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    p->rtsp_fmt->max_delay = 0;

    p->rtsp_stream = avformat_new_stream(p->rtsp_fmt, NULL);
    if (p->rtsp_stream == NULL) {
        vp_rtsp_close(p);
        return APP_ERR_NOMEM;
    }
    p->rtsp_stream->time_base = p->stream_enc->time_base;
    p->rtsp_stream->avg_frame_rate = p->stream_enc->framerate;

    ret = avcodec_parameters_from_context(p->rtsp_stream->codecpar, p->stream_enc);
    if (ret < 0) {
        LOGE("rtsp parameters_from_context failed: %d", ret);
        vp_rtsp_close(p);
        return APP_ERR_FFMPEG;
    }

    av_dict_set(&opts, "rtsp_transport", "udp", 0);
    av_dict_set(&opts, "muxdelay", "0", 0);
    av_dict_set(&opts, "flush_packets", "1", 0);
    av_dict_set(&opts, "stimeout", "3000000", 0);

    ret = avformat_write_header(p->rtsp_fmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        LOGE("rtsp write_header failed: %d url=%s", ret, publish_url);
        vp_rtsp_close(p);
        return APP_ERR_IO;
    }

    p->rtsp_header_written = 1;
    LOGI("rtsp publish opened: %s", publish_url);
    return APP_OK;
}

app_status_t vp_rtsp_write_packet(vp_ctx_t *p, AVPacket *pkt) {
    int ret;

    if (p == NULL || p->rtsp_fmt == NULL || p->rtsp_stream == NULL || pkt == NULL) {
        return APP_ERR_PARAM;
    }

    av_packet_rescale_ts(pkt, p->stream_enc->time_base, p->rtsp_stream->time_base);
    pkt->stream_index = p->rtsp_stream->index;

    ret = av_write_frame(p->rtsp_fmt, pkt);
    if (ret < 0) {
        LOGW("rtsp write packet failed: %d", ret);
        return APP_ERR_IO;
    }
    return APP_OK;
}

void vp_rtsp_close(vp_ctx_t *p) {
    if (p == NULL) {
        return;
    }

    if (p->rtsp_fmt != NULL && p->rtsp_header_written) {
        av_write_trailer(p->rtsp_fmt);
    }
    if (p->rtsp_fmt != NULL) {
        avformat_free_context(p->rtsp_fmt);
        p->rtsp_fmt = NULL;
    }
    p->rtsp_stream = NULL;
    p->rtsp_header_written = 0;
}
