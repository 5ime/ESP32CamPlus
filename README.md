# ESP32CamPlus

一个基于 ESP32-CAM 的轻量 Web 摄像头服务器，由 Espressif 官方的 `CameraWebServer` 示例修改而来。固件提供 MJPEG 流媒体、静态抓拍、传感器参数调节与板载闪光灯控制，适合远程监控、实验室取像或 IoT Demo。

## 功能亮点
- **摄像头管理**：初始化 OV2640/OV3660/OV5640 等主流传感器，默认开启水平/垂直镜像以适配常见安装姿态。
- **Web 服务**：`/`（网页控制台）、`/stream`（MJPEG 流）、`/capture`/`/bmp`（静态图）、`/status`（当前参数 JSON）、`/control`（调参）等常用接口一应俱全。
- **闪光灯/补光灯**：全新的 `/flash` REST 接口，支持常亮/常灭以及固定次数的闪烁序列，参数只有 4 个，使用简单。
- **Wi-Fi自愈**：定期检测 `WiFi.status()`，失联时自动重连，超过设定阈值则重启 MCU，保证远程场景的可用性。
- **日志友好**：序列口输出初始化过程、Wi-Fi 状态、HTTP 控制与 LED 操作记录，便于排查问题。

## 工程结构

| 文件 | 说明 |
| --- | --- |
| `ESP32CamPlus.ino` | ESP32CamPlus 入口，负责硬件初始化、Wi-Fi 维护、LED 封装和调用 `startCameraServer()` |
| `app_httpd.cpp` | HTTP 服务主体（源自 Espressif 示例并扩展），含拍照/流媒体/调参/闪光灯接口 |
| `camera_pins.h` | AI Thinker 板载引脚映射 |
| `camera_index.h` | Web 控制台的 gzip HTML 资源 |
| `partitions.csv` | 自定义分区表（例如人脸识别数据存储） |

## `ESP32CamPlus.ino` 主要职责

- **`setup()`**：开启串口→初始化摄像头（双帧缓冲、VGA、JPEG 质量 12）→准备补光 LED PWM→连接 Wi-Fi→启动 Web Server。成功后会打印访问地址 `http://<IP>:81/stream`。
- **`loop()`**：仅调用 `ensureWiFi()`，每 5 秒检查一次连接情况；若长时间断链则自动重启。
- **摄像头初始化**：配置所有 D0~D7、XCLK、PCLK、VSYNC、HREF、SCCB、PWDN、RESET 等引脚，默认 `PIXFORMAT_JPEG`，`CAMERA_GRAB_LATEST` 以降低延迟。
- **Wi-Fi 初始化/保活**：STA 模式、关闭省电和自动重连，由固件手动管理重连策略，`WIFI_MAX_FAIL_MS` 可调。
- **LED 控制**：使用 `ledcAttachPin(GPIO_NUM_4, channel0)`，导出 `app_set_flash_duty()` / `app_get_flash_duty()` 供 HTTP 层调用。

## 闪光灯 HTTP 接口

Endpoint：`GET /flash`

| 参数 | 必填 | 取值 | 说明 |
| --- | --- | --- | --- |
| `enable` | 是 | `0`/`1`/`true`/`false`/`on`/`off` | 是否开启闪光灯逻辑 |
| `level` | 否 | `0-255` | 开启时的亮度占空比，未提供则沿用当前值（闪烁模式默认 255） |
| `interval` | 否 | `0-5000`（ms） | 闪烁间隔；`0` 表示常亮 |
| `count` | 否 | `0-20` | 闪烁次数；`0` 表示常亮 |

行为规则：
1. `enable=0` → 立即熄灭（忽略其他参数）。
2. `enable=1` 且 `count=0` 或 `interval=0` → 常亮到指定 `level`。
3. `enable=1` 且 `count>0` 且 `interval>0` → 按 `[亮 interval] -> [灭 interval]` 的节奏闪烁 `count` 次，结束后恢复先前亮度。

示例：
```bash
# 常亮 160
curl "http://<IP>/flash?enable=1&level=160&interval=0&count=0"

# 以亮度 200 闪烁 5 次，亮/灭各 120 ms
curl "http://<IP>/flash?enable=1&level=200&interval=120&count=5"

# 常灭
curl "http://<IP>/flash?enable=0"
```

返回 JSON（`level` 反映当前占空比；若执行闪烁，会在结束后恢复为原先亮度）：
```json
{"enable":1,"level":0,"interval":120,"count":5}
```

## 构建与烧录
1. **环境**：Arduino IDE / PlatformIO + ESP32 Arduino Core（>=2.0），选择开发板 “AI Thinker ESP32-CAM”。
2. **配置 Wi-Fi**：修改 `ESP32CamPlus.ino` 顶部的 `WIFI_SSID` / `WIFI_PASS`。
3. **编译 & 烧录**：按常规 ESP32-CAM 步骤（需 USB-TTL + IO0 拉低进入下载模式）。
4. **监视串口**：115200 波特率，确认获取的 IP 与服务状态。

## 运行验证
1. 访问 `http://<IP>/` 打开网页控制台，能看到实时流、拍照、参数调整等控件。
2. 打开 `http://<IP>:81/stream` 验证 MJPEG 实时流。
3. 使用上述 `/flash` 示例命令检查 LED 是否可调亮度并闪烁。
4. 断开/恢复 Wi-Fi，观察串口中 `ensureWiFi()` 的重连与自恢复日志。

欢迎在此基础上拓展 AI 分析、云端存储或更友好的前端 UI。