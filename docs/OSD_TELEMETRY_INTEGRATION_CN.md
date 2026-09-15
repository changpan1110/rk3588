# OSD 云台与激光数据对接说明

## 1. 文档目的

本文说明如何把外部采集到的云台航向、云台俯仰和激光测距数据送入视频 OSD。

OSD 只叠加到推流画面，不写入各通道的原始录像。运行时先生成透明 RGBA OSD 图层，再由 RK3588 的 `overlay_rkrga` 叠加到视频帧。

推荐使用控制层接口：

```c
#include "control/video_pipeline/video_pipeline_control.h"
```

## 2. 初始化与关闭

视频管线初始化成功后创建控制对象：

```c
vp_ctx_t *pipeline = NULL;
vp_control_t *control = NULL;
app_status_t status;

status = vp_init(&pipeline, &video_config);
if (status != APP_OK) {
    /* 处理视频管线初始化失败 */
}

status = vp_control_init(&control, pipeline);
if (status != APP_OK) {
    vp_deinit(pipeline);
    pipeline = NULL;
    /* 处理控制层初始化失败 */
}

/* 启用 OSD，十字中心使用画面中心。 */
vp_control_osd_set_enabled(control, 1);
vp_control_osd_set_position(control, -1, -1);
```

程序退出时，必须先释放控制层，再释放视频管线：

```c
vp_control_deinit(control);
vp_deinit(pipeline);
```

## 3. 云台数据接口

函数声明：

```c
app_status_t vp_control_osd_set_gimbal_angles(vp_control_t *control,
                                              float yaw_deg,
                                              float pitch_deg,
                                              int valid);
```

参数说明：

| 参数 | 单位 | 有效范围 | 说明 |
| --- | --- | --- | --- |
| `control` | - | 非空 | `vp_control_init()` 返回的控制对象 |
| `yaw_deg` | 度 | `-140.0` 到 `+140.0` | 航向角，中间位置为 `0` |
| `pitch_deg` | 度 | `-90.0` 到 `+30.0` | 向上为正，向下为负 |
| `valid` | - | `0` 或非 `0` | 非 `0` 表示数据有效，`0` 表示当前数据失效 |

超出显示范围的数据会自动限位：

- 航向角小于 `-140` 时按 `-140` 显示，大于 `+140` 时按 `+140` 显示。
- 俯仰角小于 `-90` 时按 `-90` 显示，大于 `+30` 时按 `+30` 显示。
- `NaN` 或无穷值在 `valid != 0` 时返回 `APP_ERR_PARAM`。
- `valid == 0` 时航向和俯仰同时进入无效状态，OSD 显示 `---.-°`。

正常更新示例：

```c
float yaw = 45.6f;
float pitch = -12.3f;

if (vp_control_osd_set_gimbal_angles(control, yaw, pitch, 1) != APP_OK) {
    /* 记录本次更新失败 */
}
```

云台断线或数据超时：

```c
vp_control_osd_set_gimbal_angles(control, 0.0f, 0.0f, 0);
```

## 4. 激光测距接口

函数声明：

```c
app_status_t vp_control_osd_set_laser_distance(vp_control_t *control,
                                               float distance_m,
                                               int signal_level,
                                               int valid);
```

参数说明：

| 参数 | 单位 | 有效范围 | 说明 |
| --- | --- | --- | --- |
| `control` | - | 非空 | `vp_control_init()` 返回的控制对象 |
| `distance_m` | 米 | 大于或等于 `0` | 激光测距结果 |
| `signal_level` | 自定义 | 整数 | 预留的信号强度字段，目前仅保存，尚未改变 OSD 样式 |
| `valid` | - | `0` 或非 `0` | 非 `0` 表示测距有效，`0` 表示测距失效 |

显示端会把最大距离限制为 `9999.9 m`。负数、`NaN` 或无穷值在 `valid != 0` 时返回 `APP_ERR_PARAM`。

正常更新示例：

```c
float distance_m = 128.6f;
int signal_level = 80;

if (vp_control_osd_set_laser_distance(control,
                                      distance_m,
                                      signal_level,
                                      1) != APP_OK) {
    /* 记录本次更新失败 */
}
```

激光断线、无回波或数据超时：

```c
vp_control_osd_set_laser_distance(control, 0.0f, 0, 0);
```

此时红色距离位置显示 `---.- m`。

## 5. 推荐线程模型

接口内部有互斥锁，可以从不同线程调用。推荐结构如下：

```text
云台采集线程  -> vp_control_osd_set_gimbal_angles()
激光采集线程  -> vp_control_osd_set_laser_distance()
视频推流线程  -> 读取最新 OSD 数据并完成 RGA 叠加
```

不需要再创建一个专门的 OSD 更新线程。云台和激光可以分别在自己的采集线程中直接提交最新数据。

这些接口采用“最新值”模型，不是消息队列：

- 每次调用都会覆盖对应字段的旧值。
- 云台更新不会覆盖激光数据。
- 激光更新不会覆盖云台数据。
- 如果采集频率高于视频帧率，中间部分数据可能不会显示，但最新数据会被下一帧读取。
- 不要为了每一个遥测数据建立 OSD 绘制任务队列，否则会增加延迟并显示过期数据。

## 6. 刷新频率

接口可以按照传感器实际频率调用，例如 `10 Hz`、`50 Hz` 或更高。

最终肉眼可见的 OSD 刷新频率受推流帧率限制：

| 视频帧率 | 最大可见 OSD 刷新率 |
| --- | --- |
| `25 FPS` | 约 `25 Hz` |
| `30 FPS` | 约 `30 Hz` |
| `50 FPS` | 约 `50 Hz` |
| `60 FPS` | 约 `60 Hz` |

例如云台以 `100 Hz` 上报、视频为 `30 FPS` 时，可以继续提交 `100 Hz` 数据，但画面最多显示约 `30 Hz`，每一帧使用当时最新的角度。

如果希望减少 CPU 锁竞争，可以在采集层把提交频率限制到视频帧率附近，但通常没有必要。

## 7. 完整采集回调示例

```c
typedef struct {
    vp_control_t *video_control;
} telemetry_context_t;

void on_gimbal_packet(telemetry_context_t *ctx,
                      int packet_valid,
                      float yaw_deg,
                      float pitch_deg) {
    if (ctx == NULL || ctx->video_control == NULL) {
        return;
    }

    if (packet_valid) {
        vp_control_osd_set_gimbal_angles(ctx->video_control,
                                         yaw_deg,
                                         pitch_deg,
                                         1);
    } else {
        vp_control_osd_set_gimbal_angles(ctx->video_control,
                                         0.0f,
                                         0.0f,
                                         0);
    }
}

void on_laser_packet(telemetry_context_t *ctx,
                     int packet_valid,
                     float distance_m,
                     int signal_level) {
    if (ctx == NULL || ctx->video_control == NULL) {
        return;
    }

    if (packet_valid) {
        vp_control_osd_set_laser_distance(ctx->video_control,
                                          distance_m,
                                          signal_level,
                                          1);
    } else {
        vp_control_osd_set_laser_distance(ctx->video_control,
                                          0.0f,
                                          0,
                                          0);
    }
}
```

建议在采集层增加数据超时判断。例如连续 `500 ms` 没有收到云台或激光数据，就调用对应接口并设置 `valid = 0`。

## 8. 读取当前 OSD 数据

调试或状态查询时可以读取当前快照：

```c
vp_osd_params_t osd;

if (vp_control_osd_get(control, &osd) == APP_OK) {
    printf("yaw=%.1f valid=%d, pitch=%.1f valid=%d, laser=%.1f valid=%d\n",
           osd.yaw_deg,
           osd.yaw_valid,
           osd.pitch_deg,
           osd.pitch_valid,
           osd.distance_m,
           osd.laser_valid);
}
```

## 9. 不使用控制层时的底层接口

如果当前模块只有 `vp_ctx_t *pipeline`，也可以直接调用底层接口：

```c
vp_osd_set_gimbal_angles(pipeline, yaw_deg, pitch_deg, valid);
vp_osd_set_laser_distance(pipeline, distance_m, signal_level, valid);
```

底层接口同样是线程安全的。但是应用层已经持有 `vp_control_t` 时，应优先使用 `vp_control_osd_*` 接口，以保持控制状态一致。

## 10. 相关代码位置

- 公共控制接口：`include/control/video_pipeline/video_pipeline_control.h`
- 视频管线底层接口和数据结构：`include/process/video/video_pipeline.h`
- 控制层实现：`src/control/video_pipeline/video_pipeline_control.c`
- OSD 数据保存、限位和绘制：`src/process/osd/video_pipeline_osd.c`
- 推流线程更新与 RGA 叠加：`src/output/stream/video_pipeline_stream.c`

## 11. 可交给其他对话的任务说明

可以把下面内容和本文档路径交给负责串口、CAN、网络或云台协议的开发对话：

```text
请阅读 docs/OSD_TELEMETRY_INTEGRATION_CN.md。
把当前模块解析到的云台航向角、俯仰角和激光距离接入视频 OSD。
优先使用 vp_control_osd_set_gimbal_angles() 和
vp_control_osd_set_laser_distance()，不要修改 OSD 绘制代码，
不要建立遥测显示队列。数据断线或超时时调用接口并设置 valid=0。
完成后检查角度单位为度、距离单位为米，并验证多线程调用安全。
```
