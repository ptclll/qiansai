"""
FastAPI WebSocket Server — ESP32 Serial Bridge
===============================================
Routes:
  GET  /              → static/index.html
  WS   /ws/esp32      → ESP32 WebSocket
  WS   /ws/frontend   → Browser WebSocket

Data flow:
  ESP32 UART1 RX  →  /ws/esp32  →  broadcast /ws/frontend
  Browser input   →  /ws/frontend →  /ws/esp32  →  ESP32 UART1 TX
"""

import asyncio
import logging
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
import uvicorn

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("esp32-bridge")

app = FastAPI(title="ESP32 Serial Bridge")

# ── Connection Manager ────────────────────────────────────────
class ConnectionManager:
    def __init__(self):
        self.esp32: WebSocket | None = None
        self.frontends: set[WebSocket] = set()
        self._lock = asyncio.Lock()

    async def esp32_connect(self, ws: WebSocket):
        await ws.accept()
        async with self._lock:
            self.esp32 = ws
        logger.info("ESP32 connected")

    async def esp32_disconnect(self):
        async with self._lock:
            self.esp32 = None
        logger.info("ESP32 disconnected")

    async def frontend_connect(self, ws: WebSocket):
        await ws.accept()
        async with self._lock:
            self.frontends.add(ws)
        logger.info(f"Frontend connected (total {len(self.frontends)})")

    async def frontend_disconnect(self, ws: WebSocket):
        async with self._lock:
            self.frontends.discard(ws)
        logger.info(f"Frontend disconnected (total {len(self.frontends)})")

    async def send_to_esp32(self, text: str) -> bool:
        async with self._lock:
            ws = self.esp32
        if ws is None:
            return False
        try:
            await ws.send_text(text)
            return True
        except Exception:
            await self.esp32_disconnect()
            return False

    async def broadcast_to_frontends(self, text: str):
        dead: set[WebSocket] = set()
        async with self._lock:
            fds = self.frontends.copy()
        for ws in fds:
            try:
                await ws.send_text(text)
            except Exception:
                dead.add(ws)
        if dead:
            async with self._lock:
                self.frontends -= dead

manager = ConnectionManager()

# ── WebSocket Endpoints ───────────────────────────────────────
@app.websocket("/ws/esp32")
async def ws_esp32(websocket: WebSocket):
    """ESP32 sends BINARY frames. receive() returns ASGI dict."""
    await manager.esp32_connect(websocket)
    try:
        while True:
            msg = await websocket.receive()
            if msg["type"] == "websocket.disconnect":
                break
            if "text" in msg:
                text = msg["text"]
            elif "bytes" in msg:
                text = msg["bytes"].decode("utf-8", errors="replace")
            else:
                continue
            await manager.broadcast_to_frontends(text)
    except WebSocketDisconnect:
        pass
    except Exception as e:
        logger.error(f"ESP32 WS error: {e}")
    finally:
        await manager.esp32_disconnect()

@app.websocket("/ws/frontend")
async def ws_frontend(websocket: WebSocket):
    """Browser sends text frames, ESP32 expects binary frames."""
    await manager.frontend_connect(websocket)
    try:
        while True:
            data = await websocket.receive_text()
            ok = await manager.send_to_esp32(data)
            if not ok:
                await websocket.send_text("[SYS] ESP32 not connected")
    except WebSocketDisconnect:
        pass
    except Exception as e:
        logger.error(f"Frontend WS error: {e}")
    finally:
        await manager.frontend_disconnect(websocket)

# ── Serve static frontend ─────────────────────────────────────
@app.get("/")
async def root():
    return FileResponse("static/index.html")

# ── Health check ──────────────────────────────────────────────
@app.get("/health")
async def health():
    return {
        "esp32_connected": manager.esp32 is not None,
        "frontend_count": len(manager.frontends),
    }

# ── Main ──────────────────────────────────────────────────────
if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8010, log_level="info")
