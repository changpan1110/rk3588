# RK3588 推流 OSD 硬件化对接说明

## 1. 目标和结论

目标：在推流画面中叠加十字线、文字、数字、状态图标等 OSD，同时满足：

- OSD 只进入推流编码，不进入 USB/HDMI 录像文件。
- 主视频帧不由 CPU 逐像素修改。
- USB 1080p 和 HDMI 4K 输入切换后，推流输出仍为 1920x1080。
- 尽量保持 DRM PRIME / DMA-BUF 硬件帧链路，减少下载、复制和再次上传。
- 串口线程只更新 OSD 参数，不直接操作视频帧或 FFmpeg filter graph。

当前工程推荐采用：

```text
小尺寸 RGBA OSD 图层（参数变化时生成）
                         |
                         v
输入 DRM PRIME -> RGA 缩放/转 NV12 -> overlay_rkrga -> NV12 DRM PRIME
                                                    -> h264_rkmpp
                                                    -> RTSP/RTP
```

录像继续使用原来的独立录像队列和编码线程，不调用 OSD 模块。

不要继续使用当前的 CPU 主画面绘制方式：

```text
RGA -> hwdownload 到 CPU NV12
    -> av_frame_make_writable()
    -> CPU 修改 Y 平面
    -> MPP 编码
```

## 2. 当前代码位置

### 2.1 对外 OSD 参数和入口

文件：`include/video/video_pipeline.h`

现有结构：

```c
typedef struct {
    int show_crosshair;
    int cross_x;
    int cross_y;
    float distance_m;
    int signal_level;
    char text[64];
    uint32_t seq;
} vp_osd_params_t;
```

现有线程安全入口：

```c
app_status_t vp_osd_update(vp_ctx_t *ctx,
                           const vp_osd_params_t *params);
```

这个函数适合作为串口、SBUS、按键和业务线程统一更新 OSD 的底层接口。

### 2.2 高层控制接口

文件：

- `include/video/video_pipeline_control.h`
- `src/video/video_pipeline_control.c`

现有接口：

```c
app_status_t vp_control_osd_set_enabled(vp_control_t *control,
                                        int enabled);
app_status_t vp_control_osd_set_position(vp_control_t *control,
                                         int x,
                                         int y);
app_status_t vp_control_osd_get(vp_control_t *control,
                                vp_osd_params_t *out_params);
```

`vp_control_t` 内部已经有互斥锁和一份 OSD 参数副本，适合后续串口命令调用。

### 2.3 当前 CPU 绘制实现

文件：`src/video/video_pipeline_osd.c`

函数：

```c
int vp_osd_needed(vp_ctx_t *p);
void vp_osd_draw(vp_ctx_t *p, AVFrame *f);
app_status_t vp_osd_update(vp_ctx_t *p,
                           const vp_osd_params_t *params);
```

`vp_osd_draw()` 当前仅支持 CPU NV12，直接修改 `f->data[0]` 的 Y 平面绘制白色十字线。它没有使用 `distance_m`、`signal_level` 和 `text`。

### 2.4 CPU OSD 的调用点

文件：`src/video/video_pipeline_stream.c`

当前顺序：

```c
enc_frame = output_encode_prepare_frame(&p->stream_convert,
                                        frame,
                                        &converted);

if (vp_osd_needed(p)) {
    if (av_frame_make_writable(enc_mut) == 0) {
        vp_osd_draw(p, enc_mut);
    }
}

avcodec_send_frame(p->stream_enc, enc_mut);
```

这里是必须替换的核心位置。硬件 OSD 完成后，应删除推流热路径中的：

```c
av_frame_make_writable(enc_mut);
vp_osd_draw(p, enc_mut);
```

### 2.5 推流转换和编码器初始化

文件：`src/video/video_pipeline.c`

当前推流编码器使用 CPU NV12 输入：

```c
vp_open_h264_encoder(...,
                     AV_PIX_FMT_NV12,
                     NULL,
                     ...);

output_encode_convert_init(&p->stream_convert,
                           1920,
                           1080,
                           AV_PIX_FMT_NV12);
```

完整硬件 OSD 目标应改为：

- 转换输出：`AV_PIX_FMT_DRM_PRIME`，软件格式为 NV12。
- OSD 合成输出：`AV_PIX_FMT_DRM_PRIME`，软件格式为 NV12。
- 推流编码器输入：`AV_PIX_FMT_DRM_PRIME`。
- 编码器 `hw_frames_ctx` 来自硬件 OSD 输出帧。

这通常需要像录像预热逻辑一样，在拿到第一帧后再完成推流 filter graph 和编码器初始化。

### 2.6 RGA 转换模块

文件：

- `src/output/output_encode_common.c`
- `include/output/output_encode_common.h`
- `src/output/output_encode_rkrga_filter.c`
- `include/output/output_encode_rkrga_filter.h`

现有能力：

- DRM PRIME 硬件帧输入。
- USB NV16 DRM -> NV12。
- HDMI BGR24 DRM -> NV12。
- HDMI 4K -> 推流 1080p 缩放。
- CPU NV12 或 DRM PRIME NV12 输出。

当前 `output_encode_rkrga_filter` 只有一个视频输入，只负责缩放和格式转换；硬件 OSD 需要增加第二个 RGBA 输入和 `overlay_rkrga`。

### 2.7 录像与推流分流位置

文件：`src/video/video_pipeline_input.c`

采集线程将同一采集帧分别引用到两个独立队列：

```c
if (ch->record_wanted) {
    vp_queue_push_locked(&ch->rec_q, frame.av_frame);
}

if (p->stream_enabled && ch->id == p->stream_ch) {
    vp_queue_push_latest_locked(&p->stream_q, frame.av_frame);
}
```

文件：`src/video/video_pipeline_record.c`

录像线程只从 `rec_q` 取帧，然后调用录像转换和编码函数。它没有调用 OSD。因此，只要硬件 OSD 保持在 `vp_stream_thread()` 内，录像文件天然不包含 OSD。

### 2.8 旧的占位模块

文件：

- `src/process/process_osd.c`
- `include/process/process_osd.h`

`process_osd_apply_laser()` 当前只打印日志，没有实际绘制。不要同时在这里和推流线程实现两套 OSD。建议保留为业务数据适配层，最终仍调用 `vp_control_osd_update()` 或 `vp_osd_update()`。

## 3. 板端能力验证结果

2026-08-04 在当前 RK3588 安装环境验证：

### 3.1 FFmpeg RGA filter

已安装：

```text
scale_rkrga
vpp_rkrga
overlay_rkrga
```

`overlay_rkrga` 支持：

```text
x / y
global alpha: 0..255
straight / premultiplied alpha
output pixel format
repeatlast
RGA core selection
async_depth
AFBC
```

当前工程实际使用的 `/home/cat/rk3588/install/bin/ffmpeg` 已编译
`drawtext` filter，构建参数包含 `--enable-libfreetype`、
`--enable-libharfbuzz`、`--enable-libfontconfig` 和
`--enable-libfribidi`。

但是 `drawtext` 仍是 CPU filter，不能直接在 DRM PRIME 主视频帧上完成
RGA 硬件文字合成。它可以作为“小尺寸 RGBA OSD 图层”的文字生成方案，
但不要对每一帧 1920x1080/3840x2160 主视频执行 `hwdownload -> drawtext`。

### 3.2 h264_rkmpp

当前 `h264_rkmpp` 支持 `drm_prime` 输入：

```text
Supported pixel formats: ... nv12 ... bgr24 ... drm_prime
```

但当前 FFmpeg encoder AVOptions 没有暴露 OSD、region 或 palette 参数。

### 3.3 MPP 底层 OSD

板端 MPP 头文件 `rk_venc_cmd.h` 确实包含编码器 OSD：

- 最多 8 个 region。
- 256 色 palette。
- `MppEncOSDRegion` / `MppEncOSDData` 等结构。

但是这些底层接口目前没有通过工程使用的 `h264_rkmpp` FFmpeg 封装暴露出来。直接使用需要修改 FFmpeg rkmpp encoder 或绕过 `AVCodecContext` 调用 MPP，改动面明显更大。

结论：当前工程优先使用 `overlay_rkrga`。MPP OSD 只作为后续专项方案。

## 4. 推荐硬件 OSD 架构

### 4.1 视频数据流

```text
                         +--------------------------+
采集 DRM PRIME ----------> rec_q -> 录像转换/编码 ----> MP4（无 OSD）
       |
       +------------------> stream_q
                              |
                              v
                    RGA 缩放和格式转换
                  USB NV16 / HDMI BGR24
                         -> 1080p NV12 DRM
                              |
                              v
RGBA OSD surface -------> overlay_rkrga
                              |
                              v
                       1080p NV12 DRM
                              |
                              v
                         h264_rkmpp
                              |
                              v
                           RTSP/RTP
```

### 4.2 OSD 图层生成原则

RGA 是 2D 缩放、转换和混合引擎，不是字体排版引擎。字符串必须先变成像素图层。

推荐：

1. 创建一张透明 RGBA/BGRA OSD surface，大小可以是整张 1920x1080，也可以是覆盖内容的最小矩形。
2. 仅当 `vp_osd_params_t.seq` 变化时，重新生成 OSD surface。
3. 视频每帧只执行一次 `overlay_rkrga` 硬件合成。
4. OSD 参数未变化时，重复使用同一张硬件 OSD 帧。

允许 CPU 做的事情：

- 格式化字符串，例如 `DIST 123.4 m`。
- 在小尺寸透明图层中生成字形。
- 参数变化时更新几十 KB 或几百 KB 的 OSD 图层。

不允许 CPU 做的事情：

- 下载整张 1920x1080/3840x2160 主画面。
- 每帧遍历主画面像素。
- 每帧重新渲染没有变化的文字。

## 5. 建议增加的文件

为了避免继续扩张 `output_encode_rkrga_filter.c` 的职责，建议新增独立模块：

```text
include/output/output_osd_rkrga.h
src/output/output_osd_rkrga.c
```

建议职责：

```c
typedef struct output_osd_rkrga output_osd_rkrga_t;

app_status_t output_osd_rkrga_init(
    output_osd_rkrga_t **out,
    const AVFrame *first_input,
    int output_width,
    int output_height);

app_status_t output_osd_rkrga_set_overlay(
    output_osd_rkrga_t *ctx,
    const AVFrame *rgba_overlay,
    int x,
    int y,
    int global_alpha,
    uint32_t sequence);

const AVFrame *output_osd_rkrga_process(
    output_osd_rkrga_t *ctx,
    const AVFrame *input);

AVBufferRef *output_osd_rkrga_hw_frames_ctx(
    output_osd_rkrga_t *ctx);

void output_osd_rkrga_deinit(
    output_osd_rkrga_t **ctx);
```

输出层不要直接依赖 `vp_osd_params_t`。`src/video/video_pipeline_osd.c` 负责把业务参数渲染成 RGBA 图层；`output_osd_rkrga` 只负责硬件转换、图层上传和合成。

`process()` 的输出必须是：

```text
format = AV_PIX_FMT_DRM_PRIME
sw_format = AV_PIX_FMT_NV12
size = 1920x1080
```

如果 OSD 关闭，模块仍应返回硬件转换后的 DRM PRIME 帧，或者采用硬件直通分支；不能退回 CPU NV12。

## 6. 参数接口建议

现有结构可以兼容扩展，不需要推翻：

```c
#define VP_OSD_TEXT_MAX 128

typedef struct {
    int enabled;

    int show_crosshair;
    int cross_x;
    int cross_y;

    int show_telemetry;
    float distance_m;
    int signal_level;

    int show_text;
    char text[VP_OSD_TEXT_MAX];
    int text_x;
    int text_y;

    uint32_t text_argb;
    uint32_t background_argb;
    int font_size;

    uint32_t seq;
} vp_osd_params_t;
```

坐标建议统一使用推流输出坐标系，即 1920x1080，而不是 HDMI 原始 4K 坐标。这样 USB/HDMI 切换时 OSD 位置不会变化。

保留统一底层更新接口：

```c
app_status_t vp_osd_update(vp_ctx_t *ctx,
                           const vp_osd_params_t *params);
```

建议为串口层增加高层接口：

```c
app_status_t vp_control_osd_update(
    vp_control_t *control,
    const vp_osd_params_t *params);

app_status_t vp_control_osd_set_text(
    vp_control_t *control,
    const char *text,
    int x,
    int y);

app_status_t vp_control_osd_set_telemetry(
    vp_control_t *control,
    float distance_m,
    int signal_level);
```

串口线程调用示例：

```c
vp_osd_params_t osd;

vp_control_osd_get(control, &osd);
osd.enabled = 1;
osd.show_telemetry = 1;
osd.distance_m = serial_data.distance_m;
osd.signal_level = serial_data.signal_level;
snprintf(osd.text, sizeof(osd.text),
         "DIST %.1fm  SIG %d",
         osd.distance_m,
         osd.signal_level);
vp_control_osd_update(control, &osd);
```

串口线程不能直接调用 `output_osd_rkrga_process()`，也不能直接修改 OSD surface。

## 7. 文字和数字的生成方式

### 7.1 第一阶段推荐：位图字库

适合数字、英文、单位和固定符号：

- 预先准备 0-9、A-Z、`.`、`-`、`:`、`%` 等 RGBA glyph。
- OSD 参数变化时，将 glyph 组合到透明 surface。
- 可先使用 CPU 对小 surface 做 memcpy；主画面仍全程硬件处理。
- 后续可把 glyph atlas 放到 DMA-BUF，由 RGA blit 组合。

优点：依赖少、速度稳定、容易在嵌入式系统部署。

### 7.2 需要中文或可变字体：FreeType

可选用 FreeType 把 UTF-8 文本渲染到 RGBA surface。只在文字变化时执行，不在每帧执行。

如果采用 FreeType，需要修改 `CMakeLists.txt` 检测和链接 `freetype2`。当前工程没有该依赖。

也可以通过当前 FFmpeg 已有的 `drawtext` filter 生成小尺寸透明 RGBA
图层。应在文字内容变化时生成一帧并缓存，然后交给 `overlay_rkrga`
重复合成；不要让 `drawtext` 在主视频的每一帧上运行。

### 7.3 固定图标

告警、电池、连接状态等固定图标可以预生成 RGBA 数据或 PNG 解码后缓存。状态变化时选择对应 surface，不需要每帧解码图片。

## 8. 推流线程改造位置

文件：`src/video/video_pipeline_stream.c`

目标伪代码：

```c
while (!p->stop) {
    pop_stream_frame(frame);

    if (p->stream_osd == NULL || source_context_changed(frame)) {
        rebuild_stream_hardware_pipeline(p, frame);
    }

    snapshot_osd_params(p, &params);
    if (params.seq != p->stream_osd_applied_seq) {
        overlay = vp_osd_render_rgba(p, &params);
        output_osd_rkrga_set_overlay(p->stream_osd,
                                     overlay,
                                     params.text_x,
                                     params.text_y,
                                     255,
                                     params.seq);
        p->stream_osd_applied_seq = params.seq;
    }

    enc_frame = output_osd_rkrga_process(p->stream_osd, frame);
    if (enc_frame == NULL) {
        continue;
    }

    enc_frame->pts = frame->pts - p->stream_epoch_us;
    avcodec_send_frame(p->stream_enc, enc_frame);
}
```

需要删除的旧 CPU 路径：

```c
av_frame_make_writable(enc_mut);
vp_osd_draw(p, enc_mut);
```

建议在 timing 日志中拆分：

```text
queue
rga_convert
osd_update（仅参数变化时）
rga_overlay
codec
packet_age
```

## 9. 初始化和通道切换

### 9.1 推流编码器延迟初始化

当前推流编码器在 `vp_init()` 中、收到第一帧之前打开。要把编码器改成 DRM PRIME 输入，建议：

1. `vp_init()` 只创建线程、队列和 RTSP/RTP 状态。
2. 推流线程拿到第一帧后创建 RGA OSD graph。
3. 先处理一帧，获得输出 DRM PRIME 的 `hw_frames_ctx`。
4. 使用该 `hw_frames_ctx` 打开 `h264_rkmpp`。
5. 第一帧强制 IDR。

可以参考 `src/video/video_pipeline_record.c` 中 HDMI 录像预热和 DRM encoder 打开的实现。

### 9.2 USB/HDMI 切换

USB 输入软件格式为 NV16，HDMI 输入软件格式为 BGR24，输入 `hw_frames_ctx` 也可能不同。

切换时应：

- 清空推流队列。
- 重建输入相关的 RGA filter graph。
- 保留 OSD 参数和已生成的 OSD surface。
- 确认新 graph 输出的 DRM frames context 与编码器兼容。
- 必要时重新打开推流编码器，并强制 IDR。

如果重新打开编码器会影响无缝切换，应进一步实现稳定的共享 RKMPP device/output frame pool，使所有输入最终写入同一类 1080p NV12 DRM 输出缓冲区。

## 10. 线程和资源规则

- `vp_osd_update()` 只在 `osd_lock` 下复制结构体并递增 `seq`。
- 推流线程读取 OSD 参数快照后立即释放锁。
- FFmpeg filter graph、RGA surface、AVFrame 和 DMA-BUF 只由推流线程创建、更新和销毁。
- 串口线程不能持有 `AVFrame *`。
- OSD surface 更新时使用双缓冲，避免 RGA 正在读取的 surface 被同时改写。
- OSD 关闭时保持透明 surface 或硬件直通，不要销毁/重建 graph。
- 退出时先停止推流线程，再释放 OSD graph、surface、encoder 和 packet。

## 11. 不建议的实现

### 11.1 每帧 CPU drawtext/drawbox

会让整张主画面进入 CPU 路径，增加内存带宽、缓存污染和延迟。

### 11.2 继续直接写 NV12 Y 平面

当前十字线只改 Y，不改 UV，只能得到灰白线；文字和颜色扩展困难，并且依赖 CPU 可写帧。

### 11.3 在采集线程叠加 OSD

采集帧同时进入录像和推流队列。在采集线程修改会污染录像，违反“录像无 OSD”的要求。

### 11.4 直接假设 h264_rkmpp 已支持 FFmpeg OSD 参数

MPP SDK 有 OSD region，但当前安装的 FFmpeg encoder 没有暴露对应 AVOptions。没有完成封装验证前，不应直接把 MPP OSD 作为现成接口使用。

## 12. 分阶段实施顺序

### 阶段 A：接口和小图层

- 扩展 `vp_osd_params_t`。
- 增加 `vp_control_osd_update()`、文字和遥测接口。
- 实现位图数字/英文到 RGBA surface。
- 保持现有 CPU 十字线作为临时回退。

### 阶段 B：RGA 合成验证

- 新增 `output_osd_rkrga.c/.h`。
- 使用 `overlay_rkrga` 合成一张固定 RGBA 图标。
- 先验证 USB 和 HDMI 两种输入。
- 验证 Alpha、坐标、颜色和 source switch。

### 阶段 C：全硬件推流链路

- stream convert 输出改成 DRM PRIME NV12。
- overlay 输出保持 DRM PRIME NV12。
- h264_rkmpp 改为 DRM PRIME 输入。
- 删除 `hwdownload`、`av_frame_make_writable()` 和 CPU 主画面绘制。

### 阶段 D：串口和动态数据

- 串口解析后只调用 control API。
- 以 `seq` 控制 surface 更新。
- 增加数字、文字、图标和告警状态。

## 13. 验证标准

### 13.1 日志标准

开启 OSD 后应看到类似：

```text
stream convert output=drm_prime sw_fmt=nv12
overlay_rkrga opened main=nv12 DRM overlay=rgba output=nv12 DRM
h264 encoder opened pix_fmt=drm_prime sw_fmt=nv12
```

推流热路径不应看到：

```text
hardware-download
hwdownload,format=pix_fmts=nv12
av_frame_make_writable failure
swscale
```

### 13.2 功能标准

- `osd off`：推流无 OSD。
- `osd on`：推流显示 OSD。
- USB1、USB2、HDMI 切换后位置和比例正确。
- 录像文件中不出现 OSD。
- 拍照仍使用原始通道帧，不出现推流 OSD。
- 文字、数字变化时画面更新，不变化时不重复生成 surface。

### 13.3 性能标准

分别测试：

```text
USB 推流，OSD off/on
HDMI 推流，OSD off/on
HDMI 推流 + HDMI 4K 录像，OSD off/on
三通道同时录像 + 任意通道推流，OSD off/on
```

关注：

- 推流 30fps。
- 录像 30fps、dropped=0。
- `packet_age` 不应因为 OSD 明显增加。
- OSD on/off 的 CPU 占用差异应很小。
- OSD 参数高频更新时不能阻塞推流线程。

建议保留每秒日志：

```text
frames packets queue rga_convert rga_overlay codec packet_age max
```

## 14. 交接给后续实现者的重点

1. OSD 必须只放在 `vp_stream_thread()` 路径，不能放到采集线程或录像线程。
2. 目标不是“用 RGA 修改 CPU NV12”，而是保持主画面为 DRM PRIME，并由 RGA 合成后直接送 h264_rkmpp。
3. RGA 不负责字体排版；文字先生成小 RGBA 图层，再由 `overlay_rkrga` 合成。
4. 当前项目 FFmpeg 已确认同时有 `overlay_rkrga` 和 `drawtext`；前者是
   RGA 硬件合成，后者是 CPU 文字渲染，不能混为同一种硬件能力。
5. 当前 h264_rkmpp 支持 DRM PRIME，但没有暴露 MPP OSD AVOptions。
6. `vp_osd_update()` 是业务/串口参数入口；FFmpeg 和 RGA 对象必须由推流线程独占。
7. 通道切换会改变输入格式和硬件 frames context，必须设计 graph 重建和 IDR 处理。
8. 完成后必须确认录像、拍照不含 OSD，并对比 OSD on/off 的延迟和 CPU 数据。
