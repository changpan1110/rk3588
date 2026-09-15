from __future__ import annotations

import copy
import json
import tempfile
import unittest
from pathlib import Path

from ground_gateway.admin_config import (
    PlayerConfigError,
    restart_required_fields,
    save_player_config,
    validate_player_config,
)


BASE_CONFIG = {
    "video": {"url": "http://192.168.31.14:8889/live/"},
    "web": {"port": 8090},
    "gateway": {"port": 8091, "debug": True},
    "mavlink": {
        "transport": "udp",
        "udp": {"mode": "out", "host": "192.168.31.14", "port": 14550},
        "tcp": {"mode": "client", "host": "192.168.31.14", "port": 5760},
        "serial": {"device": "/dev/ttyUSB0", "baud": 115200},
    },
}


class AdminConfigTests(unittest.TestCase):
    def test_video_update_does_not_require_restart(self):
        updated = copy.deepcopy(BASE_CONFIG)
        updated["video"]["url"] = "https://example.test/live/"
        normalized = validate_player_config(updated)
        self.assertEqual(restart_required_fields(BASE_CONFIG, normalized), [])

    def test_transport_update_requires_restart(self):
        updated = copy.deepcopy(BASE_CONFIG)
        updated["mavlink"]["transport"] = "serial"
        self.assertEqual(
            restart_required_fields(BASE_CONFIG, validate_player_config(updated)),
            ["mavlink"],
        )

    def test_invalid_config_is_rejected(self):
        invalid = copy.deepcopy(BASE_CONFIG)
        invalid["video"]["url"] = "ftp://example.test/live/"
        with self.assertRaisesRegex(PlayerConfigError, "video.url"):
            validate_player_config(invalid)

    def test_save_writes_backup_and_runtime_config(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config_path = root / "config" / "player.json"
            web_dir = root / "web_player"
            config_path.parent.mkdir()
            web_dir.mkdir()
            (web_dir / "index.html").write_text("<html>", encoding="utf-8")
            config_path.write_text(json.dumps(BASE_CONFIG), encoding="utf-8")

            updated = copy.deepcopy(BASE_CONFIG)
            updated["video"]["url"] = "http://10.0.0.4:8889/live/"
            saved, restart_fields = save_player_config(updated, config_path, web_dir)

            self.assertEqual(saved["video"]["url"], updated["video"]["url"])
            self.assertEqual(restart_fields, [])
            self.assertTrue(config_path.with_suffix(".json.bak").is_file())
            runtime = json.loads((web_dir / "runtime-config.json").read_text(encoding="utf-8"))
            self.assertEqual(runtime["video"]["url"], updated["video"]["url"])
            self.assertIn("PLAYER_RUNTIME_CONFIG", (web_dir / "runtime-config.js").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
