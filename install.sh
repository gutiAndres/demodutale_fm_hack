#!/usr/bin/env bash
set -euo pipefail

echo "========================================"
echo " WebRTC Sensor – Dependency Installer"
echo " Raspberry Pi OS / Debian"
echo "========================================"

# -----------------------------
# 1) Update system
# -----------------------------
echo "[1/6] Updating system..."
sudo apt update
sudo apt -y upgrade

# -----------------------------
# 2) Install system packages
# -----------------------------
echo "[2/6] Installing system dependencies..."

sudo apt install -y \
  python3 python3-venv python3-pip python3-gi \
  gir1.2-gstreamer-1.0 \
  gir1.2-gst-plugins-base-1.0 \
  gir1.2-gst-plugins-bad-1.0 \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav \
  gstreamer1.0-alsa \
  ca-certificates

# -----------------------------
# 3) Create Python venv
# -----------------------------
echo "[3/6] Creating Python virtual environment..."

if [ ! -d "venv" ]; then
  python3 -m venv --system-site-packages venv
fi

# shellcheck disable=SC1091
source venv/bin/activate

# -----------------------------
# 4) Install Python packages
# -----------------------------
echo "[4/6] Installing Python packages..."

python -m pip install --upgrade pip
python -m pip install websockets==15.0.1

# -----------------------------
# 5) Sanity checks (GStreamer)
# -----------------------------
echo "[5/6] Verifying GStreamer plugins..."

gst-inspect-1.0 webrtcbin >/dev/null || {
  echo "[ERROR] webrtcbin NOT found. gstreamer1.0-plugins-bad is missing."
  exit 1
}

gst-inspect-1.0 opusparse >/dev/null || {
  echo "[ERROR] opusparse NOT found."
  exit 1
}

gst-inspect-1.0 rtpopuspay >/dev/null || {
  echo "[ERROR] rtpopuspay NOT found."
  exit 1
}

# -----------------------------
# 6) Sanity checks (Python GI)
# -----------------------------
echo "[6/6] Verifying Python GI bindings..."

python - <<'EOF'
import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
gi.require_version("GstSdp", "1.0")
gi.require_version("GLib", "2.0")

from gi.repository import Gst, GstWebRTC, GstSdp, GLib
Gst.init(None)

print("✔ Python GI OK")
print("✔ GStreamer version:", Gst.version_string())
print("✔ GstWebRTC available")
EOF

echo "========================================"
echo " Installation completed successfully"
echo " Activate env with: source venv/bin/activate"
echo " Run your code with: python3 sensor_webrtc_publisher.py"
echo "========================================"
