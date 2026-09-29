#!/usr/bin/env python3
"""Generate the board's NVS partition without putting credentials in firmware."""
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from setup_wifi import PRIVATE, validate


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--idf", required=True, type=Path)
    args = parser.parse_args()
    wifi = json.loads((PRIVATE / "wifi.json").read_text())
    bridge = json.loads((PRIVATE / "bridge.json").read_text())
    validate(wifi["ssid"], wifi["password"])
    rows = [
        ["key", "type", "encoding", "value"],
        ["espclaw", "namespace", "", ""],
        ["wifi_ssid", "data", "string", wifi["ssid"]],
        ["wifi_pass", "data", "string", wifi["password"]],
        ["llm_backend", "data", "i32", "4"],
        ["llm_api_key", "data", "string", bridge["token"]],
        ["llm_model", "data", "string", "qwen3.8-27b"],
        ["llm_api_url", "data", "string", f"http://{bridge['bind']}:{bridge['port']}/v1/chat/completions"],
    ]
    output = PRIVATE / "nvs_partition.bin"
    os.umask(0o077)
    with tempfile.NamedTemporaryFile(mode="w", suffix=".csv", dir=PRIVATE, newline="", delete=False) as f:
        temp = Path(f.name)
        csv.writer(f).writerows(rows)
    try:
        generator = args.idf / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
        result = subprocess.run([sys.executable, str(generator), "generate", str(temp), str(output), "0x6000"], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError("NVS generation failed; no credentials printed. Check the ESP-IDF Python environment.")
        output.chmod(0o600)
    finally:
        temp.unlink(missing_ok=True)
    print("NVS partition ready (24 KiB); credentials remain local.")


if __name__ == "__main__":
    main()
