from __future__ import annotations

import os
from dataclasses import dataclass


def _env_int(name: str, default: int) -> int:
    return int(os.getenv(name, str(default)))


def _env_float(name: str, default: float) -> float:
    return float(os.getenv(name, str(default)))


def _env_bool(name: str, default: bool) -> bool:
    value = os.getenv(name)
    if value is None:
        return default
    return value.lower() in {"1", "true", "yes", "on"}


@dataclass(frozen=True)
class GatewaySettings:
    mavlink_endpoint: str
    mavlink_baud: int
    source_system: int
    source_component: int
    target_system: int
    target_component: int
    ack_timeout_seconds: float
    max_attempts: int
    max_command_age_ms: int
    dry_run: bool
    debug: bool

    @classmethod
    def from_env(cls) -> "GatewaySettings":
        return cls(
            mavlink_endpoint=os.getenv("MAVLINK_ENDPOINT", "udpout:192.168.31.14:14550"),
            mavlink_baud=_env_int("MAVLINK_BAUD", 115200),
            source_system=_env_int("MAVLINK_SOURCE_SYSTEM", 250),
            source_component=_env_int("MAVLINK_SOURCE_COMPONENT", 191),
            target_system=_env_int("MAVLINK_TARGET_SYSTEM", 1),
            target_component=_env_int("MAVLINK_TARGET_COMPONENT", 191),
            ack_timeout_seconds=_env_float("MAVLINK_ACK_TIMEOUT", 0.8),
            max_attempts=_env_int("MAVLINK_MAX_ATTEMPTS", 3),
            max_command_age_ms=_env_int("CONTROL_MAX_AGE_MS", 3000),
            dry_run=_env_bool("MAVLINK_DRY_RUN", False),
            debug=_env_bool("CONTROL_DEBUG", False),
        )
