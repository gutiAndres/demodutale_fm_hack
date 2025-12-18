import asyncio
import struct
import time
import json
import websockets

TCP_HOST = "0.0.0.0"
TCP_PORT = 9000

SENSOR_ID = "d8:3a:dd:f7:1a:cc"

# --- WS endpoints ---
SERVER_WS_AUDIO   = f"wss://rsm.ane.gov.co:12443/ws/audio/sensor/{SENSOR_ID}"
SERVER_WS_METRICS = f"wss://rsm.ane.gov.co:12443/ws/audio/sensor/{SENSOR_ID}"

# --- Audio framing (sensor -> this gateway) ---
HDR_FMT = "!IIIHH"  # magic, seq, sample_rate, channels, payload_len
HDR_SIZE = struct.calcsize(HDR_FMT)
MAGIC = 0x4F505530  # 'OPU0'

# --- Metrics JSON expected by server ---
# You can update these at runtime if your demod changes frequency/modulation, etc.
METRICS = {
    "codec": "opus",
    "sample_rate": 48000,
    "channels": 1,
    "frequency": 98500000,
    "modulation": "FM"
}

# Optional: initial config string you were already sending to audio WS
CFG_TEXT = json.dumps({
    "sensor_id": SENSOR_ID,
    "codec": "opus",
    "sample_rate": METRICS["sample_rate"],
    "channels": METRICS["channels"],
    "frame_ms": 20
})

# How often to push metrics (seconds)
METRICS_PERIOD_S = 1.0


async def ws_connect_with_retry(url: str, *, send_text: str | None = None):
    """Connects to a WebSocket with infinite retry; optionally sends a first text message."""
    while True:
        try:
            ws = await websockets.connect(url, max_size=None)
            if send_text is not None:
                await ws.send(send_text)
            print(f"[PY] WS conectado a {url}")
            return ws
        except Exception as e:
            print(f"[PY] No pude conectar WS a {url} ({e}). Reintentando en 2s...")
            await asyncio.sleep(2)


async def ensure_ws_send_json(ws, url: str, payload: dict):
    """
    Sends JSON over a WS. If it fails, reconnects and retries once.
    Returns a possibly new ws instance.
    """
    msg = json.dumps(payload, separators=(",", ":"), ensure_ascii=False)
    try:
        await ws.send(msg)
        return ws
    except Exception as e:
        print(f"[PY] WS metrics cayó ({e}). Reconectando...")
        try:
            await ws.close()
        except:
            pass
        ws = await ws_connect_with_retry(url)
        # retry once after reconnect
        try:
            await ws.send(msg)
        except Exception as e2:
            print(f"[PY] No pude enviar métricas tras reconectar ({e2}).")
        return ws


async def ensure_ws_send_binary(ws, url: str, data: bytes, *, first_text_on_connect: str | None = None):
    """
    Sends binary over a WS. If it fails, reconnects and retries once.
    Returns a possibly new ws instance.
    """
    try:
        await ws.send(data)
        return ws
    except Exception as e:
        print(f"[PY] WS audio cayó ({e}). Reconectando...")
        try:
            await ws.close()
        except:
            pass
        ws = await ws_connect_with_retry(url, send_text=first_text_on_connect)
        # retry once after reconnect
        try:
            await ws.send(data)
        except Exception as e2:
            print(f"[PY] No pude enviar audio tras reconectar ({e2}).")
        return ws


async def metrics_publisher(stop_evt: asyncio.Event, ws_metrics, metrics_url: str):
    """
    Periodically publishes metrics JSON to metrics_url until stop_evt is set.
    Sends immediately once at start.
    """
    # Send once immediately
    ws_metrics = await ensure_ws_send_json(ws_metrics, metrics_url, METRICS)

    last_sent = None
    while not stop_evt.is_set():
        # If you want: only send if changed
        # current = json.dumps(METRICS, sort_keys=True)
        # if current != last_sent:
        #     ws_metrics = await ensure_ws_send_json(ws_metrics, metrics_url, METRICS)
        #     last_sent = current

        # Simpler: send periodically
        ws_metrics = await ensure_ws_send_json(ws_metrics, metrics_url, METRICS)

        try:
            await asyncio.wait_for(stop_evt.wait(), timeout=METRICS_PERIOD_S)
        except asyncio.TimeoutError:
            pass

    # best-effort close
    try:
        await ws_metrics.close()
    except:
        pass


async def handle_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    peer = writer.get_extra_info("peername")
    print(f"[PY] C conectado desde {peer}")

    # Create both WS connections
    ws_audio = await ws_connect_with_retry(SERVER_WS_AUDIO, send_text=CFG_TEXT)
    ws_metrics = await ws_connect_with_retry(SERVER_WS_METRICS)

    # Start metrics task
    stop_evt = asyncio.Event()
    metrics_task = asyncio.create_task(metrics_publisher(stop_evt, ws_metrics, SERVER_WS_METRICS))

    frames = 0
    bytes_ = 0
    t0 = time.time()
    last_seq = None

    try:
        while True:
            hdr = await reader.readexactly(HDR_SIZE)
            magic, seq, sr, ch, plen = struct.unpack(HDR_FMT, hdr)

            if magic != MAGIC:
                print("[PY] Magic inválido, cerrando.")
                break

            payload = await reader.readexactly(plen)

            if last_seq is not None and seq != (last_seq + 1):
                print(f"[PY] WARNING: salto de seq {last_seq} -> {seq}")
            last_seq = seq

            # (Optional) Keep METRICS aligned with what sensor is sending
            # Only update if different, to avoid churn
            if (METRICS.get("sample_rate") != sr) or (METRICS.get("channels") != ch):
                METRICS["sample_rate"] = int(sr)
                METRICS["channels"] = int(ch)
                # If your server expects "codec" always opus and modulation/frequency stable, keep them.

            # Send audio as binary to audio WS
            ws_audio = await ensure_ws_send_binary(ws_audio, SERVER_WS_AUDIO, hdr + payload, first_text_on_connect=CFG_TEXT)

            frames += 1
            bytes_ += (HDR_SIZE + plen)
            now = time.time()
            if now - t0 >= 1.0:
                print(f"[PY] RX {frames}/s {(bytes_/1024):.1f} KiB/s (last_seq={seq})")
                frames = 0
                bytes_ = 0
                t0 = now

    except asyncio.IncompleteReadError:
        print("[PY] C desconectado.")
    finally:
        # Stop metrics loop
        stop_evt.set()
        try:
            await metrics_task
        except:
            pass

        # Close audio WS
        try:
            await ws_audio.close()
        except:
            pass

        writer.close()
        await writer.wait_closed()
        print("[PY] Sesión cerrada.")


async def main():
    server = await asyncio.start_server(handle_client, TCP_HOST, TCP_PORT)
    print(f"[PY] Escuchando TCP en {TCP_HOST}:{TCP_PORT}")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
