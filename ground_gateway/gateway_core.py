from __future__ import annotations

import time
import uuid
from collections import OrderedDict
from dataclasses import dataclass
from typing import Any


ACTION_BY_COMMAND = {
    "video.switch": 1,
    "camera.snapshot": 2,
    "record.toggle": 3,
    "camera.zoom_in": 4,
    "camera.zoom_out": 5,
    "device.trigger": 6,
    "camera.focus_in": 7,
    "camera.focus_out": 8,
    "device.unlock": 9,
    "thermal.pseudocolor": 10,
    "laser.single_measure": 11,
    "laser.continuous_measure": 12,
    "osd.enabled": 13,
}

EVENT_BY_NAME = {
    "click": 0,
    "press": 1,
    "release": 2,
}

HOLD_COMMANDS = {
    "camera.zoom_in",
    "camera.zoom_out",
    "camera.focus_in",
    "camera.focus_out",
}

CONTROL_ACTION_MASK = 0x00FF
CONTROL_EVENT_SHIFT = 8
CONTROL_EVENT_MASK = 0x03
CONTROL_VALUE_SHIFT = 10
CONTROL_VALUE_MASK = 0x3F


class CommandValidationError(ValueError):
    pass


@dataclass(frozen=True)
class ControlCommand:
    client_id: str
    command_id: str
    sequence: int
    command: str
    action: int
    event: str
    event_code: int
    value: int
    command_uid: int
    sent_at_ms: int
    params: dict[str, Any]

    @classmethod
    def from_payload(cls, payload: dict[str, Any], max_age_ms: int) -> "ControlCommand":
        if payload.get("type") != "command":
            raise CommandValidationError("message type must be command")

        try:
            client_uuid = uuid.UUID(str(payload["client_id"]))
            command_uuid = uuid.UUID(str(payload["command_id"]))
            sequence = int(payload["sequence"])
            sent_at_ms = int(payload["sent_at_ms"])
            command = str(payload["command"])
            event = str(payload.get("event", "click"))
        except (KeyError, TypeError, ValueError) as exc:
            raise CommandValidationError("invalid command envelope") from exc

        if sequence < 1 or sequence > 0xFFFFFFFF:
            raise CommandValidationError("sequence must be between 1 and 4294967295")

        if command not in ACTION_BY_COMMAND:
            raise CommandValidationError(f"unsupported command: {command}")
        if event not in EVENT_BY_NAME:
            raise CommandValidationError(f"unsupported command event: {event}")
        if command in HOLD_COMMANDS and event not in {"press", "release"}:
            raise CommandValidationError(f"{command} requires press or release event")
        if command not in HOLD_COMMANDS and event != "click":
            raise CommandValidationError(f"{command} only supports click event")

        now_ms = int(time.time() * 1000)
        age_ms = now_ms - sent_at_ms
        if age_ms > max_age_ms:
            raise CommandValidationError("command expired")
        if age_ms < -5000:
            raise CommandValidationError("command timestamp is too far in the future")

        params = payload.get("params") or {}
        if not isinstance(params, dict):
            raise CommandValidationError("params must be an object")

        value = _command_value(command, params)

        return cls(
            client_id=str(client_uuid),
            command_id=str(command_uuid),
            sequence=sequence,
            command=command,
            action=ACTION_BY_COMMAND[command],
            event=event,
            event_code=EVENT_BY_NAME[event],
            value=value,
            command_uid=command_uuid.int & 0xFFFFFFFFFFFFFFFF,
            sent_at_ms=sent_at_ms,
            params=params,
        )


def _command_value(command: str, params: dict[str, Any]) -> int:
    if command == "thermal.pseudocolor":
        try:
            mode = int(params["mode"])
        except (KeyError, TypeError, ValueError) as exc:
            raise CommandValidationError("thermal.pseudocolor requires integer mode") from exc
        if mode < 0 or mode > 14:
            raise CommandValidationError("pseudocolor mode must be between 0 and 14")
        return mode

    if command in {"laser.continuous_measure", "osd.enabled"}:
        enabled = params.get("enabled")
        if not isinstance(enabled, bool):
            raise CommandValidationError(f"{command} requires boolean enabled")
        return 1 if enabled else 0

    return 0


def encode_user_command_params(command: ControlCommand) -> tuple[float, ...]:
    sequence_low = command.sequence & 0xFFFF
    sequence_high = (command.sequence >> 16) & 0xFFFF
    uid = command.command_uid
    control_word = (
        (command.action & CONTROL_ACTION_MASK)
        | ((command.event_code & CONTROL_EVENT_MASK) << CONTROL_EVENT_SHIFT)
        | ((command.value & CONTROL_VALUE_MASK) << CONTROL_VALUE_SHIFT)
    )

    return (
        float(control_word),
        float(sequence_low),
        float(sequence_high),
        float(uid & 0xFFFF),
        float((uid >> 16) & 0xFFFF),
        float((uid >> 32) & 0xFFFF),
        float((uid >> 48) & 0xFFFF),
    )


def decode_user_command_params(params: tuple[float, ...]) -> tuple[int, int, int, int, int]:
    if len(params) != 7:
        raise ValueError("MAVLink command requires exactly seven parameters")

    values = [int(value) for value in params]
    if any(value < 0 or value > 0xFFFF for value in values):
        raise ValueError("encoded MAVLink parameter is outside uint16 range")

    control_word = values[0]
    action = control_word & CONTROL_ACTION_MASK
    event_code = (control_word >> CONTROL_EVENT_SHIFT) & CONTROL_EVENT_MASK
    value = (control_word >> CONTROL_VALUE_SHIFT) & CONTROL_VALUE_MASK
    sequence = values[1] | (values[2] << 16)
    command_uid = values[3] | (values[4] << 16) | (values[5] << 32) | (values[6] << 48)
    return action, event_code, value, sequence, command_uid


class CommandResultCache:
    def __init__(self, max_entries: int = 2048, ttl_seconds: float = 600.0) -> None:
        self._max_entries = max_entries
        self._ttl_seconds = ttl_seconds
        self._entries: OrderedDict[str, tuple[float, dict[str, Any]]] = OrderedDict()

    def get(self, command_id: str) -> dict[str, Any] | None:
        self._purge()
        entry = self._entries.get(command_id)
        if entry is None:
            return None
        self._entries.move_to_end(command_id)
        return dict(entry[1])

    def put(self, command_id: str, result: dict[str, Any]) -> None:
        self._purge()
        self._entries[command_id] = (time.monotonic(), dict(result))
        self._entries.move_to_end(command_id)
        while len(self._entries) > self._max_entries:
            self._entries.popitem(last=False)

    def _purge(self) -> None:
        cutoff = time.monotonic() - self._ttl_seconds
        while self._entries:
            first_key = next(iter(self._entries))
            if self._entries[first_key][0] >= cutoff:
                break
            self._entries.popitem(last=False)


class ClientSequenceTracker:
    def __init__(self, max_clients: int = 256) -> None:
        self._max_clients = max_clients
        self._sequences: OrderedDict[str, int] = OrderedDict()

    def accept(self, client_id: str, sequence: int) -> bool:
        previous = self._sequences.get(client_id)
        if previous is not None and sequence <= previous:
            return False

        self._sequences[client_id] = sequence
        self._sequences.move_to_end(client_id)
        while len(self._sequences) > self._max_clients:
            self._sequences.popitem(last=False)
        return True
