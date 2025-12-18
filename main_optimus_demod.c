// main.c
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

/* Your libs */
#include "rb_sig.h"
#include "ring_buffer.h"

#include "fm_demod.h"
#include "am_demod.h"
#include "opus_tx.h"

#include "psd.h"
#include "datatypes.h"
#include "sdr_HAL.h"

/* New modular libs */
#include "pipeline_threads.h"   /* threads library */
#include "cic_decim.h"          /* not used directly in main, but OK to include */

/* ===================== CONFIG ===================== */
#define FREQ_HZ                 105700000

/* High Fs for PSD (wide span) */
#define SAMPLE_RATE_RF_IN       19200000/2   /* 19.2 MHz */

/* Demod Fs (narrower) */
#define SAMPLE_RATE_DEMOD       1920000    /* 1.92 MHz */

/* Must be integer */
#define DECIM_FACTOR            (SAMPLE_RATE_RF_IN / SAMPLE_RATE_DEMOD)

#if (SAMPLE_RATE_RF_IN % SAMPLE_RATE_DEMOD) != 0
#error "SAMPLE_RATE_RF_IN must be an integer multiple of SAMPLE_RATE_DEMOD"
#endif

#if DECIM_FACTOR < 2
#error "DECIM_FACTOR must be >= 2"
#endif

#define SAMPLE_RATE_AUDIO       48000
#define DECIMATION_AUDIO        (SAMPLE_RATE_DEMOD / SAMPLE_RATE_AUDIO) /* 40 */

#if (SAMPLE_RATE_DEMOD % SAMPLE_RATE_AUDIO) != 0
#error "SAMPLE_RATE_DEMOD must be divisible by SAMPLE_RATE_AUDIO"
#endif

#define FRAME_MS                20
#define FRAME_SAMPLES           ((SAMPLE_RATE_AUDIO * FRAME_MS) / 1000)

#define PY_HOST                 "127.0.0.1"
#define PY_PORT                 8000

/* RBs */
#define IQ_RB_RAW_BYTES         (32 * 1024 * 1024)   /* IQ @ 19.2 MHz */
#define IQ_RB_DEMOD_BYTES       (4  * 1024 * 1024)   /* IQ @ 1.92 MHz */
#define PCM_RB_BYTES            (256 * 1024)

/* PSD ring buffer */
#define PSD_RB_BYTES            (100 * 1024 * 1024)

/* PSD output */
#define PSD_CSV_PATH            "static2/last_psd.csv"

/* PSD loop */
#define PSD_WAIT_TIMEOUT_ITERS  500
#define PSD_WAIT_SLEEP_US       10000
#define PSD_POST_SLEEP_US       500000

/* ===================== DEMOD MODES ===================== */
static demod_mode_t g_mode = DEMOD_FM; /* DEMOD_FM or DEMOD_AM */

/* ===================== GLOBAL STATE ===================== */
static atomic_int g_stop = 0;

static hackrf_device *g_dev = NULL;
static opus_tx_t *g_tx = NULL;

/* RBs (streaming) */
static rb_sig_t g_iq_raw_rb;     /* IQ @ Fs_in */
static rb_sig_t g_iq_demod_rb;   /* IQ @ 1.92 MHz */
static rb_sig_t g_pcm_rb;

static atomic_ulong g_iq_raw_drops   = 0;
static atomic_ulong g_iq_demod_drops = 0;
static atomic_ulong g_pcm_drops      = 0;

/* PSD RB + control */
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
    switch (m) {
        case DEMOD_AM: return "AM";
        case DEMOD_FM: return "FM";
        default:       return "UNKNOWN";
    }
}


/* ===================== HACKRF CALLBACK ===================== */
static int rx_callback(hackrf_transfer* transfer) {
    if (atomic_load(&g_stop)) return 0;

    /* 1) RAW IQ always -> decimator thread */
    size_t wraw = rb_sig_write(&g_iq_raw_rb, transfer->buffer, (size_t)transfer->valid_length);
    if (wraw < (size_t)transfer->valid_length) {
        atomic_fetch_add(&g_iq_raw_drops,
                         (unsigned long)((size_t)transfer->valid_length - wraw));
    }

    /* 2) PSD capture only when active */
    if (g_psd_capture_active) {
        size_t w2 = rb_write(&g_psd_rb, transfer->buffer, (size_t)transfer->valid_length);
        if (w2 < (size_t)transfer->valid_length) {
            atomic_fetch_add(&g_psd_drops,
                             (unsigned long)((size_t)transfer->valid_length - w2));
        }
    }

    return 0;
}

/* ===================== MAIN ===================== */
int main(void) {
    /* 0) Select mode in code */
    g_mode = DEMOD_FM; /* change to DEMOD_AM if needed */

    fprintf(stderr, "[MAIN] Boot | mode=%s\n", mode_str(g_mode));
    fprintf(stderr, "[MAIN] Rates | Fs_in=%d | Fs_demod=%d | R=%d | Fs_audio=%d | DecimAudio=%d\n",
            SAMPLE_RATE_RF_IN, SAMPLE_RATE_DEMOD, (int)DECIM_FACTOR,
            SAMPLE_RATE_AUDIO, (int)DECIMATION_AUDIO);

    /* 1) Opus TX */
    opus_tx_cfg_t ocfg = {
        .sample_rate = SAMPLE_RATE_AUDIO,
        .channels    = 1,
        .bitrate     = 64000,
        .complexity  = 5,
        .vbr         = 1
    };
    g_tx = opus_tx_create(PY_HOST, PY_PORT, &ocfg);
    if (!g_tx) {
        fprintf(stderr, "[MAIN] opus_tx_create failed\n");
        return 1;
    }

    /* 2) Streaming RBs */
    rb_sig_init(&g_iq_raw_rb,   IQ_RB_RAW_BYTES);
    rb_sig_init(&g_iq_demod_rb, IQ_RB_DEMOD_BYTES);
    rb_sig_init(&g_pcm_rb,      PCM_RB_BYTES);

    /* 3) PSD ring buffer */
    rb_init(&g_psd_rb, PSD_RB_BYTES);
    fprintf(stderr, "[MAIN] PSD ring buffer init: %zu MB\n", (size_t)PSD_RB_BYTES / (1024*1024));

    /* 4) Build desired config (HW + PSD) */
    memset(&g_desired_cfg, 0, sizeof(g_desired_cfg));
    g_desired_cfg.rbw          = 1000; /* example */
    g_desired_cfg.center_freq  = (double)FREQ_HZ;
    g_desired_cfg.sample_rate  = (double)SAMPLE_RATE_RF_IN;  /* PSD uses high Fs */
    g_desired_cfg.span         = (double)SAMPLE_RATE_RF_IN;  /* typical <= Fs */
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

    /* 6) Start pipeline threads (from libs/pipeline_threads.*) */
    pipeline_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.stop = &g_stop;
    ctx.mode = g_mode;

    ctx.sample_rate_rf_in   = SAMPLE_RATE_RF_IN;
    ctx.sample_rate_demod   = SAMPLE_RATE_DEMOD;
    ctx.decim_factor        = (int)DECIM_FACTOR;
    ctx.sample_rate_audio   = SAMPLE_RATE_AUDIO;
    ctx.decimation_audio    = (int)DECIMATION_AUDIO;
    ctx.frame_samples       = FRAME_SAMPLES;

    ctx.iq_raw_rb   = &g_iq_raw_rb;
    ctx.iq_demod_rb = &g_iq_demod_rb;
    ctx.pcm_rb      = &g_pcm_rb;

    ctx.iq_raw_drops   = &g_iq_raw_drops;
    ctx.iq_demod_drops = &g_iq_demod_drops;
    ctx.pcm_drops      = &g_pcm_drops;

    ctx.psd_rb            = &g_psd_rb;
    ctx.psd_capture_active = &g_psd_capture_active;
    ctx.psd_drops         = &g_psd_drops;

    ctx.tx = g_tx;

    ctx.fm_audio_bw_or_deemph = g_fm_deemph_or_audio_bw;
    ctx.am_audio_bw           = g_am_audio_bw;

    ctx.desired_cfg = &g_desired_cfg;
    ctx.hack_cfg    = &g_hack_cfg;
    ctx.psd_cfg     = &g_psd_cfg;
    ctx.rb_cfg      = &g_rb_cfg;

    ctx.psd_csv_path = PSD_CSV_PATH;

    ctx.psd_wait_timeout_iters = PSD_WAIT_TIMEOUT_ITERS;
    ctx.psd_wait_sleep_us      = PSD_WAIT_SLEEP_US;
    ctx.psd_post_sleep_us      = PSD_POST_SLEEP_US;

    pipeline_threads_t threads;
    if (pipeline_threads_start(&threads, &ctx) != 0) {
        fprintf(stderr, "[MAIN] pipeline_threads_start failed\n");
        hackrf_close(g_dev);
        hackrf_exit();
        opus_tx_destroy(g_tx);
        return 1;
    }

    /* 7) Start RX (single producer) */
    if (hackrf_start_rx(g_dev, rx_callback, NULL) != HACKRF_SUCCESS) {
        fprintf(stderr, "[MAIN] hackrf_start_rx failed\n");
        pipeline_threads_stop(&ctx);
    }

    fprintf(stderr,
        "[MAIN] Running | Fc=%.3f MHz | Fs_in=%d | Fs_demod=%d | Demod=%s | PSD total_bytes=%zu | ENTER to stop\n",
        (double)FREQ_HZ / 1e6, SAMPLE_RATE_RF_IN, SAMPLE_RATE_DEMOD,
        mode_str(g_mode), (size_t)g_rb_cfg.total_bytes
    );

    getchar();

    /* 8) Stop + join */
    pipeline_threads_stop(&ctx);

    hackrf_stop_rx(g_dev);
    hackrf_close(g_dev);
    hackrf_exit();

    pipeline_threads_join(&threads);

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
