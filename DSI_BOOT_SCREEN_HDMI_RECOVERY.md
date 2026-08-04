# RK3588 DSI 启动画面关闭与 HDMI 恢复记录

记录日期：2026-08-03  
目标板卡：`192.168.31.14`，主机名 `lubancat`  
系统：Ubuntu 24.04，内核 `6.1.118`

## 1. 最终目标

- HDMI 正常启动 GNOME/GDM 图形界面。
- MIPI-DSI 驱动和设备树保持启用。
- DSI 不显示内核控制台、Plymouth 和 GNOME 登录画面。
- DSI 背光在启动时关闭，后续应用可以主动恢复背光并使用屏幕。

## 2. DRM 和设备树映射

DRM 的 `cardN` 编号会随驱动加载顺序变化，不能写死。

本次最终启动后的映射：

```text
/dev/dri/card2                              Rockchip display-subsystem
/dev/dri/by-path/platform-display-subsystem-card -> ../card2
card2-HDMI-A-1                              HDMI 输出
card2-DSI-1                                 MIPI-DSI 输出
/dev/fb0                                    rockchipdrmfb
```

DSI 硬件与设备树节点：

```text
控制器：/dsi@fde20000
面板：  /dsi@fde20000/panel@0
路由：  /display-subsystem/route/route-dsi0
背光：  /backlight-dsi0
```

程序应优先使用稳定路径：

```text
/dev/dri/by-path/platform-display-subsystem-card
```

然后通过 DRM API 查找 `DSI-1`，不要依赖固定的 `card2` 或 connector ID。

## 3. 问题和排查过程

### 3.1 设备树禁用后的结果

曾将以下节点禁用：

```text
route-dsi0       disabled
dsi@fde20000     disabled
panel@0          不存在
```

结果是 `DSI-1` connector 完全消失。即使 `/dev/dri/card0` 或 `/dev/fb0` 仍存在，也不能绕过设备树直接驱动已禁用的 DSI。

执行原 DSI modeset 时返回：

```text
failed to find mode "1024x600" for connector 255
failed to create dumb buffer: Invalid argument
```

因此重新启用了 DSI overlay：

```text
rk3588-lubancat-5io-dsi0-vp2-1024x600-7inch-ebf410173-overlay
```

DSI 恢复为 `connected`，模式为 `1024x600@60Hz`。

### 3.2 全局关闭图形目标的问题

为了关闭 DSI 启动画面，曾将系统默认目标设置为：

```text
multi-user.target
```

这会同时关闭 HDMI 和 DSI 的 GDM 图形界面，范围过大，不符合最终目标。

随后已恢复：

```bash
sudo systemctl set-default graphical.target
sudo systemctl start gdm.service
```

最终默认目标为 `graphical.target`，GDM 状态为 `active`。

### 3.3 HDMI 没有登录画面的原因

恢复 GDM 后，Mutter 的原始布局是：

```text
DSI-1：  主屏，位置 0,0，1024x600
HDMI-1： 副屏，位置 1024,0，1920x1080
```

DSI 背光为 0，因此 GNOME 登录界面实际画在不可见的 DSI 主屏上，HDMI 只显示副屏背景。

最终修正为：

```text
HDMI-1：唯一逻辑显示器、主屏、1920x1080@60Hz
DSI-1：在 GNOME/Mutter 布局中禁用
```

这只禁用桌面对 DSI 的输出，不会禁用 DSI 设备树或 DRM connector。

## 4. 修改的板卡文件

### 4.1 `/boot/firmware/ubuntuEnv.txt`

原文件已备份：

```text
/boot/firmware/ubuntuEnv.txt.codex-backup-20260803-114331
```

当前 `bootargs`：

```text
bootargs=rootfstype=ext4 rootwait rw console=ttyS2,1500000 console=ttyFIQ0 cgroup_enable=cpuset cgroup_memory=1 cgroup_enable=memory swapaccount=1 systemd.unified_cgroup_hierarchy=0 quiet plymouth.enable=0 vt.global_cursor_default=0 logo.nologo
```

主要变化：

- 删除 `console=tty1`，避免 Linux 虚拟终端显示在屏幕上。
- 删除 `splash` 和 `plymouth.ignore-serial-consoles`。
- 添加 `plymouth.enable=0`，关闭 Plymouth。
- 添加 `vt.global_cursor_default=0`，关闭虚拟终端光标。
- 添加 `logo.nologo`，关闭内核 logo。
- 保留串口控制台 `console=ttyFIQ0`。

`boot.scr` 会直接导入 `ubuntuEnv.txt`，修改该文件后不需要重新生成 `boot.scr`。

### 4.2 `/etc/systemd/system/dsi-backlight-off.service`

用途：系统启动时关闭 DSI0 背光，但不关闭 DSI 驱动。

```ini
[Unit]
Description=Keep DSI0 backlight off until an application enables it
After=systemd-udev-trigger.service
Before=multi-user.target

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'for i in $(seq 1 100); do if [ -e /sys/class/backlight/backlight-dsi0/brightness ]; then echo 0 > /sys/class/backlight/backlight-dsi0/brightness; exit 0; fi; sleep 0.1; done; exit 0'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

启用命令：

```bash
sudo systemctl daemon-reload
sudo systemctl enable dsi-backlight-off.service
```

### 4.3 `/var/lib/gdm3/.config/monitors.xml`

用途：配置 GDM 登录界面只使用 HDMI，将 DSI 从 GDM 布局中禁用。

文件权限应为：

```text
owner: gdm
group: gdm
mode:  0600
```

### 4.4 `/home/cat/.config/monitors.xml`

用途：`cat` 用户登录 GNOME 后继续保持 HDMI 为主屏、DSI 桌面输出关闭。

文件权限应为：

```text
owner: cat
group: cat
mode:  0600
```

两个 `monitors.xml` 使用相同内容：

```xml
<monitors version="2">
  <configuration>
    <logicalmonitor>
      <x>0</x>
      <y>0</y>
      <scale>1</scale>
      <primary>yes</primary>
      <monitor>
        <monitorspec>
          <connector>HDMI-1</connector>
          <vendor>XMI</vendor>
          <product>Redmi 27 NF</product>
          <serial>3751300128588</serial>
        </monitorspec>
        <mode>
          <width>1920</width>
          <height>1080</height>
          <rate>60.000</rate>
        </mode>
      </monitor>
    </logicalmonitor>
    <disabled>
      <monitorspec>
        <connector>DSI-1</connector>
        <vendor>unknown</vendor>
        <product>unknown</product>
        <serial>unknown</serial>
      </monitorspec>
    </disabled>
  </configuration>
</monitors>
```

配置修改后重启 GDM：

```bash
sudo systemctl restart gdm.service
```

## 5. 最终验证状态

最终从 Mutter、DRM sysfs 和 systemd 验证到：

```text
default target:               graphical.target
GDM:                          active
dsi-backlight-off.service:    active
DSI brightness:               0
card2-HDMI-A-1 status:         connected
card2-HDMI-A-1 enabled:        enabled
card2-DSI-1 status:            connected
card2-DSI-1 enabled by Mutter: disabled
Mutter primary monitor:        HDMI-1
HDMI mode:                     1920x1080@60Hz
```

`DSI-1 enabled=disabled` 只表示当前 GNOME/Mutter 没有给它分配逻辑显示器。DSI connector、面板驱动和设备树仍然存在。

## 6. 常用检查命令

### 查看 DRM 编号和稳定路径

```bash
ls -l /dev/dri/by-path

for c in /sys/class/drm/card[0-9]*; do
    [ -e "$c" ] || continue
    echo "$(basename "$c") -> $(readlink -f "$c/device")"
done
```

### 查看 HDMI 和 DSI 状态

```bash
for c in /sys/class/drm/card*-HDMI-A-1 /sys/class/drm/card*-DSI-1; do
    [ -d "$c" ] || continue
    echo "$(basename "$c") status=$(cat "$c/status") enabled=$(cat "$c/enabled")"
done
```

### 查看 DSI 背光

```bash
cat /sys/class/backlight/backlight-dsi0/brightness
cat /sys/class/backlight/backlight-dsi0/max_brightness
```

### 查看显示 connector 和模式

```bash
modetest -D /dev/dri/by-path/platform-display-subsystem-card -c
```

## 7. 后续应用使用 DSI

恢复当前测试亮度：

```bash
echo 78 | sudo tee /sys/class/backlight/backlight-dsi0/brightness
```

再次关闭背光：

```bash
echo 0 | sudo tee /sys/class/backlight/backlight-dsi0/brightness
```

注意：GDM/Mutter 运行时持有整个 Rockchip DRM card 的 DRM master。即使 DSI connector 没有加入桌面布局，普通 `modetest` 或独立 DRM/KMS 程序也可能无法直接 modeset DSI。

可选方案：

- 让应用通过 Wayland/GNOME 输出，而不是直接取得 DRM master。
- 应用启动前停止 GDM，应用退出后再恢复 GDM。
- 使用支持 DRM lease 的 compositor，将 DSI connector 租给应用。
- 使用一个自定义 compositor 同时管理 HDMI 和 DSI。

停止和恢复 GDM 会同时影响 HDMI：

```bash
sudo systemctl stop gdm.service
sudo systemctl start gdm.service
```

## 8. 回退方法

### 恢复原始内核启动参数

```bash
sudo cp /boot/firmware/ubuntuEnv.txt.codex-backup-20260803-114331 \
  /boot/firmware/ubuntuEnv.txt
```

### 恢复 DSI 背光自动启动

```bash
sudo systemctl disable --now dsi-backlight-off.service
sudo rm /etc/systemd/system/dsi-backlight-off.service
sudo systemctl daemon-reload
```

### 删除 GNOME 固定显示布局

```bash
sudo rm -f /var/lib/gdm3/.config/monitors.xml
rm -f /home/cat/.config/monitors.xml
sudo systemctl restart gdm.service
```

### 确保 HDMI 图形桌面自动启动

```bash
sudo systemctl set-default graphical.target
sudo systemctl start gdm.service
```

完成回退后重启：

```bash
sudo reboot
```

## 9. 安全说明

- 本文不记录 SSH 私钥内容或 sudo 密码。
- 工作区私钥的 Windows ACL 不适合直接由普通用户读取；日常连接使用：

```powershell
ssh -i "$env:USERPROFILE\.ssh\rk14_key" cat@192.168.31.14
```

- 修改显示配置前应先备份目标文件。
- 不要通过禁用 DSI 设备树来实现“启动时不显示”，否则 DSI connector 会消失，后续程序也无法使用。
