# rk3588

RK3588 机载视频管线项目（C + 部分 Python）：多路视频采集（CSI/HDMI/USB）、硬件编解码（Rockchip MPP + FFmpeg）、RGA/OSD 叠加、RTSP/RTP/SRT/UDP 推流、MP4 录像，以及 CAN/串口/SBUS/MAVLink 等外设控制。

## 依赖仓库

本项目编译依赖 Rockchip FFmpeg/MPP/RGA 及 MAVLink C 库，请先下载依赖仓库：

**https://github.com/changpan1110/ffmpeg**

```bash
# 在板子上 clone 到 /home/cat/ffmpeg（与本项目硬编码路径一致）
cd /home/cat
git clone https://github.com/changpan1110/ffmpeg.git ffmpeg

# 编译并安装依赖（FFmpeg + MPP + RGA）
cd ffmpeg
./install_rk3588_ffmpeg.sh deps      # 装系统依赖（需 sudo）
./install_rk3588_ffmpeg.sh mpp       # 编译安装 MPP
./install_rk3588_ffmpeg.sh rga       # 检查/安装 RGA
./install_rk3588_ffmpeg.sh ffmpeg    # 编译安装 FFmpeg
./install_rk3588_ffmpeg.sh check     # 检查版本与功能
# 或直接: ./install_rk3588_ffmpeg.sh all

# MAVLink C 头文件库（c_library_v2）在该仓库中已附带；
# 如需重新生成：./install_mavlink.sh
```

安装完成后将得到：

- `~/ffmpeg/install/` — FFmpeg / MPP / RGA 安装前缀（本项目 `RK_MEDIA_PREFIX` 指向这里）
- `~/ffmpeg/c_library_v2/` — MAVLink C 头文件库（本项目 `MAVLINK_C_ROOT` 指向这里）

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

> 依赖（FFmpeg/MPP/RGA/MAVLink）的下载与安装见上方「依赖仓库」章节；
> 本仓库同级的 `ffmpeg` 目录即依赖仓库（`install_rk3588_ffmpeg.sh`），
> 新板子可用 `deploy_rk3588_env.sh` 一键部署。

## 脚本使用详解

### 板载管理脚本（在 RK3588 上运行）

#### `rk3588_video.sh` — 视频系统统一管理（can0 + mediamtx + pipeline）

```bash
sudo ./rk3588_video.sh start       # 临时启动（立即启动，不开机自启）
sudo ./rk3588_video.sh enable      # 设置开机自启 + 立即启动
sudo ./rk3588_video.sh disable     # 取消开机自启
sudo ./rk3588_video.sh stop        # 停止
sudo ./rk3588_video.sh restart     # 重启
sudo ./rk3588_video.sh status      # 查看状态
sudo ./rk3588_video.sh             # 无参数 -> 显示帮助与状态
```

#### `deploy_rk3588_env.sh` — 新板子一键部署

```bash
./deploy_rk3588_env.sh all /path/to/rk3588_env.tar.gz   # 一键全部部署
./deploy_rk3588_env.sh deps                              # 只装系统依赖（需 sudo）
./deploy_rk3588_env.sh extract /path/to/tar.gz           # 只解压
./deploy_rk3588_env.sh build                             # 只编译项目
./deploy_rk3588_env.sh verify                            # 只验证
```

> 只有 `deps` 步骤需要 sudo，其余步骤用普通用户（cat）执行；
> 目标路径固定为 `/home/cat`（与代码内硬编码路径保持一致）。

#### `install_mavlink.sh` — 安装 MAVLink C 库

```bash
./install_mavlink.sh          # 一键安装并生成 c_library_v2 C 头文件
./install_mavlink.sh check    # 只做检查，不安装
```

#### `install_mediamtx_service.sh` — 安装 mediamtx systemd 服务

```bash
sudo bash install_mediamtx_service.sh
# 可覆盖二进制/配置路径（默认 ~/mediamtx、~/mediamtx.yml）：
sudo MEDIAMTX_BIN=/path/to/mediamtx MEDIAMTX_CONF=/path/to/mediamtx.yml bash install_mediamtx_service.sh
```

#### `set_ip.sh` — 网口 IP 切换（DHCP / 静态）

```bash
sudo ./set_ip.sh dhcp [网卡]                      # 切回 DHCP，默认 eth0
sudo ./set_ip.sh static [网卡] [IP] [前缀] [网关] [DNS]

# 示例：
sudo ./set_ip.sh static eth1                          # eth1 用默认 IP
sudo ./set_ip.sh static eth1 192.168.1.100            # eth1 指定 IP
sudo ./set_ip.sh static eth1 192.168.1.100 24 192.168.1.1 "223.5.5.5 114.114.114.114"
sudo ./set_ip.sh dhcp eth1
sudo ./set_ip.sh dhcp                                 # 默认 eth0
```

#### `can0.sh` — CAN0 手动管理（1Mbps）

```bash
./can0.sh start      # 配置波特率(1Mbps)并启动 can0
./can0.sh stop       # 停用 can0
./can0.sh restart    # 重启 can0
./can0.sh status     # 查看 can0 状态与收发统计
```

#### `can0_start.sh` / `can0_stop.sh` — CAN0 systemd 服务安装

```bash
sudo ./can0_start.sh     # 生成并启动 rk3588-can0.service（开机自启）
sudo ./can0_stop.sh      # 停止该服务
# 环境变量可覆盖默认参数（默认 can0 / 500kbps）：
sudo CAN_INTERFACE=can1 CAN_BITRATE=1000000 ./can0_start.sh
```

> systemd 单元文件模板见 `systemd/rk3588-can0.service`；详细说明见
> [CAN0_AUTOSTART.md](CAN0_AUTOSTART.md)。

#### `create_ftp_user.sh` — 创建 vsftpd 用户

```bash
sudo ./create_ftp_user.sh <用户名> <密码> <访问路径>
# 示例：
sudo ./create_ftp_user.sh ftpuser 123456 /home/cat/ftp_share
```

### 地面站脚本（在 Windows 开发机上运行）

#### `player.ps1`（入口 `player.cmd`）— 地面站播放器总管理

```powershell
player.cmd install       # 创建 .venv 并安装依赖
player.cmd start-test    # 启动 web + 网关（dry-run 模拟模式）
player.cmd start         # 启动 web + 真实 MAVLink 网关
player.cmd restart-test  # 模拟模式下重启
player.cmd restart       # 真实 MAVLink 模式下重启
player.cmd stop          # 停止 web 和网关
player.cmd status        # 查看服务状态和 URL
player.cmd logs          # 查看近期日志
player.cmd logs-follow   # 跟踪按钮命令与 ACK 日志
player.cmd smoke         # 发送一条按钮命令并校验 ACK
player.cmd test          # 运行本地单元与语法测试
player.cmd open          # 用默认浏览器打开播放器

# 常用参数：
player.cmd start -AirIp 192.168.31.14          # 指定机载端 MediaMTX/MAVLink 地址
player.cmd start -WebPort 8090 -GatewayPort 8091
player.cmd start -MavlinkSerial COM5 -MavlinkBaud 115200   # 用串口代替 UDP
player.cmd start -NoBrowser                    # 启动后不自动开浏览器
player.cmd start -DebugControl                 # 打印原始按钮数据和 MAVLink 参数
player.cmd install -BootstrapPython C:\Python312\python.exe
```

#### `service_manager.py` — 播放器服务底层管理（player.ps1 内部调用，也可直接用）

```powershell
.\.venv\Scripts\python.exe scripts\service_manager.py start [--dry-run] [--no-browser] [--debug-control]
.\.venv\Scripts\python.exe scripts\service_manager.py status
.\.venv\Scripts\python.exe scripts\service_manager.py open
.\.venv\Scripts\python.exe scripts\service_manager.py smoke
.\.venv\Scripts\python.exe scripts\service_manager.py stop
.\.venv\Scripts\python.exe scripts\service_manager.py logs
```

#### `ground_gateway/main.py` — 地面站 MAVLink 网关（FastAPI + WebSocket）

```powershell
# 模拟模式（不发 UDP，先验证网页与 WebSocket 链路）：
$env:MAVLINK_DRY_RUN="1"
.\.venv\Scripts\python.exe -m uvicorn ground_gateway.main:app --host 0.0.0.0 --port 8091

# 真实模式（环境变量控制目标，默认 udpout:192.168.31.14:14550）：
$env:MAVLINK_ENDPOINT="udpout:192.168.31.14:14550"
$env:MAVLINK_BAUD="115200"
.\.venv\Scripts\python.exe -m uvicorn ground_gateway.main:app --host 0.0.0.0 --port 8091
# 健康检查: http://127.0.0.1:8091/health
```

> 详细说明见 [ground_gateway/README.md](ground_gateway/README.md)。

### 机载端 MAVLink 模拟与测试

#### `scripts/air_mock.sh` — 机载端 MAVLink mock 管理（在板子上运行）

```bash
./air_mock.sh start        # 后台启动 mock 接收器
./air_mock.sh restart      # 修改配置后重启
./air_mock.sh foreground   # 前台启动
./air_mock.sh stop         # 停止后台接收器
./air_mock.sh status       # 查看状态
./air_mock.sh logs         # 跟踪日志
./air_mock.sh config       # 打印当前生效配置
```

> 不要用 sudo 运行；配置文件 `config/air_mavlink.conf`，默认监听
> `udpin:0.0.0.0:14550`。

#### `ground_gateway/air_mock.py` — mock 接收器本体（可单独运行）

```bash
python air_mock.py --listen udpin:0.0.0.0:14550 --baud 115200 --system 1 --component 191
```

#### `scripts/test_high_speed_camera_pty.py` — 高速相机 PTY 集成测试

```bash
python scripts/test_high_speed_camera_pty.py
# 通过伪终端(pty)驱动相机程序，校验密钥请求/响应、坏 CRC、错误响应等协议行为
```

### 开发辅助脚本

#### `scripts/generate_osd_mockups.ps1` — 生成 OSD 效果图

```powershell
powershell -ExecutionPolicy Bypass -File scripts\generate_osd_mockups.ps1
# 可选参数：-OutputDirectory docs/osd_mockups（默认）
```

#### `examples/mavlink_udp_receiver.c` — MAVLink UDP 接收示例

编译后运行：接收并打印 MAVLink v2 报文（参考用，演示 c_library_v2 的集成方式）。

#### 地面站单元测试

```powershell
.\.venv\Scripts\python.exe -m pytest ground_gateway/tests -v
```

### 主要配置文件

| 文件 | 用途 |
|---|---|
| `config/video_pipeline.json` | 视频管线主配置（输入/编码/OSD/输出各节点） |
| `config/app.ini` | 应用层配置 |
| `config/mavlink_control.conf` | 板载 MAVLink 控制服务配置 |
| `config/air_mavlink.conf` | 机载端 mock（air_mock.sh）配置 |
| `ground_gateway/.env.example` | 地面站网关环境变量示例 |

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
