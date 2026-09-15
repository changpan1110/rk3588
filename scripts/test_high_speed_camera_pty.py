#!/usr/bin/env python3
import os
import pty
import select
import subprocess
import sys
import time


KEY_REQUEST = bytes.fromhex("AA 01 01 95 D1 01 55")
KEY_RESPONSE = bytes.fromhex("AA 03 01 F3 B3 01 55")
BAD_CRC_REQUEST = bytes.fromhex("AA 01 01 00 00 01 55")
ERROR_RESPONSE = bytes.fromhex("AA 04 00 7A 05 01 55")


def read_exact(fd, size, timeout):
    deadline = time.monotonic() + timeout
    data = bytearray()
    while len(data) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        readable, _, _ = select.select([fd], [], [], remaining)
        if not readable:
            break
        data.extend(os.read(fd, size - len(data)))
    return bytes(data)


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./mian"
    master_fd, slave_fd = pty.openpty()
    slave_name = os.ttyname(slave_fd)
    os.close(slave_fd)

    process = subprocess.Popen(
        [binary, "high", slave_name, "115200"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        time.sleep(0.2)
        process.stdin.write(b"1\n")
        process.stdin.flush()
        time.sleep(0.2)

        os.write(master_fd, KEY_REQUEST)
        response = read_exact(master_fd, 7, 2.0)
        if response != KEY_RESPONSE:
            raise RuntimeError(
                f"key response mismatch: {response.hex(' ').upper()}"
            )
        print("PASS key response:", response.hex(" ").upper())

        os.write(master_fd, BAD_CRC_REQUEST)
        response = read_exact(master_fd, 7, 2.0)
        if response != ERROR_RESPONSE:
            raise RuntimeError(
                f"NAK response mismatch: {response.hex(' ').upper()}"
            )
        print("PASS CRC NAK:", response.hex(" ").upper())

        process.stdin.write(b"q\n0\n")
        process.stdin.flush()
        stdout, stderr = process.communicate(timeout=3.0)
        if process.returncode != 0:
            raise RuntimeError(
                f"mian exited {process.returncode}\n"
                f"stdout={stdout.decode(errors='replace')}\n"
                f"stderr={stderr.decode(errors='replace')}"
            )
    finally:
        os.close(master_fd)
        if process.poll() is None:
            process.kill()
            process.wait()

    print("High-speed camera PTY integration test passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
