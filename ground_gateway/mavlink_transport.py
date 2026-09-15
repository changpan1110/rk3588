from __future__ import annotations

import threading
import logging
from dataclasses import dataclass

from .config import GatewaySettings
from .gateway_core import ControlCommand, encode_user_command_params


logger = logging.getLogger("uvicorn.error")


@dataclass(frozen=True)
class TransportResult:
    accepted: bool
    result_code: int
    result_name: str
    attempts: int


class DryRunTransport:
    def send_and_wait(self, command: ControlCommand) -> TransportResult:
        return TransportResult(True, 0, "DRY_RUN_ACCEPTED", 1)


class MavlinkCommandTransport:
    def __init__(self, settings: GatewaySettings) -> None:
        try:
            from pymavlink import mavutil
        except ImportError as exc:
            raise RuntimeError("pymavlink is not installed; run pip install -r requirements.txt") from exc

        self._mavutil = mavutil
        self._settings = settings
        self._lock = threading.Lock()
        self._has_sent = False
        self._connection = mavutil.mavlink_connection(
            settings.mavlink_endpoint,
            baud=settings.mavlink_baud,
            source_system=settings.source_system,
            source_component=settings.source_component,
            dialect="common",
            autoreconnect=True,
        )

    def send_and_wait(self, command: ControlCommand) -> TransportResult:
        mavutil = self._mavutil
        command_code = mavutil.mavlink.MAV_CMD_USER_1
        params = encode_user_command_params(command)

        with self._lock:
            if self._has_sent:
                self._drain_stale_acks(command_code)

            for attempt in range(self._settings.max_attempts):
                if self._settings.debug:
                    logger.debug(
                        "mavlink send endpoint=%s baud=%s command=%s action=%s sequence=%s "
                        "uid=%016x attempt=%s params=%s",
                        self._settings.mavlink_endpoint,
                        self._settings.mavlink_baud,
                        command.command,
                        command.action,
                        command.sequence,
                        command.command_uid,
                        attempt + 1,
                        params,
                    )
                self._connection.mav.command_long_send(
                    self._settings.target_system,
                    self._settings.target_component,
                    command_code,
                    attempt,
                    *params,
                )
                self._has_sent = True

                ack = self._wait_for_ack(command_code)
                if ack is None:
                    logger.warning(
                        "mavlink ack timeout command=%s sequence=%s uid=%016x attempt=%s",
                        command.command,
                        command.sequence,
                        command.command_uid,
                        attempt + 1,
                    )
                    continue

                result_code = int(ack.result)
                result_entry = mavutil.mavlink.enums["MAV_RESULT"].get(result_code)
                result_name = result_entry.name if result_entry is not None else f"MAV_RESULT_{result_code}"
                accepted = result_code == mavutil.mavlink.MAV_RESULT_ACCEPTED
                if self._settings.debug:
                    logger.debug(
                        "mavlink ack received source=%s/%s result=%s attempt=%s",
                        ack.get_srcSystem(),
                        ack.get_srcComponent(),
                        result_name,
                        attempt + 1,
                    )
                return TransportResult(accepted, result_code, result_name, attempt + 1)

        raise TimeoutError(
            f"no COMMAND_ACK after {self._settings.max_attempts} attempts"
        )

    def _wait_for_ack(self, command_code: int):
        while True:
            message = self._connection.recv_match(
                type="COMMAND_ACK",
                blocking=True,
                timeout=self._settings.ack_timeout_seconds,
            )
            if message is None:
                return None
            if int(message.command) == command_code:
                return message

    def _drain_stale_acks(self, command_code: int) -> None:
        while True:
            message = self._connection.recv_match(type="COMMAND_ACK", blocking=False)
            if message is None:
                return
            if int(message.command) != command_code:
                continue


def create_transport(settings: GatewaySettings):
    if settings.dry_run:
        return DryRunTransport()
    return MavlinkCommandTransport(settings)
