#!/usr/bin/env python3
import asyncio
import json
import threading
import websockets

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
gi.require_version("GstSdp", "1.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gst, GstWebRTC, GstSdp, GLib

SENSOR_ID   = "d8:3a:dd:f7:1a:cc"
SIGNAL_URL  = f"wss://rsm.ane.gov.co:12443/ws/signal/{SENSOR_ID}"
STUN_SERVER = "stun://stun.l.google.com:19302"

PT = 96

Gst.init(None)

PIPELINE_DESC = f"""
webrtcbin name=wb bundle-policy=max-bundle stun-server="{STUN_SERVER}"
audiotestsrc is-live=true wave=sine freq=1000 !
  audioconvert ! audioresample !
  opusenc bitrate=24000 !
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
    want_prefix = ("m=audio", "a=rtpmap", "a=fmtp", "a=ice-ufrag", "a=ice-pwd", "a=fingerprint", "a=setup")
    out = []
    for ln in sdp_text.splitlines():
        if ln.startswith(want_prefix):
            out.append(ln)
    return out[:40]

class Publisher:
    def __init__(self, loop, ws):
        self.loop = loop
        self.ws = ws
        self.cand_out = 0

        self.glib_loop = GLib.MainLoop()
        self.glib_thread = threading.Thread(target=self.glib_loop.run, daemon=True)

        self.pipe = Gst.parse_launch(PIPELINE_DESC)
        self.webrtc = self.pipe.get_by_name("wb")
        if not self.webrtc:
            raise RuntimeError("webrtcbin 'wb' not found")

        self.webrtc.connect("on-negotiation-needed", self.on_negotiation_needed)
        self.webrtc.connect("on-ice-candidate", self.on_ice_candidate)

        bus = self.pipe.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self.on_bus)

    def start(self):
        self.glib_thread.start()
        self.pipe.set_state(Gst.State.PLAYING)
        print("[SENSOR] Pipeline PLAYING")

    def on_bus(self, bus, msg):
        if msg.type == Gst.MessageType.ERROR:
            err, dbg = msg.parse_error()
            print("[GST][ERROR]", err, dbg)
        elif msg.type == Gst.MessageType.WARNING:
            err, dbg = msg.parse_warning()
            print("[GST][WARN]", err, dbg)

    def _ws_send(self, obj):
        asyncio.run_coroutine_threadsafe(self.ws.send(json.dumps(obj)), self.loop)

    def on_ice_candidate(self, element, mline, candidate):
        self.cand_out += 1
        if self.cand_out <= 3:
            print(f"[SENSOR] local candidate #{self.cand_out} mline={mline} head='{candidate[:60]}'")
        self._ws_send({"type":"candidate","mlineindex":int(mline),"candidate":candidate})

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

        print("[SENSOR][DBG] offer is None?:", offer is None)
        if offer is None or getattr(offer, "sdp", None) is None:
            print("[SENSOR][FATAL] offer or offer.sdp is NULL -> cannot continue")
            return

        sdp_text = sdp_to_text(offer.sdp)
        first = sdp_text.splitlines()[0] if sdp_text else "<empty>"
        print(f"[SENSOR][DBG] SDP len={len(sdp_text)} first='{first}'")

        # MUST contain an audio m-line, otherwise browser will not receive a track
        if "m=audio" not in sdp_text:
            print("[SENSOR][FATAL] SDP has no 'm=audio' line. Not sending.")
            print("[SENSOR][DBG] SDP key lines:", *sdp_extract_lines(sdp_text), sep="\n  ")
            return

        print("[SENSOR][DBG] SDP key lines:", *sdp_extract_lines(sdp_text), sep="\n  ")

        # now set-local + send
        element.emit("set-local-description", offer, Gst.Promise.new())
        print("[SENSOR] sending OFFER")
        self._ws_send({"type":"offer","sdp":sdp_text})

    # -------- signaling -> webrtcbin --------
    def set_answer(self, sdp_text):
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

async def main():
    async with websockets.connect(SIGNAL_URL, open_timeout=20, max_size=None) as ws:
        await ws.send(json.dumps({"role":"sensor","sensor_id":SENSOR_ID}))
        loop = asyncio.get_running_loop()

        pub = Publisher(loop, ws)
        pub.start()

        async for msg in ws:
            obj = json.loads(msg)
            if obj.get("type") == "answer":
                pub.set_answer(obj["sdp"])
            elif obj.get("type") == "candidate":
                pub.add_candidate(obj["mlineindex"], obj["candidate"])

if __name__ == "__main__":
    asyncio.run(main())
