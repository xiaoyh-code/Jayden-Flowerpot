#!/bin/zsh
set -euo pipefail
webui_dir="${0:A:h}"
webui_py="$webui_dir/webui_server.py"
bridge_pid=''
webui_pid=''

cleanup() {
  if [[ -n "$webui_pid" ]]; then kill "$webui_pid" 2>/dev/null || true; fi
  if [[ -n "$bridge_pid" ]]; then kill "$bridge_pid" 2>/dev/null || true; fi
}
trap cleanup EXIT INT TERM

if ! python3 "$webui_py" check model; then
  "$webui_dir/Start-Qwen.command"
fi

if ! python3 "$webui_py" check bridge; then
  python3 "$webui_dir/espclaw_bridge.py" serve &
  bridge_pid=$!
  bridge_ready=0
  for attempt in {1..20}; do
    if python3 "$webui_py" check bridge; then bridge_ready=1; break; fi
    sleep 0.25
  done
  if (( ! bridge_ready )); then
    print -u2 '本機橋接器未能啟動，請檢查 bridge.json 的 LAN IP。'
    exit 1
  fi
fi

if python3 "$webui_py" check webui; then
  open 'http://127.0.0.1:8787'
  print 'ESPClaw WebUI 已經啟動，模型及橋接器已檢查，已打開瀏覽器。'
  if [[ -n "$bridge_pid" ]]; then
    print '保留此終端視窗；按 Control-C 只會停止本次補開的橋接器，既有 WebUI 會繼續運行。'
    wait "$bridge_pid"
    bridge_pid=''
  fi
  exit 0
fi

python3 "$webui_py" serve &
webui_pid=$!
webui_ready=0
for attempt in {1..20}; do
  if python3 "$webui_py" check webui; then webui_ready=1; break; fi
  sleep 0.25
done
if (( ! webui_ready )); then
  print -u2 'WebUI 未能啟動，請檢查 8787 埠是否被使用。'
  exit 1
fi
open 'http://127.0.0.1:8787'
print '保留此終端視窗；按 Control-C 停止本次啟動的 WebUI 和橋接器。'
wait "$webui_pid"
