# ESP32CamPlus

一个基于 ESP32-CAM 的轻量 Web 摄像头服务器，由 Espressif 官方的 `CameraWebServer` 示例修改而来。

支持 MJPEG 流媒体、静态抓拍、传感器参数调节、闪光灯控制和云服务器上传。

## 功能特性

- 摄像头管理：支持 OV2640/OV3660/OV5640 等传感器，默认开启镜像
- Web 服务：网页控制台、MJPEG 流、静态图、参数调节等接口
- 闪光灯控制：支持常亮、闪烁等模式
- 云服务器上传：支持自动或手动上传图像到云服务器
- WebSocket 视频流：实时推送视频流到服务器，支持多客户端观看
- Wi-Fi 自愈：自动检测并重连 Wi-Fi

## 工程结构

| 文件/目录 | 说明 |
| --- | --- |
| `ESP32CamPlus.ino` | 主程序，硬件初始化和 Wi-Fi 管理 |
| `app_httpd.cpp` | HTTP 服务实现 |
| `camera_pins.h` | 摄像头引脚映射 |
| `camera_index.h` | Web 界面资源 |
| `partitions.csv` | 分区表配置 |
| `server/` | Python 服务器端示例 |

## 快速开始

### 1. 配置 Wi-Fi

修改 `ESP32CamPlus.ino`：
```cpp
constexpr char WIFI_SSID[] = "your-wifi-ssid";
constexpr char WIFI_PASS[] = "your-wifi-password";
```

### 2. 配置云上传（可选）

修改 `ESP32CamPlus.ino`：
```cpp
// 服务器配置
constexpr char SERVER_HOST[] = "192.168.1.100";     // 服务器地址
constexpr int SERVER_PORT = 8000;                   // 服务器端口
constexpr char CLOUD_API_PATH[] = "/api/upload";    // 云上传API路径

// 云上传配置
constexpr bool CLOUD_UPLOAD_ENABLED = true;
constexpr char CLOUD_API_KEY[] = "your-api-key";
constexpr uint32_t CLOUD_UPLOAD_INTERVAL = 0;   // 0=禁用自动上传，30000则代表每30秒自动上传1次
```

### 3. 配置 WebSocket 视频流（可选）

修改 `ESP32CamPlus.ino`：
```cpp
constexpr bool WEBSOCKET_STREAM_ENABLED = true;      // 启用 WebSocket 视频流
constexpr char WS_STREAM_PATH[] = "/ws/stream";      // WebSocket流路径
constexpr int WS_STREAM_FPS = 10;                    // 视频流帧率（FPS）
```

### 4. 编译烧录

使用 Arduino IDE 或 PlatformIO，选择开发板 "AI Thinker ESP32-CAM"。

### 5. 运行验证

- 访问 `http://<IP>/` 打开网页控制台
- 访问 `http://<IP>:81/stream` 查看 MJPEG 视频流
- 手动上传：`curl "http://<IP>/cloud/upload"`
- 查看状态：`curl "http://<IP>/cloud/status"`
- 视频流查看：`http://<服务器IP>:8000/stream/<设备ID>`（如果启用了 WebSocket 视频流）

## API 接口

### 闪光灯控制

```
GET /flash?enable=1&level=128&interval=0&count=0
```

参数：
- `enable`: 0/1 开启/关闭
- `level`: 0-255 亮度
- `interval`: 闪烁间隔(ms)，0=常亮
- `count`: 闪烁次数，0=常亮

### 云上传

- `GET /cloud/upload` - 手动触发上传
- `GET /cloud/status` - 查询上传统计

## 服务器端

Python 服务器端示例位于 `server/` 目录，基于 FastAPI 框架。

快速开始：

```bash
cd server
pip install -r requirements.txt
python server.py
```

访问 `http://localhost:8000/docs` 查看 API 文档。

详细说明请参考 `server/README.md`
