#!/usr/bin/env python3
import asyncio
import json
import threading
import struct
import traceback
import time

import websockets
from websockets.exceptions import InvalidStatusCode, InvalidHandshake, ConnectionClosed

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
gi.require_version("GstSdp", "1.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gst, GstWebRTC, GstSdp, GLib

# =========================
# Config
# =========================
SENSOR_ID   = "d8:3a:dd:f7:1a:cc"
SIGNAL_URL  = f"wss://rsm.ane.gov.co:12443/ws/signal/{SENSOR_ID}"
STUN_SERVER = "stun://stun.l.google.com:19302"

# TCP donde el motor C conecta y envía Opus frames
TCP_HOST = "0.0.0.0"
TCP_PORT = 8000

# RTP payload type
PT = 96

# Header del motor C: (magic, seq, sample_rate, channels, payload_len)
HDR_FMT  = "!IIIHH"
HDR_SIZE = struct.calcsize(HDR_FMT)
MAGIC    = 0x4F505530  # 'OPU0'

# Si el motor manda 20ms por frame (típico Opus)
DEFAULT_FRAME_MS = 20

RETRY_SECONDS = 2

# Cola global que desacopla TCP (siempre arriba) de WebRTC (puede reconectar)
# maxsize ~ 200 frames = ~4s si frame_ms=20
OPUS_QUEUE_MAX = 200
opus_q: asyncio.Queue[bytes] = asyncio.Queue(maxsize=OPUS_QUEUE_MAX)

# Límite razonable de tamaño de frame Opus (evita desincronización por plen corrupto)
MAX_OPUS_FRAME_BYTES = 4096

Gst.init(None)

# =========================
# Pipeline: appsrc -> opusparse -> rtpopuspay -> webrtcbin
# Nota: ponemos do-timestamp=false y manejamos PTS/DUR nosotros, determinista.
# =========================
PIPELINE_DESC = f"""
webrtcbin name=wb bundle-policy=max-bundle stun-server="{STUN_SERVER}"

appsrc name=opussrc is-live=true format=time do-timestamp=false !
  queue !
  opusparse !
  rtpopuspay pt={PT} !
  application/x-rtp,media=audio,encoding-name=OPUS,payload={PT},clock-rate=48000 !
  queue !
  wb.
"""


def sdp_to_text(sdp_msg) -> str:
    try:
        return sdp_msg.as_text()
    except Exception:
        return sdp_msg.to_string()


def sdp_extract_lines(sdp_text: str):
    want_prefix = (
        "m=audio", "a=rtpmap", "a=fmtp",
        "a=ice-ufrag", "a=ice-pwd",
        "a=fingerprint", "a=setup"
    )
    out = []
    for ln in sdp_text.splitlines():
        if ln.startswith(want_prefix):
            out.append(ln)
    return out[:40]


class Publisher:
    """
    Publisher WebRTC que:
    - negocia SDP/ICE via WebSocket
    - recibe Opus frames por push_opus_frame() y los inyecta en appsrc
    """
    def __init__(self, loop: asyncio.AbstractEventLoop, ws):
        self.loop = loop
        self.ws = ws
        self.cand_out = 0

        self.glib_loop = GLib.MainLoop()
        self.glib_thread = threading.Thread(target=self.glib_loop.run, daemon=True)

        self.pipe = Gst.parse_launch(PIPELINE_DESC)

        self.webrtc = self.pipe.get_by_name("wb")
        if not self.webrtc:
            raise RuntimeError("webrtcbin 'wb' not found")

        self.appsrc = self.pipe.get_by_name("opussrc")
        if not self.appsrc:
            raise RuntimeError("appsrc 'opussrc' not found")

        # Caps para Opus "raw" (paquetes Opus) que vienen del motor C
        caps = Gst.Caps.from_string("audio/x-opus, rate=48000, channels=1")
        self.appsrc.set_property("caps", caps)

        self.webrtc.connect("on-negotiation-needed", self.on_negotiation_needed)
        self.webrtc.connect("on-ice-candidate", self.on_ice_candidate)

        bus = self.pipe.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self.on_bus)

        self._running = False
        self._pts = 0  # ns

    def start(self):
        if not self.glib_thread.is_alive():
            self.glib_thread.start()
        self.pipe.set_state(Gst.State.PLAYING)
        self._running = True
        self._pts = 0
        print("[SENSOR] Pipeline PLAYING (Opus over TCP -> WebRTC)")

    def stop(self):
        self._running = False
        try:
            self.pipe.set_state(Gst.State.NULL)
        except Exception:
            pass
        try:
            if self.glib_loop.is_running():
                self.glib_loop.quit()
        except Exception:
            pass

    def on_bus(self, bus, msg):
        if msg.type == Gst.MessageType.ERROR:
            err, dbg = msg.parse_error()
            print("[GST][ERROR]", err, dbg)
        elif msg.type == Gst.MessageType.WARNING:
            err, dbg = msg.parse_warning()
            print("[GST][WARN]", err, dbg)

    def _ws_send(self, obj):
        try:
            asyncio.run_coroutine_threadsafe(self.ws.send(json.dumps(obj)), self.loop)
        except Exception:
            pass

    def on_ice_candidate(self, element, mline, candidate):
        self.cand_out += 1
        if self.cand_out <= 3:
            print(f"[SENSOR] local candidate #{self.cand_out} mline={mline} head='{candidate[:60]}'")
        self._ws_send({"type": "candidate", "mlineindex": int(mline), "candidate": candidate})

    def on_negotiation_needed(self, element):
        print("[SENSOR] negotiation needed")
        promise = Gst.Promise.new_with_change_func(self.on_offer_created, element, None)
        element.emit("create-offer", None, promise)

    def on_offer_created(self, promise, element, _):
        reply = promise.get_reply()
        try:
            print("[SENSOR][DBG] create-offer reply:", reply.to_string())
        except Exception:
            pass

        offer = None
        if reply and reply.has_field("offer"):
            offer = reply.get_value("offer")

        if offer is None or getattr(offer, "sdp", None) is None:
            print("[SENSOR][FATAL] offer or offer.sdp is NULL -> cannot continue")
            return

        sdp_text = sdp_to_text(offer.sdp)
        if "m=audio" not in sdp_text:
            print("[SENSOR][FATAL] SDP has no 'm=audio' line. Not sending.")
            print("[SENSOR][DBG] SDP key lines:", *sdp_extract_lines(sdp_text), sep="\n  ")
            return

        print("[SENSOR][DBG] SDP key lines:", *sdp_extract_lines(sdp_text), sep="\n  ")

        element.emit("set-local-description", offer, Gst.Promise.new())
        print("[SENSOR] sending OFFER")
        self._ws_send({"type": "offer", "sdp": sdp_text})

    def set_answer(self, sdp_text: str):
        def _do():
            res, sdp = GstSdp.sdp_message_new()
            if res != GstSdp.SDPResult.OK:
                print("[SENSOR][ERR] sdp_message_new failed:", res)
                return False

            ret = GstSdp.sdp_message_parse_buffer(sdp_text.encode("utf-8"), sdp)
            if ret != GstSdp.SDPResult.OK:
                print("[SENSOR][ERR] SDP parse failed:", ret)
                return False

            ans = GstWebRTC.WebRTCSessionDescription.new(GstWebRTC.WebRTCSDPType.ANSWER, sdp)
            self.webrtc.emit("set-remote-description", ans, Gst.Promise.new())
            print("[SENSOR] answer set")
            return False

        GLib.idle_add(_do)

    def add_candidate(self, mline, cand):
        def _do():
            self.webrtc.emit("add-ice-candidate", int(mline), cand)
            return False
        GLib.idle_add(_do)

    def push_opus_frame(self, opus_bytes: bytes, frame_ms: int = DEFAULT_FRAME_MS):
        """
        Inyecta un frame Opus (payload) en appsrc.
        Thread-safe: se agenda con GLib.idle_add().
        """
        if not self._running:
            return

        dur_ns = int(frame_ms * 1e6)

        def _do():
            if not self._running:
                return False

            buf = Gst.Buffer.new_allocate(None, len(opus_bytes), None)
            buf.fill(0, opus_bytes)

            buf.pts = self._pts
            buf.dts = self._pts
            buf.duration = dur_ns
            self._pts += dur_ns

            ret = self.appsrc.emit("push-buffer", buf)
            if ret != Gst.FlowReturn.OK:
                print("[SENSOR][WARN] push-buffer flow:", ret)
            return False

        GLib.idle_add(_do)


# =========================
# TCP server (persistente)
# =========================
async def tcp_server_forever():
    """
    Servidor TCP persistente (SIEMPRE arriba) que recibe frames del motor C:
      hdr (OPU0 + seq + sr + ch + plen) + payload(opus)
    y los pone en la cola global opus_q.
    """

    async def handle_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        peer = writer.get_extra_info("peername")
        print(f"[TCP] Motor C conectado desde {peer}")

        frames = 0
        bytes_ = 0
        t0 = time.time()
        last_seq = None

        try:
            while True:
                hdr = await reader.readexactly(HDR_SIZE)
                magic, seq, sr, ch, plen = struct.unpack(HDR_FMT, hdr)

                if magic != MAGIC:
                    print("[TCP] Magic inválido (no es OPU0). Cerrando.")
                    break

                if plen <= 0 or plen > MAX_OPUS_FRAME_BYTES:
                    print(f"[TCP] plen inválido={plen}. Cerrando (posible desincronización).")
                    break

                payload = await reader.readexactly(plen)

                if last_seq is not None and seq != (last_seq + 1):
                    print(f"[TCP] WARNING: salto de seq {last_seq} -> {seq}")
                last_seq = seq

                # Backpressure: si la cola está llena, descartamos el más viejo
                if opus_q.full():
                    try:
                        _ = opus_q.get_nowait()
                    except asyncio.QueueEmpty:
                        pass

                await opus_q.put(payload)

                frames += 1
                bytes_ += (HDR_SIZE + plen)
                now = time.time()
                if now - t0 >= 1.0:
                    print(f"[TCP] RX {frames}/s {(bytes_/1024):.1f} KiB/s (last_seq={seq})")
                    frames = 0
                    bytes_ = 0
                    t0 = now

        except asyncio.IncompleteReadError:
            print("[TCP] Motor C desconectado.")
        except Exception as e:
            print("[TCP] Error:", e)
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass
            print("[TCP] Sesión TCP cerrada.")

    server = await asyncio.start_server(handle_client, TCP_HOST, TCP_PORT)
    print(f"[TCP] Escuchando motor C en {TCP_HOST}:{TCP_PORT}")

    try:
        async with server:
            await server.serve_forever()
    finally:
        server.close()
        await server.wait_closed()


# =========================
# Pump: cola TCP -> Publisher (por sesión WebRTC)
# =========================
async def pump_queue_to_publisher(pub: Publisher):
    """
    Consume frames Opus desde opus_q y los inyecta en el Publisher actual.
    Si WebRTC cae y se recrea Publisher, este task se cancela y se crea otro.
    """
    while True:
        payload = await opus_q.get()
        pub.push_opus_frame(payload, frame_ms=DEFAULT_FRAME_MS)


# =========================
# WebRTC session
# =========================
async def run_one_session():
    async with websockets.connect(SIGNAL_URL, open_timeout=20, max_size=None) as ws:
        await ws.send(json.dumps({"role": "sensor", "sensor_id": SENSOR_ID}))
        print("[GW] Connected to signaling server")

        loop = asyncio.get_running_loop()
        pub = Publisher(loop, ws)
        pub.start()

        pump_task = asyncio.create_task(pump_queue_to_publisher(pub))

        try:
            async for msg in ws:
                obj = json.loads(msg)
                if obj.get("type") == "answer":
                    pub.set_answer(obj["sdp"])
                elif obj.get("type") == "candidate":
                    pub.add_candidate(obj["mlineindex"], obj["candidate"])
        finally:
            pump_task.cancel()
            try:
                await pump_task
            except Exception:
                pass
            pub.stop()


# =========================
# Main
# =========================
async def main():
    # TCP server siempre arriba, independiente de WebRTC reconexiones
    tcp_task = asyncio.create_task(tcp_server_forever())

    while True:
        try:
            await run_one_session()

        except InvalidStatusCode as e:
            print(f"[GW] WS rejected: HTTP {e.status_code}. Retrying in {RETRY_SECONDS}s...")

        except (InvalidHandshake,) as e:
            print(f"[GW] WS handshake failed: {e}. Retrying in {RETRY_SECONDS}s...")

        except ConnectionClosed as e:
            print(f"[GW] WS closed: {e}. Retrying in {RETRY_SECONDS}s...")

        except OSError as e:
            print(f"[GW] Network error: {e}. Retrying in {RETRY_SECONDS}s...")

        except Exception as e:
            print(f"[GW] Unexpected error: {e}\n{traceback.format_exc()}\nRetrying in {RETRY_SECONDS}s...")

        await asyncio.sleep(RETRY_SECONDS)


if __name__ == "__main__":
    asyncio.run(main())
