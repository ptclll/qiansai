"""
地图标点工具 + 图片 — FastAPI 服务
==================================
Routes:
  GET  /                  → 交互地图页面
  GET  /imagic            → 图片显示页面
  GET  /api/location      → 默认坐标 JSON
  POST /api/upload_image  → 上传图片（替换旧图）
  GET  /api/current_image → 当前图片信息
  WS   /ws/image          → WebSocket 接收图片（ESP32 二进制帧）
"""

import os
import io
import json
import shutil
import asyncio
import logging
from fastapi import FastAPI, UploadFile, File, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from PIL import Image

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("map-server")

app = FastAPI(title="地图标点工具 + 图片", version="3.0.0")

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
STATIC_DIR = os.path.join(BASE_DIR, "static")
IMAGES_DIR = os.path.join(STATIC_DIR, "images")

os.makedirs(IMAGES_DIR, exist_ok=True)

# 目标尺寸：1080p = 1920×1080 (16:9)
TARGET_W, TARGET_H = 1920, 1080
TARGET_RATIO = TARGET_W / TARGET_H  # ≈ 1.778
JPEG_QUALITY = 85


def process_image(data: bytes) -> bytes:
    """压缩+裁剪到 1920×1080，输出 JPEG"""
    img = Image.open(io.BytesIO(data))

    # 修正 EXIF 旋转
    try:
        from PIL import ImageOps
        img = ImageOps.exif_transpose(img)
    except Exception:
        pass

    # 转为 RGB（JPEG 不支持透明通道）
    if img.mode in ('RGBA', 'P', 'LA'):
        rgb = Image.new('RGB', img.size, (255, 255, 255))
        if img.mode == 'P':
            img = img.convert('RGBA')
        rgb.paste(img, mask=img.split()[-1] if img.mode == 'RGBA' else None)
        img = rgb
    elif img.mode != 'RGB':
        img = img.convert('RGB')

    w, h = img.size
    current_ratio = w / h

    # 如果宽高比偏离 16:9 超过 2%，先居中裁剪
    if abs(current_ratio - TARGET_RATIO) / TARGET_RATIO > 0.02:
        if current_ratio > TARGET_RATIO:
            new_w = int(h * TARGET_RATIO)
            left = (w - new_w) // 2
            img = img.crop((left, 0, left + new_w, h))
        else:
            new_h = int(w / TARGET_RATIO)
            top = (h - new_h) // 2
            img = img.crop((0, top, w, top + new_h))
        logger.info(f"[IMG] Cropped {w}x{h} → {img.size[0]}x{img.size[1]} (to 16:9)")

    # 如果比目标大，缩小
    w, h = img.size
    if w > TARGET_W or h > TARGET_H:
        img.thumbnail((TARGET_W, TARGET_H), Image.LANCZOS)
        logger.info(f"[IMG] Resized to {img.size[0]}x{img.size[1]}")

    buf = io.BytesIO()
    img.save(buf, format='JPEG', quality=JPEG_QUALITY, optimize=True)
    return buf.getvalue()


def save_current_image(data: bytes) -> str:
    """保存图片：压缩裁剪 → 删除旧图 → 写入新图，返回文件名"""
    processed = process_image(data)
    # 删除旧图片
    for f in os.listdir(IMAGES_DIR):
        os.remove(os.path.join(IMAGES_DIR, f))
    filename = "current.jpg"
    filepath = os.path.join(IMAGES_DIR, filename)
    with open(filepath, 'wb') as f:
        f.write(processed)
    logger.info(f"[IMG] Saved {filename} ({len(data)} → {len(processed)} bytes)")
    return filename


def current_image_info() -> dict:
    """返回当前图片信息"""
    images = [f for f in os.listdir(IMAGES_DIR)
              if not f.startswith('.')]
    if images:
        img = images[0]
        path = os.path.join(IMAGES_DIR, img)
        return {
            "image": img,
            "size": os.path.getsize(path),
            "mtime": os.path.getmtime(path),
        }
    return {"image": None, "size": 0, "mtime": 0}


# ── Pages ─────────────────────────────────────────────────────

@app.get("/")
async def root():
    """交互地图主页"""
    return FileResponse(os.path.join(STATIC_DIR, "index.html"),
                        headers={"Cache-Control": "no-cache, no-store, must-revalidate"})


@app.get("/imagic")
async def imagic_page():
    """图片显示页面"""
    return FileResponse(os.path.join(STATIC_DIR, "imagic.html"),
                        headers={"Cache-Control": "no-cache, no-store, must-revalidate"})


# ── APIs ──────────────────────────────────────────────────────

@app.get("/api/location")
async def get_default_location():
    return {
        "lng_dms": "112°28'41.4\"E",
        "lat_dms": "23°00'11.9\"N",
        "lng_dec": 112.478167,
        "lat_dec": 23.003306,
    }


@app.post("/api/upload_image")
async def upload_image(file: UploadFile = File(...)):
    """浏览器上传图片 → 替换旧图"""
    data = await file.read()
    filename = save_current_image(data)
    return {"ok": True, "filename": filename, "size": len(data)}


@app.get("/api/current_image")
async def api_current_image():
    """获取当前图片信息"""
    return current_image_info()


@app.get("/images/{filename}")
async def serve_image(filename: str):
    """提供图片文件访问"""
    from fastapi.responses import FileResponse as FR
    return FR(os.path.join(IMAGES_DIR, filename))


# ── WebSocket 图片接收（替代 imagic 的 UDP） ─────────────────

@app.websocket("/ws/image")
async def ws_image(websocket: WebSocket):
    """ESP32 通过 WebSocket 发送二进制图片帧 → 自动保存"""
    await websocket.accept()
    logger.info("[IMG-WS] Device connected")
    try:
        while True:
            msg = await websocket.receive()
            if msg["type"] == "websocket.disconnect":
                break
            if "bytes" in msg:
                data = msg["bytes"]
                filename = save_current_image(data)
                await websocket.send_text(json.dumps(
                    {"ok": True, "filename": filename, "size": len(data)}
                ))
            elif "text" in msg:
                # 也接受 base64 编码的图片
                import base64
                text = msg["text"]
                if text.startswith("data:image") or len(text) > 100:
                    try:
                        # base64 data URL 格式
                        if "," in text:
                            b64 = text.split(",", 1)[1]
                        else:
                            b64 = text
                        data = base64.b64decode(b64)
                        filename = save_current_image(data)
                        await websocket.send_text(json.dumps(
                            {"ok": True, "filename": filename, "size": len(data)}
                        ))
                    except Exception:
                        await websocket.send_text(json.dumps({"ok": False, "msg": "base64 decode failed"}))
    except WebSocketDisconnect:
        pass
    except Exception as e:
        logger.error(f"[IMG-WS] Error: {e}")
    finally:
        logger.info("[IMG-WS] Device disconnected")


# ── Main ──────────────────────────────────────────────────────

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=80)
