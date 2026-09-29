#!/bin/zsh
set -euo pipefail
ESPCLAW_PYTHON="${ESPCLAW_PYTHON:-python3}"
if ! "$ESPCLAW_PYTHON" -c 'import serial' >/dev/null 2>&1; then
  print -u2 '需要 pyserial：請先啟用 ESP-IDF Python 環境，或執行 python3 -m pip install pyserial。'
  exit 1
fi
if (( $# > 0 )); then
  ESPCLAW_PORT="$1"
else
  espclaw_ports=(/dev/cu.usbmodem*(N))
  if (( ${#espclaw_ports} != 1 )); then
    print -u2 '請連接 XIAO，或指定串口：./ESPClaw-Console.command /dev/cu.usbmodemXXXX'
    exit 1
  fi
  ESPCLAW_PORT="${espclaw_ports[1]}"
fi
print 'ESPClaw: /help 查看指令；/look 拍一張相分析；Ctrl+] 離開。'
exec "$ESPCLAW_PYTHON" -m serial.tools.miniterm "$ESPCLAW_PORT" 115200 --eol LF
