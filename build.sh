#!/usr/bin/env bash
set -euo pipefail

# ========= Config =========
OUT="dual_demod_psd_webrtc"

# Ajusta aquí el nombre del main que estás usando:
MAIN_C="main_demod.c"

# Include dir
INC="-I./libs"

# Sources C (compiladas con gcc)
SRCS_C=(
  "$MAIN_C"
  "./libs/rb_sig.c"
  "./libs/ring_buffer.c"
  "./libs/fm_demod.c"
  "./libs/am_demod.c"
  "./libs/psd.c"
  "./libs/sdr_HAL.c"
)

# Sources C++ (compiladas con g++)
SRCS_CPP=(
  "./libs/webrtc_opus_tx.cpp"
)

# Toolchain flags
CFLAGS="-O2 -Wall -Wextra -pthread"
CXXFLAGS="-O2 -Wall -Wextra -pthread -std=c++17"
LDFLAGS="-lm"

# Required pkg-config modules
PKGS=(libhackrf opus fftw3 libcjson)

# Extra libs for libdatachannel (link)
EXTRA_LIBS="-ldatachannel -lssl -lcrypto -ldl"

# ========= Helpers =========
need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "[ERROR] Missing command: $1" >&2
    exit 1
  }
}

# ========= Checks =========
need_cmd gcc
need_cmd g++
need_cmd pkg-config

if [[ ! -f "$MAIN_C" ]]; then
  echo "[ERROR] MAIN_C not found: $MAIN_C" >&2
  echo "        Edit build.sh and set MAIN_C to the correct file." >&2
  exit 1
fi

for s in "${SRCS_C[@]}"; do
  if [[ ! -f "$s" ]]; then
    echo "[ERROR] Source file not found: $s" >&2
    exit 1
  fi
done

for s in "${SRCS_CPP[@]}"; do
  if [[ ! -f "$s" ]]; then
    echo "[ERROR] Source file not found: $s" >&2
    exit 1
  fi
done

missing=0
for p in "${PKGS[@]}"; do
  if ! pkg-config --exists "$p"; then
    echo "[ERROR] pkg-config cannot find package: $p" >&2
    missing=1
  fi
done

# libdatachannel no siempre trae pkg-config en distros.
# Verificamos que el linker lo encuentre.
if ! (echo "int main(){return 0;}" | g++ -x c++ - -o /tmp/_ldtest $EXTRA_LIBS >/dev/null 2>&1); then
  echo "[ERROR] Linker cannot find libdatachannel and/or its dependencies." >&2
  echo "        Ensure libdatachannel is installed and discoverable by the linker." >&2
  echo "        Example (Debian/Ubuntu): install libdatachannel or build from source." >&2
  exit 1
fi
rm -f /tmp/_ldtest

if [[ $missing -ne 0 ]]; then
  cat >&2 <<'EOF'

Fix (Debian/Ubuntu):
  sudo apt update
  sudo apt install -y pkg-config libhackrf-dev libopus-dev libfftw3-dev libcjson-dev

Then re-run:
  ./build.sh
EOF
  exit 1
fi

PC_CFLAGS="$(pkg-config --cflags "${PKGS[@]}")"
PC_LIBS="$(pkg-config --libs "${PKGS[@]}")"

# ========= Build =========
BUILD_DIR="build"
mkdir -p "$BUILD_DIR"

echo "[BUILD] Output: $OUT"
echo "[BUILD] Main:   $MAIN_C"
echo "[BUILD] CFLAGS:   $CFLAGS $INC $PC_CFLAGS"
echo "[BUILD] CXXFLAGS: $CXXFLAGS $INC $PC_CFLAGS"
echo "[BUILD] LIBS:   $PC_LIBS $EXTRA_LIBS $LDFLAGS"

# Compile C objects
OBJS=()
for s in "${SRCS_C[@]}"; do
  o="$BUILD_DIR/$(basename "${s%.*}").o"
  echo "[CC]  $s -> $o"
  gcc $CFLAGS $INC $PC_CFLAGS -c "$s" -o "$o"
  OBJS+=("$o")
done

# Compile C++ objects
for s in "${SRCS_CPP[@]}"; do
  o="$BUILD_DIR/$(basename "${s%.*}").o"
  echo "[CXX] $s -> $o"
  g++ $CXXFLAGS $INC $PC_CFLAGS -c "$s" -o "$o"
  OBJS+=("$o")
done

# Link with g++ (important for C++)
echo "[LD]  ${OBJS[*]} -> $OUT"
g++ $CXXFLAGS "${OBJS[@]}" -o "$OUT" $PC_LIBS $EXTRA_LIBS $LDFLAGS

echo "[OK] Built ./$OUT"
echo
echo "Run:"
echo "  ./$OUT"
echo
echo "Receiver (Python aiortc):"
echo "  uvicorn webrtc_rx_server:app --host 0.0.0.0 --port 8008"
