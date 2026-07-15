# esp32cam 影像傳輸

原始核心:1

- Web Server

備用核心:0

- camera影像傳輸st7789

## 使用 st7789 240 x 240 TFT LCD顯示器

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

## esp32cam 連接 st7789 pin腳接線圖

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

## TFT_eSPI 設定

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

## esp32cam 連接st7789 做影像傳輸

使用 SPIFFS Web Server（實作於 `video_tx_rx/video_tx_rx.ino`，
網頁檔拆分為 `video_tx_rx/data/` 下的 `index.html`（結構）、`style.css`（樣式）、`app.js`（邏輯））：

- ESP32 開 AP
  - SSID：`esp32_init`（開放網路，無密碼）
- 網頁為科技風深色介面，側邊欄分四個頁面：
  - 控制台：系統狀態（連線 / IP / 相機 / 資料儲存模式）+ GPIO4 燈光滑動開關 switch（GET `/led?state=0|1`）
    - GPIO4 為 AI-Thinker 板載強光 LED；TFT 模式不衝突，SD 模式 1-bit 也不占用 GPIO4
  - Wi-Fi 設定：
    - 掃描附近 SSID（GET `/scan`）
    - 輸入 SSID + PASSWORD 送出連線（POST `/connect`，同時存入 NVS，下次開機自動嘗試重連）
    - 清除 SSID 按鍵（POST `/clear`，清除 NVS 設定並斷開 STA，熱點保持開啟可重新設定）
  - 影像串流：瀏覽器輪詢 GET `/capture`（相機任務將 RGB565 幀以 frame2jpg 轉 JPEG 快照回傳）顯示即時影像
    - 旋轉按鈕：0°/90°/180°/270° 循環切換（瀏覽器端 canvas 旋轉，與移動偵測共用角度，存 localStorage）
  - 移動偵測：在影像上拖曳框選偵測區域，**偵測在 ESP32 裝置端 (core 0) 逐幀進行**，關閉網頁仍持續監控
    - 網頁 POST `/motion/config`（enabled/x/y/w/h/sens）設定區域與靈敏度；區域以感光元件方向的 240x240 座標保存，
      網頁端負責把旋轉後的框選反向換算回感光元件座標
    - 韌體把區域縮取樣成 32x32 灰階與上一幀比對，變動比例 > (16 - 靈敏度)% 即觸發
    - 網頁每秒輪詢 GET `/motion/status`（enabled/region/diff/triggered/lastId/count）更新綠框/紅框、DIFF %、
      並比對 lastId 偵測新告警 → 右下角 toast 通知 + 蜂鳴 + 側欄「告警紀錄」紅色計數徽章
    - 同樣有旋轉按鈕；框選/綠紅框顯示會依旋轉角度自動換算，與畫面一致
    - Discord 推播通知：觸發告警時由獨立任務 (core 1) 將「時間 + 擷取影像」以 multipart POST 到 Discord Webhook，
      手機安裝 Discord App 即收到推播；需 STA 連上可上網的 Wi-Fi
      - Webhook URL 存 NVS（GET/POST `/notify/config`，空字串 = 停用）、POST `/notify/test` 送測試訊息
      - 訊息時間使用瀏覽器 POST `/time` 同步的 epoch + 時區 (tz 分鐘)
    - ST7789 同步顯示：TFT 串流畫面每幀疊加偵測框（綠 = 監控、紅 = 觸發，與網頁一致），
      觸發時右上角顯示黃色小鈴鐺（用 TFT 基本圖形拼出，觸發保持 1.5 秒）
  - 觸發錄影（SD 模式限定）：告警成立時自動錄影 2 分鐘後停止
    - 格式為 MJPEG-AVI（ESP32 無 H.264 硬體編碼器，做不出真正的 .mp4），10fps，約 15~18MB/段
    - 檔名 `日期_時間_.avi`（如 `20260716_143005_.avi`，時間未同步時為 `notime_開機秒數_.avi`），存於 SD 卡 `/rec`
    - 由 cameraTask (core 0) 逐幀 frame2jpg 寫入，結束時回填 AVI 檔頭與 idx1 索引；
      錄影中再次觸發忽略；SD 剩餘空間 < 32MB 時略過；OTA 開始時先收尾關檔
  - 錄影回放（側邊欄頁面）：列出 `/rec` 錄影檔（GET `/recordings`），網頁播放器以串流方式下載
    （GET `/rec/file?name=`，分塊讀取避免卡住錄影寫入）並掃描 JPEG 幀（FFD8~FFD9）繪至 canvas，
    10fps 播放、支援目前旋轉角度；可刪除檔案（POST `/rec/delete?name=`，錄影中禁止刪除）；
    檔案亦可取出 SD 卡用電腦播放（VLC 等）
  - 告警紀錄：移動偵測觸發時韌體自動以 frame2jpg 擷取當下畫面
    - 每筆紀錄含觸發時間 + 觸發擷取圖片；儲存位置依儲存模式而定（見下方「資料儲存模式」）
    - 兩筆告警最小間隔 5 秒，避免連續移動灌爆紀錄
    - GET `/alarms` 回傳清單（新到舊）、GET `/alarm/img?id=N` 回傳 JPEG、POST `/alarm/clear` 清除全部
    - 觸發時間由網頁載入時 POST `/time`（瀏覽器 epoch）同步到裝置（AP 模式無 NTP）；未同步前的紀錄顯示「時間未同步」

## 資料儲存模式（開機自動偵測 SD 卡）

AI-Thinker 板載 microSD 與 ST7789 共用腳位（SD_MMC 1-bit 需 GPIO14/15/2，TFT 用 14/13/12/2），
兩者無法並存。開機時自動偵測（`/status` 的 `sd` 欄位、控制台「資料儲存」列可查看目前模式）：

- **SD 模式**：`SD_MMC.begin("/sdcard", true)`（1-bit）掛載成功
  - TFT 完全停用（**接 SD 卡時 TFT 排線需拔除**，避免干擾訊號）；GPIO4 LED 不受影響（1-bit 不用 D1）
  - 後端資料庫 `/appsetting.json`（cJSON 排版輸出，可用電腦直接開卡編輯）：
    - `wifi.ssid / wifi.password`：Wi-Fi 設定紀錄（取代 NVS）
    - `motion.enabled / x / y / w / h / sens`：偵測框座標紀錄（開機自動還原並開始偵測）
    - `notify.webhook`：Discord Webhook
  - 告警紀錄存 `/alarms/<id>.jpg` + `/alarms/index.json`，保留最近 50 筆（環形覆蓋最舊），斷電保留
  - 所有 SD 檔案存取以 `sdMutex` 序列化，鎖順序固定 alarmMutex → sdMutex
- **TFT 模式**：無 SD 卡（維持原本行為）
  - Wi-Fi / Webhook 存 NVS（Preferences），偵測框僅存 RAM（重開機需重設），告警存 PSRAM 保留 10 筆（斷電清空）
  - OTA 更新：上傳 .bin 無線更新（POST `/update?target=firmware|spiffs`，使用 Update.h）
    - target=firmware：韌體 .bin（Arduino IDE「Sketch > Export Compiled Binary」）
    - target=spiffs：SPIFFS 映像檔（mkspiffs 打包 `data` 資料夾），可免拉線更新網頁
    - 更新期間相機任務暫停（flash 寫入會關閉 cache），完成後自動重新啟動

核心分工：

- 原始核心 (core 1，`setup()`/`loop()`)：Wi-Fi AP/STA + WebServer + LED 控制 + Discord 推播任務（`notifyTask`）
- 備用核心 (core 0，`xTaskCreatePinnedToCore`)：相機擷取影像並串流顯示到 ST7789（含偵測框/鈴鐺疊加）+ 移動偵測比對與告警擷取

燒錄注意事項：

- Partition Scheme 需選含 SPIFFS 的方案（`.vscode/arduino.json` 已設為 `min_spiffs`）
- `video_tx_rx/data/` 內的 `index.html`、`style.css`、`app.js` 需一併上傳到 SPIFFS，才能讓 `/` 網頁正常顯示
  （例如使用 Arduino IDE 的 ESP32 Sketch Data Upload 外掛，或 arduino-cli + mkspiffs/esptool 燒錄 `data` 資料夾；
  之後也可從網頁「OTA 更新」頁面上傳 SPIFFS 映像檔免拉線更新，步驟見下節）

## 打包 data/ 的 SPIFFS 映像檔並從網頁「OTA 更新」上傳

修改 `data/` 內的網頁檔（`index.html`、`style.css`、`app.js`）後，不必拉線重燒，
用 mkspiffs 打包成映像檔再從網頁 OTA 上傳即可：

1. 打包 SPIFFS 映像檔。mkspiffs 隨 arduino-esp32 核心一起安裝，
   在 `video_tx_rx/` 資料夾下執行（PowerShell，版本號依實際安裝調整）：

   ```powershell
   & "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\mkspiffs\0.2.3\mkspiffs.exe" -c data -b 4096 -p 256 -s 0x20000 spiffs.bin
   ```

   - `-s 0x20000`（128KB）必須等於 `min_spiffs` 分割表中 spiffs 分割區的大小；
     若改用其他 Partition Scheme，需查對應分割表（`Arduino15\packages\esp32\hardware\esp32\<版本>\tools\partitions\*.csv`）調整
   - `-b 4096 -p 256` 為 ESP32 SPIFFS 預設 block/page 大小，不可更改
   - 輸出的 `spiffs.bin` 請放在 `data/` 之外（如上指令放在 `video_tx_rx/`），
     否則下次打包會把映像檔本身也包進去
2. 瀏覽器連上 ESP32：連熱點 `esp32_init` 開 `http://192.168.4.1`，或用 STA 模式取得的 IP
3. 側邊欄「OTA 更新」→ 更新目標選「SPIFFS 網頁資料」→ 選擇 `spiffs.bin` → 開始更新
4. 上傳完成後裝置自動重新啟動，網頁約 10 秒後自動重新整理，即載入新版網頁

> 注意：`spiffs.bin` 是整包映像檔，上傳會覆蓋 SPIFFS 分割區全部內容，
> 打包時 `data/` 內必須包含完整的三個網頁檔，缺檔會導致對應頁面載入失敗。
