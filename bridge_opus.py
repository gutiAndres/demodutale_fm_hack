import asyncio
import struct
import time
import websockets

TCP_HOST = "0.0.0.0"
TCP_PORT = 9000

SENSOR_ID = "ANE1"
SERVER_WS = f"ws://localhost:8000/ws/sensor/{SENSOR_ID}"

HDR_FMT = "!IIIHH"  # magic, seq, sample_rate, channels, payload_len
HDR_SIZE = struct.calcsize(HDR_FMT)
MAGIC = 0x4F505530  # 'OPU0'

CFG_TEXT = f'{{"sensor_id":"{SENSOR_ID}","codec":"opus","sample_rate":48000,"channels":1,"frame_ms":20}}'

async def ws_connect_with_retry():
    while True:
        try:
            ws = await websockets.connect(SERVER_WS, max_size=None)
            await ws.send(CFG_TEXT)
            print(f"[PY] WS conectado a {SERVER_WS}")
            return ws
        except Exception as e:
            print(f"[PY] No pude conectar WS ({e}). Reintentando en 2s...")
            await asyncio.sleep(2)

async def handle_client(reader, writer):
    peer = writer.get_extra_info("peername")
    print(f"[PY] C conectado desde {peer}")

    ws = await ws_connect_with_retry()

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

            try:
                await ws.send(hdr + payload)
            except Exception as e:
                print(f"[PY] WS cayó ({e}). Reconectando...")
                try:
                    await ws.close()
                except:
                    pass
                ws = await ws_connect_with_retry()

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
        try:
            await ws.close()
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
