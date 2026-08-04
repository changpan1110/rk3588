# RK3588 HDMI Pipeline Optimization Review

本文档评审 `HDMI_PIPELINE_OPTIMIZATION_GUIDE.md` 中的建议，并结合当前代码、板端日志和已经完成的优化，给出实施优先级与验证方法。

目标不是逐条照搬原指南，而是先确认每项建议的前提是否成立，再决定是否进入生产代码。

## 1. 当前系统事实

### 1.1 HDMI 输入

- 设备：`/dev/video40`
- 实际格式：`3840x2160@30 BGR3`
- 驱动支持枚举 `BGR3/NV24/NV16/NV12`，但当前 4K 模式下只有 `BGR3` 可以成功设置。
- HDMI 是 rawvideo，不存在压缩视频硬解码过程。

### 1.2 当前处理路径

1080P 推流：

```text
HDMI BGR3 4K
  -> FFmpeg-RKRGA hwupload
  -> RGA3: 4K BGR3 缩放到 1080P BGR3
  -> RGA2: BGR3 转 NV12
  -> hwdownload
  -> CPU NV12 OSD
  -> h264_rkmpp
```

4K 录像：

```text
HDMI BGR3 4K
  -> FFmpeg-RKRGA hwupload
  -> RGA2: BGR3 转 NV12
  -> DRM/NV12
  -> h264_rkmpp DRM PRIME 输入
  -> MP4
```

USB 已经使用：

```text
MJPEG -> mjpeg_rkmpp -> DRM/NV16 -> RKRGA -> NV12
```

USB 不再在输入模块中每帧提前下载到 CPU。

### 1.3 当前实测性能

| 场景 | 关键数据 |
|---|---:|
| USB1 推流转换 | `2.84-2.87ms` |
| USB2 推流转换 | 稳定后约 `2.25ms` |
| HDMI 单独推流转换 | `15.69-15.73ms`，偶尔约 `14.65ms` |
| HDMI 推流总延迟 | `20.63-20.68ms` |
| HDMI 推流 + 4K 录像转换 | `17.27-17.76ms` |
| HDMI 推流 + 4K 录像总延迟 | `21.75-22.47ms` |
| HDMI 4K 录像总处理 | `27.53-27.82ms` |
| HDMI 4K 录像转换 | `13.07-13.44ms` |
| HDMI 4K 编码和封装 | `14.35-14.46ms` |

4K 录像结果：`30.03fps`、`dropped=0`、`queue_peak=1/8`。

30fps 的单帧预算为 `33.33ms`。当前 4K 录像剩余约 `5.5ms` 余量。

## 2. 原指南逐项评审

## 2.1 优化1：去掉 FFmpeg HDMI 采集中间层

### 结论

**A/B基准已经证明收益显著，适合实施；但实现必须采用延迟QBUF的零复制生命周期，不能照搬原指南的逐帧memcpy方案。**

### 合理部分

- 原生 V4L2 可以精确控制 `poll/DQBUF/QBUF`、时间戳、热插拔和缓冲区数量。
- 如果 FFmpeg V4L2 demuxer和rawvideo解码确实产生额外整帧复制，原生采集可能明显降低CPU和内存带宽。
- 4K BGR3 每帧约 `24.9MB`。每减少一次整帧复制，理论内存流量可减少约 `747MB/s`。
- 使用 V4L2 原始时间戳比在读取完成后调用 `app_get_time_us()` 更准确。

### 原指南中不合理或当时尚未证实部分

- 原指南直接认定“V4L2 demuxer复制一次、rawvideo解码再复制一次”。板端结果支持demux约有一次完整复制，但rawvideo解码只额外增加约 `1.67ms/帧`，不支持“再完整复制一次”的说法。
- 测试前无法仅凭FFmpeg通用实现推断Rockchip版本的实际复制次数，因此先做A/B基准仍然是必要步骤。
- 原指南建议的“原生V4L2 + 每帧memcpy到新AVFrame”本身仍有一次 `24.9MB` 复制，不一定比当前FFmpeg路径快。
- 当前 `stream convert` 和 `record total` 的计时起点都在采集完成以后。即使采集CPU下降，这两个日志数值也不一定直接下降，主要收益可能表现为CPU下降和并发时RGA竞争减轻。
- 重写采集模块会扩大热插拔、stride、多平面格式、驱动buffer生命周期和退出阻塞等风险。

### 当前处理

已经新增独立基准工具：

```text
src/tools/v4l2_capture_bench.c
target: rk3588_v4l2_capture_bench
```

工具支持：

- `nocopy`：只执行 `poll -> DQBUF -> QBUF`
- `copy`：每帧额外执行一次完整 `memcpy`
- 输出fps、CPU时间、每帧CPU时间、复制时间和复制带宽

基准已经超过实施阈值。`input_hdmi_in.c` 当前已增加BGR3原生V4L2 MMAP路径，并保留FFmpeg作为其他格式的回退路径。

### 进入生产代码的判定标准

| 原生copy相对FFmpeg路径收益 | 决策 |
|---|---|
| 每帧节省大于 `3ms`，或CPU下降超过 `10-15` 个百分点 | 值得重写 |
| 每帧节省 `1-3ms` | 结合并发测试决定 |
| 每帧节省小于 `1ms` | 不建议重写 |

## 2.2 优化2：删除RGA全局锁并开放并行

### 结论

**对当前主路径没有收益，不应直接删除。**

### 原因

- 当前正常日志为 `FFmpeg-RKRGA-DMABUF` 或 `FFmpeg-RKRGA-hardware-frame`。
- 主路径使用 FFmpeg `scale_rkrga` filter，不调用 `output_encode_rga_resize_on_core()` 和 `output_encode_rga_cvtcolor_on_core()`。
- `g_output_encode_rga_lock` 只保护直接librga回退路径。
- 当前HDMI录像和推流已经通过两个独立FFmpeg filter graph并行提交RGA任务。
- 录像开启时推流转换从约 `15.7ms` 增加到约 `17.3ms`，这是RGA硬件和内存带宽竞争，不是该pthread锁造成的。

### 原指南风险

- `imconfig(IM_CONFIG_SCHEDULER_CORE, ...)` 是进程级配置。保留每次切核逻辑却删除mutex会产生线程竞争。
- 如果要删除mutex，必须同时完全删除运行时 `imconfig` 切核，并接受驱动默认调度。
- 即使这样修改，也只影响回退路径，不能降低当前正常RKRGA路径时间。

### 建议

- 保留现有锁，保证回退路径正确性。
- 如果日志出现 `RGA`、`RGA-two-pass-BT709` 或 `swscale`，再单独分析回退路径。
- 不把删除锁作为当前性能优化项目。

## 2.3 优化3：OSD改为RGA硬件绘制

### 结论

**单独实施收益很小，当前不建议。它应当和“推流DRM直通”一起设计。**

### 当前事实

- HDMI和USB推流正常路径都会产生新的CPU NV12输出帧，`converted=1`。
- OSD在编码器接收帧之前执行，此时通常不存在编码器持有的额外引用。
- 因此 `av_frame_make_writable()` 通常不会复制整帧。
- 当前十字准星只写少量Y平面字节，不会真正遍历整个2MB NV12帧。
- 四次 `imfill()` 会产生多次RGA ioctl和调度开销，可能比当前CPU写Y平面更慢。

### 原指南中有意义的部分

- `converted==0` 时必须保护共享帧，不能直接修改录像或snapshot仍在引用的帧。
- 如果未来推流保留DRM/NV12硬件帧，RGA OSD可以避免为了画准星而执行 `hwdownload`。
- 彩色图形、圆环、字体和半透明图层适合使用RGA blend，而不是CPU逐像素绘制。

### 建议

- 当前CPU OSD保持不变。
- 后续把以下项目合并实施：

```text
RKRGA输出DRM/NV12
  -> RGA硬件OSD
  -> h264_rkmpp DRM PRIME输入
```

- 单独替换CPU十字绘制，不作为当前降时项目。

## 2.4 优化4：录像队列按字节限深

### 结论

**适合作为内存保护，不会降低当前处理时间。**

### 合理部分

- 4K BGR3单帧约 `24.9MB`，固定8帧理论上可引用约 `199MB` 图像数据。
- 按字节限制比所有格式统一使用8帧更符合实际内存成本。
- 编码器异常或存储阻塞时，可以防止队列短时间占用过多内存。

### 当前实际情况

- HDMI 4K录像实测 `queue=0`、`queue_peak=1`、`dropped=0`。
- 正常运行时队列只持有约一帧，不存在200MB稳定占用。
- 把上限改成64MB不会降低当前延迟，只会改变异常情况下的最大内存和丢帧策略。
- DRM帧和共享AVFrame引用的实际物理内存不能简单用 `av_image_get_buffer_size()`准确表示。

### 建议

- 可以增加 `byte_limit/byte_count/peak_bytes`，但将其定义为可靠性改进。
- HDMI BGR3录像建议限制为2到3帧，或约64到80MB。
- USB DRM帧继续优先按帧数限制，同时记录引用帧数量。
- 实施后必须测试存储阻塞、编码器异常和停止录像时的队列清理。

## 3. 原指南附带小项

## 3.1 HDMI录像码率自动选择

**合理，但当前性能收益为零。**

- 当前4K固定16Mbps和自动选择结果相同。
- 改为0可以避免将来把HDMI输入改成1080P后仍使用16Mbps。
- 属于配置维护，不是性能优化。

## 3.2 录像时间戳使用采集时刻

**合理，建议实施。它改善时间轴正确性，不降低编码时间。**

- 当前录像PTS基于录像线程实际处理时间。
- 队列出现排队时，编码线程时间会把队列抖动写入文件时间轴。
- 应使用采集线程写入的 `frame->pts`，并以第一张实际录像帧为 `rec_base_pts`。
- 需要处理warmup帧、PTS回退、时间戳非单调和停止后重新录像。

## 3.3 确认转换路径

**合理，已经在使用。**

正常路径应为：

```text
USB:  FFmpeg-RKRGA-hardware-frame
HDMI推流: FFmpeg-RKRGA-DMABUF
HDMI录像: FFmpeg-RKRGA-DMABUF -> drm_prime(nv12)
```

出现 `swscale`、`hardware-download` 或持续RGA错误时应立即排查。

## 3.4 HDMI热插拔恢复

**合理且重要，但属于可靠性，不降低正常处理时间。**

- 当前连续读取失败只会打印日志并10ms重试。
- 信号恢复后是否能继续出帧取决于FFmpeg V4L2上下文和驱动状态。
- 建议增加连续失败阈值，并执行关闭、重新检查DV timings和重新打开。
- 需要避免采集线程退出与主线程deinit同时关闭同一上下文。

## 4. 原指南未覆盖但更有价值的优化

## 4.1 HDMI录像DRM PRIME直通

**已经完成并验证。**

优化前：

```text
BGR3 -> RKRGA -> CPU NV12 -> h264_rkmpp
```

优化后：

```text
BGR3 -> RKRGA -> DRM/NV12 -> h264_rkmpp
```

4K录像总时间从约 `30-31ms` 降至 `27.5-27.8ms`，降低约 `10%-12%`。

## 4.2 USB解码硬件帧保留

**已经完成并验证。**

USB推流转换时间从约 `11.5-11.9ms` 降至 `2.25-2.87ms`。

## 4.3 HDMI录像和推流共享硬件中间帧

**理论收益较大，但需要重构。**

当前同一张4K BGR3帧由录像和推流分别上传并处理：

```text
录像: BGR3 4K -> NV12 4K
推流: BGR3 4K -> BGR3 1080P -> NV12 1080P
```

可以考虑在专用预处理线程生成一次4K NV12 DRM帧，再分支：

```text
4K NV12 DRM
  -> 4K录像编码器
  -> RGA3缩放到1080P NV12 -> 推流编码器
```

收益是减少重复的BGR上传和1080P颜色转换，代价是增加跨线程同步、硬件帧池管理和录像开关状态处理。

## 4.4 推流DRM PRIME直通

**有意义，但必须同时解决OSD。**

- `h264_rkmpp`已经确认支持 `drm_prime`。
- OSD关闭时可以直接编码RKRGA硬件输出。
- OSD开启时需要RGA硬件绘制，或者重新回到CPU NV12路径。
- 动态开关OSD时不能随意向同一个编码器混送CPU NV12和DRM PRIME，需要固定模式或重新打开编码器。

## 5. 验证计划

## 5.1 上传和编译V4L2基准工具

Windows PowerShell：

```powershell
scp -i .\codex_rk3588_key .\src\tools\v4l2_capture_bench.c cat@192.168.31.14:/home/cat/rk3588/src/tools/
scp -i .\codex_rk3588_key .\CMakeLists.txt cat@192.168.31.14:/home/cat/rk3588/

ssh -i .\codex_rk3588_key cat@192.168.31.14 "cd /home/cat/rk3588/build_codex && cmake .. && cmake --build . --target rk3588_v4l2_capture_bench -j8"
```

运行测试前先退出 `rk3588_video_pipeline_main`，保证 `/dev/video40` 没有被占用。

## 5.2 原生V4L2基准

```bash
cd /home/cat/rk3588/build_codex

./rk3588_v4l2_capture_bench /dev/video40 20 nocopy
./rk3588_v4l2_capture_bench /dev/video40 20 copy
```

重点记录：

```text
fps
cpu_per_frame
cpu_load
copy_per_frame
copy_throughput
```

## 5.3 FFmpeg路径基准

只测V4L2 demux和packet传递：

```bash
ffmpeg -hide_banner -benchmark \
  -f v4l2 -video_size 3840x2160 -input_format bgr24 \
  -i /dev/video40 -t 20 -c:v copy -f null - \
  2>&1 | tee /home/cat/hdmi_ffmpeg_demux.log
```

测试FFmpeg demux加rawvideo解码：

```bash
ffmpeg -hide_banner -benchmark \
  -f v4l2 -video_size 3840x2160 -input_format bgr24 \
  -i /dev/video40 -t 20 -f null - \
  2>&1 | tee /home/cat/hdmi_ffmpeg_decode.log
```

查看结果：

```bash
grep -E "frame=|bench:" /home/cat/hdmi_ffmpeg_*.log
```

计算：

```text
FFmpeg每帧CPU时间 = (utime + stime) / frames
```

将该值与原生工具的 `cpu_per_frame` 比较。

## 5.4 生产链路压力测试

```text
switch hdmi
record all
osd on
```

运行至少5分钟，然后：

```text
stop all
```

验收标准：

- 三路采集保持30fps
- 所有录像 `dropped=0`
- HDMI录像 `total < 30ms`，至少必须 `< 33.33ms`
- HDMI录像 `queue_peak <= 2`
- HDMI推流 `packet_age < 25ms`
- 没有持续 `RGA`, `MPP`, `RTSP` 或 `V4L2` ERROR
- MP4可播放，帧率和时长正确

## 5.5 2026-08-04板端A/B实测结果

原生V4L2 MMAP，不复制帧数据：

| 轮次 | 帧数/时间 | FPS | CPU/帧 | CPU负载 |
|---:|---:|---:|---:|---:|
| 1 | `595/20.028s` | `29.708` | `0.007ms` | `0.0%` |
| 2 | `594/20.004s` | `29.694` | `0.007ms` | `0.0%` |
| 3 | `594/20.003s` | `29.695` | `0.007ms` | `0.0%` |

原生V4L2 MMAP，每帧额外复制一次完整4K BGR3：

| 轮次 | FPS | CPU/帧 | 复制/帧 | CPU负载 | 复制吞吐 |
|---:|---:|---:|---:|---:|---:|
| 1 | `29.661` | `20.726ms` | `20.718ms` | `61.5%` | `1.119GiB/s` |
| 2 | `29.657` | `21.306ms` | `21.287ms` | `63.2%` | `1.089GiB/s` |
| 3 | `29.658` | `20.741ms` | `20.732ms` | `61.5%` | `1.118GiB/s` |

平均一次完整4K BGR3复制需要约 `20.92ms/帧`，占用约 `62.1%` 的单核CPU。

FFmpeg实测：

| 路径 | 帧数 | utime+stime | CPU/帧 | 约合CPU负载 |
|---|---:|---:|---:|---:|
| V4L2 demux + packet copy | `600` | `12.194s` | `20.32ms` | `60.9%` |
| V4L2 demux + rawvideo decode | `600` | `13.196s` | `21.99ms` | `65.9%` |

结论：

- FFmpeg demux的 `20.32ms/帧` 与原生完整复制的 `20.92ms/帧` 高度接近。
- rawvideo解码层在demux基础上又增加约 `1.67ms/帧`。
- 原指南中“当前FFmpeg HDMI采集存在显著整帧复制成本”的判断已经得到板端数据支持。
- 原生V4L2零复制预计可减少约 `20-22ms/帧` CPU时间，降低约 `61-66%` 的单核负载。
- `nocopy`的约 `29.7fps` 与 `copy`完全相同，少于600帧主要是STREAMON后的起帧时间计入了20秒窗口，不是复制路径吞吐不足。

基于该结果，原生V4L2替换FFmpeg HDMI BGR3采集从“待验证”调整为“建议实施”。当前实现采用MMAP缓冲区，并将V4L2 buffer包装为带释放回调的 `AVBufferRef`；录像、推流和快照的最后一个引用释放后才执行 `VIDIOC_QBUF`，避免驱动提前覆盖仍在使用的帧。

生产回归发现MMAP CPU帧会使后续 `hwupload` 变慢，因此生产程序当前默认继续使用FFmpeg。只有显式设置下面的环境变量才启用原生MMAP实验路径：

```bash
RK_HDMI_NATIVE_V4L2=1 ./rk3588_video_pipeline_main
```

采集线程现在每秒输出：

```text
hdmi capture timing frames=30 thread_cpu=... cpu_per_frame=... cpu_load=...%
```

FFmpeg路径的HDMI采集线程约为 `20-22ms/帧`、`61-66%` 单核负载；原生路径采集线程本身已显著下降，但MMAP CPU帧不能直接作为最终生产方案。USB线程不受该开关影响。

## 5.6 原生MMAP生产回归结果

三路录像并将推流切到HDMI后，原生MMAP路径结果：

| 项目 | 结果 |
|---|---:|
| HDMI采集线程CPU | `0.02-0.07ms/帧`，约 `0.1%` |
| HDMI录像转换 | `30.3-33.0ms/帧` |
| HDMI录像编码封装 | 约 `14.3ms/帧` |
| HDMI录像总处理 | `44.7-47.3ms/帧` |
| HDMI录像结果 | `996帧/45.968s = 21.67fps` |
| HDMI录像队列峰值 | `6/8` |
| HDMI推流转换 | 约 `35-38ms/帧` |
| HDMI推流packet age | 约 `46-53ms` |

这说明采集阶段的整帧CPU复制确实被消除了，但V4L2 MMAP BGR3内存随后仍要经过FFmpeg `hwupload`。在当前驱动和内存属性下，RKRGA读取该MMAP CPU映射比读取FFmpeg普通内存明显更慢，原来的复制成本被转移到转换阶段，并且录像和推流会同时长期持有驱动buffer，最终对采集形成反压。

因此：

- 原生MMAP CPU帧不作为默认生产路径。
- 默认恢复FFmpeg路径，以保持HDMI 4K录像约30fps。
- 真正有价值的下一步是验证 `VIDIOC_EXPBUF`，将V4L2 buffer导出为DMA-BUF，再以DRM PRIME BGR3直接交给RKRGA，跳过CPU `hwupload`。
- `rk3588_v4l2_capture_bench` 已增加 `export` 模式用于验证驱动DMA-BUF导出能力。

## 5.7 V4L2 DMA-BUF导出验证

板端执行：

```text
device=/dev/video40 format=BGR3 size=3840x2160 planes=1 buffers=6 mode=export
dmabuf_export=6/6 status=supported
duration=5.032s frames=145 fps=28.815 cpu=0.001s cpu_per_frame=0.008ms
```

`VIDIOC_EXPBUF` 已成功导出全部6个驱动buffer，因此具备继续实现真正硬件零复制的前提。5秒测试中的 `28.815fps` 包含STREAMON后的起帧时间，不能单独作为稳定吞吐结论。

当前实验实现调整为：

```text
V4L2 BGR3 capture buffer
  -> VIDIOC_EXPBUF DMA-BUF fd
  -> AVDRMFrameDescriptor / DRM PRIME, sw_format=BGR24
  -> scale_rkrga直接读取DMA-BUF
  -> 4K NV12 DRM用于录像，或1080P NV12用于推流
```

原生输入不再读取或复制BGR3数据，也不再经过 `hwupload`。RKRGA过滤器已扩展支持BGR24 DRM硬件输入。该路径仍由 `RK_HDMI_NATIVE_V4L2=1` 显式启用；默认FFmpeg路径保持不变。

## 5.8 DMA-BUF/RKRGA板端回归结果

独立20秒硬件编码测试：

```text
captured=600 frames=600 packets=601 avg_fps=30.00 dropped=0 max_queue=1
decode_read_avg=33.277ms encode_avg=11.929ms
```

日志确认路径为：

```text
V4L2 DMA-BUF BGR3 DRM
  -> FFmpeg-RKRGA-hardware-frame
  -> 4K NV12
  -> h264_rkmpp
```

三路录像并将推流切换到HDMI后的生产回归：

| 项目 | DMA-BUF结果 | 原FFmpeg路径参考 | 变化 |
|---|---:|---:|---:|
| HDMI采集CPU/帧 | `0.014-0.066ms` | `20-22ms` | 降低超过99% |
| HDMI 1080P推流转换 | `10.1-11.0ms` | `15.7-17.8ms` | 降低约30-40% |
| HDMI推流packet age | `14.4-16.7ms` | `20.6-22.5ms` | 降低约25-35% |
| HDMI 4K录像转换 | `6.5-7.1ms` | `13.1-13.4ms` | 降低约47-51% |
| HDMI 4K录像总处理 | `20.8-21.8ms` | `27.5-27.8ms` | 降低约21-25% |
| HDMI录像结果 | `528/17.578s = 30.04fps` | 约 `30fps` | 保持满帧率并增加余量 |
| HDMI录像队列 | `peak=1/8, dropped=0` | `peak=1/8, dropped=0` | 稳定 |

USB1为 `30.00fps`，USB2为 `30.02fps`，均 `dropped=0`、`queue_peak=1`。这证明DMA-BUF路径在三路采集、三路录像和HDMI推流并发情况下满足实时预算。

当前剩余验收项：

- 确认HDMI画面和录像没有红蓝通道交换。
- 连续运行至少10分钟，确认DMA-BUF fd、V4L2 buffer和RGA资源没有泄漏。
- 使用 `ffprobe` 和完整解码验证MP4帧率、时长及文件完整性。
- 上述项目通过后，可考虑将DMA-BUF路径改为默认启用，环境变量用于强制回退FFmpeg。

## 5.9 MP4时间戳回归与修正

`ffprobe`确认三路文件编码、分辨率、帧数和时长基本正确，HDMI为 `3840x2160`、528帧、约17.59秒。完整解码时仅USB1报告部分重复DTS：

```text
Application provided invalid, non monotonically increasing dts
```

原因不是DMA-BUF，而是录像线程原先使用“该帧开始编码的墙钟时间”生成PTS。线程调度和并发负载会让相邻帧间隔发生抖动，经过MP4或解码输出时间基准量化后可能落在同一个时间点。USB1后半段首先暴露该问题。

录像PTS已经改为严格CFR帧序号：

```text
PTS = frame_index * encoder_time_base / configured_fps
```

在30fps下对应 `0, 33333, 66667...` 微秒。这样可以保证无B帧编码器输入PTS严格递增，并使MP4的 `r_frame_rate` 和 `avg_frame_rate` 稳定接近配置帧率。录像开始/结束时间和命令行统计仍使用真实墙钟时间，不受影响。

## 6. 推荐实施顺序

| 顺序 | 项目 | 类型 | 当前建议 |
|---:|---|---|---|
| 1 | 原生V4L2 nocopy/copy基准 | 验证 | 已完成，证明收益显著 |
| 2 | 录像PTS改用采集时间 | 正确性 | 建议实施 |
| 3 | HDMI热插拔自动恢复 | 可靠性 | 建议实施 |
| 4 | 队列字节上限 | 内存保护 | 可实施，不作为降时项目 |
| 5 | 原生V4L2替换FFmpeg | 性能 | DMA-BUF生产性能回归已通过；待颜色和10分钟稳定性验收后默认启用 |
| 6 | 共享4K NV12硬件中间帧 | 性能/架构 | 在采集优化后评估 |
| 7 | 推流DRM直通 + RGA OSD | 性能/功能 | 作为同一项目实施 |
| 8 | 删除RGA全局锁 | 风险项 | 当前不实施 |

## 7. 总结

原指南中最有意义的是：

- 验证原生V4L2采集是否能减少整帧复制
- 使用采集时间戳
- HDMI热插拔恢复
- 录像队列内存上限

其中原生V4L2采集可以直接降低当前资源消耗。板端A/B测试已经确认FFmpeg demux约有一次完整4K BGR3复制的CPU成本，因此实施条件已经成立。

当前不应直接实施的是：

- 删除RGA全局锁
- 单独把简单十字OSD改成四次RGA `imfill`
- 照搬“原生V4L2后仍逐帧memcpy”的实现方式

当前已经验证并产生明确收益的优化是：

- USB DRM硬件帧保留
- HDMI 4K录像DRM PRIME直通编码
- HDMI原生V4L2 MMAP采集可降低采集CPU，但生产回归暴露 `hwupload` 性能退化，当前仅保留为实验路径
