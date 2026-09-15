#!/bin/bash
# =============================================================
#  FTP 用户创建脚本 (vsftpd)
#  功能: 新建/更新 FTP 用户 + 密码 + 访问路径(读写删除) + 重启服务
#
#  用法: sudo ./create_ftp_user.sh <用户名> <密码> <访问路径>
#  示例: sudo ./create_ftp_user.sh ftpuser 123456 /home/cat/ftp_share
# =============================================================
set -u

if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$@"
fi

USER_NAME="${1:-}"
USER_PASS="${2:-}"
USER_PATH="${3:-}"

if [ -z "$USER_NAME" ] || [ -z "$USER_PASS" ] || [ -z "$USER_PATH" ]; then
    echo "用法: $0 <用户名> <密码> <访问路径>"
    echo "示例: $0 ftpuser 123456 /home/cat/ftp_share"
    exit 1
fi

log() { echo "[$(date '+%H:%M:%S')] $*"; }

if ! command -v vsftpd >/dev/null 2>&1; then
    log "[FAIL] 未安装 vsftpd，请先: sudo apt install vsftpd"
    exit 1
fi

# 1. 创建/更新用户，家目录设为访问路径
if id "$USER_NAME" >/dev/null 2>&1; then
    log "用户 $USER_NAME 已存在，更新家目录为 $USER_PATH"
    usermod -d "$USER_PATH" "$USER_NAME"
else
    log "创建用户 $USER_NAME, 家目录 $USER_PATH"
    useradd -m -d "$USER_PATH" -s /bin/bash "$USER_NAME"
fi

# 2. 设置密码
echo "${USER_NAME}:${USER_PASS}" | chpasswd
log "密码已设置"

# 3. 确保路径存在并赋予读写删除权限
mkdir -p "$USER_PATH"
chown -R "${USER_NAME}:${USER_NAME}" "$USER_PATH"
chmod 775 "$USER_PATH"
log "路径已就绪: $USER_PATH (属主=${USER_NAME}, 权限=775)"

# 3.1 确保父目录可遍历(否则 vsftpd 无法进入，报 cannot change directory)
_parent=$(dirname "$USER_PATH")
while [ "$_parent" != "/" ]; do
    chmod o+x "$_parent" 2>/dev/null || true
    _parent=$(dirname "$_parent")
done
log "父目录已添加遍历权限(o+x)"

# 4. 重启 vsftpd 服务
log "重启 vsftpd 服务 ..."
systemctl restart vsftpd
sleep 1
if systemctl is-active --quiet vsftpd; then
    log "[OK] vsftpd 已重启并运行正常"
else
    log "[FAIL] vsftpd 重启失败"
    exit 1
fi

log "完成 ✔"
log "  FTP 地址: <板子IP>:21"
log "  用户名:   $USER_NAME"
log "  密码:     $USER_PASS"
log "  访问路径: $USER_PATH"
