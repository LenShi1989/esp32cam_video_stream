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

## esp32cam 連接st7789 做影像傳輸
