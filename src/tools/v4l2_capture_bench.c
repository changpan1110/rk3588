#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define BENCH_MAX_BUFFERS 8

typedef struct {
    void *start[VIDEO_MAX_PLANES];
    size_t length[VIDEO_MAX_PLANES];
    unsigned int plane_count;
} bench_buffer_t;

static volatile unsigned char g_copy_sink;

static int64_t bench_now_ns(clockid_t clock_id) {
    struct timespec ts;

    if (clock_gettime(clock_id, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int bench_ioctl(int fd, unsigned long request, void *arg) {
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

static void bench_fourcc(char out[5], uint32_t value) {
    out[0] = (char)(value & 0xff);
    out[1] = (char)((value >> 8) & 0xff);
    out[2] = (char)((value >> 16) & 0xff);
    out[3] = (char)((value >> 24) & 0xff);
    out[4] = '\0';
}

static void bench_usage(const char *program) {
    fprintf(stderr,
            "usage: %s <device> <seconds> <nocopy|copy|export>\n"
            "example: %s /dev/video40 20 export\n",
            program,
            program);
}

int main(int argc, char **argv) {
    bench_buffer_t buffers[BENCH_MAX_BUFFERS] = {0};
    struct v4l2_requestbuffers request;
    struct v4l2_capability capability;
    enum v4l2_buf_type buffer_type;
    unsigned char *copy_buffer = NULL;
    size_t copy_capacity = 0;
    uint64_t copied_bytes = 0;
    uint64_t frame_count = 0;
    int64_t copy_ns = 0;
    int64_t cpu_started_ns = 0;
    int64_t cpu_ended_ns = 0;
    int64_t wall_started_ns = 0;
    int64_t wall_ended_ns = 0;
    double duration_sec;
    uint32_t pixel_format;
    uint32_t device_caps;
    unsigned int width;
    unsigned int height;
    unsigned int format_plane_count;
    unsigned int mapped_count = 0;
    unsigned int exported_count = 0;
    int stream_started = 0;
    int copy_enabled;
    int export_enabled;
    int fd = -1;
    int result = 1;
    unsigned int i;
    char fourcc[5];

    if (argc != 4) {
        bench_usage(argv[0]);
        return 2;
    }
    duration_sec = strtod(argv[2], NULL);
    if (duration_sec <= 0.0) {
        bench_usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[3], "copy") == 0) {
        copy_enabled = 1;
        export_enabled = 0;
    } else if (strcmp(argv[3], "nocopy") == 0) {
        copy_enabled = 0;
        export_enabled = 0;
    } else if (strcmp(argv[3], "export") == 0) {
        copy_enabled = 0;
        export_enabled = 1;
    } else {
        bench_usage(argv[0]);
        return 2;
    }

    fd = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", argv[1], strerror(errno));
        goto cleanup;
    }

    memset(&capability, 0, sizeof(capability));
    if (bench_ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0) {
        fprintf(stderr, "VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
        goto cleanup;
    }
    device_caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
                      ? capability.device_caps
                      : capability.capabilities;
    if (device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    } else if (device_caps & V4L2_CAP_VIDEO_CAPTURE) {
        buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    } else {
        fprintf(stderr, "device has no capture capability\n");
        goto cleanup;
    }
    if ((device_caps & V4L2_CAP_STREAMING) == 0) {
        fprintf(stderr, "device has no streaming capability\n");
        goto cleanup;
    }

    if (buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        struct v4l2_format format;

        memset(&format, 0, sizeof(format));
        format.type = buffer_type;
        if (bench_ioctl(fd, VIDIOC_G_FMT, &format) < 0) {
            fprintf(stderr, "VIDIOC_G_FMT failed: %s\n", strerror(errno));
            goto cleanup;
        }
        width = format.fmt.pix_mp.width;
        height = format.fmt.pix_mp.height;
        pixel_format = format.fmt.pix_mp.pixelformat;
        format_plane_count = format.fmt.pix_mp.num_planes;
    } else {
        struct v4l2_format format;

        memset(&format, 0, sizeof(format));
        format.type = buffer_type;
        if (bench_ioctl(fd, VIDIOC_G_FMT, &format) < 0) {
            fprintf(stderr, "VIDIOC_G_FMT failed: %s\n", strerror(errno));
            goto cleanup;
        }
        width = format.fmt.pix.width;
        height = format.fmt.pix.height;
        pixel_format = format.fmt.pix.pixelformat;
        format_plane_count = 1;
    }
    if (format_plane_count == 0 || format_plane_count > VIDEO_MAX_PLANES) {
        fprintf(stderr, "unsupported plane count: %u\n", format_plane_count);
        goto cleanup;
    }

    memset(&request, 0, sizeof(request));
    request.type = buffer_type;
    request.memory = V4L2_MEMORY_MMAP;
    request.count = 6;
    if (bench_ioctl(fd, VIDIOC_REQBUFS, &request) < 0) {
        fprintf(stderr, "VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        goto cleanup;
    }
    if (request.count < 2) {
        fprintf(stderr, "driver returned too few buffers: %u\n", request.count);
        goto cleanup;
    }
    mapped_count = request.count > BENCH_MAX_BUFFERS
                       ? BENCH_MAX_BUFFERS
                       : request.count;

    for (i = 0; i < mapped_count; ++i) {
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        unsigned int plane;

        memset(&buffer, 0, sizeof(buffer));
        memset(planes, 0, sizeof(planes));
        buffer.type = buffer_type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buffer.m.planes = planes;
            buffer.length = format_plane_count;
        }
        if (bench_ioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            fprintf(stderr, "VIDIOC_QUERYBUF index=%u failed: %s\n", i, strerror(errno));
            goto cleanup;
        }

        buffers[i].plane_count = format_plane_count;
        for (plane = 0; plane < format_plane_count; ++plane) {
            size_t length;
            off_t offset;

            if (buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
                length = planes[plane].length;
                offset = (off_t)planes[plane].m.mem_offset;
            } else {
                length = buffer.length;
                offset = (off_t)buffer.m.offset;
            }
            buffers[i].start[plane] = mmap(NULL,
                                            length,
                                            PROT_READ | PROT_WRITE,
                                            MAP_SHARED,
                                            fd,
                                            offset);
        if (buffers[i].start[plane] == MAP_FAILED) {
                buffers[i].start[plane] = NULL;
                fprintf(stderr, "mmap index=%u plane=%u failed: %s\n",
                        i,
                        plane,
                        strerror(errno));
                goto cleanup;
            }
            buffers[i].length[plane] = length;
            copy_capacity += length;
        }

        if (export_enabled) {
            struct v4l2_exportbuffer export_buffer;

            memset(&export_buffer, 0, sizeof(export_buffer));
            export_buffer.type = buffer_type;
            export_buffer.index = i;
            export_buffer.plane = 0;
            export_buffer.flags = O_CLOEXEC;
            if (bench_ioctl(fd, VIDIOC_EXPBUF, &export_buffer) < 0) {
                fprintf(stderr,
                        "VIDIOC_EXPBUF index=%u failed: %s\n",
                        i,
                        strerror(errno));
                goto cleanup;
            }
            close(export_buffer.fd);
            exported_count++;
        }

        if (bench_ioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
            fprintf(stderr, "VIDIOC_QBUF index=%u failed: %s\n", i, strerror(errno));
            goto cleanup;
        }
    }

    if (copy_enabled) {
        size_t max_frame_bytes = 0;

        for (i = 0; i < mapped_count; ++i) {
            size_t frame_bytes = 0;
            unsigned int plane;

            for (plane = 0; plane < buffers[i].plane_count; ++plane) {
                frame_bytes += buffers[i].length[plane];
            }
            if (frame_bytes > max_frame_bytes) {
                max_frame_bytes = frame_bytes;
            }
        }
        copy_capacity = max_frame_bytes;
        copy_buffer = malloc(copy_capacity);
        if (copy_buffer == NULL) {
            fprintf(stderr, "copy buffer allocation failed: %zu bytes\n", copy_capacity);
            goto cleanup;
        }
    } else {
        copy_capacity = 0;
    }

    if (bench_ioctl(fd, VIDIOC_STREAMON, &buffer_type) < 0) {
        fprintf(stderr, "VIDIOC_STREAMON failed: %s\n", strerror(errno));
        goto cleanup;
    }
    stream_started = 1;
    wall_started_ns = bench_now_ns(CLOCK_MONOTONIC_RAW);
    cpu_started_ns = bench_now_ns(CLOCK_PROCESS_CPUTIME_ID);

    while (1) {
        struct pollfd poll_fd;
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        int64_t now_ns;
        int poll_ret;

        now_ns = bench_now_ns(CLOCK_MONOTONIC_RAW);
        if ((double)(now_ns - wall_started_ns) / 1000000000.0 >= duration_sec) {
            break;
        }

        memset(&poll_fd, 0, sizeof(poll_fd));
        poll_fd.fd = fd;
        poll_fd.events = POLLIN;
        poll_ret = poll(&poll_fd, 1, 1000);
        if (poll_ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "poll failed: %s\n", strerror(errno));
            goto cleanup;
        }
        if (poll_ret == 0) {
            fprintf(stderr, "poll timeout\n");
            continue;
        }

        memset(&buffer, 0, sizeof(buffer));
        memset(planes, 0, sizeof(planes));
        buffer.type = buffer_type;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buffer.m.planes = planes;
            buffer.length = format_plane_count;
        }
        if (bench_ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) {
                continue;
            }
            fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", strerror(errno));
            goto cleanup;
        }
        if (buffer.index >= mapped_count) {
            fprintf(stderr, "driver returned invalid buffer index: %u\n", buffer.index);
            goto cleanup;
        }

        if (copy_enabled) {
            size_t destination_offset = 0;
            int64_t copy_started_ns = bench_now_ns(CLOCK_MONOTONIC_RAW);
            unsigned int plane;

            for (plane = 0; plane < buffers[buffer.index].plane_count; ++plane) {
                size_t bytes_used;

                if (buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
                    bytes_used = planes[plane].bytesused;
                } else {
                    bytes_used = buffer.bytesused;
                }
                if (bytes_used == 0 || bytes_used > buffers[buffer.index].length[plane]) {
                    bytes_used = buffers[buffer.index].length[plane];
                }
                if (destination_offset + bytes_used > copy_capacity) {
                    fprintf(stderr, "captured frame exceeds copy buffer\n");
                    goto cleanup;
                }
                memcpy(copy_buffer + destination_offset,
                       buffers[buffer.index].start[plane],
                       bytes_used);
                destination_offset += bytes_used;
            }
            copy_ns += bench_now_ns(CLOCK_MONOTONIC_RAW) - copy_started_ns;
            copied_bytes += destination_offset;
            if (destination_offset > 0) {
                g_copy_sink ^= copy_buffer[destination_offset - 1];
            }
        }
        frame_count++;

        if (bench_ioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
            fprintf(stderr, "VIDIOC_QBUF failed: %s\n", strerror(errno));
            goto cleanup;
        }
    }

    cpu_ended_ns = bench_now_ns(CLOCK_PROCESS_CPUTIME_ID);
    wall_ended_ns = bench_now_ns(CLOCK_MONOTONIC_RAW);
    bench_fourcc(fourcc, pixel_format);
    {
        double actual_sec = (double)(wall_ended_ns - wall_started_ns) / 1000000000.0;
        double cpu_sec = (double)(cpu_ended_ns - cpu_started_ns) / 1000000000.0;
        double fps = actual_sec > 0.0 ? (double)frame_count / actual_sec : 0.0;
        double cpu_ms_per_frame = frame_count > 0
                                      ? cpu_sec * 1000.0 / (double)frame_count
                                      : 0.0;
        double copy_ms_per_frame = frame_count > 0
                                       ? (double)copy_ns / 1000000.0 / (double)frame_count
                                       : 0.0;
        double copy_gib_per_sec = copy_ns > 0
                                      ? ((double)copied_bytes / (1024.0 * 1024.0 * 1024.0)) /
                                            ((double)copy_ns / 1000000000.0)
                                      : 0.0;

        printf("device=%s format=%s size=%ux%u planes=%u buffers=%u mode=%s\n",
               argv[1],
               fourcc,
               width,
               height,
               format_plane_count,
               mapped_count,
               argv[3]);
        if (export_enabled) {
            printf("dmabuf_export=%u/%u status=%s\n",
                   exported_count,
                   mapped_count,
                   exported_count == mapped_count ? "supported" : "incomplete");
        }
        printf("duration=%.3fs frames=%llu fps=%.3f cpu=%.3fs cpu_per_frame=%.3fms cpu_load=%.1f%%\n",
               actual_sec,
               (unsigned long long)frame_count,
               fps,
               cpu_sec,
               cpu_ms_per_frame,
               actual_sec > 0.0 ? cpu_sec * 100.0 / actual_sec : 0.0);
        if (copy_enabled) {
            printf("copied=%.3fGiB copy_per_frame=%.3fms copy_throughput=%.3fGiB/s sink=%u\n",
                   (double)copied_bytes / (1024.0 * 1024.0 * 1024.0),
                   copy_ms_per_frame,
                   copy_gib_per_sec,
                   (unsigned int)g_copy_sink);
        }
    }
    result = 0;

cleanup:
    if (stream_started) {
        bench_ioctl(fd, VIDIOC_STREAMOFF, &buffer_type);
    }
    for (i = 0; i < mapped_count; ++i) {
        unsigned int plane;

        for (plane = 0; plane < buffers[i].plane_count; ++plane) {
            if (buffers[i].start[plane] != NULL) {
                munmap(buffers[i].start[plane], buffers[i].length[plane]);
            }
        }
    }
    free(copy_buffer);
    if (fd >= 0) {
        close(fd);
    }
    return result;
}
