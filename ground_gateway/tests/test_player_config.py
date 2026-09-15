from __future__ import annotations

import argparse
import json
import tempfile
import unittest
from pathlib import Path

from scripts.service_manager import load_runtime_settings


BASE_CONFIG = {
    "video": {"url": "http://192.168.31.14:8889/live/"},
    "web": {"port": 8090},
    "gateway": {"port": 8091, "debug": False},
    "mavlink": {
        "transport": "udp",
        "udp": {"mode": "out", "host": "192.168.31.14", "port": 14550},
        "tcp": {"mode": "client", "host": "192.168.31.14", "port": 5760},
        "serial": {"device": "COM5", "baud": 115200},
    },
}


def arguments(config_path: Path) -> argparse.Namespace:
    return argparse.Namespace(
        config=str(config_path),
        air_ip="",
        web_port=0,
        gateway_port=0,
        mavlink_port=0,
        mavlink_serial="",
        mavlink_baud=0,
        debug_control=False,
    )


class PlayerConfigTests(unittest.TestCase):
    def load(self, transport: str):
        config = json.loads(json.dumps(BASE_CONFIG))
        config["mavlink"]["transport"] = transport
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "player.json"
            path.write_text(json.dumps(config), encoding="utf-8")
            return load_runtime_settings(arguments(path))

    def test_udp_transport(self):
        settings = self.load("udp")
        self.assertEqual(settings.mavlink_endpoint, "udpout:192.168.31.14:14550")

    def test_tcp_transport(self):
        settings = self.load("tcp")
        self.assertEqual(settings.mavlink_endpoint, "tcp:192.168.31.14:5760")

    def test_serial_transport(self):
        settings = self.load("serial")
        self.assertEqual(settings.mavlink_endpoint, "COM5")
        self.assertEqual(settings.mavlink_baud, 115200)


if __name__ == "__main__":
    unittest.main()
