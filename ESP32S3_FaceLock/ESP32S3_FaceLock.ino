// ============================================================
// BLYNK
// ============================================================
#define BLYNK_TEMPLATE_ID "TMPL6RF1VcCxl"
#define BLYNK_TEMPLATE_NAME "Smkey"
#define BLYNK_AUTH_TOKEN "YOUR_BLYNK_AUTH_TOKEN"

// ============================================================
// LIBRARY
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Preferences.h>
#include <time.h>
#include "esp_camera.h"

// ============================================================
// WIFI
// ============================================================
const char *ssid = "YOUR_WIFI_NAME";
const char *password = "YOUR_WIFI_PASSWORD";

// ============================================================
// ESP32-S3-WROOM CAM + OV5640
// ============================================================
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1

#define XCLK_GPIO_NUM   15
#define SIOD_GPIO_NUM    4
#define SIOC_GPIO_NUM    5

#define Y2_GPIO_NUM     11
#define Y3_GPIO_NUM      9
#define Y4_GPIO_NUM      8
#define Y5_GPIO_NUM     10
#define Y6_GPIO_NUM     12
#define Y7_GPIO_NUM     18
#define Y8_GPIO_NUM     17
#define Y9_GPIO_NUM     16

#define VSYNC_GPIO_NUM   6
#define HREF_GPIO_NUM    7
#define PCLK_GPIO_NUM   13

// ============================================================
// LOCK
// ============================================================
#define RELAY_PIN          1
#define BOOT_PIN           0

#define RELAY_ON_LEVEL     HIGH
#define RELAY_OFF_LEVEL    LOW

#define UNLOCK_TIME_MS     3000
#define AUTH_TARGET_MS     1000UL
#define ALERT_TARGET_MS    5000UL

#define MAX_RFID_USERS              120
#define ACCESS_LOG_CAPACITY         100
#define STRANGER_ALERT_COOLDOWN_MS  30000UL
#define BLYNK_EVENT_STRANGER        "stranger_alert"


#define STM32_RX_PIN 44
#define STM32_TX_PIN 43

HardwareSerial STM32Serial(1);


void startCameraServer();

bool recognizeFaceOnce();
uint32_t getLastFaceAuthTimeMs();
int getLastRecognizedFaceID();
float getLastFaceSimilarity();

int getFaceCount();
bool deleteFaceByID(int id);
bool deleteAllFaceIDs();


bool doorUnlocked = false;
unsigned long unlockStartTime = 0;

// Face
int faceIDToDelete = -1;

// RFID
int rfidIDToDelete = -1;

// UART line buffer
String stm32Line = "";

// Blynk reconnect
unsigned long lastBlynkReconnect = 0;

typedef struct
{
  uint32_t epoch;
  uint32_t authTimeMs;
  int16_t userID;
  uint8_t granted;
  char method[8];
} AccessLogEntry;

Preferences accessLogStorage;
AccessLogEntry accessLogs[ACCESS_LOG_CAPACITY];
uint16_t accessLogHead = 0;
uint16_t accessLogCount = 0;
bool accessLogStorageReady = false;

int registeredRFIDCount = 0;

portMUX_TYPE unknownFaceMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool unknownFacePending = false;
volatile uint32_t unknownFaceDetectedAtMs = 0;
volatile uint32_t unknownFaceAuthTimeMs = 0;
volatile int32_t unknownFaceSimilarityMilli = 0;

portMUX_TYPE faceAccessMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool faceAccessPending = false;
volatile int faceAccessID = -1;
volatile uint32_t faceAccessAuthTimeMs = 0;
volatile int32_t faceAccessSimilarityMilli = 0;

bool strangerAlertSent = false;
unsigned long lastStrangerAlertMs = 0;


String formatTimestamp(uint32_t epoch)
{
  if (epoch < 1700000000UL)
  {
    return "Chua dong bo thoi gian";
  }

  time_t rawTime = (time_t)epoch;
  struct tm timeInfo;

  localtime_r(&rawTime, &timeInfo);

  char text[24];

  strftime(
    text,
    sizeof(text),
    "%d/%m/%Y %H:%M:%S",
    &timeInfo
  );

  return String(text);
}


String formatAccessLog(const AccessLogEntry &entry)
{
  String text =
    formatTimestamp(entry.epoch) +
    " | " +
    String(entry.method);

  if (entry.userID >= 0)
  {
    text += " ID " + String(entry.userID);
  }

  text += (entry.granted ? " | CHO PHEP" : " | TU CHOI");
  text += " | " + String(entry.authTimeMs) + " ms";

  return text;
}


void publishLatestAccessLog()
{
  if (
    accessLogCount == 0 ||
    !Blynk.connected()
  )
  {
    return;
  }

  uint16_t latestIndex =
    (accessLogHead + ACCESS_LOG_CAPACITY - 1) %
    ACCESS_LOG_CAPACITY;

  Blynk.virtualWrite(
    V9,
    formatAccessLog(accessLogs[latestIndex])
  );
}


void initAccessLogStorage()
{
  memset(
    accessLogs,
    0,
    sizeof(accessLogs)
  );

  accessLogStorageReady =
    accessLogStorage.begin(
      "access-log",
      false
    );

  if (!accessLogStorageReady)
  {
    Serial.println("[LOG] NVS initialization failed");
    return;
  }

  size_t savedSize =
    accessLogStorage.getBytesLength("entries");

  if (savedSize == sizeof(accessLogs))
  {
    accessLogStorage.getBytes(
      "entries",
      accessLogs,
      sizeof(accessLogs)
    );
  }

  accessLogHead =
    accessLogStorage.getUShort(
      "head",
      0
    ) % ACCESS_LOG_CAPACITY;

  accessLogCount =
    accessLogStorage.getUShort(
      "count",
      0
    );

  if (accessLogCount > ACCESS_LOG_CAPACITY)
  {
    accessLogCount = ACCESS_LOG_CAPACITY;
  }

  Serial.print("[LOG] Loaded records = ");
  Serial.println(accessLogCount);
}


void saveAccessLog(
  const char *method,
  int userID,
  bool granted,
  uint32_t authTimeMs
)
{
  AccessLogEntry &entry =
    accessLogs[accessLogHead];

  memset(
    &entry,
    0,
    sizeof(entry)
  );

  time_t now = time(nullptr);

  if (now >= 1700000000)
  {
    entry.epoch = (uint32_t)now;
  }

  entry.authTimeMs = authTimeMs;
  entry.userID = (int16_t)userID;
  entry.granted = granted ? 1 : 0;

  strncpy(
    entry.method,
    method,
    sizeof(entry.method) - 1
  );

  accessLogHead =
    (accessLogHead + 1) %
    ACCESS_LOG_CAPACITY;

  if (accessLogCount < ACCESS_LOG_CAPACITY)
  {
    accessLogCount++;
  }

  if (accessLogStorageReady)
  {
    accessLogStorage.putBytes(
      "entries",
      accessLogs,
      sizeof(accessLogs)
    );

    accessLogStorage.putUShort(
      "head",
      accessLogHead
    );

    accessLogStorage.putUShort(
      "count",
      accessLogCount
    );
  }

  String text = formatAccessLog(entry);

  Serial.print("[LOG] ");
  Serial.println(text);

  if (Blynk.connected())
  {
    Blynk.virtualWrite(V9, text);
    Blynk.virtualWrite(V10, authTimeMs);
  }
}


void initSystemTime()
{
  configTime(
    7 * 3600,
    0,
    "pool.ntp.org",
    "time.google.com"
  );

  struct tm timeInfo;

  if (getLocalTime(&timeInfo, 3000))
  {
    Serial.println("[TIME] NTP synchronized");
  }
  else
  {
    Serial.println("[TIME] NTP is not ready; synchronization will continue in background");
  }
}


void recordFaceAccess(
  int faceID,
  uint32_t authTimeMs,
  float similarity
)
{
  portENTER_CRITICAL(&faceAccessMux);

  if (!faceAccessPending)
  {
    faceAccessID = faceID;
    faceAccessAuthTimeMs = authTimeMs;
    faceAccessSimilarityMilli =
      (int32_t)(similarity * 1000.0f);
    faceAccessPending = true;
  }

  portEXIT_CRITICAL(&faceAccessMux);
}


void processFaceAccessLog()
{
  int faceID = -1;
  uint32_t authTimeMs = 0;
  int32_t similarityMilli = 0;
  bool hasPendingAccess = false;

  portENTER_CRITICAL(&faceAccessMux);

  if (faceAccessPending)
  {
    faceID = faceAccessID;
    authTimeMs = faceAccessAuthTimeMs;
    similarityMilli = faceAccessSimilarityMilli;
    faceAccessPending = false;
    hasPendingAccess = true;
  }

  portEXIT_CRITICAL(&faceAccessMux);

  if (!hasPendingAccess)
  {
    return;
  }

  Serial.printf(
    "[AUTH] FACE ID=%d time=%lu ms similarity=%.3f target=%s\n",
    faceID,
    (unsigned long)authTimeMs,
    similarityMilli / 1000.0f,
    authTimeMs < AUTH_TARGET_MS ? "PASS" : "FAIL"
  );

  saveAccessLog(
    "FACE",
    faceID,
    true,
    authTimeMs
  );
}


void reportUnknownFace(
  float similarity,
  uint32_t authTimeMs
)
{
  portENTER_CRITICAL(&unknownFaceMux);

  if (!unknownFacePending)
  {
    unknownFaceDetectedAtMs = millis();
    unknownFaceAuthTimeMs = authTimeMs;
    unknownFaceSimilarityMilli =
      (int32_t)(similarity * 1000.0f);
    unknownFacePending = true;
  }

  portEXIT_CRITICAL(&unknownFaceMux);
}


void processUnknownFaceAlert()
{
  uint32_t detectedAtMs = 0;
  uint32_t authTimeMs = 0;
  int32_t similarityMilli = 0;
  bool hasPendingAlert = false;

  portENTER_CRITICAL(&unknownFaceMux);

  if (unknownFacePending)
  {
    detectedAtMs = unknownFaceDetectedAtMs;
    authTimeMs = unknownFaceAuthTimeMs;
    similarityMilli = unknownFaceSimilarityMilli;
    unknownFacePending = false;
    hasPendingAlert = true;
  }

  portEXIT_CRITICAL(&unknownFaceMux);

  if (!hasPendingAlert)
  {
    return;
  }

  unsigned long nowMs = millis();

  if (
    strangerAlertSent &&
    nowMs - lastStrangerAlertMs < STRANGER_ALERT_COOLDOWN_MS
  )
  {
    return;
  }

  strangerAlertSent = true;
  lastStrangerAlertMs = nowMs;

  saveAccessLog(
    "FACE",
    -1,
    false,
    authTimeMs
  );

  if (
    !Blynk.connected() &&
    WiFi.status() == WL_CONNECTED
  )
  {
    Blynk.connect(2000);
  }

  uint32_t alertTimeMs =
    millis() - detectedAtMs;

  String message =
    "CANH BAO: Phat hien nguoi la luc " +
    formatTimestamp((uint32_t)time(nullptr));

  if (Blynk.connected())
  {
    Blynk.logEvent(
      BLYNK_EVENT_STRANGER,
      message
    );

    Blynk.virtualWrite(V8, message);
    Blynk.virtualWrite(V12, alertTimeMs);
  }

  Serial.printf(
    "[ALERT] Stranger similarity=%.3f dispatch=%lu ms target=%s blynk=%s\n",
    similarityMilli / 1000.0f,
    (unsigned long)alertTimeMs,
    alertTimeMs < ALERT_TARGET_MS ? "PASS" : "FAIL",
    Blynk.connected() ? "SENT" : "OFFLINE"
  );
}


void unlockDoor()
{
  Serial.println();
  Serial.println("==============================");
  Serial.println("[LOCK] OPEN");
  Serial.println("==============================");

  digitalWrite(
    RELAY_PIN,
    RELAY_ON_LEVEL
  );

  doorUnlocked = true;
  unlockStartTime = millis();
}

void updateDoorLock()
{
  if (!doorUnlocked)
  {
    return;
  }

  if (millis() - unlockStartTime >= UNLOCK_TIME_MS)
  {
    digitalWrite(
      RELAY_PIN,
      RELAY_OFF_LEVEL
    );

    doorUnlocked = false;

    Serial.println("[LOCK] CLOSED");
  }
}


void processSTM32Line(String line)
{
  line.trim();

  if (line.length() == 0)
  {
    return;
  }

  Serial.print("[STM32] ");
  Serial.println(line);

 
  if (line.startsWith("RFID_OK:"))
  {
    int secondColon =
      line.indexOf(':', 8);

    if (secondColon < 0)
    {
      Serial.println("[RFID] Invalid RFID_OK message");
      return;
    }

    int cardID =
      line.substring(8, secondColon).toInt();

    uint32_t authTimeMs =
      (uint32_t)line.substring(secondColon + 1).toInt();

    Serial.printf(
      "[AUTH] RFID ID=%d time=%lu ms target=%s\n",
      cardID,
      (unsigned long)authTimeMs,
      authTimeMs < AUTH_TARGET_MS ? "PASS" : "FAIL"
    );

    unlockDoor();

    saveAccessLog(
      "RFID",
      cardID,
      true,
      authTimeMs
    );

    if (Blynk.connected())
    {
      Blynk.virtualWrite(
        V8,
        "Mo khoa bang RFID ID " + String(cardID)
      );
    }
  }


  else if (line.startsWith("RFID_DENIED:"))
  {
    uint32_t authTimeMs =
      (uint32_t)line.substring(12).toInt();

    Serial.printf(
      "[AUTH] RFID DENIED time=%lu ms target=%s\n",
      (unsigned long)authTimeMs,
      authTimeMs < AUTH_TARGET_MS ? "PASS" : "FAIL"
    );

    saveAccessLog(
      "RFID",
      -1,
      false,
      authTimeMs
    );

    if (Blynk.connected())
    {
      Blynk.virtualWrite(
        V8,
        "The RFID chua duoc cap quyen"
      );
    }
  }


  else if (line.startsWith("COUNT:"))
  {
    registeredRFIDCount =
      line.substring(6).toInt();

    if (registeredRFIDCount < 0)
    {
      registeredRFIDCount = 0;
    }

    if (registeredRFIDCount > MAX_RFID_USERS)
    {
      registeredRFIDCount = MAX_RFID_USERS;
    }

    Serial.print("[RFID] Registered users = ");
    Serial.println(registeredRFIDCount);

    if (Blynk.connected())
    {
      Blynk.virtualWrite(
        V11,
        registeredRFIDCount
      );
    }
  }


  else if (line.startsWith("ADD_OK:"))
  {
    int newID = line.substring(7).toInt();

    Serial.print("[RFID] ADD SUCCESS ID = ");
    Serial.println(newID);

    Blynk.virtualWrite(
      V7,
      newID
    );

    String text =
      "Da them the RFID ID " +
      String(newID);

    Blynk.virtualWrite(
      V8,
      text
    );
  }


  else if (line.startsWith("EXISTS:"))
  {
    int cardID = line.substring(7).toInt();

    Serial.print("[RFID] CARD EXISTS ID = ");
    Serial.println(cardID);

    Blynk.virtualWrite(
      V7,
      cardID
    );

    String text =
      "The da ton tai ID " +
      String(cardID);

    Blynk.virtualWrite(
      V8,
      text
    );
  }

  else if (line.startsWith("DEL_OK:"))
  {
    int deleteID = line.substring(7).toInt();

    Serial.print("[RFID] DELETE SUCCESS ID = ");
    Serial.println(deleteID);

    Blynk.virtualWrite(
      V7,
      deleteID
    );

    String text =
      "Da xoa RFID ID " +
      String(deleteID);

    Blynk.virtualWrite(
      V8,
      text
    );
  }

  else if (line == "ID_EMPTY")
  {
    Serial.println("[RFID] ID DOES NOT EXIST");

    Blynk.virtualWrite(
      V8,
      "ID nay khong ton tai"
    );
  }

  else if (line == "FULL")
  {
    Serial.println("[RFID] DATABASE FULL");

    Blynk.virtualWrite(
      V8,
      "Bo nho RFID da day"
    );
  }
}


void checkSTM32UART()
{
  while (STM32Serial.available() > 0)
  {
    char c = STM32Serial.read();


    if (c == 'a' && stm32Line.length() == 0)
    {
      uint32_t authStartMs = millis();

      Serial.println();
      Serial.println("[RFID] VALID CARD");
      Serial.println("[RFID] OPEN DOOR");

      unlockDoor();

      saveAccessLog(
        "RFID",
        -1,
        true,
        millis() - authStartMs
      );

      continue;
    }

  
    if (c == '\n')
    {
      processSTM32Line(
        stm32Line
      );

      stm32Line = "";
    }

    else if (c != '\r')
    {
      stm32Line += c;

      // Chống buffer quá dài
      if (stm32Line.length() > 50)
      {
        stm32Line = "";
      }
    }
  }
}


BLYNK_WRITE(V0)
{
  int value = param.asInt();

  if (value == 1)
  {
    uint32_t authStartMs = millis();

    Serial.println();
    Serial.println("[BLYNK] OPEN DOOR");

    unlockDoor();

    saveAccessLog(
      "APP",
      -1,
      true,
      millis() - authStartMs
    );

    Blynk.virtualWrite(
      V0,
      0
    );
  }
}


BLYNK_WRITE(V1)
{
  faceIDToDelete = param.asInt();

  Serial.print("[BLYNK] Selected Face ID = ");
  Serial.println(faceIDToDelete);
}


BLYNK_WRITE(V2)
{
  int value = param.asInt();

  if (value != 1)
  {
    return;
  }

  Blynk.virtualWrite(
    V2,
    0
  );

  Serial.println();
  Serial.println("==============================");
  Serial.println("[BLYNK] DELETE ONE FACE");
  Serial.println("==============================");

  if (faceIDToDelete < 0)
  {
    Serial.println("[FACE] Invalid Face ID");

    return;
  }

  Serial.print("[FACE] Current face count = ");
  Serial.println(getFaceCount());

  Serial.print("[FACE] Delete ID = ");
  Serial.println(faceIDToDelete);

  bool result =
    deleteFaceByID(faceIDToDelete);

  if (result)
  {
    Serial.println("[FACE] DELETE SUCCESS");

    Serial.print("[FACE] Remaining faces = ");
    Serial.println(getFaceCount());
  }
  else
  {
    Serial.println("[FACE] DELETE FAILED");
  }
}


BLYNK_WRITE(V3)
{
  int value = param.asInt();

  if (value != 1)
  {
    return;
  }

  Blynk.virtualWrite(
    V3,
    0
  );

  Serial.println();
  Serial.println("==============================");
  Serial.println("[BLYNK] DELETE ALL FACES");
  Serial.println("==============================");

  int count =
    getFaceCount();

  Serial.print("[FACE] Current face count = ");
  Serial.println(count);

  if (count <= 0)
  {
    Serial.println("[FACE] Database already empty");

    return;
  }

  bool result =
    deleteAllFaceIDs();

  if (result)
  {
    Serial.println("[FACE] ALL FACES DELETED");

    Serial.print("[FACE] Remaining faces = ");
    Serial.println(getFaceCount());
  }
  else
  {
    Serial.println("[FACE] DELETE ALL FAILED");
  }
}


BLYNK_WRITE(V4)
{
  int value = param.asInt();

  if (value != 1)
  {
    return;
  }

  Serial.println();
  Serial.println("==============================");
  Serial.println("[BLYNK] ADD RFID");
  Serial.println("==============================");

  // Gửi lệnh ADD xuống STM32
  STM32Serial.write('A');

  Serial.println("[UART] TX -> A");

  Blynk.virtualWrite(
    V8,
    "Hay quet the RFID moi"
  );

  // Reset button
  Blynk.virtualWrite(
    V4,
    0
  );
}


BLYNK_WRITE(V5)
{
  rfidIDToDelete =
    param.asInt();

  Serial.print("[BLYNK] Selected RFID ID = ");
  Serial.println(rfidIDToDelete);
}


BLYNK_WRITE(V6)
{
  int value = param.asInt();

  if (value != 1)
  {
    return;
  }

  Blynk.virtualWrite(
    V6,
    0
  );

  Serial.println();
  Serial.println("==============================");
  Serial.println("[BLYNK] DELETE RFID");
  Serial.println("==============================");

  // STM32 quản lý tối đa 120 thẻ, ID từ 0 đến 119
  if (
    rfidIDToDelete < 0 ||
    rfidIDToDelete >= MAX_RFID_USERS
  )
  {
    Serial.println("[RFID] INVALID ID");

    Blynk.virtualWrite(
      V8,
      "RFID ID khong hop le"
    );

    return;
  }

  Serial.print("[RFID] Delete ID = ");
  Serial.println(rfidIDToDelete);



  STM32Serial.write('D');

  STM32Serial.write(
    (uint8_t)rfidIDToDelete
  );

  Serial.print("[UART] TX -> D, ID = ");
  Serial.println(rfidIDToDelete);

  Blynk.virtualWrite(
    V8,
    "Dang xoa RFID ID " +
    String(rfidIDToDelete)
  );
}


BLYNK_CONNECTED()
{
  Serial.println("[BLYNK] Connected to server");

  // Face + lock
  Blynk.virtualWrite(V0, 0);
  Blynk.virtualWrite(V2, 0);
  Blynk.virtualWrite(V3, 0);

  // RFID
  Blynk.virtualWrite(V4, 0);
  Blynk.virtualWrite(V6, 0);

  // Monitoring and access log
  Blynk.virtualWrite(V10, 0);
  Blynk.virtualWrite(V11, registeredRFIDCount);
  Blynk.virtualWrite(V12, 0);
  publishLatestAccessLog();
}


bool initCamera()
{
  camera_config_t config;

  config.ledc_channel =
    LEDC_CHANNEL_0;

  config.ledc_timer =
    LEDC_TIMER_0;

  // Camera data
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;

  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  // Camera clock
  config.pin_xclk =
    XCLK_GPIO_NUM;

  config.pin_pclk =
    PCLK_GPIO_NUM;

  config.pin_vsync =
    VSYNC_GPIO_NUM;

  config.pin_href =
    HREF_GPIO_NUM;

  // Arduino ESP32 Core 2.0.5
  config.pin_sscb_sda =
    SIOD_GPIO_NUM;

  config.pin_sscb_scl =
    SIOC_GPIO_NUM;

  config.pin_pwdn =
    PWDN_GPIO_NUM;

  config.pin_reset =
    RESET_GPIO_NUM;

  config.xclk_freq_hz =
    20000000;

  config.pixel_format =
    PIXFORMAT_JPEG;

  // Face recognition
  config.frame_size =
    FRAMESIZE_QVGA;

  config.jpeg_quality =
    15;

  config.fb_count =
    2;

  config.grab_mode =
    CAMERA_GRAB_LATEST;

  config.fb_location =
    CAMERA_FB_IN_PSRAM;

  // ========================================================
  // CHECK PSRAM
  // ========================================================
  if (!psramFound())
  {
    Serial.println("[ERROR] PSRAM NOT FOUND");

    return false;
  }

  Serial.println("[PSRAM] FOUND");

  // ========================================================
  // CAMERA INIT
  // ========================================================
  esp_err_t err =
    esp_camera_init(&config);

  if (err != ESP_OK)
  {
    Serial.printf(
      "[ERROR] Camera init failed: 0x%x\n",
      err
    );

    return false;
  }

  sensor_t *s =
    esp_camera_sensor_get();

  if (s)
  {
    // OV5640
    s->set_vflip(
      s,
      1
    );

    s->set_framesize(
      s,
      FRAMESIZE_QVGA
    );
  }

  Serial.println("[CAM] OV5640 READY");
  Serial.println("[CAM] Resolution = 320x240");

  return true;
}


void connectWiFi()
{
  WiFi.mode(
    WIFI_STA
  );

  WiFi.setSleep(
    false
  );

  WiFi.begin(
    ssid,
    password
  );

  Serial.print("[WIFI] Connecting");

  while (
    WiFi.status() != WL_CONNECTED
  )
  {
    delay(500);

    Serial.print(".");
  }

  Serial.println();

  Serial.println("[WIFI] CONNECTED");

  Serial.print("[WIFI] IP: ");
  Serial.println(WiFi.localIP());

  Serial.print("[WEB] http://");
  Serial.println(WiFi.localIP());
}


void connectBlynk()
{
  Serial.println();
  Serial.println("[BLYNK] Connecting...");

  Blynk.config(
    BLYNK_AUTH_TOKEN
  );

  bool connected =
    Blynk.connect(5000);

  if (connected)
  {
    Serial.println("[BLYNK] CONNECTED");
  }
  else
  {
    Serial.println("[BLYNK] NOT CONNECTED");

    Serial.println(
      "[INFO] Camera + RFID + Face Recognition still work"
    );
  }
}


void updateBlynkConnection()
{
  if (
    WiFi.status() != WL_CONNECTED
  )
  {
    return;
  }

  if (
    Blynk.connected()
  )
  {
    return;
  }

  if (
    millis() - lastBlynkReconnect < 10000
  )
  {
    return;
  }

  lastBlynkReconnect =
    millis();

  Serial.println(
    "[BLYNK] Reconnecting..."
  );

  Blynk.connect(
    2000
  );
}


void setup()
{

  Serial.begin(
    115200
  );

  Serial.setDebugOutput(
    true
  );

  delay(1000);

  Serial.println();
  Serial.println(
    "========================================"
  );

  initAccessLogStorage();

  Serial.println(
    " ESP32-S3 FACE + RFID SMART LOCK "
  );

  Serial.println(
    "========================================"
  );

 
  pinMode(
    RELAY_PIN,
    OUTPUT
  );

  digitalWrite(
    RELAY_PIN,
    RELAY_OFF_LEVEL
  );

 
  pinMode(
    BOOT_PIN,
    INPUT_PULLUP
  );


  STM32Serial.begin(
    115200,
    SERIAL_8N1,
    STM32_RX_PIN,
    STM32_TX_PIN
  );

  Serial.println(
    "[UART] STM32 UART READY"
  );

  Serial.println(
    "[UART] RX = GPIO44"
  );

  Serial.println(
    "[UART] TX = GPIO43"
  );

  Serial.println(
    "[UART] Baud = 115200"
  );


  if (!initCamera())
  {
    Serial.println(
      "[SYSTEM] CAMERA ERROR"
    );

    while (true)
    {
      delay(1000);
    }
  }


  connectWiFi();


  initSystemTime();


  connectBlynk();


  startCameraServer();


  Serial.println();
  Serial.println(
    "========================================"
  );

  Serial.println("[SYSTEM] READY");

  Serial.println();
  Serial.println(
    "RFID STM32 'a' = OPEN DOOR"
  );

  Serial.println(
    "BOOT = FACE RECOGNITION"
  );

  Serial.println();
  Serial.println(
    "BLYNK V0 = OPEN DOOR"
  );

  Serial.println(
    "BLYNK V1 = SELECT FACE ID"
  );

  Serial.println(
    "BLYNK V2 = DELETE FACE"
  );

  Serial.println(
    "BLYNK V3 = DELETE ALL FACES"
  );

  Serial.println();
  Serial.println(
    "BLYNK V4 = ADD RFID"
  );

  Serial.println(
    "BLYNK V5 = SELECT RFID ID"
  );

  Serial.println(
    "BLYNK V6 = DELETE RFID ID"
  );

  Serial.println(
    "BLYNK V7 = RFID ID DISPLAY"
  );

  Serial.println(
    "BLYNK V8 = RFID STATUS"
  );

  Serial.println(
    "BLYNK V9 = LATEST ACCESS LOG"
  );

  Serial.println(
    "BLYNK V10 = AUTH TIME (MS)"
  );

  Serial.println(
    "BLYNK V11 = REGISTERED RFID USERS"
  );

  Serial.println(
    "BLYNK V12 = ALERT DISPATCH TIME (MS)"
  );

  Serial.println(
    "========================================"
  );
}


void loop()
{
 
  Blynk.run();

  updateBlynkConnection();

  processUnknownFaceAlert();

  processFaceAccessLog();

 
  updateDoorLock();


  checkSTM32UART();


  static bool oldBoot =
    HIGH;

  bool boot =
    digitalRead(BOOT_PIN);

  if (
    oldBoot == HIGH &&
    boot == LOW
  )
  {
    delay(30);

    if (
      digitalRead(BOOT_PIN) == LOW
    )
    {
      Serial.println();

      Serial.println(
        "================================"
      );

      Serial.println(
        "[BOOT] FACE RECOGNITION"
      );

      Serial.println(
        "================================"
      );

      bool matched =
        recognizeFaceOnce();

      if (matched)
      {
        uint32_t authTimeMs =
          getLastFaceAuthTimeMs();

        int faceID =
          getLastRecognizedFaceID();

        Serial.println(
          "[FACE] ACCESS GRANTED"
        );

        unlockDoor();

        recordFaceAccess(
          faceID,
          authTimeMs,
          getLastFaceSimilarity()
        );
      }

      else
      {
        Serial.println(
          "[FACE] ACCESS DENIED"
        );
      }

      // Chờ thả BOOT
      while (
        digitalRead(BOOT_PIN) == LOW
      )
      {
        Blynk.run();

        updateDoorLock();

        checkSTM32UART();

        delay(10);
      }
    }
  }

  oldBoot =
    digitalRead(BOOT_PIN);

  delay(5);
}
