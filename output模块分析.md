# output 模块分析记录

> 分析对象：`src/output/` 与 `include/output/` 下全部 13 组模块（26 个文件）
> 分析日期：2026-07-31（只读分析，未修改代码）

【文件】output_recorder.c
【功能】单通道"录像 + 拍照"一体化模块：init 按 source_type 打开 usb / csi0 / csi1 / hdmi_in 采集源，自起采集线程 + 编码线程；采集线程读帧、单独缓存最新帧供拍照，仅在录像中才把帧推入 8 深队列（满则丢最老一帧保实时）；编码线程收到 record_start 后置状态，等第一帧到达才按实际尺寸打开编码器（默认 h264_rkmpp，缺失回落 h264_v4l2m2m）+ mp4 封装，帧先经 output_encode_common 转 NV12（RGA / swscale）再编码写包；record_stop 排空队列后 flush 编码器并写 trailer(moov)，保证文件完整可播，一帧未录则不生成文件；snapshot 同步取最新帧转 yuvj420p 用 MJPEG 编码存盘；deinit / 进程退出都会走完 trailer 收尾。存储目录自动 mkdir -p，路径传 NULL 按 record_时间戳.mp4 / snapshot_时间戳.jpg 自动命名。需要几路同时录就创建几个实例；mp4_encode_test.c 是其 CLI 外壳。
【对外接口】output_recorder_init() / record_start() / record_stop() / snapshot() / is_recording() / deinit()；独立工具 output_recorder_write_jpeg()（不依赖实例）；事件回调 output_recorder_event_cb（RECORD_STARTED / RECORD_FINISHED / RECORD_CANCELED / SNAPSHOT_SAVED / ERROR）；配置 output_recorder_config_t（input / encoder_name / bitrate / gop / storage_dir / event_cb）。
【依赖】pthread、FFmpeg（libavformat / libavcodec / libavutil）、input 四通道（input_usb / input_csi0 / input_csi1 / input_hdmi_in）、output/output_encode_common.h、common/common.h（app_status_t、video_input_config_t、APP_PATH_MAX_LEN、app_get_time_us）、common/debug.h。
【关键约束】所有 API 线程安全，可在任意线程调用；record_start / stop 非阻塞（结果经事件回调通知，回调在内部线程上下文、锁外触发，回调里可再调 API）；snapshot 同步阻塞几十~几百 ms；mp4 必须经 finalize 写 trailer 才可播（已验证直接 kill 进程不损坏录像，但推荐用 stop API 正常停止）；USB MJPEG 源同样受"必须软解"约束（在 input 层保证）。
【状态】稳定（板上实测通过：mp4 可播、JPEG 有效、中断后文件完整）

【文件】output_recorder.h
【功能】录像模块头文件：定义事件枚举 output_recorder_event_t、回调类型 output_recorder_event_cb、配置结构 output_recorder_config_t，声明 6 个实例 API + 独立工具 output_recorder_write_jpeg()；头注释含完整用法示例与线程安全 / 自动命名 / 多实例说明。
【对外接口】output_recorder_ctx_t（不透明指针）、output_recorder_config_t、output_recorder_event_t、output_recorder_event_cb；init / record_start / record_stop / snapshot / is_recording / deinit / write_jpeg。
【依赖】common/common.h。
【状态】稳定

【文件】output_encode_common.c
【功能】编码前帧归一化层：把任意来源帧转成目标尺寸 + 目标像素格式的 AVFrame。尺寸格式一致直接透传原帧；否则优先走 RGA 硬件（同尺寸 imcvtcolor、不同尺寸 imresize，缩放 + 格式转换一步完成），RGA 不可用（格式不支持 / 多平面不连续 / 运行时失败）自动回落 swscale（SWS_BILINEAR，sws_getCachedContext 缓存上下文）；RGA 运行失败一次置 rga_failed，该上下文永久回落 swscale。目标 work_frame 复用、按 64 字节对齐分配，pts 透传。被 output_recorder、output_encode_main / sub、process_common、video_pipeline 及 hw_encode_test 等多个测试工具使用。
【对外接口】output_encode_convert_init() / output_encode_prepare_frame() / output_encode_convert_deinit()；上下文 output_encode_convert_ctx_t（见 output_encode_common.h）。
【依赖】FFmpeg（libswscale / libavutil imgutils）、HAVE_LIBRGA 时 librga（RgaApi.h / im2d.h，CMake 自动探测）、common/common.h、common/debug.h。
【关键约束】RGA 要求所有平面在一块连续内存，入口显式校验、不满足走 swscale；RGA 不区分 yuvj 全范围，USB MJPEG 软解帧经此转换颜色范围按 limited 处理（与原 swscale 默认行为一致）；RGA 侧支持 NV12 / NV21 / YUV420P / NV16 / YUV422P / YUYV422 / UYVY422。
【状态】稳定（板上编译实测通过）

【文件】output_encode_common.h
【功能】帧归一化层头文件：定义 output_encode_convert_ctx_t（目标 / 源尺寸格式、sws 上下文、复用 work_frame、rga_failed 永久回落标志）并声明三个接口。
【对外接口】output_encode_convert_ctx_t；output_encode_convert_init() / output_encode_prepare_frame() / output_encode_convert_deinit()。
【依赖】libswscale、common/common.h。
【状态】稳定

【文件】output_display_gui.c
【功能】SDL2 本地预览窗口：init 建窗口与 renderer（硬件加速失败回落软件渲染），show 把一帧渲染上屏。按源格式选四条上传路径：UYVY / YUY2 直通纹理、NV12 / NV21 走 SDL_UpdateNVTexture、yuv420p（limited）走 SDL_UpdateYUVTexture（IYUV），其余（含 MJPEG 软解的全范围 yuvj420p）走 swscale 转 BGRA 并做色彩空间 / 范围校正（未标记时按分辨率猜 BT.709 / BT.601）；窗口可缩放，帧尺寸 / 格式 / 窗口大小变化时重建纹理。poll_quit 处理 SDL_QUIT 关闭事件。被 app_manager、video_pipeline、gui_smoke_test、usb_mjpeg_hw_preview 等使用。
【对外接口】output_display_gui_init() / show() / poll_quit() / deinit()；上下文 output_display_gui_ctx_t（见 output_display_gui.h）。
【依赖】SDL2、FFmpeg（libswscale / libavutil imgutils）、common/common.h、common/debug.h。
【关键约束】SDL 的 IYUV 纹理按 BT.601 limited range 渲染，全范围 yuvj420p 必须走 sws 路径保证颜色正确；enabled=0 时 init / show 为空操作（无显示环境可跑）。
【状态】稳定（主 app 单路预览在用，板上跑通）

【文件】output_display_gui.h
【功能】GUI 预览头文件：定义 output_display_gui_ctx_t（window / renderer / texture、sws 与 BGRA 缓冲、direct / nv / yuv 三条直通上传标志）并声明四个接口。
【对外接口】output_display_gui_ctx_t；output_display_gui_init() / show() / poll_quit() / deinit()。
【依赖】common/common.h。
【状态】稳定

【文件】output_display_hdmi.c
【功能】HDMI 屏显输出占位：init 只保存 connector_id 并打日志，show 只 LOGD 一帧的 source / 分辨率，不做任何真实 DRM/KMS 输出。仅被旧 app_manager 链路调用。
【对外接口】output_display_hdmi_init() / show() / deinit()；上下文 output_display_hdmi_ctx_t（见 output_display_hdmi.h）。
【依赖】common/common.h、common/debug.h。
【状态】开发中（空壳占位，DRM/KMS 直出未实现）

【文件】output_display_hdmi.h
【功能】HDMI 屏显头文件：定义 output_display_hdmi_ctx_t（仅 connector_id）并声明三个接口。
【对外接口】output_display_hdmi_ctx_t；output_display_hdmi_init() / show() / deinit()。
【依赖】common/common.h。
【状态】开发中

【文件】output_encode_main.c
【功能】主码流编码通道框架：init 保存 mode / params 并建立到 NV12 的转换上下文，push 只做帧 → 目标尺寸 NV12 转换（复用 output_encode_common）+ LOGD，没有接真正的编码器（无 avcodec_open2 / send_frame）。仅被旧 app_manager 链路调用。
【对外接口】output_encode_main_init() / push() / deinit()；上下文 output_encode_main_ctx_t（见 output_encode_main.h）。
【依赖】output/output_encode_common.h、common/common.h、common/debug.h。
【状态】开发中（编码空壳：转换已通，rkmpp 编码器未接入；实际编码由 output_recorder、video_pipeline 承担）

【文件】output_encode_main.h
【功能】主码流编码头文件：定义 output_encode_main_ctx_t（mode、params、内嵌 output_encode_convert_ctx_t）并声明三个接口。
【对外接口】output_encode_main_ctx_t；output_encode_main_init() / push() / deinit()。
【依赖】common/common.h、output/output_encode_common.h。
【状态】开发中

【文件】output_encode_sub.c
【功能】子码流编码通道框架：与 output_encode_main 相同，多一个 enabled 开关（init 传入，关闭时 push 直接返回 OK）；同样只做转换 + LOGD，未接编码器。仅被旧 app_manager 链路调用。
【对外接口】output_encode_sub_init() / push() / deinit()；上下文 output_encode_sub_ctx_t（见 output_encode_sub.h）。
【依赖】output/output_encode_common.h、common/common.h、common/debug.h。
【状态】开发中（编码空壳，同 main）

【文件】output_encode_sub.h
【功能】子码流编码头文件：定义 output_encode_sub_ctx_t（mode、params、enabled、内嵌 output_encode_convert_ctx_t）并声明三个接口。
【对外接口】output_encode_sub_ctx_t；output_encode_sub_init() / push() / deinit()。
【依赖】common/common.h、output/output_encode_common.h。
【状态】开发中

【文件】output_record.c
【功能】早期录像编排层：start 记录路径 / 编码参数并调 output_store_mp4_init，write 把 encoded_packet_t 转交 mp4 store，stop 收尾，is_running 查询状态。但底层 store 的 write 是空操作，实际不落盘；功能已被 output_recorder 完整取代。仅被旧 app_manager 链路调用。
【对外接口】output_record_init() / start() / write() / stop() / is_running() / deinit()；参数 output_record_params_t（见 output_record.h）。
【依赖】output/output_store_mp4.h、common/common.h、common/debug.h。
【状态】待重构（链路底层空壳不落盘，与 output_recorder 功能重叠，建议删除或收敛）

【文件】output_record.h
【功能】早期录像编排头文件：定义 output_record_ctx_t（path / is_recording / encode 参数 / 内嵌 mp4 store）与 output_record_params_t，声明六个接口。
【对外接口】output_record_ctx_t、output_record_params_t；output_record_init() / start() / write() / stop() / is_running() / deinit()。
【依赖】common/common.h、output/output_store_mp4.h。
【状态】待重构

【文件】output_snapshot.c
【功能】早期拍照占位实现：take 只往目标路径写 4 字节假 JPEG（FF D8 FF D9），并不真正编码当前帧；已被 output_recorder_snapshot() / output_recorder_write_jpeg() 取代。仅被旧 app_manager 链路调用。
【对外接口】output_snapshot_init() / take() / deinit()；参数 output_snapshot_params_t（见 output_snapshot.h）。
【依赖】stdio、common/common.h、common/debug.h。
【状态】待重构（假实现，产出文件不是有效图像）

【文件】output_snapshot.h
【功能】早期拍照头文件：定义 output_snapshot_ctx_t（output_dir）与 output_snapshot_params_t（path），声明三个接口。
【对外接口】output_snapshot_ctx_t、output_snapshot_params_t；output_snapshot_init() / take() / deinit()。
【依赖】common/common.h。
【状态】待重构

【文件】output_store_mp4.c
【功能】MP4 存储占位：init 仅保存路径，write 空操作直接返回 OK（无 avformat 封装、不落盘），deinit 空。仅经 output_record 被旧 app_manager 链路间接调用。
【对外接口】output_store_mp4_init() / write() / deinit()；上下文 output_store_mp4_ctx_t（见 output_store_mp4.h）。
【依赖】common/common.h。
【状态】待重构（空壳；真实 mp4 封装在 output_recorder 内）

【文件】output_store_mp4.h
【功能】MP4 存储头文件：定义 output_store_mp4_ctx_t（path）并声明三个接口。
【对外接口】output_store_mp4_ctx_t；output_store_mp4_init() / write() / deinit()。
【依赖】common/common.h。
【状态】待重构

【文件】output_store_ts.c
【功能】TS 存储占位：init 仅保存路径，write 空操作直接返回 OK，deinit 空。全工程无调用方。
【对外接口】output_store_ts_init() / write() / deinit()；上下文 output_store_ts_ctx_t（见 output_store_ts.h）。
【依赖】common/common.h。
【状态】开发中（空壳占位，未接线）

【文件】output_store_ts.h
【功能】TS 存储头文件：定义 output_store_ts_ctx_t（path）并声明三个接口。
【对外接口】output_store_ts_ctx_t；output_store_ts_init() / write() / deinit()。
【依赖】common/common.h。
【状态】开发中

【文件】output_stream_rtmp.c
【功能】RTMP 推流占位：init 仅保存 url，send 空操作直接返回 OK，deinit 空。全工程无调用方。
【对外接口】output_stream_rtmp_init() / send() / deinit()；上下文 output_stream_rtmp_ctx_t（见 output_stream_rtmp.h）。
【依赖】common/common.h。
【状态】开发中（空壳占位；当前推流由 video_pipeline 内 RTP/UDP 实现，后续规划 WebRTC）

【文件】output_stream_rtmp.h
【功能】RTMP 推流头文件：定义 output_stream_rtmp_ctx_t（url）并声明三个接口。
【对外接口】output_stream_rtmp_ctx_t；output_stream_rtmp_init() / send() / deinit()。
【依赖】common/common.h。
【状态】开发中

【文件】output_stream_rtsp.c
【功能】RTSP 推流占位：init 仅保存 url，send 空操作直接返回 OK，deinit 空。全工程无调用方。
【对外接口】output_stream_rtsp_init() / send() / deinit()；上下文 output_stream_rtsp_ctx_t（见 output_stream_rtsp.h）。
【依赖】common/common.h。
【状态】开发中（空壳占位，未接线）

【文件】output_stream_rtsp.h
【功能】RTSP 推流头文件：定义 output_stream_rtsp_ctx_t（url）并声明三个接口。
【对外接口】output_stream_rtsp_ctx_t；output_stream_rtsp_init() / send() / deinit()。
【依赖】common/common.h。
【状态】开发中

【文件】output_stream_udp.c
【功能】UDP 推流占位：init 仅保存 url，send 空操作直接返回 OK，deinit 空。全工程无调用方。
【对外接口】output_stream_udp_init() / send() / deinit()；上下文 output_stream_udp_ctx_t（见 output_stream_udp.h）。
【依赖】common/common.h。
【状态】开发中（空壳占位；实际 UDP/RTP 推流在 video_pipeline 里直接用 FFmpeg 实现）

【文件】output_stream_udp.h
【功能】UDP 推流头文件：定义 output_stream_udp_ctx_t（url）并声明三个接口。
【对外接口】output_stream_udp_ctx_t；output_stream_udp_init() / send() / deinit()。
【依赖】common/common.h。
【状态】开发中
