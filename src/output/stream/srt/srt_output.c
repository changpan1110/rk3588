#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "srt_output.c"

#include <stdio.h>
#include <string.h>

#include <libavutil/error.h>

#include "common/debug.h"
#include "output/stream/srt/srt_output.h"

#define SRT_OUTPUT_PASSPHRASE_MIN 10
#define SRT_OUTPUT_PASSPHRASE_MAX 79
#define SRT_OUTPUT_AES_KEY_BYTES 32

static const char *srt_output_error_string(int error,
                                           char *buffer,
                                           size_t buffer_size) {
    if (av_strerror(error, buffer, buffer_size) < 0) {
        snprintf(buffer, buffer_size, "FFmpeg error %d", error);
    }
    return buffer;
}

static app_status_t srt_output_load_passphrase(const char *path,
                                               char *passphrase,
                                               size_t capacity) {
    FILE *file;
    size_t bytes_read;

    if (path == NULL || path[0] == '\0' || passphrase == NULL ||
        capacity <= SRT_OUTPUT_PASSPHRASE_MAX) {
        return APP_ERR_PARAM;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        LOGE("SRT passphrase file open failed: %s", path);
        return APP_ERR_IO;
    }
    bytes_read = fread(passphrase, 1, capacity - 1, file);
    if (ferror(file)) {
        fclose(file);
        memset(passphrase, 0, capacity);
        LOGE("SRT passphrase file read failed: %s", path);
        return APP_ERR_IO;
    }
    fclose(file);

    passphrase[bytes_read] = '\0';
    while (bytes_read > 0 &&
           (passphrase[bytes_read - 1] == '\n' ||
            passphrase[bytes_read - 1] == '\r')) {
        passphrase[--bytes_read] = '\0';
    }
    if (strlen(passphrase) != bytes_read ||
        bytes_read < SRT_OUTPUT_PASSPHRASE_MIN ||
        bytes_read > SRT_OUTPUT_PASSPHRASE_MAX) {
        LOGE("SRT passphrase in %s must contain %d..%d bytes",
             path,
             SRT_OUTPUT_PASSPHRASE_MIN,
             SRT_OUTPUT_PASSPHRASE_MAX);
        memset(passphrase, 0, capacity);
        return APP_ERR_PARAM;
    }
    return APP_OK;
}

app_status_t srt_output_init(srt_output_ctx_t *ctx,
                             const char *url,
                             const char *passphrase_file,
                             const AVCodecContext *encoder) {
    AVDictionary *io_options = NULL;
    AVDictionary *mux_options = NULL;
    char passphrase[SRT_OUTPUT_PASSPHRASE_MAX + 2] = {0};
    char error_buffer[AV_ERROR_MAX_STRING_SIZE];
    int encrypted;
    int ret;

    if (ctx == NULL || url == NULL || url[0] == '\0' || encoder == NULL) {
        return APP_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));
    snprintf(ctx->url, sizeof(ctx->url), "%s", url);
    if (passphrase_file != NULL) {
        snprintf(ctx->passphrase_file,
                 sizeof(ctx->passphrase_file),
                 "%s",
                 passphrase_file);
    }
    encrypted = ctx->passphrase_file[0] != '\0';
    if (encrypted) {
        app_status_t status = srt_output_load_passphrase(
            ctx->passphrase_file,
            passphrase,
            sizeof(passphrase));
        if (status != APP_OK) {
            return status;
        }
    }

    ret = avformat_alloc_output_context2(&ctx->format_ctx,
                                         NULL,
                                         "mpegts",
                                         ctx->url);
    if (ret < 0 || ctx->format_ctx == NULL) {
        LOGE("SRT MPEG-TS output allocation failed: %s",
             srt_output_error_string(ret,
                                     error_buffer,
                                     sizeof(error_buffer)));
        memset(passphrase, 0, sizeof(passphrase));
        srt_output_deinit(ctx);
        return APP_ERR_FFMPEG;
    }
    ctx->format_ctx->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    ctx->format_ctx->max_delay = 0;
    ctx->input_time_base = encoder->time_base;

    ctx->stream = avformat_new_stream(ctx->format_ctx, NULL);
    if (ctx->stream == NULL) {
        memset(passphrase, 0, sizeof(passphrase));
        srt_output_deinit(ctx);
        return APP_ERR_NOMEM;
    }
    ctx->stream->time_base = encoder->time_base;
    ctx->stream->avg_frame_rate = encoder->framerate;

    ret = avcodec_parameters_from_context(ctx->stream->codecpar, encoder);
    if (ret < 0) {
        LOGE("SRT codec parameter copy failed: %s",
             srt_output_error_string(ret,
                                     error_buffer,
                                     sizeof(error_buffer)));
        memset(passphrase, 0, sizeof(passphrase));
        srt_output_deinit(ctx);
        return APP_ERR_FFMPEG;
    }
    ctx->stream->codecpar->codec_tag = 0;

    if (encrypted) {
        char key_length[8];

        snprintf(key_length, sizeof(key_length), "%d", SRT_OUTPUT_AES_KEY_BYTES);
        av_dict_set(&io_options, "passphrase", passphrase, 0);
        av_dict_set(&io_options, "pbkeylen", key_length, 0);
    }
    ret = avio_open2(&ctx->format_ctx->pb,
                     ctx->url,
                     AVIO_FLAG_WRITE,
                     NULL,
                     &io_options);
    av_dict_free(&io_options);
    memset(passphrase, 0, sizeof(passphrase));
    if (ret < 0) {
        LOGE("SRT connection open failed: %s url=%s",
             srt_output_error_string(ret,
                                     error_buffer,
                                     sizeof(error_buffer)),
             ctx->url);
        srt_output_deinit(ctx);
        return APP_ERR_IO;
    }

    av_dict_set(&mux_options, "mpegts_flags", "+resend_headers", 0);
    av_dict_set(&mux_options, "muxdelay", "0", 0);
    av_dict_set(&mux_options, "muxpreload", "0", 0);
    av_dict_set(&mux_options, "flush_packets", "1", 0);
    ret = avformat_write_header(ctx->format_ctx, &mux_options);
    av_dict_free(&mux_options);
    if (ret < 0) {
        LOGE("SRT MPEG-TS header write failed: %s url=%s",
             srt_output_error_string(ret,
                                     error_buffer,
                                     sizeof(error_buffer)),
             ctx->url);
        srt_output_deinit(ctx);
        return APP_ERR_IO;
    }

    ctx->header_written = 1;
    LOGI("SRT publish opened: %s encryption=%s key_bytes=%d",
         ctx->url,
         encrypted ? "AES" : "off",
         encrypted ? SRT_OUTPUT_AES_KEY_BYTES : 0);
    return APP_OK;
}

app_status_t srt_output_send(srt_output_ctx_t *ctx,
                             const AVPacket *packet) {
    AVPacket output_packet = {0};
    char error_buffer[AV_ERROR_MAX_STRING_SIZE];
    int ret;

    if (!srt_output_is_open(ctx) || packet == NULL) {
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
        LOGW("SRT packet write failed: %s",
             srt_output_error_string(ret,
                                     error_buffer,
                                     sizeof(error_buffer)));
        return APP_ERR_IO;
    }
    return APP_OK;
}

int srt_output_is_open(const srt_output_ctx_t *ctx) {
    return ctx != NULL && ctx->format_ctx != NULL &&
           ctx->format_ctx->pb != NULL && ctx->stream != NULL &&
           ctx->header_written;
}

void srt_output_deinit(srt_output_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    if (ctx->format_ctx != NULL && ctx->header_written) {
        av_write_trailer(ctx->format_ctx);
    }
    if (ctx->format_ctx != NULL && ctx->format_ctx->pb != NULL) {
        avio_closep(&ctx->format_ctx->pb);
    }
    if (ctx->format_ctx != NULL) {
        avformat_free_context(ctx->format_ctx);
    }
    memset(ctx, 0, sizeof(*ctx));
}
