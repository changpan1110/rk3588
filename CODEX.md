文件状态已确认，以下是当前会话的完整总结：
【已完成】
全面代码 review：确认主 app 只是单路预览器，原编码/存储/推流是空壳
RGA 硬件缩放（替换 CPU swscale):src/output/output_encode_common.c 用 imresize/imcvtcolor，失败自动回落 swscale;CMake 自动探测 HAVE_LIBRGA。板上编译+实测通过
录像/停止/拍照 API:include/output/output_recorder.h + src/output/output_recorder.c，线程安全的 record_start/stop/snapshot，事件回调，存储路径可自定义并自动建目录；mp4_encode_test.c 是 CLI 外壳。板上实测通过（mp4 可播、JPEG 有效、kill 中断后文件完整——已验证直接杀进程不会损坏录像，但推荐用 stop API 正常停止）
多路管线:include/video/video_pipeline.h + src/video/video_pipeline.c:按运行时 channel_count 创建采集线程 + 按需录像线程 + 常驻推流线程（1080p RGA 缩放 + 十字 OSD 仅推流 + RTP/UDP)，切流时清队列+强制 IDR 实现无缝切换；主入口 src/video/video_pipeline_main.c（命令 record/stop/snap/switch/osd/status/quit)。默认配置 HDMI+CSI+USB，已在板上跑通采集
修复启动卡死：去掉 4 个 input 文件的 avformat_find_stream_info（无信号设备会永久阻塞，这就是之前 csi0 那里"卡死"的原因）
单行刷新日志（已改完，未编译验证）:input_usb.c 每帧日志 LOGD→LOGT;video_pipeline.h/c 新增 vp_channel_stats_t/vp_get_channel_stats/vp_get_stream_frames;video_pipeline_main.c 加状态线程，每秒用 \r\033[K 重写同一行状态
【当前任务】
单行刷新日志的 4 个改动文件还没发给用户上传，scp 命令被打断未发出
【相关文件】
待上传（4 个）:F:\rk3588\src\input\input_usb.c、F:\rk3588\include\video\video_pipeline.h、F:\rk3588\src\video\video_pipeline.c、F:\rk3588\src\video\video_pipeline_main.c
板上对应路径：/home/cat/rk3588/ 下相同目录结构，编译目录用 build_codex（旧 build/ 是 root 属主，勿用）
SSH:ssh -i "$env:USERPROFILE\.ssh\rk14_key" -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null cat@192.168.31.14
播放测试：play_stream.sdp(VLC 打开）或 vlc rtp://@:5000
【关键决策/约束】
系统目标：HDMI 4K 分两路（4K 录 MP4 + 1080p30 推流）;CSI/USB 录 1080p，录像推流共用一路编码；3 路同步录像/拍照，路径可配
OSD 十字准星只在推流、不进录像，参数以后从串口来（vp_osd_update 已留接口）
推流先用 UDP/RTP，后续换 WebRTC（板上 ~/mediamtx 已装好）
板上 FFmpeg 的 mjpeg_rkmpp 硬解会死锁，USB MJPEG 必须软解;h264_rkmpp/hevc_rkmpp 正常
协作流程：本地改码 → 给用户 scp 命令 → 用户自己拷贝编译贴结果，解决不了才远程
本地文件 UTF-8 写中文注释，PowerShell 终端显示乱码是终端问题，文件本身正常
【下一步】
立即给用户 4 个文件的 scp 上传命令 + 板上 cd /home/cat/rk3588 && cmake --build build_codex -j8 编译
运行 ./rk3588_video_pipeline_main，推流协议和地址从 g_pipeline_default_config.stream_output 读取，验证单行状态刷新（形如 hdmi 30fps REC | csi 0fps - | usb 27fps - | stream=usb enc=1234)
后续：OSD 数值文本叠加（需小字体）+ 串口线程对接 vp_osd_update（可复用 uart_base.c/uart_laser.c)；推流换 WebRTC
【遇到的问题】
CSI 路（/dev/video22）暂不可用：板上有约 22 个挂起的 root 属主 rk3588 测试进程，需 sudo kill -9 $(ps -eo pid,stat,cmd | grep ' T ' | grep rk3588 | awk '{print $1}') 或重启
已知自查点：vp_deinit 在设备无信号时 capture 线程 join 会阻塞（用户已知晓）;HDMI-IN 换信号源/热插拔需处理 DV timings（未做）
