cat > ~/rk3588/CODEX1.md << 'EOF'
# RK3588 视频采集系统 - 项目记忆

## 技术栈
- 芯片：RK3588
- 系统：Debian/Ubuntu
- 语言：C11
- 库：FFmpeg、SDL2、librga（RGA 硬件缩放）
- 编译：CMake，build_codex/ 目录

## 系统目标
- HDMI 4K 分两路：4K 录 MP4 + 1080p30 推流
- CSI/USB 录 1080p，录像推流共用一路编码
- 3 路同步录像/拍照，路径可配

## 关键决策（不可违背）
1. USB MJPEG 必须软解（硬解 mjpeg_rkmpp 会死锁 等排查后在改）
2. h264_rkmpp / hevc_rkmpp 硬解正常
3. OSD 十字准星只在推流、不进录像(后面需要修改样式)
4. OSD 参数以后从串口来（vp_osd_update 已留接口）
4. OSD  需要添加一个 激光测距得数值  这个数字来自 串口得数据 预留接口 位置 字体 颜色 开关 都是可以设置呢
5. 推流先用 UDP/RTP，后续换 WebRTC
6. 协作流程：本地改码 → scp → 用户编译 → 贴结果

## 已知问题
- vp_deinit 在设备无信号时 capture 线程 join 会阻塞
- HDMI-IN 换信号源/热插拔需处理 DV timings（未做）
- CSI 路（/dev/video22）暂不可用（板上有挂起进程）

## 编译命令
cd /home/cat/rk3588/build_codex && cmake .. && make -j8

## 测试命令
./rk3588_video_pipeline_main
播放：vlc rtp://@:5000 或打开 play_stream.sdp
EOF
