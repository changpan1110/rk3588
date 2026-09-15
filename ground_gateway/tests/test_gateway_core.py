from __future__ import annotations

import time
import unittest
import uuid

from ground_gateway.gateway_core import (
    ClientSequenceTracker,
    CommandResultCache,
    CommandValidationError,
    ControlCommand,
    decode_user_command_params,
    encode_user_command_params,
)


def payload(**updates):
    value = {
        "version": 1,
        "type": "command",
        "client_id": str(uuid.uuid4()),
        "command_id": str(uuid.uuid4()),
        "sequence": 1,
        "command": "camera.snapshot",
        "params": {},
        "sent_at_ms": int(time.time() * 1000),
    }
    value.update(updates)
    return value


class ControlCommandTests(unittest.TestCase):
    def test_encode_decode_round_trip(self):
        command = ControlCommand.from_payload(
            payload(
                command="camera.zoom_in",
                event="press",
                sequence=0x89ABCDEF,
            ),
            3000,
        )
        self.assertEqual(
            decode_user_command_params(encode_user_command_params(command)),
            (
                command.action,
                command.event_code,
                command.value,
                command.sequence,
                command.command_uid,
            ),
        )

    def test_hold_command_requires_press_or_release(self):
        with self.assertRaisesRegex(CommandValidationError, "requires press or release"):
            ControlCommand.from_payload(payload(command="camera.zoom_in"), 3000)

    def test_pseudocolor_mode_is_encoded(self):
        command = ControlCommand.from_payload(
            payload(command="thermal.pseudocolor", params={"mode": 14}),
            3000,
        )
        self.assertEqual(command.value, 14)

    def test_toggle_requires_boolean_enabled(self):
        with self.assertRaisesRegex(CommandValidationError, "boolean enabled"):
            ControlCommand.from_payload(
                payload(command="osd.enabled", params={"enabled": 1}),
                3000,
            )

    def test_continuous_measure_start_and_stop_are_encoded(self):
        start = ControlCommand.from_payload(
            payload(command="laser.continuous_measure", params={"enabled": True}),
            3000,
        )
        stop = ControlCommand.from_payload(
            payload(command="laser.continuous_measure", params={"enabled": False}),
            3000,
        )
        self.assertEqual(start.value, 1)
        self.assertEqual(stop.value, 0)

    def test_expired_command_is_rejected(self):
        with self.assertRaisesRegex(CommandValidationError, "expired"):
            ControlCommand.from_payload(payload(sent_at_ms=int(time.time() * 1000) - 4000), 3000)

    def test_unknown_command_is_rejected(self):
        with self.assertRaisesRegex(CommandValidationError, "unsupported"):
            ControlCommand.from_payload(payload(command="camera.unknown"), 3000)


class ReplayProtectionTests(unittest.TestCase):
    def test_sequence_must_increase_per_client(self):
        tracker = ClientSequenceTracker()
        client_id = str(uuid.uuid4())
        self.assertTrue(tracker.accept(client_id, 1))
        self.assertFalse(tracker.accept(client_id, 1))
        self.assertFalse(tracker.accept(client_id, 0))
        self.assertTrue(tracker.accept(client_id, 2))

    def test_cached_ack_is_copied(self):
        cache = CommandResultCache()
        result = {"ok": True}
        cache.put("command-id", result)
        cached = cache.get("command-id")
        self.assertEqual(cached, result)
        self.assertIsNot(cached, result)


if __name__ == "__main__":
    unittest.main()
