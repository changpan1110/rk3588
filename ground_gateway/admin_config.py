from __future__ import annotations

import copy
import json
import os
import shutil
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit


ROOT_DIR = Path(__file__).resolve().parent.parent
PROJECT_CONFIG_PATH = ROOT_DIR / "websocket" / "config" / "player.json"
PLAYER_CONFIG_PATH = (
    PROJECT_CONFIG_PATH
    if PROJECT_CONFIG_PATH.is_file()
    else ROOT_DIR / "config" / "player.json"
)
PROJECT_WEB_PLAYER_DIR = ROOT_DIR / "websocket" / "web_player"
WEB_PLAYER_DIR = (
    PROJECT_WEB_PLAYER_DIR
    if PROJECT_WEB_PLAYER_DIR.is_dir()
    else ROOT_DIR / "web_player"
)


class PlayerConfigError(ValueError):
    pass


def _mapping(value: Any, name: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise PlayerConfigError(f"{name} must be an object")
    return value


def _string(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise PlayerConfigError(f"{name} must be a non-empty string")
    return value.strip()


def _integer(value: Any, name: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise PlayerConfigError(f"{name} must be an integer")
    if value < minimum or value > maximum:
        raise PlayerConfigError(f"{name} must be between {minimum} and {maximum}")
    return value


def _boolean(value: Any, name: str) -> bool:
    if not isinstance(value, bool):
        raise PlayerConfigError(f"{name} must be a boolean")
    return value


def validate_player_config(payload: Any) -> dict[str, Any]:
    root = _mapping(payload, "config")
    video = _mapping(root.get("video"), "video")
    web = _mapping(root.get("web"), "web")
    gateway = _mapping(root.get("gateway"), "gateway")
    mavlink = _mapping(root.get("mavlink"), "mavlink")
    udp = _mapping(mavlink.get("udp"), "mavlink.udp")
    tcp = _mapping(mavlink.get("tcp"), "mavlink.tcp")
    serial = _mapping(mavlink.get("serial"), "mavlink.serial")

    video_url = _string(video.get("url"), "video.url")
    parsed_video_url = urlsplit(video_url)
    if parsed_video_url.scheme not in {"http", "https"} or not parsed_video_url.netloc:
        raise PlayerConfigError("video.url must use http:// or https://")

    web_port = _integer(web.get("port"), "web.port", 1, 65535)
    gateway_port = _integer(gateway.get("port"), "gateway.port", 1, 65535)
    if web_port == gateway_port:
        raise PlayerConfigError("web.port and gateway.port must be different")

    transport = _string(mavlink.get("transport"), "mavlink.transport").lower()
    if transport not in {"udp", "tcp", "serial"}:
        raise PlayerConfigError("mavlink.transport must be udp, tcp, or serial")

    udp_mode = _string(udp.get("mode"), "mavlink.udp.mode").lower()
    if udp_mode not in {"out", "in"}:
        raise PlayerConfigError("mavlink.udp.mode must be out or in")

    tcp_mode = _string(tcp.get("mode"), "mavlink.tcp.mode").lower()
    if tcp_mode not in {"client", "server"}:
        raise PlayerConfigError("mavlink.tcp.mode must be client or server")

    normalized = copy.deepcopy(root)
    normalized["video"]["url"] = video_url
    normalized["web"]["port"] = web_port
    normalized["gateway"]["port"] = gateway_port
    normalized["gateway"]["debug"] = _boolean(
        gateway.get("debug", False), "gateway.debug"
    )
    normalized["mavlink"]["transport"] = transport
    normalized["mavlink"]["udp"].update(
        {
            "mode": udp_mode,
            "host": _string(udp.get("host"), "mavlink.udp.host"),
            "port": _integer(udp.get("port"), "mavlink.udp.port", 1, 65535),
        }
    )
    normalized["mavlink"]["tcp"].update(
        {
            "mode": tcp_mode,
            "host": _string(tcp.get("host"), "mavlink.tcp.host"),
            "port": _integer(tcp.get("port"), "mavlink.tcp.port", 1, 65535),
        }
    )
    normalized["mavlink"]["serial"].update(
        {
            "device": _string(serial.get("device"), "mavlink.serial.device"),
            "baud": _integer(
                serial.get("baud"), "mavlink.serial.baud", 1200, 4_000_000
            ),
        }
    )
    return normalized


def load_player_config(config_path: Path = PLAYER_CONFIG_PATH) -> dict[str, Any]:
    try:
        payload = json.loads(config_path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise PlayerConfigError(f"configuration file not found: {config_path}") from exc
    except json.JSONDecodeError as exc:
        raise PlayerConfigError(f"invalid JSON configuration: {exc}") from exc
    return validate_player_config(payload)


def _atomic_write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = path.with_name(f".{path.name}.tmp")
    temporary_path.write_text(content, encoding="utf-8", newline="\n")
    os.replace(temporary_path, path)


def write_web_runtime_config(
    config: dict[str, Any], web_player_dir: Path = WEB_PLAYER_DIR
) -> None:
    if not (web_player_dir / "index.html").is_file():
        raise PlayerConfigError(f"web player not found: {web_player_dir}")

    runtime_config = {
        "video": {"url": config["video"]["url"]},
        "gateway": {"port": config["gateway"]["port"]},
    }
    serialized = json.dumps(runtime_config, ensure_ascii=False, indent=2)
    _atomic_write(web_player_dir / "runtime-config.json", serialized + "\n")
    _atomic_write(
        web_player_dir / "runtime-config.js",
        f"window.PLAYER_RUNTIME_CONFIG = {serialized};\n",
    )


def restart_required_fields(
    previous: dict[str, Any], updated: dict[str, Any]
) -> list[str]:
    fields: list[str] = []
    comparisons = (
        ("web.port", previous.get("web", {}).get("port"), updated["web"]["port"]),
        (
            "gateway.port",
            previous.get("gateway", {}).get("port"),
            updated["gateway"]["port"],
        ),
        (
            "gateway.debug",
            previous.get("gateway", {}).get("debug"),
            updated["gateway"]["debug"],
        ),
        ("mavlink", previous.get("mavlink"), updated["mavlink"]),
    )
    for name, old_value, new_value in comparisons:
        if old_value != new_value:
            fields.append(name)
    return fields


def save_player_config(
    payload: Any,
    config_path: Path = PLAYER_CONFIG_PATH,
    web_player_dir: Path = WEB_PLAYER_DIR,
) -> tuple[dict[str, Any], list[str]]:
    updated = validate_player_config(payload)
    previous = load_player_config(config_path) if config_path.is_file() else {}
    restart_fields = restart_required_fields(previous, updated)

    write_web_runtime_config(updated, web_player_dir)
    if config_path.is_file():
        shutil.copy2(config_path, config_path.with_suffix(config_path.suffix + ".bak"))
    serialized = json.dumps(updated, ensure_ascii=False, indent=2) + "\n"
    _atomic_write(config_path, serialized)
    return updated, restart_fields
