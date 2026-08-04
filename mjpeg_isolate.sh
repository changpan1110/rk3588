kill -9 69903 69906 2>/dev/null
pkill -9 -f "mjpeg_rkmpp" 2>/dev/null
sleep 1
echo "=== dmesg mpp/vpu errors ==="
dmesg | tail -30 | grep -iE "mpp|vpu|vdpu|rkvdec|jpeg" || echo "(no recent mpp errors)"
echo
echo "=== TEST 1: generate known-good MJPEG (sw encode) ==="
timeout -k 5 25 ffmpeg -nostdin -hide_banner -loglevel error -f lavfi -i testsrc=size=1280x720:rate=30 -frames:v 30 -c:v mjpeg -q:v 3 -y /tmp/synth.avi && echo "gen OK"
ls -l /tmp/synth.avi
echo
echo "=== TEST 2: rkmpp hw decode of known-good MJPEG ==="
timeout -k 5 20 ffmpeg -nostdin -hide_banner -loglevel info -c:v mjpeg_rkmpp -i /tmp/synth.avi -f null - 2>&1 | grep -E "mjpeg_rkmpp|frame=|Error|error" | head -6
echo "test2_rc=$?"
echo
echo "=== TEST 3: software decode of UVC capture (sanity) ==="
timeout -k 5 20 ffmpeg -nostdin -hide_banner -loglevel error -c:v mjpeg -i /tmp/mjpeg_test.avi -frames:v 10 -f null - && echo "sw decode OK"
echo
echo "=== TEST 4: rkmpp hw decode of UVC capture ==="
timeout -k 5 20 ffmpeg -nostdin -hide_banner -loglevel info -c:v mjpeg_rkmpp -i /tmp/mjpeg_test.avi -frames:v 10 -f null - 2>&1 | grep -E "mjpeg_rkmpp|frame=|Error|error" | head -6
echo "test4_rc=$?"
echo
echo "=== TEST 5: live camera MJPEG -> rkmpp hw decode (the target pipeline) ==="
timeout -k 5 30 ffmpeg -nostdin -hide_banner -loglevel info -benchmark -c:v mjpeg_rkmpp -f v4l2 -input_format mjpeg -video_size 1280x720 -framerate 30 -i /dev/video41 -t 3 -f null - 2>&1 | grep -E "mjpeg_rkmpp|Stream|frame=|Error|error|bench" | head -10
echo "test5_rc=$?"
echo
echo "=== ISOLATION DONE ==="
