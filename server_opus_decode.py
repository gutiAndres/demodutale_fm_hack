from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from typing import Dict, Set
import struct
import time
import opuslib

app = FastAPI()

listeners: Dict[str, Set[WebSocket]] = {}
last_config: Dict[str, str] = {}

# Opus frame header: magic 'OPU0'
OPUS_HDR_FMT = "!IIIHH"
OPUS_HDR_SIZE = struct.calcsize(OPUS_HDR_FMT)
OPUS_MAGIC = 0x4F505530  # 'OPU0'

# PCM frame header: magic 'AUD0'
PCM_MAGIC = 0x41554430   # 'AUD0'
PCM_HDR_FMT = "!IIIHH"
PCM_HDR_SIZE = struct.calcsize(PCM_HDR_FMT)

SAMPLE_RATE = 48000
CHANNELS = 1
FRAME_SAMPLES = 960  # 20 ms @ 48k

decoders: Dict[str, opuslib.Decoder] = {}

metrics = {}

@app.get("/")
def health():
    return {"ok": True, "ws_ingest": "/ws/sensor/{sensor_id}", "ws_listen": "/ws/listen/{sensor_id}"}

def mget(sensor_id: str):
    if sensor_id not in metrics:
        metrics[sensor_id] = {"in": 0, "inB": 0, "out": 0, "outB": 0, "t0": time.time()}
    return metrics[sensor_id]

def maybe_log(sensor_id: str):
    m = mget(sensor_id)
    now = time.time()
    dt = now - m["t0"]
    if dt >= 1.0:
        li = len(listeners.get(sensor_id, set()))
        print(f"[SV][{sensor_id}] IN {m['in']}/s {(m['inB']/1024):.1f} KiB/s | OUT {m['out']}/s {(m['outB']/1024):.1f} KiB/s | listeners={li}")
        m["in"]=m["inB"]=m["out"]=m["outB"]=0
        m["t0"]=now

def build_pcm_frame(seq: int, pcm_bytes: bytes) -> bytes:
    # pcm_bytes = 960 * 2 = 1920 bytes
    hdr = struct.pack(
        PCM_HDR_FMT,
        PCM_MAGIC,
        seq,
        SAMPLE_RATE,
        CHANNELS,
        FRAME_SAMPLES
    )
    return hdr + pcm_bytes

@app.websocket("/ws/sensor/{sensor_id}")
async def ws_sensor_ingest(ws: WebSocket, sensor_id: str):
    await ws.accept()
    print(f"[SV] Sensor conectado: {sensor_id}")

    try:
        cfg = await ws.receive_text()
        last_config[sensor_id] = cfg
        print(f"[SV] Config {sensor_id}: {cfg}")

        # crear decoder opus para este sensor
        decoders[sensor_id] = opuslib.Decoder(SAMPLE_RATE, CHANNELS)

        # mandar config a listeners existentes
        for client in list(listeners.get(sensor_id, set())):
            try:
                await client.send_text(cfg.replace('"codec":"opus"', '"codec":"pcm_s16le"'))
            except:
                listeners[sensor_id].discard(client)

        while True:
            msg = await ws.receive()
            data = msg.get("bytes")
            if not data:
                continue

            m = mget(sensor_id)
            m["in"] += 1
            m["inB"] += len(data)

            if len(data) < OPUS_HDR_SIZE:
                continue

            magic, seq, sr, ch, plen = struct.unpack(OPUS_HDR_FMT, data[:OPUS_HDR_SIZE])
            if magic != OPUS_MAGIC:
                continue

            packet = data[OPUS_HDR_SIZE:OPUS_HDR_SIZE+plen]
            if len(packet) != plen:
                continue

            # decode -> PCM s16le bytes (1920 bytes)
            dec = decoders[sensor_id]
            pcm_bytes = dec.decode(packet, FRAME_SAMPLES, decode_fec=False)

            pcm_frame = build_pcm_frame(seq, pcm_bytes)

            dead = []
            for client in list(listeners.get(sensor_id, set())):
                try:
                    await client.send_bytes(pcm_frame)
                    m["out"] += 1
                    m["outB"] += len(pcm_frame)
                except:
                    dead.append(client)

            for d in dead:
                listeners[sensor_id].discard(d)

            maybe_log(sensor_id)

    except WebSocketDisconnect:
        print(f"[SV] Sensor desconectado: {sensor_id}")
    finally:
        decoders.pop(sensor_id, None)

@app.websocket("/ws/listen/{sensor_id}")
async def ws_listen(ws: WebSocket, sensor_id: str):
    await ws.accept()
    listeners.setdefault(sensor_id, set()).add(ws)
    print(f"[SV] Listener conectado a {sensor_id}. Total={len(listeners[sensor_id])}")

    # enviar config guardada (ajustada a pcm) al listener nuevo
    cfg = last_config.get(sensor_id)
    if cfg:
        try:
            await ws.send_text(cfg.replace('"codec":"opus"', '"codec":"pcm_s16le"'))
        except:
            pass

    try:
        while True:
            await ws.receive_text()  # ping
    except WebSocketDisconnect:
        pass
    finally:
        listeners[sensor_id].discard(ws)
        print(f"[SV] Listener salió de {sensor_id}. Total={len(listeners[sensor_id])}")
