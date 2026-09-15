#!/bin/bash
# =============================================================
#  网口 IP 切换脚本 (DHCP / 静态)  -  一次性命令行, 无交互提示
#
#  用法:
#    sudo ./set_ip.sh dhcp [网卡]
#    sudo ./set_ip.sh static [网卡] [IP] [前缀] [网关] [DNS]
#
#  示例:
#    sudo ./set_ip.sh static eth1                    # eth1 用默认IP(不弹提示)
#    sudo ./set_ip.sh static eth1 192.168.1.100      # eth1 指定IP
#    sudo ./set_ip.sh static eth1 192.168.1.100 24 192.168.1.1 "223.5.5.5 114.114.114.114"
#    sudo ./set_ip.sh dhcp eth1
#    sudo ./set_ip.sh dhcp                           # 默认 eth0
# =============================================================
set -u

if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$@"
fi

# ================= 默认配置(可修改) =================
DEFAULT_IFACE="eth0"
DEFAULT_IP="192.168.31.201"
DEFAULT_PREFIX="24"
DEFAULT_GATEWAY="192.168.31.1"
DEFAULT_DNS="192.168.31.1"
# ====================================================

MODE="${1:-}"
IFACE="${2:-${DEFAULT_IFACE}}"
# 没给 IP 就直接用脚本开头默认 IP(一次性, 不提示)
IP="${3:-${DEFAULT_IP}}"
PREFIX="${4:-${DEFAULT_PREFIX}}"
GW="${5:-${DEFAULT_GATEWAY}}"
DNS="${6:-${DEFAULT_DNS}}"

log() { echo "[$(date '+%H:%M:%S')] $*"; }

get_con() {
    local con
    con="$(nmcli -t -f NAME,DEVICE con show 2>/dev/null | grep ":${IFACE}$" | head -1 | cut -d: -f1)"
    if [ -z "$con" ]; then
        while IFS= read -r name; do
            [ -z "$name" ] && continue
            if [ "$(nmcli -g connection.interface-name con show "$name" 2>/dev/null)" = "$IFACE" ]; then
                con="$name"
                break
            fi
        done < <(nmcli -t -f NAME con show 2>/dev/null)
    fi
    echo "$con"
}

ensure_con() {
    local con="$(get_con)"
    if [ -z "$con" ]; then
        con="iface-${IFACE}"
        echo "[$(date '+%H:%M:%S')] 网卡 ${IFACE} 无连接配置, 自动创建 ${con}" >&2
        if ! nmcli con add type ethernet ifname "${IFACE}" con-name "${con}" >/dev/null 2>&1; then
            echo "[FAIL] 创建 ${IFACE} 连接失败" >&2
            exit 1
        fi
    fi
    echo "$con"
}

set_dhcp() {
    local con="$(ensure_con)"
    log "切换网卡 ${IFACE} 为 DHCP ..."
    nmcli con modify "$con" ipv4.method auto ipv4.addresses "" ipv4.gateway "" ipv4.dns ""
    nmcli con up "$con" >/dev/null 2>&1
    sleep 2
    log "[OK] 完成:"
    ip -4 addr show "$IFACE" 2>/dev/null | grep "inet " || log "  (无地址/网卡未连接)"
}

set_static() {
    local ip="$1" prefix="$2" gw="$3" dns="$4"
    local con="$(ensure_con)"
    log "切换网卡 ${IFACE} 为静态: ${ip}/${prefix} 网关=${gw} DNS=${dns}"
    nmcli con modify "$con" ipv4.method manual ipv4.addresses "${ip}/${prefix}" ipv4.gateway "$gw" ipv4.dns "$dns"
    nmcli con up "$con" >/dev/null 2>&1
    sleep 2
    log "[OK] 完成:"
    ip -4 addr show "$IFACE" 2>/dev/null | grep "inet " || log "  (无地址/网卡未连接)"
}

case "$MODE" in
    dhcp)   set_dhcp ;;
    static) set_static "$IP" "$PREFIX" "$GW" "$DNS" ;;
    *)
        echo "用法(一次性, 无交互):"
        echo "  $0 dhcp [网卡]"
        echo "  $0 static [网卡] [IP] [前缀] [网关] [DNS]"
        echo ""
        echo "示例:"
        echo "  $0 static eth1                    # eth1 用默认 IP ${DEFAULT_IP}"
        echo "  $0 static eth1 192.168.1.100 24 192.168.1.1 \"223.5.5.5 114.114.114.114\""
        echo "  $0 dhcp eth1"
        exit 1
        ;;
esac
