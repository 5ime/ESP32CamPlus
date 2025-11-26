#include "esp_camera.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_http_server.h>

#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

// ================== WiFi 配置 ==================
constexpr char WIFI_SSID[] = "iami233";
constexpr char WIFI_PASS[] = "12345678";
constexpr uint32_t WIFI_CHECK_INTERVAL = 5000;
constexpr uint32_t WIFI_MAX_FAIL_MS = 20000;

// ================== Flash LED 配置 ==================
constexpr bool USE_FLASH = true;
constexpr gpio_num_t FLASH_PIN = GPIO_NUM_4;
constexpr uint8_t FLASH_CHANNEL = 0;
constexpr uint16_t FLASH_FREQ = 5000;
constexpr uint8_t FLASH_RESOLUTION = 8;

// ================== 全局状态 ==================
uint32_t lastWifiCheck = 0;
uint32_t wifiFailStart = 0;
bool flashPwmReady = false;
uint8_t flashDuty = 0;

// ================== 外部声明（由 app_httpd.cpp 提供） ==================
extern httpd_handle_t stream_httpd;
extern httpd_handle_t camera_httpd;
extern void startCameraServer();

// 供 app_httpd.cpp 中闪光灯接口调用
extern "C" void app_set_flash_duty(uint8_t duty);
extern "C" uint8_t app_get_flash_duty();

// ================== 函数声明 ==================
bool initCamera();
bool initWiFi();
void ensureWiFi();
void initFlashLED(bool enable);
void setFlashDuty(uint8_t duty);
uint8_t getFlashDuty();

// ================== Setup ==================
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.println("\n=== ESP32-CAM 启动 ===");

  if (!initCamera()) {
    Serial.println("[致命] 摄像头初始化失败，准备重启...");
    delay(1000);
    ESP.restart();
  }

  if (USE_FLASH) initFlashLED(true);

  if (!initWiFi()) {
    Serial.println("[致命] WiFi 初始化失败，准备重启...");
    delay(1000);
    ESP.restart();
  }

  startCameraServer();
  Serial.printf("摄像头流已就绪！访问：http://%s:81/stream\n",
                WiFi.localIP().toString().c_str());
}

// ================== Loop ==================
void loop() {
  ensureWiFi();
}

// ================== Camera 初始化 ==================
bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[错误] 摄像头初始化返回 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
  }

  Serial.println("[成功] 摄像头初始化完成");
  return true;
}

// ================== WiFi 初始化 ==================
bool initWiFi() {
  Serial.printf("正在连接 WiFi：%s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    Serial.print("·");
    if (millis() - start > WIFI_MAX_FAIL_MS) {
      Serial.println("\n[错误] WiFi 连接超时");
      return false;
    }
  }

  Serial.printf("\n[成功] WiFi 已连接，IP：%s\n",
                WiFi.localIP().toString().c_str());
  return true;
}

// ================== Flash LED 控制 ==================
void initFlashLED(bool enable) {
  if (!enable) return;
  ledcSetup(FLASH_CHANNEL, FLASH_FREQ, FLASH_RESOLUTION);
  ledcAttachPin(FLASH_PIN, FLASH_CHANNEL);
  flashPwmReady = true;
  setFlashDuty(0);
  Serial.println("[成功] 闪光灯 PWM 已就绪");
}

void setFlashDuty(uint8_t duty) {
  if (!flashPwmReady) return;
  flashDuty = duty;
  ledcWrite(FLASH_CHANNEL, duty);
}

uint8_t getFlashDuty() {
  return flashDuty;
}

// 提供给 app_httpd.cpp 的 C 接口
void app_set_flash_duty(uint8_t duty) {
  setFlashDuty(duty);
}

uint8_t app_get_flash_duty() {
  return getFlashDuty();
}

// ================== WiFi 状态检测 ==================
void ensureWiFi() {
  uint32_t now = millis();
  if (now - lastWifiCheck < WIFI_CHECK_INTERVAL) return;
  lastWifiCheck = now;

  if (WiFi.status() == WL_CONNECTED) {
    if (wifiFailStart != 0) {
      Serial.printf("[信息] WiFi 已恢复，IP：%s\n",
                    WiFi.localIP().toString().c_str());
    }
    wifiFailStart = 0;
    return;
  }

  if (wifiFailStart == 0) {
    wifiFailStart = now;
    Serial.println("[警告] WiFi 断开，开始重新连接...");
  }

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  if (now - wifiFailStart > WIFI_MAX_FAIL_MS) {
    Serial.println("[错误] WiFi 重连超时，准备重启...");
    delay(1000);
    ESP.restart();
  }
}