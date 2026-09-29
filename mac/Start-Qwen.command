#!/bin/zsh
set -euo pipefail

LMS='/Applications/LM Studio.app/Contents/Resources/app/.webpack/lms'
MODEL='qwen/qwen3.8-27b'
MODEL_ID='qwen3.8-27b'
RUNTIME='mlx-llm-mac-arm64-apple-metal-nax-advsimd@1.10.0'

if [[ ! -x "$LMS" ]]; then
  print -u2 '搵唔到 LM Studio，請先安裝 LM Studio。'
  exit 1
fi

open -g -a 'LM Studio'
ready=0
for attempt in {1..30}; do
  if "$LMS" status >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 1
done
if (( ! ready )); then
  print -u2 'LM Studio 未能啟動，請打開主程式再試。'
  exit 1
fi

"$LMS" server start --port 1234 --bind 127.0.0.1
"$LMS" runtime select "$RUNTIME"
if ! "$LMS" ps --json | /usr/bin/grep -Eq '"identifier"[[:space:]]*:[[:space:]]*"qwen3\.8-27b"'; then
  "$LMS" load "$MODEL" --identifier "$MODEL_ID" --context-length 32768 --parallel 1 -y
fi

print ''
print 'Qwen 本地 API 已就緒。'
print 'Base URL: http://127.0.0.1:1234/v1'
print "Model ID: $MODEL_ID"
print '關閉呢個 Terminal 視窗唔會停止 LM Studio。'
