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
  - 控制台：系統狀態（連線 / IP / 相機）+ GPIO4 燈光按鍵（GET `/led?state=0|1`）
    - GPIO4 為 AI-Thinker 板載強光 LED，未使用 microSD 卡時與相機、TFT 腳位皆不衝突
  - Wi-Fi 設定：
    - 掃描附近 SSID（GET `/scan`）
    - 輸入 SSID + PASSWORD 送出連線（POST `/connect`，同時存入 NVS，下次開機自動嘗試重連）
    - 清除 SSID 按鍵（POST `/clear`，清除 NVS 設定並斷開 STA，熱點保持開啟可重新設定）
  - 影像串流：瀏覽器輪詢 GET `/capture`（相機任務將 RGB565 幀以 frame2jpg 轉 JPEG 快照回傳）顯示即時影像
    - 旋轉按鈕：0°/90°/180°/270° 循環切換（瀏覽器端 canvas 旋轉，與移動偵測共用角度，存 localStorage）
  - 移動偵測：在影像上拖曳框選偵測區域，瀏覽器端做畫格差異比對；綠框 = 正常、紅框 = 已觸發，附靈敏度調整
    - 同樣有旋轉按鈕；偵測取樣直接來自已旋轉的畫布，框選座標與顯示畫面一致，切換角度時會重置比對基準
  - OTA 更新：上傳 .bin 無線更新（POST `/update?target=firmware|spiffs`，使用 Update.h）
    - target=firmware：韌體 .bin（Arduino IDE「Sketch > Export Compiled Binary」）
    - target=spiffs：SPIFFS 映像檔（mkspiffs 打包 `data` 資料夾），可免拉線更新網頁
    - 更新期間相機任務暫停（flash 寫入會關閉 cache），完成後自動重新啟動

核心分工：

- 原始核心 (core 1，`setup()`/`loop()`)：Wi-Fi AP/STA + WebServer + LED 控制
- 備用核心 (core 0，`xTaskCreatePinnedToCore`)：相機擷取影像並串流顯示到 ST7789

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
