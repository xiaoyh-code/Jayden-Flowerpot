"""No-hardware tests for the public app-only flashing guardrails."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

import reflash_application as reflash


class ReflashTests(unittest.TestCase):
    MAC = "aa:bb:cc:dd:ee:ff"
    PORT = "/dev/cu.usbmodemTEST"

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.firmware = self.root / "espclaw.bin"
        self.manifest = self.root / "manifest.json"
        self.firmware.write_bytes(b"test firmware")
        self.metadata = {
            "board": "Seeed XIAO ESP32S3 Sense",
            "files": {"espclaw.bin": {
                "offset": "0x20000", "bytes": len(b"test firmware"),
                "sha256": hashlib.sha256(b"test firmware").hexdigest(),
            }},
        }
        self.save_manifest()
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        for name, value in (("ROOT", self.root), ("FIRMWARE", self.firmware), ("MANIFEST", self.manifest)):
            self.stack.enter_context(patch.object(reflash, name, value))
        self.find_spec = self.stack.enter_context(patch.object(reflash.importlib.util, "find_spec", return_value=object()))
        self.ports = self.stack.enter_context(patch.object(reflash.glob, "glob", return_value=[self.PORT]))
        self.identity = self.stack.enter_context(patch.object(reflash.subprocess, "run", return_value=subprocess.CompletedProcess(
            [], 0, f"Chip is ESP32-S3 (QFN56)\nMAC: {self.MAC}\n", "")))
        self.child = Mock(stdout=io.StringIO("Writing...\nHash of data verified.\n"))
        self.child.wait.return_value = 0
        self.flash = self.stack.enter_context(patch.object(reflash.subprocess, "Popen", return_value=self.child))
        self.stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        self.stack.enter_context(contextlib.redirect_stderr(io.StringIO()))

    def save_manifest(self):
        self.manifest.write_text(json.dumps(self.metadata))

    def run_main(self, *extra):
        reflash.main(["--expected-mac", self.MAC, *extra])

    def assert_rejected_before_serial(self):
        with self.assertRaises(SystemExit):
            self.run_main()
        self.identity.assert_not_called()
        self.flash.assert_not_called()

    def test_valid_flash_writes_only_application_and_verifies_digest(self):
        self.run_main()
        identity_command = self.identity.call_args.args[0]
        self.assertEqual(identity_command[-4:], ["--no-stub", "--after", "no_reset", "read_mac"])
        command = self.flash.call_args.args[0]
        self.assertEqual(command[-2:], ["0x20000", str(self.firmware)])
        self.assertEqual(command.count("write_flash"), 1)
        self.assertEqual(command[command.index("--chip") + 1], "esp32s3")
        self.assertEqual(command[command.index("--before") + 1], "no_reset")
        self.assertEqual(command[command.index("--after") + 1], "hard_reset")
        self.assertEqual(command[command.index("write_flash") + 1:-2],
                         ["--flash_mode", "keep", "--flash_freq", "keep", "--flash_size", "keep"])
        self.assertEqual(stat.S_IMODE((self.root / "private/reflash-application.log").stat().st_mode), 0o600)

    def test_esptool5_identity_and_uppercase_requested_mac(self):
        self.identity.return_value.stdout = f"Chip type: ESP32-S3 (QFN56)\nMAC: {self.MAC.upper()}\n"
        reflash.main(["--expected-mac", self.MAC.upper()])
        self.flash.assert_called_once()

    def test_mac_is_required_and_must_be_well_formed(self):
        for argv in ([], ["--expected-mac", "not-a-mac"]):
            with self.subTest(argv=argv), self.assertRaises(SystemExit):
                reflash.main(argv)
        self.identity.assert_not_called()
        self.flash.assert_not_called()

    def test_artifact_tamper_is_rejected(self):
        self.firmware.write_bytes(b"tampered image")
        self.assert_rejected_before_serial()

    def test_invalid_manifest_fields_are_rejected(self):
        for field, value in (("offset", "0x9000"), ("offset", "invalid"), ("bytes", 1),
                             ("bytes", True), ("sha256", "0" * 64)):
            with self.subTest(field=field, value=value):
                original = self.metadata["files"]["espclaw.bin"][field]
                self.metadata["files"]["espclaw.bin"][field] = value
                self.save_manifest()
                self.assert_rejected_before_serial()
                self.metadata["files"]["espclaw.bin"][field] = original

    def test_wrong_board_rejected(self):
        self.metadata["board"] = "ESP32-C3"
        self.save_manifest()
        self.assert_rejected_before_serial()

    def test_missing_or_malformed_manifest_rejected(self):
        for content in ("{broken", "{}", "[]"):
            with self.subTest(content=content):
                self.manifest.write_text(content)
                self.assert_rejected_before_serial()
        self.manifest.unlink()
        self.assert_rejected_before_serial()

    def test_empty_or_oversized_image_rejected_even_with_matching_digest(self):
        for size in (0, reflash.APP_CAPACITY + 1):
            with self.subTest(size=size):
                raw = b"x" * size
                self.firmware.write_bytes(raw)
                self.metadata["files"]["espclaw.bin"].update(bytes=size, sha256=hashlib.sha256(raw).hexdigest())
                self.save_manifest()
                self.assert_rejected_before_serial()

    def test_missing_esptool_rejected(self):
        self.find_spec.return_value = None
        self.assert_rejected_before_serial()

    def test_missing_or_ambiguous_usb_rejected(self):
        for ports in ([], [self.PORT, "/dev/cu.usbmodemSECOND"]):
            with self.subTest(ports=ports):
                self.ports.return_value = ports
                self.assert_rejected_before_serial()

    def test_unknown_explicit_port_rejected(self):
        with self.assertRaises(SystemExit):
            self.run_main("/dev/cu.usbmodemUNKNOWN")
        self.identity.assert_not_called()
        self.flash.assert_not_called()

    def test_explicit_port_selects_one_of_multiple_boards(self):
        self.ports.return_value = [self.PORT, "/dev/cu.usbmodemSECOND"]
        self.run_main(self.PORT)
        command = self.identity.call_args.args[0]
        self.assertEqual(command[command.index("--port") + 1], self.PORT)

    def test_wrong_missing_or_ambiguous_identity_never_writes(self):
        for output in ("Chip is ESP32-S3\nMAC: 00:11:22:33:44:55\n",
                       f"Chip is ESP32-C3\nMAC: {self.MAC}\n",
                       f"MAC: {self.MAC}\n", "Chip is ESP32-S3\n",
                       f"Chip is ESP32-S3\nMAC: {self.MAC}\nMAC: 00:11:22:33:44:55\n"):
            with self.subTest(output=output):
                self.identity.return_value.stdout = output
                with self.assertRaises(SystemExit):
                    self.run_main()
                self.flash.assert_not_called()

    def test_identity_failure_never_writes(self):
        self.identity.return_value.returncode = 1
        with self.assertRaises(SystemExit):
            self.run_main()
        self.flash.assert_not_called()

    def test_identity_timeout_never_writes(self):
        self.identity.side_effect = subprocess.TimeoutExpired("esptool", 60)
        with self.assertRaises(SystemExit):
            self.run_main()
        self.flash.assert_not_called()

    def test_failed_write_is_not_success_even_with_digest_message(self):
        self.child.wait.return_value = 2
        with self.assertRaisesRegex(SystemExit, "Flash failed"):
            self.run_main()

    def test_success_exit_without_digest_confirmation_is_rejected(self):
        self.child.stdout = io.StringIO("Writing...\nDone.\n")
        with self.assertRaisesRegex(SystemExit, "verification was not confirmed"):
            self.run_main()


if __name__ == "__main__":
    unittest.main()
