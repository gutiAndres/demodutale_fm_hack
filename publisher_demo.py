#!/usr/bin/env python3
import asyncio
import json
import websockets
from websockets.exceptions import InvalidStatusCode, InvalidHandshake

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
gi.require_version("GstSdp", "1.0")
from gi.repository import Gst, GstWebRTC, GstSdp

Gst.init(None)

# =========================
# Config
# =========================
SENSOR_ID = "d8:3a:dd:f7:1a:cc"
SIGNAL_URL = f"wss://rsm.ane.gov.co:12443/ws/signal/{SENSOR_ID}"
STUN = "stun://stun.l.google.com:19302"
RETRY_SECONDS = 2

# =========================
# GStreamer pipeline
# Fix: add clock-rate=48000 (OPUS) + queue before webrtcbin
# =========================
PIPELINE_DESC = f"""
webrtcbin name=webrtc bundle-policy=max-bundle stun-server="{STUN}"
audiotestsrc is-live=true wave=sine freq=1000 !
  audioconvert ! audioresample !
  opusenc bitrate=24000 !
  rtpopuspay pt=96 !
  queue !
  application/x-rtp,media=audio,encoding-name=OPUS,payload=96,clock-rate=48000 !
  webrtc.
"""

class Pub:
    def __init__(self, ws, loop: asyncio.AbstractEventLoop):
        self.ws = ws
        self.loop = loop

        self.pipe = Gst.parse_launch(PIPELINE_DESC)
        self.webrtc = self.pipe.get_by_name("webrtc")
        if not self.webrtc:
            raise RuntimeError("webrtcbin element not found in pipeline")

        self.webrtc.connect("on-negotiation-needed", self.on_negotiation_needed)
        self.webrtc.connect("on-ice-candidate", self.on_ice_candidate)

    def start(self):
        self.pipe.set_state(Gst.State.PLAYING)

    def stop(self):
        try:
            self.pipe.set_state(Gst.State.NULL)
        except Exception:
            pass

    def on_ice_candidate(self, element, mlineindex, candidate):
        async def _send():
            await self.ws.send(json.dumps({
                "type": "candidate",
                "mlineindex": int(mlineindex),
                "candidate": str(candidate),
            }))
        self.loop.create_task(_send())

    def on_negotiation_needed(self, element):
        promise = Gst.Promise.new_with_change_func(self.on_offer_created, element, None)
        element.emit("create-offer", None, promise)

    def on_offer_created(self, promise, element, _):
        reply = promise.get_reply()
        offer = reply.get_value("offer")
        element.emit("set-local-description", offer, Gst.Promise.new())
        sdp_text = offer.sdp.as_text()

        async def _send():
            await self.ws.send(json.dumps({"type": "offer", "sdp": sdp_text}))
        self.loop.create_task(_send())

    def set_remote_answer(self, sdp_text: str):
        res, sdpmsg = GstSdp.SDPMessage.new()
        if res != GstSdp.SDPResult.OK:
            raise RuntimeError("Failed to allocate SDPMessage")

        res = GstSdp.sdp_message_parse_buffer(sdp_text.encode("utf-8"), sdpmsg)
        if res != GstSdp.SDPResult.OK:
            raise RuntimeError("Failed to parse remote SDP answer")

        ans = GstWebRTC.WebRTCSessionDescription.new(
            GstWebRTC.WebRTCSDPType.ANSWER, sdpmsg
        )
        self.webrtc.emit("set-remote-description", ans, Gst.Promise.new())

    def add_ice_candidate(self, mlineindex, candidate):
        self.webrtc.emit("add-ice-candidate", int(mlineindex), str(candidate))


async def run_once():
    loop = asyncio.get_running_loop()

    async with websockets.connect(SIGNAL_URL) as ws:
        await ws.send(json.dumps({"role": "sensor"}))

        pub = Pub(ws, loop)
        pub.start()

        try:
            async for msg in ws:
                obj = json.loads(msg)
                t = obj.get("type")

                if t == "answer":
                    pub.set_remote_answer(obj["sdp"])
                elif t == "candidate":
                    pub.add_ice_candidate(obj["mlineindex"], obj["candidate"])
        finally:
            pub.stop()


async def main():
    while True:
        try:
            await run_once()
            print("[GW] Disconnected. Reconnecting...")
        except InvalidStatusCode as e:
            print(f"[GW] WebSocket rejected (HTTP {e.status_code}). Retrying in {RETRY_SECONDS}s...")
        except (InvalidHandshake, OSError, asyncio.TimeoutError) as e:
            print(f"[GW] Connection error: {type(e).__name__}: {e}. Retrying in {RETRY_SECONDS}s...")
        except Exception as e:
            # Aquí cae también gst_parse_error (parse_launch / link)
            print(f"[GW] Unexpected error: {type(e).__name__}: {e}. Retrying in {RETRY_SECONDS}s...")

        await asyncio.sleep(RETRY_SECONDS)


if __name__ == "__main__":
    asyncio.run(main())
