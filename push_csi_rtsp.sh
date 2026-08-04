#!/bin/sh
set -eu

# Push a local CSI camera node as an RTSP stream using ffmpeg.
# VLC on the PC can open: rtsp://<board-ip>:<port>/<stream_path>

DEVICE="${DEVICE:-/dev/video31}"
WIDTH="${WIDTH:-1632}"
HEIGHT="${HEIGHT:-1224}"
FPS="${FPS:-30}"
INPUT_FORMAT="${INPUT_FORMAT:-uyvy422}"
PORT="${PORT:-8554}"
STREAM_PATH="${STREAM_PATH:-live.sdp}"
BITRATE="${BITRATE:-4M}"
GOP="${GOP:-60}"
ENCODER="${ENCODER:-auto}"
RTSP_TRANSPORT="${RTSP_TRANSPORT:-tcp}"

usage() {
  cat <<EOF
Usage:
  $0 [device] [width] [height] [fps]

Examples:
  $0
  $0 /dev/video31 1632 1224 30
  DEVICE=/dev/video22 WIDTH=3840 HEIGHT=2160 FPS=30 $0

Environment overrides:
  DEVICE=/dev/video31
  WIDTH=1632
  HEIGHT=1224
  FPS=30
  INPUT_FORMAT=uyvy422
  PORT=8554
  STREAM_PATH=live.sdp
  BITRATE=4M
  GOP=60
  ENCODER=auto|h264_v4l2m2m|libx264
  RTSP_TRANSPORT=tcp|udp

Common camera nodes on this board:
  OV8858: /dev/video31  (1632x1224)
  IMX415: /dev/video22  (3840x2160)

VLC URL:
  rtsp://<board-ip>:\${PORT}/\${STREAM_PATH}
EOF
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  usage
  exit 0
fi

if [ $# -ge 1 ]; then
  DEVICE="$1"
fi
if [ $# -ge 2 ]; then
  WIDTH="$2"
fi
if [ $# -ge 3 ]; then
  HEIGHT="$3"
fi
if [ $# -ge 4 ]; then
  FPS="$4"
fi

if [ ! -e "$DEVICE" ]; then
  echo "error: camera device not found: $DEVICE" >&2
  exit 1
fi

has_encoder() {
  ffmpeg -hide_banner -encoders 2>/dev/null | grep -q "[[:space:]]$1\$"
}

pick_encoder() {
  if [ "$ENCODER" != "auto" ]; then
    echo "$ENCODER"
    return
  fi

  if has_encoder h264_v4l2m2m; then
    echo "h264_v4l2m2m"
  else
    echo "libx264"
  fi
}

VIDEO_ENCODER="$(pick_encoder)"
RTSP_URL="rtsp://0.0.0.0:${PORT}/${STREAM_PATH}"

echo "Starting RTSP stream"
echo "  device: $DEVICE"
echo "  size:   ${WIDTH}x${HEIGHT}"
echo "  fps:    $FPS"
echo "  input:  $INPUT_FORMAT"
echo "  codec:  $VIDEO_ENCODER"
echo "  url:    rtsp://<board-ip>:${PORT}/${STREAM_PATH}"
echo
echo "Open in VLC:"
echo "  rtsp://<board-ip>:${PORT}/${STREAM_PATH}"
echo

case "$VIDEO_ENCODER" in
  h264_v4l2m2m)
    exec ffmpeg \
      -hide_banner \
      -loglevel info \
      -fflags nobuffer \
      -f v4l2 \
      -framerate "$FPS" \
      -video_size "${WIDTH}x${HEIGHT}" \
      -input_format "$INPUT_FORMAT" \
      -i "$DEVICE" \
      -an \
      -pix_fmt nv12 \
      -c:v h264_v4l2m2m \
      -b:v "$BITRATE" \
      -g "$GOP" \
      -f rtsp \
      -rtsp_transport "$RTSP_TRANSPORT" \
      -rtsp_flags listen \
      "$RTSP_URL"
    ;;
  libx264)
    exec ffmpeg \
      -hide_banner \
      -loglevel info \
      -fflags nobuffer \
      -f v4l2 \
      -framerate "$FPS" \
      -video_size "${WIDTH}x${HEIGHT}" \
      -input_format "$INPUT_FORMAT" \
      -i "$DEVICE" \
      -an \
      -pix_fmt yuv420p \
      -c:v libx264 \
      -preset veryfast \
      -tune zerolatency \
      -b:v "$BITRATE" \
      -g "$GOP" \
      -f rtsp \
      -rtsp_transport "$RTSP_TRANSPORT" \
      -rtsp_flags listen \
      "$RTSP_URL"
    ;;
  *)
    echo "error: unsupported encoder: $VIDEO_ENCODER" >&2
    exit 1
    ;;
esac
