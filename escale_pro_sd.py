import numpy as np
import csv
import time
from pathlib import Path
from typing import Tuple


# ============================================================
# 1) I/O PSD
# ============================================================

def load_psd_csv(csv_path: str | Path) -> Tuple[np.ndarray, np.ndarray]:
    """
    Lee un CSV con columnas: freq_hz, psd_dBm
    Retorna freqs_hz, psd_dBm (ordenados por frecuencia).
    """
    csv_path = Path(csv_path)
    if not csv_path.exists():
        raise FileNotFoundError(csv_path)

    freqs, psd = [], []
    with csv_path.open("r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            raise ValueError("CSV sin encabezados. Se esperan: freq_hz, psd_dBm")
        if "freq_hz" not in reader.fieldnames or "psd_dBm" not in reader.fieldnames:
            raise ValueError(f"Columnas inválidas. Encontré: {reader.fieldnames}")

        for row in reader:
            freqs.append(float(row["freq_hz"]))
            psd.append(float(row["psd_dBm"]))

    freqs = np.asarray(freqs, dtype=float)
    psd = np.asarray(psd, dtype=float)

    order = np.argsort(freqs)
    return freqs[order], psd[order]


def atomic_save_psd_csv(csv_path: str | Path,
                        freqs_hz: np.ndarray,
                        psd_dbm: np.ndarray) -> None:
    """
    Guardado atómico: escribe a .tmp y luego reemplaza.
    Evita que otro proceso lea un archivo a medio escribir.
    """
    csv_path = Path(csv_path)
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = csv_path.with_suffix(csv_path.suffix + ".tmp")

    with tmp_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["freq_hz", "psd_dBm"])
        writer.writeheader()
        for fhz, p in zip(freqs_hz, psd_dbm):
            writer.writerow({"freq_hz": float(fhz), "psd_dBm": float(p)})

    tmp_path.replace(csv_path)


# ============================================================
# 2) Piso de ruido (histograma sobre percentil bajo)
# ============================================================

def detect_noise_floor_from_psd(psd_db: np.ndarray,
                                delta_dB: float = 0.5,
                                noise_percentile: float = 50.0) -> float:
    x = np.asarray(psd_db, dtype=float)

    thr = np.percentile(x, noise_percentile)
    x_low = x[x <= thr]
    if x_low.size < 10:
        return float(np.median(x))

    x_min, x_max = float(np.min(x_low)), float(np.max(x_low))
    nbins = max(10, int(np.ceil((x_max - x_min) / max(delta_dB, 1e-6))))

    counts, edges = np.histogram(x_low, bins=nbins, range=(x_min, x_max))
    idx = int(np.argmax(counts))
    return float(0.5 * (edges[idx] + edges[idx + 1]))


# ============================================================
# 3) Detección de canales (centros) por umbral NF + delta
# ============================================================

def _fill_small_gaps(mask: np.ndarray, max_gap: int) -> np.ndarray:
    if max_gap <= 0:
        return mask

    mask = mask.copy()
    i, N = 0, mask.size
    while i < N:
        if mask[i]:
            i += 1
            continue

        g0 = i
        while i < N and not mask[i]:
            i += 1
        g1 = i

        if (g1 - g0) <= max_gap:
            if g0 > 0 and g1 < N and mask[g0 - 1] and mask[g1]:
                mask[g0:g1] = True
    return mask


def detect_channels_from_psd(freqs_hz: np.ndarray,
                             psd_db: np.ndarray,
                             noise_floor_db: float,
                             delta_above_nf_db: float,
                             gap_fill_bins: int = 1) -> Tuple[np.ndarray, np.ndarray]:
    """
    Devuelve:
      - centers_hz: centroides ponderados por potencia lineal
      - mask: bins detectados como emisión
    """
    f = np.asarray(freqs_hz, dtype=float)
    xdb = np.asarray(psd_db, dtype=float)

    thr = noise_floor_db + delta_above_nf_db
    mask = xdb > thr
    mask = _fill_small_gaps(mask, gap_fill_bins)

    idx = np.where(mask)[0]
    if idx.size == 0:
        return np.array([], dtype=float), mask

    segments = []
    s0 = idx[0]
    prev = idx[0]
    for k in idx[1:]:
        if k == prev + 1:
            prev = k
        else:
            segments.append((s0, prev))
            s0 = k
            prev = k
    segments.append((s0, prev))

    P_lin = 10.0 ** (xdb / 10.0)

    centers = []
    for a, b in segments:
        w = P_lin[a:b + 1]
        ff = f[a:b + 1]
        centers.append(float(np.sum(ff * w) / (np.sum(w) + 1e-30)))

    return np.asarray(centers, dtype=float), mask


# ============================================================
# 4) Reconstrucción PSD (FM-like BW fijo 200 kHz) + ruido
#    con "calibración" negativa (restar dB)
# ============================================================

def reconstruct_psd_from_centers(freqs_hz: np.ndarray,
                                 # NF estimado (sobre PSD original)
                                 noise_floor_db: float,
                                 centers_hz: np.ndarray,
                                 psd_original_db: np.ndarray,
                                 # --- emisión FM-like ---
                                 emission_bw_hz: float = 200e3,
                                 level_method: str = "p95",
                                 shape: str = "raised_cosine",
                                 # --- ruido "rugosidad" ---
                                 noise_sigma_db: float = 1.0,
                                 noise_corr_bw_hz: float = 30e3,
                                 seed: int | None = None,
                                 # --- NUEVO: factores de calibración (dB) ---
                                 calib_emissions_db: float = 0.0,
                                 calib_noise_floor_db: float = 0.0) -> np.ndarray:
    """
    Reconstruye PSD en dB:
      - base = (noise_floor_db - calib_noise_floor_db)
      - emisiones: nivel estimado desde PSD original, pero ajustado:
            level_db_recon = level_db_original - calib_emissions_db

    Nota: "calib_*_db" son factores en dB que se RESTAN para bajar potencia.
    """
    f = np.asarray(freqs_hz, dtype=float)
    xdb = np.asarray(psd_original_db, dtype=float)
    centers = np.asarray(centers_hz, dtype=float)

    # Aplicar calibración al piso de ruido (bajar)
    nf_recon_db = float(noise_floor_db) - float(calib_noise_floor_db)

    base_lin = 10.0 ** (nf_recon_db / 10.0)
    recon_lin = np.full_like(f, base_lin, dtype=float)

    half = emission_bw_hz / 2.0

    for fc in centers:
        sel = (f >= fc - half) & (f <= fc + half)
        if not np.any(sel):
            continue

        xs = xdb[sel]
        if level_method == "max":
            level_db_orig = float(np.max(xs))
        elif level_method == "mean":
            level_db_orig = float(np.mean(xs))
        else:
            level_db_orig = float(np.percentile(xs, 95.0))

        # Aplicar calibración a emisiones (bajar)
        level_db_recon = level_db_orig - float(calib_emissions_db)
        level_lin = 10.0 ** (level_db_recon / 10.0)

        if shape == "flat":
            recon_lin[sel] += level_lin
        else:
            t = (f[sel] - fc) / half  # [-1, 1]
            w = 0.5 * (1.0 + np.cos(np.pi * t))  # 1 en centro, 0 en bordes
            recon_lin[sel] += level_lin * w

    recon_db = 10.0 * np.log10(recon_lin + 1e-30)

    # Ruido en dB (rugosidad)
    if noise_sigma_db > 0.0:
        rng = np.random.default_rng(seed)
        noise_db = rng.normal(0.0, noise_sigma_db, size=recon_db.size)

        if noise_corr_bw_hz is not None and noise_corr_bw_hz > 0.0 and recon_db.size > 3:
            df = float(np.median(np.diff(f)))
            if df > 0:
                win = max(1, int(np.round(noise_corr_bw_hz / df)))
                if win > 1:
                    kernel = np.ones(win, dtype=float) / float(win)
                    noise_db = np.convolve(noise_db, kernel, mode="same")

        recon_db = recon_db + noise_db

    return recon_db


# ============================================================
# 5) Loop cíclico (1 Hz) sobrescribiendo static/last_psd.csv
# ============================================================

def main_loop():
    # IMPORTANTE:
    # - Si quieres leer la PSD REAL desde otro path (recomendado), cambia INPUT_CSV.
    # - OUTPUT_CSV SIEMPRE será static/last_psd.csv (reconstruida).
    INPUT_CSV = Path("static2/last_psd.csv")   # PSD real (recomendado)
    OUTPUT_CSV = Path("static/last_psd.csv")   # PSD reconstruida (se sobrescribe)

    # --- Parámetros detección ---
    NOISE_FLOOR_DELTA_DB = 0.5
    NOISE_PERCENTILE = 50.0
    DELTA_ABOVE_NF_DB = 15.0
    GAP_FILL_BINS = 1

    # --- Parámetros reconstrucción (FM-like) ---
    EMISSION_BW_HZ = 200e3
    LEVEL_METHOD = "p95"
    SHAPE = "raised_cosine"

    # --- Rugosidad de PSD reconstruida ---
    NOISE_SIGMA_DB = 1.0
    NOISE_CORR_BW_HZ = 30e3

    # ======================================================
    # NUEVO: Calibración (dB) para BAJAR potencia
    # ======================================================
    # Ejemplo: si pones 3.0 => bajas 3 dB respecto a la PSD original.
    CALIB_EMISSIONS_DB = 50.0      # se RESTA a cada emisión (baja señales)
    CALIB_NOISE_FLOOR_DB = 30.0    # se RESTA al piso (baja NF)

    print("[INFO] Loop 1 Hz: reconstrucción con calibración negativa (restar dB)")
    print(f"[INFO] INPUT={INPUT_CSV} | OUTPUT={OUTPUT_CSV}")
    print(f"[INFO] calib_emissions_db={CALIB_EMISSIONS_DB} dB | calib_noise_floor_db={CALIB_NOISE_FLOOR_DB} dB")

    while True:
        t0 = time.time()

        try:
            f, psd_dbm = load_psd_csv(INPUT_CSV)

            nf = detect_noise_floor_from_psd(
                psd_dbm,
                delta_dB=NOISE_FLOOR_DELTA_DB,
                noise_percentile=NOISE_PERCENTILE
            )

            centers, _ = detect_channels_from_psd(
                freqs_hz=f,
                psd_db=psd_dbm,
                noise_floor_db=nf,
                delta_above_nf_db=DELTA_ABOVE_NF_DB,
                gap_fill_bins=GAP_FILL_BINS
            )

            psd_recon = reconstruct_psd_from_centers(
                freqs_hz=f,
                noise_floor_db=nf,
                centers_hz=centers,
                psd_original_db=psd_dbm,
                emission_bw_hz=EMISSION_BW_HZ,
                level_method=LEVEL_METHOD,
                shape=SHAPE,
                noise_sigma_db=NOISE_SIGMA_DB,
                noise_corr_bw_hz=NOISE_CORR_BW_HZ,
                seed=None,  # None => cambia cada iteración
                calib_emissions_db=CALIB_EMISSIONS_DB,
                calib_noise_floor_db=CALIB_NOISE_FLOOR_DB
            )

            atomic_save_psd_csv(OUTPUT_CSV, f, psd_recon)

            print(f"[OK] nf_orig={nf:.2f} dB | nf_recon={nf - CALIB_NOISE_FLOOR_DB:.2f} dB | "
                  f"emisiones={len(centers)} | actualizado={OUTPUT_CSV}")

        except Exception as e:
            print(f"[ERROR] {e}")

        elapsed = time.time() - t0
        time.sleep(max(0.0, 1.0 - elapsed))


if __name__ == "__main__":
    main_loop()
