#!/bin/zsh
# Update the existing ota_0 application only; leave private NVS intact.
set -euo pipefail
reflash_dir="${0:A:h}"
reflash_python="${ESPCLAW_PYTHON:-python3}"
if ! "$reflash_python" -c 'import esptool' >/dev/null 2>&1; then
  print -u2 '需要 esptool：請先啟用 ESP-IDF Python 環境，或設定 ESPCLAW_PYTHON 指向已安裝 esptool 嘅 Python。'
  exit 1
fi
exec "$reflash_python" "$reflash_dir/reflash_application.py" "$@"
