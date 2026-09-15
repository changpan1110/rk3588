#!/bin/sh

set -eu

CAN_INTERFACE="${CAN_INTERFACE:-can0}"
CAN_BITRATE="${CAN_BITRATE:-500000}"
CAN_RESTART_MS="${CAN_RESTART_MS:-100}"
SERVICE_NAME="rk3588-can0.service"
UNIT_FILE="/etc/systemd/system/${SERVICE_NAME}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)"
START_SCRIPT="${SCRIPT_DIR}/can0_start.sh"
STOP_SCRIPT="${SCRIPT_DIR}/can0_stop.sh"

if [ "$(id -u)" -ne 0 ]; then
    exec sudo sh "${START_SCRIPT}" "$@"
fi

write_systemd_unit() {
    command -v systemctl >/dev/null 2>&1 || {
        echo "systemctl command not found" >&2
        exit 1
    }

    cat >"${UNIT_FILE}" <<EOF
[Unit]
Description=Configure RK3588 CAN0 interface
After=systemd-modules-load.service
Before=network.target

[Service]
Type=oneshot
RemainAfterExit=yes
TimeoutStartSec=15
Environment=CAN_INTERFACE=${CAN_INTERFACE}
Environment=CAN_BITRATE=${CAN_BITRATE}
Environment=CAN_RESTART_MS=${CAN_RESTART_MS}
ExecStart=${START_SCRIPT} --service
ExecStop=${STOP_SCRIPT} --service

[Install]
WantedBy=multi-user.target
EOF
}

if [ "${1:-}" != "--service" ]; then
    chmod +x "${START_SCRIPT}" "${STOP_SCRIPT}"
    write_systemd_unit
    systemctl daemon-reload
    systemctl enable "${SERVICE_NAME}"
    systemctl restart "${SERVICE_NAME}"
    echo "${SERVICE_NAME} installed, enabled, and started"
    systemctl --no-pager --full status "${SERVICE_NAME}" || true
    exit 0
fi

command -v ip >/dev/null 2>&1 || {
    echo "ip command not found" >&2
    exit 1
}

attempt=0
while [ ! -e "/sys/class/net/${CAN_INTERFACE}" ]; do
    if [ "${attempt}" -ge 50 ]; then
        echo "CAN interface did not appear: ${CAN_INTERFACE}" >&2
        exit 1
    fi
    sleep 0.1
    attempt=$((attempt + 1))
done

ip link set "${CAN_INTERFACE}" down || true
ip link set "${CAN_INTERFACE}" type can \
    bitrate "${CAN_BITRATE}" restart-ms "${CAN_RESTART_MS}"
ip link set "${CAN_INTERFACE}" up

echo "${CAN_INTERFACE} started: bitrate=${CAN_BITRATE}, restart-ms=${CAN_RESTART_MS}"
ip -details link show "${CAN_INTERFACE}"
