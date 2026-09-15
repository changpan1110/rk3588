#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "rtsp_output.c"

#include <stdio.h>
#include <string.h>

#include <libavutil/error.h>
#include <libavutil/opt.h>

#include "common/debug.h"
#include "output/stream/rtsp/rtsp_output.h"

app_status_t rtsp_output_init(rtsp_output_ctx_t *ctx,
                              const char *url,
                              const char *transport,
                              const AVCodecContext *encoder) {
    AVDictionary *options = NULL;
    const char *selected_transport;
    int ret;

    if (ctx == NULL || url == NULL || url[0] == '\0' || encoder == NULL) {
        return APP_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->url, url, sizeof(ctx->url) - 1);
    selected_transport = transport != NULL && transport[0] != '\0'
                             ? transport
                             : "tcp";
    if (strcmp(selected_transport, "tcp") != 0 &&
        strcmp(selected_transport, "udp") != 0) {
        LOGW("unsupported RTSP transport '%s', falling back to tcp",
             selected_transport);
        selected_transport = "tcp";
    }
    strncpy(ctx->transport,
            selected_transport,
            sizeof(ctx->transport) - 1);
    ctx->input_time_base = encoder->time_base;

    ret = avformat_alloc_output_context2(&ctx->format_ctx, NULL, "rtsp", ctx->url);
    if (ret < 0 || ctx->format_ctx == NULL) {
        LOGE("rtsp alloc output failed: %d url=%s", ret, ctx->url);
        rtsp_output_deinit(ctx);
        return APP_ERR_FFMPEG;
    }
    ctx->format_ctx->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    ctx->format_ctx->max_delay = 0;

    ctx->stream = avformat_new_stream(ctx->format_ctx, NULL);
    if (ctx->stream == NULL) {
        rtsp_output_deinit(ctx);
        return APP_ERR_NOMEM;
    }
    ctx->stream->time_base = encoder->time_base;
    ctx->stream->avg_frame_rate = encoder->framerate;

    ret = avcodec_parameters_from_context(ctx->stream->codecpar, encoder);
    if (ret < 0) {
        LOGE("rtsp parameters_from_context failed: %d", ret);
        rtsp_output_deinit(ctx);
        return APP_ERR_FFMPEG;
    }

    av_dict_set(&options, "rtsp_transport", ctx->transport, 0);
    av_dict_set(&options, "muxdelay", "0", 0);
    av_dict_set(&options, "flush_packets", "1", 0);
    av_dict_set(&options, "stimeout", "3000000", 0);

    ret = avformat_write_header(ctx->format_ctx, &options);
    av_dict_free(&options);
    if (ret < 0) {
        LOGE("rtsp write_header failed: %d url=%s", ret, ctx->url);
        rtsp_output_deinit(ctx);
        return APP_ERR_IO;
    }

    ctx->header_written = 1;
    LOGI("rtsp publish opened: %s transport=%s",
         ctx->url,
         ctx->transport);
    return APP_OK;
}

app_status_t rtsp_output_send(rtsp_output_ctx_t *ctx, AVPacket *packet) {
    AVPacket output_packet = {0};
    char error_buffer[AV_ERROR_MAX_STRING_SIZE];
    int ret;

    if (!rtsp_output_is_open(ctx) || packet == NULL) {
        return APP_ERR_PARAM;
    }

    ret = av_packet_ref(&output_packet, packet);
    if (ret < 0) {
        return APP_ERR_NOMEM;
    }
    av_packet_rescale_ts(&output_packet,
                         ctx->input_time_base,
                         ctx->stream->time_base);
    output_packet.stream_index = ctx->stream->index;

    ret = av_write_frame(ctx->format_ctx, &output_packet);
    av_packet_unref(&output_packet);
    if (ret < 0) {
        if (av_strerror(ret, error_buffer, sizeof(error_buffer)) < 0) {
            snprintf(error_buffer, sizeof(error_buffer), "FFmpeg error %d", ret);
        }
        LOGW("rtsp write packet failed: %s", error_buffer);
        return APP_ERR_IO;
    }
    return APP_OK;
}

int rtsp_output_is_open(const rtsp_output_ctx_t *ctx) {
    return ctx != NULL && ctx->format_ctx != NULL && ctx->stream != NULL &&
           ctx->header_written;
}

void rtsp_output_deinit(rtsp_output_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    if (ctx->format_ctx != NULL && ctx->header_written) {
        av_write_trailer(ctx->format_ctx);
    }
    if (ctx->format_ctx != NULL) {
        avformat_free_context(ctx->format_ctx);
    }
    memset(ctx, 0, sizeof(*ctx));
}
