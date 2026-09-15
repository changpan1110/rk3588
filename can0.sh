#!/bin/bash
#=========================================================================
# can0 管理脚本  (LubanCat-5IOB / RK3588, rockchip_canfd)
#
# 用法: can0.sh {start|stop|restart|status}
#   start    - 配置波特率(1Mbps)并启用 can0
#   stop     - 停用 can0
#   restart  - 重启 can0
#   status   - 查看/查询 can0 状态与收发统计
#=========================================================================

IFACE=can0
BITRATE=1000000          # 波特率 1Mbps

# root 直接执行；普通用户走 sudo
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo -n"
fi

start() {
    echo "[start] 配置 $IFACE 波特率 ${BITRATE} bps ..."
    $SUDO ip link set $IFACE down 2>/dev/null
    $SUDO ip link set $IFACE type can bitrate $BITRATE || { echo "[start] 配置波特率失败"; exit 1; }
    $SUDO ip link set $IFACE up || { echo "[start] 启用 $IFACE 失败"; exit 1; }
    echo "[start] $IFACE 已启用"
    status
}

stop() {
    echo "[stop] 停用 $IFACE ..."
    $SUDO ip link set $IFACE down || { echo "[stop] 停用失败"; exit 1; }
    echo "[stop] $IFACE 已停用"
}

status() {
    echo "========== $IFACE 状态 =========="
    ip -d link show $IFACE 2>/dev/null || { echo "$IFACE 不存在"; exit 1; }
    echo
    echo "---------- 收发统计 (rx: bytes packets errs drop | tx: bytes packets errs drop) ----------"
    grep -E "^\s*${IFACE}:" /proc/net/dev | sed 's/^ *//' || true
    echo
    echo "operstate: $(cat /sys/class/net/$IFACE/operstate 2>/dev/null)"
}

case "$1" in
    start)    start ;;
    stop)     stop ;;
    restart)  stop; sleep 0.5; start ;;
    status|query|show) status ;;
    *) echo "用法: $0 {start|stop|restart|status}"; exit 1 ;;
esac
