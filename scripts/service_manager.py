from __future__ import annotations

import argparse
import ctypes
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import webbrowser
from dataclasses import dataclass
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parent.parent
RUN_DIR = ROOT_DIR / ".run"

# The project keeps the editable web UI inside the portable websocket folder.
# A copied portable package falls back to its own local web_player directory.
PROJECT_WEB_PLAYER_DIR = ROOT_DIR / "websocket" / "web_player"
WEB_PLAYER_DIR = (
    PROJECT_WEB_PLAYER_DIR
    if PROJECT_WEB_PLAYER_DIR.is_dir()
    else ROOT_DIR / "web_player"
)
PROJECT_CONFIG_PATH = ROOT_DIR / "websocket" / "config" / "player.json"
DEFAULT_CONFIG_PATH = (
    PROJECT_CONFIG_PATH
    if PROJECT_CONFIG_PATH.is_file()
    else ROOT_DIR / "config" / "player.json"
)


@dataclass(frozen=True)
class RuntimeSettings:
    config_path: Path
    video_url: str
    web_port: int
    gateway_port: int
    mavlink_endpoint: str
    mavlink_baud: int
    control_debug: bool


def load_runtime_settings(args: argparse.Namespace) -> RuntimeSettings:
    config_path = Path(args.config)
    if not config_path.is_absolute():
        project_config_path = ROOT_DIR / "websocket" / config_path
        config_path = (
            project_config_path
            if project_config_path.is_file()
            else ROOT_DIR / config_path
        )
    try:
        config = json.loads(config_path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ValueError(f"configuration file not found: {config_path}") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(f"invalid JSON configuration: {exc}") from exc

    video_url = str(config["video"]["url"])
    web_port = args.web_port or int(config["web"]["port"])
    gateway_port = args.gateway_port or int(config["gateway"]["port"])
    control_debug = bool(config["gateway"].get("debug", False)) or bool(
        getattr(args, "debug_control", False)
    )

    mavlink = config["mavlink"]
    transport = str(mavlink["transport"]).lower()
    if args.mavlink_serial:
        transport = "serial"

    if args.air_ip:
        parsed_video_url = urllib.parse.urlsplit(video_url)
        video_url = urllib.parse.urlunsplit(
            (
                parsed_video_url.scheme,
                f"{args.air_ip}:{parsed_video_url.port or 8889}",
                parsed_video_url.path,
                parsed_video_url.query,
                parsed_video_url.fragment,
            )
        )

    if transport == "udp":
        udp = mavlink["udp"]
        mode = str(udp.get("mode", "out")).lower()
        prefix = {"out": "udpout", "in": "udpin"}.get(mode)
        if prefix is None:
            raise ValueError("mavlink.udp.mode must be 'out' or 'in'")
        host = args.air_ip or str(udp["host"])
        port = args.mavlink_port or int(udp["port"])
        endpoint = f"{prefix}:{host}:{port}"
        baud = args.mavlink_baud or int(mavlink["serial"].get("baud", 115200))
    elif transport == "tcp":
        tcp = mavlink["tcp"]
        mode = str(tcp.get("mode", "client")).lower()
        prefix = {"client": "tcp", "server": "tcpin"}.get(mode)
        if prefix is None:
            raise ValueError("mavlink.tcp.mode must be 'client' or 'server'")
        host = args.air_ip or str(tcp["host"])
        port = args.mavlink_port or int(tcp["port"])
        endpoint = f"{prefix}:{host}:{port}"
        baud = args.mavlink_baud or int(mavlink["serial"].get("baud", 115200))
    elif transport == "serial":
        serial = mavlink["serial"]
        endpoint = args.mavlink_serial or str(serial["device"])
        baud = args.mavlink_baud or int(serial.get("baud", 115200))
    else:
        raise ValueError("mavlink.transport must be 'udp', 'tcp', or 'serial'")

    for name, port in (("web.port", web_port), ("gateway.port", gateway_port)):
        if port < 1 or port > 65535:
            raise ValueError(f"{name} must be between 1 and 65535")

    return RuntimeSettings(
        config_path=config_path,
        video_url=video_url,
        web_port=web_port,
        gateway_port=gateway_port,
        mavlink_endpoint=endpoint,
        mavlink_baud=baud,
        control_debug=control_debug,
    )


def metadata_path(name: str) -> Path:
    return RUN_DIR / f"{name}.json"


def read_metadata(name: str) -> dict | None:
    path = metadata_path(name)
    try:
        return json.loads(path.read_text(encoding="ascii"))
    except (FileNotFoundError, ValueError, TypeError):
        return None


def process_exists(pid: int) -> bool:
    if os.name == "nt":
        process_query_limited_information = 0x1000
        handle = ctypes.windll.kernel32.OpenProcess(
            process_query_limited_information, False, pid
        )
        if not handle:
            return False
        ctypes.windll.kernel32.CloseHandle(handle)
        return True

    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def managed_pid(name: str) -> int | None:
    metadata = read_metadata(name)
    if not metadata:
        return None
    try:
        pid = int(metadata["pid"])
    except (KeyError, TypeError, ValueError):
        metadata_path(name).unlink(missing_ok=True)
        return None
    if not process_exists(pid):
        metadata_path(name).unlink(missing_ok=True)
        return None
    return pid


def start_service(
    name: str,
    command: list[str],
    cwd: Path,
    environment: dict[str, str] | None = None,
) -> int:
    existing_pid = managed_pid(name)
    if existing_pid is not None:
        print(f"{name} is already running (PID {existing_pid}).")
        return existing_pid

    RUN_DIR.mkdir(parents=True, exist_ok=True)
    stdout_file = (RUN_DIR / f"{name}.out.log").open("ab", buffering=0)
    stderr_file = (RUN_DIR / f"{name}.err.log").open("ab", buffering=0)
    child_environment = os.environ.copy()
    if environment:
        child_environment.update(environment)

    creation_flags = 0
    start_new_session = os.name != "nt"
    if os.name == "nt":
        creation_flags = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP

    try:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            stdin=subprocess.DEVNULL,
            stdout=stdout_file,
            stderr=stderr_file,
            env=child_environment,
            close_fds=True,
            creationflags=creation_flags,
            start_new_session=start_new_session,
        )
    finally:
        stdout_file.close()
        stderr_file.close()

    metadata_path(name).write_text(
        json.dumps({"pid": process.pid, "command": command}),
        encoding="ascii",
    )
    print(f"{name} started (PID {process.pid}).")
    return process.pid


def terminate_process(pid: int) -> None:
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(pid), "/T", "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        return
    os.kill(pid, signal.SIGTERM)


def stop_service(name: str) -> None:
    pid = managed_pid(name)
    if pid is None:
        print(f"{name} is not running.")
        return
    terminate_process(pid)
    metadata_path(name).unlink(missing_ok=True)
    print(f"{name} stopped.")


def wait_http(url: str, attempts: int = 30) -> bool:
    for _ in range(attempts):
        try:
            with urllib.request.urlopen(url, timeout=1) as response:
                if 200 <= response.status < 500:
                    return True
        except (OSError, urllib.error.URLError):
            pass
        time.sleep(0.25)
    return False


def player_url(video_url: str, web_port: int) -> str:
    return f"http://127.0.0.1:{web_port}/"


def write_web_runtime_config(settings: RuntimeSettings) -> None:
    runtime_config_path = WEB_PLAYER_DIR / "runtime-config.json"
    runtime_script_path = WEB_PLAYER_DIR / "runtime-config.js"
    runtime_config = {
        "video": {"url": settings.video_url},
        "gateway": {"port": settings.gateway_port},
    }
    serialized_config = json.dumps(runtime_config, ensure_ascii=False, indent=2)
    runtime_config_path.write_text(
        serialized_config + "\n",
        encoding="utf-8",
    )
    runtime_script_path.write_text(
        f"window.PLAYER_RUNTIME_CONFIG = {serialized_config};\n",
        encoding="utf-8",
    )


def start(args: argparse.Namespace) -> int:
    settings = load_runtime_settings(args)
    python = sys.executable
    write_web_runtime_config(settings)
    start_service(
        "web",
        [python, "-m", "http.server", str(settings.web_port), "--bind", "0.0.0.0"],
        WEB_PLAYER_DIR,
    )
    start_service(
        "gateway",
        [
            python,
            "-m",
            "uvicorn",
            "ground_gateway.main:app",
            "--host",
            "0.0.0.0",
            "--port",
            str(settings.gateway_port),
        ],
        ROOT_DIR,
        {
            "MAVLINK_DRY_RUN": "1" if args.dry_run else "0",
            "MAVLINK_ENDPOINT": settings.mavlink_endpoint,
            "MAVLINK_BAUD": str(settings.mavlink_baud),
            "CONTROL_DEBUG": "1" if settings.control_debug else "0",
        },
    )

    web_ready = wait_http(f"http://127.0.0.1:{settings.web_port}/")
    gateway_ready = wait_http(f"http://127.0.0.1:{settings.gateway_port}/health")
    if not web_ready or not gateway_ready:
        print("A service failed to become ready. Run: player.cmd logs", file=sys.stderr)
        show_status(args)
        return 1

    url = player_url(settings.video_url, settings.web_port)
    print(f"Mode: {'DRY-RUN' if args.dry_run else 'REAL MAVLINK'}")
    print(f"Config: {settings.config_path}")
    print(f"Control debug: {'ON' if settings.control_debug else 'OFF'}")
    print(f"MAVLink: {settings.mavlink_endpoint} baud={settings.mavlink_baud}")
    print(f"Player: {url}")
    print(f"Video: {settings.video_url}")
    print(f"Control: ws://127.0.0.1:{settings.gateway_port}/ws/control")
    if not args.no_browser:
        webbrowser.open(url)
    return 0


def show_status(args: argparse.Namespace) -> int:
    settings = load_runtime_settings(args)
    for name in ("web", "gateway"):
        pid = managed_pid(name)
        state = f"RUNNING (PID {pid})" if pid is not None else "STOPPED"
        print(f"{name.capitalize():8} {state}")
    print(f"Config:  {settings.config_path}")
    print(f"MAVLink: {settings.mavlink_endpoint} baud={settings.mavlink_baud}")
    print(f"Player:  {player_url(settings.video_url, settings.web_port)}")
    print(f"Health:  http://127.0.0.1:{settings.gateway_port}/health")
    return 0


def show_logs() -> int:
    for name in ("web.out.log", "web.err.log", "gateway.out.log", "gateway.err.log"):
        path = RUN_DIR / name
        print(f"\n===== {name} =====")
        if not path.exists():
            print("No log file.")
            continue
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        print("\n".join(lines[-80:]))
    return 0


def smoke_test(args: argparse.Namespace) -> int:
    from websockets.sync.client import connect

    command_id = str(uuid.uuid4())
    payload = {
        "version": 1,
        "type": "command",
        "client_id": str(uuid.uuid4()),
        "command_id": command_id,
        "sequence": 1,
        "command": "camera.snapshot",
        "params": {},
        "sent_at_ms": int(time.time() * 1000),
    }
    settings = load_runtime_settings(args)
    url = f"ws://127.0.0.1:{settings.gateway_port}/ws/control"
    try:
        with connect(url, open_timeout=3, close_timeout=1) as websocket:
            websocket.send(json.dumps(payload))
            response = json.loads(websocket.recv(timeout=5))
    except Exception as exc:
        print(f"Smoke test failed: {exc}", file=sys.stderr)
        return 1

    if response.get("type") != "ack" or response.get("command_id") != command_id:
        print(f"Smoke test returned an invalid ACK: {response}", file=sys.stderr)
        return 1
    if not response.get("ok"):
        print(f"Command was rejected: {response}", file=sys.stderr)
        return 1
    print(f"Smoke test passed: {response.get('message')}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="action", required=True)

    def add_common(subparser: argparse.ArgumentParser) -> None:
        subparser.add_argument("--config", default=str(DEFAULT_CONFIG_PATH))
        subparser.add_argument("--air-ip", default="")
        subparser.add_argument("--web-port", type=int, default=0)
        subparser.add_argument("--gateway-port", type=int, default=0)
        subparser.add_argument("--mavlink-port", type=int, default=0)
        subparser.add_argument("--mavlink-serial", default="")
        subparser.add_argument("--mavlink-baud", type=int, default=0)

    start_parser = subparsers.add_parser("start")
    add_common(start_parser)
    start_parser.add_argument("--dry-run", action="store_true")
    start_parser.add_argument("--debug-control", action="store_true")
    start_parser.add_argument("--no-browser", action="store_true")

    status_parser = subparsers.add_parser("status")
    add_common(status_parser)

    open_parser = subparsers.add_parser("open")
    add_common(open_parser)

    smoke_parser = subparsers.add_parser("smoke")
    add_common(smoke_parser)

    subparsers.add_parser("stop")
    subparsers.add_parser("logs")
    return parser


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(errors="backslashreplace")
    args = build_parser().parse_args()
    if args.action == "start":
        return start(args)
    if args.action == "stop":
        stop_service("gateway")
        stop_service("web")
        return 0
    if args.action == "status":
        return show_status(args)
    if args.action == "logs":
        return show_logs()
    if args.action == "open":
        settings = load_runtime_settings(args)
        webbrowser.open(player_url(settings.video_url, settings.web_port))
        return 0
    if args.action == "smoke":
        return smoke_test(args)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
