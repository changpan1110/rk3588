# RK3588 鲁班猫5 (lubancat-5io) MJPEG 测试结论 + HDMI 转 MIPI-CSI 支持分析

调查日期: 2026-07-31,板子 192.168.31.14,内核 6.1.118,设备树 rk3588-lubancat-5io

---

## 一、MJPEG 测试结论(全部实测通过)

### 1. 各视频节点 MJPEG 支持情况

| 节点 | 设备 | MJPG 支持 | 实测结果 |
|---|---|---|---|
| /dev/video41 | HD Camera (USB) | 720p30 / 1080p30 / 480p30 | 采集 30 帧成功,FFD8 JPEG 头验证通过 |
| /dev/video43 | USB Video | 1080p30 / 720p60 / 480p60 等 | 格式枚举正常 |
| /dev/video40 | rk_hdmirx 原生 HDMI 输入 | 不支持 MJPG | 正常,只输出 NV12/NV16/NV24/BGR3 原始图像 |
| mipi 摄像头 (IMX415/OV8858) | rkcif/rkisp | 不支持 MJPG | 正常,MIPI 传感器输出 RAW Bayer (SGBRG10) |

结论:**MJPEG 是 USB 摄像头 (UVC) 的特性**,HDMI RX 和 MIPI-CSI 通道都只出原始格式。
如果最终目标是 MJPEG 码流,正确做法是:采集原始帧 -> `mjpeg_rkmpp` 硬件编码(板子已确认带此编码器)。

### 2. 硬解/硬编实测(2026-07-31 最终修正结论)

**MJPEG 硬件解码:ffmpeg 的 `mjpeg_rkmpp` 是坏的(任何输入都死锁,0 帧,只能 kill -9);
正确路径是 GStreamer 的 `mppjpegdec`(同一块 VPU 硬件,实测正常)。**

完整测试矩阵(全部真实退出码 + 帧数验证):

| 路径 | 结果 |
|---|---|
| ffmpeg `mjpeg_rkmpp` 解码(420/422/444、文件/实时) | 全部死锁,本固件 ffmpeg 构建 bug |
| GStreamer `mppjpegdec` 解码(yuvj420p / 真实摄像头 422) | 硬件解码正常,60 帧约 35~55ms |
| ffmpeg `mjpeg_rkmpp` **编码** | 正常(30 帧 0.1s) |
| ffmpeg `h264_rkmpp` / `hevc_rkmpp` 解码 | 正常(90 帧验证,77x / 43x 实时) |
| `h264_rkmpp` / `hevc_rkmpp` / `mjpeg_rkmpp` 编码 | 全部正常 |
| ffmpeg 软件 `mjpeg` 解码 | 正常,720p 约 8.5x 实时,可作兜底 |

可用的 MJPEG 硬解命令(gstreamer 路径):

### 2.1 故障定位:为什么 ffmpeg 不行、GStreamer 行(分层验证,2026-07-31)

调用链四层,逐层验证:

```
ffmpeg mjpeg_rkmpp  ─┐
                     ├─> librockchip_mpp.so.1 ─> /dev/mpp_service ─> 内核 ─> VPU 硬件
gst mppjpegdec      ─┘        (两者 ldd 确认链接的是同一个库文件)
```

| 层 | 验证方法 | 结果 |
|---|---|---|
| VPU 硬件 + 内核驱动 | mpp 源码构建的原生工具 `mpi_dec_test -t 8 -i stream.mjpeg -o out.yuv -n 20` | 解出 20 帧 NV12(27,648,000 字节 = 20 x 1280x720x1.5),转 PNG 像素正确,见工作目录 mpp_decoded_frame.png |
| librockchip_mpp 用户库 | 同上,且 gst/ffmpeg 链接同一 .so | 正常 |
| GStreamer 胶水层 (mppjpegdec) | gst-launch 管道实测 | 正常 |
| **ffmpeg 胶水层 (mjpeg_rkmpp)** | 干净系统+标准文件,任何格式 | **死锁:init 成功后 0 帧输出,线程停在 futex_wait_queue** |

结论:bug 在这个定制 ffmpeg 构建的 MJPEG-rkmpp 封装层里(解码线程等一个永远不会
到达的帧通知,与硬件无关)。同库同硬件,h264/hevc 的 ffmpeg 封装是好的,只有
mjpeg 这一路坏。要修只能重编 ffmpeg(Rockchip ffmpeg 补丁的 mjpeg 解码路径),
否则 MJPEG 硬解一律走 gstreamer。

```bash
# 摄像头实时 MJPEG 硬解显示帧率
gst-launch-1.0 v4l2src device=/dev/video41 ! image/jpeg,width=1280,height=720,framerate=30/1 ! \
  jpegparse ! mppjpegdec ! fpsdisplaysink video-sink=fakesink text-overlay=false

# 文件 MJPEG 硬解
gst-launch-1.0 filesrc location=/tmp/cap.avi ! avidemux ! jpegparse ! mppjpegdec ! fakesink sync=false

# MJPEG 硬解 -> 重新硬编 H264(转码)
gst-launch-1.0 v4l2src device=/dev/video41 ! image/jpeg,width=1280,height=720 ! \
  jpegparse ! mppjpegdec ! mpph264enc ! h264parse ! filesink location=/tmp/out.h264
```

### 3. 重要坑(你之前卡死的真正原因)

7-30/7-31 板上多次出现 `mjpeg_rkmpp` 死锁(futex_wait,SIGTERM 杀不掉,只能 kill -9)。
**真正根因:这个固件里 ffmpeg 的 `mjpeg_rkmpp` 解码器本身就是坏的** —— 干净系统、
标准测试文件、任何色度格式下都稳定复现死锁,与文件内容无关,与进程占用无关。
你 7-30 的进程就是撞上了这个 bug 才卡住的。

教训:
- **MJPEG 硬解不要走 ffmpeg,走 GStreamer `mppjpegdec`**(见第 2 节命令)
- ffmpeg 里硬解 h264/hevc、硬编 mjpeg/h264/hevc 都正常,只有 mjpeg 硬解这一个点坏
- mpp 进程不要 Ctrl-Z,用 Ctrl-C 结束;卡住就 `kill -9`
- 采集设备被占时释放通道: `sudo fuser -k /dev/video41`
- VPU 被占时释放: `sudo fuser -k /dev/mpp_service`

可用命令速查:

```bash
# 查看设备支持的具体传输格式(yuv420 还是 mjpg)
v4l2-ctl -d /dev/video41 --list-formats-ext
v4l2-ctl -d /dev/video41 -V -D

# 释放被占用的通道
sudo fuser -k /dev/video41

# USB 摄像头 MJPEG -> 硬解 -> 丢弃(验证链路, 用 gstreamer 不用 ffmpeg)
gst-launch-1.0 v4l2src device=/dev/video41 num-buffers=100 ! \
  image/jpeg,width=1280,height=720,framerate=30/1 ! jpegparse ! mppjpegdec ! fakesink sync=false

# USB 摄像头 MJPEG 直接存文件(不解码)
ffmpeg -nostdin -f v4l2 -input_format mjpeg -video_size 1280x720 \
  -framerate 30 -i /dev/video41 -frames:v 100 -c copy -y /tmp/cap.avi
```

---

## 二、HDMI 输入:先看原生 HDMI RX(零驱动工作)

板子的 RK3588 **原生 HDMI RX 已经驱动好并且能用**,节点 `/dev/video40`:

- 驱动 `rk_hdmirx` 已编译进内核 (CONFIG_VIDEO_ROCKCHIP_HDMIRX=y)
- 设备树 `hdmirx-controller@fdee0000` 已使能,带插拔检测引脚
- 今天 dmesg 里有真实记录: `signal lock ok` + `enable audio`(当时插着 HDMI 源)
- 当前显示 "Link has been severed" = 现在没插线,插上源即可

采集方式:

```bash
# 插上 HDMI 源后查询检测到的时序(分辨率/帧率)
v4l2-ctl -d /dev/video40 --query-dv-timings
v4l2-ctl -d /dev/video40 --set-dv-timings

# 采集(多平面格式)
v4l2-ctl -d /dev/video40 --set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
  --stream-mmap=4 --stream-count=100 --stream-to=/tmp/hdmi_nv12.yuv

# 或者直接 ffmpeg -> MJPEG 硬编码输出
ffmpeg -nostdin -f v4l2 -input_format nv12 -video_size 1920x1080 \
  -i /dev/video40 -c:v mjpeg_rkmpp -q:v 3 -y /tmp/hdmi_mjpeg.avi
```

**如果需求只是"采集一路 HDMI",用这个原生口就行,不需要买任何转接板。**

---

## 三、HDMI 转 MIPI-CSI 桥接(需要多路 HDMI / 原生口不够时)

### 1. 支持的桥接芯片与当前驱动状态

内核 6.1 源码里(Rockchip SDK / mainline)存在的驱动,当前配置全部未启用:

| 芯片 | 规格 | 驱动来源 | 当前 config | 推荐度 |
|---|---|---|---|---|
| TC358743 (东芝) | HDMI 1.4 -> CSI-2,最高 1080p60 | mainline `tc358743.c` + Rockchip `tc35874x.c` | `CONFIG_VIDEO_TC358743/TC35874X = not set` | 首选:模块便宜(几十~一百多元),树莓派生态成熟 |
| LT6911UXC/UXE (龙迅) | HDMI 2.0 -> CSI-2,最高 4K60 | 仅 Rockchip SDK `lt6911uxc.c` 等 | `CONFIG_VIDEO_LT6911UXC/UXE = not set` | 需要 4K 时选,驱动要拿 SDK 源码 |
| LT6911C (龙迅) | HDMI 1.4 -> CSI-2,1080p60 | 仅 Rockchip SDK | `not set` | 一般 |
| ADV7604/7611 (ADI) | 工业级 | mainline `adv7604.c` | `not set` | 贵,特殊场景 |

购买建议: 搜 "TC358743 HDMI转MIPI 模块"(树莓派 C779/C790 类板子),注意确认是
**CSI-2 输出、几 lane**(2 lane 版即可跑 1080p60 YUV422,约 1Gbps/lane,RK3588 dphy 没问题)。

### 2. 驱动怎么补:不需要重编整个内核

板上条件已核实齐全:
- 内核头文件完整: `/usr/src/linux-headers-6.1.118`(含 `Module.symvers`,CONFIG_MODVERSIONS=y 可满足)
- 板载 gcc / make / dtc 都有
- 板子可上外网

所以直接**在板子上编译外部模块(.ko)**,不动原内核。以 TC358743 为例:

```bash
mkdir -p ~/tc358743 && cd ~/tc358743

# 拿 6.1 主线驱动源码(单文件)
wget https://raw.githubusercontent.com/torvalds/linux/v6.1/drivers/media/i2c/tc358743.c

# 写 Makefile
printf 'obj-m += tc358743.o\n' > Makefile

# 编译(依赖板上的 linux-headers-6.1.118)
make -C /lib/modules/$(uname -r)/build M=$PWD modules

# 加载
sudo insmod tc358743.ko
# 想开机自动加载:
sudo cp tc358743.ko /lib/modules/$(uname -r)/kernel/drivers/media/i2c/
sudo depmod -a && echo tc358743 | sudo tee /etc/modules-load.d/tc358743.conf
```

备注:
- 主线 `tc358743.c` 与平台无关,一般可直接用;若采集异常,可换 Rockchip SDK 的
  `drivers/media/i2c/tc35874x.c`(从野火内核源码仓库取,同样按外部模块编译)
- LT6911UXC 同理,但源码只能从 Rockchip/野火 SDK 内核树拿 `drivers/media/i2c/lt6911uxc.c`

### 3. 设备树:也不用重编 dtb,用 overlay

板上机制已确认: U-Boot 读 `/boot/firmware/ubuntuEnv.txt` 里的 `overlays=` 列表,
从 `/boot/firmware/dtbs/rockchip/overlay/` 加载同名 `.dtbo`。

做法:

1. 反编译一个现成的摄像头 overlay 当模板(板上就有 dtc):
   ```bash
   dtc -I dtb -O dts -o ~/cam2-template.dts \
     /boot/firmware/dtbs/rockchip/overlay/rk3588-lubancat-5io-cam2-imx415-3840x2160-15fps-overlay.dtbo
   ```
2. 照着改成 tc358743 节点(关键属性):
   - 挂在该 CSI 口对应的 i2c 总线上,`compatible = "toshiba,tc358743"`,`reg = <0x0f>`
   - `clocks` 27MHz 参考时钟(模块自带晶振则配 fixed-clock)
   - `reset-gpios` 按实际接线
   - `port`/`endpoint` 连到对应 mipi dphy,`data-lanes` 按模块(2 lane 写 <1 2>)
   - i2c 上还可挂 0x50 的 EDID EEPROM(部分模块有)
3. 编译并安装 overlay:
   ```bash
   dtc -I dts -O dtb -@ -o rk3588-lubancat-5io-cam2-tc358743-overlay.dtbo tc358743-overlay.dts
   sudo cp rk3588-lubancat-5io-cam2-tc358743-overlay.dtbo /boot/firmware/dtbs/rockchip/overlay/
   ```
4. 编辑 `/boot/firmware/ubuntuEnv.txt`,在 `overlays=` 行尾加:
   `rk3588-lubancat-5io-cam2-tc358743-overlay`(不带 .dtbo)
5. 重启

注意: 建议占用空闲的 cam2~cam5 口(cam0=IMX415、cam1=OV8858 已在用);
具体哪个 CSI 口对应哪个 i2c/哪个 dphy,反编译对应 camN 的 imx415 overlay 一看便知。

### 4. 接好后的采集流程

```bash
# 找到桥接芯片实体和所属 media 节点
media-ctl -p -d /dev/media2 | grep -i tc358743

# 查询/设置 HDMI 源时序(桥接芯片是 DV timings 设备,这步必须)
v4l2-ctl -d /dev/v4l-subdevX --query-dv-timings
v4l2-ctl -d /dev/v4l-subdevX --set-dv-timings

# 设置 rkcif 采集节点格式并抓流
v4l2-ctl -d /dev/videoY --set-fmt-video=width=1920,height=1080,pixelformat=UYVY \
  --stream-mmap=4 --stream-count=100 --stream-to=/tmp/bridge.uyvy
```

---

## 四、给你的路线建议

1. **先插线试原生 HDMI IN(/dev/video40)**,5 分钟验证,大概率直接满足需求
2. 需要第二路 HDMI 输入,或原生口物理上接不了,再买 TC358743 模块(1080p 够用)
   或 LT6911UXC 模块(要 4K),按本文第三节编 .ko + 写 overlay
3. 不管哪条路,要 MJPEG 码流都是采集后走 `mjpeg_rkmpp` 硬编码,不要指望采集设备直接出 MJPG

## 附:本次使用的诊断脚本(工作目录内)

- `mjpeg_isolate.sh` — MJPEG 硬解 5 项隔离测试(回归用)
- `hdmi_csi_survey.sh` / `hdmi_csi_survey2.sh` / `hdmi_csi_survey3.sh` — 板载环境调查
- `hw_decode_test.sh` — VPU 占用检查 + mjpeg/h264/hevc 硬解一键测试(已同步到板子 ~/hw_decode_test.sh)

## 附:VPU(硬件编解码器)占用排查命令

```bash
# 查看谁占用硬件编解码器
sudo fuser -v /dev/mpp_service

# 查看挂起(T 状态)的媒体进程 —— 最危险的占用来源
ps -eo pid,stat,etime,cmd | grep -E "^ *[0-9]+ +T" | grep -E "ffmpeg|gst"

# 强制释放
sudo fuser -k /dev/mpp_service
kill -9 <PID>
```
