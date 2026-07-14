/*
  esp32cam_video_stream

  雙核心分工 (依 CLAUDE.md)：
    - 原始核心 (core 1，即 Arduino setup()/loop())：
      Web Server：開 AP (SSID esp32_init) -> 掃描/設定 Wi-Fi -> 連線成功後
      提供網頁開關控制 GPIO4 LED 亮滅。
    - 備用核心 (core 0，透過 xTaskCreatePinnedToCore 建立)：
      相機擷取影像並直接串流顯示到外接的 ST7789 240x240 TFT LCD，不經過 WiFi。

  接線請參考 README.md / CLAUDE.md：
    ST7789 SCK -> GPIO14
    ST7789 SDA(MOSI) -> GPIO13
    ST7789 RES -> GPIO12
    ST7789 DC  -> GPIO2
    ST7789 BLK -> NC (不接，依模組預設常亮)
    ST7789 沒有獨立 CS 腳位，不需接線

  需要的函式庫 (Library Manager 安裝)：
    - TFT_eSPI (Bodmer)

  開發板需選擇 "AI Thinker ESP32-CAM"，並開啟 PSRAM (Enabled)。
  Partition Scheme 需選擇含 SPIFFS 的方案 (例如 min_spiffs)，才有空間放網頁檔案。

  網頁檔案 (video_tx_rx/data/index.html) 需另外上傳到 ESP32 的 SPIFFS，
  例如使用 arduino-esp32fs-plugin 的 "ESP32 Sketch Data Upload"，
  或透過 arduino-cli + mkspiffs/esptool 燒錄 data 資料夾內容。
*/

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_camera.h"
#include <SPI.h>
// FS.h 必須在 TFT_eSPI.h 之前先引入：TFT_eSPI 的 ESP32 處理器標頭會以
// #define FS_NO_GLOBALS 引入 FS.h，導致 fs::FS/fs::File 不會別名到全域的
// FS/File，造成 WebServer.h 及下面的 SPIFFS 存取編譯失敗。在此先正常引入一次，
// 之後 TFT_eSPI 內的重複引入會因 include guard 而略過，全域別名得以保留。
#include <FS.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <Update.h> // OTA：網頁上傳 .bin 更新韌體 / SPIFFS
#include "img_converters.h" // frame2jpg：RGB565 -> JPEG，供網頁快照串流使用

// ---------- TFT 顯示尺寸 ----------
#define TFT_WIDTH 240
#define TFT_HEIGHT 240

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

// ---------- Web Server / Wi-Fi / LED 設定 ----------
#define LED_PIN 4  // AI-Thinker 板載強光 LED，與相機腳位不衝突
#define AP_SSID "esp32_init"
#define WIFI_PREF_NAMESPACE "wifi"
static const unsigned long STA_CONNECT_TIMEOUT_MS = 15000;

TFT_eSPI tft = TFT_eSPI();
WebServer server(80);
Preferences wifiPrefs;

volatile bool ledState = false;
bool staConnecting = false;
unsigned long staConnectStart = 0;

// ---------- 網頁影像快照 (core0 編碼 JPEG -> core1 回傳) ----------
// WebServer 是單執行緒且相機由 core 0 獨占，因此用「請求/完成」握手：
// /capture 先設 jpegRequested，相機任務在推完 TFT 後將該幀轉成 JPEG
// 放進共用緩衝區並發出 jpegReadySem，core 1 再把緩衝區送給瀏覽器。
volatile bool cameraOk = false;
volatile bool jpegRequested = false;
uint8_t *jpegBuf = NULL;
size_t jpegLen = 0;
SemaphoreHandle_t jpegMutex;
SemaphoreHandle_t jpegReadySem;

// ---------- OTA 更新 ----------
// OTA 寫入 flash 時會暫時關閉 flash cache，為避免 core 0 的相機/TFT
// 任務在此期間存取 flash 造成當機或畫面異常，更新中先讓相機任務暫停。
volatile bool otaInProgress = false;
bool otaSuccess = false;

static bool initCamera();
void cameraTask(void *param);
String jsonEscape(const String &s);
void handleRoot();
void handleScan();
void handleConnect();
void handleStatus();
void handleLed();
void handleCapture();
void handleClearWifi();
void handleUpdateFinished();
void handleUpdateUpload();
void tryAutoConnectSavedWifi();

// ================= setup / loop：core 1 = Web Server =================

void setup()
{
  // 關閉 brownout 偵測，避免相機啟動瞬間電流過大導致重開機
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  Serial.println("ESP32-CAM 啟動中 (core1=Web Server, core0=camera->TFT)...");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  if (!SPIFFS.begin(true))
  {
    Serial.println("SPIFFS 掛載失敗，網頁將無法載入");
  }

  // AP + STA 同時開啟：手機可持續連著 esp32_init 這個熱點進行設定，
  // 同時 ESP32 也能以 STA 模式連上使用者選擇的 Wi-Fi。
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);
  // 降低 WiFi 發射功率以減少尖峰電流，避免與相機/TFT 同時運作時因供電不足
  // (電壓瞬間下降) 導致 SPI Flash 讀取錯誤而觸發 IllegalInstruction 當機。
  // 供電穩定 (建議由外部 5V/2A 供應並加大電容旁路) 後可調高或移除此行。
  WiFi.setTxPower(WIFI_POWER_11dBm);
  Serial.print("AP 已啟動，SSID: ");
  Serial.println(AP_SSID);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  tryAutoConnectSavedWifi();

  jpegMutex = xSemaphoreCreateMutex();
  jpegReadySem = xSemaphoreCreateBinary();

  server.on("/", HTTP_GET, handleRoot);
  server.serveStatic("/style.css", SPIFFS, "/style.css");
  server.serveStatic("/app.js", SPIFFS, "/app.js");
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/connect", HTTP_POST, handleConnect);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/led", HTTP_GET, handleLed);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/clear", HTTP_POST, handleClearWifi);
  server.on("/update", HTTP_POST, handleUpdateFinished, handleUpdateUpload);
  server.begin();
  Serial.println("Web Server 已啟動 (core 1)");

  // 相機擷取 + TFT 顯示放到備用核心 (core 0)，避免與 Web Server 互相卡住
  xTaskCreatePinnedToCore(cameraTask, "cameraTask", 10240, NULL, 1, NULL, 0);
}

void loop()
{
  server.handleClient();

  if (staConnecting)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      staConnecting = false;
      Serial.print("Wi-Fi 連線成功, IP: ");
      Serial.println(WiFi.localIP());
    }
    else if (millis() - staConnectStart > STA_CONNECT_TIMEOUT_MS)
    {
      staConnecting = false;
      Serial.println("Wi-Fi 連線逾時");
    }
  }
}

// ================= core 0：相機影像 -> ST7789 =================

void cameraTask(void *param)
{
  tft.init();
  tft.setRotation(1); // 0~3，順時針旋轉 90°

  if (!initCamera())
  {
    tft.setCursor(10, 100);
    tft.setTextColor(TFT_RED);
    tft.println("Camera init failed!");
    vTaskDelete(NULL);
    return;
  }

  Serial.println("相機初始化完成，開始串流 (core 0)");
  cameraOk = true;

  bool printedInfo = false;
  for (;;)
  {
    if (otaInProgress)
    {
      // OTA 寫入 flash 期間暫停擷取/顯示，降低 cache 關閉造成的風險
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb)
    {
      Serial.println("擷取影像失敗");
      continue;
    }

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

    // 網頁 /capture 有人在等快照時，才把這一幀轉成 JPEG (約數十 ms)，
    // 平常不編碼以免拖慢 TFT 串流
    if (jpegRequested)
    {
      jpegRequested = false;
      uint8_t *out = NULL;
      size_t outLen = 0;
      if (frame2jpg(fb, 80, &out, &outLen))
      {
        xSemaphoreTake(jpegMutex, portMAX_DELAY);
        if (jpegBuf)
        {
          free(jpegBuf);
        }
        jpegBuf = out;
        jpegLen = outLen;
        xSemaphoreGive(jpegMutex);
        xSemaphoreGive(jpegReadySem);
      }
    }

    esp_camera_fb_return(fb);
  }
}

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

// ================= Web Server 處理函式 =================

String jsonEscape(const String &s)
{
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++)
  {
    char c = s[i];
    if (c == '"' || c == '\\')
    {
      out += '\\';
    }
    out += c;
  }
  return out;
}

void handleRoot()
{
  File f = SPIFFS.open("/index.html", "r");
  if (!f)
  {
    server.send(500, "text/plain", "index.html not found，請先上傳 SPIFFS data 資料夾");
    return;
  }
  server.streamFile(f, "text/html");
  f.close();
}

void handleScan()
{
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++)
  {
    if (i > 0)
    {
      json += ",";
    }
    json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  json += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}

void handleConnect()
{
  if (!server.hasArg("ssid") || server.arg("ssid").length() == 0)
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid\"}");
    return;
  }

  String ssid = server.arg("ssid");
  String password = server.hasArg("password") ? server.arg("password") : "";

  wifiPrefs.begin(WIFI_PREF_NAMESPACE, false);
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.putString("password", password);
  wifiPrefs.end();

  WiFi.begin(ssid.c_str(), password.c_str());
  staConnecting = true;
  staConnectStart = millis();

  server.send(200, "application/json", "{\"ok\":true}");
}

void handleStatus()
{
  bool connected = WiFi.status() == WL_CONNECTED;
  String json = "{";
  json += "\"connected\":" + String(connected ? "true" : "false");
  json += ",\"connecting\":" + String(staConnecting ? "true" : "false");
  json += ",\"ip\":\"" + (connected ? WiFi.localIP().toString() : String("")) + "\"";
  json += ",\"ssid\":\"" + jsonEscape(connected ? WiFi.SSID() : String("")) + "\"";
  json += ",\"led\":" + String(ledState ? "true" : "false");
  json += ",\"camera\":" + String(cameraOk ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleCapture()
{
  if (!cameraOk)
  {
    server.send(503, "application/json", "{\"error\":\"camera not ready\"}");
    return;
  }

  // 清掉前一次逾時可能殘留的 ready 訊號，再向相機任務要一張新的快照
  while (xSemaphoreTake(jpegReadySem, 0) == pdTRUE)
  {
  }
  jpegRequested = true;

  if (xSemaphoreTake(jpegReadySem, pdMS_TO_TICKS(1500)) != pdTRUE)
  {
    server.send(503, "application/json", "{\"error\":\"capture timeout\"}");
    return;
  }

  xSemaphoreTake(jpegMutex, portMAX_DELAY);
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/jpeg", (const char *)jpegBuf, jpegLen);
  xSemaphoreGive(jpegMutex);
}

void handleClearWifi()
{
  wifiPrefs.begin(WIFI_PREF_NAMESPACE, false);
  wifiPrefs.clear();
  wifiPrefs.end();

  staConnecting = false;
  // 只斷開 STA 並清除 WiFi 函式庫內部記住的 AP 資訊，softAP (esp32_init) 保持運作，
  // 使用者可繼續連著熱點重新掃描/設定 Wi-Fi
  WiFi.disconnect(false, true);
  Serial.println("已清除儲存的 Wi-Fi 設定並斷線");

  server.send(200, "application/json", "{\"ok\":true}");
}

// 上傳結束後回覆結果；成功則重新啟動套用新韌體/新網頁資料
void handleUpdateFinished()
{
  server.sendHeader("Connection", "close");
  if (otaSuccess)
  {
    server.send(200, "application/json", "{\"ok\":true}");
    Serial.println("OTA 更新完成，重新啟動...");
    delay(500); // 給 WebServer 一點時間把回應送出去
    ESP.restart();
  }
  else
  {
    String err = Update.errorString();
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}");
    otaInProgress = false; // 更新失敗，恢復相機串流
  }
}

// 分段接收上傳的 .bin：/update?target=firmware 寫入 OTA app 分割區，
// /update?target=spiffs 寫入 SPIFFS 分割區 (整包 spiffs 映像檔)
void handleUpdateUpload()
{
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START)
  {
    otaInProgress = true;
    otaSuccess = false;

    bool isSpiffs = server.hasArg("target") && server.arg("target") == "spiffs";
    Serial.printf("OTA 開始: %s (%s)\n", upload.filename.c_str(), isSpiffs ? "SPIFFS" : "firmware");

    if (isSpiffs)
    {
      SPIFFS.end(); // 先卸載，避免更新途中還有檔案存取
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, isSpiffs ? U_SPIFFS : U_FLASH))
    {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
    {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (Update.end(true))
    {
      otaSuccess = true;
      Serial.printf("OTA 寫入完成，共 %u bytes\n", upload.totalSize);
    }
    else
    {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED)
  {
    Update.abort();
    otaInProgress = false;
    Serial.println("OTA 已中斷");
  }
}

void handleLed()
{
  if (server.hasArg("state"))
  {
    String s = server.arg("state");
    ledState = (s == "1" || s == "true");
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  }
  server.send(200, "application/json", String("{\"led\":") + (ledState ? "true" : "false") + "}");
}

void tryAutoConnectSavedWifi()
{
  wifiPrefs.begin(WIFI_PREF_NAMESPACE, true);
  String ssid = wifiPrefs.getString("ssid", "");
  String password = wifiPrefs.getString("password", "");
  wifiPrefs.end();

  if (ssid.length() > 0)
  {
    Serial.print("嘗試自動連線已儲存的 Wi-Fi: ");
    Serial.println(ssid);
    WiFi.begin(ssid.c_str(), password.c_str());
    staConnecting = true;
    staConnectStart = millis();
  }
}
