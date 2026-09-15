# RK3588 Ground MAVLink Gateway

地面端网页通过本机 WebSocket 发送 JSON 命令，本服务把命令转换为 MAVLink v2 `COMMAND_LONG/MAV_CMD_USER_1`，并通过 UDP、TCP 或串口发送，等待 `COMMAND_ACK`。Windows 推荐直接使用根目录的 `player.cmd`，通信参数统一保存在 `websocket/config/player.json`。

## 1. 地面端安装

Ubuntu：

```bash
cd ~/rk3588
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r ground_gateway/requirements.txt
```

Windows PowerShell 不需要激活虚拟环境：

```powershell
cd F:\rk3588
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r ground_gateway\requirements.txt
```

## 2. 先验证网页按钮

模拟模式不会发送 UDP，适合先确认网页与 WebSocket：

Ubuntu：

```bash
MAVLINK_DRY_RUN=1 .venv/bin/python -m uvicorn ground_gateway.main:app --host 0.0.0.0 --port 8091
```

Windows PowerShell：

```powershell
$env:MAVLINK_DRY_RUN="1"
.\.venv\Scripts\python.exe -m uvicorn ground_gateway.main:app --host 0.0.0.0 --port 8091
```

健康检查：`http://127.0.0.1:8091/health`。

另开一个终端，在地面电脑启动网页：

```bash
cd web_player
python3 -m http.server 8090 --bind 0.0.0.0
```

打开 `http://地面电脑IP:8090/`。视频地址保持为 `http://192.168.31.14:8889/live/`，控制连接会自动使用地面电脑的 `8091` 端口。

系统配置页面：

```text
http://地面电脑IP:8091/admin
```

配置页面可以保存视频、服务端口和 MAVLink 参数，并同步更新播放器运行配置。视频地址可以直接刷新生效；端口、MAVLink 和调试参数需要重启服务。

## 3. 验证真实 MAVLink UDP

把 `ground_gateway/air_mock.py` 放到空中 RK3588 的 `~/mavlink/air_mock.py`，然后运行：

```bash
cd ~/mavlink
source .venv/bin/activate
python air_mock.py --listen udpin:0.0.0.0:14550
```

地面端关闭模拟模式并启动网关：

Ubuntu：

```bash
MAVLINK_DRY_RUN=0 MAVLINK_ENDPOINT=udpout:192.168.31.14:14550 \
  .venv/bin/python -m uvicorn ground_gateway.main:app --host 0.0.0.0 --port 8091
```

Windows PowerShell：

```powershell
$env:MAVLINK_DRY_RUN="0"
$env:MAVLINK_ENDPOINT="udpout:192.168.31.14:14550"
.\.venv\Scripts\python.exe -m uvicorn ground_gateway.main:app --host 0.0.0.0 --port 8091
```

点击网页按钮后，空中端应打印命令、事件和参数，网页控制状态应显示“指令已接收”。

## 4. 接入空中 C 程序

UDP 模拟验证通过后，用 `include/control/mavlink/mavlink_web_control.h` 和 `src/control/mavlink/mavlink_web_control.c` 替换 Python 模拟接收器。空中端必须：

1. 监听 UDP `14550`。
2. 使用 `mavlink_parse_char()` 解析字节流。
3. 调用 `mavlink_web_control_handle()` 去重并分发动作。
4. 把生成的 `COMMAND_ACK` 发回 UDP 消息来源地址。

重试时地面端复用相同的命令 UID；空中端只执行一次并返回缓存 ACK。

## 5. 按下/松开与参数编码

`COMMAND_LONG.param1` 使用一个 16 位控制字，同时保持旧点击命令兼容：

```text
bit 0..7    动作编号
bit 8..9    事件：0=click, 1=press, 2=release
bit 10..15  参数值：伪彩编号或开关状态
```

`param2..3` 保存 32 位 sequence，`param4..7` 保存 64 位命令 UID。具体校验和编码位于 `ground_gateway/gateway_core.py`。
