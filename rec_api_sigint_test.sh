#!/bin/bash
cd /home/cat/rk3588/build_codex
echo "=== ffprobe record mp4 ==="
ffprobe -v error -show_entries format=duration,size -show_entries stream=codec_name,width,height,nb_frames /tmp/rec_test/record_*.mp4
echo "=== snapshot jpeg check ==="
xxd -l 4 /tmp/rec_test/snapshot_*.jpg
echo "=== SIGINT mid-record test ==="
rm -rf /tmp/rec_sigint
{ printf 'record\n'; sleep 3; } | ./rk3588_mp4_encode_test usb /dev/video41 mjpeg 1280 720 30 /tmp/rec_sigint 2>/dev/null &
PID=$!
sleep 4
kill -INT $(pgrep -f 'rk3588_mp4_encode_test usb' | head -1)
wait $PID 2>/dev/null
ls -lh /tmp/rec_sigint/ 2>/dev/null
ffprobe -v error -show_entries format=duration /tmp/rec_sigint/record_*.mp4 2>&1
