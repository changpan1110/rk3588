echo "===== 1. ubuntuEnv.txt (overlay support?) ====="
cat /boot/firmware/ubuntuEnv.txt 2>/dev/null
echo
echo "===== 2. media1 sensor check ====="
media-ctl -p -d /dev/media1 2>/dev/null | grep -iE "sensor|imx|ov[0-9]|tc358|lt691" || echo "(no sensor on media1/mipi1)"
echo
echo "===== 3. native HDMI RX current signal ====="
v4l2-ctl -d /dev/video40 --get-dv-timings 2>&1 | head -6
v4l2-ctl -d /dev/video40 --query-dv-timings 2>&1 | head -8
echo
echo "===== 4. board internet check ====="
timeout 6 curl -sI -o /dev/null -w "%{http_code}" https://mirrors.tuna.tsinghua.edu.cn 2>&1 || echo " NO-INTERNET"
echo
echo "===== 5. gpio for hdmirx detect (plug status) ====="
v4l2-ctl -d /dev/video40 --all 2>/dev/null | grep -iE "input|status|detect" | head -6
echo
echo "===== SURVEY3 DONE ====="
