#include "esp_camera.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_http_server.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>

#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

// ================== WiFi 配置 ==================
constexpr char WIFI_SSID[] = "iami233";
constexpr char WIFI_PASS[] = "12345678";
constexpr uint32_t WIFI_CHECK_INTERVAL = 5000;      // WiFi检查间隔(ms)
constexpr uint32_t WIFI_MAX_FAIL_MS = 20000;        // WiFi最大失败时间(ms)
constexpr uint32_t WIFI_CONNECT_TIMEOUT = 20000;    // WiFi连接超时(ms)
constexpr uint32_t WIFI_RETRY_DELAY = 200;          // WiFi重试延迟(ms)

// ================== Flash LED 配置 ==================
constexpr bool USE_FLASH = true;
constexpr gpio_num_t FLASH_PIN = GPIO_NUM_4;
constexpr uint8_t FLASH_CHANNEL = 0;
constexpr uint16_t FLASH_FREQ = 5000;
constexpr uint8_t FLASH_RESOLUTION = 8;
constexpr uint8_t FLASH_DEFAULT_DUTY = 0;           // 默认占空比

// ================== Camera 配置 ==================
constexpr uint32_t CAMERA_XCLK_FREQ = 20000000;     // 20MHz
constexpr pixformat_t CAMERA_PIXEL_FORMAT = PIXFORMAT_JPEG;
constexpr framesize_t CAMERA_FRAME_SIZE = FRAMESIZE_VGA;
constexpr uint8_t CAMERA_JPEG_QUALITY = 12;
constexpr uint8_t CAMERA_FB_COUNT = 2;
constexpr camera_grab_mode_t CAMERA_GRAB_MODE = CAMERA_GRAB_LATEST;

// ================== Serial 配置 ==================
constexpr uint32_t SERIAL_BAUD = 115200;

// ================== 服务器配置 ==================
constexpr char SERVER_HOST[] = "your_server_ip";          // 服务器地址
constexpr int SERVER_PORT = 8000;                        // 服务器端口
constexpr char CLOUD_API_PATH[] = "/api/upload";         // 云上传API路径
constexpr char WS_STREAM_PATH[] = "/ws/stream";          // WebSocket流路径

// ================== 云服务器配置 ==================
constexpr bool CLOUD_UPLOAD_ENABLED = false;              // 是否启用云上传
constexpr char CLOUD_API_KEY[] = "your-api-key";         // API密钥（可选）
constexpr uint32_t CLOUD_UPLOAD_INTERVAL = 0;            // 自动上传间隔(ms)，0=禁用自动上传
constexpr uint32_t CLOUD_UPLOAD_TIMEOUT = 10000;         // 上传超时时间(ms)
constexpr bool CLOUD_UPLOAD_VERIFY_SSL = false;          // 是否验证SSL证书（生产环境建议true）

// ================== WebSocket 视频流配置 ==================
constexpr bool WEBSOCKET_STREAM_ENABLED = false;          // 是否启用 WebSocket 视频流推送
constexpr int WS_STREAM_FPS = 10;                        // 视频流帧率（FPS）
constexpr uint32_t WS_STREAM_INTERVAL = 1000 / WS_STREAM_FPS;  // 帧间隔（毫秒）
constexpr uint32_t WS_RECONNECT_INTERVAL = 5000;        // WebSocket 重连间隔(ms)

// ================== 全局状态 ==================
static uint32_t lastWifiCheck = 0;
static uint32_t wifiFailStart = 0;
static bool flashPwmReady = false;
static uint8_t flashDuty = 0;
static uint32_t lastCloudUpload = 0;                    // 上次上传时间
static uint32_t cloudUploadCount = 0;                    // 上传计数
static uint32_t cloudUploadFailCount = 0;                // 上传失败计数

// WebSocket 视频流相关
static WebSocketsClient webSocket;
static uint32_t lastStreamFrameTime = 0;
static char deviceId[32] = {0};                           // 设备ID缓存

// ================== 外部声明（由 app_httpd.cpp 提供） ==================
extern httpd_handle_t stream_httpd;      // 流媒体服务器句柄
extern httpd_handle_t camera_httpd;      // 控制服务器句柄
extern void startCameraServer();         // 启动Web服务器

// 供 app_httpd.cpp 中闪光灯接口调用的C接口
extern "C" void app_set_flash_duty(uint8_t duty);
extern "C" uint8_t app_get_flash_duty();

// ================== 函数声明 ==================
bool initCamera();
bool initWiFi();
void ensureWiFi();
void initFlashLED(bool enable);
void setFlashDuty(uint8_t duty);
uint8_t getFlashDuty();
bool uploadImageToCloud(camera_fb_t* fb = nullptr);
void checkCloudUpload();
const char* getDeviceId();
void initWebSocket();
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length);
void sendVideoFrameToWebSocket();
void checkWebSocketStream();

// ================== Setup ==================
void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.setDebugOutput(false);
  Serial.println("\n=== ESP32-CAM 启动 ===");

  // 初始化摄像头
  if (!initCamera()) {
    Serial.println("[致命] 摄像头初始化失败，准备重启...");
    delay(1000);
    ESP.restart();
  }

  // 初始化闪光灯
  if (USE_FLASH) {
    initFlashLED(true);
  }

  // 连接WiFi
  if (!initWiFi()) {
    Serial.println("[致命] WiFi 初始化失败，准备重启...");
    delay(1000);
    ESP.restart();
  }

  // 启动Web服务器
  startCameraServer();
  Serial.printf("[成功] 摄像头流已就绪！\n");
  Serial.printf("  - 控制台: http://%s/\n", WiFi.localIP().toString().c_str());
  Serial.printf("  - 视频流: http://%s:81/stream\n", WiFi.localIP().toString().c_str());
  Serial.printf("  - 闪光灯: http://%s/flash?enable=1&level=128\n", WiFi.localIP().toString().c_str());
  
  // 显示云上传配置
  if (CLOUD_UPLOAD_ENABLED) {
    Serial.printf("[信息] 云上传已启用\n");
    Serial.printf("  - 服务器: http://%s:%d%s\n", SERVER_HOST, SERVER_PORT, CLOUD_API_PATH);
    if (CLOUD_UPLOAD_INTERVAL > 0) {
      Serial.printf("  - 自动上传间隔: %lu秒\n", CLOUD_UPLOAD_INTERVAL / 1000);
    } else {
      Serial.printf("  - 自动上传: 已禁用（手动触发）\n");
    }
  }
  
  // 初始化 WebSocket 视频流（如果启用）
  if (WEBSOCKET_STREAM_ENABLED) {
    initWebSocket();
    Serial.printf("[信息] WebSocket 视频流已启用\n");
    Serial.printf("  - 服务器: ws://%s:%d%s/%s\n", SERVER_HOST, SERVER_PORT, WS_STREAM_PATH, getDeviceId());
    Serial.printf("  - 帧率: %d FPS\n", WS_STREAM_FPS);
  }
}

// ================== Loop ==================
void loop() {
  // 定期检查并维护WiFi连接
  ensureWiFi();
  
  // WebSocket 循环处理（如果启用）
  if (WEBSOCKET_STREAM_ENABLED) {
    webSocket.loop();
    checkWebSocketStream();
  }
  
  // 检查是否需要上传到云服务器
  if (CLOUD_UPLOAD_ENABLED) {
    checkCloudUpload();
  }
  
  // 注意：其他任务（如HTTP服务器）在后台任务中运行
}

// ================== Camera 初始化 ==================
bool initCamera() {
  camera_config_t config = {};
  
  // LEDC配置
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  
  // 数据引脚配置
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  
  // 控制引脚配置
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  
  // 摄像头参数配置
  config.xclk_freq_hz = CAMERA_XCLK_FREQ;
  config.pixel_format = CAMERA_PIXEL_FORMAT;
  config.frame_size = CAMERA_FRAME_SIZE;
  config.jpeg_quality = CAMERA_JPEG_QUALITY;
  config.fb_count = CAMERA_FB_COUNT;
  config.grab_mode = CAMERA_GRAB_MODE;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[错误] 摄像头初始化失败: 0x%x\n", err);
    return false;
  }

  // 配置传感器参数
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 1);      // 垂直翻转
    s->set_hmirror(s, 1);    // 水平镜像
    Serial.println("[信息] 摄像头镜像已启用");
  } else {
    Serial.println("[警告] 无法获取传感器对象");
  }

  Serial.println("[成功] 摄像头初始化完成");
  return true;
}

// ================== WiFi 初始化 ==================
bool initWiFi() {
  Serial.printf("[信息] 正在连接 WiFi: %s\n", WIFI_SSID);
  
  // 配置WiFi模式
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);              // 关闭WiFi省电模式以提高稳定性
  WiFi.setAutoReconnect(false);     // 禁用自动重连，由固件手动管理
  
  // 开始连接
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // 等待连接，带超时检测
  uint32_t start = millis();
  uint8_t dotCount = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(WIFI_RETRY_DELAY);
    Serial.print(".");
    dotCount++;
    if (dotCount % 10 == 0) {
      Serial.print(" ");
    }
    
    if (millis() - start > WIFI_CONNECT_TIMEOUT) {
      Serial.println("\n[错误] WiFi 连接超时");
      return false;
    }
  }

  Serial.printf("\n[成功] WiFi 已连接\n");
  Serial.printf("  - IP地址: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("  - 子网掩码: %s\n", WiFi.subnetMask().toString().c_str());
  Serial.printf("  - 网关: %s\n", WiFi.gatewayIP().toString().c_str());
  Serial.printf("  - RSSI: %d dBm\n", WiFi.RSSI());
  return true;
}

// ================== Flash LED 控制 ==================
void initFlashLED(bool enable) {
  if (!enable) {
    Serial.println("[信息] 闪光灯功能已禁用");
    return;
  }
  
  ledcSetup(FLASH_CHANNEL, FLASH_FREQ, FLASH_RESOLUTION);
  ledcAttachPin(FLASH_PIN, FLASH_CHANNEL);
  flashPwmReady = true;
  setFlashDuty(FLASH_DEFAULT_DUTY);
  Serial.println("[成功] 闪光灯 PWM 已就绪");
}

void setFlashDuty(uint8_t duty) {
  if (!flashPwmReady) {
    Serial.println("[警告] 闪光灯PWM未初始化");
    return;
  }
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
  
  // 节流：避免过于频繁的检查
  if (now - lastWifiCheck < WIFI_CHECK_INTERVAL) {
    return;
  }
  lastWifiCheck = now;

  // WiFi已连接
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiFailStart != 0) {
      // 从断开状态恢复
      Serial.printf("[信息] WiFi 已恢复，IP: %s\n",
                    WiFi.localIP().toString().c_str());
      wifiFailStart = 0;
    }
    return;
  }

  // WiFi断开，开始重连流程
  if (wifiFailStart == 0) {
    wifiFailStart = now;
    Serial.println("[警告] WiFi 断开，开始重新连接...");
  }

  // 尝试重连
  WiFi.disconnect();
  delay(100);  // 短暂延迟确保断开完成
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // 检查是否超过最大失败时间
  if (now - wifiFailStart > WIFI_MAX_FAIL_MS) {
    Serial.println("[错误] WiFi 重连超时，准备重启...");
    delay(1000);
    ESP.restart();
  }
}

// ================== 云服务器上传 ==================
void checkCloudUpload() {
  // 如果禁用了自动上传，则跳过
  if (CLOUD_UPLOAD_INTERVAL == 0) {
    return;
  }
  
  uint32_t now = millis();
  
  // 检查是否到了上传时间
  if (now - lastCloudUpload < CLOUD_UPLOAD_INTERVAL) {
    return;
  }
  
  // 确保WiFi已连接
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[警告] WiFi未连接，跳过云上传");
    return;
  }
  
  // 执行上传
  lastCloudUpload = now;
  uploadImageToCloud();
}

bool uploadImageToCloud(camera_fb_t* fb) {
  bool needRelease = false;
  
  // 如果没有提供帧缓冲区，则获取新的
  if (fb == nullptr) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[错误] 无法获取摄像头帧");
      cloudUploadFailCount++;
      return false;
    }
    needRelease = true;
  }
  
  // 检查图像格式
  if (fb->format != PIXFORMAT_JPEG) {
    Serial.println("[错误] 仅支持JPEG格式上传");
    if (needRelease) {
      esp_camera_fb_return(fb);
    }
    cloudUploadFailCount++;
    return false;
  }
  
  Serial.printf("[信息] 开始上传图像到云服务器 (%u字节)...\n", fb->len);
  
  // 构建完整的服务器URL
  char serverUrl[128];
  snprintf(serverUrl, sizeof(serverUrl), "http://%s:%d%s", SERVER_HOST, SERVER_PORT, CLOUD_API_PATH);
  
  HTTPClient http;
  http.begin(serverUrl);
  http.setTimeout(CLOUD_UPLOAD_TIMEOUT);
  
  // 设置请求头
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("Content-Length", String(fb->len));
  
  // 如果配置了API密钥，添加到请求头
  if (strlen(CLOUD_API_KEY) > 0) {
    http.addHeader("X-API-Key", CLOUD_API_KEY);
  }
  
  // 添加设备信息（使用统一的设备ID函数）
  http.addHeader("X-Device-ID", getDeviceId());
  
  // 添加时间戳
  char timestamp[32];
  snprintf(timestamp, sizeof(timestamp), "%ld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
  http.addHeader("X-Timestamp", timestamp);
  
  // 发送POST请求
  int httpResponseCode = http.POST(fb->buf, fb->len);
  
  // 检查响应
  bool success = false;
  if (httpResponseCode > 0) {
    Serial.printf("[信息] 上传响应代码: %d\n", httpResponseCode);
    if (httpResponseCode == HTTP_CODE_OK || httpResponseCode == HTTP_CODE_CREATED) {
      String response = http.getString();
      Serial.printf("[成功] 上传成功！响应: %s\n", response.c_str());
      cloudUploadCount++;
      success = true;
    } else {
      Serial.printf("[警告] 上传失败，响应代码: %d\n", httpResponseCode);
      String response = http.getString();
      Serial.printf("响应内容: %s\n", response.c_str());
      cloudUploadFailCount++;
    }
  } else {
    Serial.printf("[错误] 上传失败: %s\n", http.errorToString(httpResponseCode).c_str());
    cloudUploadFailCount++;
  }
  
  http.end();
  
  // 释放帧缓冲区（如果需要）
  if (needRelease) {
    esp_camera_fb_return(fb);
  }
  
  // 打印统计信息
  Serial.printf("[统计] 上传成功: %lu, 失败: %lu\n", cloudUploadCount, cloudUploadFailCount);
  
  return success;
}

// 提供给 app_httpd.cpp 的 C 接口
extern "C" bool app_upload_to_cloud() {
  if (!CLOUD_UPLOAD_ENABLED) {
    Serial.println("[警告] 云上传功能已禁用");
    return false;
  }
  
  // 确保WiFi已连接
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[错误] WiFi未连接，无法上传");
    return false;
  }
  
  // 上传图像（会自动获取新的帧）
  return uploadImageToCloud();
}

extern "C" uint32_t app_get_upload_count() {
  return cloudUploadCount;
}

extern "C" uint32_t app_get_upload_fail_count() {
  return cloudUploadFailCount;
}

// ================== WebSocket 视频流功能 ==================
const char* getDeviceId() {
  // 如果设备ID还未生成，则生成并缓存
  if (deviceId[0] == '\0') {
    snprintf(deviceId, sizeof(deviceId), "ESP32-CAM-%08X", (uint32_t)ESP.getEfuseMac());
  }
  return deviceId;
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.println("[WebSocket] 断开连接");
      break;
    case WStype_CONNECTED:
      Serial.printf("[WebSocket] 已连接到服务器: %s\n", payload);
      break;
    case WStype_TEXT:
      Serial.printf("[WebSocket] 收到文本消息: %s\n", payload);
      break;
    case WStype_BIN:
      Serial.printf("[WebSocket] 收到二进制数据: %u 字节\n", length);
      break;
    case WStype_ERROR:
      Serial.println("[WebSocket] 连接错误");
      break;
    default:
      break;
  }
}

void initWebSocket() {
  // 确保WiFi已连接
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[警告] WiFi未连接，无法初始化WebSocket");
    return;
  }
  
  // 构建 WebSocket 路径（包含 API 密钥）
  String wsPath = String(WS_STREAM_PATH) + "/" + getDeviceId() + "?api_key=" + CLOUD_API_KEY;
  
  // 配置 WebSocket
  webSocket.begin(SERVER_HOST, SERVER_PORT, wsPath);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL);
  
  Serial.printf("[信息] WebSocket 配置完成，等待连接...\n");
}

void sendVideoFrameToWebSocket() {
  // 检查连接状态
  if (!webSocket.isConnected()) {
    return;
  }
  
  // 获取摄像头帧
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[警告] 无法获取摄像头帧（WebSocket）");
    return;
  }
  
  // 检查 JPEG 格式
  if (fb->format != PIXFORMAT_JPEG) {
    Serial.println("[警告] 仅支持 JPEG 格式（WebSocket）");
    esp_camera_fb_return(fb);
    return;
  }
  
  // 通过 WebSocket 发送 JPEG 帧
  webSocket.sendBIN(fb->buf, fb->len);
  
  // 释放帧缓冲区
  esp_camera_fb_return(fb);
}

void checkWebSocketStream() {
  // 确保WiFi已连接
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  
  uint32_t now = millis();
  
  // 检查是否到了发送帧的时间
  if (now - lastStreamFrameTime >= WS_STREAM_INTERVAL) {
    if (webSocket.isConnected()) {
      sendVideoFrameToWebSocket();
    } else {
      // 如果未连接，尝试重连
      initWebSocket();
    }
    lastStreamFrameTime = now;
  }
}