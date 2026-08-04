echo "===== 1. kernel headers / build tree ====="
ls -l /lib/modules/$(uname -r)/ 2>/dev/null | grep -E "build|source"
ls -d /usr/src/* 2>/dev/null | head -5
echo
echo "===== 2. build tools on board ====="
which gcc make dtc 2>/dev/null || echo "(no build tools)"
echo
echo "===== 3. boot config (uEnv / extlinux) ====="
ls /boot/ 2>/dev/null
echo "--- uEnv ---"
cat /boot/uEnv/uEnv.txt 2>/dev/null || cat /boot/uEnv.txt 2>/dev/null || echo "(no uEnv)"
echo "--- extlinux ---"
cat /boot/extlinux/extlinux.conf 2>/dev/null | head -20
echo
echo "===== 4. available dtb overlays ====="
ls /boot/dtb/overlays/ 2>/dev/null | head -30 || find /boot -iname "*overlay*" -o -iname "*.dtbo" 2>/dev/null | head -30
echo
echo "===== 5. current CSI0 topology (what sensor is on mipi0) ====="
media-ctl -p -d /dev/media0 2>/dev/null | grep -E "entity|type|link" | head -25
echo
echo "===== 6. i2c buses ====="
i2cdetect -l 2>/dev/null
echo
echo "===== 7. native HDMI RX status ====="
dmesg | grep -iE "hdmirx" | head -10
v4l2-ctl -d /dev/video40 --all 2>/dev/null | grep -E "Driver name|Card type|Bus info|Status|Timings" | head -8
echo
echo "===== 8. hdmirx in device tree ====="
find /sys/firmware/devicetree/base -maxdepth 2 -iname "*hdmirx*" 2>/dev/null
cat /sys/firmware/devicetree/base/*/hdmirx*/status 2>/dev/null; echo
echo
echo "===== SURVEY DONE ====="
