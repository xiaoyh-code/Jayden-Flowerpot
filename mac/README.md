# Mac 設定與使用

以下指令喺 **Jayden-Flowerpot 專案根目錄** 執行。先按 [主 README](../README.md) 用 ESP-IDF v5.5.5 編譯及燒錄韌體。

## 1. 準備本地 Qwen

先安裝 LM Studio 到 `/Applications/LM Studio.app`，並下載模型及 runtime。隨附嘅 [Start-Qwen.command](Start-Qwen.command) 使用：

| 項目 | 設定 |
| --- | --- |
| 模型 | `qwen/qwen3.8-27b` |
| API model ID | `qwen3.8-27b` |
| MLX runtime | `mlx-llm-mac-arm64-apple-metal-nax-advsimd@1.10.0` |
| Context | 32,768 tokens |
| 同時處理 | 1 個請求 |
| API | `http://127.0.0.1:1234/v1` |

呢個啟動器係 Apple Silicon Mac 嘅既定配置，唔會自動下載 LM Studio、模型或 runtime。請先確認硬件同記憶體足夠載入你下載嘅模型；其他硬件要選擇相容 runtime。更換模型時，亦要同步修改橋接器及 NVS 產生器使用嘅 model ID。

```sh
./mac/Start-Qwen.command
```

LM Studio API 保持只聽 `127.0.0.1`，唔需要開啟對外網絡存取。

## 2. 設定 Wi-Fi 同 Mac 橋接器

ESP32 要用 **2.4 GHz Wi-Fi**，Mac 同 ESP32 要喺可以互相連線嘅同一個 LAN。以下全部係例子，請換成自己嘅 IP：

| 裝置 | 例子 | 用途 |
| --- | --- | --- |
| Mac | `192.168.1.10` | 橋接器 `--bind`，ESP32 會連去呢個位址 |
| ESP32 | `192.168.1.50` | WebUI「設定」入面嘅裝置 IP |

先建立橋接器設定；程式會產生隨機 token，唔會覆寫已有設定：

```sh
python3 mac/espclaw_bridge.py init --bind 192.168.1.10
python3 mac/setup_wifi.py
```

打開 Terminal 顯示嘅 `http://127.0.0.1:8766/`，填寫 Wi-Fi 名稱同密碼。儲存後按 Control-C 關閉設定頁服務。

兩個步驟會建立 `mac/private/bridge.json` 同 `mac/private/wifi.json`，檔案權限為 `0600`。唔好將呢個資料夾或其內容上傳 GitHub。

## 3. 將設定寫入 ESP32

喺已啟用 ESP-IDF 嘅 Terminal 執行；`ESPCLAW_PORT` 要換成板嘅實際 USB 埠：

```sh
. /path/to/esp-idf-v5.5.5/export.sh
ESPCLAW_PORT='/dev/cu.usbmodemXXXX'
python3 mac/prepare_nvs.py --idf "$IDF_PATH"
python -m esptool --chip esp32s3 --port "$ESPCLAW_PORT" \
  write_flash 0x9000 mac/private/nvs_partition.bin
idf.py -B build.xiao_s3_sense -p "$ESPCLAW_PORT" monitor
```

呢步會取代板上嘅 NVS 設定。產生器會寫入 Wi-Fi、橋接 token、`qwen3.8-27b` 同完整 API URL，例如 `http://192.168.1.10:1235/v1/chat/completions`。使用本專案 8 MB partition table 時，NVS 位址係 `0x9000`、大小係 `0x6000`。

板重啟連線後，喺 serial log 搵到 ESP32 獲派嘅 IP。按 Control-] 離開 monitor；如未重啟，可按板上 Reset。

## 4. 打開 WebUI

```sh
./mac/Start-ESPClaw-WebUI.command
```

瀏覽器會打開 `http://127.0.0.1:8787`。第一次請打開「設定」，將裝置 IP 改成上一步 log 顯示嘅 **ESP32 IP**。等 ESP32、橋接器及模型狀態就緒，就可以使用。

- **開始直播**：只用相機；每次最多 10 分鐘，唔會自動分析或錄影。畫面同時顯示目標及實際 FPS。
- **拍一張**：停止直播後，手動拍一張相片。
- **拍照並分析**：填問題，再拍攝新相片交畀本地 Qwen。
- **文字對話**：經 ESP32 agent 回答，需要 ESP32、橋接器及模型都在線。
- **相機調校**：停止直播後儲存；設定會喺下一次手動拍照或開始直播時套用。

保留啟動器 Terminal。按 Control-C 只會停止今次由啟動器開啟嘅 WebUI／橋接器；既有服務及 LM Studio 唔會被關閉。再次執行啟動器，即使 WebUI 已開住，都會檢查並補開模型或橋接器。

## 連線排查

| 情況 | 檢查 |
| --- | --- |
| ESP32 離線 | USB 供電、2.4 GHz Wi-Fi、serial log 嘅 IP，同 WebUI 裝置 IP 是否一致。 |
| 模型離線 | 執行 `./mac/Start-Qwen.command`，檢查指定模型及 MLX runtime 已下載。 |
| 橋接器啟動失敗 | `bridge.json` 嘅 `bind` 必須係 Mac 目前嘅 LAN IPv4；檢查 1235 埠是否被其他程式使用。 |
| 換咗 Wi-Fi／Mac IP | 更新私人設定，保留橋接 token，重新產生及寫入 NVS，重啟橋接器；板 IP 改咗就同步更新 WebUI。 |
| 同一 LAN 都連唔到 | 檢查訪客 Wi-Fi 裝置隔離、防火牆或 VPN 嘅 LAN 存取設定。 |
| FPS 低或畫面閃爍 | 實際 FPS 受曝光及 Wi-Fi 影響；按光源選 50／60 Hz 防閃爍，改善照明後再試。 |

可以喺路由器為 Mac 同 ESP32 保留 DHCP 位址，減少 IP 改變後要重新設定嘅情況。

## 開發檢查

Mac 橋接器／WebUI 使用 Python 標準函式庫；NVS 產生及燒錄使用 ESP-IDF 提供嘅 Python 環境。

```sh
python3 -m unittest discover -s mac -p 'test_*.py'
```

本機埠分工：WebUI `127.0.0.1:8787`、LM Studio `127.0.0.1:1234`、Mac LAN 橋接器 `:1235`；ESP32 控制 API 用 `:80`，MJPEG 用 `:81`。唔好將呢啲埠轉發到互聯網。
