# RK3588 Rockchip FFmpeg 完整安装与验证指南

本文档用于在 RK3588 Ubuntu 开发板上编译并安装以下组件：

- Rockchip MPP：硬件视频编解码
- Rockchip RGA：硬件缩放、格式转换和图层合成
- FFmpeg Rockchip：调用 MPP 和 RGA，并支持文字、字幕和网络协议

本文档中的关键命令已经在以下环境实测：

```text
开发板：RK3588 LubanCat
系统：Ubuntu 24.04.4 LTS
架构：aarch64
GCC：13.3.0
CMake：3.28.3
Meson：1.3.2
Ninja：1.11.1
USB 摄像头：/dev/video43
USB 格式：MJPEG 1280x720@30
```

对应源码版本：

```text
FFmpeg Rockchip：2eb0b5d
MPP：4e5b4cf，版本 1.3.9
RGA：1d330cc，版本 2.1.0，RGA API 1.10.4
```

## 1. 安装目录说明

本文档统一使用下面的目录：

```text
/home/cat/ffmpeg/
├── ffmpeg/
│   ├── ffmpeg-rockchip/    FFmpeg 源码
│   ├── rkmpp/              MPP 源码
│   └── rkrga/              RGA 源码
├── build-local/
│   ├── mpp/                MPP 构建目录
│   ├── rga/                RGA 构建目录
│   └── ffmpeg-full/        FFmpeg 构建目录
└── install/                最终安装目录
    ├── bin/
    ├── include/
    └── lib/
```

安装到 `/home/cat/ffmpeg/install` 的优点：

- 不覆盖系统的 `/usr/bin/ffmpeg`
- 可以同时保留系统 FFmpeg 和 Rockchip FFmpeg
- MPP、RGA、FFmpeg 使用同一个安装前缀，路径容易管理
- 验证正常之后再决定是否替换系统版本

## 2. 安装编译依赖

执行：

```bash
sudo apt update

sudo apt install -y \
  build-essential pkg-config git \
  cmake meson ninja-build \
  libdrm-dev \
  libfreetype6-dev \
  libharfbuzz-dev \
  libfontconfig1-dev \
  libfribidi-dev \
  libass-dev \
  libssl-dev \
  libsrt-openssl-dev \
  librist-dev \
  libssh-dev \
  libv4l-dev \
  libasound2-dev \
  libpulse-dev \
  libopus-dev \
  libmp3lame-dev \
  libvorbis-dev \
  libsoxr-dev \
  libwebp-dev \
  libzimg-dev \
  zlib1g-dev \
  libbz2-dev \
  liblzma-dev
```

主要依赖作用：

| 依赖 | 功能 |
|---|---|
| `libdrm-dev` | DRM PRIME 硬件帧 |
| `libfreetype6-dev` | `drawtext` 字体绘制 |
| `libharfbuzz-dev` | `drawtext` 字形排版 |
| `libfontconfig1-dev` | 自动查找系统字体 |
| `libfribidi-dev` | 双向文字排版 |
| `libass-dev` | ASS/SRT 字幕渲染 |
| `libssl-dev` | HTTPS/TLS |
| `libsrt-openssl-dev` | SRT 网络传输 |
| `librist-dev` | RIST 网络传输 |
| `libv4l-dev` | V4L2 扩展支持 |
| `libopus-dev` | Opus 音频 |
| `libmp3lame-dev` | MP3 编码 |
| `libzimg-dev` | `zscale` 滤镜 |

## 3. 设置环境变量

当前终端执行：

```bash
export RK_FFMPEG_ROOT=/home/cat/ffmpeg
export RK_SOURCE_ROOT=/home/cat/ffmpeg/ffmpeg
export RK_BUILD_ROOT=/home/cat/ffmpeg/build-local
export RK_INSTALL_PREFIX=/home/cat/ffmpeg/install

mkdir -p "$RK_SOURCE_ROOT"
mkdir -p "$RK_BUILD_ROOT"
mkdir -p "$RK_INSTALL_PREFIX"
```

后面的所有命令都依赖这些变量。如果重新打开终端，需要重新执行本节命令。

## 4. 下载源码

如果下面三个源码目录已经存在，可以跳过本节。

```bash
cd "$RK_SOURCE_ROOT"

git clone https://github.com/nyanmisaka/ffmpeg-rockchip.git
git clone -b jellyfin-mpp https://github.com/nyanmisaka/mpp.git rkmpp
git clone -b jellyfin-rga https://github.com/nyanmisaka/rk-mirrors.git rkrga
```

为了与本文实测版本完全一致，可以切换到对应提交：

```bash
cd "$RK_SOURCE_ROOT/ffmpeg-rockchip"
git checkout 2eb0b5d

cd "$RK_SOURCE_ROOT/rkmpp"
git checkout 4e5b4cf

cd "$RK_SOURCE_ROOT/rkrga"
git checkout 1d330cc
```

检查版本：

```bash
git -C "$RK_SOURCE_ROOT/ffmpeg-rockchip" rev-parse --short HEAD
git -C "$RK_SOURCE_ROOT/rkmpp" rev-parse --short HEAD
git -C "$RK_SOURCE_ROOT/rkrga" rev-parse --short HEAD
```

## 5. 编译并安装 MPP

MPP 使用 CMake 编译：

```bash
cmake \
  -S "$RK_SOURCE_ROOT/rkmpp" \
  -B "$RK_BUILD_ROOT/mpp" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$RK_INSTALL_PREFIX" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DBUILD_SHARED_LIBS=ON \
  -DBUILD_TEST=OFF

cmake --build "$RK_BUILD_ROOT/mpp" -j"$(nproc)"
cmake --install "$RK_BUILD_ROOT/mpp"
```

安装后应该存在：

```bash
ls -l "$RK_INSTALL_PREFIX/lib/librockchip_mpp.so"
ls -l "$RK_INSTALL_PREFIX/lib/pkgconfig/rockchip_mpp.pc"
ls -l "$RK_INSTALL_PREFIX/include/rockchip/rk_mpi.h"
```

## 6. 编译并安装 RGA

当前 RGA 源码使用 Meson 和 Ninja 编译：

```bash
meson setup \
  "$RK_BUILD_ROOT/rga" \
  "$RK_SOURCE_ROOT/rkrga" \
  --prefix="$RK_INSTALL_PREFIX" \
  --libdir=lib \
  --buildtype=release \
  --default-library=shared \
  -Dlibdrm=true \
  -Dlibrga_demo=false

meson compile -C "$RK_BUILD_ROOT/rga" -j"$(nproc)"
meson install -C "$RK_BUILD_ROOT/rga"
```

安装后应该存在：

```bash
ls -l "$RK_INSTALL_PREFIX/lib/librga.so"
ls -l "$RK_INSTALL_PREFIX/lib/pkgconfig/librga.pc"
ls -l "$RK_INSTALL_PREFIX/include/rga/RgaApi.h"
```

如果 RGA 构建目录已经配置过，重新配置使用：

```bash
meson setup --reconfigure \
  "$RK_BUILD_ROOT/rga" \
  "$RK_SOURCE_ROOT/rkrga" \
  --prefix="$RK_INSTALL_PREFIX" \
  --libdir=lib \
  --buildtype=release \
  --default-library=shared \
  -Dlibdrm=true \
  -Dlibrga_demo=false
```

## 7. 配置 MPP、RGA 查找路径

FFmpeg 没有单独的 `--mpp-path` 参数。它通过 `pkg-config` 查找：

```text
rockchip_mpp.pc
librga.pc
libdrm.pc
```

执行：

```bash
export PKG_CONFIG_PATH="$RK_INSTALL_PREFIX/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/lib/pkgconfig"
export LD_LIBRARY_PATH="$RK_INSTALL_PREFIX/lib:${LD_LIBRARY_PATH:-}"
```

验证路径和版本：

```bash
pkg-config --variable=prefix rockchip_mpp
pkg-config --modversion rockchip_mpp
pkg-config --cflags --libs rockchip_mpp

pkg-config --variable=prefix librga
pkg-config --modversion librga
pkg-config --cflags --libs librga

pkg-config --modversion libdrm
```

MPP 和 RGA 的 `prefix` 应该是：

```text
/home/cat/ffmpeg/install
```

实测版本为：

```text
rockchip_mpp：1.3.9
librga：2.1.0
libdrm：2.4.125
```

## 8. 配置 FFmpeg

FFmpeg 使用独立的构建目录，不需要在源码目录执行 `make distclean`：

```bash
mkdir -p "$RK_BUILD_ROOT/ffmpeg-full"
cd "$RK_BUILD_ROOT/ffmpeg-full"
```

执行完整配置：

```bash
"$RK_SOURCE_ROOT/ffmpeg-rockchip/configure" \
  --prefix="$RK_INSTALL_PREFIX" \
  --enable-gpl \
  --enable-version3 \
  --enable-pthreads \
  --enable-libdrm \
  --enable-rkmpp \
  --enable-rkrga \
  --enable-libfreetype \
  --enable-libharfbuzz \
  --enable-libfontconfig \
  --enable-libfribidi \
  --enable-libass \
  --enable-openssl \
  --enable-libsrt \
  --enable-librist \
  --enable-libssh \
  --enable-libv4l2 \
  --enable-libopus \
  --enable-libmp3lame \
  --enable-libvorbis \
  --enable-libsoxr \
  --enable-libwebp \
  --enable-libzimg \
  --extra-cflags="-I$RK_INSTALL_PREFIX/include" \
  --extra-ldflags="-L$RK_INSTALL_PREFIX/lib -Wl,-rpath,$RK_INSTALL_PREFIX/lib" \
  --extra-libs="-lpthread -lm"
```

如果配置成功，命令最后不会出现 `ERROR`。

检查关键配置：

```bash
grep -E 'CONFIG_(RKMPP|RKRGA|LIBDRM|LIBFREETYPE|LIBHARFBUZZ|LIBASS)' config.h
```

预期关键项为 `1`：

```text
CONFIG_RKMPP
CONFIG_RKRGA
CONFIG_LIBDRM
CONFIG_LIBFREETYPE
CONFIG_LIBHARFBUZZ
CONFIG_LIBASS
```

## 9. 编译并安装 FFmpeg

执行：

```bash
cd "$RK_BUILD_ROOT/ffmpeg-full"

make -j"$(nproc)"
make install
```

安装后的程序：

```text
/home/cat/ffmpeg/install/bin/ffmpeg
/home/cat/ffmpeg/install/bin/ffprobe
```

查看版本：

```bash
"$RK_INSTALL_PREFIX/bin/ffmpeg" -hide_banner -version
```

本文实测版本应该包含以下配置：

```text
--enable-libdrm
--enable-rkmpp
--enable-rkrga
--enable-libfreetype
--enable-libharfbuzz
--enable-libfontconfig
--enable-libfribidi
--enable-libass
--enable-openssl
--enable-libsrt
--enable-librist
--enable-libssh
--enable-libv4l2
```

## 10. 可选：加入 PATH

当前终端临时使用：

```bash
export PATH="$RK_INSTALL_PREFIX/bin:$PATH"
export LD_LIBRARY_PATH="$RK_INSTALL_PREFIX/lib:${LD_LIBRARY_PATH:-}"
```

之后可以直接执行：

```bash
ffmpeg -version
ffprobe -version
```

如果不修改 `PATH`，直接使用绝对路径最稳妥：

```bash
/home/cat/ffmpeg/install/bin/ffmpeg
```

## 11. 验证硬件功能

### 11.1 查看硬件加速类型

```bash
"$RK_INSTALL_PREFIX/bin/ffmpeg" -hide_banner -hwaccels
```

预期包含：

```text
drm
rkmpp
```

### 11.2 查看 MPP 硬件解码器

```bash
"$RK_INSTALL_PREFIX/bin/ffmpeg" -hide_banner -decoders | grep rkmpp
```

本文实测包含：

```text
av1_rkmpp
h263_rkmpp
h264_rkmpp
hevc_rkmpp
mjpeg_rkmpp
mpeg1_rkmpp
mpeg2_rkmpp
mpeg4_rkmpp
vp8_rkmpp
vp9_rkmpp
```

### 11.3 查看 MPP 硬件编码器

```bash
"$RK_INSTALL_PREFIX/bin/ffmpeg" -hide_banner -encoders | grep rkmpp
```

本文实测包含：

```text
h264_rkmpp
hevc_rkmpp
mjpeg_rkmpp
```

### 11.4 查看 RGA 和 OSD 滤镜

```bash
"$RK_INSTALL_PREFIX/bin/ffmpeg" -hide_banner -filters | \
grep -E 'drawtext|subtitles|overlay_rkrga|scale_rkrga|vpp_rkrga|zscale'
```

预期包含：

```text
drawtext
subtitles
overlay_rkrga
scale_rkrga
vpp_rkrga
zscale
```

### 11.5 查看网络协议

```bash
"$RK_INSTALL_PREFIX/bin/ffmpeg" -hide_banner -protocols | \
grep -E 'https|srt|rist|ssh|rtp|udp|tcp|rtmp'
```

## 12. USB MJPEG 硬件解码测试

首先确认摄像头格式：

```bash
v4l2-ctl -d /dev/video43 --list-formats-ext
```

只测试 MPP 硬件解码，不添加滤镜：

```bash
"$RK_INSTALL_PREFIX/bin/ffmpeg" \
  -hide_banner \
  -c:v mjpeg_rkmpp \
  -f v4l2 \
  -video_size 1280x720 \
  -input_format mjpeg \
  -framerate 30 \
  -i /dev/video43 \
  -t 10 \
  -an \
  -f null -
```

成功日志应该包含：

```text
mjpeg (mjpeg_rkmpp) -> wrapped_avframe (native)
Video: wrapped_avframe, nv16
```

这里的 `wrapped_avframe (native)` 是 Null 输出使用的伪编码器，不表示软件解码。判断解码器要看左侧的 `mjpeg_rkmpp`。

本文远程实测结果：

```text
90 帧
约 30 fps
MPP 输出 NV16
```

## 13. MPP 解码和 RGA 转换测试

USB MJPEG 是 YUV 4:2:2，MPP 解码后通常输出 NV16。下面通过 RGA 转换成 NV12：

```bash
"$RK_INSTALL_PREFIX/bin/ffmpeg" \
  -hide_banner \
  -hwaccel rkmpp \
  -hwaccel_output_format drm_prime \
  -c:v mjpeg_rkmpp \
  -f v4l2 \
  -video_size 1280x720 \
  -input_format mjpeg \
  -framerate 30 \
  -i /dev/video43 \
  -t 10 \
  -an \
  -vf "scale_rkrga=w=1280:h=720:format=nv12,hwdownload,format=nv12" \
  -f null -
```

成功日志应该包含：

```text
rga_api version 1.10.4
Video: wrapped_avframe, nv12
```

## 14. 完整零拷贝硬件编解码测试

处理链路：

```text
USB MJPEG
 -> mjpeg_rkmpp 硬件解码
 -> DRM PRIME 硬件帧
 -> scale_rkrga 转 NV12
 -> h264_rkmpp 硬件编码
```

测试命令：

```bash
"$RK_INSTALL_PREFIX/bin/ffmpeg" \
  -hide_banner \
  -hwaccel rkmpp \
  -hwaccel_output_format drm_prime \
  -c:v mjpeg_rkmpp \
  -f v4l2 \
  -video_size 1280x720 \
  -input_format mjpeg \
  -framerate 30 \
  -i /dev/video43 \
  -t 10 \
  -an \
  -vf "scale_rkrga=w=1280:h=720:format=nv12" \
  -c:v h264_rkmpp \
  -b:v 4000000 \
  -g 60 \
  -f null -
```

成功日志应该包含：

```text
mjpeg (mjpeg_rkmpp) -> h264 (h264_rkmpp)
Video: h264 (High), drm_prime
```

本文远程实测为 `90` 帧、约 `29 fps`。

## 15. 生成固定文字水印 MP4

下面生成 10 秒 MP4，左上角显示固定文字 `USB_MJPEG`：

```bash
"$RK_INSTALL_PREFIX/bin/ffmpeg" \
  -hide_banner \
  -y \
  -hwaccel rkmpp \
  -hwaccel_output_format drm_prime \
  -c:v mjpeg_rkmpp \
  -f v4l2 \
  -video_size 1280x720 \
  -input_format mjpeg \
  -framerate 30 \
  -i /dev/video43 \
  -t 10 \
  -an \
  -vf "scale_rkrga=w=1280:h=720:format=nv12,hwdownload,format=nv12,drawtext=text=USB_MJPEG:x=40:y=40:fontsize=36:fontcolor=white:box=1:boxcolor=black@0.5" \
  -c:v h264_rkmpp \
  -b:v 4000000 \
  -g 60 \
  -movflags +faststart \
  /home/cat/ffmpeg/usb_mjpeg_osd_test.mp4
```

这里的处理链路是：

```text
MPP 硬解 -> RGA 转 NV12 -> 下载到 CPU -> drawtext -> MPP 硬编
```

`drawtext` 是 CPU 文字绘制，`h264_rkmpp` 仍然是硬件编码。

## 16. 生成实时时间水印 MP4

下面生成 10 秒视频，左上角显示开发板当前日期和时间，每秒变化：

```bash
"$RK_INSTALL_PREFIX/bin/ffmpeg" \
  -hide_banner \
  -y \
  -hwaccel rkmpp \
  -hwaccel_output_format drm_prime \
  -c:v mjpeg_rkmpp \
  -f v4l2 \
  -video_size 1280x720 \
  -input_format mjpeg \
  -framerate 30 \
  -i /dev/video43 \
  -t 10 \
  -an \
  -vf "scale_rkrga=w=1280:h=720:format=nv12,hwdownload,format=nv12,drawtext=text=%{localtime}:x=40:y=40:fontsize=42:fontcolor=white:box=1:boxcolor=black@0.65:boxborderw=12" \
  -c:v h264_rkmpp \
  -b:v 4000000 \
  -g 60 \
  -movflags +faststart \
  /home/cat/ffmpeg/usb_mjpeg_realtime_osd_test.mp4
```

本文实测输出：

```text
编码：H.264 High
分辨率：1280x720
帧率：30 fps
时长：10 秒
帧数：300
码率：约 3.93 Mbps
```

查看文件信息：

```bash
"$RK_INSTALL_PREFIX/bin/ffprobe" \
  -hide_banner \
  -show_entries format=duration,size,bit_rate \
  -show_entries stream=codec_name,profile,width,height,pix_fmt,r_frame_rate,nb_frames \
  /home/cat/ffmpeg/usb_mjpeg_realtime_osd_test.mp4
```

## 17. 常见错误处理

### 17.1 `Package rockchip_mpp was not found`

原因：FFmpeg 没找到 `rockchip_mpp.pc`。

检查：

```bash
find "$RK_INSTALL_PREFIX" -name rockchip_mpp.pc
echo "$PKG_CONFIG_PATH"
pkg-config --cflags --libs rockchip_mpp
```

修复：

```bash
export PKG_CONFIG_PATH="$RK_INSTALL_PREFIX/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/lib/pkgconfig"
```

### 17.2 `ERROR: rkmpp requires --enable-libdrm`

确认安装：

```bash
sudo apt install -y libdrm-dev
pkg-config --modversion libdrm
```

FFmpeg 配置必须同时包含：

```text
--enable-libdrm
--enable-rkmpp
```

### 17.3 `ERROR: rkrga requires --enable-rkmpp`

RKRGA 依赖 RKMPP。FFmpeg 配置必须同时包含：

```text
--enable-libdrm
--enable-rkmpp
--enable-rkrga
```

### 17.4 没有 `drawtext`

检查：

```bash
"$RK_INSTALL_PREFIX/bin/ffmpeg" -hide_banner -filters | grep drawtext
```

该 FFmpeg 分支中 `drawtext` 必需依赖：

```text
libfreetype
libharfbuzz
```

建议同时启用：

```text
libfontconfig
libfribidi
```

### 17.5 `Impossible to convert between the formats`

如果命令只指定 `mjpeg_rkmpp`，解码器可能直接输出普通 NV16 帧。这时不能直接接：

```text
hwdownload,format=nv12
```

只测试硬件解码时，删除 `hwdownload`：

```bash
-c:v mjpeg_rkmpp ... -f null -
```

需要使用 RGA 和 `hwdownload` 时，必须强制 DRM PRIME：

```text
-hwaccel rkmpp
-hwaccel_output_format drm_prime
```

然后使用：

```text
scale_rkrga=format=nv12,hwdownload,format=nv12
```

### 17.6 摄像头权限不足

检查：

```bash
id
ls -l /dev/video43
```

用户应属于 `video` 组：

```bash
sudo usermod -aG video cat
```

执行后需要退出登录并重新登录。正常情况下不需要使用 `sudo ffmpeg`。

### 17.7 摄像头被占用

检查：

```bash
fuser /dev/video43
```

先正常停止占用摄像头的程序，再重新测试。不要在不确认进程用途的情况下直接强制结束。

## 18. 重新编译 FFmpeg

源码或配置改变后，建议新建另一个构建目录，避免旧配置污染：

```bash
export RK_FFMPEG_REBUILD=/home/cat/ffmpeg/build-local/ffmpeg-rebuild
mkdir -p "$RK_FFMPEG_REBUILD"
cd "$RK_FFMPEG_REBUILD"
```

然后重新执行第 8 节的 `configure`、第 9 节的 `make` 和 `make install`。

## 19. 最终检查清单

安装完成后逐项执行：

```bash
pkg-config --variable=prefix rockchip_mpp
pkg-config --variable=prefix librga

"$RK_INSTALL_PREFIX/bin/ffmpeg" -hide_banner -version
"$RK_INSTALL_PREFIX/bin/ffmpeg" -hide_banner -hwaccels
"$RK_INSTALL_PREFIX/bin/ffmpeg" -hide_banner -decoders | grep rkmpp
"$RK_INSTALL_PREFIX/bin/ffmpeg" -hide_banner -encoders | grep rkmpp
"$RK_INSTALL_PREFIX/bin/ffmpeg" -hide_banner -filters | grep rkrga
"$RK_INSTALL_PREFIX/bin/ffmpeg" -hide_banner -filters | grep drawtext
```

最终必须确认：

```text
MPP 路径：/home/cat/ffmpeg/install
RGA 路径：/home/cat/ffmpeg/install
硬件加速：drm、rkmpp
硬件解码：mjpeg_rkmpp、h264_rkmpp、hevc_rkmpp 等
硬件编码：h264_rkmpp、hevc_rkmpp、mjpeg_rkmpp
RGA 滤镜：overlay_rkrga、scale_rkrga、vpp_rkrga
文字滤镜：drawtext
字幕滤镜：subtitles
```

## 20. 重要说明

1. `drawtext` 是 CPU 绘制文字，不是 MPP 编码器硬件 OSD。
2. `overlay_rkrga`、`scale_rkrga`、`vpp_rkrga` 使用 RGA 硬件。
3. 真正的 MPP 编码器硬件 OSD 需要在 C 代码中通过 `MppMeta` 和 `KEY_OSD_DATA` 接入。
4. MPP 硬件 OSD 一般最多支持 8 个区域，并且位置和尺寸通常需要按 16 像素对齐。
5. MPP 编码器 OSD 只出现在编码输出中；如果预览画面和编码画面都需要水印，应在编码前使用 RGA 合成。
6. 本文使用独立安装目录，不会覆盖系统 FFmpeg。

