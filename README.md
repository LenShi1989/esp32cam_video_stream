# esp32cam 影像傳輸

ESP32-CAM（AI-Thinker）影像監控專案：ST7789 TFT 即時串流、科技風 Web 控制中心、
裝置端移動偵測 + 告警擷取、Discord 推播、觸發錄影與網頁回放、SD 卡資料儲存、OTA 無線更新。

## 功能總覽

網頁為深色科技風介面（`video_tx_rx/data/`：`index.html` / `style.css` / `app.js`，存於 SPIFFS），
側邊欄共七個頁面：

| 頁面      | 功能                                                                                             |
| --------- | ------------------------------------------------------------------------------------------------ |
| 控制台    | 系統狀態（STA 連線 / SSID / IP / 相機 / 資料儲存模式）+ GPIO4 強光 LED 滑動開關                    |
| Wi-Fi 設定 | 掃描附近 SSID、輸入密碼連線（設定會保存，開機自動重連）、清除設定重新配對                          |
| 影像串流  | 輪詢 `/capture` 顯示即時影像（240×240）、FPS 顯示、暫停/繼續、旋轉 0°/90°/180°/270°               |
| 移動偵測  | 拖曳框選偵測區域 + 靈敏度調整（偵測在 ESP32 端執行）、Discord Webhook 推播設定                     |
| 告警紀錄  | 觸發時間 + 觸發當下擷取影像清單、側欄紅色計數徽章、清除紀錄                                        |
| 錄影回放  | 列出 SD 卡 `/rec` 錄影檔，網頁串流播放（MJPEG 解析）、刪除檔案（SD 模式限定）                      |
| OTA 更新  | 上傳 .bin 無線更新韌體或 SPIFFS 網頁資料，完成後自動重啟                                           |

### 移動偵測（裝置端）

- 偵測跑在 ESP32 core 0，**關閉網頁仍持續監控**；區域縮取樣成 32×32 灰階逐幀比對，
  變動比例 > (16 − 靈敏度)% 即觸發
- 網頁框選的區域會依旋轉角度換算回感光元件座標，框到哪就偵測哪；綠框 = 監控中、紅框 = 觸發
- 觸發後動作：
  - 擷取當下畫面存入**告警紀錄**（兩筆告警最小間隔 5 秒）
  - 網頁右下角 toast 通知 + 蜂鳴 + 側欄徽章
  - **Discord 推播**：把「時間 + 擷取影像」傳到 Webhook，手機裝 Discord App 即收到通知
    （需 STA 連上可上網的 Wi-Fi；Webhook 在移動偵測頁設定，可測試傳送）
  - **觸發錄影**（SD 模式）：自動錄影 2 分鐘存 SD 卡
- ST7789 同步顯示：TFT 畫面即時疊加偵測框（綠/紅），觸發時右上角顯示黃色小鈴鐺

### 觸發錄影與回放（SD 模式）

- 觸發告警後自動錄影 **2 分鐘**（10fps，約 15~18MB/段），存於 SD 卡 `/rec`
- 檔名 `日期_時間_.avi`（例 `20260716_143005_.avi`；時間未同步時為 `notime_開機秒數_.avi`）
- 格式為 **MJPEG-AVI**：ESP32 沒有 H.264 硬體編碼器，做不出真正的 .mp4；
  AVI 檔可取出 SD 卡直接用電腦播放（VLC、PotPlayer 等），含 idx1 索引可拖曳進度
- 網頁「錄影回放」頁以串流方式邊下載邊解析 JPEG 幀播放，播放舊檔不會卡住進行中的錄影；
  錄影中再次觸發忽略、SD 剩餘空間 < 32MB 時略過錄影

### 資料儲存模式（開機自動偵測）

板載 microSD 與 ST7789 共用腳位（SD_MMC 1-bit 需 GPIO14/15/2，TFT 用 14/13/12/2），兩者無法並存。
開機自動偵測，控制台「資料儲存」列可查看目前模式：

| 項目        | SD 模式（有插卡）                              | TFT 模式（無卡，預設）             |
| ----------- | ---------------------------------------------- | ---------------------------------- |
| ST7789 顯示 | 停用（**TFT 排線需拔除**）                     | 啟用                               |
| Wi-Fi 設定  | SD 卡 `/appsetting.json`                       | NVS                                |
| 偵測框座標  | SD 卡 `/appsetting.json`（重開機自動還原偵測） | 僅 RAM（重開機需重設）             |
| Discord Webhook | SD 卡 `/appsetting.json`                   | NVS                                |
| 告警紀錄    | SD 卡 `/alarms/`，保留 50 筆，斷電保留         | PSRAM，保留 10 筆，斷電清空        |
| 觸發錄影    | SD 卡 `/rec/*.avi`                             | 不支援                             |

`appsetting.json` 為 cJSON 排版輸出，可把卡拔下來用電腦直接檢視/編輯：

```json
{
  "wifi":   { "ssid": "...", "password": "..." },
  "motion": { "enabled": true, "x": 40, "y": 60, "w": 120, "h": 100, "sens": 4 },
  "notify": { "webhook": "https://discord.com/api/webhooks/..." }
}
```

> SD 模式 1-bit 只占用 GPIO14(CLK)/15(CMD)/2(D0)，GPIO4 LED 不受影響。
> SD 卡建議 FAT32 格式；告警觸發時間由網頁載入時同步瀏覽器時間（AP 模式無 NTP）。

### 核心分工

- 原始核心 core 1（`setup()`/`loop()`）：Wi-Fi AP+STA、WebServer、LED 控制、Discord 推播任務
- 備用核心 core 0（`xTaskCreatePinnedToCore`）：相機擷取 → ST7789 串流（含偵測框/鈴鐺疊加）、
  移動偵測比對、告警擷取、觸發錄影寫入

### HTTP API 一覽

| 方法 | 路徑             | 說明                                                       |
| ---- | ---------------- | ---------------------------------------------------------- |
| GET  | `/status`        | 系統狀態（連線/IP/LED/相機/儲存模式）                       |
| GET  | `/scan`          | 掃描附近 Wi-Fi                                             |
| POST | `/connect`       | 連線 Wi-Fi（ssid/password，並保存）                         |
| POST | `/clear`         | 清除已存 Wi-Fi 設定並斷線                                   |
| GET  | `/led?state=0\|1` | GPIO4 LED 開關                                            |
| GET  | `/capture`       | 取得一張 JPEG 快照（影像串流輪詢用）                        |
| POST | `/motion/config` | 設定偵測區域/靈敏度（enabled/x/y/w/h/sens，感光元件座標）   |
| GET  | `/motion/status` | 偵測狀態（diff/triggered/lastId/count/recording）           |
| GET  | `/alarms`        | 告警清單（新到舊）                                          |
| GET  | `/alarm/img?id=` | 告警擷取 JPEG                                              |
| POST | `/alarm/clear`   | 清除全部告警                                                |
| GET  | `/notify/config` | 讀取 Discord Webhook                                        |
| POST | `/notify/config` | 儲存 Discord Webhook（空字串 = 停用）                       |
| POST | `/notify/test`   | 送出 Discord 測試訊息                                       |
| GET  | `/recordings`    | 錄影檔清單                                                  |
| GET  | `/rec/file?name=`| 串流下載錄影檔                                              |
| POST | `/rec/delete?name=` | 刪除錄影檔（錄影中禁止）                                 |
| POST | `/time`          | 同步瀏覽器時間與時區（epoch/tz）                            |
| POST | `/update?target=firmware\|spiffs` | OTA 上傳 .bin 更新                        |

## 硬體

### 使用 st7789 240 x 240 TFT LCD顯示器

工作電壓：3.3V ~ 5V
連結方法：2.54mm DIP
通訊方式：SPI通訊
腳位說明：

- ◎ VCC：供電端
- ◎ GND：接地端
- ◎ SCK：SCK
- ◎ SDA：MOSI
- ◎ RES：重新啟動
- ◎ DC：資料或指令選擇腳位
- ◎ BLK：拉低電位關背光

### esp32cam 連接 st7789 pin腳接線圖

以下接線表使用 ESP32-CAM（AI-Thinker）**未使用 microSD 卡**時釋放出來的 GPIO12~15，
避免與相機（HS2_DATA1/GPIO4、GPIO0 XCLK）及燒錄用序列埠（GPIO1/GPIO3）衝突：

| ST7789 腳位 | 說明          | ESP32-CAM 腳位 |
| ----------- | ------------- | -------------- |
| VCC         | 供電 3.3V     | 3.3V           |
| GND         | 接地          | GND            |
| SCK         | SPI 時脈      | GPIO_14        |
| SDA         | SPI MOSI      | GPIO_13        |
| RES         | 重新啟動      | GPIO_12        |
| DC          | 資料/指令選擇 | GPIO_2         |
| BLK         | 背光控制      | NC             |

> 注意：ESP32-CAM 只有一組 SPI 排線可用的腳位較少，此模組沒有獨立 CS 腳位，
> 因此不需另外接 CS。若接線後畫面異常，可嘗試將 BLK 直接接 3.3V 確認是否為背光腳位問題。
> 要使用 SD 卡儲存（SD 模式）時請拔除 TFT 排線，兩者腳位衝突無法並存。

### TFT_eSPI 設定

文件\Arduino\libraries\TFT_eSPI
取消註解
User_Setup_Select.h

- #include <User_Setups/Setup24_ST7789.h>

Setup24_ST7789.h 設定

- #define TFT_MOSI 13
- #define TFT_SCLK 14
- #define TFT_CS -1 // Not connected
- #define TFT_DC 2
- #define TFT_RST 12

## 燒錄與部署

1. Arduino IDE 選擇開發板 **AI Thinker ESP32-CAM**，開啟 PSRAM (Enabled)
2. Partition Scheme 選含 SPIFFS 的方案（`min_spiffs`，本專案 OTA 也依此配置）
3. 燒錄 `video_tx_rx/video_tx_rx.ino`（需安裝 TFT_eSPI 函式庫；cJSON 為 arduino-esp32 內建）
4. 將 `video_tx_rx/data/` 三個網頁檔上傳到 SPIFFS（ESP32 Sketch Data Upload 外掛，
   或 mkspiffs 打包後從網頁 OTA 上傳，見下節）
5. 連上熱點 `esp32_init`（開放網路）→ 瀏覽器開 `http://192.168.4.1`

### 打包 SPIFFS 映像檔並從網頁 OTA 更新

修改網頁檔後不必拉線重燒：

```powershell
# 於 video_tx_rx/ 資料夾下執行（mkspiffs 隨 arduino-esp32 核心安裝，版本號依實際調整）
& "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\mkspiffs\0.2.3\mkspiffs.exe" -c data -b 4096 -p 256 -s 0x20000 spiffs.bin
```

- `-s 0x20000`（128KB）必須等於 `min_spiffs` 分割表中 spiffs 分割區大小
- 網頁「OTA 更新」→ 目標選「SPIFFS 網頁資料」→ 上傳 `spiffs.bin` → 裝置自動重啟
- 韌體更新同理：Arduino IDE「Sketch > Export Compiled Binary」產生 .bin，目標選「韌體」

### 注意事項

- 供電建議外部 5V/2A；韌體已降低 WiFi 發射功率避免尖峰電流造成不穩
- OTA 更新期間相機任務與錄影自動暫停/收尾，完成後自動重啟
- Discord 推播使用 `setInsecure()` 不驗證憑證（Webhook URL 本身含密鑰）
- 錄影期間串流幀率會下降（JPEG 編碼與 SD 寫入都在 core 0）
