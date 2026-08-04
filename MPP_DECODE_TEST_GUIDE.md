# RK3588 MPP 解码测试说明

---

## ⚠️ 2026-07-31 重要修正(实测推翻旧结论,先看这里)

经 2026-07-31 重新逐层实测,本文档原来的失败结论 **参数使用错误导致**,更正如下:

1. **mpi_dec_test 必须带 `-t` 编码类型参数**。原文所有调用都漏了:
   - 系统版 `/usr/bin/mpi_dec_test` 不带 `-t` 时 **静默失败**(返回 0、无报错、输出 0 字节)→ 原文 7.2 的"输出为空"
   - 源码版不带 `-t` 时 **大声报错**:`invalid input coding type` / `unable to create dec unused`(dec unused = coding 0 = 未指定类型)→ 原文 7.3 的"初始化失败"
   - 两个版本的"失败"是同一个原因:缺 `-t`,不是解码器坏
2. 补上 `-t` 后实测(2026-07-31):
   - 系统版: `mpi_dec_test -t 8 -i stream.mjpeg -o out.yuv -w 1280 -h 720 -n 20` → 解出 20 帧 NV12,字节数精确,转 PNG 像素正确
   - 源码版 `/home/cat/mpp-develop/.../mpi_dec_test -t 8 ...` → `decode 10 frames ... fps 130.34, test success`
   - **原文 7.2、7.3、第 8 节情况 A/B、第 10 节厂家模板、第 13 节结论均作废**
3. `-t` 取值(实测确认):
   - **解码**: `-t 7` = H.264,`-t 8` = MJPEG
   - **编码**: `-t 7` = H.264,`-t 8` = H.265(enc 侧 `-t 8 -f 16777220` 实测产出真实 HEVC,ffprobe 验证;注意 enc/dec 的 -t=8 含义不同)
4. **单帧/极少帧 JPEG 测试不可靠**: 即使参数正确,1~2 帧输入也输出 0 字节(工具不排空解码器)。**测试用 ≥10 帧的流**,别用单张 JPEG 判成败
5. 最终真实结论(详见 HDMI_TO_CSI_ANALYSIS.md 2.1 节):
   - VPU 硬件、librockchip_mpp、mpi_dec_test(系统版+源码版)、GStreamer mppjpegdec 的 MJPEG 硬解 **全部正常**
   - 唯一坏的是 **ffmpeg 的 `mjpeg_rkmpp` 解码封装**(任何输入死锁),MJPEG 硬解请走 GStreamer

---

这份文档用于排查 `RK3588 + MPP` 的硬件解码是否正常，重点是：

- `mpi_dec_test` 是否能正常初始化
- `JPEG / MJPEG` 硬件解码是否可用
- `H.264 / H.265` 硬件解码是否可用
- 当前问题是在 `码流本身`、`MPP 驱动/库`、还是 `测试程序/环境`

本文档按 `准备 -> 执行 -> 看结果 -> 看报错 -> 给厂家反馈` 的顺序写，后面你可以直接改给厂家。

---

## 1. 先确认当前环境

在板子上先看系统里有哪些 MPP 测试程序：

```bash
find /usr /usr/local /home/cat -name "mpi_dec_test" 2>/dev/null
find /usr /usr/local /home/cat -name "mpi_enc_test" 2>/dev/null
```

我们之前确认过有两套：

- 系统自带：
  - `/usr/bin/mpi_dec_test`
  - `/usr/bin/mpi_enc_test`
- 你自己源码编译的：
  - `/home/cat/mpp-develop/build/linux/aarch64/test/mpi_dec_test`
  - `/home/cat/mpp-develop/build/linux/aarch64/test/mpi_enc_test`

这两套都要测。

---

## 2. 为什么要分开测

MPP 解码排查不能直接只拿 USB 摄像头测，因为 USB 摄像头这条链路里同时包含：

- V4L2 采集
- MJPEG 码流输出
- MPP 解码
- 颜色格式转换
- GUI 显示

一旦黑屏，没法立刻判断到底是哪一层有问题。

所以建议拆成两步：

1. 先测 `离线文件解码`
2. 再测 `实时设备输入`

这样更容易把问题定位给厂家。

---

## 3. 准备测试文件

建议准备 4 类文件：

- 单张 JPEG
- MJPEG 文件
- H.264 文件
- H.265 文件

### 3.1 生成 JPEG 测试文件

如果系统里有 ffmpeg，可以直接生成一张标准 JPEG：

```bash
ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=1280x720:rate=1 -frames:v 1 /tmp/std_one.jpg
```

作用：

- 这个文件格式标准
- 不依赖 USB 摄像头
- 可以先排除摄像头 MJPEG 是否“非标”

---

### 3.2 生成 MJPEG 测试文件

```bash
ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=1280x720:rate=30 -t 3 -c:v mjpeg /tmp/std_test.mjpeg
```

作用：

- 用标准 ffmpeg 生成一段 MJPEG
- 如果这个也解不出来，就更像是 `JPEG/MJPEG 硬解链路有问题`

---

### 3.3 从 USB 摄像头抓一段真实 MJPEG

你的 USB 摄像头 `/dev/video41` 已确认支持：

- `MJPG 1280x720@30`
- `MJPG 1920x1080@30`

可以抓一段真实数据：

```bash
ffmpeg -hide_banner -y -f v4l2 -input_format mjpeg -video_size 1280x720 -framerate 30 -i /dev/video41 -t 3 -c copy /tmp/usb_mjpeg.mjpg
```

再抓一帧单图：

```bash
ffmpeg -hide_banner -y -f v4l2 -input_format mjpeg -video_size 1280x720 -framerate 30 -i /dev/video41 -frames:v 1 /tmp/usb_one.jpg
```

作用：

- 用真实摄像头数据验证
- 可以对比 “标准 JPEG/MJPEG” 和 “真实 USB MJPEG” 的差异

---

### 3.4 生成 H.264 / H.265 测试文件

先准备一段标准 YUV420P 原始数据：

```bash
ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=1280x720:rate=30 -pix_fmt yuv420p -frames:v 30 /tmp/mpp_enc_in_720p.yuv
```

再用系统的 MPP 编码器生成 H.264 / H.265：

```bash
/usr/bin/mpi_enc_test -w 1280 -h 720 -t 7 -f 8 -i /tmp/mpp_enc_in_720p.yuv -o /tmp/mpp_test_h264.h264
```

```bash
/usr/bin/mpi_enc_test -w 1280 -h 720 -t 8 -f 16777220 -i /tmp/mpp_enc_in_720p.yuv -o /tmp/mpp_test_h265.h265
```

说明：

- `-t 7` 通常对应 H.264
- `-t 8` 通常对应 H.265/HEVC
- `-f` 是输入像素格式，使用 YUV420P

如果你担心参数和版本有差异，可以先执行：

```bash
/usr/bin/mpi_enc_test
```

或者：

```bash
/usr/bin/mpi_enc_test -h
```

查看本机参数说明。

---

## 4. 如何测试系统自带 MPP 解码

### 4.1 测 JPEG

```bash
rm -f /tmp/out_std_one.yuv
# 注意: 必须带 -t 8 (MJPEG); 单帧测试不可靠, 见顶部修正说明第 4 条
/usr/bin/mpi_dec_test -t 8 -i /tmp/std_one.jpg -o /tmp/out_std_one.yuv -w 1280 -h 720
ls -lh /tmp/std_one.jpg /tmp/out_std_one.yuv
```

正常预期：

- 程序能正常退出
- `/tmp/out_std_one.yuv` 存在
- 文件大小明显大于 0

异常现象：

- 输出文件不存在
- 输出文件大小为 0
- 程序无明显报错但没有有效结果

---

### 4.2 测标准 MJPEG

```bash
rm -f /tmp/out_std_test.yuv
/usr/bin/mpi_dec_test -t 8 -i /tmp/std_test.mjpeg -o /tmp/out_std_test.yuv -w 1280 -h 720 -n 20
ls -lh /tmp/std_test.mjpeg /tmp/out_std_test.yuv
```

正常预期：

- 有正常输出 YUV 文件
- 文件大小大于 0

如果失败，说明：

- 不是 USB 摄像头特有问题
- 更可能是 `JPEG/MJPEG 硬解路径本身有问题`

---

### 4.3 测真实 USB MJPEG

```bash
rm -f /tmp/out_usb_mjpeg.yuv
/usr/bin/mpi_dec_test -t 8 -i /tmp/usb_mjpeg.mjpg -o /tmp/out_usb_mjpeg.yuv -w 1280 -h 720 -n 20
ls -lh /tmp/usb_mjpeg.mjpg /tmp/out_usb_mjpeg.yuv
```

如果标准 MJPEG 不行、USB MJPEG 也不行，那就更接近 `系统 MPP JPEG/MJPEG 解码不可用`。

如果标准 MJPEG 可以、USB MJPEG 不行，那就可能是：

- 摄像头输出码流不完全标准
- MPP 的 MJPEG 容错性不足

---

## 5. 如何测试源码编译版 MPP 解码

源码版测试程序：

```bash
/home/cat/mpp-develop/build/linux/aarch64/test/mpi_dec_test
```

### 5.1 测 H.264

```bash
rm -f /tmp/src_dec_h264.yuv
/home/cat/mpp-develop/build/linux/aarch64/test/mpi_dec_test -t 7 -i /tmp/mpp_test_h264.h264 -o /tmp/src_dec_h264.yuv -w 1280 -h 720
ls -lh /tmp/mpp_test_h264.h264 /tmp/src_dec_h264.yuv
```

### 5.2 测 JPEG

```bash
rm -f /tmp/src_dec_jpeg.yuv
# 注意: -t 8 = MJPEG; 建议改用多帧流 /tmp/std_test.mjpeg 判断, 单帧不可靠
/home/cat/mpp-develop/build/linux/aarch64/test/mpi_dec_test -t 8 -i /tmp/std_test.mjpeg -o /tmp/src_dec_jpeg.yuv -w 1280 -h 720 -n 20
ls -lh /tmp/std_one.jpg /tmp/src_dec_jpeg.yuv
```

---

## 6. 怎么看“是否真的解出来了”

不能只看命令有没有退出，还要看输出文件。

### 6.1 看文件大小

```bash
ls -lh /tmp/out_*.yuv /tmp/src_dec_*.yuv 2>/dev/null
```

如果是 1280x720 的 YUV420：

- 单帧大约是 `1280 * 720 * 1.5 = 1382400` 字节

如果输出文件远小于这个值，或者是 0，基本就是失败。

---

### 6.2 用 ffplay / ffmpeg 再验证

比如单张 1280x720 的 YUV420P：

```bash
ffplay -f rawvideo -pixel_format yuv420p -video_size 1280x720 /tmp/out_std_one.yuv
```

或者转成 PNG 再看：

```bash
ffmpeg -hide_banner -y -f rawvideo -pixel_format yuv420p -video_size 1280x720 -i /tmp/out_std_one.yuv -frames:v 1 /tmp/out_std_one.png
```

如果图片能正常看到，说明解码是成功的。

---

## 7. 我们当前已经测到的结果

截至 `2026-07-30`，当前板子上已经验证到的现象如下。

### 7.1 编码正常

系统自带 MPP 编码器可用：

- `/usr/bin/mpi_enc_test` 能正常编码 H.264
- `/usr/bin/mpi_enc_test` 能正常编码 H.265

FFmpeg 的 RKMPP 硬编码也正常：

- `h264_rkmpp` 正常
- `hevc_rkmpp` 正常

这说明：

- MPP 不是“整体完全坏掉”
- `编码侧` 基本是好的

---

### 7.2 系统自带 `mpi_dec_test` 对 JPEG/MJPEG 测试结果异常

之前实测现象：

- 程序可以运行
- 但 JPEG / MJPEG 解码没有得到有效输出
- 输出文件为空，或者没有可用 YUV 结果

这说明：

- `系统自带解码测试程序` 至少在 `JPEG/MJPEG` 上不可用或结果异常

---

### 7.3 源码编译版 `mpi_dec_test` 初始化失败

源码路径：

```bash
/home/cat/mpp-develop/build/linux/aarch64/test/mpi_dec_test
```

之前看到的典型报错包括：

```text
mpp_platform: client 12 driver is not ready!
invalid input coding type
mpp_init failed
unable to create dec unused for soc rk3588 unsupported
```

而且这个问题不是只在 MJPEG 上出现，连 H.264 测试也出现过。

这说明：

- 你源码编译出来的 `mpi_dec_test` 不是单纯 “MJPEG 不支持”
- 更像是 `源码版 MPP 解码运行环境 / 驱动对接 / 库版本` 有问题

---

## 8. 怎么判断问题大概挂在哪一层

可以用下面这个判断方式：

### 情况 A

- `mpi_enc_test` 正常
- `mpi_dec_test` 的 JPEG/MJPEG 不正常
- ffmpeg `mjpeg_rkmpp` 也不正常

更像是：

- `JPEG/MJPEG 硬件解码链路有问题`
- 不是单纯 ffmpeg 命令问题

### 情况 B

- 系统自带 `mpi_dec_test` 还能跑
- 源码编译版 `mpi_dec_test` 初始化失败

更像是：

- 源码编译环境和系统驱动/库不匹配
- 运行时链接库、内核驱动、mpp 版本组合有问题

### 情况 C

- 标准 JPEG/MJPEG 能解
- USB 摄像头抓出来的 MJPEG 不能解

更像是：

- 摄像头输出码流不够标准
- MPP 的 MJPEG 容错性不足

---

## 9. 建议给厂家时附上的关键信息

建议把下面这些一起给厂家：

### 9.1 系统信息

```bash
uname -a
cat /etc/os-release
```

### 9.2 MPP 测试程序位置

```bash
find /usr /usr/local /home/cat -name "mpi_dec_test" 2>/dev/null
find /usr /usr/local /home/cat -name "mpi_enc_test" 2>/dev/null
```

### 9.3 源码版 MPP 的编译配置

在源码目录里：

```bash
grep -E "ENABLE_|BUILD_" /home/cat/mpp-develop/build/linux/aarch64/CMakeCache.txt
```

重点看这些是否开启：

- `ENABLE_H264D`
- `ENABLE_H265D`
- `ENABLE_JPEGD`
- `ENABLE_H264E`
- `ENABLE_H265E`
- `ENABLE_JPEGE`

### 9.4 关键测试文件

建议把这些文件信息一起给厂家：

- `/tmp/std_one.jpg`
- `/tmp/std_test.mjpeg`
- `/tmp/usb_one.jpg`
- `/tmp/usb_mjpeg.mjpg`
- `/tmp/mpp_test_h264.h264`

先看大小和类型：

```bash
ls -lh /tmp/std_one.jpg /tmp/std_test.mjpeg /tmp/usb_one.jpg /tmp/usb_mjpeg.mjpg /tmp/mpp_test_h264.h264
file /tmp/std_one.jpg /tmp/std_test.mjpeg /tmp/usb_one.jpg /tmp/usb_mjpeg.mjpg /tmp/mpp_test_h264.h264
```

### 9.5 关键报错日志

建议保留完整终端输出，至少包括：

- 系统自带 `mpi_dec_test`
- 源码版 `mpi_dec_test`
- ffmpeg `-c:v mjpeg_rkmpp`

---

## 10. 可以直接发给厂家的问题描述模板

下面这段你可以直接改一改发给厂家：

```text
平台：RK3588

现象：
1. MPP 编码正常，/usr/bin/mpi_enc_test 可以正常生成 H.264 / H.265。
2. FFmpeg 的 h264_rkmpp / hevc_rkmpp 编码也正常。
3. 但 JPEG / MJPEG 硬件解码异常：
   - 系统自带 /usr/bin/mpi_dec_test 对 JPEG / MJPEG 测试时没有得到有效 YUV 输出。
   - 源码编译版 /home/cat/mpp-develop/build/linux/aarch64/test/mpi_dec_test 在 RK3588 上初始化失败，报错包含：
     - mpp_platform: client 12 driver is not ready!
     - invalid input coding type
     - mpp_init failed
     - unable to create dec unused for soc rk3588 unsupported
4. 使用 ffmpeg 的 mjpeg_rkmpp 解码 USB MJPEG 也无法正常得到有效显示结果。

已验证：
1. 问题不只出现在 USB 摄像头真实 MJPEG 上。
2. 用 ffmpeg 生成的标准 JPEG / 标准 MJPEG 文件测试，也存在相同异常。
3. 因此怀疑是当前版本 MPP/驱动环境下 JPEG/MJPEG 硬件解码链路存在问题，或源码版 MPP decoder 与系统驱动/库不匹配。

希望协助确认：
1. RK3588 当前 BSP/内核/MPP 版本是否正式支持 JPEG / MJPEG 硬件解码。
2. 对应应使用哪一套官方 MPP、内核驱动和测试程序版本。
3. mpi_dec_test 在 RK3588 上的正确测试方法、参数和依赖环境。
4. 如果 JPEG/MJPEG 硬解有限制，请明确支持的输入格式、分辨率、码流封装方式。
```

---

## 11. 当前更现实的工程建议

在厂家没确认前，项目上建议先这样走：

- USB 摄像头：
  - `MJPEG -> 软件解码 -> YUV420/NV12 -> 硬件编码`
- CSI0 / CSI1 / HDMI IN：
  - `原始帧 -> 必要时做颜色转换 -> 硬件编码`

这样能先把主流程打通，不会卡死在 USB MJPEG 硬解上。

---

## 12. 一套最小测试命令清单

如果你后面只想快速复现，直接按这个顺序跑：

```bash
ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=1280x720:rate=1 -frames:v 1 /tmp/std_one.jpg
ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=1280x720:rate=30 -t 3 -c:v mjpeg /tmp/std_test.mjpeg
ffmpeg -hide_banner -y -f v4l2 -input_format mjpeg -video_size 1280x720 -framerate 30 -i /dev/video41 -t 3 -c copy /tmp/usb_mjpeg.mjpg
ffmpeg -hide_banner -y -f v4l2 -input_format mjpeg -video_size 1280x720 -framerate 30 -i /dev/video41 -frames:v 1 /tmp/usb_one.jpg
/usr/bin/mpi_dec_test -t 8 -i /tmp/std_test.mjpeg -o /tmp/out_std_test.yuv -w 1280 -h 720 -n 20
/usr/bin/mpi_dec_test -t 8 -i /tmp/usb_mjpeg.mjpg -o /tmp/out_usb_mjpeg.yuv -w 1280 -h 720 -n 20
ls -lh /tmp/std_one.jpg /tmp/std_test.mjpeg /tmp/usb_mjpeg.mjpg /tmp/out_std_one.yuv /tmp/out_std_test.yuv /tmp/out_usb_mjpeg.yuv
```

如果还要测源码版：

```bash
/home/cat/mpp-develop/build/linux/aarch64/test/mpi_dec_test -t 8 -i /tmp/std_test.mjpeg -o /tmp/src_dec_jpeg.yuv -w 1280 -h 720 -n 20
/home/cat/mpp-develop/build/linux/aarch64/test/mpi_dec_test -t 7 -i /tmp/mpp_test_h264.h264 -o /tmp/src_dec_h264.yuv -w 1280 -h 720
```

---

## 13. 一句话结论

当前更像是：

- `MPP 编码正常`
- `JPEG/MJPEG 硬件解码链路异常`
- `源码编译版 mpi_dec_test 在当前 RK3588 环境下还有初始化/驱动匹配问题`

所以现在最适合拿给厂家看的，不是“应用层代码黑屏”，而是这份 `最小复现测试`。
