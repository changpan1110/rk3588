# input 模块分析记录

> 分析对象：`src/input/input_common.c`、`src/input/input_usb.c`、`include/input/input_usb.h`
> 分析日期：2026-07-31（只读分析，未修改代码）

【文件】input_common.c
【功能】基于 FFmpeg 的 V4L2 采集+解码通用封装：以 video4linux2 输入格式打开设备（下发 video_size / framerate / input_format 参数），用 av_find_best_stream 定位视频流，按 codecpar 的 codec_id 自动查找软解码器并打开，send_packet/receive_frame 循环读出一帧，以及统一释放 packet / dec_ctx / fmt_ctx。
【对外接口】ffmpeg_input_open() / ffmpeg_input_read_frame() / ffmpeg_input_close()；配套上下文 ffmpeg_input_ctx_t 定义在 input_common.h，被 usb / csi0 / hdmi_in 各通道 ctx 内嵌复用。注意：这三个函数当前全工程无调用方，各输入模块只复用了结构体，读帧逻辑各自重写了一份。
【依赖】FFmpeg（libavformat / libavcodec / libavutil）、common/common.h（app_status_t、video_input_config_t）、common/debug.h（LOGE）。
【状态】待重构（函数已成死代码，逻辑与 input_usb.c 等重复；且只支持软解，无 rkmpp 硬解选择，建议各通道收敛回调此公共层或删除）

【文件】input_usb.c
【功能】USB 摄像头（UVC）采集输入通道：打开 V4L2 设备并配置分辨率/帧率/像素格式，选择解码器（默认软解，打开 INPUT_USB_ENABLE_HW_DECODER 宏可切 mjpeg_rkmpp / h264_rkmpp / hevc_rkmpp），解码读帧；若解出硬解帧（hw_frames_ctx 或 DRM_PRIME）自动 av_hwframe_transfer_data 转成软件帧；读帧成功后填充 video_frame_t（pts_us 取当前时间、source_type=VIDEO_SOURCE_USB、source_name="usb"）。被 app_manager、video_pipeline、output_recorder 及多个测试工具使用。
【对外接口】input_usb_open() / input_usb_read() / input_usb_close()，上下文 input_usb_ctx_t（见 input_usb.h）。
【关键约束】USB MJPEG 必须软解：板上 FFmpeg 的 mjpeg_rkmpp 硬解会死锁，故 INPUT_USB_ENABLE_HW_DECODER 默认注释关闭（h264_rkmpp / hevc_rkmpp 正常）；另外不调 avformat_find_stream_info，避免无信号设备永久阻塞在帧读取上。
【状态】稳定（板上已跑通采集；最近一次 LOGD→LOGT 单行刷新日志改动尚未编译验证）

【文件】input_usb.h
【功能】USB 输入通道头文件：定义 input_usb_ctx_t（保存采集配置 cfg、内嵌 ffmpeg_input_ctx_t 解码上下文、硬解转软解用的 sw_frame、is_opened 标志）并声明三个接口函数。
【对外接口】input_usb_ctx_t；input_usb_open() / input_usb_read() / input_usb_close()。
【依赖】common/common.h、input/input_common.h。
【状态】稳定
