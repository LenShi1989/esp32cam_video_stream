# esp32cam 影像傳輸

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

使用 SPIFFS Web Server（`video_tx_rx/video_tx_rx.ino` + `video_tx_rx/data/index.html`）：

- ESP32 開 AP，SSID `esp32_init`（開放網路），提供網頁掃描附近 Wi-Fi、輸入密碼連線
- 連線成功後網頁切換為開關，控制 GPIO4 LED 亮滅
- 核心分工：core 1 (Arduino `loop()`) 跑 Web Server，core 0 (獨立 task) 跑相機 -> ST7789 串流
- 燒錄前需將 `video_tx_rx/data/index.html` 上傳到 ESP32 的 SPIFFS，並使用含 SPIFFS 的 Partition Scheme（如 `min_spiffs`）
