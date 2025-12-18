#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-dev}"  # dev | release

APP_NAME="psd_acquire"
SRC_MAIN="rf_metrics.c"      # <-- AJUSTA AQUÍ (tu main real)
LIBS_DIR="./libs"
BUILD_DIR="./build"

mkdir -p "${BUILD_DIR}"

# -----------------------------
# Flags según modo
# -----------------------------
CSTD="-std=gnu11"
COMMON_CFLAGS="-Wall -Wextra -Wpedantic -pthread -fno-omit-frame-pointer"
if [[ "${MODE}" == "release" ]]; then
  OPTFLAGS="-O2 -DNDEBUG"
else
  OPTFLAGS="-O0 -g"
fi

CFLAGS="${CSTD} ${COMMON_CFLAGS} ${OPTFLAGS} -I${LIBS_DIR}"
LDFLAGS="-lm -pthread"

echo "[BUILD] mode=${MODE}"
echo "[BUILD] output=${BUILD_DIR}/${APP_NAME}"

# -----------------------------
# Dependencias vía pkg-config
# -----------------------------
need_pkg () {
  local pkg="$1"
  if ! pkg-config --exists "$pkg"; then
    echo "[ERROR] pkg-config no encuentra '$pkg'. Instala el -dev correspondiente."
    exit 1
  fi
}

need_pkg libhackrf
HACKRF_CFLAGS="$(pkg-config --cflags libhackrf)"
HACKRF_LIBS="$(pkg-config --libs libhackrf)"
echo "[BUILD] hackrf cflags=${HACKRF_CFLAGS}"
echo "[BUILD] hackrf libs=${HACKRF_LIBS}"

# PSD usa FFTW (por tus undefined refs a fftw_*)
# En Debian/Ubuntu lo normal es: pkg-config fftw3
need_pkg fftw3
FFTW_CFLAGS="$(pkg-config --cflags fftw3)"
FFTW_LIBS="$(pkg-config --libs fftw3)"

# cJSON: dependiendo tu sistema, puede ser "libcjson" o "cjson"
# probamos ambos; si ninguno existe, caemos a compilar cJSON.c si lo tienes local.
CJSON_CFLAGS=""
CJSON_LIBS=""
if pkg-config --exists libcjson; then
  CJSON_CFLAGS="$(pkg-config --cflags libcjson)"
  CJSON_LIBS="$(pkg-config --libs libcjson)"
elif pkg-config --exists cjson; then
  CJSON_CFLAGS="$(pkg-config --cflags cjson)"
  CJSON_LIBS="$(pkg-config --libs cjson)"
else
  # Fallback: si tienes cJSON.c en libs/, lo incluimos en la lista de fuentes
  echo "[WARN] No encontré cJSON por pkg-config. Intentaré compilar cJSON.c local si existe."
fi

# -----------------------------
# Lista blanca de fuentes en libs
# (incluye SOLO lo necesario para adquirir + PSD + CSV)
# -----------------------------
LIB_SOURCES=(
  "${LIBS_DIR}/psd.c"
  "${LIBS_DIR}/sdr_HAL.c"
  "${LIBS_DIR}/ring_buffer.c"
)

# Si tienes archivos adicionales requeridos por tu main/PSD (ej: datatypes helpers), agrégalos aquí.
# LIB_SOURCES+=( "${LIBS_DIR}/algo.c" )

# Si no hay cJSON en el sistema y existe cJSON.c local, incluirlo:
if [[ -z "${CJSON_LIBS}" && -f "${LIBS_DIR}/cJSON.c" ]]; then
  LIB_SOURCES+=( "${LIBS_DIR}/cJSON.c" )
fi

# Validaciones de existencia (para evitar "No such file or directory")
for f in "${SRC_MAIN}" "${LIB_SOURCES[@]}"; do
  if [[ ! -f "$f" ]]; then
    echo "[ERROR] Falta el archivo: $f"
    echo "        Ajusta SRC_MAIN o LIB_SOURCES en build_psd.sh"
    exit 1
  fi
done

# -----------------------------
# Compilar y linkear
# -----------------------------
gcc ${CFLAGS} ${HACKRF_CFLAGS} ${FFTW_CFLAGS} ${CJSON_CFLAGS} \
  "${SRC_MAIN}" "${LIB_SOURCES[@]}" \
  -o "${BUILD_DIR}/${APP_NAME}" \
  ${HACKRF_LIBS} ${FFTW_LIBS} ${CJSON_LIBS} ${LDFLAGS}

echo "[OK] Binario generado: ${BUILD_DIR}/${APP_NAME}"
