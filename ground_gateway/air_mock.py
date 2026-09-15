from __future__ import annotations

import argparse
import math
from collections import OrderedDict

from pymavlink import mavutil


ACTIONS = {
    1: "video.switch",
    2: "camera.snapshot",
    3: "record.toggle",
    4: "camera.zoom_in",
    5: "camera.zoom_out",
    6: "device.trigger",
    7: "camera.focus_in",
    8: "camera.focus_out",
    9: "device.unlock",
    10: "thermal.pseudocolor",
    11: "laser.single_measure",
    12: "laser.continuous_measure",
    13: "osd.enabled",
}

EVENTS = {0: "click", 1: "press", 2: "release"}


def decode_params(message) -> tuple[int, int, int, int, int]:
    raw_values = [
        message.param1,
        message.param2,
        message.param3,
        message.param4,
        message.param5,
        message.param6,
        message.param7,
    ]
    values: list[int] = []
    for value in raw_values:
        if not math.isfinite(value) or value < 0 or value > 65535 or int(value) != value:
            raise ValueError("invalid uint16 command parameter")
        values.append(int(value))

    control_word = values[0]
    action = control_word & 0xFF
    event_code = (control_word >> 8) & 0x03
    value = (control_word >> 10) & 0x3F
    sequence = values[1] | (values[2] << 16)
    command_uid = (
        values[3]
        | (values[4] << 16)
        | (values[5] << 32)
        | (values[6] << 48)
    )
    return action, event_code, value, sequence, command_uid


def send_ack(connection, message, result: int) -> None:
    connection.mav.command_ack_send(
        mavutil.mavlink.MAV_CMD_USER_1,
        result,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="RK3588 airborne MAVLink command mock")
    parser.add_argument("--listen", default="udpin:0.0.0.0:14550")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--system", type=int, default=1)
    parser.add_argument("--component", type=int, default=191)
    args = parser.parse_args()

    connection = mavutil.mavlink_connection(
        args.listen,
        baud=args.baud,
        source_system=args.system,
        source_component=args.component,
        dialect="common",
    )
    results: OrderedDict[tuple[int, int, int], int] = OrderedDict()
    print(
        f"Listening on {args.listen}; baud={args.baud}; "
        f"system={args.system}, component={args.component}",
        flush=True,
    )

    while True:
        message = connection.recv_match(type="COMMAND_LONG", blocking=True)
        if message.command != mavutil.mavlink.MAV_CMD_USER_1:
            continue
        if message.target_system not in (0, args.system):
            continue
        if message.target_component not in (0, args.component):
            continue

        try:
            action, event_code, value, sequence, command_uid = decode_params(message)
            cache_key = (message.get_srcSystem(), message.get_srcComponent(), command_uid)
            duplicate = cache_key in results
            if duplicate:
                result = results[cache_key]
            elif action in ACTIONS:
                result = mavutil.mavlink.MAV_RESULT_ACCEPTED
                results[cache_key] = result
                while len(results) > 64:
                    results.popitem(last=False)
            else:
                result = mavutil.mavlink.MAV_RESULT_UNSUPPORTED

            print(
                f"command={ACTIONS.get(action, action)} "
                f"event={EVENTS.get(event_code, event_code)} value={value} sequence={sequence} "
                f"uid={command_uid:016x} duplicate={duplicate} result={result}",
                flush=True,
            )
        except ValueError as exc:
            result = mavutil.mavlink.MAV_RESULT_FAILED
            print(f"rejected command: {exc}", flush=True)

        send_ack(connection, message, result)


if __name__ == "__main__":
    main()
