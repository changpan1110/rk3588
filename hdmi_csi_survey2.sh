echo "===== 1. board model ====="
cat /proc/device-tree/compatible 2>/dev/null | tr '\0' '\n'
echo
echo "===== 2. where is the dtb ====="
ls /boot/firmware/ 2>/dev/null | head -10
find /boot /usr/lib -maxdepth 4 -name "*.dtb" 2>/dev/null | head -10
echo "--- cmdline ---"
cat /proc/cmdline
echo
echo "===== 3. full CSI0 topology (sensor entities) ====="
media-ctl -p -d /dev/media0 2>/dev/null | grep -B1 -A3 "subdev" | head -40
echo
echo "===== 4. full CSI1 topology ====="
media-ctl -p -d /dev/media1 2>/dev/null | grep -E "entity|subdev" | head -15
echo
echo "===== 5. sensor probes in dmesg ====="
dmesg | grep -iE "ov[0-9]{4}|imx[0-9]{3}|os[0-9]{4}|sc[0-9]{3}|gc[0-9]{4}|sensor" | head -10
echo
echo "===== 6. i2c devices live scan (camera buses) ====="
for b in 1 2 5 7; do echo "--- i2c-$b ---"; i2cdetect -y -r $b 2>/dev/null | tail -n +2; done
echo
echo "===== 7. kernel config: what we need for module build ====="
grep -E "^CONFIG_MODVERSIONS|^CONFIG_VIDEO_V4L2_SUBDEV_API|^CONFIG_MEDIA_SUPPORT|^CONFIG_VIDEO_V4L2=|^CONFIG_I2C=" /boot/config-6.1.118
echo "--- headers tree completeness ---"
ls /usr/src/linux-headers-6.1.118/ | head -15
test -f /usr/src/linux-headers-6.1.118/Module.symvers && echo "Module.symvers: PRESENT" || echo "Module.symvers: MISSING"
echo
echo "===== SURVEY2 DONE ====="
