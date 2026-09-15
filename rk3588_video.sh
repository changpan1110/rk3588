#!/bin/bash
# =============================================================
#  RK3588 视频系统管理脚本 (can0 + mediamtx + pipeline)
#
#  用法:
#    sudo ./rk3588_video.sh start      临时启动(立即启动, 不开机自启)
#    sudo ./rk3588_video.sh enable     自启动(设置开机自启 + 立即启动)
#    sudo ./rk3588_video.sh disable    取消自启动
#    sudo ./rk3588_video.sh stop       停止
#    sudo ./rk3588_video.sh restart    重启
#    sudo ./rk3588_video.sh status     查看状态
#    sudo ./rk3588_video.sh            无参数 -> 显示本帮助+状态
# =============================================================
set -u
if [ "$(id -u)" -ne 0 ]; then exec sudo "$0" "$@"; fi

CAN_IF="can0"
CAN_BITRATE="1000000"
MEDIAMTX_BIN="/home/cat/mediamtx"
MEDIAMTX_DIR="/home/cat"
MEDIAMTX_LOG="/tmp/mediamtx.log"
PIPELINE_BIN="/home/cat/rk3588/build/rk3588_video_pipeline_main"
PIPELINE_DIR="/home/cat/rk3588/build"
PIPELINE_LOG="/tmp/video_pipeline.log"
SERVICE="rk3588-video.service"

log() { echo "[$(date '+%H:%M:%S')] $*"; }

can0_status()     { ip link show "$CAN_IF" 2>/dev/null | grep -q "state UP"; }
mediamtx_status() { pgrep -x mediamtx >/dev/null 2>&1; }
pipeline_status() { pgrep -f rk3588_video_pipeline_main >/dev/null 2>&1; }

# 轮询等待就绪: 每0.5s检查一次, 最多等 $2 秒, 提前就绪立即返回
wait_for() {
    local func="$1" max=$(( $2 * 2 )) i=0
    while [ $i -lt $max ]; do
        if "$func"; then return 0; fi
        sleep 0.5
        i=$((i+1))
    done
    return 1
}

start_components() {
    local fail=0 t0=$(date +%s)

    log "步骤 1/3: 启动 can0 (${CAN_BITRATE})"
    ip link set "$CAN_IF" down 2>/dev/null || true
    ip link set "$CAN_IF" type can bitrate "$CAN_BITRATE" restart-ms 100
    ip link set "$CAN_IF" up
    if wait_for can0_status 3; then log "  [OK] can0 已启动"; else log "  [FAIL] can0 启动失败"; fail=1; fi

    log "步骤 2/3: 启动 mediamtx"
    if mediamtx_status; then
        log "  [OK] mediamtx 已在运行"
    else
        ( cd "$MEDIAMTX_DIR" && nohup "$MEDIAMTX_BIN" >"$MEDIAMTX_LOG" 2>&1 & )
        if wait_for mediamtx_status 5; then log "  [OK] mediamtx 启动成功"; else log "  [FAIL] mediamtx 启动失败! 日志: $MEDIAMTX_LOG"; fail=1; fi
    fi

    log "步骤 3/3: 启动 rk3588_video_pipeline_main"
    if pipeline_status; then
        log "  [OK] pipeline 已在运行"
    else
        ( cd "$PIPELINE_DIR" && nohup sh -c "tail -f /dev/null | exec $PIPELINE_BIN" >"$PIPELINE_LOG" 2>&1 & )
        if wait_for pipeline_status 10; then log "  [OK] pipeline 启动成功"; else log "  [FAIL] pipeline 启动失败! 日志: $PIPELINE_LOG"; fail=1; fi
    fi

    if [ $fail -eq 0 ]; then
        log "启动结果: [成功] 全部组件已运行, 耗时 $(( $(date +%s) - t0 )) 秒"
    else
        log "启动结果: [失败] 请查看上面 [FAIL] 提示"
        return 1
    fi
}

stop_components() {
    log "停止 pipeline ..."
    pkill -9 -f rk3588_video_pipeline_main 2>/dev/null && log "  [OK] pipeline 已停止" || log "  [跳过] 未运行"
    pkill -f 'tail -f /dev/null' 2>/dev/null || true
    log "停止 mediamtx ..."
    pkill -f mediamtx 2>/dev/null && log "  [OK] mediamtx 已停止" || log "  [跳过] 未运行"
    log "停止 can0 ..."
    ip link set "$CAN_IF" down 2>/dev/null && log "  [OK] can0 已 down" || log "  [跳过]"
    log "停止完成"
}

do_status() {
    echo ""
    echo "=================================================="
    echo "  RK3588 视频系统状态"
    echo "=================================================="
    if can0_status; then
        _br=$(ip -details link show "$CAN_IF" 2>/dev/null | grep -o 'bitrate [0-9]*' | awk '{print $2}')
        echo "  can0     : [运行中] UP bitrate=${_br}"
    else
        echo "  can0     : [未运行] DOWN"
    fi
    mediamtx_status && echo "  mediamtx : [运行中] PID $(pgrep -x mediamtx | head -1)" || echo "  mediamtx : [未运行]"
    pipeline_status && echo "  pipeline : [运行中] PID $(pgrep -f rk3588_video_pipeline_main | head -1)" || echo "  pipeline : [未运行]"
    echo "--------------------------------------------------"
    _en=$(systemctl is-enabled "$SERVICE" 2>/dev/null)
    if [ "$_en" = "enabled" ]; then
        echo "  启动方式 : 自启动 (开机自动拉起)"
    else
        echo "  启动方式 : 临时启动 (手动, 开机不自启)"
    fi
    echo "  服务状态 : $(systemctl is-active "$SERVICE" 2>/dev/null)"
    echo "=================================================="
    if can0_status && mediamtx_status && pipeline_status; then
        echo "  启动结果 : [成功] 全部组件正常运行"
    else
        echo "  启动结果 : [失败] 有组件未运行"
    fi
    echo ""
}

usage() {
    echo ""
    echo "用法: $0 {start|enable|disable|stop|restart|status}"
    echo ""
    echo "  start     临时启动 (立即启动, 不设开机自启)"
    echo "  enable    自启动   (设置开机自启 + 立即启动)"
    echo "  disable   取消自启动"
    echo "  stop      停止"
    echo "  restart   重启"
    echo "  status    查看状态"
    echo ""
    do_status
}

case "${1:-}" in
    start)
        systemctl disable "$SERVICE" >/dev/null 2>&1
        log "临时启动 (已取消开机自启)"
        start_components
        ;;
    enable)
        systemctl enable "$SERVICE" >/dev/null 2>&1
        log "已设置自启动 (开机自动拉起)"
        start_components
        ;;
    disable)
        systemctl disable "$SERVICE" >/dev/null 2>&1
        log "已取消自启动 (当前进程不受影响, 开机不再自启)"
        ;;
    stop)
        stop_components
        ;;
    restart)
        stop_components
        start_components
        ;;
    status)
        do_status
        ;;
    _boot)   # systemd 开机自启内部入口: 只拉起不等待, 不阻塞开机
        log "开机自启: 后台拉起组件(不阻塞开机)"
        ip link set "$CAN_IF" down 2>/dev/null || true
        ip link set "$CAN_IF" type can bitrate "$CAN_BITRATE" restart-ms 100
        ip link set "$CAN_IF" up
        mediamtx_status || ( cd "$MEDIAMTX_DIR" && nohup "$MEDIAMTX_BIN" >"$MEDIAMTX_LOG" 2>&1 & )
        pipeline_status || ( cd "$PIPELINE_DIR" && nohup sh -c "tail -f /dev/null | exec $PIPELINE_BIN" >"$PIPELINE_LOG" 2>&1 & )
        log "开机自启完成(组件后台初始化中)"
        ;;
    *)
        usage
        ;;
esac
