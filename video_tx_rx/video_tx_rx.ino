/*
  esp32cam_video_stream
  ESP32-CAM (AI-Thinker) 直接將相機影像串流顯示在外接的 ST7789 240x240 TFT LCD 上。
  不經過 WiFi，相機擷取 (tx) 與顯示 (rx) 都在同一顆 ESP32 上完成。

  接線請參考 README.md / CLAUDE.md：
    ST7789 SCK -> GPIO14
    ST7789 SDA(MOSI) -> GPIO13
    ST7789 RES -> GPIO12
    ST7789 DC  -> GPIO2
    ST7789 BLK -> NC (不接，依模組預設常亮)
    ST7789 沒有獨立 CS 腳位，不需接線

  需要的函式庫 (Library Manager 安裝)：
    - TFT_eSPI (Bodmer)

  本檔案透過在 #include <TFT_eSPI.h> 之前定義 USER_SETUP_LOADED 及各項腳位，
  直接在專案內覆寫函式庫的 User_Setup.h，不需另外修改函式庫本身的設定檔。

  開發板需選擇 "AI Thinker ESP32-CAM"，並開啟 PSRAM (Enabled)。
*/

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_camera.h"
#include <SPI.h>
#include <TFT_eSPI.h>

// ---------- TFT_eSPI 設定 (取代函式庫內的 User_Setup.h) ----------
// #define USER_SETUP_LOADED 1
// #define ST7789_DRIVER

#define TFT_WIDTH 240
#define TFT_HEIGHT 240

// 常見的 240x240 ST7789 模組面板色序為 BGR，若實際顯示顏色是反的 (紅藍互換)，
// 可改成 #define TFT_RGB_ORDER TFT_RGB 試試看。
// #define TFT_RGB_ORDER TFT_BGR

// #define USE_HSPI_PORT

// #define TFT_MOSI 13
// #define TFT_SCLK 14
// #define TFT_MISO -1 // 未使用
// #define TFT_RST 12
// #define TFT_DC 2
// #define TFT_CS -1 // 此模組無獨立 CS 腳位
// #define TFT_BL -1 // BLK 未接線 (NC)，依模組預設常亮

// #define SPI_FREQUENCY 10000000 // 先降速確認排線可正常通訊，穩定後可再調高
// #define SPI_READ_FREQUENCY 10000000

// ---------- AI-Thinker ESP32-CAM 相機腳位 ----------
#define CAM_PIN_PWDN 32
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 0
#define CAM_PIN_SIOD 26
#define CAM_PIN_SIOC 27
#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22

TFT_eSPI tft = TFT_eSPI();

static bool initCamera()
{
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000;

  // 直接輸出 RGB565，畫面大小與 TFT 一致 (240x240)，省去 JPEG 解碼
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_240X240;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK)
  {
    Serial.printf("相機初始化失敗，錯誤代碼 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s)
  {
    // OV2640 模組安裝方向常是上下顛倒，視實際畫面調整
    s->set_vflip(s, 1);
    s->set_hmirror(s, 0);
  }
  return true;
}

void setup()
{
  // 關閉 brownout 偵測，避免相機啟動瞬間電流過大導致重開機
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  Serial.println("ESP32-CAM -> ST7789 影像串流啟動中...");

  tft.init();
  tft.setRotation(1); // 0~3，順時針旋轉 90°

  // 自我測試：確認 SPI/接線本身沒問題，與相機資料無關
  // 若這裡看不到紅/綠/藍全螢幕依序顯示，代表問題在 TFT 接線或初始化，不是相機
  // Serial.println("TFT 自我測試: 紅");
  // tft.fillScreen(TFT_RED);
  // delay(800);
  // Serial.println("TFT 自我測試: 綠");
  // tft.fillScreen(TFT_GREEN);
  // delay(800);
  // Serial.println("TFT 自我測試: 藍");
  // tft.fillScreen(TFT_BLUE);
  // delay(800);
  // tft.fillScreen(TFT_BLACK);

  if (!initCamera())
  {
    tft.setCursor(10, 100);
    tft.setTextColor(TFT_RED);
    tft.println("Camera init failed!");
    while (true)
    {
      delay(1000);
    }
  }

  Serial.println("初始化完成，開始串流");
}

void loop()
{
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb)
  {
    Serial.println("擷取影像失敗");
    return;
  }

  static bool printedInfo = false;
  if (!printedInfo)
  {
    Serial.printf("實際擷取畫面: %dx%d, format=%d, len=%d\n", fb->width, fb->height, fb->format, fb->len);
    printedInfo = true;
  }

  if (fb->format == PIXFORMAT_RGB565)
  {
    // 相機實際協商出的解析度不一定剛好是 240x240，置中顯示避免因尺寸不符而整張跳過
    int16_t x = (TFT_WIDTH - fb->width) / 2;
    int16_t y = (TFT_HEIGHT - fb->height) / 2;
    tft.startWrite();
    tft.setAddrWindow(x, y, fb->width, fb->height);
    tft.pushPixels((uint16_t *)fb->buf, fb->len / 2);
    tft.endWrite();
  }

  esp_camera_fb_return(fb);
}
