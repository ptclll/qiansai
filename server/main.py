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
import json
import logging
import math
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.staticfiles import StaticFiles
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
        """Send text frame to ESP32. Returns False if not connected."""
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
        """Send text frame to every frontend. Cleans up dead connections."""
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

# ── Azimuth computation ────────────────────────────────────────
def enrich_mag_json(text: str) -> str | None:
    """If text is a mag JSON lacking azimuth, compute it from mag_x/mag_y.

    When the ESP32 has already computed azimuth (Kalman-filtered on-device),
    the field is preserved as-is; this function only fills in the gap when
    azimuth is missing (e.g. from older firmware).
    """
    try:
        obj = json.loads(text)
    except (json.JSONDecodeError, TypeError):
        return None

    if obj.get("type") != "mag":
        return None

    # If ESP32 already sent azimuth (Kalman-filtered), pass through unchanged
    if "azimuth" in obj:
        return None   # no enrichment needed

    mag = obj.get("mag")
    if not isinstance(mag, list) or len(mag) < 2:
        return None

    x, y = mag[0], mag[1]

    # azimuth = atan2(y, x) in degrees, 0°=X+ axis, CCW positive
    azimuth_rad = math.atan2(y, x)
    azimuth_deg = math.degrees(azimuth_rad)

    # normalize to [0, 360)
    if azimuth_deg < 0:
        azimuth_deg += 360.0

    obj["azimuth"] = round(azimuth_deg, 2)
    return json.dumps(obj, separators=(",", ":"))


# ── WebSocket Endpoints ───────────────────────────────────────
@app.websocket("/ws/esp32")
async def ws_esp32(websocket: WebSocket):
    """ESP32 sends BINARY frames (transport_ws default). receive() returns ASGI dict."""
    await manager.esp32_connect(websocket)
    try:
        while True:
            msg = await websocket.receive()
            # receive() returns ASGI dict: {"type":"...", "text":"..."} or {"type":"...", "bytes":b"..."}
            if msg["type"] == "websocket.disconnect":
                break
            if "text" in msg:
                text = msg["text"]
            elif "bytes" in msg:
                text = msg["bytes"].decode("utf-8", errors="replace")
            else:
                continue

            # Enrich mag messages with azimuth, then forward
            enriched = enrich_mag_json(text)
            await manager.broadcast_to_frontends(enriched if enriched else text)
    except WebSocketDisconnect:
        pass
    except Exception as e:
        logger.error(f"ESP32 WS error: {e}")
    finally:
        await manager.esp32_disconnect()

@app.websocket("/ws/frontend")
async def ws_frontend(websocket: WebSocket):
    """Browser sends text frames, ESP32 expects binary frames via transport_ws."""
    await manager.frontend_connect(websocket)
    try:
        while True:
            data = await websocket.receive_text()
            # Forward frontend → ESP32 (send as text, FastAPI will encode)
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
