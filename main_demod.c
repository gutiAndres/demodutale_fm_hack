#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include <libhackrf/hackrf.h>

#include "rb_sig.h"
#include "ring_buffer.h"

#include "fm_demod.h"
#include "am_demod.h"
#include "opus_tx.h"

#include "psd.h"
#include "datatypes.h"
#include "sdr_HAL.h"

/* ===================== CONFIG ===================== */
#define FREQ_HZ                 105700000

/* Fs alta para PSD (span amplio). Recomendada: múltiplo entero de 1.92 MHz */
#define SAMPLE_RATE_RF_IN       19200000   /* 19.2 MHz */

/* Fs que verá la demod (tu valor original) */
#define SAMPLE_RATE_DEMOD       1920000    /* 1.92 MHz */

/* Debe cumplirse: SAMPLE_RATE_RF_IN / SAMPLE_RATE_DEMOD = entero */
#define DECIM_FACTOR            (SAMPLE_RATE_RF_IN / SAMPLE_RATE_DEMOD)

#if (SAMPLE_RATE_RF_IN % SAMPLE_RATE_DEMOD) != 0
#error "SAMPLE_RATE_RF_IN must be an integer multiple of SAMPLE_RATE_DEMOD"
#endif

#if DECIM_FACTOR < 2
#error "DECIM_FACTOR must be >= 2"
#endif

#define SAMPLE_RATE_AUDIO       48000
#define DECIMATION_AUDIO        (SAMPLE_RATE_DEMOD / SAMPLE_RATE_AUDIO) /* 1.92e6/48e3=40 */

#if (SAMPLE_RATE_DEMOD % SAMPLE_RATE_AUDIO) != 0
#error "SAMPLE_RATE_DEMOD must be divisible by SAMPLE_RATE_AUDIO"
#endif

#define FRAME_MS                20
#define FRAME_SAMPLES           ((SAMPLE_RATE_AUDIO * FRAME_MS) / 1000)

#define PY_HOST                 "127.0.0.1"
#define PY_PORT                 9000

/* RBs */
#define IQ_RB_RAW_BYTES         (32 * 1024 * 1024)   /* IQ a 19.2 MHz -> grande */
#define IQ_RB_DEMOD_BYTES       (4  * 1024 * 1024)   /* IQ ya decimado */
#define PCM_RB_BYTES            (256 * 1024)

/* PSD ring buffer grande (como tu standalone) */
#define PSD_RB_BYTES            (100 * 1024 * 1024)

/* PSD output */
#define PSD_CSV_PATH            "static/last_psd.csv"

/* PSD loop */
#define PSD_WAIT_TIMEOUT_ITERS  500
#define PSD_WAIT_SLEEP_US       10000
#define PSD_POST_SLEEP_US       500000

/* ===================== DEMOD MODES ===================== */
typedef enum { DEMOD_FM = 1, DEMOD_AM = 2 } demod_mode_t;
static demod_mode_t g_mode = DEMOD_FM;

/* ===================== ESTADO GLOBAL ===================== */
static atomic_int g_stop = 0;

static hackrf_device *g_dev = NULL;
static opus_tx_t *g_tx = NULL;

/* RBs */
static rb_sig_t g_iq_raw_rb;     /* IQ @ Fs_in */
static rb_sig_t g_iq_demod_rb;   /* IQ @ 1.92 MHz */
static rb_sig_t g_pcm_rb;

static atomic_ulong g_iq_raw_drops   = 0;
static atomic_ulong g_iq_demod_drops = 0;
static atomic_ulong g_pcm_drops      = 0;

/* PSD RB */
static ring_buffer_t g_psd_rb;
static volatile bool g_psd_capture_active = false;
static atomic_ulong g_psd_drops = 0;

/* Demod params */
static float g_fm_deemph_or_audio_bw = 8000.0f;
static float g_am_audio_bw           = 12000.0f;

/* PSD pipeline config */
static DesiredCfg_t g_desired_cfg = {0};
static SDR_cfg_t    g_hack_cfg    = {0};
static PsdConfig_t  g_psd_cfg     = {0};
static RB_cfg_t     g_rb_cfg      = {0};

/* ===================== HELPERS ===================== */
static const char* mode_str(demod_mode_t m) {
    switch (m) { case DEMOD_AM: return "AM"; case DEMOD_FM: return "FM"; default: return "UNKNOWN"; }
}

/* CSV: freq_rel + center_freq => freq_abs */
static int save_results_csv(const char *csv_path,
                            double* freq_array_rel,
                            double* psd_array,
                            int length,
                            const SDR_cfg_t *local_hack,
                            const char *scale_label)
{
    if (!csv_path || !freq_array_rel || !psd_array || length <= 0 || !local_hack) return -1;

    FILE *fp = fopen(csv_path, "w");
    if (!fp) { perror("[CSV] fopen"); return -1; }

    fprintf(fp, "freq_hz,psd_%s\n", (scale_label && scale_label[0]) ? scale_label : "lin");

    for (int i = 0; i < length; i++) {
        double f_abs = freq_array_rel[i] + (double)local_hack->center_freq;
        fprintf(fp, "%.6f,%.12e\n", f_abs, psd_array[i]);
    }

    fclose(fp);
    return 0;
}

/* ===================== CIC DECIMATOR (FAST STREAMING) ===================== */
/*
   CIC decimator:
   - Muy barato computacionalmente (sumas/restas).
   - Hace anti-alias al decimar (integrator+comb).
   - Orden N=3 típicamente suficiente para audio FM/AM en sensores edge.
   - Decim factor R = DECIM_FACTOR (por defecto 10 si Fs_in=19.2MHz y Fs_out=1.92MHz).

   Nota: Esto prioriza "no entrecortado" (tiempo real) sobre stopband extrema.
*/

typedef struct {
    int R;              /* decimation factor */
    int N;              /* number of stages */
    int ctr;            /* sample counter */
    int64_t int_i[4];   /* integrator states I */
    int64_t int_q[4];   /* integrator states Q */
    int64_t comb_i[4];  /* comb delay states I */
    int64_t comb_q[4];  /* comb delay states Q */
} cic_decim_t;

static void cic_init(cic_decim_t *c, int R, int N) {
    memset(c, 0, sizeof(*c));
    c->R = R;
    c->N = N;
    c->ctr = 0;
}

static inline void cic_process_one(cic_decim_t *c, int32_t xi, int32_t xq,
                                   int32_t *yo_i, int32_t *yo_q, bool *produced)
{
    /* integrator chain */
    int64_t yi = xi;
    int64_t yq = xq;
    for (int s = 0; s < c->N; s++) {
        c->int_i[s] += yi;
        c->int_q[s] += yq;
        yi = c->int_i[s];
        yq = c->int_q[s];
    }

    c->ctr++;
    if (c->ctr < c->R) {
        *produced = false;
        return;
    }
    c->ctr = 0;

    /* comb chain at output rate */
    for (int s = 0; s < c->N; s++) {
        int64_t prev_i = c->comb_i[s];
        int64_t prev_q = c->comb_q[s];
        c->comb_i[s] = yi;
        c->comb_q[s] = yq;
        yi = yi - prev_i;
        yq = yq - prev_q;
    }

    /* scaling: CIC gain = (R^N). We bring it down to reasonable dynamic range.
       Input is int8 -> [-128,127]. We promoted to int32 then to int64 in integrators.
       Divide by R^N to roughly normalize.
    */
    int64_t gain = 1;
    for (int k = 0; k < c->N; k++) gain *= c->R;
    if (gain <= 0) gain = 1;

    yi /= gain;
    yq /= gain;

    /* clamp to int8-ish range scaled in int32 */
    if (yi > 127) yi = 127;
    if (yi < -128) yi = -128;
    if (yq > 127) yq = 127;
    if (yq < -128) yq = -128;

    *yo_i = (int32_t)yi;
    *yo_q = (int32_t)yq;
    *produced = true;
}

/* ===================== HACKRF CALLBACK ===================== */
static int rx_callback(hackrf_transfer* transfer) {
    if (atomic_load(&g_stop)) return 0;

    /* 1) Rama IQ RAW (siempre) -> hilo decimator */
    size_t wraw = rb_sig_write(&g_iq_raw_rb, transfer->buffer, (size_t)transfer->valid_length);
    if (wraw < (size_t)transfer->valid_length) {
        atomic_fetch_add(&g_iq_raw_drops,
                         (unsigned long)((size_t)transfer->valid_length - wraw));
    }

    /* 2) Rama PSD: solo cuando activo */
    if (g_psd_capture_active) {
        size_t w2 = rb_write(&g_psd_rb, transfer->buffer, (size_t)transfer->valid_length);
        if (w2 < (size_t)transfer->valid_length) {
            atomic_fetch_add(&g_psd_drops,
                             (unsigned long)((size_t)transfer->valid_length - w2));
        }
    }

    return 0;
}

/* ===================== DECIM THREAD: RAW -> DEMOD ===================== */
static void* decim_thread_fn(void* arg) {
    (void)arg;

    fprintf(stderr, "[DECIM] Start | Fs_in=%d -> Fs_demod=%d | R=%d\n",
            SAMPLE_RATE_RF_IN, SAMPLE_RATE_DEMOD, (int)DECIM_FACTOR);

    cic_decim_t cic;
    cic_init(&cic, (int)DECIM_FACTOR, 3); /* N=3 */

    enum { IN_CHUNK = 32768 }; /* bytes (must be even) */
    uint8_t in_bytes[IN_CHUNK];

    /* Output chunk: worst-case outputs = inputs/2 (bytes per IQ) / R -> bytes_out = (in/2/R)*2 */
    uint8_t out_bytes[IN_CHUNK]; /* safe upper bound for typical R>=2 */

    while (!atomic_load(&g_stop)) {
        /* Wait at least 2 bytes (1 IQ sample) */
        size_t got = rb_sig_read_blocking(&g_iq_raw_rb, in_bytes, 2, &g_stop);
        if (got == 0) break;

        got += rb_sig_read(&g_iq_raw_rb, in_bytes + got, IN_CHUNK - got);
        got = (got / 2) * 2;

        int8_t *b = (int8_t*)in_bytes;
        int n_iq = (int)(got / 2);

        int out_idx = 0;

        for (int k = 0; k < n_iq && !atomic_load(&g_stop); k++) {
            int32_t xi = (int32_t)b[2*k];
            int32_t xq = (int32_t)b[2*k + 1];

            int32_t yo_i, yo_q;
            bool produced;
            cic_process_one(&cic, xi, xq, &yo_i, &yo_q, &produced);

            if (produced) {
                /* write as int8 interleaved */
                if (out_idx + 2 <= (int)sizeof(out_bytes)) {
                    out_bytes[out_idx++] = (uint8_t)((int8_t)yo_i);
                    out_bytes[out_idx++] = (uint8_t)((int8_t)yo_q);
                }
            }
        }

        if (out_idx > 0) {
            size_t w = rb_sig_write(&g_iq_demod_rb, out_bytes, (size_t)out_idx);
            if (w < (size_t)out_idx) {
                atomic_fetch_add(&g_iq_demod_drops,
                                 (unsigned long)((size_t)out_idx - w));
            }
        }
    }

    fprintf(stderr, "[DECIM] Exit\n");
    return NULL;
}

/* ===================== DEMOD DSP THREAD ===================== */
static void* demod_thread_fn(void* arg) {
    (void)arg;

    fprintf(stderr, "[DEMOD] Start | mode=%s | Fs_demod=%d | DecimAudio=%d -> %d Hz\n",
            mode_str(g_mode), SAMPLE_RATE_DEMOD, DECIMATION_AUDIO, SAMPLE_RATE_AUDIO);

    enum { IQ_CHUNK = 16384 };
    uint8_t iq_bytes[IQ_CHUNK];

    fm_demod_t fm;
    am_demod_t am;

    if (g_mode == DEMOD_FM) {
        fm_demod_init(&fm, SAMPLE_RATE_DEMOD, DECIMATION_AUDIO, g_fm_deemph_or_audio_bw);
    } else {
        am_demod_init(&am, SAMPLE_RATE_DEMOD, DECIMATION_AUDIO, g_am_audio_bw);
    }

    while (!atomic_load(&g_stop)) {
        size_t got = rb_sig_read_blocking(&g_iq_demod_rb, iq_bytes, 2, &g_stop);
        if (got == 0) break;

        got += rb_sig_read(&g_iq_demod_rb, iq_bytes + got, IQ_CHUNK - got);
        got = (got / 2) * 2;

        int8_t *buf = (int8_t*)iq_bytes;
        int count = (int)(got / 2);

        for (int j = 0; j < count && !atomic_load(&g_stop); j++) {
            float i = (float)buf[2*j]     / 128.0f;
            float q = (float)buf[2*j + 1] / 128.0f;

            if (g_mode == DEMOD_FM) {
                float dphi = fm_demod_phase_diff(&fm, i, q);

                fm.sum_audio += dphi;
                fm.dec_counter++;
                if (fm.dec_counter == fm.decimation) {
                    float audio = fm.sum_audio / (float)fm.decimation;

                    int16_t s16 = (int16_t)lrintf(
                        fmaxf(fminf(audio * fm.audio_gain, 32767.0f), -32768.0f)
                    );

                    size_t wpcm = rb_sig_write(&g_pcm_rb, &s16, sizeof(s16));
                    if (wpcm < sizeof(s16))
                        atomic_fetch_add(&g_pcm_drops,
                                         (unsigned long)(sizeof(s16) - wpcm));

                    fm.sum_audio = 0.0f;
                    fm.dec_counter = 0;
                }
            } else {
                int16_t pcm;
                am_depth_report_t rep;
                if (am_demod_process_iq(&am, i, q, &pcm, &rep)) {
                    size_t wpcm = rb_sig_write(&g_pcm_rb, &pcm, sizeof(pcm));
                    if (wpcm < sizeof(pcm))
                        atomic_fetch_add(&g_pcm_drops,
                                         (unsigned long)(sizeof(pcm) - wpcm));
                }
            }
        }
    }

    fprintf(stderr, "[DEMOD] Exit\n");
    return NULL;
}

/* ===================== NET/OPUS THREAD ===================== */
static void* net_thread_fn(void* arg) {
    (void)arg;
    fprintf(stderr, "[NET] Start\n");

    int16_t frame[FRAME_SAMPLES];

    while (!atomic_load(&g_stop)) {
        size_t need = FRAME_SAMPLES * sizeof(int16_t);
        size_t got = rb_sig_read_blocking(&g_pcm_rb, frame, need, &g_stop);
        if (got == 0) break;

        if (opus_tx_send_frame(g_tx, frame, FRAME_SAMPLES) != 0) {
            fprintf(stderr, "[NET] opus_tx_send_frame error -> stop\n");
            atomic_store(&g_stop, 1);
            break;
        }
    }

    fprintf(stderr, "[NET] Exit\n");
    return NULL;
}

/* ===================== PSD THREAD ===================== */
static void* psd_thread_fn(void* arg) {
    (void)arg;

    fprintf(stderr, "[PSD] Start | total_bytes=%zu nperseg=%d scale=%s\n",
            (size_t)g_rb_cfg.total_bytes,
            g_psd_cfg.nperseg,
            (g_desired_cfg.scale ? g_desired_cfg.scale : "lin"));

    if ((size_t)g_rb_cfg.total_bytes > g_psd_rb.size) {
        fprintf(stderr, "[PSD] ERROR: total_bytes=%zu > PSD_RB_BYTES=%zu\n",
                (size_t)g_rb_cfg.total_bytes, g_psd_rb.size);
        atomic_store(&g_stop, 1);
        return NULL;
    }

    while (!atomic_load(&g_stop)) {
        rb_reset(&g_psd_rb);
        g_psd_capture_active = true;

        int safety = PSD_WAIT_TIMEOUT_ITERS;
        while (!atomic_load(&g_stop) && safety-- > 0) {
            if (rb_available(&g_psd_rb) >= (size_t)g_rb_cfg.total_bytes) break;
            usleep(PSD_WAIT_SLEEP_US);
        }

        g_psd_capture_active = false;

        if (atomic_load(&g_stop)) break;
        if (safety <= 0) {
            fprintf(stderr, "[PSD] Timeout waiting bytes (drops=%lu). Will retry.\n",
                    (unsigned long)atomic_load(&g_psd_drops));
            usleep(PSD_POST_SLEEP_US);
            continue;
        }

        int8_t *linear_buffer = (int8_t*)malloc((size_t)g_rb_cfg.total_bytes);
        if (!linear_buffer) {
            fprintf(stderr, "[PSD] malloc linear_buffer failed\n");
            usleep(PSD_POST_SLEEP_US);
            continue;
        }

        rb_read(&g_psd_rb, linear_buffer, (size_t)g_rb_cfg.total_bytes);

        signal_iq_t *sig = load_iq_from_buffer(linear_buffer, (size_t)g_rb_cfg.total_bytes);
        free(linear_buffer);

        if (!sig) {
            fprintf(stderr, "[PSD] load_iq_from_buffer failed\n");
            usleep(PSD_POST_SLEEP_US);
            continue;
        }

        double *freq = (double*)malloc((size_t)g_psd_cfg.nperseg * sizeof(double));
        double *psd  = (double*)malloc((size_t)g_psd_cfg.nperseg * sizeof(double));

        if (!freq || !psd) {
            fprintf(stderr, "[PSD] malloc freq/psd failed\n");
            free(freq); free(psd);
            free_signal_iq(sig);
            usleep(PSD_POST_SLEEP_US);
            continue;
        }

        execute_welch_psd(sig, &g_psd_cfg, freq, psd);
        scale_psd(psd, g_psd_cfg.nperseg, g_desired_cfg.scale);

        double half_span = g_desired_cfg.span / 2.0;
        int start_idx = 0;
        int end_idx = g_psd_cfg.nperseg - 1;

        for (int i = 0; i < g_psd_cfg.nperseg; i++) {
            if (freq[i] >= -half_span) { start_idx = i; break; }
        }
        for (int i = start_idx; i < g_psd_cfg.nperseg; i++) {
            if (freq[i] > half_span) { end_idx = i - 1; break; }
            end_idx = i;
        }

        int valid_len = end_idx - start_idx + 1;
        if (valid_len > 0) {
            if (save_results_csv(PSD_CSV_PATH,
                                 &freq[start_idx],
                                 &psd[start_idx],
                                 valid_len,
                                 &g_hack_cfg,
                                 g_desired_cfg.scale) == 0) {
                fprintf(stderr, "[PSD] Saved CSV: %s | bins=%d | drops=%lu\n",
                        PSD_CSV_PATH, valid_len,
                        (unsigned long)atomic_load(&g_psd_drops));
            }
        } else {
            fprintf(stderr, "[PSD] Warning: span crop -> 0 bins\n");
        }

        free(freq);
        free(psd);
        free_signal_iq(sig);

        usleep(PSD_POST_SLEEP_US);
    }

    fprintf(stderr, "[PSD] Exit\n");
    return NULL;
}

/* ===================== MAIN ===================== */
int main(void) {
    /* 0) Elegir modo runtime */
    g_mode = DEMOD_FM; /* o DEMOD_AM */
    fprintf(stderr, "[MAIN] Boot | mode=%s\n", mode_str(g_mode));

    fprintf(stderr, "[MAIN] Rates | Fs_in=%d | Fs_demod=%d | R=%d | Fs_audio=%d | DecimAudio=%d\n",
            SAMPLE_RATE_RF_IN, SAMPLE_RATE_DEMOD, (int)DECIM_FACTOR,
            SAMPLE_RATE_AUDIO, (int)DECIMATION_AUDIO);

    /* 1) Opus TX */
    opus_tx_cfg_t ocfg = {
        .sample_rate = SAMPLE_RATE_AUDIO,
        .channels = 1,
        .bitrate = 64000,
        .complexity = 5,
        .vbr = 1
    };
    g_tx = opus_tx_create(PY_HOST, PY_PORT, &ocfg);
    if (!g_tx) {
        fprintf(stderr, "[MAIN] opus_tx_create failed\n");
        return 1;
    }

    /* 2) RBs */
    rb_sig_init(&g_iq_raw_rb,   IQ_RB_RAW_BYTES);
    rb_sig_init(&g_iq_demod_rb, IQ_RB_DEMOD_BYTES);
    rb_sig_init(&g_pcm_rb,      PCM_RB_BYTES);

    /* 3) PSD ring buffer */
    rb_init(&g_psd_rb, PSD_RB_BYTES);
    fprintf(stderr, "[MAIN] PSD ring buffer init: %zu MB\n", (size_t)PSD_RB_BYTES / (1024*1024));

    /* 4) Config única (hardware + PSD params) */
    memset(&g_desired_cfg, 0, sizeof(g_desired_cfg));
    g_desired_cfg.rbw          = 1000; /* ejemplo */
    g_desired_cfg.center_freq  = (double)FREQ_HZ;
    g_desired_cfg.sample_rate  = (double)SAMPLE_RATE_RF_IN;   /* <-- Fs alta */
    g_desired_cfg.span         = (double)SAMPLE_RATE_RF_IN;   /* típico <= Fs */
    g_desired_cfg.scale        = "dBm";
    g_desired_cfg.lna_gain     = 28;
    g_desired_cfg.vga_gain     = 32;
    g_desired_cfg.amp_enabled  = 1;
    g_desired_cfg.antenna_port = 1;

    find_params_psd(g_desired_cfg, &g_hack_cfg, &g_psd_cfg, &g_rb_cfg);
    print_config_summary(&g_desired_cfg, &g_hack_cfg, &g_psd_cfg, &g_rb_cfg);

    /* 5) HackRF init/open/apply */
    if (hackrf_init() != HACKRF_SUCCESS) {
        fprintf(stderr, "[MAIN] hackrf_init failed\n");
        return 1;
    }
    if (hackrf_open(&g_dev) != HACKRF_SUCCESS || !g_dev) {
        fprintf(stderr, "[MAIN] hackrf_open failed\n");
        hackrf_exit();
        return 1;
    }
    hackrf_apply_cfg(g_dev, &g_hack_cfg);

    /* 6) Threads */
    pthread_t th_decim, th_demod, th_net, th_psd;

    if (pthread_create(&th_decim, NULL, decim_thread_fn, NULL) != 0) {
        fprintf(stderr, "[MAIN] pthread_create decim failed\n");
        return 1;
    }
    if (pthread_create(&th_demod, NULL, demod_thread_fn, NULL) != 0) {
        fprintf(stderr, "[MAIN] pthread_create demod failed\n");
        atomic_store(&g_stop, 1);
        rb_sig_wake_all(&g_iq_raw_rb);
        pthread_join(th_decim, NULL);
        return 1;
    }
    if (pthread_create(&th_net, NULL, net_thread_fn, NULL) != 0) {
        fprintf(stderr, "[MAIN] pthread_create net failed\n");
        atomic_store(&g_stop, 1);
        rb_sig_wake_all(&g_iq_raw_rb);
        rb_sig_wake_all(&g_iq_demod_rb);
        pthread_join(th_decim, NULL);
        pthread_join(th_demod, NULL);
        return 1;
    }
    if (pthread_create(&th_psd, NULL, psd_thread_fn, NULL) != 0) {
        fprintf(stderr, "[MAIN] pthread_create psd failed\n");
        atomic_store(&g_stop, 1);
        rb_sig_wake_all(&g_iq_raw_rb);
        rb_sig_wake_all(&g_iq_demod_rb);
        rb_sig_wake_all(&g_pcm_rb);
        pthread_join(th_decim, NULL);
        pthread_join(th_demod, NULL);
        pthread_join(th_net, NULL);
        return 1;
    }

    /* 7) Start RX (único) */
    if (hackrf_start_rx(g_dev, rx_callback, NULL) != HACKRF_SUCCESS) {
        fprintf(stderr, "[MAIN] hackrf_start_rx failed\n");
        atomic_store(&g_stop, 1);
    }

    fprintf(stderr,
        "[MAIN] Running | Fc=%.3f MHz | Fs_in=%d | Fs_demod=%d | Demod=%s | PSD total_bytes=%zu | ENTER to stop\n",
        (double)FREQ_HZ / 1e6, SAMPLE_RATE_RF_IN, SAMPLE_RATE_DEMOD,
        mode_str(g_mode), (size_t)g_rb_cfg.total_bytes
    );

    getchar();
    atomic_store(&g_stop, 1);

    /* 8) Stop + join */
    hackrf_stop_rx(g_dev);
    hackrf_close(g_dev);
    hackrf_exit();

    rb_sig_wake_all(&g_iq_raw_rb);
    rb_sig_wake_all(&g_iq_demod_rb);
    rb_sig_wake_all(&g_pcm_rb);

    pthread_join(th_decim, NULL);
    pthread_join(th_demod, NULL);
    pthread_join(th_net, NULL);
    pthread_join(th_psd, NULL);

    /* 9) Cleanup */
    opus_tx_destroy(g_tx);
    rb_sig_free(&g_iq_raw_rb);
    rb_sig_free(&g_iq_demod_rb);
    rb_sig_free(&g_pcm_rb);

    rb_free(&g_psd_rb);

    fprintf(stderr,
        "[MAIN] Done | RAW drops=%lu | DEMOD_IQ drops=%lu | PSD drops=%lu | PCM drops=%lu\n",
        (unsigned long)atomic_load(&g_iq_raw_drops),
        (unsigned long)atomic_load(&g_iq_demod_drops),
        (unsigned long)atomic_load(&g_psd_drops),
        (unsigned long)atomic_load(&g_pcm_drops));

    return 0;
}
