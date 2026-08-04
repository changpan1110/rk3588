#!/bin/bash
# ============================================================
# RK3588 hardware decode test - HONEST edition
#
# Verified facts on lubancat-5io, kernel 6.1.118 (2026-07-31):
#   - ffmpeg mjpeg_rkmpp DECODER hangs on ANY input (build bug).
#     MJPEG hw decode works via GStreamer mppjpegdec instead.
#   - h264_rkmpp / hevc_rkmpp decode (ffmpeg) works fine.
#   - mjpeg/h264/hevc rkmpp ENCODE works fine.
# PASS here requires: real ffmpeg/gst exit code 0 AND frames > 0.
# Usage:  bash hw_decode_test.sh
# ============================================================

TMP=/tmp/hw_decode_test
mkdir -p "$TMP"
PASS=0; FAIL=0

echo "=========================================="
echo " [0] VPU occupancy check (/dev/mpp_service)"
echo "=========================================="
if sudo fuser /dev/mpp_service 2>/dev/null; then
  echo "!! VPU is BUSY, holders:"
  sudo fuser -v /dev/mpp_service 2>&1
  echo "   cleanup: sudo fuser -k /dev/mpp_service"
else
  echo "OK: VPU is free"
fi
STOPPED=$(ps -eo pid,stat,etime,cmd | grep -E "^ *[0-9]+ +T" | grep -E "ffmpeg|gst" | grep -v grep)
if [ -n "$STOPPED" ]; then
  echo "!! DANGER: stopped (T-state) media processes:"
  echo "$STOPPED"
  echo "   cleanup: kill -9 <PID>"
else
  echo "OK: no stopped media processes"
fi
echo

echo "=========================================="
echo " [1] Generate test clips"
echo "=========================================="
# MJPEG clip: yuvj420p (the sampling that hw decoder accepts)
timeout -k 5 30 ffmpeg -nostdin -hide_banner -loglevel error \
  -f lavfi -i testsrc=size=1280x720:rate=30 -frames:v 60 \
  -pix_fmt yuvj420p -c:v mjpeg -q:v 3 -y "$TMP/test_mjpeg.avi" \
  && echo "OK: test_mjpeg.avi (yuvj420p, sw encode)" || echo "FAIL: mjpeg clip gen"

timeout -k 5 30 ffmpeg -nostdin -hide_banner -loglevel error \
  -f lavfi -i testsrc=size=1280x720:rate=30 -frames:v 90 \
  -pix_fmt nv12 -c:v h264_rkmpp -y "$TMP/test_h264.mp4" \
  && echo "OK: test_h264.mp4 (h264_rkmpp hw encode)" || echo "FAIL: h264 clip gen"

timeout -k 5 30 ffmpeg -nostdin -hide_banner -loglevel error \
  -f lavfi -i testsrc=size=1280x720:rate=30 -frames:v 90 \
  -pix_fmt nv12 -c:v hevc_rkmpp -y "$TMP/test_hevc.mp4" \
  && echo "OK: test_hevc.mp4 (hevc_rkmpp hw encode)" || echo "FAIL: hevc clip gen"
echo

# ffmpeg decode test with REAL exit code and frame-count verification
ff_dec_test() {
  name="$1"; codec="$2"; file="$3"
  echo "------------------------------------------"
  echo " [TEST] $name (ffmpeg ${codec}_rkmpp)"
  echo "------------------------------------------"
  timeout -k 5 40 ffmpeg -nostdin -hide_banner -benchmark -stats \
    -c:v "${codec}_rkmpp" -i "$file" -f null - > "$TMP/log.txt" 2>&1
  rc=$?
  frames=$(grep -oE "frame= *[0-9]+" "$TMP/log.txt" | tail -1 | grep -oE "[0-9]+")
  grep -E "Stream #0:0.*Video|frame=|bench:" "$TMP/log.txt" | tail -3
  if [ "$rc" = "0" ] && [ -n "$frames" ] && [ "$frames" -gt 0 ]; then
    echo "RESULT: $name PASS ($frames frames decoded)"
    PASS=$((PASS+1))
  else
    echo "RESULT: $name FAIL (rc=$rc frames=${frames:-0}; rc=137 means HANG->timeout kill)"
    FAIL=$((FAIL+1))
  fi
  echo
}

echo "=========================================="
echo " [2] MJPEG hw decode via GStreamer mppjpegdec"
echo "     (ffmpeg mjpeg_rkmpp hangs - known bug)"
echo "=========================================="
timeout -k 5 40 gst-launch-1.0 filesrc location="$TMP/test_mjpeg.avi" ! \
  avidemux ! jpegparse ! mppjpegdec ! fakesink sync=false > "$TMP/gstlog.txt" 2>&1
rc=$?
if [ "$rc" = "0" ]; then
  echo "RESULT: MJPEG 720p30 hw decode PASS (gstreamer mppjpegdec)"
  PASS=$((PASS+1))
else
  tail -4 "$TMP/gstlog.txt"
  echo "RESULT: MJPEG hw decode FAIL (rc=$rc; rc=137 means HANG)"
  FAIL=$((FAIL+1))
fi
echo

echo "=========================================="
echo " [3] H264 / HEVC hw decode via ffmpeg rkmpp"
echo "=========================================="
[ -f "$TMP/test_h264.mp4" ] && ff_dec_test "H264 720p30" h264 "$TMP/test_h264.mp4"
[ -f "$TMP/test_hevc.mp4" ] && ff_dec_test "HEVC 720p30" hevc "$TMP/test_hevc.mp4"

echo "=========================================="
echo " [4] reference: MJPEG software decode speed"
echo "=========================================="
timeout -k 5 40 ffmpeg -nostdin -hide_banner -benchmark -stats \
  -c:v mjpeg -i "$TMP/test_mjpeg.avi" -f null - 2>&1 | grep -E "frame=|bench:" | tail -2
echo

echo "=========================================="
echo " SUMMARY: PASS=$PASS FAIL=$FAIL"
echo "=========================================="
if sudo fuser /dev/mpp_service 2>/dev/null; then
  echo "!! VPU still held, run: sudo fuser -k /dev/mpp_service"
else
  echo "OK: VPU released"
fi
