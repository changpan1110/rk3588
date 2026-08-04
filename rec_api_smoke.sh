#!/bin/bash
cd /home/cat/rk3588/build_codex
rm -rf /tmp/rec_test
{
  printf 'record\n'; sleep 5
  printf 'stop\n'; sleep 1
  printf 'snapshot\n'; sleep 1
  printf 'quit\n'
} | ./rk3588_mp4_encode_test usb /dev/video41 mjpeg 1280 720 30 /tmp/rec_test 2>&1 | grep -vE 'DEBUG' | tail -30
echo "=== files ==="
ls -lh /tmp/rec_test/
