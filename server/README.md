# ESP32-CAM 云服务器接收端

基于 FastAPI 的 Python 服务器，用于接收 ESP32-CAM 上传的图像和实时视频流。

## 功能特性

- 接收 JPEG 图像上传
- 实时视频流推送（WebSocket）
- API 密钥验证
- 设备 ID 和时间戳记录
- 图像文件保存和管理
- 设备统计信息
- 自动生成 API 文档
- 多客户端同时观看视频流

## 快速开始

### 1. 安装依赖

```bash
pip install -r requirements.txt
```

### 2. 配置（可选）

创建 `.env` 文件（参考 `.env.example`）：

```bash
cp .env.example .env
```

然后编辑 `.env` 文件，修改相应的配置值：

```env
# API 密钥（用于验证设备上传请求）
API_KEY=your-api-key

# 上传文件保存目录
UPLOAD_DIR=uploads

# 最大文件大小（字节），默认 10MB
MAX_FILE_SIZE=10485760

# 是否启用设备日志
ENABLE_DEVICE_LOG=true

# 服务器监听地址（0.0.0.0 表示监听所有网络接口）
HOST=0.0.0.0

# 服务器端口
PORT=8000
```

如果不创建 `.env` 文件，将使用代码中的默认值。

### 3. 运行服务器

```bash
python server.py
```

服务器启动后，访问：
- API 文档: `http://localhost:8000/docs`
- 视频流查看: `http://localhost:8000/stream/{device_id}`

## API 接口

### 上传图像

```
POST /api/upload
```

请求头：
- `X-API-Key`: API 密钥
- `X-Device-ID`: 设备 ID
- `X-Timestamp`: 图像时间戳
- `Content-Type`: image/jpeg

请求体：JPEG 图像二进制数据

### 获取服务器状态

```
GET /api/status
```

返回服务器状态、图像统计、设备信息等。

### 实时视频流

#### WebSocket 视频流接口

```
WS /ws/stream/{device_id}?api_key={api_key}
```

**设备端连接**（推送视频流）：
- 需要提供 `api_key` 查询参数
- 通过 WebSocket 发送 JPEG 帧（二进制数据）
- 服务器会自动转发给所有订阅的客户端

**客户端连接**（接收视频流）：
- 不需要 API 密钥
- 接收服务器转发的 JPEG 帧
- 支持多客户端同时观看

#### 视频流查看页面

```
GET /stream/{device_id}
```

在浏览器中打开此页面即可实时观看视频流。
- 实时状态指示（连接/断开）
- 帧率显示
- 设备ID显示
- 响应式布局，支持移动端

#### 获取活跃流列表

```
GET /api/streams
```

返回所有正在推送视频流的设备信息。

### 其他接口

- `GET /api/devices` - 获取所有设备信息
- `GET /api/device/{device_id}` - 获取特定设备信息
- `GET /api/images?page=1&per_page=20` - 列出图像
- `GET /api/image/{filename}` - 获取图像文件
- `GET /api/health` - 健康检查

## 配置说明

所有配置项都可以通过 `.env` 文件进行配置。创建 `.env` 文件（参考 `.env.example`），然后修改相应的值：

```env
# API 密钥（用于验证设备上传请求）
API_KEY=your-api-key

# 上传文件保存目录
UPLOAD_DIR=my_uploads

# 最大文件大小（字节），例如 20MB = 20971520
MAX_FILE_SIZE=20971520

# 是否启用设备日志（true/false）
ENABLE_DEVICE_LOG=false

# 服务器监听地址（0.0.0.0 表示监听所有网络接口）
HOST=0.0.0.0

# 服务器端口
PORT=8000
```

**注意：** 修改 `.env` 文件后需要重启服务器才能生效。

## ESP32 端配置

### 单张图片上传

ESP32 定期上传单张图片，参考 `ESP32CamPlus.ino` 中的实现。

### 实时视频流推送（推荐）

使用 WebSocket 推送实时视频流，功能已集成到 `ESP32CamPlus.ino` 中：

1. 安装 WebSockets 库（Arduino IDE）：
   - 工具 -> 管理库 -> 搜索 "WebSockets" -> 安装 "WebSockets by Markus Sattler"

2. 配置参数：
   ```cpp
   // 服务器配置
   constexpr char SERVER_HOST[] = "192.168.1.100";     // 服务器地址
   constexpr int SERVER_PORT = 8000;                   // 服务器端口
   constexpr char WS_STREAM_PATH[] = "/ws/stream";    // WebSocket流路径
   
   // WebSocket 视频流配置
   constexpr bool WEBSOCKET_STREAM_ENABLED = true;    // 启用 WebSocket 视频流
   constexpr char CLOUD_API_KEY[] = "your-api-key";   // API密钥
   constexpr int WS_STREAM_FPS = 10;                  // 视频流帧率（FPS）
   ```

3. 上传代码到 ESP32

4. 在浏览器中访问：`http://服务器IP:8000/stream/<设备ID>`
   - 设备ID会自动生成，格式为 `ESP32-CAM-XXXXXXXX`（基于MAC地址）

## 测试

### 测试图片上传

```bash
curl -X POST http://localhost:8000/api/upload \
  -H "X-API-Key: your-api-key" \
  -H "X-Device-ID: ESP32-CAM-TEST" \
  -H "X-Timestamp: 1234567890.123456" \
  -H "Content-Type: image/jpeg" \
  --data-binary @test_image.jpg
```