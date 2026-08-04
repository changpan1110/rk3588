#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="mediamtx"
RUN_USER="${SUDO_USER:-$USER}"
RUN_HOME="$(getent passwd "$RUN_USER" | cut -d: -f6)"
MEDIAMTX_BIN="${MEDIAMTX_BIN:-$RUN_HOME/mediamtx}"
MEDIAMTX_CONF="${MEDIAMTX_CONF:-$RUN_HOME/mediamtx.yml}"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run with sudo:"
    echo "  sudo bash $0"
    exit 1
fi

if [ ! -x "$MEDIAMTX_BIN" ]; then
    echo "MediaMTX binary not found or not executable: $MEDIAMTX_BIN"
    echo "You can override it with:"
    echo "  sudo MEDIAMTX_BIN=/path/to/mediamtx MEDIAMTX_CONF=/path/to/mediamtx.yml bash $0"
    exit 1
fi

if [ ! -f "$MEDIAMTX_CONF" ]; then
    echo "MediaMTX config not found: $MEDIAMTX_CONF"
    echo "You can override it with:"
    echo "  sudo MEDIAMTX_BIN=/path/to/mediamtx MEDIAMTX_CONF=/path/to/mediamtx.yml bash $0"
    exit 1
fi

cat > "$SERVICE_FILE" <<EOF
[Unit]
Description=MediaMTX RTSP/WebRTC/HLS server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$RUN_USER
WorkingDirectory=$RUN_HOME
ExecStart=$MEDIAMTX_BIN $MEDIAMTX_CONF
Restart=always
RestartSec=2
LimitNOFILE=1048576

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable "$SERVICE_NAME"
systemctl restart "$SERVICE_NAME"

echo "MediaMTX service installed and started."
echo
echo "Useful commands:"
echo "  sudo systemctl status $SERVICE_NAME"
echo "  sudo journalctl -u $SERVICE_NAME -f"
echo "  sudo systemctl restart $SERVICE_NAME"
echo "  sudo systemctl stop $SERVICE_NAME"
