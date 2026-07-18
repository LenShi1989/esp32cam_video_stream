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
#include <WiFiClientSecure.h>
#include <HTTPClient.h> // Discord Webhook 推播通知
#include <WebServer.h>
#include <DNSServer.h> // 配合 captive portal：DNS 全導向 AP IP，讓手機/電腦連上熱點時自動彈出瀏覽器
#include <SPIFFS.h>
#include <SD_MMC.h> // 板載 microSD (SD 模式儲存 appsetting.json / 告警)
#include <cJSON.h>  // arduino-esp32 內建 (ESP-IDF json 元件)，解析/產生 appsetting.json
#include <Preferences.h>
#include <Update.h> // OTA：網頁上傳 .bin 更新韌體 / SPIFFS
#include <time.h>
#include <sys/time.h> // settimeofday：由網頁同步時間，供告警紀錄時間戳使用
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
static const byte DNS_PORT = 53;

TFT_eSPI tft = TFT_eSPI();
WebServer server(80);
DNSServer dnsServer; // 所有網域查詢導向 AP IP，觸發手機/電腦的 captive portal 自動開瀏覽器
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

// ---------- 移動偵測 (core 0 逐幀比對，core 1 讀寫設定) ----------
// 偵測區域縮取樣成 MOTION_GRID x MOTION_GRID 灰階，與上一幀比較，
// 變動像素比例超過門檻 (16 - 靈敏度)% 即觸發。區域座標為感光元件
// 原始方向的 240x240 座標，網頁端負責把旋轉後的框選換算回來。
#define MOTION_GRID 32
#define MOTION_PIXEL_DIFF 28    // 單點灰階差異門檻 (灰階值 0~250)
#define MOTION_TRIGGER_HOLD_MS 1500 // 觸發後紅框/告警狀態保持時間
#define MOTION_COOLDOWN_MS 5000 // 兩筆告警紀錄的最小間隔，避免連續移動灌爆紀錄

struct MotionConfig
{
  bool enabled;
  int x, y, w, h;  // 偵測區域 (感光元件座標)
  int sensitivity; // 1(最靈敏)~15，門檻 = (16 - sensitivity)%
};
MotionConfig motionCfg = {false, 0, 0, 0, 0, 4};
SemaphoreHandle_t motionCfgMutex;
volatile bool motionResetBaseline = false; // 設定變更後丟棄上一幀基準
volatile float motionDiff = 0;             // 最近一次比對的變動比例 (%)
volatile uint32_t motionTriggeredUntilMs = 0;

// ---------- SD 卡儲存模式 ----------
// AI-Thinker 板載 microSD 與 ST7789 共用腳位 (SD_MMC 1-bit 需 GPIO14/15/2，
// TFT 用 14/13/12/2)，兩者無法並存。開機自動偵測：
//   掛載 SD 成功 -> SD 模式：TFT 停用，appsetting.json (wifi/偵測框/webhook)
//                  與告警紀錄 (index.json + JPEG) 都存 SD，斷電保留
//   掛載失敗     -> TFT 模式：維持原本 NVS (wifi/webhook) + PSRAM (告警)
// 1-bit 模式只占用 GPIO14(CLK)/15(CMD)/2(D0)，GPIO4 LED 不受影響。
// 注意：SD 模式運作時 TFT 排線需拔除，避免干擾 SD 訊號。
#define APPSETTING_PATH "/appsetting.json"
#define ALARM_DIR "/alarms"
#define ALARM_INDEX_PATH "/alarms/index.json"

bool sdMode = false;
SemaphoreHandle_t sdMutex; // 序列化所有 SD 卡檔案存取 (FATFS 併發不可靠)
String sdWifiSsid;         // SD 模式的 Wi-Fi 紀錄 (取代 NVS)
String sdWifiPassword;

// ---------- 移動偵測觸發錄影 (SD 模式，MJPEG-AVI) ----------
// ESP32 沒有 H.264 硬體編碼器，無法產生真正的 .mp4；採用 ESP32-CAM 標準的
// MJPEG-AVI (每幀 JPEG 包進 AVI 容器)，電腦可用 VLC 等直接播放，
// 網頁「錄影回放」頁以串流解析 JPEG 幀方式播放。
// 檔名：/rec/日期_時間_.avi (時間未同步時為 notime_開機秒數_.avi)
#define REC_DIR "/rec"
#define RECORD_SECONDS 120 // 觸發後錄影時長 2 分鐘
#define RECORD_FPS 10
#define RECORD_MAX_FRAMES (RECORD_SECONDS * RECORD_FPS)

volatile bool recording = false;       // 錄影中 (網頁狀態顯示用)
volatile bool recordRequested = false; // 偵測觸發 -> 相機迴圈開始錄影

// 以下僅 cameraTask (core 0) 存取
File recFile;
String recPath;
uint32_t recFrames = 0;
uint32_t recMoviBytes = 0;  // AVI movi 區塊大小 (含 'movi' fourcc)
uint32_t recStartMs = 0;
uint32_t recNextFrameMs = 0;
uint32_t *recIdx = NULL;    // idx1 索引 [offset,size] * RECORD_MAX_FRAMES (PSRAM)

// ---------- 告警紀錄 (觸發時間 + 觸發當下 JPEG) ----------
// TFT 模式：JPEG 存 PSRAM 環形緩衝 (斷電消失，保留 10 筆)。
// SD 模式：JPEG 寫入 /alarms/<id>.jpg + index.json (斷電保留，保留 50 筆)。
// SPIFFS 不用來存告警：僅 128KB 且內部 flash 寫入會與相機任務衝突。
#define MAX_ALARMS_RAM 10
#define MAX_ALARMS_SD 50

struct AlarmRec
{
  uint32_t id;
  time_t epoch; // 觸發時間 (由網頁 POST /time 同步後才是真實時間)
  uint8_t *jpg; // TFT 模式：PSRAM 指標；SD 模式：NULL (內容在 SD 檔案)
  size_t len;
};
AlarmRec alarmList[MAX_ALARMS_SD] = {};
int alarmCap = MAX_ALARMS_RAM;      // 依儲存模式在 setup() 調整
int alarmWrite = 0;                 // 下一筆寫入位置
int alarmCount = 0;                 // 目前保存筆數 (<= alarmCap)
volatile uint32_t alarmLastId = 0;  // 最新一筆 id，網頁輪詢比對用
SemaphoreHandle_t alarmMutex;

// ---------- Discord / Telegram 推播通知 ----------
// 告警觸發時把「時間 + 擷取圖片」推播到 Discord Webhook 和/或 Telegram Bot，
// 手機裝對應 App 並開啟通知即可收到推播。需 STA 連上可上網的 Wi-Fi。傳送由
// 獨立任務處理 (TLS 握手需數秒)，避免卡住相機任務或 WebServer。
#define NOTIFY_PREF_NAMESPACE "notify"
Preferences notifyPrefs;
String discordWebhook;            // 空字串 = 未設定/停用
String telegramBotToken;          // 空字串 = 未設定/停用；格式 123456:ABC-...
String telegramChatId;            // 收訊的 chat id (可為負數群組 id)
SemaphoreHandle_t notifyMutex;    // 保護以上推播設定跨任務存取
// 待推播的任務：alarmId (0 = 測試訊息) + 要推播到哪些管道
struct NotifyJob
{
  uint32_t alarmId;
  bool toDiscord;
  bool toTelegram;
};
QueueHandle_t notifyQueue;
volatile int tzOffsetMin = 0;     // 瀏覽器時區 (分鐘，UTC+8 = 480)，供訊息時間顯示

// ---------- 使用者登入保護 (HTTP Basic Auth) ----------
// authUser 空字串 = 停用登入保護 (預設)，一旦設定，網頁 API 皆需登入才能存取
// (靜態檔案 style.css/app.js 除外)。憑證存放位置比照 Discord/Telegram 設定：
// TFT 模式存 NVS，SD 模式存 appsetting.json。忘記密碼需清除該儲存位置才能重設。
#define AUTH_PREF_NAMESPACE "auth"
Preferences authPrefs;
String authUser; // 空字串 = 未設定/停用
String authPass;
SemaphoreHandle_t authMutex;
bool otaAuthOk = true; // OTA 上傳開始時檢查一次，未授權則整段上傳略過寫入

static bool initCamera();
void cameraTask(void *param);
void detectMotionOnFrame(camera_fb_t *fb);
void addAlarmRecord(camera_fb_t *fb);
void drawMotionOverlayOnTft(int16_t xoff, int16_t yoff);
void drawBellIcon(int16_t cx, int16_t cy);
void notifyTask(void *param);
void sendDiscordNotify(uint32_t alarmId);
void sendTelegramNotify(uint32_t alarmId);
String formatEpoch(time_t epoch);
void loadAppSettings();
void saveAppSettings();
void loadAlarmIndex();
void saveAlarmIndexLocked();
String alarmJpgPath(uint32_t id);
bool startRecording();
void stopRecording();
void writeRecordingFrame(const uint8_t *jpg, size_t len);
String recFilePath();
void handleRecList();
void handleRecFile();
void handleRecDelete();
String jsonEscape(const String &s);
void handleNotifyConfigGet();
void handleNotifyConfigSet();
void handleNotifyTest();
void handleNotifyTelegramTest();
bool isAuthed();
bool checkAuth();
void handleAuthConfigGet();
void handleAuthConfigSet();
void handleRoot();
void handleCaptivePortal();
void handleScan();
void handleConnect();
void handleStatus();
void handleLed();
void handleCapture();
void handleClearWifi();
void handleMotionConfig();
void handleMotionStatus();
void handleAlarmList();
void handleAlarmImage();
void handleAlarmClear();
void handleTimeSync();
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

  jpegMutex = xSemaphoreCreateMutex();
  jpegReadySem = xSemaphoreCreateBinary();
  motionCfgMutex = xSemaphoreCreateMutex();
  alarmMutex = xSemaphoreCreateMutex();
  notifyMutex = xSemaphoreCreateMutex();
  sdMutex = xSemaphoreCreateMutex();
  authMutex = xSemaphoreCreateMutex();
  notifyQueue = xQueueCreate(5, sizeof(NotifyJob));

  // 自動偵測儲存模式：能掛載 microSD (1-bit) 就用 SD，否則維持 TFT + NVS/RAM
  if (SD_MMC.begin("/sdcard", true)) // true = 1-bit 模式，只占用 GPIO14/15/2
  {
    sdMode = true;
    alarmCap = MAX_ALARMS_SD;
    SD_MMC.mkdir(ALARM_DIR);
    loadAppSettings(); // wifi / 偵測框 / webhook
    loadAlarmIndex();  // 還原既有告警紀錄
    Serial.printf("SD 卡已掛載 (%.1f MB)：SD 模式，TFT 停用，設定與告警存 SD\n",
                  SD_MMC.cardSize() / 1048576.0f);
  }
  else
  {
    Serial.println("未偵測到 SD 卡：TFT 模式，設定存 NVS，告警存 RAM");
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

  // DNS 全導向 AP IP：手機/電腦連上 esp32_init 熱點時，系統的 captive portal
  // 偵測請求 (如 connectivitycheck.gstatic.com、captive.apple.com) 會被導到本機，
  // 收到非預期回應後系統即自動彈出瀏覽器視窗導向設定頁。
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  tryAutoConnectSavedWifi();

  // TFT 模式才從 NVS 讀推播設定 (SD 模式已由 appsetting.json 載入)
  if (!sdMode)
  {
    notifyPrefs.begin(NOTIFY_PREF_NAMESPACE, true);
    discordWebhook = notifyPrefs.getString("webhook", "");
    telegramBotToken = notifyPrefs.getString("tgToken", "");
    telegramChatId = notifyPrefs.getString("tgChatId", "");
    notifyPrefs.end();

    authPrefs.begin(AUTH_PREF_NAMESPACE, true);
    authUser = authPrefs.getString("user", "");
    authPass = authPrefs.getString("pass", "");
    authPrefs.end();
  }
  if (authUser.length() > 0)
  {
    Serial.println("登入保護已啟用");
  }
  if (discordWebhook.length() > 0)
  {
    Serial.println("Discord 推播已設定");
  }
  if (telegramBotToken.length() > 0 && telegramChatId.length() > 0)
  {
    Serial.println("Telegram 推播已設定");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.serveStatic("/style.css", SPIFFS, "/style.css");
  server.serveStatic("/app.js", SPIFFS, "/app.js");
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/connect", HTTP_POST, handleConnect);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/led", HTTP_GET, handleLed);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/clear", HTTP_POST, handleClearWifi);
  server.on("/motion/config", HTTP_POST, handleMotionConfig);
  server.on("/motion/status", HTTP_GET, handleMotionStatus);
  server.on("/alarms", HTTP_GET, handleAlarmList);
  server.on("/alarm/img", HTTP_GET, handleAlarmImage);
  server.on("/alarm/clear", HTTP_POST, handleAlarmClear);
  server.on("/notify/config", HTTP_GET, handleNotifyConfigGet);
  server.on("/notify/config", HTTP_POST, handleNotifyConfigSet);
  server.on("/notify/test", HTTP_POST, handleNotifyTest);
  server.on("/notify/telegram/test", HTTP_POST, handleNotifyTelegramTest);
  server.on("/auth/config", HTTP_GET, handleAuthConfigGet);
  server.on("/auth/config", HTTP_POST, handleAuthConfigSet);
  server.on("/recordings", HTTP_GET, handleRecList);
  server.on("/rec/file", HTTP_GET, handleRecFile);
  server.on("/rec/delete", HTTP_POST, handleRecDelete);
  server.on("/time", HTTP_POST, handleTimeSync);
  server.on("/update", HTTP_POST, handleUpdateFinished, handleUpdateUpload);
  server.onNotFound(handleCaptivePortal);
  server.begin();
  Serial.println("Web Server 已啟動 (core 1)");

  // 相機擷取 + TFT 顯示放到備用核心 (core 0)，避免與 Web Server 互相卡住
  xTaskCreatePinnedToCore(cameraTask, "cameraTask", 10240, NULL, 1, NULL, 0);
  // Discord 推播任務放 core 1：TLS 握手耗時數秒，獨立任務不卡相機/WebServer
  xTaskCreatePinnedToCore(notifyTask, "notifyTask", 8192, NULL, 1, NULL, 1);
}

void loop()
{
  server.handleClient();
  dnsServer.processNextRequest();

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
  // SD 模式下 TFT 腳位被 SD_MMC 占用，完全不碰 TFT
  if (!sdMode)
  {
    tft.init();
    tft.setRotation(1); // 0~3，順時針旋轉 90°
  }

  if (!initCamera())
  {
    if (!sdMode)
    {
      tft.setCursor(10, 100);
      tft.setTextColor(TFT_RED);
      tft.println("Camera init failed!");
    }
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
      if (recording)
      {
        stopRecording(); // 先收尾寫入索引與檔頭，避免留下毀損檔
      }
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
      if (!sdMode)
      {
        tft.startWrite();
        tft.setAddrWindow(x, y, fb->width, fb->height);
        tft.pushPixels((uint16_t *)fb->buf, fb->len / 2);
        tft.endWrite();
      }

      // 移動偵測在裝置端逐幀進行，瀏覽器關掉也持續監控 (SD 模式亦然)
      detectMotionOnFrame(fb);

      if (!sdMode)
      {
        // 疊加偵測框 (綠=監控/紅=觸發) 與觸發鈴鐺；下一幀影像會蓋掉重畫
        drawMotionOverlayOnTft(x, y);
      }

      // ---------- 觸發錄影 (SD 模式) ----------
      if (sdMode)
      {
        if (recordRequested && !recording)
        {
          recordRequested = false;
          startRecording();
        }
        if (recording)
        {
          uint32_t nowMs = millis();
          if (nowMs - recStartMs >= RECORD_SECONDS * 1000UL || recFrames >= RECORD_MAX_FRAMES)
          {
            stopRecording();
          }
          else if ((int32_t)(nowMs - recNextFrameMs) >= 0)
          {
            recNextFrameMs += 1000 / RECORD_FPS;
            if ((int32_t)(nowMs - recNextFrameMs) > 1000)
            {
              recNextFrameMs = nowMs; // SD 寫入落後過多時重新對時
            }
            uint8_t *out = NULL;
            size_t outLen = 0;
            if (frame2jpg(fb, 80, &out, &outLen))
            {
              writeRecordingFrame(out, outLen);
              free(out);
            }
          }
        }
      }
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

// ---------- 移動偵測：core 0 逐幀比對 (RGB565) ----------

void detectMotionOnFrame(camera_fb_t *fb)
{
  static uint8_t prevGray[MOTION_GRID * MOTION_GRID];
  static bool prevValid = false;
  static uint32_t lastAlarmMs = 0;

  MotionConfig cfg;
  xSemaphoreTake(motionCfgMutex, portMAX_DELAY);
  cfg = motionCfg;
  if (motionResetBaseline)
  {
    motionResetBaseline = false;
    prevValid = false;
  }
  xSemaphoreGive(motionCfgMutex);

  if (!cfg.enabled || cfg.w < 8 || cfg.h < 8)
  {
    prevValid = false;
    motionDiff = 0;
    return;
  }

  // 區域內等距取 MOTION_GRID x MOTION_GRID 點轉灰階
  // (相機 RGB565 為 big-endian，需先交換位元組)
  const uint16_t *pix = (const uint16_t *)fb->buf;
  uint8_t cur[MOTION_GRID * MOTION_GRID];
  for (int gy = 0; gy < MOTION_GRID; gy++)
  {
    int sy = cfg.y + (gy * cfg.h) / MOTION_GRID;
    if (sy >= fb->height) sy = fb->height - 1;
    for (int gx = 0; gx < MOTION_GRID; gx++)
    {
      int sx = cfg.x + (gx * cfg.w) / MOTION_GRID;
      if (sx >= fb->width) sx = fb->width - 1;
      uint16_t p = pix[sy * fb->width + sx];
      p = (p >> 8) | (p << 8);
      uint8_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
      // 近似亮度 3R + 2G + B (max 250)，與網頁版舊算法權重一致
      cur[gy * MOTION_GRID + gx] = 3 * r + 2 * g + b;
    }
  }

  if (prevValid)
  {
    int changed = 0;
    for (int i = 0; i < MOTION_GRID * MOTION_GRID; i++)
    {
      if (abs((int)cur[i] - (int)prevGray[i]) > MOTION_PIXEL_DIFF) changed++;
    }
    float ratio = changed * 100.0f / (MOTION_GRID * MOTION_GRID);
    motionDiff = ratio;

    // 靈敏度 1(最靈敏)~15：變動像素比例門檻 %
    if (ratio > (float)(16 - cfg.sensitivity))
    {
      motionTriggeredUntilMs = millis() + MOTION_TRIGGER_HOLD_MS;
      // 錄影中不再截圖告警、不再推播通知，只保留紅框/DIFF% 顯示；
      // 錄影結束 (recording 變 false) 後才繼續正常觸發偵測
      if (!recording && (lastAlarmMs == 0 || millis() - lastAlarmMs > MOTION_COOLDOWN_MS))
      {
        lastAlarmMs = millis();
        addAlarmRecord(fb);
        if (sdMode)
        {
          recordRequested = true; // 觸發後錄影 2 分鐘 (錄影中再觸發則忽略)
        }
      }
    }
  }
  memcpy(prevGray, cur, sizeof(cur));
  prevValid = true;
}

// 觸發告警：把當下畫面轉 JPEG 存進環形緩衝 (滿了覆蓋最舊一筆)。
// TFT 模式存 PSRAM；SD 模式寫入 /alarms/<id>.jpg 並更新 index.json。
void addAlarmRecord(camera_fb_t *fb)
{
  uint8_t *out = NULL;
  size_t outLen = 0;
  if (!frame2jpg(fb, 80, &out, &outLen))
  {
    Serial.println("告警擷取失敗：frame2jpg 錯誤");
    return;
  }

  uint8_t *copy = NULL;
  if (!sdMode)
  {
    // frame2jpg 配置在內部 RAM，複製到 PSRAM 保存以免排擠 WiFi 堆疊
    copy = (uint8_t *)ps_malloc(outLen);
    if (!copy)
    {
      free(out);
      Serial.println("告警擷取失敗：PSRAM 配置失敗");
      return;
    }
    memcpy(copy, out, outLen);
  }

  xSemaphoreTake(alarmMutex, portMAX_DELAY);
  AlarmRec *rec = &alarmList[alarmWrite];
  uint32_t id = alarmLastId + 1;

  bool stored = true;
  if (sdMode)
  {
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    if (rec->id != 0)
    {
      SD_MMC.remove(alarmJpgPath(rec->id)); // 環形覆蓋：先刪被淘汰的舊檔
    }
    File f = SD_MMC.open(alarmJpgPath(id), FILE_WRITE);
    if (f)
    {
      stored = f.write(out, outLen) == outLen;
      f.close();
    }
    else
    {
      stored = false;
    }
    xSemaphoreGive(sdMutex);
  }
  free(out);

  if (!stored)
  {
    xSemaphoreGive(alarmMutex);
    Serial.println("告警擷取失敗：SD 卡寫入失敗");
    return;
  }

  if (rec->jpg)
  {
    free(rec->jpg); // TFT 模式：釋放被覆蓋的舊 JPEG
  }
  rec->id = id;
  rec->epoch = time(NULL);
  rec->jpg = copy; // SD 模式為 NULL，內容在 SD 檔案
  rec->len = outLen;
  alarmLastId = id;
  alarmWrite = (alarmWrite + 1) % alarmCap;
  if (alarmCount < alarmCap) alarmCount++;
  if (sdMode)
  {
    saveAlarmIndexLocked();
  }
  xSemaphoreGive(alarmMutex);

  // 交給通知任務推播 Discord/Telegram (佇列滿了就放棄這則，不能卡住相機任務)
  if (notifyQueue)
  {
    NotifyJob job{id, true, true};
    xQueueSend(notifyQueue, &job, 0);
  }

  Serial.printf("移動偵測告警 #%u，JPEG %u bytes\n", id, (unsigned)outLen);
}

// ---------- ST7789 疊加偵測框 / 觸發鈴鐺 (core 0，每幀重畫) ----------
// 偵測框座標為感光元件座標；影像與圖形都經過 tft.setRotation 同一座標
// 轉換，因此直接以相同座標繪製即可與畫面內容對齊。
void drawMotionOverlayOnTft(int16_t xoff, int16_t yoff)
{
  xSemaphoreTake(motionCfgMutex, portMAX_DELAY);
  MotionConfig cfg = motionCfg;
  xSemaphoreGive(motionCfgMutex);
  if (!cfg.enabled)
  {
    return;
  }

  bool triggered = (int32_t)(motionTriggeredUntilMs - millis()) > 0;
  uint16_t color = triggered ? TFT_RED : TFT_GREEN;
  tft.drawRect(xoff + cfg.x, yoff + cfg.y, cfg.w, cfg.h, color);
  // 內縮一圈加粗，串流畫面上比較醒目
  tft.drawRect(xoff + cfg.x + 1, yoff + cfg.y + 1, cfg.w - 2, cfg.h - 2, color);

  if (triggered)
  {
    drawBellIcon(TFT_WIDTH - 16, 16);
  }
}

// 用基本圖形拼出約 20x22 的小鈴鐺 (cx, cy 為鐘身中心)
void drawBellIcon(int16_t cx, int16_t cy)
{
  const uint16_t c = TFT_YELLOW;
  tft.fillCircle(cx, cy - 8, 2, c);        // 頂部掛鈕
  tft.fillCircle(cx, cy - 2, 7, c);        // 鐘頂圓弧
  tft.fillRect(cx - 7, cy - 2, 15, 7, c);  // 鐘身
  tft.fillRect(cx - 10, cy + 5, 21, 2, c); // 底部外擴邊
  tft.fillCircle(cx, cy + 9, 2, c);        // 鈴舌
}

// ================= Discord / Telegram 推播通知 (core 1 獨立任務) =================

void notifyTask(void *param)
{
  for (;;)
  {
    NotifyJob job;
    if (xQueueReceive(notifyQueue, &job, portMAX_DELAY) == pdTRUE)
    {
      if (job.toDiscord) sendDiscordNotify(job.alarmId);
      if (job.toTelegram) sendTelegramNotify(job.alarmId);
    }
  }
}

// 依瀏覽器同步的時區把 epoch 格式化成當地時間字串
String formatEpoch(time_t epoch)
{
  if (epoch < 1000000000)
  {
    return "時間未同步";
  }
  time_t local = epoch + (time_t)tzOffsetMin * 60;
  struct tm tmv;
  gmtime_r(&local, &tmv);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
  return String(buf);
}

// 以 multipart/form-data 把「時間 + 觸發擷取圖片」POST 到 Discord Webhook。
// alarmId = 0 表示測試訊息 (不帶圖片)。
void sendDiscordNotify(uint32_t alarmId)
{
  // OTA 期間讓出頻寬與記憶體，等更新結束再送
  while (otaInProgress)
  {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  xSemaphoreTake(notifyMutex, portMAX_DELAY);
  String url = discordWebhook;
  xSemaphoreGive(notifyMutex);
  if (url.length() == 0)
  {
    return;
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("Discord 通知略過：STA 未連上可上網的 Wi-Fi");
    return;
  }

  // 取出告警 JPEG 副本，避免傳送期間環形緩衝被覆蓋
  // (TFT 模式從 PSRAM 複製；SD 模式從 SD 檔案讀入)
  uint8_t *jpg = NULL;
  size_t jpgSize = 0;
  time_t epoch = time(NULL);
  if (alarmId != 0)
  {
    xSemaphoreTake(alarmMutex, portMAX_DELAY);
    for (int i = 0; i < alarmCount; i++)
    {
      if (alarmList[i].id != alarmId)
      {
        continue;
      }
      epoch = alarmList[i].epoch;
      if (alarmList[i].jpg)
      {
        jpg = (uint8_t *)ps_malloc(alarmList[i].len);
        if (jpg)
        {
          memcpy(jpg, alarmList[i].jpg, alarmList[i].len);
          jpgSize = alarmList[i].len;
        }
      }
      else if (sdMode)
      {
        xSemaphoreTake(sdMutex, portMAX_DELAY);
        File f = SD_MMC.open(alarmJpgPath(alarmId), FILE_READ);
        if (f)
        {
          size_t fsize = f.size();
          jpg = (uint8_t *)ps_malloc(fsize);
          if (jpg && f.read(jpg, fsize) == fsize)
          {
            jpgSize = fsize;
          }
          else if (jpg)
          {
            free(jpg);
            jpg = NULL;
          }
          f.close();
        }
        xSemaphoreGive(sdMutex);
      }
      break;
    }
    xSemaphoreGive(alarmMutex);
  }

  String content = (alarmId != 0)
                       ? "⚠️ ESP32-CAM 移動偵測告警 #" + String(alarmId)
                       : "🔔 ESP32-CAM 測試通知";
  String payload = "{\"content\":\"" + content + "\\n時間：" + formatEpoch(epoch) + "\"}";

  const String boundary = "----Esp32CamAlarmBoundary";
  String pre = "--" + boundary + "\r\n"
               "Content-Disposition: form-data; name=\"payload_json\"\r\n"
               "Content-Type: application/json\r\n\r\n" + payload;
  String imgHead = "\r\n--" + boundary + "\r\n"
                   "Content-Disposition: form-data; name=\"files[0]\"; filename=\"alarm_" +
                   String(alarmId) + ".jpg\"\r\n"
                   "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  // Discord 要求整包 multipart，先在 PSRAM 組好再一次 POST
  size_t total = pre.length() + (jpg ? imgHead.length() + jpgSize : 0) + tail.length();
  uint8_t *body = (uint8_t *)ps_malloc(total);
  if (!body)
  {
    if (jpg) free(jpg);
    Serial.println("Discord 通知失敗：PSRAM 配置失敗");
    return;
  }
  size_t off = 0;
  memcpy(body + off, pre.c_str(), pre.length());
  off += pre.length();
  if (jpg)
  {
    memcpy(body + off, imgHead.c_str(), imgHead.length());
    off += imgHead.length();
    memcpy(body + off, jpg, jpgSize);
    off += jpgSize;
    free(jpg);
  }
  memcpy(body + off, tail.c_str(), tail.length());

  WiFiClientSecure client;
  client.setInsecure(); // 不驗證憑證：webhook 網址本身即含密鑰，省去維護根憑證
  HTTPClient https;
  https.setConnectTimeout(8000);
  https.setTimeout(15000);
  if (https.begin(client, url))
  {
    https.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    int code = https.POST(body, total);
    Serial.printf("Discord 通知 #%u -> HTTP %d %s\n", alarmId, code,
                  (code >= 200 && code < 300) ? "OK" : "失敗");
    https.end();
  }
  else
  {
    Serial.println("Discord 通知失敗：無法解析 Webhook URL");
  }
  free(body);
}

// 以 multipart/form-data 呼叫 Telegram Bot API 推播「時間 + 觸發擷取圖片」。
// 有圖片用 sendPhoto，測試訊息 (alarmId = 0，無圖片) 用 sendMessage。
void sendTelegramNotify(uint32_t alarmId)
{
  // OTA 期間讓出頻寬與記憶體，等更新結束再送
  while (otaInProgress)
  {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  xSemaphoreTake(notifyMutex, portMAX_DELAY);
  String token = telegramBotToken;
  String chatId = telegramChatId;
  xSemaphoreGive(notifyMutex);
  if (token.length() == 0 || chatId.length() == 0)
  {
    return;
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("Telegram 通知略過：STA 未連上可上網的 Wi-Fi");
    return;
  }

  // 取出告警 JPEG 副本，避免傳送期間環形緩衝被覆蓋
  // (TFT 模式從 PSRAM 複製；SD 模式從 SD 檔案讀入)
  uint8_t *jpg = NULL;
  size_t jpgSize = 0;
  time_t epoch = time(NULL);
  if (alarmId != 0)
  {
    xSemaphoreTake(alarmMutex, portMAX_DELAY);
    for (int i = 0; i < alarmCount; i++)
    {
      if (alarmList[i].id != alarmId)
      {
        continue;
      }
      epoch = alarmList[i].epoch;
      if (alarmList[i].jpg)
      {
        jpg = (uint8_t *)ps_malloc(alarmList[i].len);
        if (jpg)
        {
          memcpy(jpg, alarmList[i].jpg, alarmList[i].len);
          jpgSize = alarmList[i].len;
        }
      }
      else if (sdMode)
      {
        xSemaphoreTake(sdMutex, portMAX_DELAY);
        File f = SD_MMC.open(alarmJpgPath(alarmId), FILE_READ);
        if (f)
        {
          size_t fsize = f.size();
          jpg = (uint8_t *)ps_malloc(fsize);
          if (jpg && f.read(jpg, fsize) == fsize)
          {
            jpgSize = fsize;
          }
          else if (jpg)
          {
            free(jpg);
            jpg = NULL;
          }
          f.close();
        }
        xSemaphoreGive(sdMutex);
      }
      break;
    }
    xSemaphoreGive(alarmMutex);
  }

  bool hasImage = (jpg != NULL);
  String caption = (alarmId != 0)
                       ? "⚠️ ESP32-CAM 移動偵測告警 #" + String(alarmId)
                       : "🔔 ESP32-CAM 測試通知";
  caption += "\n時間：" + formatEpoch(epoch);

  const String boundary = "----Esp32CamTgBoundary";
  String pre = "--" + boundary + "\r\n"
               "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + chatId + "\r\n"
               "--" + boundary + "\r\n"
               "Content-Disposition: form-data; name=\"" + String(hasImage ? "caption" : "text") +
               "\"\r\n\r\n" + caption;
  String imgHead = "\r\n--" + boundary + "\r\n"
                   "Content-Disposition: form-data; name=\"photo\"; filename=\"alarm_" +
                   String(alarmId) + ".jpg\"\r\n"
                   "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  size_t total = pre.length() + (hasImage ? imgHead.length() + jpgSize : 0) + tail.length();
  uint8_t *body = (uint8_t *)ps_malloc(total);
  if (!body)
  {
    if (jpg) free(jpg);
    Serial.println("Telegram 通知失敗：PSRAM 配置失敗");
    return;
  }
  size_t off = 0;
  memcpy(body + off, pre.c_str(), pre.length());
  off += pre.length();
  if (hasImage)
  {
    memcpy(body + off, imgHead.c_str(), imgHead.length());
    off += imgHead.length();
    memcpy(body + off, jpg, jpgSize);
    off += jpgSize;
    free(jpg);
  }
  memcpy(body + off, tail.c_str(), tail.length());

  String url = "https://api.telegram.org/bot" + token + "/" + (hasImage ? "sendPhoto" : "sendMessage");
  WiFiClientSecure client;
  client.setInsecure(); // 不驗證憑證：Bot Token 本身即含密鑰，省去維護根憑證
  HTTPClient https;
  https.setConnectTimeout(8000);
  https.setTimeout(15000);
  if (https.begin(client, url))
  {
    https.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    int code = https.POST(body, total);
    Serial.printf("Telegram 通知 #%u -> HTTP %d %s\n", alarmId, code,
                  (code >= 200 && code < 300) ? "OK" : "失敗");
    https.end();
  }
  else
  {
    Serial.println("Telegram 通知失敗：無法連線 api.telegram.org");
  }
  free(body);
}

// ================= SD 卡儲存 (appsetting.json / 告警索引) =================

String alarmJpgPath(uint32_t id)
{
  return String(ALARM_DIR) + "/" + String(id) + ".jpg";
}

// 讀取 /appsetting.json：wifi 設定紀錄、偵測框座標紀錄、Discord webhook。
// 僅在 setup() 單執行緒階段呼叫，直接寫全域變數不需鎖。
void loadAppSettings()
{
  File f = SD_MMC.open(APPSETTING_PATH, FILE_READ);
  if (!f)
  {
    Serial.println("SD 無 appsetting.json，將於首次設定時建立");
    return;
  }
  String raw = f.readString();
  f.close();

  cJSON *root = cJSON_Parse(raw.c_str());
  if (!root)
  {
    Serial.println("appsetting.json 解析失敗，忽略");
    return;
  }

  cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
  if (wifi)
  {
    cJSON *it = cJSON_GetObjectItem(wifi, "ssid");
    if (cJSON_IsString(it)) sdWifiSsid = it->valuestring;
    it = cJSON_GetObjectItem(wifi, "password");
    if (cJSON_IsString(it)) sdWifiPassword = it->valuestring;
  }

  cJSON *motion = cJSON_GetObjectItem(root, "motion");
  if (motion)
  {
    MotionConfig cfg = motionCfg;
    cJSON *it = cJSON_GetObjectItem(motion, "enabled");
    if (cJSON_IsBool(it)) cfg.enabled = cJSON_IsTrue(it);
    it = cJSON_GetObjectItem(motion, "x");
    if (cJSON_IsNumber(it)) cfg.x = it->valueint;
    it = cJSON_GetObjectItem(motion, "y");
    if (cJSON_IsNumber(it)) cfg.y = it->valueint;
    it = cJSON_GetObjectItem(motion, "w");
    if (cJSON_IsNumber(it)) cfg.w = it->valueint;
    it = cJSON_GetObjectItem(motion, "h");
    if (cJSON_IsNumber(it)) cfg.h = it->valueint;
    it = cJSON_GetObjectItem(motion, "sens");
    if (cJSON_IsNumber(it)) cfg.sensitivity = it->valueint;

    // 邊界防呆：檔案可能被手動編輯過
    cfg.x = constrain(cfg.x, 0, TFT_WIDTH - 1);
    cfg.y = constrain(cfg.y, 0, TFT_HEIGHT - 1);
    cfg.w = constrain(cfg.w, 0, TFT_WIDTH - cfg.x);
    cfg.h = constrain(cfg.h, 0, TFT_HEIGHT - cfg.y);
    if (cfg.sensitivity < 1 || cfg.sensitivity > 15) cfg.sensitivity = 4;
    if (cfg.w < 8 || cfg.h < 8) cfg.enabled = false;
    motionCfg = cfg;
  }

  cJSON *notify = cJSON_GetObjectItem(root, "notify");
  if (notify)
  {
    cJSON *it = cJSON_GetObjectItem(notify, "webhook");
    if (cJSON_IsString(it)) discordWebhook = it->valuestring;
    it = cJSON_GetObjectItem(notify, "tgToken");
    if (cJSON_IsString(it)) telegramBotToken = it->valuestring;
    it = cJSON_GetObjectItem(notify, "tgChatId");
    if (cJSON_IsString(it)) telegramChatId = it->valuestring;
  }

  cJSON *auth = cJSON_GetObjectItem(root, "auth");
  if (auth)
  {
    cJSON *it = cJSON_GetObjectItem(auth, "user");
    if (cJSON_IsString(it)) authUser = it->valuestring;
    it = cJSON_GetObjectItem(auth, "pass");
    if (cJSON_IsString(it)) authPass = it->valuestring;
  }

  cJSON_Delete(root);
  Serial.println("已載入 appsetting.json");
}

// 把目前設定寫回 /appsetting.json (wifi / 偵測框 / webhook)
void saveAppSettings()
{
  if (!sdMode)
  {
    return;
  }

  xSemaphoreTake(motionCfgMutex, portMAX_DELAY);
  MotionConfig cfg = motionCfg;
  xSemaphoreGive(motionCfgMutex);
  xSemaphoreTake(notifyMutex, portMAX_DELAY);
  String webhook = discordWebhook;
  String tgToken = telegramBotToken;
  String tgChatId = telegramChatId;
  xSemaphoreGive(notifyMutex);
  xSemaphoreTake(authMutex, portMAX_DELAY);
  String authU = authUser;
  String authP = authPass;
  xSemaphoreGive(authMutex);

  cJSON *root = cJSON_CreateObject();
  cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
  cJSON_AddStringToObject(wifi, "ssid", sdWifiSsid.c_str());
  cJSON_AddStringToObject(wifi, "password", sdWifiPassword.c_str());
  cJSON *motion = cJSON_AddObjectToObject(root, "motion");
  cJSON_AddBoolToObject(motion, "enabled", cfg.enabled);
  cJSON_AddNumberToObject(motion, "x", cfg.x);
  cJSON_AddNumberToObject(motion, "y", cfg.y);
  cJSON_AddNumberToObject(motion, "w", cfg.w);
  cJSON_AddNumberToObject(motion, "h", cfg.h);
  cJSON_AddNumberToObject(motion, "sens", cfg.sensitivity);
  cJSON *notify = cJSON_AddObjectToObject(root, "notify");
  cJSON_AddStringToObject(notify, "webhook", webhook.c_str());
  cJSON_AddStringToObject(notify, "tgToken", tgToken.c_str());
  cJSON_AddStringToObject(notify, "tgChatId", tgChatId.c_str());
  cJSON *auth = cJSON_AddObjectToObject(root, "auth");
  cJSON_AddStringToObject(auth, "user", authU.c_str());
  cJSON_AddStringToObject(auth, "pass", authP.c_str());

  char *text = cJSON_Print(root); // 含縮排排版，可直接用電腦開卡檢視/編輯
  cJSON_Delete(root);
  if (!text)
  {
    Serial.println("appsetting.json 產生失敗");
    return;
  }

  xSemaphoreTake(sdMutex, portMAX_DELAY);
  File f = SD_MMC.open(APPSETTING_PATH, FILE_WRITE);
  bool ok = false;
  if (f)
  {
    ok = f.print(text) > 0;
    f.close();
  }
  xSemaphoreGive(sdMutex);
  cJSON_free(text);
  if (!ok)
  {
    Serial.println("appsetting.json 寫入失敗");
  }
}

// 開機還原 /alarms/index.json -> RAM 環形索引 (JPEG 內容留在 SD 檔案)
void loadAlarmIndex()
{
  File f = SD_MMC.open(ALARM_INDEX_PATH, FILE_READ);
  if (!f)
  {
    return;
  }
  String raw = f.readString();
  f.close();

  cJSON *root = cJSON_Parse(raw.c_str());
  if (!root)
  {
    return;
  }

  cJSON *arr = cJSON_GetObjectItem(root, "alarms");
  int n = cJSON_GetArraySize(arr); // arr 為 NULL 時回傳 0
  int start = n > alarmCap ? n - alarmCap : 0; // 超量只取最新 alarmCap 筆
  for (int i = start; i < n; i++)
  {
    cJSON *item = cJSON_GetArrayItem(arr, i);
    cJSON *idIt = cJSON_GetObjectItem(item, "id");
    if (!cJSON_IsNumber(idIt))
    {
      continue;
    }
    AlarmRec *rec = &alarmList[alarmWrite];
    rec->id = (uint32_t)idIt->valuedouble;
    cJSON *tIt = cJSON_GetObjectItem(item, "time");
    rec->epoch = cJSON_IsNumber(tIt) ? (time_t)tIt->valuedouble : 0;
    cJSON *lIt = cJSON_GetObjectItem(item, "len");
    rec->len = cJSON_IsNumber(lIt) ? (size_t)lIt->valuedouble : 0;
    rec->jpg = NULL;
    if (rec->id > alarmLastId)
    {
      alarmLastId = rec->id;
    }
    alarmWrite = (alarmWrite + 1) % alarmCap;
    if (alarmCount < alarmCap)
    {
      alarmCount++;
    }
  }
  cJSON_Delete(root);
  Serial.printf("已從 SD 還原 %d 筆告警紀錄\n", alarmCount);
}

// 將 RAM 索引寫回 index.json (由最舊到最新)；呼叫者需已持有 alarmMutex
void saveAlarmIndexLocked()
{
  cJSON *root = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(root, "alarms");
  for (int i = 0; i < alarmCount; i++)
  {
    int idx = ((alarmWrite - alarmCount + i) % alarmCap + alarmCap) % alarmCap;
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "id", alarmList[idx].id);
    cJSON_AddNumberToObject(item, "time", (double)(uint32_t)alarmList[idx].epoch);
    cJSON_AddNumberToObject(item, "len", (double)alarmList[idx].len);
    cJSON_AddItemToArray(arr, item);
  }
  char *text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!text)
  {
    return;
  }

  xSemaphoreTake(sdMutex, portMAX_DELAY);
  File f = SD_MMC.open(ALARM_INDEX_PATH, FILE_WRITE);
  if (f)
  {
    f.print(text);
    f.close();
  }
  xSemaphoreGive(sdMutex);
  cJSON_free(text);
}

// ================= 觸發錄影：MJPEG-AVI 寫入 (core 0) =================

static void wrLE32(uint8_t *p, uint32_t v)
{
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void wrLE16(uint8_t *p, uint16_t v)
{
  p[0] = v; p[1] = v >> 8;
}
static void wrFourcc(uint8_t *p, const char *s)
{
  memcpy(p, s, 4);
}

// 檔名：日期_時間_.avi (依瀏覽器同步的時區)；時間未同步用開機秒數
String recFilePath()
{
  time_t now = time(NULL);
  if (now < 1000000000)
  {
    return String(REC_DIR) + "/notime_" + String(millis() / 1000) + "_.avi";
  }
  time_t local = now + (time_t)tzOffsetMin * 60;
  struct tm tmv;
  gmtime_r(&local, &tmv);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S_", &tmv);
  return String(REC_DIR) + "/" + buf + ".avi";
}

// 寫入 224 bytes AVI 檔頭 (RIFF/hdrl/movi)；幀數與大小先填 0，結束時回填。
// 回填位置：riffSize@4、totalFrames@48、strh.length@140、moviSize@216，首幀@224
bool startRecording()
{
  if (!recIdx)
  {
    recIdx = (uint32_t *)ps_malloc(RECORD_MAX_FRAMES * 2 * sizeof(uint32_t));
    if (!recIdx)
    {
      Serial.println("錄影失敗：idx1 索引配置失敗");
      return false;
    }
  }

  xSemaphoreTake(sdMutex, portMAX_DELAY);
  uint64_t freeBytes = SD_MMC.totalBytes() - SD_MMC.usedBytes();
  if (freeBytes < 32ULL * 1024 * 1024) // 預估一段約 18MB，保留餘裕
  {
    xSemaphoreGive(sdMutex);
    Serial.println("錄影略過：SD 卡剩餘空間不足 32MB");
    return false;
  }
  SD_MMC.mkdir(REC_DIR);
  recPath = recFilePath();
  recFile = SD_MMC.open(recPath, FILE_WRITE);
  if (!recFile)
  {
    xSemaphoreGive(sdMutex);
    Serial.println("錄影失敗：無法建立檔案 " + recPath);
    return false;
  }

  uint8_t hdr[224];
  memset(hdr, 0, sizeof(hdr));
  wrFourcc(hdr + 0, "RIFF");                    // riffSize @4 回填
  wrFourcc(hdr + 8, "AVI ");
  wrFourcc(hdr + 12, "LIST");
  wrLE32(hdr + 16, 192);
  wrFourcc(hdr + 20, "hdrl");
  wrFourcc(hdr + 24, "avih");
  wrLE32(hdr + 28, 56);
  wrLE32(hdr + 32, 1000000 / RECORD_FPS);       // dwMicroSecPerFrame
  wrLE32(hdr + 36, 400000);                     // dwMaxBytesPerSec (估)
  wrLE32(hdr + 44, 0x10);                       // AVIF_HASINDEX
  wrLE32(hdr + 56, 1);                          // dwStreams
  wrLE32(hdr + 60, 32768);                      // dwSuggestedBufferSize
  wrLE32(hdr + 64, TFT_WIDTH);
  wrLE32(hdr + 68, TFT_HEIGHT);
  wrFourcc(hdr + 88, "LIST");
  wrLE32(hdr + 92, 116);
  wrFourcc(hdr + 96, "strl");
  wrFourcc(hdr + 100, "strh");
  wrLE32(hdr + 104, 56);
  wrFourcc(hdr + 108, "vids");
  wrFourcc(hdr + 112, "MJPG");
  wrLE32(hdr + 128, 1);                         // dwScale
  wrLE32(hdr + 132, RECORD_FPS);                // dwRate -> fps
  wrLE32(hdr + 144, 32768);                     // dwSuggestedBufferSize
  wrLE16(hdr + 160, TFT_WIDTH);                 // rcFrame.right
  wrLE16(hdr + 162, TFT_HEIGHT);                // rcFrame.bottom
  wrFourcc(hdr + 164, "strf");
  wrLE32(hdr + 168, 40);
  wrLE32(hdr + 172, 40);                        // biSize
  wrLE32(hdr + 176, TFT_WIDTH);
  wrLE32(hdr + 180, TFT_HEIGHT);
  wrLE16(hdr + 184, 1);                         // biPlanes
  wrLE16(hdr + 186, 24);                        // biBitCount
  wrFourcc(hdr + 188, "MJPG");                  // biCompression
  wrLE32(hdr + 192, TFT_WIDTH * TFT_HEIGHT * 3);
  wrFourcc(hdr + 212, "LIST");                  // moviSize @216 回填
  wrFourcc(hdr + 220, "movi");
  recFile.write(hdr, sizeof(hdr));
  xSemaphoreGive(sdMutex);

  recFrames = 0;
  recMoviBytes = 4; // 'movi' fourcc
  recStartMs = millis();
  recNextFrameMs = recStartMs;
  recording = true;
  Serial.println("開始錄影: " + recPath);
  return true;
}

// 追加一幀 '00dc' JPEG 區塊 (資料長度補齊為偶數)
void writeRecordingFrame(const uint8_t *jpg, size_t len)
{
  if (!recording || recFrames >= RECORD_MAX_FRAMES)
  {
    return;
  }
  uint8_t chunkHdr[8];
  wrFourcc(chunkHdr, "00dc");
  wrLE32(chunkHdr + 4, len);

  xSemaphoreTake(sdMutex, portMAX_DELAY);
  recFile.write(chunkHdr, 8);
  recFile.write(jpg, len);
  if (len & 1)
  {
    uint8_t pad = 0;
    recFile.write(&pad, 1);
  }
  xSemaphoreGive(sdMutex);

  recIdx[recFrames * 2] = recMoviBytes; // '00dc' 相對 'movi' fourcc 的位移
  recIdx[recFrames * 2 + 1] = (uint32_t)len;
  recMoviBytes += 8 + ((len + 1) & ~1UL);
  recFrames++;
}

// 附上 idx1 索引、回填幀數與各區塊大小後關檔
void stopRecording()
{
  if (!recording)
  {
    return;
  }
  recording = false;

  xSemaphoreTake(sdMutex, portMAX_DELAY);
  uint8_t entry[16];
  wrFourcc(entry, "idx1");
  wrLE32(entry + 4, recFrames * 16);
  recFile.write(entry, 8);
  for (uint32_t i = 0; i < recFrames; i++)
  {
    wrFourcc(entry, "00dc");
    wrLE32(entry + 4, 0x10); // AVIIF_KEYFRAME
    wrLE32(entry + 8, recIdx[i * 2]);
    wrLE32(entry + 12, recIdx[i * 2 + 1]);
    recFile.write(entry, 16);
  }
  uint32_t fileSize = recFile.position();
  uint8_t num[4];
  recFile.seek(4);
  wrLE32(num, fileSize - 8);
  recFile.write(num, 4);
  recFile.seek(48);
  wrLE32(num, recFrames);
  recFile.write(num, 4);
  recFile.seek(140);
  wrLE32(num, recFrames);
  recFile.write(num, 4);
  recFile.seek(216);
  wrLE32(num, recMoviBytes);
  recFile.write(num, 4);
  recFile.close();
  xSemaphoreGive(sdMutex);

  Serial.printf("錄影完成: %s，%u 幀，%.1f 秒\n",
                recPath.c_str(), recFrames, recFrames / (float)RECORD_FPS);
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

// ---------- 使用者登入保護 (HTTP Basic Auth) ----------

// 純檢查 Authorization 標頭，不送出任何回應 (供上傳中途安全呼叫)
bool isAuthed()
{
  xSemaphoreTake(authMutex, portMAX_DELAY);
  String u = authUser;
  String p = authPass;
  xSemaphoreGive(authMutex);
  if (u.length() == 0)
  {
    return true; // 未設定帳號密碼 = 不需登入
  }
  return server.authenticate(u.c_str(), p.c_str());
}

// 未通過驗證時送出 401 + Basic Auth 挑戰，觸發瀏覽器原生登入視窗
bool checkAuth()
{
  if (isAuthed())
  {
    return true;
  }
  server.requestAuthentication();
  return false;
}

void handleAuthConfigGet()
{
  if (!checkAuth()) return;
  xSemaphoreTake(authMutex, portMAX_DELAY);
  bool enabled = authUser.length() > 0;
  String u = authUser;
  xSemaphoreGive(authMutex);
  server.send(200, "application/json",
              "{\"enabled\":" + String(enabled ? "true" : "false") +
                  ",\"user\":\"" + jsonEscape(u) + "\"}");
}

// POST user=...&pass=...：user 空字串 = 停用登入保護；否則兩者皆須非空，
// 密碼至少 4 個字元
void handleAuthConfigSet()
{
  if (!checkAuth()) return;

  String u = server.arg("user");
  u.trim();
  String p = server.arg("pass");

  if (u.length() > 0 && p.length() < 4)
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"密碼至少需 4 個字元\"}");
    return;
  }

  xSemaphoreTake(authMutex, portMAX_DELAY);
  authUser = u;
  authPass = (u.length() > 0) ? p : "";
  xSemaphoreGive(authMutex);

  if (sdMode)
  {
    saveAppSettings();
  }
  else
  {
    authPrefs.begin(AUTH_PREF_NAMESPACE, false);
    authPrefs.putString("user", authUser);
    authPrefs.putString("pass", authPass);
    authPrefs.end();
  }

  Serial.println(u.length() > 0 ? "登入保護已啟用" : "登入保護已停用");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRoot()
{
  if (!checkAuth()) return;
  File f = SPIFFS.open("/index.html", "r");
  if (!f)
  {
    server.send(500, "text/plain", "index.html not found，請先上傳 SPIFFS data 資料夾");
    return;
  }
  server.streamFile(f, "text/html");
  f.close();
}

// 攔截所有未註冊的路徑 (server.onNotFound)。手機/電腦連上 esp32_init 熱點後
// 會發出各種 captive portal 偵測請求 (connectivitycheck.gstatic.com/generate_204、
// captive.apple.com/hotspot-detect.html、msftconnecttest.com/connecttest.txt 等)，
// 這些網域經 dnsServer 全部解析到 AP IP，request 進來時 Host 不是 AP IP 本身，
// 一律回 302 導回首頁，系統偵測到非預期回應即視為 captive portal 並自動彈出瀏覽器。
void handleCaptivePortal()
{
  String apIp = WiFi.softAPIP().toString();
  if (server.hostHeader() != apIp)
  {
    server.sendHeader("Location", "http://" + apIp + "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "Not Found");
}

void handleScan()
{
  if (!checkAuth()) return;
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
  if (!checkAuth()) return;
  if (!server.hasArg("ssid") || server.arg("ssid").length() == 0)
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid\"}");
    return;
  }

  String ssid = server.arg("ssid");
  String password = server.hasArg("password") ? server.arg("password") : "";

  if (sdMode)
  {
    sdWifiSsid = ssid;
    sdWifiPassword = password;
    saveAppSettings();
  }
  else
  {
    wifiPrefs.begin(WIFI_PREF_NAMESPACE, false);
    wifiPrefs.putString("ssid", ssid);
    wifiPrefs.putString("password", password);
    wifiPrefs.end();
  }

  WiFi.begin(ssid.c_str(), password.c_str());
  staConnecting = true;
  staConnectStart = millis();

  server.send(200, "application/json", "{\"ok\":true}");
}

void handleStatus()
{
  if (!checkAuth()) return;
  bool connected = WiFi.status() == WL_CONNECTED;
  String json = "{";
  json += "\"connected\":" + String(connected ? "true" : "false");
  json += ",\"connecting\":" + String(staConnecting ? "true" : "false");
  json += ",\"ip\":\"" + (connected ? WiFi.localIP().toString() : String("")) + "\"";
  json += ",\"ssid\":\"" + jsonEscape(connected ? WiFi.SSID() : String("")) + "\"";
  json += ",\"led\":" + String(ledState ? "true" : "false");
  json += ",\"camera\":" + String(cameraOk ? "true" : "false");
  json += ",\"sd\":" + String(sdMode ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleCapture()
{
  if (!checkAuth()) return;
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
  if (!checkAuth()) return;
  if (sdMode)
  {
    sdWifiSsid = "";
    sdWifiPassword = "";
    saveAppSettings();
  }
  else
  {
    wifiPrefs.begin(WIFI_PREF_NAMESPACE, false);
    wifiPrefs.clear();
    wifiPrefs.end();
  }

  staConnecting = false;
  // 只斷開 STA 並清除 WiFi 函式庫內部記住的 AP 資訊，softAP (esp32_init) 保持運作，
  // 使用者可繼續連著熱點重新掃描/設定 Wi-Fi
  WiFi.disconnect(false, true);
  Serial.println("已清除儲存的 Wi-Fi 設定並斷線");

  server.send(200, "application/json", "{\"ok\":true}");
}

// 設定偵測區域/靈敏度 (POST enabled=0|1&x&y&w&h&sens，座標為感光元件方向 240x240)
void handleMotionConfig()
{
  if (!checkAuth()) return;
  bool enabled = server.arg("enabled") == "1";
  int x = server.arg("x").toInt();
  int y = server.arg("y").toInt();
  int w = server.arg("w").toInt();
  int h = server.arg("h").toInt();
  int sens = server.arg("sens").toInt();

  x = constrain(x, 0, TFT_WIDTH - 1);
  y = constrain(y, 0, TFT_HEIGHT - 1);
  w = constrain(w, 0, TFT_WIDTH - x);
  h = constrain(h, 0, TFT_HEIGHT - y);
  if (sens < 1 || sens > 15) sens = 4;
  if (w < 8 || h < 8) enabled = false;

  xSemaphoreTake(motionCfgMutex, portMAX_DELAY);
  motionCfg = {enabled, x, y, w, h, sens};
  motionResetBaseline = true;
  xSemaphoreGive(motionCfgMutex);
  motionTriggeredUntilMs = 0;

  saveAppSettings(); // SD 模式：偵測框座標即時寫入 appsetting.json (重開機保留)

  Serial.printf("移動偵測設定: %s 區域(%d,%d %dx%d) 靈敏度=%d\n",
                enabled ? "啟用" : "停用", x, y, w, h, sens);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleMotionStatus()
{
  if (!checkAuth()) return;
  xSemaphoreTake(motionCfgMutex, portMAX_DELAY);
  MotionConfig cfg = motionCfg;
  xSemaphoreGive(motionCfgMutex);

  bool triggered = (int32_t)(motionTriggeredUntilMs - millis()) > 0;
  String json = "{";
  json += "\"enabled\":" + String(cfg.enabled ? "true" : "false");
  json += ",\"x\":" + String(cfg.x) + ",\"y\":" + String(cfg.y);
  json += ",\"w\":" + String(cfg.w) + ",\"h\":" + String(cfg.h);
  json += ",\"sens\":" + String(cfg.sensitivity);
  json += ",\"diff\":" + String(motionDiff, 1);
  json += ",\"triggered\":" + String(triggered ? "true" : "false");
  json += ",\"lastId\":" + String(alarmLastId);
  json += ",\"count\":" + String(alarmCount);
  json += ",\"recording\":" + String(recording ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

// 告警清單 (新到舊)：[{"id":N,"time":epoch}, ...]
void handleAlarmList()
{
  if (!checkAuth()) return;
  xSemaphoreTake(alarmMutex, portMAX_DELAY);
  String json = "[";
  for (int i = 0; i < alarmCount; i++)
  {
    int idx = ((alarmWrite - 1 - i) % alarmCap + alarmCap) % alarmCap;
    if (i > 0) json += ",";
    json += "{\"id\":" + String(alarmList[idx].id) +
            ",\"time\":" + String((uint32_t)alarmList[idx].epoch) + "}";
  }
  json += "]";
  xSemaphoreGive(alarmMutex);
  server.send(200, "application/json", json);
}

void handleAlarmImage()
{
  if (!checkAuth()) return;
  uint32_t id = (uint32_t)server.arg("id").toInt();

  if (sdMode)
  {
    // SD 模式：JPEG 在 SD 檔案，直接串流回瀏覽器
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    File f = SD_MMC.open(alarmJpgPath(id), FILE_READ);
    if (f)
    {
      server.sendHeader("Cache-Control", "max-age=3600"); // id 唯一，可放心快取
      server.streamFile(f, "image/jpeg");
      f.close();
      xSemaphoreGive(sdMutex);
      return;
    }
    xSemaphoreGive(sdMutex);
    server.send(404, "application/json", "{\"error\":\"alarm not found\"}");
    return;
  }

  xSemaphoreTake(alarmMutex, portMAX_DELAY);
  for (int i = 0; i < alarmCount; i++)
  {
    if (alarmList[i].id == id && alarmList[i].jpg)
    {
      // 送出期間持有鎖，避免環形緩衝覆蓋/清除已釋放的 JPEG
      server.sendHeader("Cache-Control", "max-age=3600"); // id 唯一，可放心快取
      server.send_P(200, "image/jpeg", (const char *)alarmList[i].jpg, alarmList[i].len);
      xSemaphoreGive(alarmMutex);
      return;
    }
  }
  xSemaphoreGive(alarmMutex);
  server.send(404, "application/json", "{\"error\":\"alarm not found\"}");
}

void handleAlarmClear()
{
  if (!checkAuth()) return;
  xSemaphoreTake(alarmMutex, portMAX_DELAY);
  for (int i = 0; i < alarmCap; i++)
  {
    if (sdMode && alarmList[i].id != 0)
    {
      xSemaphoreTake(sdMutex, portMAX_DELAY);
      SD_MMC.remove(alarmJpgPath(alarmList[i].id));
      xSemaphoreGive(sdMutex);
    }
    if (alarmList[i].jpg)
    {
      free(alarmList[i].jpg);
    }
    alarmList[i] = {};
  }
  alarmWrite = 0;
  alarmCount = 0;
  if (sdMode)
  {
    saveAlarmIndexLocked();
  }
  xSemaphoreGive(alarmMutex);
  server.send(200, "application/json", "{\"ok\":true}");
}

// ---------- 錄影回放 ----------

// 錄影檔清單 (檔名含日期時間，由網頁端解析排序)
void handleRecList()
{
  if (!checkAuth()) return;
  if (!sdMode)
  {
    server.send(200, "application/json", "[]");
    return;
  }
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  String json = "[";
  bool first = true;
  File dir = SD_MMC.open(REC_DIR);
  if (dir && dir.isDirectory())
  {
    File f;
    while ((f = dir.openNextFile()))
    {
      String name = f.name();
      if (!f.isDirectory() && name.endsWith(".avi"))
      {
        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"" + jsonEscape(name) + "\",\"size\":" + String((uint32_t)f.size()) + "}";
      }
      f.close();
    }
  }
  if (dir) dir.close();
  json += "]";
  xSemaphoreGive(sdMutex);
  server.send(200, "application/json", json);
}

// 檢查檔名參數，防止路徑跳脫
static bool validRecName(const String &name)
{
  return name.length() > 0 && name.indexOf("..") < 0 &&
         name.indexOf('/') < 0 && name.indexOf('\\') < 0 && name.endsWith(".avi");
}

// 串流錄影檔。分塊讀取、每塊之間釋放 sdMutex，避免長時間下載
// 卡死 core 0 的錄影寫入
void handleRecFile()
{
  if (!checkAuth()) return;
  String name = server.arg("name");
  if (!sdMode || !validRecName(name))
  {
    server.send(400, "application/json", "{\"error\":\"bad name\"}");
    return;
  }

  xSemaphoreTake(sdMutex, portMAX_DELAY);
  File f = SD_MMC.open(String(REC_DIR) + "/" + name, FILE_READ);
  size_t fsize = f ? f.size() : 0;
  xSemaphoreGive(sdMutex);
  if (!f)
  {
    server.send(404, "application/json", "{\"error\":\"not found\"}");
    return;
  }

  const size_t BUF_SIZE = 4096;
  uint8_t *buf = (uint8_t *)malloc(BUF_SIZE);
  if (!buf)
  {
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    f.close();
    xSemaphoreGive(sdMutex);
    server.send(500, "application/json", "{\"error\":\"no mem\"}");
    return;
  }

  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(fsize);
  server.send(200, "video/x-msvideo", "");
  WiFiClient client = server.client();
  size_t remaining = fsize;
  while (remaining > 0 && client.connected())
  {
    size_t want = remaining < BUF_SIZE ? remaining : BUF_SIZE;
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    int r = f.read(buf, want);
    xSemaphoreGive(sdMutex);
    if (r <= 0)
    {
      break;
    }
    client.write(buf, r);
    remaining -= r;
  }
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  f.close();
  xSemaphoreGive(sdMutex);
  free(buf);
}

void handleRecDelete()
{
  if (!checkAuth()) return;
  String name = server.arg("name");
  if (!sdMode || !validRecName(name))
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad name\"}");
    return;
  }
  if (recording)
  {
    // 錄影中的檔案還握在 core 0 手上，FATFS 刪除開啟中的檔案行為未定義
    server.send(409, "application/json",
                "{\"ok\":false,\"error\":\"錄影進行中，請稍後再刪除\"}");
    return;
  }
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  bool ok = SD_MMC.remove(String(REC_DIR) + "/" + name);
  xSemaphoreGive(sdMutex);
  server.send(200, "application/json",
              ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"刪除失敗\"}");
}

// 網頁載入時 POST epoch=秒&tz=時區分鐘 同步時間 (AP 模式沒網路拿不到 NTP)
void handleTimeSync()
{
  if (!checkAuth()) return;
  long long epoch = atoll(server.arg("epoch").c_str());
  if (epoch > 1000000000LL)
  {
    struct timeval tv = {(time_t)epoch, 0};
    settimeofday(&tv, NULL);
  }
  if (server.hasArg("tz"))
  {
    tzOffsetMin = server.arg("tz").toInt();
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// ---------- Discord / Telegram 推播設定 ----------

void handleNotifyConfigGet()
{
  if (!checkAuth()) return;
  xSemaphoreTake(notifyMutex, portMAX_DELAY);
  String url = discordWebhook;
  String tgToken = telegramBotToken;
  String tgChatId = telegramChatId;
  xSemaphoreGive(notifyMutex);
  server.send(200, "application/json",
              "{\"webhook\":\"" + jsonEscape(url) +
                  "\",\"tgToken\":\"" + jsonEscape(tgToken) +
                  "\",\"tgChatId\":\"" + jsonEscape(tgChatId) + "\"}");
}

// POST webhook=... 和/或 tgToken=...&tgChatId=...，只更新請求中出現的欄位，
// 未出現的欄位維持原值 (Discord 卡片與 Telegram 卡片各自獨立送出)。傳空字串 = 清除/停用該管道。
void handleNotifyConfigSet()
{
  if (!checkAuth()) return;
  bool hasWebhook = server.hasArg("webhook");
  bool hasTgToken = server.hasArg("tgToken");
  bool hasTgChatId = server.hasArg("tgChatId");

  String url = server.arg("webhook");
  url.trim();
  if (hasWebhook && url.length() > 0 && !url.startsWith("https://"))
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"Webhook 需為 https:// 開頭\"}");
    return;
  }

  String tgToken = server.arg("tgToken");
  tgToken.trim();
  if (hasTgToken && tgToken.length() > 0 && tgToken.indexOf(':') <= 0)
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"Bot Token 格式錯誤，應為數字:字母組合\"}");
    return;
  }

  String tgChatId = server.arg("tgChatId");
  tgChatId.trim();

  xSemaphoreTake(notifyMutex, portMAX_DELAY);
  if (hasWebhook) discordWebhook = url;
  if (hasTgToken) telegramBotToken = tgToken;
  if (hasTgChatId) telegramChatId = tgChatId;
  xSemaphoreGive(notifyMutex);

  if (sdMode)
  {
    saveAppSettings();
  }
  else
  {
    notifyPrefs.begin(NOTIFY_PREF_NAMESPACE, false);
    if (hasWebhook) notifyPrefs.putString("webhook", url);
    if (hasTgToken) notifyPrefs.putString("tgToken", tgToken);
    if (hasTgChatId) notifyPrefs.putString("tgChatId", tgChatId);
    notifyPrefs.end();
  }

  if (hasWebhook)
  {
    Serial.println(url.length() > 0 ? "Discord Webhook 已更新" : "Discord Webhook 已清除");
  }
  if (hasTgToken || hasTgChatId)
  {
    Serial.println("Telegram 設定已更新");
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// 排入一則測試訊息 (不帶圖片，只送 Discord)，實際結果看序列埠與 Discord 頻道
void handleNotifyTest()
{
  if (!checkAuth()) return;
  xSemaphoreTake(notifyMutex, portMAX_DELAY);
  bool hasWebhook = discordWebhook.length() > 0;
  xSemaphoreGive(notifyMutex);

  if (!hasWebhook)
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"尚未設定 Webhook URL\"}");
    return;
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"STA 未連線，請先連上可上網的 Wi-Fi\"}");
    return;
  }

  NotifyJob job{0, true, false};
  xQueueSend(notifyQueue, &job, 0);
  server.send(200, "application/json", "{\"ok\":true}");
}

// 排入一則測試訊息 (不帶圖片，只送 Telegram)，實際結果看序列埠與 Telegram 對話
void handleNotifyTelegramTest()
{
  if (!checkAuth()) return;
  xSemaphoreTake(notifyMutex, portMAX_DELAY);
  bool hasTelegram = telegramBotToken.length() > 0 && telegramChatId.length() > 0;
  xSemaphoreGive(notifyMutex);

  if (!hasTelegram)
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"尚未設定 Bot Token 與 Chat ID\"}");
    return;
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"STA 未連線，請先連上可上網的 Wi-Fi\"}");
    return;
  }

  NotifyJob job{0, false, true};
  xQueueSend(notifyQueue, &job, 0);
  server.send(200, "application/json", "{\"ok\":true}");
}

// 上傳結束後回覆結果；成功則重新啟動套用新韌體/新網頁資料
void handleUpdateFinished()
{
  if (!checkAuth()) return;
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
    // 未通過驗證：不呼叫 Update.begin，略過整段上傳寫入。此時回應尚未送出，
    // 不能在這裡呼叫 requestAuthentication (會打斷仍在傳送中的上傳)，
    // 留待上傳結束後 handleUpdateFinished 的 checkAuth() 一併回覆 401
    otaAuthOk = isAuthed();
    if (!otaAuthOk)
    {
      return;
    }

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
    if (!otaAuthOk) return;
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
    {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (!otaAuthOk) return;
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
    if (!otaAuthOk) return;
    Update.abort();
    otaInProgress = false;
    Serial.println("OTA 已中斷");
  }
}

void handleLed()
{
  if (!checkAuth()) return;
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
  String ssid, password;
  if (sdMode)
  {
    ssid = sdWifiSsid;
    password = sdWifiPassword;
  }
  else
  {
    wifiPrefs.begin(WIFI_PREF_NAMESPACE, true);
    ssid = wifiPrefs.getString("ssid", "");
    password = wifiPrefs.getString("password", "");
    wifiPrefs.end();
  }

  if (ssid.length() > 0)
  {
    Serial.print("嘗試自動連線已儲存的 Wi-Fi: ");
    Serial.println(ssid);
    WiFi.begin(ssid.c_str(), password.c_str());
    staConnecting = true;
    staConnectStart = millis();
  }
}
