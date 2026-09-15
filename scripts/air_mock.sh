#!/usr/bin/env bash
set -euo pipefail

ACTION="${1:-help}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
MAVLINK_DIR="${MAVLINK_DIR:-$SCRIPT_DIR}"
CONFIG_FILE="${AIR_CONFIG_FILE:-$MAVLINK_DIR/air_mavlink.conf}"
if [[ -f "$CONFIG_FILE" ]]; then
    # shellcheck source=/dev/null
    source "$CONFIG_FILE"
fi
PYTHON_BIN="${MAVLINK_PYTHON:-$MAVLINK_DIR/.venv/bin/python}"
MOCK_FILE="${AIR_MOCK_FILE:-$MAVLINK_DIR/air_mock.py}"
PID_FILE="$MAVLINK_DIR/air_mock.pid"
LOG_FILE="$MAVLINK_DIR/air_mock.log"
LISTEN_ENDPOINT="${MAVLINK_LISTEN:-udpin:0.0.0.0:14550}"
MAVLINK_BAUD="${MAVLINK_BAUD:-115200}"
MAVLINK_SYSTEM_ID="${MAVLINK_SYSTEM_ID:-1}"
MAVLINK_COMPONENT_ID="${MAVLINK_COMPONENT_ID:-191}"

usage() {
    cat <<EOF
RK3588 airborne MAVLink mock manager

Usage:
  ./air_mock.sh start       Start receiver in background
  ./air_mock.sh restart     Restart after changing configuration
  ./air_mock.sh foreground  Start receiver in foreground
  ./air_mock.sh stop        Stop background receiver
  ./air_mock.sh status      Show receiver status
  ./air_mock.sh logs        Follow receiver log
  ./air_mock.sh config      Print active configuration

Required files:
  $PYTHON_BIN
  $MOCK_FILE
EOF
}

if [[ "$EUID" -eq 0 && "${ALLOW_ROOT:-0}" != "1" ]]; then
    echo "Do not use sudo. Run as the normal user: ./air_mock.sh $ACTION" >&2
    exit 1
fi

check_files() {
    if [[ ! -x "$PYTHON_BIN" ]]; then
        echo "Python virtual environment not found: $PYTHON_BIN" >&2
        exit 1
    fi
    if [[ ! -f "$MOCK_FILE" ]]; then
        echo "Mock receiver not found: $MOCK_FILE" >&2
        exit 1
    fi
}

is_running() {
    [[ -f "$PID_FILE" ]] || return 1
    local pid
    pid="$(cat "$PID_FILE")"
    [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null
}

start_background() {
    check_files
    if is_running; then
        echo "Air mock is already running (PID $(cat "$PID_FILE"))."
        return
    fi
    nohup "$PYTHON_BIN" "$MOCK_FILE" \
        --listen "$LISTEN_ENDPOINT" \
        --baud "$MAVLINK_BAUD" \
        --system "$MAVLINK_SYSTEM_ID" \
        --component "$MAVLINK_COMPONENT_ID" \
        >"$LOG_FILE" 2>&1 &
    echo $! >"$PID_FILE"
    sleep 1
    if is_running; then
        echo "Air mock started (PID $(cat "$PID_FILE"))."
        echo "Log: $LOG_FILE"
    else
        echo "Air mock failed to start. Check $LOG_FILE" >&2
        exit 1
    fi
}

stop_background() {
    if ! is_running; then
        rm -f "$PID_FILE"
        echo "Air mock is not running."
        return
    fi
    local pid
    pid="$(cat "$PID_FILE")"
    kill "$pid"
    rm -f "$PID_FILE"
    echo "Air mock stopped."
}

case "$ACTION" in
    start)
        start_background
        ;;
    restart)
        stop_background
        start_background
        ;;
    foreground)
        check_files
        exec "$PYTHON_BIN" "$MOCK_FILE" \
            --listen "$LISTEN_ENDPOINT" \
            --baud "$MAVLINK_BAUD" \
            --system "$MAVLINK_SYSTEM_ID" \
            --component "$MAVLINK_COMPONENT_ID"
        ;;
    stop)
        stop_background
        ;;
    status)
        if is_running; then
            echo "Air mock is running (PID $(cat "$PID_FILE"))."
        else
            echo "Air mock is stopped."
            exit 1
        fi
        ;;
    logs)
        touch "$LOG_FILE"
        tail -f "$LOG_FILE"
        ;;
    config)
        echo "Config: $CONFIG_FILE"
        echo "Endpoint: $LISTEN_ENDPOINT"
        echo "Baud: $MAVLINK_BAUD"
        echo "System/component: $MAVLINK_SYSTEM_ID/$MAVLINK_COMPONENT_ID"
        ;;
    help|-h|--help)
        usage
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
