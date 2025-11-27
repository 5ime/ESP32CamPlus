#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32-CAM 云服务器接收端
支持接收图像上传、API密钥验证、设备管理等功能
"""

from fastapi import FastAPI, Request, HTTPException, Header, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, HTMLResponse
from fastapi.middleware.cors import CORSMiddleware
from datetime import datetime
from typing import Optional, Dict, Set
import json
import os
import asyncio
from pathlib import Path
import logging
from dotenv import load_dotenv

load_dotenv()

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

app = FastAPI(title="ESP32-CAM Cloud Server", version="1.0.0")
app.add_middleware(CORSMiddleware, allow_origins=["*"], allow_credentials=True, 
                   allow_methods=["*"], allow_headers=["*"])

CONFIG = {
    'API_KEY': os.getenv('API_KEY', 'your-api-key'),
    'UPLOAD_DIR': os.getenv('UPLOAD_DIR', 'uploads'),
    'MAX_FILE_SIZE': int(os.getenv('MAX_FILE_SIZE', str(10 * 1024 * 1024))),
    'ENABLE_DEVICE_LOG': os.getenv('ENABLE_DEVICE_LOG', 'true').lower() == 'true',
    'HOST': os.getenv('HOST', '0.0.0.0'),
    'PORT': int(os.getenv('PORT', '8000')),
}

upload_dir = Path(CONFIG['UPLOAD_DIR'])
upload_dir.mkdir(exist_ok=True)
device_stats = {}
device_streams: Dict[str, WebSocket] = {}
client_streams: Dict[str, Set[WebSocket]] = {}


# ==================== 辅助函数 ====================
def validate_api_key(api_key: Optional[str]) -> None:
    """验证API密钥"""
    if api_key != CONFIG['API_KEY']:
        raise HTTPException(status_code=401, detail="Invalid API key")


def is_valid_jpeg(data: bytes) -> bool:
    """验证JPEG格式"""
    return len(data) >= 4 and data[:2] == b'\xff\xd8' and data[-2:] == b'\xff\xd9'


def generate_filename(device_id: str, timestamp: Optional[str] = None) -> str:
    """生成文件名"""
    if timestamp:
        safe_ts = timestamp.replace('.', '_').replace(':', '-')
        return f"{device_id}_{safe_ts}.jpg"
    return f"{device_id}_{datetime.now().strftime('%Y%m%d_%H%M%S_%f')}.jpg"


def save_device_log(device_id: str, timestamp: str, filename: str, success: bool = True):
    """保存设备上传日志"""
    if not CONFIG['ENABLE_DEVICE_LOG']:
        return
    
    log_file = upload_dir / 'device_log.json'
    log_entry = {
        'device_id': device_id,
        'timestamp': timestamp,
        'filename': filename,
        'success': success,
        'upload_time': datetime.now().isoformat()
    }
    
    logs = json.loads(log_file.read_text(encoding='utf-8')) if log_file.exists() else []
    logs.append(log_entry)
    if len(logs) > 1000:
        logs = logs[-1000:]
    
    log_file.write_text(json.dumps(logs, ensure_ascii=False, indent=2), encoding='utf-8')


def update_device_stats(device_id: str, success: bool = True):
    """更新设备统计信息"""
    if device_id not in device_stats:
        device_stats[device_id] = {'total': 0, 'success': 0, 'fail': 0, 'last_upload': None}
    
    stats = device_stats[device_id]
    stats['total'] += 1
    if success:
        stats['success'] += 1
        stats['last_upload'] = datetime.now().isoformat()
    else:
        stats['fail'] += 1


def get_upload_files():
    """获取所有上传的文件"""
    return sorted(upload_dir.glob('*.jpg'), key=lambda x: x.stat().st_mtime, reverse=True)


# ==================== API 路由 ====================
@app.post("/api/upload")
async def upload_image(
    request: Request,
    x_api_key: Optional[str] = Header(None, alias="X-API-Key"),
    x_device_id: Optional[str] = Header(None, alias="X-Device-ID"),
    x_timestamp: Optional[str] = Header(None, alias="X-Timestamp")
):
    """接收ESP32-CAM上传的图像"""
    validate_api_key(x_api_key)
    
    device_id = x_device_id or 'unknown'
    image_timestamp = x_timestamp or ''
    logger.info(f"Upload from device: {device_id}, timestamp: {image_timestamp}")
    
    image_data = await request.body()
    if not image_data:
        raise HTTPException(status_code=400, detail="No image data received")
    
    if len(image_data) > CONFIG['MAX_FILE_SIZE']:
        raise HTTPException(status_code=413, detail=f"File too large. Max: {CONFIG['MAX_FILE_SIZE']} bytes")
    
    if not is_valid_jpeg(image_data):
        raise HTTPException(status_code=400, detail="Invalid JPEG format")
    
    filename = generate_filename(device_id, image_timestamp)
    filepath = upload_dir / filename
    filepath.write_bytes(image_data)
    
    logger.info(f"Image saved: {filename} ({len(image_data)} bytes)")
    update_device_stats(device_id, success=True)
    save_device_log(device_id, image_timestamp, filename, success=True)
    
    return {
        'success': True,
        'filename': filename,
        'size': len(image_data),
        'device_id': device_id,
        'timestamp': image_timestamp,
        'upload_time': datetime.now().isoformat()
    }


@app.get("/api/status")
async def get_status():
    """获取服务器状态和统计信息"""
    upload_files = list(upload_dir.glob('*.jpg'))
    total_size = sum(f.stat().st_size for f in upload_files)
    
    return {
        'success': True,
        'server_status': 'running',
        'total_images': len(upload_files),
        'total_size_mb': round(total_size / (1024 * 1024), 2),
        'device_count': len(device_stats),
        'devices': device_stats,
        'upload_dir': str(upload_dir.absolute())
    }


@app.get("/api/devices")
async def get_devices():
    """获取所有设备信息"""
    return {'success': True, 'devices': device_stats}


@app.get("/api/device/{device_id}")
async def get_device_info(device_id: str):
    """获取特定设备信息"""
    if device_id not in device_stats:
        raise HTTPException(status_code=404, detail="Device not found")
    return {'success': True, 'device_id': device_id, 'stats': device_stats[device_id]}


@app.get("/api/images")
async def list_images(page: int = 1, per_page: int = 20):
    """列出所有上传的图像"""
    upload_files = get_upload_files()
    total = len(upload_files)
    start = (page - 1) * per_page
    
    images = []
    for f in upload_files[start:start + per_page]:
        stat = f.stat()
        images.append({
            'filename': f.name,
            'size': stat.st_size,
            'upload_time': datetime.fromtimestamp(stat.st_mtime).isoformat(),
            'url': f'/api/image/{f.name}'
        })
    
    return {'success': True, 'total': total, 'page': page, 'per_page': per_page, 'images': images}


@app.get("/api/image/{filename}")
async def get_image(filename: str):
    """获取图像文件"""
    if '..' in filename or '/' in filename:
        raise HTTPException(status_code=400, detail="Invalid filename")
    
    filepath = upload_dir / filename
    if not filepath.exists():
        raise HTTPException(status_code=404, detail="Image not found")
    
    return FileResponse(filepath, media_type='image/jpeg')


@app.get("/api/health")
async def health_check():
    """健康检查接口"""
    return {'status': 'healthy', 'timestamp': datetime.now().isoformat()}


# ==================== 视频流功能 ====================
@app.websocket("/ws/stream/{device_id}")
async def video_stream(websocket: WebSocket, device_id: str, x_api_key: Optional[str] = None):
    """
    WebSocket 视频流接口
    设备端连接此接口推送视频帧，客户端连接此接口接收视频流
    """
    await websocket.accept()
    
    if not x_api_key:
        query_params = dict(websocket.query_params)
        x_api_key = query_params.get('api_key')
    
    is_device = False
    if x_api_key:
        try:
            validate_api_key(x_api_key)
            is_device = True
            logger.info(f"设备 {device_id} 连接视频流")
        except HTTPException:
            pass
    
    try:
        if is_device:
            device_streams[device_id] = websocket
            
            if device_id not in client_streams:
                client_streams[device_id] = set()
            
            async def broadcast_frame(frame_data: bytes):
                """向所有客户端广播视频帧"""
                disconnected = set()
                for client in client_streams.get(device_id, set()):
                    try:
                        await client.send_bytes(frame_data)
                    except Exception as e:
                        logger.warning(f"向客户端发送帧失败: {e}")
                        disconnected.add(client)
                
                for client in disconnected:
                    client_streams[device_id].discard(client)
                    try:
                        await client.close()
                    except:
                        pass
            
            while True:
                try:
                    frame_data = await websocket.receive_bytes()
                    
                    if is_valid_jpeg(frame_data):
                        await broadcast_frame(frame_data)
                    else:
                        logger.warning(f"设备 {device_id} 发送了无效的 JPEG 帧")
                        
                except WebSocketDisconnect:
                    logger.info(f"设备 {device_id} 断开连接")
                    break
                except Exception as e:
                    logger.error(f"处理设备 {device_id} 视频流错误: {e}")
                    break
        else:
            logger.info(f"客户端连接设备 {device_id} 的视频流")
            
            if device_id not in client_streams:
                client_streams[device_id] = set()
            client_streams[device_id].add(websocket)
            
            try:
                while True:
                    data = await websocket.receive()
                    if data.get("type") == "websocket.disconnect":
                        break
            except WebSocketDisconnect:
                logger.info(f"客户端断开设备 {device_id} 的视频流")
            except Exception as e:
                logger.error(f"客户端连接错误: {e}")
                
    except Exception as e:
        logger.error(f"WebSocket 错误: {e}")
    finally:
        if is_device:
            device_streams.pop(device_id, None)
        else:
            client_streams.get(device_id, set()).discard(websocket)
            if device_id in client_streams and not client_streams[device_id]:
                del client_streams[device_id]
        
        try:
            await websocket.close()
        except:
            pass


@app.get("/stream/{device_id}", response_class=HTMLResponse)
async def stream_viewer(device_id: str):
    """视频流查看页面"""
    host = CONFIG['HOST']
    port = CONFIG['PORT']
    ws_host = host if host != '0.0.0.0' else 'localhost'
    ws_url = f"ws://{ws_host}:{port}/ws/stream/{device_id}"
    
    html = f"""
    <!DOCTYPE html>
    <html lang="zh-CN">
    <head>
        <meta charset="utf-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>实时视频流 · {device_id}</title>
        <style>
            * {{
                margin: 0;
                padding: 0;
                box-sizing: border-box;
            }}
            
            body {{
                font-family: -apple-system, BlinkMacSystemFont, "SF Pro Display", "Segoe UI", "Helvetica Neue", Arial, sans-serif;
                background: linear-gradient(135deg, #0a0a0a 0%, #1a1a1a 100%);
                color: #ffffff;
                min-height: 100vh;
                display: flex;
                flex-direction: column;
                align-items: center;
                justify-content: center;
                padding: 40px 20px;
                overflow-x: hidden;
            }}
            
            .container {{
                width: 100%;
                max-width: 1200px;
                display: flex;
                flex-direction: column;
                align-items: center;
                gap: 32px;
            }}
            
            .header {{
                text-align: center;
                margin-bottom: 8px;
            }}
            
            .header-content {{
                display: flex;
                align-items: center;
                justify-content: center;
                gap: 12px;
                flex-wrap: wrap;
            }}
            
            .header h1 {{
                font-size: 32px;
                font-weight: 600;
                letter-spacing: -0.5px;
                color: #ffffff;
                margin: 0;
                opacity: 0.95;
            }}
            
            .header .subtitle {{
                font-size: 15px;
                font-weight: 400;
                color: rgba(255, 255, 255, 0.6);
                letter-spacing: 0.2px;
                margin-top: 8px;
            }}
            
            .status-indicator {{
                display: inline-flex;
                align-items: center;
                gap: 6px;
                padding: 6px 12px;
                background: rgba(255, 255, 255, 0.08);
                backdrop-filter: blur(20px);
                border-radius: 16px;
                font-size: 12px;
                font-weight: 500;
                color: rgba(255, 255, 255, 0.8);
                transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
                border: 1px solid rgba(255, 255, 255, 0.1);
            }}
            
            .status-indicator.connected {{
                background: rgba(52, 199, 89, 0.2);
                border-color: rgba(52, 199, 89, 0.3);
                color: #34c759;
            }}
            
            .status-indicator.disconnected {{
                background: rgba(255, 59, 48, 0.2);
                border-color: rgba(255, 59, 48, 0.3);
                color: #ff3b30;
            }}
            
            .status-dot {{
                width: 5px;
                height: 5px;
                border-radius: 50%;
                background: currentColor;
                animation: pulse 2s ease-in-out infinite;
            }}
            
            @keyframes pulse {{
                0%, 100% {{
                    opacity: 1;
                    transform: scale(1);
                }}
                50% {{
                    opacity: 0.5;
                    transform: scale(0.8);
                }}
            }}
            
            .video-wrapper {{
                position: relative;
                width: 100%;
                max-width: 900px;
                aspect-ratio: 16 / 9;
                border-radius: 24px;
                overflow: hidden;
                background: #000000;
                box-shadow: 0 20px 60px rgba(0, 0, 0, 0.5),
                            0 0 0 1px rgba(255, 255, 255, 0.05);
                transition: all 0.4s cubic-bezier(0.4, 0, 0.2, 1);
            }}
            
            .video-wrapper:hover {{
                transform: translateY(-2px);
                box-shadow: 0 24px 80px rgba(0, 0, 0, 0.6),
                            0 0 0 1px rgba(255, 255, 255, 0.1);
            }}
            
            #videoFrame {{
                width: 100%;
                height: 100%;
                object-fit: contain;
                display: block;
                opacity: 0;
                transition: opacity 0.3s ease-in-out;
            }}
            
            #videoFrame.loaded {{
                opacity: 1;
            }}
            
            .loading-overlay {{
                position: absolute;
                top: 0;
                left: 0;
                right: 0;
                bottom: 0;
                display: flex;
                align-items: center;
                justify-content: center;
                background: rgba(0, 0, 0, 0.3);
                backdrop-filter: blur(10px);
                transition: opacity 0.3s ease-in-out;
            }}
            
            .loading-overlay.hidden {{
                opacity: 0;
                pointer-events: none;
            }}
            
            .spinner {{
                width: 40px;
                height: 40px;
                border: 3px solid rgba(255, 255, 255, 0.1);
                border-top-color: #ffffff;
                border-radius: 50%;
                animation: spin 1s linear infinite;
            }}
            
            @keyframes spin {{
                to {{ transform: rotate(360deg); }}
            }}
            
            .info-panel {{
                display: flex;
                gap: 24px;
                align-items: center;
                padding: 16px 24px;
                background: rgba(255, 255, 255, 0.05);
                backdrop-filter: blur(20px);
                border-radius: 16px;
                border: 1px solid rgba(255, 255, 255, 0.1);
                font-size: 14px;
            }}
            
            .info-item {{
                display: flex;
                flex-direction: column;
                gap: 4px;
            }}
            
            .info-label {{
                font-size: 11px;
                font-weight: 500;
                text-transform: uppercase;
                letter-spacing: 0.5px;
                color: rgba(255, 255, 255, 0.5);
            }}
            
            .info-value {{
                font-size: 17px;
                font-weight: 600;
                color: #ffffff;
                font-variant-numeric: tabular-nums;
            }}
            
            .device-id {{
                font-family: "SF Mono", Monaco, "Cascadia Code", "Roboto Mono", monospace;
                font-size: 13px;
                color: rgba(255, 255, 255, 0.7);
            }}
            
            @media (max-width: 768px) {{
                body {{
                    padding: 24px 16px;
                }}
                
                .header h1 {{
                    font-size: 24px;
                }}
                
                .header-content {{
                    gap: 8px;
                }}
                
                .status-indicator {{
                    font-size: 11px;
                    padding: 5px 10px;
                }}
                
                .video-wrapper {{
                    border-radius: 16px;
                }}
                
                .info-panel {{
                    flex-direction: column;
                    align-items: flex-start;
                    gap: 16px;
                    width: 100%;
                }}
            }}
        </style>
    </head>
    <body>
        <div class="container">
            <div class="header">
                <div class="header-content">
                    <h1>实时视频流</h1>
                    <div id="status" class="status-indicator disconnected">
                        <span class="status-dot"></span>
                        <span id="statusText">连接中...</span>
                    </div>
                </div>
                <div class="subtitle">Live Stream</div>
            </div>
            
            <div class="video-wrapper">
                <div id="loadingOverlay" class="loading-overlay">
                    <div class="spinner"></div>
                </div>
                <img id="videoFrame" alt="视频流" />
            </div>
            
            <div class="info-panel">
                <div class="info-item">
                    <div class="info-label">设备</div>
                    <div class="info-value device-id">{device_id}</div>
                </div>
                <div class="info-item">
                    <div class="info-label">帧率</div>
                    <div class="info-value"><span id="fps">0</span> FPS</div>
                </div>
            </div>
        </div>
        <script>
            const wsUrl = '{ws_url}';
            const videoFrame = document.getElementById('videoFrame');
            const statusIndicator = document.getElementById('status');
            const statusText = document.getElementById('statusText');
            const loadingOverlay = document.getElementById('loadingOverlay');
            const fpsSpan = document.getElementById('fps');
            
            let frameCount = 0;
            let lastFpsUpdate = Date.now();
            let ws = null;
            let previousBlobUrl = null;
            
            function updateStatus(text, connected) {{
                statusText.textContent = text;
                statusIndicator.className = 'status-indicator ' + (connected ? 'connected' : 'disconnected');
            }}
            
            function hideLoading() {{
                loadingOverlay.classList.add('hidden');
                videoFrame.classList.add('loaded');
            }}
            
            function connect() {{
                ws = new WebSocket(wsUrl);
                
                ws.onopen = function() {{
                    updateStatus('已连接', true);
                    hideLoading();
                    frameCount = 0;
                    lastFpsUpdate = Date.now();
                }};
                
                ws.onmessage = function(event) {{
                    let blob = null;
                    if (event.data instanceof Blob) {{
                        blob = event.data;
                    }} else if (event.data instanceof ArrayBuffer) {{
                        blob = new Blob([event.data], {{type: 'image/jpeg'}});
                    }} else {{
                        return;
                    }}
                    
                    const newBlobUrl = URL.createObjectURL(blob);
                    
                    videoFrame.onload = function() {{
                        hideLoading();
                    }};
                    
                    videoFrame.src = newBlobUrl;
                    
                    if (previousBlobUrl) {{
                        setTimeout(function() {{
                            try {{
                                URL.revokeObjectURL(previousBlobUrl);
                            }} catch(e) {{
                                console.warn('释放 Blob URL 失败:', e);
                            }}
                        }}, 100);
                    }}
                    
                    previousBlobUrl = newBlobUrl;
                    
                    frameCount++;
                    const now = Date.now();
                    if (now - lastFpsUpdate >= 1000) {{
                        fpsSpan.textContent = frameCount;
                        frameCount = 0;
                        lastFpsUpdate = now;
                    }}
                }};
                
                ws.onerror = function(error) {{
                    console.error('WebSocket 错误:', error);
                    updateStatus('连接错误', false);
                }};
                
                ws.onclose = function() {{
                    updateStatus('已断开，正在重连...', false);
                    loadingOverlay.classList.remove('hidden');
                    if (previousBlobUrl) {{
                        try {{
                            URL.revokeObjectURL(previousBlobUrl);
                        }} catch(e) {{
                            console.warn('清理 Blob URL 失败:', e);
                        }}
                        previousBlobUrl = null;
                    }}
                    setTimeout(connect, 3000);
                }};
            }}
            
            connect();
        </script>
    </body>
    </html>
    """
    return html


@app.get("/api/streams")
async def list_streams():
    """获取所有活跃的视频流"""
    return {
        'success': True,
        'active_devices': list(device_streams.keys()),
        'client_counts': {device_id: len(clients) for device_id, clients in client_streams.items()}
    }


@app.on_event("startup")
async def startup_event():
    host = CONFIG['HOST']
    port = CONFIG['PORT']
    base_url = f"http://{host if host != '0.0.0.0' else 'localhost'}:{port}"
    
    print("=" * 60)
    print("ESP32-CAM 云服务器")
    print("=" * 60)
    print(f"监听地址: {host}:{port}")
    print(f"上传目录: {upload_dir.absolute()}")
    print(f"API密钥: {CONFIG['API_KEY']}")
    print(f"最大文件大小: {CONFIG['MAX_FILE_SIZE'] / 1024 / 1024} MB")
    print("=" * 60)
    print(f"API文档: {base_url}/docs")
    print(f"上传接口: POST {base_url}/api/upload")
    print(f"视频流查看: {base_url}/stream/{{device_id}}")
    print(f"视频流WebSocket: ws://{host if host != '0.0.0.0' else 'localhost'}:{port}/ws/stream/{{device_id}}\n")


if __name__ == '__main__':
    import uvicorn
    uvicorn.run(app, host=CONFIG['HOST'], port=CONFIG['PORT'], log_level="info")

