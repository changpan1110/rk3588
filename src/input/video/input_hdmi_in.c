#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "input_hdmi_in.c"

#include "input/video/input_hdmi_in.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_rkmpp.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>

#include "common/debug.h"

#define INPUT_HDMI_NATIVE_BUFFER_COUNT 16
#define INPUT_HDMI_DRM_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
     ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
/* Little-endian DRM RGB888 stores bytes as B, G, R, matching AV_PIX_FMT_BGR24. */
#define INPUT_HDMI_DRM_FORMAT_RGB888 INPUT_HDMI_DRM_FOURCC('R', 'G', '2', '4')
#define INPUT_HDMI_DRM_FORMAT_NV12 INPUT_HDMI_DRM_FOURCC('N', 'V', '1', '2')
#define INPUT_HDMI_DRM_FORMAT_NV16 INPUT_HDMI_DRM_FOURCC('N', 'V', '1', '6')
#define INPUT_HDMI_DRM_FORMAT_NV24 INPUT_HDMI_DRM_FOURCC('N', 'V', '2', '4')
#define INPUT_HDMI_DRM_FORMAT_MOD_LINEAR 0ULL

static int input_hdmi_native_format_requested(const char *format) {
    return format == NULL || format[0] == '\0' ||
           strcmp(format, "bgr3") == 0 ||
           strcmp(format, "bgr24") == 0 ||
           strcmp(format, "nv12") == 0 ||
           strcmp(format, "nv16") == 0 ||
           strcmp(format, "nv24") == 0;
}

static uint32_t input_hdmi_native_requested_fourcc(const char *format) {
    if (format == NULL || format[0] == '\0') {
        return 0;
    }
    if (strcmp(format, "bgr3") == 0 || strcmp(format, "bgr24") == 0) {
        return V4L2_PIX_FMT_BGR24;
    }
    if (strcmp(format, "nv12") == 0) {
        return V4L2_PIX_FMT_NV12;
    }
    if (strcmp(format, "nv16") == 0) {
        return V4L2_PIX_FMT_NV16;
    }
    if (strcmp(format, "nv24") == 0) {
        return V4L2_PIX_FMT_NV24;
    }
    return 0;
}

static int input_hdmi_native_enabled(const video_input_config_t *cfg) {
    const char *value = getenv("RK_HDMI_NATIVE_V4L2");

    if (value == NULL || value[0] == '\0') {
        return cfg != NULL && cfg->use_native_v4l2 != 0;
    }
    if (strcmp(value, "1") == 0 ||
        strcmp(value, "on") == 0 ||
        strcmp(value, "true") == 0) {
        return 1;
    }
    if (strcmp(value, "0") == 0 ||
        strcmp(value, "off") == 0 ||
        strcmp(value, "false") == 0) {
        return 0;
    }

    LOGW("invalid RK_HDMI_NATIVE_V4L2=%s; using code configuration=%d",
         value,
         cfg != NULL ? cfg->use_native_v4l2 : 0);
    return cfg != NULL && cfg->use_native_v4l2 != 0;
}

typedef struct {
    struct input_hdmi_native_ctx *owner;
    size_t length[VIDEO_MAX_PLANES];
    int dmabuf_fd[VIDEO_MAX_PLANES];
    AVRKMPPDRMFrameDescriptor drm_desc;
    unsigned int plane_count;
    unsigned int index;
} input_hdmi_native_buffer_t;

struct input_hdmi_native_ctx {
    int fd;
    enum v4l2_buf_type buffer_type;
    unsigned int width;
    unsigned int height;
    unsigned int plane_count;
    unsigned int buffer_count;
    unsigned int bytes_per_line[VIDEO_MAX_PLANES];
    uint32_t pixel_format;
    uint32_t drm_format;
    enum AVPixelFormat sw_format;
    unsigned int drm_plane_count;
    enum AVColorRange color_range;
    enum AVColorSpace color_space;
    AVBufferRef *hw_device_ctx;
    AVBufferRef *hw_frames_ctx;
    input_hdmi_native_buffer_t buffers[INPUT_HDMI_NATIVE_BUFFER_COUNT];
    pthread_mutex_t lock;
    int refs;
    int closing;
    int streaming;
    uint64_t frame_count;
};

static int input_hdmi_in_ioctl(int fd, unsigned long request, void *arg) {
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

static void input_hdmi_native_destroy(input_hdmi_native_ctx_t *native) {
    unsigned int i;

    if (native == NULL) {
        return;
    }
    for (i = 0; i < native->buffer_count; ++i) {
        unsigned int plane;

        for (plane = 0; plane < native->buffers[i].plane_count; ++plane) {
            if (native->buffers[i].dmabuf_fd[plane] >= 0) {
                close(native->buffers[i].dmabuf_fd[plane]);
                native->buffers[i].dmabuf_fd[plane] = -1;
            }
        }
    }
    av_buffer_unref(&native->hw_frames_ctx);
    av_buffer_unref(&native->hw_device_ctx);
    pthread_mutex_destroy(&native->lock);
    free(native);
}

static app_status_t input_hdmi_native_open_hw_frames(input_hdmi_native_ctx_t *native) {
    AVHWFramesContext *frames_ctx;
    int ret;

    ret = av_hwdevice_ctx_create(&native->hw_device_ctx,
                                 AV_HWDEVICE_TYPE_RKMPP,
                                 NULL,
                                 NULL,
                                 0);
    if (ret < 0) {
        LOGE("hdmi_in create RKMPP hardware device failed: %d", ret);
        return APP_ERR_FFMPEG;
    }
    native->hw_frames_ctx = av_hwframe_ctx_alloc(native->hw_device_ctx);
    if (native->hw_frames_ctx == NULL) {
        return APP_ERR_NOMEM;
    }
    frames_ctx = (AVHWFramesContext *)native->hw_frames_ctx->data;
    frames_ctx->format = AV_PIX_FMT_DRM_PRIME;
    frames_ctx->sw_format = native->sw_format;
    frames_ctx->width = (int)native->width;
    frames_ctx->height = (int)native->height;
    frames_ctx->initial_pool_size = 0;
    ret = av_hwframe_ctx_init(native->hw_frames_ctx);
    if (ret < 0) {
        LOGE("hdmi_in initialize DRM PRIME/%s frames context failed: %d",
             av_get_pix_fmt_name(native->sw_format),
             ret);
        return APP_ERR_FFMPEG;
    }
    return APP_OK;
}

static void input_hdmi_native_init_drm_descriptor(input_hdmi_native_ctx_t *native,
                                                  input_hdmi_native_buffer_t *buffer) {
    AVDRMFrameDescriptor *desc = &buffer->drm_desc.drm_desc;
    unsigned int plane;
    uint32_t luma_pitch;
    uint32_t chroma_pitch;

    memset(&buffer->drm_desc, 0, sizeof(buffer->drm_desc));
    desc->nb_objects = (int)buffer->plane_count;
    for (plane = 0; plane < buffer->plane_count; ++plane) {
        desc->objects[plane].fd = buffer->dmabuf_fd[plane];
        desc->objects[plane].size = buffer->length[plane];
        desc->objects[plane].format_modifier = INPUT_HDMI_DRM_FORMAT_MOD_LINEAR;
    }
    desc->nb_layers = 1;
    desc->layers[0].format = native->drm_format;
    desc->layers[0].nb_planes = (int)native->drm_plane_count;
    desc->layers[0].planes[0].object_index = 0;
    desc->layers[0].planes[0].offset = 0;
    desc->layers[0].planes[0].pitch = native->bytes_per_line[0];
    if (native->drm_plane_count == 1) {
        return;
    }

    luma_pitch = native->bytes_per_line[0];
    chroma_pitch = native->sw_format == AV_PIX_FMT_NV24
                       ? luma_pitch * 2
                       : luma_pitch;
    desc->layers[0].planes[1].object_index = buffer->plane_count == 1 ? 0 : 1;
    desc->layers[0].planes[1].offset = buffer->plane_count == 1
                                          ? luma_pitch * native->height
                                          : 0;
    desc->layers[0].planes[1].pitch = buffer->plane_count == 1
                                         ? chroma_pitch
                                         : native->bytes_per_line[1];
}

static int input_hdmi_native_configure_pixel_format(input_hdmi_native_ctx_t *native) {
    switch (native->pixel_format) {
        case V4L2_PIX_FMT_BGR24:
            native->sw_format = AV_PIX_FMT_BGR24;
            native->drm_format = INPUT_HDMI_DRM_FORMAT_RGB888;
            native->drm_plane_count = 1;
            break;
        case V4L2_PIX_FMT_NV12:
#ifdef V4L2_PIX_FMT_NV12M
        case V4L2_PIX_FMT_NV12M:
#endif
            native->sw_format = AV_PIX_FMT_NV12;
            native->drm_format = INPUT_HDMI_DRM_FORMAT_NV12;
            native->drm_plane_count = 2;
            break;
        case V4L2_PIX_FMT_NV16:
#ifdef V4L2_PIX_FMT_NV16M
        case V4L2_PIX_FMT_NV16M:
#endif
            native->sw_format = AV_PIX_FMT_NV16;
            native->drm_format = INPUT_HDMI_DRM_FORMAT_NV16;
            native->drm_plane_count = 2;
            break;
        case V4L2_PIX_FMT_NV24:
#ifdef V4L2_PIX_FMT_NV24M
        case V4L2_PIX_FMT_NV24M:
#endif
            native->sw_format = AV_PIX_FMT_NV24;
            native->drm_format = INPUT_HDMI_DRM_FORMAT_NV24;
            native->drm_plane_count = 2;
            break;
        default:
            return 0;
    }

    if ((native->drm_plane_count == 1 && native->plane_count != 1) ||
        (native->drm_plane_count == 2 &&
         native->plane_count != 1 && native->plane_count != 2)) {
        return 0;
    }
    if (native->bytes_per_line[0] == 0) {
        native->bytes_per_line[0] = native->width *
                                    (native->sw_format == AV_PIX_FMT_BGR24 ? 3U : 1U);
    }
    if (native->plane_count == 2 && native->bytes_per_line[1] == 0) {
        native->bytes_per_line[1] = native->width *
                                    (native->sw_format == AV_PIX_FMT_NV24 ? 2U : 1U);
    }
    return 1;
}

static void input_hdmi_native_configure_color(input_hdmi_native_ctx_t *native,
                                              uint32_t colorspace,
                                              uint32_t quantization) {
    if (native->sw_format == AV_PIX_FMT_BGR24) {
        native->color_range = AVCOL_RANGE_JPEG;
        native->color_space = AVCOL_SPC_RGB;
        return;
    }

    native->color_range = quantization == V4L2_QUANTIZATION_FULL_RANGE
                              ? AVCOL_RANGE_JPEG
                              : AVCOL_RANGE_MPEG;
    switch (colorspace) {
        case V4L2_COLORSPACE_REC709:
            native->color_space = AVCOL_SPC_BT709;
            break;
        case V4L2_COLORSPACE_SMPTE170M:
            native->color_space = AVCOL_SPC_SMPTE170M;
            break;
#ifdef V4L2_COLORSPACE_BT2020
        case V4L2_COLORSPACE_BT2020:
            native->color_space = AVCOL_SPC_BT2020_NCL;
            break;
#endif
        default:
            native->color_space = native->width >= 1280
                                      ? AVCOL_SPC_BT709
                                      : AVCOL_SPC_SMPTE170M;
            break;
    }
}

static int input_hdmi_native_validate_buffer_layout(
    const input_hdmi_native_ctx_t *native,
    const input_hdmi_native_buffer_t *buffer) {
    uint64_t luma_size;
    uint64_t chroma_size;
    unsigned int chroma_height;
    unsigned int chroma_pitch;

    luma_size = (uint64_t)native->bytes_per_line[0] * native->height;
    if (native->drm_plane_count == 1) {
        return buffer->length[0] >= luma_size;
    }

    chroma_height = native->sw_format == AV_PIX_FMT_NV12
                        ? (native->height + 1) / 2
                        : native->height;
    chroma_pitch = native->plane_count == 1
                       ? native->bytes_per_line[0] *
                             (native->sw_format == AV_PIX_FMT_NV24 ? 2U : 1U)
                       : native->bytes_per_line[1];
    chroma_size = (uint64_t)chroma_pitch * chroma_height;
    if (native->plane_count == 1) {
        return buffer->length[0] >= luma_size + chroma_size;
    }
    return buffer->length[0] >= luma_size &&
           buffer->length[1] >= chroma_size;
}

static int input_hdmi_native_queue_locked(input_hdmi_native_ctx_t *native,
                                          unsigned int index) {
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    unsigned int plane;

    memset(&buffer, 0, sizeof(buffer));
    memset(planes, 0, sizeof(planes));
    buffer.type = native->buffer_type;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    if (native->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buffer.m.planes = planes;
        buffer.length = native->plane_count;
        for (plane = 0; plane < native->plane_count; ++plane) {
            planes[plane].length = (uint32_t)native->buffers[index].length[plane];
        }
    }
    return input_hdmi_in_ioctl(native->fd, VIDIOC_QBUF, &buffer);
}

static void input_hdmi_native_buffer_release(void *opaque, uint8_t *data) {
    input_hdmi_native_buffer_t *buffer = (input_hdmi_native_buffer_t *)opaque;
    input_hdmi_native_ctx_t *native;
    int destroy = 0;

    (void)data;
    if (buffer == NULL || buffer->owner == NULL) {
        return;
    }
    native = buffer->owner;

    pthread_mutex_lock(&native->lock);
    if (!native->closing && native->fd >= 0) {
        if (input_hdmi_native_queue_locked(native, buffer->index) < 0) {
            LOGE("hdmi_in native QBUF index=%u failed: %s",
                 buffer->index,
                 strerror(errno));
        }
    }
    native->refs--;
    if (native->refs == 0) {
        destroy = 1;
    }
    pthread_mutex_unlock(&native->lock);

    if (destroy) {
        input_hdmi_native_destroy(native);
    }
}

static app_status_t input_hdmi_native_open(input_hdmi_in_ctx_t *ctx) {
    input_hdmi_native_ctx_t *native;
    struct v4l2_capability capability;
    struct v4l2_requestbuffers request;
    struct v4l2_format format;
    uint32_t device_caps;
    uint32_t colorspace;
    uint32_t quantization;
    uint32_t requested_fourcc;
    unsigned int requested_width;
    unsigned int requested_height;
    unsigned int i;

    native = (input_hdmi_native_ctx_t *)calloc(1, sizeof(*native));
    if (native == NULL) {
        return APP_ERR_NOMEM;
    }
    native->fd = -1;
    native->refs = 1;
    for (i = 0; i < INPUT_HDMI_NATIVE_BUFFER_COUNT; ++i) {
        unsigned int plane;

        for (plane = 0; plane < VIDEO_MAX_PLANES; ++plane) {
            native->buffers[i].dmabuf_fd[plane] = -1;
        }
    }
    pthread_mutex_init(&native->lock, NULL);

    native->fd = open(ctx->cfg.device, O_RDWR | O_NONBLOCK);
    if (native->fd < 0) {
        LOGE("hdmi_in native open %s failed: %s", ctx->cfg.device, strerror(errno));
        input_hdmi_native_destroy(native);
        return APP_ERR_IO;
    }

    memset(&capability, 0, sizeof(capability));
    if (input_hdmi_in_ioctl(native->fd, VIDIOC_QUERYCAP, &capability) < 0) {
        LOGE("hdmi_in native VIDIOC_QUERYCAP failed: %s", strerror(errno));
        goto fail_io;
    }
    device_caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
                      ? capability.device_caps
                      : capability.capabilities;
    if (device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        native->buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    } else if (device_caps & V4L2_CAP_VIDEO_CAPTURE) {
        native->buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    } else {
        LOGW("hdmi_in native device has no capture capability");
        goto fail_unsupported;
    }
    if (!(device_caps & V4L2_CAP_STREAMING)) {
        LOGW("hdmi_in native device has no streaming capability");
        goto fail_unsupported;
    }

    memset(&format, 0, sizeof(format));
    format.type = native->buffer_type;
    if (input_hdmi_in_ioctl(native->fd, VIDIOC_G_FMT, &format) < 0) {
        LOGE("hdmi_in native VIDIOC_G_FMT failed: %s", strerror(errno));
        goto fail_io;
    }

    requested_fourcc = input_hdmi_native_requested_fourcc(ctx->cfg.input_format);
    requested_width = ctx->cfg.width > 0 ? (unsigned int)ctx->cfg.width : 0;
    requested_height = ctx->cfg.height > 0 ? (unsigned int)ctx->cfg.height : 0;
    if (requested_fourcc != 0 || requested_width != 0 || requested_height != 0) {
        if (native->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            if (requested_width != 0) {
                format.fmt.pix_mp.width = requested_width;
            }
            if (requested_height != 0) {
                format.fmt.pix_mp.height = requested_height;
            }
            if (requested_fourcc != 0) {
                format.fmt.pix_mp.pixelformat = requested_fourcc;
            }
            format.fmt.pix_mp.field = V4L2_FIELD_ANY;
        } else {
            if (requested_width != 0) {
                format.fmt.pix.width = requested_width;
            }
            if (requested_height != 0) {
                format.fmt.pix.height = requested_height;
            }
            if (requested_fourcc != 0) {
                format.fmt.pix.pixelformat = requested_fourcc;
            }
            format.fmt.pix.field = V4L2_FIELD_ANY;
        }
        if (input_hdmi_in_ioctl(native->fd, VIDIOC_S_FMT, &format) < 0) {
            LOGW("hdmi_in native requested format %s %ux%u failed: %s; keeping driver format",
                 ctx->cfg.input_format[0] != '\0' ? ctx->cfg.input_format : "current",
                 requested_width,
                 requested_height,
                 strerror(errno));
            memset(&format, 0, sizeof(format));
            format.type = native->buffer_type;
            if (input_hdmi_in_ioctl(native->fd, VIDIOC_G_FMT, &format) < 0) {
                LOGE("hdmi_in native VIDIOC_G_FMT after S_FMT failure failed: %s",
                     strerror(errno));
                goto fail_io;
            }
        }
    }
    if (native->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        native->width = format.fmt.pix_mp.width;
        native->height = format.fmt.pix_mp.height;
        native->pixel_format = format.fmt.pix_mp.pixelformat;
        native->plane_count = format.fmt.pix_mp.num_planes;
        colorspace = format.fmt.pix_mp.colorspace;
        quantization = format.fmt.pix_mp.quantization;
        if (native->plane_count == 0 || native->plane_count > VIDEO_MAX_PLANES) {
            LOGE("hdmi_in native invalid plane count: %u", native->plane_count);
            goto fail_unsupported;
        }
        for (i = 0; i < native->plane_count; ++i) {
            native->bytes_per_line[i] = format.fmt.pix_mp.plane_fmt[i].bytesperline;
        }
    } else {
        native->width = format.fmt.pix.width;
        native->height = format.fmt.pix.height;
        native->pixel_format = format.fmt.pix.pixelformat;
        native->plane_count = 1;
        native->bytes_per_line[0] = format.fmt.pix.bytesperline;
        colorspace = format.fmt.pix.colorspace;
        quantization = format.fmt.pix.quantization;
    }
    if (!input_hdmi_native_configure_pixel_format(native)) {
        LOGW("hdmi_in native unsupported format fourcc=%c%c%c%c memory_planes=%u",
             (char)(native->pixel_format & 0xff),
             (char)((native->pixel_format >> 8) & 0xff),
             (char)((native->pixel_format >> 16) & 0xff),
             (char)((native->pixel_format >> 24) & 0xff),
             native->plane_count);
        goto fail_unsupported;
    }
    input_hdmi_native_configure_color(native, colorspace, quantization);
    if (input_hdmi_native_open_hw_frames(native) != APP_OK) {
        goto fail_unsupported;
    }

    memset(&request, 0, sizeof(request));
    request.count = INPUT_HDMI_NATIVE_BUFFER_COUNT;
    request.type = native->buffer_type;
    request.memory = V4L2_MEMORY_MMAP;
    if (input_hdmi_in_ioctl(native->fd, VIDIOC_REQBUFS, &request) < 0 || request.count == 0) {
        LOGE("hdmi_in native VIDIOC_REQBUFS failed: %s", strerror(errno));
        goto fail_io;
    }
    native->buffer_count = request.count > INPUT_HDMI_NATIVE_BUFFER_COUNT
                               ? INPUT_HDMI_NATIVE_BUFFER_COUNT
                               : request.count;

    for (i = 0; i < native->buffer_count; ++i) {
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        unsigned int plane;

        memset(&buffer, 0, sizeof(buffer));
        memset(planes, 0, sizeof(planes));
        buffer.type = native->buffer_type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (native->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buffer.m.planes = planes;
            buffer.length = native->plane_count;
        }
        if (input_hdmi_in_ioctl(native->fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            LOGE("hdmi_in native QUERYBUF index=%u failed: %s", i, strerror(errno));
            goto fail_io;
        }
        native->buffers[i].owner = native;
        native->buffers[i].index = i;
        native->buffers[i].plane_count = native->plane_count;
        for (plane = 0; plane < native->plane_count; ++plane) {
            native->buffers[i].length[plane] =
                native->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                    ? planes[plane].length
                    : buffer.length;
        }
        if (!input_hdmi_native_validate_buffer_layout(native, &native->buffers[i])) {
            LOGE("hdmi_in native buffer layout too small: index=%u size=%zu/%zu",
                 i,
                 native->buffers[i].length[0],
                 native->plane_count > 1 ? native->buffers[i].length[1] : 0);
            goto fail_unsupported;
        }
        for (plane = 0; plane < native->plane_count; ++plane) {
            struct v4l2_exportbuffer export_buffer;

            memset(&export_buffer, 0, sizeof(export_buffer));
            export_buffer.type = native->buffer_type;
            export_buffer.index = i;
            export_buffer.plane = plane;
            export_buffer.flags = O_CLOEXEC;
            if (input_hdmi_in_ioctl(native->fd, VIDIOC_EXPBUF, &export_buffer) < 0) {
                LOGW("hdmi_in native EXPBUF index=%u plane=%u failed: %s",
                     i,
                     plane,
                     strerror(errno));
                goto fail_unsupported;
            }
            native->buffers[i].dmabuf_fd[plane] = export_buffer.fd;
        }
        input_hdmi_native_init_drm_descriptor(native, &native->buffers[i]);
        if (input_hdmi_native_queue_locked(native, i) < 0) {
            LOGE("hdmi_in native QBUF index=%u failed: %s", i, strerror(errno));
            goto fail_io;
        }
    }

    if (input_hdmi_in_ioctl(native->fd, VIDIOC_STREAMON, &native->buffer_type) < 0) {
        LOGE("hdmi_in native STREAMON failed: %s", strerror(errno));
        goto fail_io;
    }
    native->streaming = 1;
    ctx->native = native;
    ctx->use_native = 1;
    ctx->cfg.width = (int)native->width;
    ctx->cfg.height = (int)native->height;
    LOGI("hdmi_in native V4L2 DMA-BUF opened: %ux%u fourcc=%c%c%c%c sw_fmt=%s "
         "memory_planes=%u drm_planes=%u stride=%u/%u range=%s buffers=%u",
         native->width,
         native->height,
         (char)(native->pixel_format & 0xff),
         (char)((native->pixel_format >> 8) & 0xff),
         (char)((native->pixel_format >> 16) & 0xff),
         (char)((native->pixel_format >> 24) & 0xff),
         av_get_pix_fmt_name(native->sw_format),
         native->plane_count,
         native->drm_plane_count,
         native->bytes_per_line[0],
         native->plane_count > 1 ? native->bytes_per_line[1] : 0,
         native->color_range == AVCOL_RANGE_JPEG ? "full" : "limited",
         native->buffer_count);
    return APP_OK;

fail_unsupported:
    if (native->fd >= 0) {
        close(native->fd);
        native->fd = -1;
    }
    input_hdmi_native_destroy(native);
    return APP_ERR_UNSUPPORTED;

fail_io:
    if (native->streaming) {
        input_hdmi_in_ioctl(native->fd, VIDIOC_STREAMOFF, &native->buffer_type);
    }
    if (native->fd >= 0) {
        close(native->fd);
        native->fd = -1;
    }
    input_hdmi_native_destroy(native);
    return APP_ERR_IO;
}

static app_status_t input_hdmi_native_read(input_hdmi_in_ctx_t *ctx, AVFrame *frame) {
    input_hdmi_native_ctx_t *native = ctx->native;
    struct pollfd poll_fd;
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    input_hdmi_native_buffer_t *native_buffer;
    AVBufferRef *buffer_ref;
    int poll_ret;
    int saved_errno;

    poll_fd.fd = native->fd;
    poll_fd.events = POLLIN | POLLPRI;
    poll_fd.revents = 0;
    do {
        poll_ret = poll(&poll_fd, 1, 1000);
    } while (poll_ret < 0 && errno == EINTR);
    if (poll_ret == 0) {
        return APP_ERR_EOF;
    }
    if (poll_ret < 0 || (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        LOGE("hdmi_in native poll failed: %s revents=0x%x",
             poll_ret < 0 ? strerror(errno) : "device error",
             poll_fd.revents);
        return APP_ERR_IO;
    }

    memset(&buffer, 0, sizeof(buffer));
    memset(planes, 0, sizeof(planes));
    buffer.type = native->buffer_type;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (native->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buffer.m.planes = planes;
        buffer.length = native->plane_count;
    }

    pthread_mutex_lock(&native->lock);
    if (native->closing || native->fd < 0) {
        pthread_mutex_unlock(&native->lock);
        return APP_ERR_EOF;
    }
    if (input_hdmi_in_ioctl(native->fd, VIDIOC_DQBUF, &buffer) < 0) {
        saved_errno = errno;
        pthread_mutex_unlock(&native->lock);
        if (saved_errno == EAGAIN) {
            return APP_ERR_EOF;
        }
        LOGE("hdmi_in native DQBUF failed: %s", strerror(saved_errno));
        return APP_ERR_IO;
    }
    if (buffer.index >= native->buffer_count) {
        pthread_mutex_unlock(&native->lock);
        LOGE("hdmi_in native invalid buffer index: %u", buffer.index);
        return APP_ERR_IO;
    }
    native->refs++;
    pthread_mutex_unlock(&native->lock);

    native_buffer = &native->buffers[buffer.index];
    buffer_ref = av_buffer_create((uint8_t *)&native_buffer->drm_desc,
                                  sizeof(native_buffer->drm_desc),
                                  input_hdmi_native_buffer_release,
                                  native_buffer,
                                  AV_BUFFER_FLAG_READONLY);
    if (buffer_ref == NULL) {
        input_hdmi_native_buffer_release(native_buffer,
                                         (uint8_t *)&native_buffer->drm_desc);
        return APP_ERR_NOMEM;
    }

    av_frame_unref(frame);
    frame->buf[0] = buffer_ref;
    frame->data[0] = (uint8_t *)&native_buffer->drm_desc;
    frame->format = AV_PIX_FMT_DRM_PRIME;
    frame->width = (int)native->width;
    frame->height = (int)native->height;
    frame->hw_frames_ctx = av_buffer_ref(native->hw_frames_ctx);
    if (frame->hw_frames_ctx == NULL) {
        av_frame_unref(frame);
        return APP_ERR_NOMEM;
    }
    frame->color_range = native->color_range;
    frame->colorspace = native->color_space;
    frame->color_primaries = native->color_space == AVCOL_SPC_BT2020_NCL
                                 ? AVCOL_PRI_BT2020
                                 : AVCOL_PRI_BT709;
    frame->color_trc = native->color_space == AVCOL_SPC_BT2020_NCL
                           ? AVCOL_TRC_BT2020_10
                           : AVCOL_TRC_BT709;
    frame->sample_aspect_ratio = (AVRational){1, 1};
    native->frame_count++;
    if (native->frame_count == 1) {
        LOGD("hdmi_in native DMA-BUF frame count=%llu buffer=%u fd=%d "
             "fourcc=%c%c%c%c sw_fmt=%s drm_fmt=%c%c%c%c",
             (unsigned long long)native->frame_count,
             buffer.index,
             native_buffer->dmabuf_fd[0],
             (char)(native->pixel_format & 0xff),
             (char)((native->pixel_format >> 8) & 0xff),
             (char)((native->pixel_format >> 16) & 0xff),
             (char)((native->pixel_format >> 24) & 0xff),
             av_get_pix_fmt_name(native->sw_format) != NULL
                 ? av_get_pix_fmt_name(native->sw_format)
                 : "unknown",
             (char)(native->drm_format & 0xff),
             (char)((native->drm_format >> 8) & 0xff),
             (char)((native->drm_format >> 16) & 0xff),
             (char)((native->drm_format >> 24) & 0xff));
    }
    return APP_OK;
}

static void input_hdmi_native_close(input_hdmi_in_ctx_t *ctx) {
    input_hdmi_native_ctx_t *native = ctx->native;
    int destroy = 0;

    if (native == NULL) {
        return;
    }
    pthread_mutex_lock(&native->lock);
    native->closing = 1;
    pthread_mutex_unlock(&native->lock);

    if (native->streaming && native->fd >= 0) {
        input_hdmi_in_ioctl(native->fd, VIDIOC_STREAMOFF, &native->buffer_type);
        native->streaming = 0;
    }
    if (native->fd >= 0) {
        close(native->fd);
        native->fd = -1;
    }

    pthread_mutex_lock(&native->lock);
    native->refs--;
    if (native->refs == 0) {
        destroy = 1;
    }
    pthread_mutex_unlock(&native->lock);
    if (destroy) {
        input_hdmi_native_destroy(native);
    } else {
        LOGW("hdmi_in native close deferred: frames are still referenced");
    }
    ctx->native = NULL;
    ctx->use_native = 0;
}

static int input_hdmi_in_is_rawvideo(enum AVCodecID codec_id) {
    return codec_id == AV_CODEC_ID_RAWVIDEO;
}

static app_status_t input_hdmi_in_check_link(const char *device) {
    struct v4l2_dv_timings timings;
    int fd;
    int saved_errno;

    fd = open(device, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        LOGE("hdmi_in open %s for timing query failed: %s", device, strerror(errno));
        return APP_ERR_IO;
    }

    memset(&timings, 0, sizeof(timings));
    if (ioctl(fd, VIDIOC_QUERY_DV_TIMINGS, &timings) < 0) {
        saved_errno = errno;
        close(fd);
        LOGW("hdmi_in has no valid input timing on %s: %s",
             device,
             strerror(saved_errno));
        return APP_ERR_IO;
    }
    close(fd);

    if (timings.bt.width == 0 || timings.bt.height == 0) {
        LOGW("hdmi_in has no active video on %s", device);
        return APP_ERR_IO;
    }

    LOGI("hdmi_in signal detected: %ux%u pixelclock=%llu",
         timings.bt.width,
         timings.bt.height,
         (unsigned long long)timings.bt.pixelclock);
    return APP_OK;
}

static int input_hdmi_in_is_raw_v4l2_format(const char *fmt) {
    if (fmt == NULL || fmt[0] == '\0') {
        return 0;
    }

    return strcmp(fmt, "uyvy422") == 0 ||
           strcmp(fmt, "yuyv422") == 0 ||
           strcmp(fmt, "nv12") == 0 ||
           strcmp(fmt, "nv21") == 0 ||
           strcmp(fmt, "nv16") == 0 ||
           strcmp(fmt, "nv61") == 0 ||
           strcmp(fmt, "nv24") == 0 ||
           strcmp(fmt, "bgr3") == 0 ||
           strcmp(fmt, "rgb3") == 0 ||
           strcmp(fmt, "nm12") == 0 ||
           strcmp(fmt, "nm21") == 0;
}

static const AVCodec *input_hdmi_in_select_decoder(enum AVCodecID codec_id) {
    const AVCodec *decoder = NULL;

    if (input_hdmi_in_is_rawvideo(codec_id)) {
        decoder = avcodec_find_decoder(codec_id);
        if (decoder != NULL) {
            LOGD("hdmi_in rawvideo path: %s", decoder->name);
        }
        return decoder;
    }

    switch (codec_id) {
        case AV_CODEC_ID_H264:
            decoder = avcodec_find_decoder_by_name("h264_rkmpp");
            if (decoder != NULL) {
                LOGD("hdmi_in decoder select: h264_rkmpp");
                return decoder;
            }
            break;
        case AV_CODEC_ID_HEVC:
            decoder = avcodec_find_decoder_by_name("hevc_rkmpp");
            if (decoder != NULL) {
                LOGD("hdmi_in decoder select: hevc_rkmpp");
                return decoder;
            }
            break;
        case AV_CODEC_ID_MJPEG:
            decoder = avcodec_find_decoder_by_name("mjpeg_rkmpp");
            if (decoder != NULL) {
                LOGD("hdmi_in decoder select: mjpeg_rkmpp");
                return decoder;
            }
            break;
        default:
            break;
    }

    decoder = avcodec_find_decoder(codec_id);
    if (decoder != NULL) {
        LOGD("hdmi_in decoder fallback: %s", decoder->name);
    }
    return decoder;
}

static app_status_t input_hdmi_in_open_decoder(input_hdmi_in_ctx_t *ctx) {
    AVStream *stream;
    const AVCodec *decoder;
    int ret;

    stream = ctx->decoder.fmt_ctx->streams[ctx->decoder.video_stream_index];
    if (input_hdmi_in_is_rawvideo(stream->codecpar->codec_id)) {
        LOGD("hdmi_in stream codec is rawvideo, skip rkmpp compressed decoder selection");
    }
    decoder = input_hdmi_in_select_decoder(stream->codecpar->codec_id);
    if (decoder == NULL) {
        LOGE("hdmi_in decoder not found for codec id=%d", stream->codecpar->codec_id);
        return APP_ERR_UNSUPPORTED;
    }

    ctx->decoder.dec_ctx = avcodec_alloc_context3(decoder);
    if (ctx->decoder.dec_ctx == NULL) {
        return APP_ERR_NOMEM;
    }

    ret = avcodec_parameters_to_context(ctx->decoder.dec_ctx, stream->codecpar);
    if (ret < 0) {
        LOGE("hdmi_in avcodec_parameters_to_context failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    ret = avcodec_open2(ctx->decoder.dec_ctx, decoder, NULL);
    if (ret < 0) {
        LOGE("hdmi_in avcodec_open2 failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    ctx->decoder.codec_id = stream->codecpar->codec_id;
    return APP_OK;
}

static app_status_t input_hdmi_in_open_device(input_hdmi_in_ctx_t *ctx) {
    char video_size[32];
    char fps[16];
    const AVInputFormat *input_fmt;
    AVDictionary *options = NULL;
    int ret;

    input_fmt = av_find_input_format("video4linux2");
    if (input_fmt == NULL) {
        LOGE("hdmi_in video4linux2 input format not found");
        return APP_ERR_UNSUPPORTED;
    }

    if (ctx->cfg.width > 0 && ctx->cfg.height > 0) {
        snprintf(video_size, sizeof(video_size), "%dx%d", ctx->cfg.width, ctx->cfg.height);
        av_dict_set(&options, "video_size", video_size, 0);
    }

    if (ctx->cfg.fps > 0 && !input_hdmi_in_is_raw_v4l2_format(ctx->cfg.input_format)) {
        snprintf(fps, sizeof(fps), "%d", ctx->cfg.fps);
        av_dict_set(&options, "framerate", fps, 0);
    }

    if (ctx->cfg.input_format[0] != '\0') {
        av_dict_set(&options, "input_format", ctx->cfg.input_format, 0);
    }

    if (ctx->cfg.input_format[0] != '\0' ||
        (ctx->cfg.width > 0 && ctx->cfg.height > 0)) {
        LOGD("hdmi_in request format: size=%s fps=%s fmt=%s",
             ctx->cfg.width > 0 && ctx->cfg.height > 0 ? video_size : "driver-default",
             (ctx->cfg.fps > 0 &&
              !input_hdmi_in_is_raw_v4l2_format(ctx->cfg.input_format))
                 ? fps
                 : "driver-default",
             ctx->cfg.input_format[0] != '\0'
                 ? ctx->cfg.input_format
                 : "driver-default");
    } else {
        LOGD("hdmi_in use driver current format");
    }

    ret = avformat_open_input(&ctx->decoder.fmt_ctx, ctx->cfg.device, (AVInputFormat *)input_fmt, &options);
    av_dict_free(&options);
    if (ret < 0) {
        LOGE("hdmi_in avformat_open_input failed for %s: %d", ctx->cfg.device, ret);
        return APP_ERR_FFMPEG;
    }

    /*
     * v4l2 只有一个视频流,avformat_open_input 时已填好 codecpar。
     * 不调 avformat_find_stream_info:设备无信号时它会永久阻塞在帧读取上。
     */
    ret = av_find_best_stream(ctx->decoder.fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret < 0) {
        LOGE("hdmi_in av_find_best_stream failed: %d", ret);
        return APP_ERR_FFMPEG;
    }
    ctx->decoder.video_stream_index = ret;
    {
        AVStream *stream = ctx->decoder.fmt_ctx->streams[ctx->decoder.video_stream_index];
        ctx->cfg.width = stream->codecpar->width;
        ctx->cfg.height = stream->codecpar->height;
    }
    ffmpeg_input_log_stream(&ctx->cfg, &ctx->decoder);

    ctx->decoder.packet = av_packet_alloc();
    if (ctx->decoder.packet == NULL) {
        return APP_ERR_NOMEM;
    }

    return input_hdmi_in_open_decoder(ctx);
}

static app_status_t input_hdmi_in_read_decoder(input_hdmi_in_ctx_t *ctx, AVFrame *frame) {
    int ret;

    while ((ret = av_read_frame(ctx->decoder.fmt_ctx, ctx->decoder.packet)) >= 0) {
        if (ctx->decoder.packet->stream_index != ctx->decoder.video_stream_index) {
            av_packet_unref(ctx->decoder.packet);
            continue;
        }

        ret = avcodec_send_packet(ctx->decoder.dec_ctx, ctx->decoder.packet);
        av_packet_unref(ctx->decoder.packet);
        if (ret < 0) {
            LOGE("hdmi_in avcodec_send_packet failed: %d", ret);
            return APP_ERR_FFMPEG;
        }

        ret = avcodec_receive_frame(ctx->decoder.dec_ctx, frame);
        if (ret == 0) {
            return APP_OK;
        }
        if (ret == AVERROR(EAGAIN)) {
            continue;
        }
        if (ret == AVERROR_EOF) {
            return APP_ERR_EOF;
        }

        LOGE("hdmi_in avcodec_receive_frame failed: %d", ret);
        return APP_ERR_FFMPEG;
    }

    return APP_ERR_EOF;
}

app_status_t input_hdmi_in_open(input_hdmi_in_ctx_t *ctx, const video_input_config_t *cfg) {
    app_status_t status;

    if (ctx == NULL || cfg == NULL) {
        return APP_ERR_PARAM;
    }

    *ctx = (input_hdmi_in_ctx_t){0};
    ctx->cfg = *cfg;
    LOGD("open hdmi_in input: %s %dx%d@%d fmt=%s",
        ctx->cfg.device, ctx->cfg.width, ctx->cfg.height, ctx->cfg.fps, ctx->cfg.input_format);
    status = input_hdmi_in_check_link(ctx->cfg.device);
    if (status != APP_OK) {
        return status;
    }

    if (!input_hdmi_native_enabled(&ctx->cfg)) {
        LOGD("hdmi_in native V4L2 path disabled by configuration/environment");
    } else if (input_hdmi_native_format_requested(ctx->cfg.input_format)) {
        status = input_hdmi_native_open(ctx);
        if (status == APP_OK) {
            ctx->is_opened = 1;
            return APP_OK;
        }
        LOGW("hdmi_in native V4L2 path unavailable (%s), falling back to FFmpeg",
             app_status_str(status));
    } else {
        LOGD("hdmi_in requested format %s uses FFmpeg input path",
             ctx->cfg.input_format);
    }

    status = input_hdmi_in_open_device(ctx);
    if (status != APP_OK) {
        input_hdmi_in_close(ctx);
        return status;
    }
    ctx->is_opened = 1;
    return APP_OK;
}

app_status_t input_hdmi_in_read(input_hdmi_in_ctx_t *ctx, video_frame_t *frame) {
    app_status_t status;

    if (ctx == NULL || frame == NULL || frame->av_frame == NULL) {
        return APP_ERR_PARAM;
    }
    if (!ctx->is_opened) {
        return APP_ERR_IO;
    }

    if (ctx->use_native) {
        status = input_hdmi_native_read(ctx, frame->av_frame);
    } else {
        status = input_hdmi_in_read_decoder(ctx, frame->av_frame);
    }
    if (status != APP_OK) {
        return status;
    }

    frame->pts_us = app_get_time_us();
    frame->source_type = VIDEO_SOURCE_HDMI_IN;
    strncpy(frame->source_name, "hdmi_in", sizeof(frame->source_name) - 1);
    frame->source_name[sizeof(frame->source_name) - 1] = '\0';
    return APP_OK;
}

void input_hdmi_in_close(input_hdmi_in_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    if (ctx->native != NULL) {
        input_hdmi_native_close(ctx);
    }
    if (ctx->decoder.packet != NULL) {
        av_packet_free(&ctx->decoder.packet);
    }
    if (ctx->decoder.dec_ctx != NULL) {
        avcodec_free_context(&ctx->decoder.dec_ctx);
    }
    if (ctx->decoder.fmt_ctx != NULL) {
        avformat_close_input(&ctx->decoder.fmt_ctx);
    }
    memset(&ctx->decoder, 0, sizeof(ctx->decoder));
    ctx->is_opened = 0;
}
