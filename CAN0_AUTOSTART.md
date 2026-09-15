# RK3588 CAN0 使用说明

## 文件

- `can0_start.sh`：配置并启动 `can0`，首次执行时自动创建并启用 systemd 自启动服务。
- `can0_stop.sh`：停止 `can0`。如果已经安装 systemd 服务，会同时停止该服务。

当前默认配置：

```text
接口：can0
波特率：500000 bit/s
BUS-OFF 自动恢复：100 ms
```

## 首次安装

将脚本放在板子的工程目录：

```bash
cd /home/cat/rk3588
```

首次执行一条命令即可完成配置、启动和开机自启：

```bash
sudo sh ./can0_start.sh
```

脚本会自动完成：

1. 配置 `can0` 的波特率和 `restart-ms`。
2. 将 `can0` 设置为 `UP`。
3. 创建 `/etc/systemd/system/rk3588-can0.service`。
4. 启用并立即启动 systemd 服务。

## 日常操作

启动：

```bash
cd /home/cat/rk3588
sudo ./can0_start.sh
```

停止：

```bash
sudo ./can0_stop.sh
```

查看接口状态：

```bash
ip -details -statistics link show can0
```

查看自启动服务：

```bash
systemctl status rk3588-can0.service
```

## 接收全部 CAN ID

`candump` 默认可以接收总线上所有普通 CAN ID。显式使用全 ID 过滤器：

```bash
candump can0,0:0
```

当前设备曾观察到的报文示例：

```text
can0 426 [8] 55 45 04 DE E5 06 C1 04
```

这里的 CAN ID 是 `0x426`，不属于 DJI RSDK 示例中的 `0x222/0x223`。

后台记录全部接收报文：

```bash
nohup candump -L can0,0:0 > /tmp/can0_broadcast.log 2>&1 < /dev/null &
tail -f /tmp/can0_broadcast.log
```

## 修改波特率

修改 `can0_start.sh` 中的：

```sh
CAN_BITRATE="${CAN_BITRATE:-500000}"
```

例如改成 1 Mbps：

```sh
CAN_BITRATE="${CAN_BITRATE:-1000000}"
```

保存后重新执行：

```bash
sudo sh ./can0_start.sh
```

脚本会重新生成 systemd 配置并立即应用新参数。

## 禁用开机自启

只停止当前接口但保留开机自启：

```bash
sudo ./can0_stop.sh
```

同时禁用并停止开机服务：

```bash
sudo systemctl disable --now rk3588-can0.service
```

## 常见状态

- `state UP`、`CAN state ERROR-ACTIVE`：接口正常工作。
- `ERROR-PASSIVE`：总线出现较多错误，检查 CANH/CANL、终端电阻、收发器使能和对端设备。
- `errno=105 (No buffer space available)`：发送队列因总线错误无法继续发送，通常表示对端没有 ACK 或物理层没有正常通信。
- `candump` 没有任何输出：说明当前监听期间没有收到有效普通 CAN 数据帧。
