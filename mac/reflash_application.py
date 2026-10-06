"""Update ota_0 on an already configured XIAO ESP32S3 Sense over USB.

Requires this repository's existing partition layout and ota_0 selected at
0x20000. This is not initial provisioning: it does not install a bootloader or
partition table, change the selected OTA slot, or write private Wi-Fi/Qwen NVS.
"""
import argparse
import glob
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
FIRMWARE = ROOT.parent / "firmware" / "espclaw.bin"
MANIFEST = ROOT.parent / "firmware" / "manifest.json"
APP_OFFSET = "0x20000"
APP_CAPACITY = 0x1E0000


def mac_address(value):
    if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", value):
        raise argparse.ArgumentTypeError("Use a MAC address such as aa:bb:cc:dd:ee:ff.")
    return value.lower()


def validate_firmware(firmware, manifest):
    """Validate the repository artifact before opening a serial connection."""
    try:
        metadata = json.loads(manifest.read_text())
        entry = metadata["files"]["espclaw.bin"]
        raw = firmware.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        valid = (
            metadata["board"] == "Seeed XIAO ESP32S3 Sense"
            and int(entry["offset"], 0) == int(APP_OFFSET, 0)
            and type(entry["bytes"]) is int
            and entry["bytes"] == len(raw)
            and 0 < len(raw) <= APP_CAPACITY
            and entry["sha256"] == digest
        )
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise SystemExit("Cannot validate firmware/manifest; nothing written.") from error
    if not valid:
        raise SystemExit("Firmware manifest, checksum, size or offset mismatch; nothing written.")
    return len(raw), digest


def select_port(requested):
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if requested is None:
        if len(ports) != 1:
            raise SystemExit("Connect exactly one XIAO USB device, or supply its port. Nothing written.")
        return ports[0]
    if requested not in ports:
        raise SystemExit("The selected USB port is not present. Nothing written.")
    return requested


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expected-mac", required=True, type=mac_address,
                        help="MAC address of the intended board, obtained independently")
    parser.add_argument("port", nargs="?", help="USB port, if more than one board is connected")
    args = parser.parse_args(argv)
    size, digest = validate_firmware(FIRMWARE, MANIFEST)
    if importlib.util.find_spec("esptool") is None:
        raise SystemExit("esptool is missing. Use an ESP-IDF Python environment or install esptool. Nothing written.")
    port = select_port(args.port)

    private = ROOT / "private"
    private.mkdir(mode=0o700, exist_ok=True)
    log_path = private / "reflash-application.log"
    fd = os.open(log_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.fchmod(fd, 0o600)
    with os.fdopen(fd, "w") as log:
        image_info = f"Firmware SHA256: {digest}; bytes: {size}; offset: {APP_OFFSET}\n"
        print(image_info, end="", flush=True)
        log.write(image_info)
        print("Requires the existing repository partition layout and active ota_0 at 0x20000.", flush=True)
        base = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port]
        print("Checking board identity before writing...", flush=True)
        # Stay in ROM so native USB does not re-enumerate between these commands.
        try:
            identity = subprocess.run(base + ["--no-stub", "--after", "no_reset", "read_mac"],
                                      capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise SystemExit("Board identity check failed; nothing written. Check USB and BOOT mode.") from error
        output = identity.stdout + identity.stderr
        log.write(output)
        log.flush()
        macs = re.findall(r"^MAC:\s*((?:[0-9a-f]{2}:){5}[0-9a-f]{2})\s*$",
                          output, flags=re.I | re.M)
        is_s3 = re.search(r"^Chip(?: is| type:)\s*ESP32-S3\b", output, flags=re.I | re.M)
        if identity.returncode or not is_s3 or [mac.lower() for mac in macs] != [args.expected_mac]:
            print(output, end="")
            raise SystemExit("ESP32-S3/MAC identity was not verified; nothing written. For BOOT mode, hold BOOT, press RESET, release BOOT, then rerun.")
        print("Target verified. Writing only ota_0 at 0x20000; NVS is not modified.", flush=True)
        command = base + ["--before", "no_reset", "--after", "hard_reset",
                          "--baud", "460800", "write_flash", "--flash_mode", "keep",
                          "--flash_freq", "keep", "--flash_size", "keep", APP_OFFSET, str(FIRMWARE)]
        child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        flash_verified = False
        try:
            for line in child.stdout:
                print(line, end="", flush=True)
                log.write(line)
                log.flush()
                # Require esptool's explicit digest confirmation, not only exit 0.
                if line.strip() == "Hash of data verified.":
                    flash_verified = True
        finally:
            child.stdout.close()
        if child.wait() != 0:
            raise SystemExit("Flash failed. Keep USB connected and retry in BOOT mode.")
        if not flash_verified:
            raise SystemExit("Application written, but flash verification was not confirmed. Check the private reflash log before continuing.")
        print("Firmware written and verified by esptool. Wait for Wi-Fi, then open http://127.0.0.1:8787/", flush=True)


if __name__ == "__main__":
    main()
