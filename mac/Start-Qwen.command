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
"$LMS" server start --port 1234 --bind 127.0.0.1

# `lms status` also succeeds while the server is OFF. Wait for the exact
# installed runtime to be selectable before attempting to load the model.
runtime_ready=0
runtime_result=''
for attempt in {1..30}; do
  if runtime_result=$("$LMS" runtime select "$RUNTIME" 2>&1); then
    print -r -- "$runtime_result"
    runtime_ready=1
    break
  fi
  if (( attempt < 30 )); then sleep 1; fi
done
if (( ! runtime_ready )); then
  print -u2 '未能選用指定嘅 MLX 1.10.0 runtime，模型未有載入。請等 LM Studio 啟動完成後再試。'
  print -ru2 -- "$runtime_result"
  exit 1
fi

if ! "$LMS" ps --json | /usr/bin/grep -Eq '"identifier"[[:space:]]*:[[:space:]]*"qwen3\.8-27b"'; then
  "$LMS" load "$MODEL" --identifier "$MODEL_ID" --context-length 32768 --parallel 1 -y
fi

print ''
print 'Qwen 本地 API 已就緒。'
print 'Base URL: http://127.0.0.1:1234/v1'
print "Model ID: $MODEL_ID"
print '關閉呢個 Terminal 視窗唔會停止 LM Studio。'
