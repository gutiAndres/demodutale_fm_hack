#!/usr/bin/env bash
set -euo pipefail

echo "=============================================="
echo " WebRTC Opus Publisher – system install script"
echo "=============================================="

# ---------- sanity ----------
if [[ $EUID -ne 0 ]]; then
  echo "[ERROR] Run this script with sudo"
  exit 1
fi

echo "[INFO] Updating system..."
apt update

# ---------- base ----------
echo "[INFO] Installing base packages..."
apt install -y \
  ca-certificates \
  curl \
  wget \
  git \
  build-essential \
  pkg-config \
  software-properties-common

# ---------- python ----------
echo "[INFO] Installing Python runtime..."
apt install -y \
  python3 \
  python3-pip \
  python3-venv \
  python3-gi \
  python3-gi-cairo

# ---------- gstreamer core ----------
echo "[INFO] Installing GStreamer core..."
apt install -y \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav

# ---------- WebRTC / ICE / Opus ----------
echo "[INFO] Installing WebRTC / Opus / ICE dependencies..."
apt install -y \
  gstreamer1.0-nice \
  gstreamer1.0-opus \
  libnice10 \
  libnice-dev \
  libsrtp2-1 \
  libssl3

# ---------- WebRTC GIR bindings ----------
echo "[INFO] Installing GstWebRTC GIR bindings..."
apt install -y \
  gir1.2-gst-webrtc-1.0 \
  gir1.2-gst-sdp-1.0

# ---------- Python deps ----------
echo "[INFO] Installing Python packages..."
python3 -m pip install --upgrade pip
python3 -m pip install \
  websockets

# ---------- validation ----------
echo "=============================================="
echo " Validating installation"
echo "=============================================="

echo "[CHECK] Python GI..."
python3 - <<'EOF'
import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
from gi.repository import Gst
Gst.init(None)
print("  OK: gi + Gst + GstWebRTC")
EOF

echo "[CHECK] webrtcbin..."
if gst-inspect-1.0 webrtcbin > /dev/null 2>&1; then
  echo "  OK: webrtcbin present"
else
  echo "  ERROR: webrtcbin NOT found"
  exit 1
fi

echo "[CHECK] opusenc..."
if gst-inspect-1.0 opusenc > /dev/null 2>&1; then
  echo "  OK: opusenc present"
else
  echo "  ERROR: opusenc NOT found"
  exit 1
fi

echo "[CHECK] rtpopuspay..."
if gst-inspect-1.0 rtpopuspay > /dev/null 2>&1; then
  echo "  OK: rtpopuspay present"
else
  echo "  ERROR: rtpopuspay NOT found"
  exit 1
fi

echo "=============================================="
echo " INSTALLATION COMPLETE"
echo "=============================================="
echo
echo "You can now run:"
echo "  python3 sensor_webrtc_publisher.py"
echo
