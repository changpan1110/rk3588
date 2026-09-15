# rk3588

RK3588 机载视频管线项目（C + 部分 Python）：多路视频采集（CSI/HDMI/USB）、硬件编解码（Rockchip MPP + FFmpeg）、RGA/OSD 叠加、RTSP/RTP/SRT/UDP 推流、MP4 录像，以及 CAN/串口/SBUS/MAVLink 等外设控制。

## 硬件与系统环境

- 平台：RK3588（aarch64），Debian 12
- 运行用户：`cat`（脚本和路径均按 `/home/cat` 硬编码）
- 依赖安装位置：
  - Rockchip FFmpeg / MPP / RGA：`/home/cat/ffmpeg/install`
  - MAVLink C 头文件：`/home/cat/ffmpeg/c_library_v2`

## 目录结构

```
.
├── CMakeLists.txt          # 构建配置（顶部可改依赖路径）
├── src/                    # 源代码
│   ├── app/                # 主程序与配置（video_pipeline_main）
│   ├── input/              # 输入：video(csi/hdmi/usb)、can、serial、network
│   ├── process/            # 处理：video(管线)、codec(编码)、osd(RGA叠加)
│   ├── output/             # 输出：display(GUI)、file(mp4)、stream(rtsp/rtp/srt/udp)
│   ├── control/            # 外设控制：mavlink、sbus、dji_rsdk、modbus、
│   │                       #   visca、thermal_camera、high_speed_camera、uart_laser 等
│   ├── service/            # 各设备服务线程
│   └── tools/              # 测试工具程序
├── include/                # 头文件（与 src 对应）
├── config/                 # 运行配置：video_pipeline.json、mavlink_control.conf 等
├── ground_gateway/         # 地面站网关（Python：mavlink 转发 + Web 管理页）
├── docs/                   # 设计文档、OSD 效果图
├── scripts/                # 辅助脚本（player、service_manager 等）
├── systemd/                # 服务单元文件（can0 等）
├── examples/               # 示例代码
└── mpp_analysis/           # MPP/FFmpeg 参考源码分析
```

## 构建

```bash
cd /home/cat/rk3588
# 首次构建前请确认 CMakeLists.txt 顶部的两个路径：
#   MAVLINK_C_ROOT  = /home/cat/ffmpeg/c_library_v2
#   RK_MEDIA_PREFIX = /home/cat/ffmpeg/install
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
# 产物: build/rk3588_video_pipeline_main
```

> 依赖（FFmpeg/MPP/RGA/MAVLink）的安装脚本在本仓库同级的
> `ffmpeg` 目录（`install_rk3588_ffmpeg.sh`，详见 `docs`），MAVLink 用
> `install_mavlink.sh`；新板子可用 `deploy_rk3588_env.sh` 一键部署。

## 运行管理

```bash
sudo ./rk3588_video.sh start      # 临时启动（can0 + mediamtx + pipeline）
sudo ./rk3588_video.sh enable     # 设置开机自启并立即启动
sudo ./rk3588_video.sh stop       # 停止
sudo ./rk3588_video.sh restart    # 重启
sudo ./rk3588_video.sh status     # 查看状态
```

## 常用脚本速查

| 脚本 | 用途 |
|---|---|
| `rk3588_video.sh` | 视频系统统一管理（can0 + mediamtx + pipeline） |
| `deploy_rk3588_env.sh` | 新板子一键部署（deps/extract/build/verify/all） |
| `install_mavlink.sh` | 安装 MAVLink C 库 |
| `install_mediamtx_service.sh` | 安装 mediamtx 推流服务 |
| `set_ip.sh` | 网口 IP 切换（DHCP/静态） |
| `can0.sh` / `can0_start.sh` / `can0_stop.sh` | CAN0 启停（详见 `CAN0_AUTOSTART.md`） |
| `create_ftp_user.sh` | 创建 vsftpd 用户 |
| `scripts/player.ps1` | Windows 端播放器启动脚本 |

## 主要文档

- [CAN0_AUTOSTART.md](CAN0_AUTOSTART.md) — CAN0 开机自启说明
- [MAVLINK_CONTROL_CODE_MAP.md](MAVLINK_CONTROL_CODE_MAP.md) — MAVLink 控制码映射
- [docs/OSD_TELEMETRY_INTEGRATION_CN.md](docs/OSD_TELEMETRY_INTEGRATION_CN.md) — OSD 遥测叠加集成
- [docs/osd_mockups/README.md](docs/osd_mockups/README.md) — OSD 渲染架构说明
- [ground_gateway/README.md](ground_gateway/README.md) — 地面站网关使用说明

## 开发同步

- 板子：`/home/cat/rk3588`（工作区直接编译运行）
- Windows 开发机：`F:\rk3588_board\rk3588`（本仓库 clone）
- 以 GitHub `main` 分支为准，板子上的改动请及时 commit + push
