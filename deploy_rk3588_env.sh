#!/usr/bin/env bash
#==============================================================================
# RK3588 视频环境一键部署脚本（新板子）
#
# 用法:
#   ./deploy_rk3588_env.sh all /path/to/rk3588_env.tar.gz   一键全部部署
#   ./deploy_rk3588_env.sh deps                             只装系统依赖(需sudo)
#   ./deploy_rk3588_env.sh extract /path/to/tar.gz          只解压
#   ./deploy_rk3588_env.sh build                            只编译项目
#   ./deploy_rk3588_env.sh verify                           只验证
#
# 注意:
#   - 只有 deps 步骤需要 sudo，其余都用普通用户(cat)
#   - 目标路径固定为 /home/cat（保持硬编码路径一致）
#==============================================================================
set -euo pipefail

DEST=/home/cat
TARBALL="${2:-/home/cat/rk3588_env.tar.gz}"

log()  { echo -e "\033[32m[$(date +%H:%M:%S)]\033[0m $*"; }
warn() { echo -e "\033[33m[提示]\033[0m $*"; }
err()  { echo -e "\033[31m[错误]\033[0m $*" >&2; exit 1; }

#-------------------- 1. 系统依赖 (需要 sudo) --------------------
step_deps() {
    log "==> [deps] 安装系统依赖（会提示 sudo 密码）"
    sudo apt update
    sudo apt install -y \
        build-essential pkg-config git cmake \
        libfreetype6-dev libharfbuzz-dev libfontconfig1-dev libfribidi-dev \
        libass-dev libssl-dev libsrt-openssl-dev librist-dev libssh-dev \
        libv4l-dev libasound2-dev libpulse-dev \
        libopus-dev libmp3lame-dev libvorbis-dev libsoxr-dev \
        libwebp-dev libzimg-dev zlib1g-dev libbz2-dev liblzma-dev \
        libsdl2-dev \
        fonts-noto-cjk \
        v4l-utils python3-pip

    # libdrm-dev 单独装：厂商板子可能被 hold，失败就跳过（可能已装）
    if sudo apt install -y libdrm-dev 2>/dev/null; then
        log "libdrm-dev 已装"
    else
        warn "libdrm-dev 安装失败（可能被厂商 hold 或已存在），跳过"
    fi

    log "==> [deps] 安装 pymavlink"
    pip3 install --break-system-packages pymavlink -i https://pypi.tuna.tsinghua.edu.cn/simple
    log "[deps] 完成"
}

#-------------------- 2. 解压部署 --------------------
step_extract() {
    log "==> [extract] 解压 $TARBALL 到 $DEST"
    [ -f "$TARBALL" ] || err "找不到 $TARBALL"
    cd "$DEST"
    tar xzf "$TARBALL"
    sudo chown -R cat:cat "$DEST"
    log "[extract] 完成"
}

#-------------------- 3. MAVLink C 库 --------------------
step_mavlink() {
    log "==> [mavlink] 检查 MAVLink C 库"
    if [ -f "$DEST/c_library_v2/mavlink/common/mavlink.h" ]; then
        log "MAVLink C 库已存在，跳过"
        return 0
    fi
    if [ ! -f "$DEST/mavlink/message_definitions/v1.0/common.xml" ]; then
        err "MAVLink 源码和 C 库都不存在，无法生成"
    fi
    log "重新生成 MAVLink C 库"
    MAVGEN=$(command -v mavgen.py || echo /home/cat/.local/bin/mavgen.py)
    mkdir -p "$DEST/c_library_v2"
    "$MAVGEN" --lang=C --wire-protocol=2.0 --no-validate \
        --output="$DEST/c_library_v2/mavlink" \
        "$DEST/mavlink/message_definitions/v1.0/common.xml"
    log "[mavlink] 完成"
}

#-------------------- 4. 编译项目 --------------------
step_build() {
    log "==> [build] 编译项目"
    cd "$DEST/rk3588"
    cmake -B build
    cmake --build build -j"$(nproc)"
    log "[build] 完成"
}

#-------------------- 5. 验证 --------------------
step_verify() {
    log "==> [verify] 验证"
    export PKG_CONFIG_PATH="$DEST/ffmpeg/install/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/lib/pkgconfig"

    echo "--- 库版本 ---"
    pkg-config --modversion rockchip_mpp 2>/dev/null && log "rockchip_mpp OK" || warn "rockchip_mpp 缺失"
    pkg-config --modversion librga 2>/dev/null && log "librga OK" || warn "librga 缺失"

    echo "--- MAVLink ---"
    [ -f "$DEST/c_library_v2/mavlink/common/mavlink.h" ] && log "MAVLink OK" || warn "MAVLink 缺失"

    echo "--- 编译产物 ---"
    [ -x "$DEST/rk3588/build/rk3588_video_pipeline_main" ] && log "主程序 OK" || warn "主程序未编译"

    echo "--- 硬件节点 ---"
    for n in /dev/mpp_service /dev/rga /dev/video0; do
        [ -e "$n" ] && log "$n OK" || warn "$n 不存在"
    done
    log "[verify] 完成"
}

#-------------------- 入口 --------------------
usage() {
    cat <<EOF
用法: $0 <步骤> [tarball路径]

步骤:
  deps     安装系统依赖（需 sudo）
  extract  解压 tarball（默认 /home/cat/rk3588_env.tar.gz）
  mavlink  生成 MAVLink C 库（缺失时）
  build    编译项目
  verify   验证
  all      按 deps -> extract -> mavlink -> build -> verify 顺序执行

示例:
  $0 all /home/cat/rk3588_env.tar.gz
  $0 deps
  $0 build
EOF
}

case "${1:-}" in
    deps)    step_deps ;;
    extract) step_extract ;;
    mavlink) step_mavlink ;;
    build)   step_build ;;
    verify)  step_verify ;;
    all)     step_deps; step_extract; step_mavlink; step_build; step_verify ;;
    *)       usage ;;
esac
