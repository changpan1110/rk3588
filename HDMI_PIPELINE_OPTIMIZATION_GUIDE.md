# RK3588 视频链路优化实施指南（HDMI 采集 / 转换 / 编码 / OSD）

> 适用工程：本仓库 `src/video/video_pipeline_*.c`、`src/input/input_hdmi_in.c`、`src/output/output_encode_common.c`。
> 前提事实（已确认）：
> - HDMI 采集源（/dev/video40，rk_hdmirx）输出的是 **BGR3（BGR24）裸流**，需要 BGR→NV12 转换才能进 h264_rkmpp；
> - 录像按 **4K 原生分辨率** 录制，推流需要 **缩放为 1080p**，两条支路参数不同，必须两个编码器实例；
> - OSD 十字准星目前是 CPU 逐像素写 Y 平面，需要改成 RGA 硬件绘制。

每项优化相互独立，可以单独实施、单独验证。建议按顺序做，做一项验证一项。

---

## 优化 1：HDMI 采集去掉 FFmpeg 中间层（收益最大）

### 现状问题

`src/input/input_hdmi_in.c` 走 FFmpeg `video4linux2` demuxer + `rawvideo` "decoder"：

- `av_read_frame()` 把驱动 mmap buffer **整帧拷贝**进 AVPacket；
- rawvideo 解码器再**整帧拷贝**一次成 AVFrame；
- 然后 `video_pipeline_input.c:95` 的抽帧逻辑把多余的帧**丢掉**（拷贝已经做完了）。

BGR24 一帧：1080p ≈ 6.2MB，4K ≈ 24.9MB。4K@60 源按 30fps 使用时，每秒无谓拷贝接近 3GB。

### 目标

重写 `input_hdmi_in.c` 的打开/读取路径为**原生 V4L2 mmap 采集**：

- 每帧只有一次拷贝（mmap buffer → AVFrame 自建 buffer），比现状少一次；
- 被抽帧逻辑丢掉的帧**零拷贝**（DQBUF 后直接 QBUF 归还驱动）。

### 修改方法

文件：`src/input/input_hdmi_in.c`（整体替换打开/读取实现，保留 `input_hdmi_in_check_link`）

打开流程：

```c
/* 1. open(device, O_RDWR | O_NONBLOCK)（注意要 O_RDWR，mmap 需要） */

/* 2. VIDIOC_QUERY_DV_TIMINGS + VIDIOC_S_DV_TIMINGS，
      确认有有效时序（复用现有 check_link 逻辑） */

/* 3. VIDIOC_G_FMT 拿当前格式；如果 cfg 指定了 width/height/input_format，
      用 VIDIOC_S_FMT 设置。记录实际的 width/height/pixelformat/bytesperline，
      期望 pixelformat == V4L2_PIX_FMT_BGR24，不是则打 LOGW 并按实际格式映射。 */

/* 4. VIDIOC_REQBUFS：V4L2_MEMORY_MMAP，count = 4~6；
      逐块 VIDIOC_QUERYBUF + mmap，保存 {start, length}；
      VIDIOC_QBUF 全部入队；VIDIOC_STREAMON。 */
```

读取流程（`input_hdmi_in_read`）：

```c
/* 1. poll(fd, POLLPRI? 用 POLLIN, timeout 100ms) 等帧；超时返回 APP_ERR_TIMEOUT
      （采集线程已有失败重试逻辑，timeout 直接走它的 usleep(10000) 即可） */

/* 2. VIDIOC_DQBUF 拿到 buf.index / buf.timestamp；
      - 帧时间戳优先用 v4l2 的 buf.timestamp（CLOCK_MONOTONIC），
        转成微秒填 frame->pts_us，比现在的 app_get_time_us() 更准； */

/* 3. 把帧包成 AVFrame：
      av_frame_unref(frame->av_frame);
      frame->av_frame->format = AV_PIX_FMT_BGR24;   /* 按实际 fmt 映射 */
      frame->av_frame->width/height = 实际值;
      buf0 = av_buffer_alloc(bytesperline * height);  /* 一次拷贝在这里 */
      memcpy(buf0->data, mmap_start[buf.index], bytesperline * height);
      frame->av_frame->buf[0] = buf0;
      frame->av_frame->data[0] = buf0->data;
      frame->av_frame->linesize[0] = bytesperline;    /* 用驱动的 stride，不要假设 width*3 */
      frame->av_frame->extended_data = frame->av_frame->data;
      补充 color_range/colorspace 字段（BT709 或按 DV timings 的 colorspace）； */

/* 4. 立即 VIDIOC_QBUF 把 v4l2 buffer 归还驱动（拷贝已完成，不持有驱动内存）。 */
```

关键点：

- **不要在 AVFrame 里直接引用 mmap 内存**（那样 QBUF 时机会和队列里的 av_frame_ref 引用计数打架，第一阶段先求稳，一帧一次拷贝）；
- 抽帧丢弃的收益自动获得：`vp_capture_thread` 抽帧判断在读帧之后，读失败/丢弃路径不再有额外拷贝——真正省掉的是 FFmpeg 那两层；
- AVFrame 的 `format/width/height/linesize` 必须和后续 `output_encode_prepare_frame` 的判断匹配（BGR24 → 走现有 RGA/RKRGA 转换分支，不需要动下游任何代码）；
- `input_hdmi_in_close` 对应改为：STREAMOFF → munmap → close；FFmpeg 相关字段（`ctx->decoder.*`）整段删除。

### 验证方法

1. 编译运行，确认日志里 HDMI 通道正常出帧（status 线程显示 `hdmi 30fps`）；
2. `cat /proc/<pid>/status | grep VmRSS`，对比改动前后稳态内存；用 `top` 看该进程 CPU%，BGR24 4K 场景应明显下降；
3. 看 stream timing 日志 `convert=xx ms`：转换路径应仍是 `FFmpeg-RKRGA-DMABUF` 或 `RGA-two-pass-BT709`（说明 AVFrame 格式对接正确）；
4. 拔掉/插回 HDMI 线，确认采集线程能在超时后恢复（原生实现里 poll 超时 + 上层重试即可，比 FFmpeg 路径更可控）。

---

## 优化 2：RGA 全局锁串行化 → 放开并行

### 现状问题

`src/output/output_encode_common.c:21` 的 `g_output_encode_rga_lock` 包住每一次 `imconfig(切核) + imresize/imcvtcolor + imconfig(切回)`。推流（4K→1080p 缩放转色）+ 录像（4K 转色）每帧两次 RGA 操作互斥排队，RGA 多核能力被锁死。

### 修改方法（先简后繁）

**方案 A（推荐先做）**：librga 的 `imresize/imcvtcolor/imfill` 单次调用本身是线程安全的（内部走 ioctl），直接：

1. 删除 `g_output_encode_rga_lock` 及 `output_encode_rga_resize_on_core` / `output_encode_rga_cvtcolor_on_core` 两个包装函数里的 lock/unlock；
2. 删除每次调用前后的 `imconfig(IM_CONFIG_SCHEDULER_CORE, ...)` 切核，统一让驱动默认调度（`IM_SCHEDULER_DEFAULT`）；
3. `imconfig` 是进程级全局配置，多线程下本来就不安全，去掉切核后这个隐患也消除了。

**方案 B（A 验证有效后再考虑）**：若测得单核 RGA 仍是瓶颈，用 librga 的异步接口（`rga_job_*` / `imsync`，视板子上 librga 版本）把缩放和转色提交为异步任务，提交后立即返回，编码前 sync。注意每线程独立 handle，不要共享。

### 验证方法

1. `record all` + 推流同时开，看每通道 `record timing ... encode_avg=x.xxx ms` 和 stream timing `convert=x.xx ms`，对比改动前，encode_avg 应下降且不再随并行路数显著恶化；
2. 连续跑 5 分钟无 RGA 错误日志（`rga convert failed`），确认线程安全没问题。

---

## 优化 3：OSD 十字准星改 RGA 硬件绘制

### 现状问题

`src/video/video_pipeline_osd.c:37-59`：CPU 双层循环写 Y 平面。竖臂是按 linesize 跨步写，每行踢一次 cache line，整帧 2MB 区域被扫过；且 `video_pipeline_stream.c:69` 的 `av_frame_make_writable` 在帧被共享时（USB 1080p 透传场景）会整帧拷贝 3MB 只为画十字。

### 修改方法

**文件 1：`src/video/video_pipeline_osd.c`** — 新增 RGA 实现，保留 CPU 版本做 fallback：

```c
#ifdef HAVE_LIBRGA
#include <rga/RgaApi.h>
#include <rga/im2d.h>

/* 返回 0 成功；frame 必须是调用方独占（私有）的 NV12 帧 */
static int vp_osd_draw_rga(AVFrame *f, int cx, int cy) {
    const int gap = 16, thick = 2;
    int w = f->width, h = f->height;
    rga_buffer_t img;
    im_rect rects[4];
    int n = 0, i;

    img = wrapbuffer_virtualaddr(f->data[0], w, h,
                                 RK_FORMAT_YCbCr_420_SP,
                                 f->linesize[0],   /* wstride，单位像素，NV12 bpp=1 */
                                 h);

    /* NV12 要求 x/宽 2 像素对齐，cx/cy/thick/gap 按需取偶 */
    /* 左臂 */ if (cx - gap > 0)          { rects[n++] = (im_rect){0, cy - thick, cx - gap, thick * 2 + 1}; }
    /* 右臂 */ if (cx + gap + 1 < w)      { rects[n++] = (im_rect){cx + gap + 1, cy - thick, w - (cx + gap + 1), thick * 2 + 1}; }
    /* 上臂 */ if (cy - gap > 0)          { rects[n++] = (im_rect){cx - thick, 0, thick * 2 + 1, cy - gap}; }
    /* 下臂 */ if (cy + gap + 1 < h)      { rects[n++] = (im_rect){cx - thick, cy + gap + 1, thick * 2 + 1, h - (cy + gap + 1)}; }

    for (i = 0; i < n; ++i) {
        /* 白色，硬件负责 RGBA→YUV 转换 */
        if (imfill(img, rects[i], 0xFFFFFFFF) != IM_STATUS_SUCCESS &&
            imfill(img, rects[i], 0xFFFFFFFF) != IM_STATUS_NOERROR) {
            return -1;
        }
    }
    return 0;
}
#endif
```

`vp_osd_draw` 改为：先走 `vp_osd_draw_rga`，失败则回退现有 CPU 循环（保留原代码，加 `#else` / 运行时 fallback 均可）。注意 imfill 返回值判断按工程现有风格（`IM_STATUS_SUCCESS || IM_STATUS_NOERROR`，见 `output_encode_rga_status_ok`）。

**文件 2：`src/video/video_pipeline_stream.c:67-72`** — 保证只在私有帧上画：

```c
enc_mut = (AVFrame *)enc_frame;
if (vp_osd_needed(p)) {
    if (!converted) {
        /*
         * 帧是 passthrough 共享帧（latest/rec_q 还引用着），
         * RGA 绘制绕开引用计数，直接写会污染拍照/录像帧。
         * 这种情况保留 av_frame_make_writable（会整帧拷贝一份私有副本）。
         */
        if (av_frame_make_writable(enc_mut) != 0) {
            continue;
        }
    }
    /* converted=1 时 enc_frame 就是 stream_convert 的私有 work_frame，直接画 */
    vp_osd_draw(p, enc_mut);
}
```

（`converted` 变量在 `video_pipeline_stream.c:59` 已经从 `output_encode_prepare_frame` 拿到，直接用。）

要点：

- **正确性前提**：共享帧（`converted==0` 的透传帧）绝不能跳过 make_writable 直接 RGA 画；HDMI 4K→1080p 路径必定 `converted==1`，零拷贝直接画；
- NV12 矩形参数全部取偶对齐（x、宽度至少 2 对齐），否则 RGA 报参数错；
- `imfill` 的颜色是 RGBA，硬件转 YUV；白色 `0xFFFFFFFF` 即可，想换颜色只改这个值；
- 后续要做彩色准星/圆环/文字：把图案预渲染成 BGRA 小图，每帧 `imblend`/带 alpha 的 `imcopy` 叠上去，成本相当。

### 验证方法

1. `osd on` 后拉流看画面：十字位置、粗细、颜色正常，`osd pos` 移动正常；
2. 推流源切到 USB（1080p 透传路径）再 `osd on`：确认录像文件和 `snap` 拍照里**没有**十字（验证共享帧保护生效）；
3. 对比 `osd on` 前后 stream timing 的 CPU 占用和 `convert` 耗时。

---

## 优化 4：录像队列按字节限深（4K 内存防护）

### 现状问题

`src/video/video_pipeline_internal.h:22` `VP_QUEUE_SIZE 8` 固定深度，rec_q 里存的是**转换前**的 BGR24 原始帧。4K 下一帧 24.9MB，一路录像队列峰值约 200MB，三路同录加上 stream_q 和 latest，内存压力明显。

### 修改方法

文件：`src/video/video_pipeline_queue.c` + `video_pipeline_internal.h`

1. `vp_frame_queue_t` 增加字段 `size_t byte_limit; size_t byte_count;`；
2. `vp_queue_push_locked` 开头计算本帧字节数：

```c
size_t frame_bytes = (size_t)frame->linesize[0] * (size_t)frame->height; /* BGR24 单平面 */
/* 通用算法用 av_image_get_buffer_size(frame->format, frame->width, frame->height, 1) */
```

3. 入队前循环判断：`byte_limit > 0 && byte_count + frame_bytes > byte_limit` 时丢最旧帧（复用现有 count==VP_QUEUE_SIZE 的丢弃分支，`byte_count` 同步扣减）；出队/清空时同步扣减 `byte_count`；
4. 初始化：录像队列 `ch->rec_q.byte_limit = 64 * 1024 * 1024`（64MB ≈ 4K BGR24 两帧半），stream_q 因 latest-frame 模式天然只有 1 帧可不改；
5. 统计日志 `queue peak` 已有（`max_count`），可同时记录峰值字节数方便观察。

### 验证方法

1. 4K 源 `record all`，`cat /proc/<pid>/status | grep VmRSS` 对比改动前，峰值应下降数百 MB；
2. 录像停止时打印的统计里 `dropped` 不为 0 属正常（编码跟不上时丢帧），确认录像文件可正常播放、帧率均匀。

---

## 附带小项（顺手改，各 5 行以内）

1. **HDMI 录像码率解除写死**：`video_pipeline_main.c:117` 的 `.bitrate = 16000000` 改为 `0`，让 `video_pipeline_record.c:15` 按实际分辨率自动选 8M/16M（1080p 源不会再浪费码率）。
2. **录像时间戳用采集时刻**：`video_pipeline_record.c:196` 的 `frame->pts = encode_started_us - ch->rec_start_us` 改为基于采集 pts（采集线程已把采集时刻写在 `frame.pts`），录像启动时记录第一帧采集 pts 作为基准：`frame->pts = frame->pts - ch->rec_base_pts`。消除队列排队抖动写进文件时间轴。
3. **确认转换路径**：启动后看日志 `encode convert path=`，HDMI 推流应为 `FFmpeg-RKRGA-DMABUF` 或 `RGA-two-pass-BT709`，录像应为 `RGA`。若出现 `swscale` 说明 RGA 路径没生效，优先排查。
4. **HDMI 热插拔**：优化 1 重写采集后，在采集线程连续失败 N 次（比如 50 次）时加一次 `input_hdmi_in_close` + `check_link` + 重新 open 的逻辑，替代现在 10ms 刷 WARN 的死循环。

---

## 实施与验证顺序建议

| 顺序 | 项目 | 主要收益 | 风险 |
|------|------|----------|------|
| 1 | 优化 3（OSD RGA） | 改 2 个文件，立竿见影 | 低（有 CPU fallback） |
| 2 | 优化 2（RGA 锁） | 删代码为主 | 低（librga 调用本身线程安全） |
| 3 | 优化 4（队列限深） | 内存防护 | 低 |
| 4 | 优化 1（V4L2 直采） | 省 1~2 次整帧拷贝/帧 | 中（重写采集模块，需充分验证信号异常场景） |

每项做完跑一遍 `record all` + 推流 + `osd on` + 拔插 HDMI 线的组合场景，观察日志无新增 WARN/ERROR 即合入下一项。
