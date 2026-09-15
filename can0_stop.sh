#!/bin/sh

set -eu

CAN_INTERFACE="${CAN_INTERFACE:-can0}"
SERVICE_NAME="rk3588-can0.service"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)"
STOP_SCRIPT="${SCRIPT_DIR}/can0_stop.sh"

if [ "$(id -u)" -ne 0 ]; then
    exec sudo sh "${STOP_SCRIPT}" "$@"
fi

if [ "${1:-}" != "--service" ] && command -v systemctl >/dev/null 2>&1 &&
   [ -f "/etc/systemd/system/${SERVICE_NAME}" ]; then
    systemctl stop "${SERVICE_NAME}"
    echo "${SERVICE_NAME} stopped"
    exit 0
fi

if [ ! -e "/sys/class/net/${CAN_INTERFACE}" ]; then
    echo "${CAN_INTERFACE} is not present; nothing to stop"
    exit 0
fi

command -v ip >/dev/null 2>&1 || {
    echo "ip command not found" >&2
    exit 1
}

ip link set "${CAN_INTERFACE}" down
echo "${CAN_INTERFACE} stopped"
