# Board SSH Connection Notes

Last updated: 2026-07-30

## Current status

- Board `192.168.31.14`
  - User: `cat`
  - Password: `temppwd`
  - Key-based SSH: working
  - Recommended client key on Windows: `C:\Users\changpanpan\.ssh\rk14_key`
- Board `192.168.31.49`
  - System was reflashed
  - Previous SSH setup should be treated as invalid until reconfigured again

## Working connection method for 192.168.31.14

Run from Windows PowerShell:

```powershell
ssh -i "$env:USERPROFILE\.ssh\rk14_key" -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null cat@192.168.31.14
```

Quick verification command:

```powershell
ssh -i "$env:USERPROFILE\.ssh\rk14_key" -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null cat@192.168.31.14 exit
```

If this command returns directly without asking for a password, key login is working.

## How this was fixed

### 1. Generate a Windows-local key under the real logged-in user

Run from Windows PowerShell:

```powershell
New-Item -ItemType Directory -Force "$env:USERPROFILE\.ssh" | Out-Null
ssh-keygen --% -t ed25519 -f C:\Users\changpanpan\.ssh\rk14_key -N ""
```

Generated public key:

```text
ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIINpXg7rJiwBvegaR9H5ReJIcN29ZqD+x/2GwnRrSPnx changpanpan@changpan
```

### 2. Install the public key on board 192.168.31.14

Log into the board and run:

```bash
mkdir -p ~/.ssh
chmod 700 ~/.ssh
printf '%s\n' 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIINpXg7rJiwBvegaR9H5ReJIcN29ZqD+x/2GwnRrSPnx changpanpan@changpan' > ~/.ssh/authorized_keys
chmod 600 ~/.ssh/authorized_keys
chown -R cat:cat ~/.ssh
sudo systemctl restart ssh 2>/dev/null || sudo service ssh restart
```

## Known pitfalls

### 1. Do not run Windows-path SSH commands inside the board shell

Wrong place:

```bash
cat@lubancat:~$ ssh -i F:\rk3588\codex_rk3588_key ...
```

Reason:

- `F:\...` is a Windows path
- `$env:USERPROFILE` is a PowerShell variable
- The board shell is Linux, so these commands will fail or behave incorrectly

### 2. Old private key path had Windows ACL issues

Problematic key:

```text
F:\rk3588\codex_rk3588_key
```

Observed failures included:

- `Load key "...": Permission denied`
- `invalid format`
- OpenSSH could read metadata partially, but failed during signing due to ACL/owner issues

Conclusion:

- Do not reuse that key for this board workflow
- Use `C:\Users\changpanpan\.ssh\rk14_key` instead

### 3. Host key verification failures

If Windows reports:

```text
Host key verification failed.
```

Use:

```powershell
-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
```

This is already included in the recommended command above.

## Recommended diagnostic commands

### From Windows PowerShell

Test SSH:

```powershell
ssh -vvv -i "$env:USERPROFILE\.ssh\rk14_key" -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null cat@192.168.31.14 exit
```

### From board 192.168.31.14

Check SSH file permissions:

```bash
ls -ld /home/cat
ls -ld /home/cat/.ssh
ls -l /home/cat/.ssh/authorized_keys
cat /home/cat/.ssh/authorized_keys
sudo sshd -t
```

## Current board facts already verified

### 192.168.31.14

- OS: `Ubuntu 24.04.4 LTS`
- Kernel: `6.1.118`
- Hostname: `lubancat`
- FFmpeg: Rockchip-enabled custom build
- FFmpeg configure flags include:
  - `--enable-libdrm`
  - `--enable-rkmpp`
  - `--enable-rkrga`
- Available RK codecs include:
  - `h264_rkmpp`
  - `hevc_rkmpp`
  - `mjpeg_rkmpp`
  - multiple `*_rkmpp` decoders

## If connection breaks next time

Follow this order:

1. Test from Windows PowerShell:

```powershell
ssh -i "$env:USERPROFILE\.ssh\rk14_key" -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null cat@192.168.31.14 exit
```

2. If password is requested, log into the board with password and re-run the `authorized_keys` setup block in this file.

3. If `Permission denied (publickey,password)` appears, verify:
   - `~/.ssh` is `700`
   - `authorized_keys` is `600`
   - key content matches the public key in this file

4. If `Host key verification failed` appears, keep using:

```powershell
-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
```

5. If a different board is reflashed, do not assume previous SSH state still exists.
