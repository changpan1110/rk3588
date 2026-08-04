#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "rga_color_probe.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "common/common.h"
#include "common/debug.h"
#include "output/output_encode_common.h"

#ifdef HAVE_LIBRGA
#include <rga/im2d.h>
#endif

#define PROBE_SOURCE_WIDTH 3840
#define PROBE_SOURCE_HEIGHT 2160
#define PROBE_OUTPUT_WIDTH 1920
#define PROBE_OUTPUT_HEIGHT 1080
#define PROBE_BAR_COUNT 8

typedef struct {
    const char *name;
    enum AVPixelFormat format;
    int sws_colorspace;
    const char *output_path;
} probe_mode_t;

static const uint8_t g_probe_rgb[PROBE_BAR_COUNT][3] = {
    {255, 255, 255},
    {255, 255,   0},
    {  0, 255, 255},
    {  0, 255,   0},
    {255,   0, 255},
    {255,   0,   0},
    {  0,   0, 255},
    {  0,   0,   0}
};

static AVFrame *probe_alloc_frame(enum AVPixelFormat format, int width, int height) {
    AVFrame *frame = av_frame_alloc();

    if (frame == NULL) {
        return NULL;
    }
    frame->format = format;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 64) < 0 || av_frame_make_writable(frame) < 0) {
        av_frame_free(&frame);
        return NULL;
    }
    return frame;
}

static void probe_fill_bgr_bars(AVFrame *frame) {
    int x;
    int y;

    for (y = 0; y < frame->height; ++y) {
        uint8_t *row = frame->data[0] + (ptrdiff_t)y * frame->linesize[0];
        for (x = 0; x < frame->width; ++x) {
            int bar = x * PROBE_BAR_COUNT / frame->width;
            row[x * 3 + 0] = g_probe_rgb[bar][2];
            row[x * 3 + 1] = g_probe_rgb[bar][1];
            row[x * 3 + 2] = g_probe_rgb[bar][0];
        }
    }
    frame->color_range = AVCOL_RANGE_JPEG;
    frame->colorspace = AVCOL_SPC_RGB;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
}

static int probe_write_ppm(const char *path, const AVFrame *bgr) {
    FILE *fp;
    uint8_t *rgb_row;
    int x;
    int y;

    fp = fopen(path, "wb");
    if (fp == NULL) {
        return -1;
    }
    rgb_row = malloc((size_t)bgr->width * 3U);
    if (rgb_row == NULL) {
        fclose(fp);
        return -1;
    }

    fprintf(fp, "P6\n%d %d\n255\n", bgr->width, bgr->height);
    for (y = 0; y < bgr->height; ++y) {
        const uint8_t *src = bgr->data[0] + (ptrdiff_t)y * bgr->linesize[0];
        for (x = 0; x < bgr->width; ++x) {
            rgb_row[x * 3 + 0] = src[x * 3 + 2];
            rgb_row[x * 3 + 1] = src[x * 3 + 1];
            rgb_row[x * 3 + 2] = src[x * 3 + 0];
        }
        if (fwrite(rgb_row, 1, (size_t)bgr->width * 3U, fp) != (size_t)bgr->width * 3U) {
            free(rgb_row);
            fclose(fp);
            return -1;
        }
    }

    free(rgb_row);
    fclose(fp);
    return 0;
}

static AVFrame *probe_yuv_to_bgr(const AVFrame *yuv,
                                 enum AVPixelFormat interpreted_format,
                                 int sws_colorspace) {
    AVFrame src_view = *yuv;
    AVFrame *bgr;
    struct SwsContext *sws;
    const int *coefficients;
    int ret;

    src_view.format = interpreted_format;
    bgr = probe_alloc_frame(AV_PIX_FMT_BGR24, yuv->width, yuv->height);
    if (bgr == NULL) {
        return NULL;
    }

    sws = sws_getContext(yuv->width,
                         yuv->height,
                         interpreted_format,
                         yuv->width,
                         yuv->height,
                         AV_PIX_FMT_BGR24,
                         SWS_POINT,
                         NULL,
                         NULL,
                         NULL);
    if (sws == NULL) {
        av_frame_free(&bgr);
        return NULL;
    }

    coefficients = sws_getCoefficients(sws_colorspace);
    ret = sws_setColorspaceDetails(sws,
                                   coefficients,
                                   0,
                                   coefficients,
                                   1,
                                   0,
                                   1 << 16,
                                   1 << 16);
    if (ret >= 0) {
        ret = sws_scale(sws,
                        (const uint8_t * const *)src_view.data,
                        src_view.linesize,
                        0,
                        src_view.height,
                        bgr->data,
                        bgr->linesize);
    }
    sws_freeContext(sws);
    if (ret <= 0) {
        av_frame_free(&bgr);
        return NULL;
    }
    return bgr;
}

static void probe_measure_error(const AVFrame *reference,
                                const AVFrame *candidate,
                                const char *name) {
    uint64_t error[3] = {0, 0, 0};
    uint64_t samples = 0;
    int bar_width = reference->width / PROBE_BAR_COUNT;
    int bar;
    int x;
    int y;
    int channel;

    for (bar = 0; bar < PROBE_BAR_COUNT; ++bar) {
        int x_start = bar * bar_width + 8;
        int x_end = (bar + 1) * bar_width - 8;
        for (y = 8; y < reference->height - 8; y += 4) {
            const uint8_t *ref_row = reference->data[0] + (ptrdiff_t)y * reference->linesize[0];
            const uint8_t *cand_row = candidate->data[0] + (ptrdiff_t)y * candidate->linesize[0];
            for (x = x_start; x < x_end; x += 4) {
                for (channel = 0; channel < 3; ++channel) {
                    int delta = (int)ref_row[x * 3 + channel] -
                                (int)cand_row[x * 3 + channel];
                    error[channel] += (uint64_t)(delta < 0 ? -delta : delta);
                }
                samples++;
            }
        }
    }

    LOGI("probe %-12s MAE B=%.2f G=%.2f R=%.2f total=%.2f",
         name,
         (double)error[0] / samples,
         (double)error[1] / samples,
         (double)error[2] / samples,
         (double)(error[0] + error[1] + error[2]) / (samples * 3U));

    for (bar = 0; bar < PROBE_BAR_COUNT; ++bar) {
        int center_x = bar * bar_width + bar_width / 2;
        int center_y = reference->height / 2;
        const uint8_t *ref_pixel = reference->data[0] +
                                   (ptrdiff_t)center_y * reference->linesize[0] +
                                   center_x * 3;
        const uint8_t *cand_pixel = candidate->data[0] +
                                    (ptrdiff_t)center_y * candidate->linesize[0] +
                                    center_x * 3;
        LOGD("probe %-12s bar=%d ref_rgb=%d,%d,%d got_rgb=%d,%d,%d",
             name,
             bar,
             ref_pixel[2], ref_pixel[1], ref_pixel[0],
             cand_pixel[2], cand_pixel[1], cand_pixel[0]);
    }
}

int main(int argc, char **argv) {
    static const probe_mode_t modes[] = {
        {"NV12-BT709", AV_PIX_FMT_NV12, SWS_CS_ITU709, "/tmp/rga_probe_nv12_bt709.ppm"},
        {"NV21-BT709", AV_PIX_FMT_NV21, SWS_CS_ITU709, "/tmp/rga_probe_nv21_bt709.ppm"},
        {"NV12-BT601", AV_PIX_FMT_NV12, SWS_CS_ITU601, "/tmp/rga_probe_nv12_bt601.ppm"},
        {"NV21-BT601", AV_PIX_FMT_NV21, SWS_CS_ITU601, "/tmp/rga_probe_nv21_bt601.ppm"}
    };
    output_encode_convert_ctx_t convert;
    output_encode_convert_ctx_t sw_convert;
    AVFrame *source = NULL;
    AVFrame *reference = NULL;
    const AVFrame *rga_nv12;
    const AVFrame *sw_nv12;
    size_t i;
    int converted = 0;
    int exit_code = EXIT_FAILURE;

#ifdef HAVE_LIBRGA
    if (argc > 1) {
        char *end = NULL;
        unsigned long core = strtoul(argv[1], &end, 0);
        IM_STATUS config_status;

        if (end == argv[1] || *end != '\0') {
            LOGE("invalid RGA scheduler core mask: %s", argv[1]);
            return EXIT_FAILURE;
        }
        config_status = imconfig(IM_CONFIG_SCHEDULER_CORE, core);
        LOGI("probe RGA scheduler core mask=0x%lx status=%s",
             core,
             imStrError(config_status));
        if (config_status < 0) {
            return EXIT_FAILURE;
        }
    }
#else
    (void)argc;
    (void)argv;
#endif

    memset(&convert, 0, sizeof(convert));
    memset(&sw_convert, 0, sizeof(sw_convert));
    source = probe_alloc_frame(AV_PIX_FMT_BGR24,
                               PROBE_SOURCE_WIDTH,
                               PROBE_SOURCE_HEIGHT);
    reference = probe_alloc_frame(AV_PIX_FMT_BGR24,
                                  PROBE_OUTPUT_WIDTH,
                                  PROBE_OUTPUT_HEIGHT);
    if (source == NULL || reference == NULL) {
        LOGE("allocate source/reference frame failed");
        goto cleanup;
    }
    probe_fill_bgr_bars(source);
    probe_fill_bgr_bars(reference);
    if (probe_write_ppm("/tmp/rga_probe_reference.ppm", reference) != 0) {
        LOGE("write reference PPM failed");
        goto cleanup;
    }

    if (output_encode_convert_init(&convert,
                                   PROBE_OUTPUT_WIDTH,
                                   PROBE_OUTPUT_HEIGHT,
                                   AV_PIX_FMT_NV12) != APP_OK) {
        LOGE("init RGA NV12 conversion failed");
        goto cleanup;
    }
    rga_nv12 = output_encode_prepare_frame(&convert, source, &converted);
    if (rga_nv12 == NULL || !converted) {
        LOGE("RGA BGR to NV12 conversion failed");
        goto cleanup;
    }

    LOGI("probe RGA output format=%d range=%d colorspace=%d linesize=%d/%d",
         rga_nv12->format,
         rga_nv12->color_range,
         rga_nv12->colorspace,
         rga_nv12->linesize[0],
         rga_nv12->linesize[1]);
    LOGI("probe NV12 plane offset=%td visible_y_bytes=%td aligned_hstride=%td",
         rga_nv12->data[1] - rga_nv12->data[0],
         (ptrdiff_t)rga_nv12->linesize[0] * rga_nv12->height,
         (rga_nv12->data[1] - rga_nv12->data[0]) / rga_nv12->linesize[0]);
    if (convert.rga_resize_frame != NULL) {
        probe_measure_error(reference, convert.rga_resize_frame, "RGA-BGR-SCALE");
        probe_write_ppm("/tmp/rga_probe_scaled_bgr.ppm", convert.rga_resize_frame);
    }
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        AVFrame *candidate = probe_yuv_to_bgr(rga_nv12,
                                               modes[i].format,
                                               modes[i].sws_colorspace);
        if (candidate == NULL) {
            LOGE("convert probe mode %s failed", modes[i].name);
            goto cleanup;
        }
        probe_measure_error(reference, candidate, modes[i].name);
        if (probe_write_ppm(modes[i].output_path, candidate) != 0) {
            LOGE("write probe output failed: %s", modes[i].output_path);
            av_frame_free(&candidate);
            goto cleanup;
        }
        av_frame_free(&candidate);
    }

    if (output_encode_convert_init(&sw_convert,
                                   PROBE_OUTPUT_WIDTH,
                                   PROBE_OUTPUT_HEIGHT,
                                   AV_PIX_FMT_NV12) != APP_OK) {
        LOGE("init swscale NV12 conversion failed");
        goto cleanup;
    }
    sw_convert.rga_failed = 1;
    sw_convert.rkrga_filter_failed = 1;
    converted = 0;
    sw_nv12 = output_encode_prepare_frame(&sw_convert, reference, &converted);
    if (sw_nv12 == NULL || !converted) {
        LOGE("forced swscale BGR to NV12 conversion failed");
        goto cleanup;
    }
    LOGI("probe swscale output range=%d colorspace=%d uv_offset=%td",
         sw_nv12->color_range,
         sw_nv12->colorspace,
         sw_nv12->data[1] - sw_nv12->data[0]);
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        AVFrame *candidate;

        if (modes[i].format != AV_PIX_FMT_NV12) {
            continue;
        }
        candidate = probe_yuv_to_bgr(sw_nv12,
                                     modes[i].format,
                                     modes[i].sws_colorspace);
        if (candidate == NULL) {
            LOGE("convert swscale probe mode %s failed", modes[i].name);
            goto cleanup;
        }
        probe_measure_error(reference,
                            candidate,
                            modes[i].sws_colorspace == SWS_CS_ITU709
                                ? "SW-NV12-709"
                                : "SW-NV12-601");
        av_frame_free(&candidate);
    }

    LOGI("probe images written under /tmp/rga_probe_*.ppm");
    exit_code = EXIT_SUCCESS;

cleanup:
    output_encode_convert_deinit(&sw_convert);
    output_encode_convert_deinit(&convert);
    av_frame_free(&source);
    av_frame_free(&reference);
    return exit_code;
}
