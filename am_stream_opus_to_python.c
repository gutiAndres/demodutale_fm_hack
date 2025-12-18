// am_stream_modular.c
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <libhackrf/hackrf.h>

#include "rb_sig.h"
#include "am_demod.h"
#include "opus_tx.h"

/* ===================== CONFIG ===================== */
#define FREQ_HZ            152000000
#define SAMPLE_RATE_RF     1920000
#define SAMPLE_RATE_AUDIO  48000
#define DECIMATION         40

#define FRAME_MS           20
#define FRAME_SAMPLES      ((SAMPLE_RATE_AUDIO * FRAME_MS) / 1000)

#define PY_HOST            "127.0.0.1"
#define PY_PORT            9000

#define IQ_RB_SIZE_BYTES   (2 * 1024 * 1024)
#define PCM_RB_SIZE_BYTES  (256 * 1024)

/* ===================== GLOBAL ===================== */
static atomic_int g_stop = 0;
static rb_sig_t g_iq_rb;
static rb_sig_t g_pcm_rb;

static atomic_ulong g_iq_drops = 0;
static atomic_ulong g_pcm_drops = 0;

static opus_tx_t *g_tx = NULL;

/* ===================== RX CALLBACK ===================== */
static int rx_callback(hackrf_transfer* t) {
    if (atomic_load(&g_stop)) return 0;

    size_t w = rb_sig_write(&g_iq_rb, t->buffer, t->valid_length);
    if (w < (size_t)t->valid_length)
        atomic_fetch_add(&g_iq_drops, t->valid_length - w);
    return 0;
}

/* ===================== DSP THREAD ===================== */
static void* dsp_thread_fn(void* arg) {
    (void)arg;

    am_demod_t dem;
    am_demod_init(&dem, SAMPLE_RATE_RF, DECIMATION, 12000.0f);

    enum { IQ_CHUNK = 16384 };
    uint8_t iq[IQ_CHUNK];

    while (!atomic_load(&g_stop)) {
        size_t got = rb_sig_read_blocking(&g_iq_rb, iq, 2, &g_stop);
        if (!got) break;

        got += rb_sig_read(&g_iq_rb, iq + got, IQ_CHUNK - got);
        got = (got / 2) * 2;

        int8_t *b = (int8_t*)iq;
        int n = got / 2;

        for (int k = 0; k < n && !atomic_load(&g_stop); k++) {
            float i = b[2*k]     / 128.0f;
            float q = b[2*k + 1] / 128.0f;

            int16_t pcm;
            am_depth_report_t rep;

            if (am_demod_process_iq(&dem, i, q, &pcm, &rep)) {
                size_t w = rb_sig_write(&g_pcm_rb, &pcm, sizeof(pcm));
                if (w < sizeof(pcm))
                    atomic_fetch_add(&g_pcm_drops, sizeof(pcm) - w);

                if (rep.ready) {
                    printf("[AM] Depth: %.1f %% | EMA: %.1f %% | IQ drops: %lu | PCM drops: %lu\n",
                           rep.depth_peak_pct, rep.depth_ema_pct,
                           (unsigned long)g_iq_drops,
                           (unsigned long)g_pcm_drops);
                }
            }
        }
    }
    return NULL;
}

/* ===================== NET THREAD ===================== */
static void* net_thread_fn(void* arg) {
    (void)arg;

    int16_t frame[FRAME_SAMPLES];

    while (!atomic_load(&g_stop)) {
        size_t need = FRAME_SAMPLES * sizeof(int16_t);
        size_t got = rb_sig_read_blocking(&g_pcm_rb, frame, need, &g_stop);
        if (!got) break;

        if (opus_tx_send_frame(g_tx, frame, FRAME_SAMPLES) != 0) {
            atomic_store(&g_stop, 1);
            break;
        }
    }
    return NULL;
}

/* ===================== MAIN ===================== */
int main(void) {
    opus_tx_cfg_t ocfg = {
        .sample_rate = SAMPLE_RATE_AUDIO,
        .channels = 1,
        .bitrate = 64000,
        .complexity = 5,
        .vbr = 1
    };

    g_tx = opus_tx_create(PY_HOST, PY_PORT, &ocfg);
    if (!g_tx) return 1;

    rb_sig_init(&g_iq_rb, IQ_RB_SIZE_BYTES);
    rb_sig_init(&g_pcm_rb, PCM_RB_SIZE_BYTES);

    hackrf_init();
    hackrf_device *dev = NULL;
    hackrf_open(&dev);

    hackrf_set_sample_rate(dev, SAMPLE_RATE_RF);
    hackrf_set_freq(dev, FREQ_HZ);
    hackrf_set_lna_gain(dev, 32);
    hackrf_set_vga_gain(dev, 28);
    hackrf_set_amp_enable(dev, 0);

    pthread_t tdsp, tnet;
    pthread_create(&tdsp, NULL, dsp_thread_fn, NULL);
    pthread_create(&tnet, NULL, net_thread_fn, NULL);

    hackrf_start_rx(dev, rx_callback, NULL);

    printf("[C] RX AM %.1f MHz – ENTER para salir\n", FREQ_HZ/1e6);
    getchar();

    atomic_store(&g_stop, 1);
    hackrf_stop_rx(dev);
    hackrf_close(dev);
    hackrf_exit();

    rb_sig_wake_all(&g_iq_rb);
    rb_sig_wake_all(&g_pcm_rb);

    pthread_join(tdsp, NULL);
    pthread_join(tnet, NULL);

    opus_tx_destroy(g_tx);
    rb_sig_free(&g_iq_rb);
    rb_sig_free(&g_pcm_rb);
    return 0;
}
