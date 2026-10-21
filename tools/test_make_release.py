"""Tests for tools/make_release.py (run from tools/: python3 -m unittest test_make_release).

Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

import hashlib
import json
import pathlib
import tempfile
import unittest

import make_release


class MakeReleaseTest(unittest.TestCase):
    def test_manifests(self):
        with tempfile.TemporaryDirectory() as tmp:
            images, site = pathlib.Path(tmp, "images"), pathlib.Path(tmp, "site")
            images.mkdir()
            for variant in make_release.VARIANTS:
                for kind in ("factory", "ota"):
                    pathlib.Path(images, f"precision-feeder-{variant}.{kind}.bin").write_bytes(
                        f"{variant}-{kind}".encode())
            m = make_release.make(images, site, "9.8.7")
            self.assertEqual(json.loads((site / "manifest.json").read_text()), m)
            self.assertEqual(m["version"], "9.8.7")
            self.assertTrue(m["notes"].endswith("/releases/tag/v9.8.7"))
            ota = m["variants"]["no-ble"]["ota"]
            self.assertEqual(ota["name"], "precision-feeder-no-ble-9.8.7.ota.bin")
            self.assertEqual(ota["sha256"], hashlib.sha256(b"no-ble-ota").hexdigest())
            self.assertEqual(ota["size"], len(b"no-ble-ota"))
            install = json.loads((site / "install-full.json").read_text())
            self.assertEqual(install["builds"][0]["chipFamily"], "ESP32-S3")
            self.assertEqual(install["builds"][0]["parts"], [{"path": "precision-feeder-full-9.8.7.factory.bin",
                                                              "offset": 0}])
            self.assertTrue((site / "index.html").exists())

    def test_release_notes(self):
        log = "# Changelog\n\n## 2.0.0: appliance\n\n- a\n- b\n\n## 1.1.0: local first\n\n- c\n"
        self.assertEqual(make_release.release_notes("2.0.0", log), "- a\n- b\n")
        self.assertEqual(make_release.release_notes("1.1.0", log), "- c\n")
        self.assertEqual(make_release.release_notes("1.1", log), "")
        self.assertEqual(make_release.release_notes("3.0.0", log), "")
        # An unreleased version's changes come along with the release that ships them.
        log = ("## 2.0.1: fixed (2026-10-16)\n\n- fix\n\n## 2.0.0: big (not released; see 2.0.1)\n\n- feature\n\n"
               "## 1.1.0: older\n\n- old\n")
        self.assertEqual(make_release.release_notes("2.0.1", log),
                         "- fix\n\n## 2.0.0: big (not released; see 2.0.1)\n\n- feature\n")
        self.assertEqual(make_release.release_notes("1.1.0", log), "- old\n")

    def test_links(self):
        self.assertEqual(make_release.absolute_links("see [a](docs/A.md), [b](https://x.y/z), [c](#top)", "9.8.7"),
                         f"see [a]({make_release.REPO}/blob/v9.8.7/docs/A.md), [b](https://x.y/z), [c](#top)")

    def test_version_matches_the_firmware(self):
        # A release ("2.1.0"), or a branch variant ("2.1.0-n20.1"), which make_release refuses to publish.
        self.assertRegex(make_release.fw_version(), r"^\d+\.\d+\.\d+(-[a-z0-9]+\.\d+)?$")


if __name__ == "__main__":
    unittest.main()
