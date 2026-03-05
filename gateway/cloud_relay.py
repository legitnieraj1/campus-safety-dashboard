"""
SafetyHub Cloud Relay  —  cloud_relay.py
─────────────────────────────────────────────────────────────
Deployed on Railway / Render.  Receives sensor data POSTed
by the laptop gateway and serves it to the Vercel dashboard.

Endpoints:
    POST /ingest         ←  laptop gateway pushes sensor JSON here
    GET  /sensor-data    →  latest sensor snapshot (JSON)
    GET  /health         →  relay + data-source status
    GET  /history        →  last N readings (default 50)
    WS   /ws             →  real-time WebSocket stream to dashboard

Run locally:
    uvicorn cloud_relay:app --host 0.0.0.0 --port 8000
"""

import asyncio
import json
import logging
import time
from datetime import datetime, timezone
from typing import Optional

import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from pydantic import BaseModel

# ── Config ───────────────────────────────────────────────────
HISTORY_MAX     = 100          # rolling window of readings
STALE_TIMEOUT   = 15           # seconds before data is considered stale
LOG_LEVEL       = logging.INFO

# ── Logging ──────────────────────────────────────────────────
logging.basicConfig(
    level=LOG_LEVEL,
    format="%(asctime)s  %(levelname)-7s  %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("cloud_relay")

# ── State ────────────────────────────────────────────────────
class RelayState:
    def __init__(self):
        self.latest: Optional[dict]      = None
        self.source_online: bool         = False
        self.last_received: Optional[str] = None
        self.last_received_ts: float     = 0
        self.history: list[dict]         = []
        self.total_ingested: int         = 0

state = RelayState()

# ── WebSocket connection manager ─────────────────────────────
class ConnectionManager:
    def __init__(self):
        self.active: list[WebSocket] = []

    async def connect(self, ws: WebSocket):
        await ws.accept()
        self.active.append(ws)
        log.info("WebSocket client connected (%d total)", len(self.active))

    def disconnect(self, ws: WebSocket):
        self.active.remove(ws)
        log.info("WebSocket client disconnected (%d remaining)", len(self.active))

    async def broadcast(self, data: dict):
        """Send data to all connected WebSocket clients."""
        if not self.active:
            return
        message = json.dumps(data)
        stale = []
        for ws in self.active:
            try:
                await ws.send_text(message)
            except Exception:
                stale.append(ws)
        for ws in stale:
            self.active.remove(ws)

manager = ConnectionManager()

# ── Pydantic model for incoming data ────────────────────────
class SensorPayload(BaseModel):
    temperature: float
    humidity: float
    gasLevel: int
    vibration: float
    alert: str = "System Normal"
    timestamp: Optional[str] = None
    source: Optional[str] = None

# ── FastAPI app ──────────────────────────────────────────────
app = FastAPI(
    title="SafetyHub Cloud Relay",
    description="Receives sensor data from laptop gateway, serves to public dashboard via HTTP + WebSocket",
    version="2.0.0",
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],          # Vercel dashboard can reach us
    allow_methods=["GET", "POST"],
    allow_headers=["*"],
)

# ── Endpoints ────────────────────────────────────────────────

@app.post("/ingest", summary="Receive sensor data from laptop gateway")
async def ingest(payload: SensorPayload):
    """
    Laptop gateway POSTs sensor JSON here every 2 seconds.
    Immediately broadcasts to all WebSocket clients.
    """
    ts = datetime.now(timezone.utc).isoformat()
    data = payload.model_dump()
    data["timestamp"] = data.get("timestamp") or ts
    data["source"] = data.get("source") or "esp32"
    data["received_at"] = ts

    state.latest            = data
    state.source_online     = True
    state.last_received     = ts
    state.last_received_ts  = time.time()
    state.total_ingested   += 1

    # Rolling history
    state.history.append(data)
    if len(state.history) > HISTORY_MAX:
        state.history.pop(0)

    log.debug(
        "INGESTED | temp=%.1f  hum=%.1f  gas=%d  vib=%.3f  alert=%s",
        data["temperature"], data["humidity"],
        data["gasLevel"],    data["vibration"], data["alert"],
    )

    # ── Broadcast to all WebSocket clients instantly ─────────
    await manager.broadcast(data)

    return {
        "status": "ok",
        "total_ingested": state.total_ingested,
        "ws_clients": len(manager.active),
    }


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    """
    Dashboard connects here for real-time sensor updates.
    On connect: sends the latest reading immediately (if available).
    Then: receives broadcasts whenever /ingest is called.
    """
    await manager.connect(ws)

    # Send latest data immediately so dashboard doesn't show blank
    if state.latest:
        try:
            await ws.send_text(json.dumps(state.latest))
        except Exception:
            pass

    try:
        # Keep connection alive — listen for client pings/messages
        while True:
            await ws.receive_text()  # blocks until client sends or disconnects
    except WebSocketDisconnect:
        manager.disconnect(ws)


@app.get("/sensor-data", summary="Latest sensor snapshot (HTTP fallback)")
async def get_sensor_data():
    """
    Returns the most recent sensor reading.
    Used as fallback when WebSocket is unavailable.
    """
    if state.latest is None:
        return JSONResponse(
            status_code=503,
            content={
                "error":  "No Data",
                "detail": "No sensor data received yet. Is the laptop gateway running?",
            },
        )

    age = time.time() - state.last_received_ts
    is_stale = age > STALE_TIMEOUT

    response = {**state.latest, "stale": is_stale, "age_seconds": round(age, 1)}

    if is_stale:
        response["warning"] = f"Data is {round(age)}s old — gateway may be offline"

    return response


@app.get("/health", summary="Relay health status")
async def health():
    age = time.time() - state.last_received_ts if state.last_received_ts > 0 else None
    return {
        "relay":             "online",
        "source":            "online" if (state.source_online and age and age < STALE_TIMEOUT) else "offline",
        "last_received":     state.last_received,
        "age_seconds":       round(age, 1) if age else None,
        "total_ingested":    state.total_ingested,
        "history_count":     len(state.history),
        "ws_clients":        len(manager.active),
    }


@app.get("/history", summary="Rolling history of sensor readings")
async def get_history(limit: int = 50):
    """Returns the last `limit` sensor readings (max 100)."""
    limit = min(limit, HISTORY_MAX)
    return {
        "count":    min(limit, len(state.history)),
        "readings": state.history[-limit:],
    }


# ── Entry point ──────────────────────────────────────────────
if __name__ == "__main__":
    import os
    port = int(os.environ.get("PORT", 8000))
    uvicorn.run(
        "cloud_relay:app",
        host="0.0.0.0",
        port=port,
        reload=False,
        log_level="info",
    )
