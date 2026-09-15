#!/usr/bin/env bash
#==============================================================================
# RK3588 MAVLink C 库安装脚本
#
# 功能：安装 pymavlink 工具，并从 MAVLink 源码生成 c_library_v2 C 头文件库
#
# 用法:
#   ./install_mavlink.sh         一键安装并生成
#   ./install_mavlink.sh check   只做检查，不安装
#==============================================================================
set -euo pipefail

MAVLINK_SRC=/home/cat/mavlink
MAVLINK_C_ROOT=/home/cat/c_library_v2
MAVGEN=/home/cat/.local/bin/mavgen.py
COMMON_XML="${MAVLINK_SRC}/message_definitions/v1.0/common.xml"
PIP_MIRROR="https://pypi.tuna.tsinghua.edu.cn/simple"

log()  { echo -e "\033[32m[$(date +%H:%M:%S)]\033[0m $*"; }
warn() { echo -e "\033[33m[提示]\033[0m $*"; }
err()  { echo -e "\033[31m[错误]\033[0m $*" >&2; exit 1; }

#-------------------- 检查源码 --------------------
check_source() {
    log "==> 检查 MAVLink 源码"
    if [ ! -d "$MAVLINK_SRC" ]; then
        err "源码目录不存在: $MAVLINK_SRC（请先 git clone mavlink）"
    fi
    if [ ! -f "$COMMON_XML" ]; then
        err "找不到 common.xml: $COMMON_XML"
    fi
    log "源码 OK: $MAVLINK_SRC"
}

#-------------------- 安装 pymavlink --------------------
install_pymavlink() {
    log "==> 安装 pymavlink"
    if [ -x "$MAVGEN" ]; then
        log "mavgen.py 已存在，跳过安装: $MAVGEN"
        return 0
    fi
    pip3 install --break-system-packages pymavlink -i "$PIP_MIRROR"
    if [ ! -x "$MAVGEN" ]; then
        err "pymavlink 安装后找不到 $MAVGEN"
    fi
    log "pymavlink 安装完成"
}

#-------------------- 生成 c_library_v2 --------------------
generate() {
    log "==> 生成 c_library_v2 C 库"
    rm -rf "$MAVLINK_C_ROOT"
    "$MAVGEN" \
        --lang=C \
        --wire-protocol=2.0 \
        --no-validate \
        --output="$MAVLINK_C_ROOT/mavlink" \
        "$COMMON_XML"
    log "生成完成: $MAVLINK_C_ROOT"
}

#-------------------- 验证 --------------------
verify() {
    log "==> 验证"
    local header="${MAVLINK_C_ROOT}/mavlink/common/mavlink.h"
    if [ -f "$header" ]; then
        log "OK: $header"
    else
        err "验证失败: 缺少 $header"
    fi
    log "C 库包含的 dialect:"
    ls -1 "$MAVLINK_C_ROOT" | grep -vE '\.(h|c)$' || true
}

#-------------------- 入口 --------------------
case "${1:-}" in
    check)
        check_source
        if [ -f "${MAVLINK_C_ROOT}/mavlink/common/mavlink.h" ]; then
            log "c_library_v2 已存在，无需重新生成"
        else
            warn "c_library_v2 尚未生成，请执行: $0"
        fi
        ;;
    *)
        check_source
        install_pymavlink
        generate
        verify
        log "全部完成。项目编译时 MAVLINK_C_ROOT=$MAVLINK_C_ROOT"
        ;;
esac
