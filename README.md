# Jayden Flowerpot

用 **Seeed Studio XIAO ESP32S3 Sense（OV3660）** 同 Mac 上嘅本地 Qwen，整一個可以睇畫面、手動拍照分析、文字對話嘅小型工作台。ESP32 負責相機同裝置端 agent；模型喺 Mac 嘅 LM Studio 運行。

## 目前功能

- **VGA 即時畫面**：640 × 480 MJPEG，手動開始／停止；顯示實際收到嘅 FPS。
- **相機調校**：目標 10／15／20／25 FPS、50／60 Hz／自動防閃爍、自動／日光／辦公室／室內白平衡，以及亮度、飽和度。實際 FPS 受光線、曝光同 Wi-Fi 影響。
- **手動相片分析**：「拍一張」只拍照；「拍照並分析」先拍一張，再交畀本地 Qwen 回答。
- **文字對話**：經 ESPClaw 裝置端 agent 接駁本地模型。
- **Mac WebUI**：繁體中文介面、裝置與模型狀態、相機設定及文字紀錄。

直播唔會自動交畀 Qwen 分析，開頁亦唔會自動開相機。每次直播最長 10 分鐘；停止直播後先可以拍照、分析、對話或調校。

## 架構

```text
Mac 瀏覽器 → 本機 WebUI（127.0.0.1:8787）→ XIAO 相機／agent
                         ↓                         ↓
                   Mac LAN 橋接器（1235）←─────────┘
                         ↓
                 LM Studio（127.0.0.1:1234）
                         ↓
                    本地 Qwen 模型
```

WebUI 同 LM Studio 只聽本機位址；ESP32 經有 token 驗證嘅 LAN 橋接器使用模型。呢個設定供可信任嘅本地網絡使用，唔需要路由器 port forwarding。

## 首次設定

需要 XIAO ESP32S3 Sense（8 MB Flash、8 MB PSRAM，呢個版本針對 OV3660）、USB 數據線、2.4 GHz Wi-Fi、Mac、Python 3，以及 **ESP-IDF v5.5.5**。本地模型需要另外安裝 LM Studio、下載模型及相容嘅 MLX runtime，詳見 [Mac 設定](mac/README.md)。

以下指令喺專案根目錄執行；先啟用你安裝好嘅 ESP-IDF 環境：

```sh
git clone https://github.com/xiaoyh-code/Jayden-Flowerpot.git
cd Jayden-Flowerpot
. /path/to/esp-idf-v5.5.5/export.sh

idf.py -B build.xiao_s3_sense \
  -D SDKCONFIG=sdkconfig.xiao_s3_sense \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults.xiao_esp32s3_sense;sdkconfig.defaults.local_qwen' \
  set-target esp32s3
idf.py -B build.xiao_s3_sense build
```

將下面 USB 埠換成自己嗰塊板嘅埠。燒錄會取代板上現有程式。

```sh
ls /dev/cu.usbmodem*
ESPCLAW_PORT='/dev/cu.usbmodemXXXX'
idf.py -B build.xiao_s3_sense -p "$ESPCLAW_PORT" flash
```

接住跟 [Mac 設定](mac/README.md) 建立 Wi-Fi、橋接器及 NVS 設定，再寫入板上。**必須完成 NVS 設定**，單獨燒錄程式未足夠連線。上述兩份 defaults 要一齊使用，先會開啟相機、USB serial、HTTP 同本地 Qwen 設定。

完成首次設定後，每次插好板，只需執行：

```sh
./mac/Start-ESPClaw-WebUI.command
```

啟動器會檢查模型、橋接器及 WebUI，然後打開 `http://127.0.0.1:8787`。保留啟動器嘅 Terminal 視窗；按 Control-C 會停止今次由佢啟動嘅 WebUI／橋接器，LM Studio 會繼續運行。

## 私人資料

Wi-Fi 密碼、橋接 token、NVS 映像及 Mac 設定放喺 `mac/private/`，唔屬於公開原始碼。相片同對話紀錄只暫存喺 WebUI 程序記憶體，重啟後清除。呢個公開版本亦唔包含測試相片或特定電腦嘅驗證報告。

## 來源與授權

基於 [Seeed-Projects/espclaw，commit `49b5bd8`](https://github.com/Seeed-Projects/espclaw/tree/49b5bd8) 改作，保留原有 [MIT LICENSE](LICENSE)。上游說明保留喺 [README.upstream.md](README.upstream.md)；其中介紹嘅其他板型、聊天平台等功能，唔代表本專案嘅 `local_qwen` 設定已經啟用。
