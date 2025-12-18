#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <libhackrf/hackrf.h>
#include <math.h>

#include "rb_sig.h"
#include "fm_demod.h"
#include "opus_tx.h"

/* ===================== CONFIG ===================== */
#define FREQ_HZ            103700000
#define SAMPLE_RATE_RF     1920000
#define SAMPLE_RATE_AUDIO  48000
#define DECIMATION         40

#define FRAME_MS           20
#define FRAME_SAMPLES      ((SAMPLE_RATE_AUDIO * FRAME_MS) / 1000)

#define PY_HOST            "127.0.0.1"
#define PY_PORT            9000

#define IQ_RB_SIZE_BYTES   (2 * 1024 * 1024)
#define PCM_RB_SIZE_BYTES  (256 * 1024)

/* ===================== ESTADO GLOBAL ===================== */
static atomic_int g_stop = 0;

static rb_sig_t g_iq_rb;
static rb_sig_t g_pcm_rb;
static atomic_ulong g_iq_dropped_bytes = 0;

static opus_tx_t *g_tx = NULL;

/* ===================== HACKRF CALLBACK ===================== */
static int rx_callback(hackrf_transfer* transfer) {
    if (atomic_load(&g_stop)) return 0;

    size_t w = rb_sig_write(&g_iq_rb, transfer->buffer, (size_t)transfer->valid_length);
    if (w < (size_t)transfer->valid_length) {
        atomic_fetch_add(&g_iq_dropped_bytes,
            (unsigned long)((size_t)transfer->valid_length - w));
    }
    return 0;
}

/* ===================== DSP THREAD ===================== */
static void* dsp_thread_fn(void* arg) {
    (void)arg;

    fm_demod_t dem;
    fm_demod_init(&dem, SAMPLE_RATE_RF, DECIMATION, 8000.0f);

    enum { IQ_CHUNK = 16384 };
    uint8_t iq_bytes[IQ_CHUNK];

    while (!atomic_load(&g_stop)) {
        size_t got = rb_sig_read_blocking(&g_iq_rb, iq_bytes, 2, &g_stop);
        if (got == 0) break;

        size_t more = rb_sig_read(&g_iq_rb, iq_bytes + got, IQ_CHUNK - got);
        got += more;
        got = (got / 2) * 2; // par I/Q

        int8_t *buf = (int8_t*)iq_bytes;
        int count = (int)(got / 2);

        for (int j = 0; j < count && !atomic_load(&g_stop); j++) {
            float i = (float)buf[2*j]     / 128.0f;
            float q = (float)buf[2*j + 1] / 128.0f;

            // phase diff (FM)
            float dphi = fm_demod_phase_diff(&dem, i, q);

            // métricas excursión (fuera del callback)
            fm_dev_report_t rep = fm_demod_update_deviation(&dem, dphi);
            if (rep.ready) {
                printf("[FM] Excursion pico: %.1f kHz | EMA: %.1f kHz | IQ drops: %lu bytes\n",
                       rep.dev_peak_khz, rep.dev_ema_khz,
                       (unsigned long)atomic_load(&g_iq_dropped_bytes));
            }

            // decimación simple a audio (reusa dphi acumulado)
            dem.sum_audio += dphi;
            dem.dec_counter++;
            if (dem.dec_counter == dem.decimation) {
                float audio = dem.sum_audio / (float)dem.decimation;
                int16_t s16 = (int16_t)lrintf(fmaxf(fminf(audio * dem.audio_gain, 32767.0f), -32768.0f));
                rb_sig_write(&g_pcm_rb, &s16, sizeof(s16));
                dem.sum_audio = 0.0f;
                dem.dec_counter = 0;
            }
        }
    }
    return NULL;
}

/* ===================== NET/OPUS THREAD ===================== */
static void* net_thread_fn(void* arg) {
    (void)arg;

    int16_t frame[FRAME_SAMPLES];

    while (!atomic_load(&g_stop)) {
        size_t need = FRAME_SAMPLES * sizeof(int16_t);
        size_t got = rb_sig_read_blocking(&g_pcm_rb, frame, need, &g_stop);
        if (got == 0) break;

        if (opus_tx_send_frame(g_tx, frame, FRAME_SAMPLES) != 0) {
            fprintf(stderr, "[C] Error enviando Opus\n");
            atomic_store(&g_stop, 1);
            break;
        }
    }
    return NULL;
}

int main(void) {
    // 1) Opus TX
    opus_tx_cfg_t ocfg = {
        .sample_rate = SAMPLE_RATE_AUDIO,
        .channels = 1,
        .bitrate = 64000,
        .complexity = 5,
        .vbr = 1
    };

    printf("[C] Conectando a Python %s:%d ...\n", PY_HOST, PY_PORT);
    g_tx = opus_tx_create(PY_HOST, PY_PORT, &ocfg);
    if (!g_tx) {
        fprintf(stderr, "[C] No pude crear Opus/TCP\n");
        return 1;
    }

    // 2) RBs
    rb_sig_init(&g_iq_rb, IQ_RB_SIZE_BYTES);
    rb_sig_init(&g_pcm_rb, PCM_RB_SIZE_BYTES);

    // 3) HackRF
    hackrf_init();
    hackrf_device *dev = NULL;
    if (hackrf_open(&dev) != HACKRF_SUCCESS) {
        fprintf(stderr, "[C] hackrf_open fallo\n");
        return 1;
    }

    hackrf_set_sample_rate(dev, SAMPLE_RATE_RF);
    hackrf_set_freq(dev, FREQ_HZ);
    hackrf_set_lna_gain(dev, 32);
    hackrf_set_vga_gain(dev, 28);
    hackrf_set_amp_enable(dev, 0);

    // 4) Threads
    pthread_t th_dsp, th_net;
    pthread_create(&th_dsp, NULL, dsp_thread_fn, NULL);
    pthread_create(&th_net, NULL, net_thread_fn, NULL);

    printf("[C] RX FM %.1f MHz | FsRF=%d | DECIM=%d -> FsAudio~%d\n",
           (float)FREQ_HZ / 1e6f, SAMPLE_RATE_RF, DECIMATION, SAMPLE_RATE_RF / DECIMATION);

    // 5) RX
    hackrf_start_rx(dev, rx_callback, NULL);

    printf("[C] ENTER para detener...\n");
    getchar();
    atomic_store(&g_stop, 1);

    // 6) Stop + join
    hackrf_stop_rx(dev);
    hackrf_close(dev);
    hackrf_exit();

    rb_sig_wake_all(&g_iq_rb);
    rb_sig_wake_all(&g_pcm_rb);

    pthread_join(th_dsp, NULL);
    pthread_join(th_net, NULL);

    // 7) Cleanup
    opus_tx_destroy(g_tx);
    rb_sig_free(&g_iq_rb);
    rb_sig_free(&g_pcm_rb);

    printf("[C] Finalizado\n");
    return 0;
}
