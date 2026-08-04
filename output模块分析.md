# output 模块现状

更新日期：2026-08-04

`src/output` 只保留有真实实现且仍在构建目标中使用的模块。早期仅保存参数、直接返回 `APP_OK` 或生成假文件的占位模块已经删除。

| 模块 | 职责 | 主要调用方 |
| --- | --- | --- |
| `output_display_gui` | SDL2 预览显示和像素格式上传 | 预览程序、GUI 测试工具 |
| `output_encode_common` | 编码前缩放和像素格式转换，优先 RGA，失败回退 swscale | 录像、推流、编码测试 |
| `output_encode_rkrga_filter` | FFmpeg RKRGA filter 封装 | `output_encode_common` |
| `output_osd_rkrga` | 用 RGA 把 RGBA OSD 叠加到视频帧 | 视频推流管线 |
| `output_recorder` | 独立录像、MP4 封装和 JPEG 抓图 | 录像测试、视频管线抓图 |
| `output_stream_udp` | H264 Annex-B 到 RTP/FU-A 分包并通过 UDP 发送 | 视频推流管线 |
| `output_stream_rtsp` | FFmpeg RTSP 发布、时间戳换算和连接收尾 | 视频推流管线 |

已删除的占位模块：

- `output_stream_rtmp`：工程没有 RTMP 调用方，也没有真实发送实现。
- `output_store_ts`：工程不使用 TS 存储，也没有真实封装实现。
- `output_store_mp4`、`output_record`：空写入链路，已由 `output_recorder` 和 `video_pipeline_record` 取代。
- `output_snapshot`：只生成 4 字节假 JPEG，已由真实 JPEG 编码取代。
- `output_encode_main`、`output_encode_sub`：只做格式转换，没有接编码器。
- `output_display_hdmi`：只打印日志，没有 DRM/KMS 输出。

协议输出实现位于 `src/output`，`src/video/video_pipeline_stream.c` 只负责编码线程、输出选择和出错后的启停控制。
