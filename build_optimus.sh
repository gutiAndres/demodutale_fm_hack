#!/usr/bin/env bash
set -euo pipefail

# ========= Config =========
OUT="dual_optimus_demod_psd"

# Ajusta aquí el nombre del main que estás usando:
# - Si ya renombraste el dual a main_dual_demod_psd.c, ponlo aquí.
# - Si estás probando dentro de main_demod.c, deja main_demod.c.
MAIN_C="main_optimus_demod.c"

# Include dir
INC="-I./libs"

# Sources
SRCS=(
  "$MAIN_C"
  "./libs/rb_sig.c"
  "./libs/ring_buffer.c"
  "./libs/fm_demod.c"
  "./libs/am_demod.c"
  "./libs/opus_tx.c"
  "./libs/psd.c"
  "./libs/sdr_HAL.c"
  "./libs/pipeline_threads.c"
  "./libs/cic_decim.c"
  
)

# Toolchain flags
CFLAGS="-O2 -Wall -Wextra -pthread"
LDFLAGS="-lm"

# Required pkg-config modules (Debian names: libhackrf, opus, fftw3, cjson)
PKGS=(libhackrf opus fftw3 libcjson)

# ========= Helpers =========
need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "[ERROR] Missing command: $1" >&2
    exit 1
  }
}

# ========= Checks =========
need_cmd gcc
need_cmd pkg-config

if [[ ! -f "$MAIN_C" ]]; then
  echo "[ERROR] MAIN_C not found: $MAIN_C" >&2
  echo "        Edit build.sh and set MAIN_C to the correct file." >&2
  exit 1
fi

for s in "${SRCS[@]}"; do
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
echo "[BUILD] Output: $OUT"
echo "[BUILD] Main:   $MAIN_C"
echo "[BUILD] CFLAGS: $CFLAGS $INC $PC_CFLAGS"
echo "[BUILD] LIBS:   $PC_LIBS $LDFLAGS"

gcc $CFLAGS $INC $PC_CFLAGS \
  "${SRCS[@]}" \
  -o "$OUT" \
  $PC_LIBS $LDFLAGS

echo "[OK] Built ./$OUT"
echo
echo "Run:"
echo "  ./$OUT"
