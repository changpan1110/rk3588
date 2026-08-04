#!/bin/sh
set -eu

OUT_DIR="${1:-./rk3588_diag_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$OUT_DIR"

run() {
  name="$1"
  shift
  {
    echo "===== $name ====="
    "$@"
  } >"$OUT_DIR/$name.txt" 2>&1 || true
}

copy_if_exists() {
  src="$1"
  dst="$2"
  if [ -e "$src" ]; then
    cp -a "$src" "$OUT_DIR/$dst"
  fi
}

run basic_uname uname -a
run basic_os_release cat /etc/os-release
run basic_cmdline cat /proc/cmdline
run basic_cpuinfo cat /proc/cpuinfo
run basic_meminfo cat /proc/meminfo
run basic_mount mount
run basic_lsblk lsblk

run net_ip_addr ip addr
run net_ip_route ip route
run net_ss_tcp ss -tnlp

run dt_compatible sh -c "cat /proc/device-tree/compatible 2>/dev/null | tr '\\0' '\\n'"
run dt_model sh -c "cat /proc/device-tree/model 2>/dev/null | tr '\\0' '\\n'"

copy_if_exists /proc/device-tree proc-device-tree
copy_if_exists /boot boot

run dev_media sh -c "ls -l /dev | grep -E 'mpp|rga|video|vpu|dri|dma_heap' || true"
run lsmod cat /proc/modules
run dmesg_media sh -c "dmesg | grep -iE 'rk|mpp|rga|vpu|hantro|vepu|vdpu|vcodec|iommu|drm|hdmi|av1' | tail -n 400"

run ffmpeg_version ffmpeg -version
run ffmpeg_decoders sh -c "ffmpeg -decoders | grep -iE 'rkmpp|v4l2|h264|hevc|vp8|vp9|av1' || true"
run ffmpeg_encoders sh -c "ffmpeg -encoders | grep -iE 'rkmpp|v4l2|h264|hevc|vp8|vp9|jpeg' || true"
run ffmpeg_hwaccels ffmpeg -hwaccels

run gst_version gst-inspect-1.0 --version
run gst_rockchip sh -c "gst-inspect-1.0 | grep -iE 'rockchip|mpp|v4l2' || true"
run gst_mppvideodec gst-inspect-1.0 mppvideodec
run gst_mpph264enc gst-inspect-1.0 mpph264enc
run gst_mpph265enc gst-inspect-1.0 mpph265enc

run clk_summary cat /sys/kernel/debug/clk/clk_summary
run regulator_summary cat /sys/kernel/debug/regulator/regulator_summary
run iommu_groups sh -c "find /sys/kernel/iommu_groups -maxdepth 2 -type l 2>/dev/null || true"
run media_topology sh -c "for n in /dev/media*; do media-ctl -d \"$n\" -p; done"
run v4l2_all sh -c "for n in /dev/video*; do v4l2-ctl -d \"$n\" --all; done"

copy_if_exists /sys/kernel/debug debugfs
copy_if_exists /sys/class/devfreq sys-class-devfreq
copy_if_exists /sys/class/video4linux sys-class-video4linux
copy_if_exists /sys/class/drm sys-class-drm

tar -czf "${OUT_DIR}.tar.gz" "$OUT_DIR"
echo "done: ${OUT_DIR}.tar.gz"
