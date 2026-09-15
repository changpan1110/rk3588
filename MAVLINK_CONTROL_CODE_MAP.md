# MAVLink 控制代码入口地图

## 1. 从主程序开始看

初始化入口：

```text
src/app/video_pipeline_main.c
  main()
    -> mavlink_control_actions_init(...)
    -> mavlink_control_service_init(...,
           mavlink_control_actions_handle,
           mavlink_actions)
```

主程序只负责初始化。所有网页按键的业务处理集中在独立文件：

```text
src/control/mavlink/mavlink_control_actions.c
  mavlink_control_actions_handle()
```

当前键值：

```text
1  MAVLINK_CONTROL_KEY_VIDEO_SWITCH   视频切换
2  MAVLINK_CONTROL_KEY_SNAPSHOT       拍照
3  MAVLINK_CONTROL_KEY_RECORD_TOGGLE  录像切换
4  MAVLINK_CONTROL_KEY_ZOOM_IN        变焦+
5  MAVLINK_CONTROL_KEY_ZOOM_OUT       变焦-
6  MAVLINK_CONTROL_KEY_TRIGGER        触发
7  MAVLINK_CONTROL_KEY_FOCUS_IN       对焦+
8  MAVLINK_CONTROL_KEY_FOCUS_OUT      对焦-
9  MAVLINK_CONTROL_KEY_UNLOCK         解锁
10 MAVLINK_CONTROL_KEY_THERMAL_PSEUDOCOLOR 热成像伪彩
11 MAVLINK_CONTROL_KEY_LASER_SINGLE_MEASURE 单次测量
12 MAVLINK_CONTROL_KEY_LASER_CONTINUOUS_MEASURE 连续测量开关
13 MAVLINK_CONTROL_KEY_OSD_ENABLED    OSD开关
```

视频切换、拍照、录像和 OSD 已调用现有异步业务接口。所有业务都在 `mavlink_control_actions_handle()` 的一个 `switch` 中直接路由；相机运动、解锁、触发、伪彩和激光尚未接入真实硬件函数，当前打印日志并返回不支持。

## 2. MAVLink 服务入口

公开接口：

```text
include/control/mavlink/mavlink_control_service.h
```

线程、MAVLink 解析、UID 去重、回调通知和 ACK：

```text
src/control/mavlink/mavlink_control_service.c
  mavlink_control_service_init()
  mavlink_control_thread()
  mavlink_control_process_bytes()
  mavlink_control_notify_key()
  mavlink_control_service_deinit()
```

服务不再直接调用视频接口。它只把解析后的 `mavlink_control_key_event_t` 通知给初始化时注册的回调。

## 3. UDP/TCP/串口入口

配置读取：

```text
src/control/mavlink/mavlink_control_config.c
config/mavlink_control.conf
```

传输选择总入口：

```text
src/control/mavlink/mavlink_transport.c
  mavlink_transport_run()
```

具体实现完全分开：

| 配置 | 源文件 | 入口函数 | 数据收发 |
| --- | --- | --- | --- |
| `transport=udp` | `mavlink_transport_udp.c` | `mavlink_transport_udp_run()` | `recvfrom()/sendto()` |
| `transport=tcp` | `mavlink_transport_tcp.c` | `mavlink_transport_tcp_run()` | `recv()/send()` |
| `transport=serial` | `mavlink_transport_serial.c` | `mavlink_transport_serial_run()` | `input/serial/uart_base` |

修改传输方式只需要修改：

```ini
# config/mavlink_control.conf
transport=udp
```

TCP 服务器：

```ini
transport=tcp
tcp_mode=server
tcp_host=0.0.0.0
tcp_port=5760
```

TCP 客户端：

```ini
transport=tcp
tcp_mode=client
tcp_host=192.168.31.100
tcp_port=5760
```

串口：

```ini
transport=serial
serial_device=/dev/ttyUSB0
serial_baud=115200
```

## 4. 完整调用顺序

```text
video_pipeline_main.c
  mavlink_control_service_init(callback)
    -> mavlink_control_service.c 创建线程
      -> mavlink_transport.c 选择传输方式
        -> mavlink_transport_udp/tcp/serial.c 收到字节
          -> mavlink_control_service.c 解析 MAVLink
            -> 去重 UID
              -> mavlink_control_actions_handle() 执行业务
                -> 返回 app_status_t 和 request_id
              -> service 生成 COMMAND_ACK
          -> 原传输文件发回 ACK
```

## 5. 后续增加按键

1. 在 `mavlink_control_service.h` 的 `mavlink_control_key_t` 增加键值。
2. 在 `mavlink_web_control.h` 增加相同协议动作编号并更新范围校验。
3. 在 `mavlink_control_key_name()` 增加名称。
4. 在 `mavlink_control_actions_handle()` 中增加 `case`，并直接调用对应的业务或设备控制函数。
5. 地面 `ground_gateway/gateway_core.py` 增加命令名称到键值的映射。
6. 网页增加对应的 `data-command` 按钮。

## 6. 持续按键和参数

`mavlink_control_key_event_t` 中包含：

```text
event = click / press / release
value = 0..63
```

变焦和对焦使用 `press/release`。伪彩使用 `value=0..14`，对应白热到深蓝色。连续测量与 OSD使用 `value=0/1`。SFL0603 连续测量的预留默认周期为 1000 ms，后续接入真实激光线程时可改为配置项。
