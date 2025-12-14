// am_stream_opus_to_python.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <libhackrf/hackrf.h>
#include <opus/opus.h>

#define FREQ_HZ            152000000
#define SAMPLE_RATE_RF     1920000
#define SAMPLE_RATE_AUDIO  48000
#define DECIMATION         40

#define FRAME_MS           20
#define FRAME_SAMPLES      ((SAMPLE_RATE_AUDIO * FRAME_MS) / 1000) // 960

#define PY_HOST            "127.0.0.1"
#define PY_PORT            9000

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;       // 'OPU0' = 0x4F505530
    uint32_t seq;         // frame sequence
    uint32_t sample_rate; // 48000
    uint16_t channels;    // 1
    uint16_t payload_len; // opus bytes
} OpusFrameHeader;
#pragma pack(pop)

/* ===================== ESTADO GLOBAL ===================== */

static int sock_fd = -1;
static volatile int g_stop = 0;

static uint32_t g_seq = 0;
static int16_t  g_pcm_frame[FRAME_SAMPLES];
static int      g_pcm_idx = 0;

// Ajusta según volumen percibido en AM (suele requerir más ganancia que FM)
static const float g_audio_gain = 12000.0f;

static OpusEncoder* g_enc = NULL;

// Ajustes Opus
static const int OPUS_BITRATE = 64000;   // 64 kbps
static const int OPUS_COMPLEXITY = 5;    // 0..10
static const int OPUS_VBR = 1;           // 1=VBR

/* ===================== UTILIDADES ===================== */

static inline int16_t float_to_i16(float x) {
    float y = x * g_audio_gain;
    if (y > 32767.0f) y = 32767.0f;
    if (y < -32768.0f) y = -32768.0f;
    return (int16_t)lrintf(y);
}

static int send_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t*)buf;
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0) return -1;
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

static int send_opus_packet(const uint8_t* opus, uint16_t len) {
    OpusFrameHeader h;
    h.magic = htonl(0x4F505530); // 'OPU0'
    h.seq = htonl(g_seq++);
    h.sample_rate = htonl(SAMPLE_RATE_AUDIO);
    h.channels = htons(1);
    h.payload_len = htons(len);

    if (send_all(sock_fd, &h, sizeof(h)) != 0) return -1;
    if (send_all(sock_fd, opus, len) != 0) return -1;
    return 0;
}

/* ===================== DC REMOVAL EN IQ (RECOMENDADO) ===================== */

// EMA para estimar DC en I y Q (muy típico en SDR)
static float g_dc_i = 0.0f, g_dc_q = 0.0f;
#define DC_ALPHA 0.001f  // más bajo = más lento; típico 1e-3..1e-4

static inline void dc_remove_iq(float *i, float *q) {
    g_dc_i = (1.0f - DC_ALPHA) * g_dc_i + DC_ALPHA * (*i);
    g_dc_q = (1.0f - DC_ALPHA) * g_dc_q + DC_ALPHA * (*q);
    *i -= g_dc_i;
    *q -= g_dc_q;
}

/* ===================== DEMODULACIÓN AM ===================== */

// Envolvente AM: A[n] = sqrt(I^2 + Q^2)
// Nota: esto produce señal positiva con un gran componente DC (portadora).
static inline float demodulate_am_envelope(float i, float q) {
    return sqrtf(i*i + q*q);
}

// Para audio AM: remover componente DC de la envolvente (AC coupling)
static float g_env_mean = 0.0f;
#define ENV_MEAN_ALPHA 0.0005f  // controla qué tan lento sigue la portadora

static inline float envelope_to_audio(float env) {
    g_env_mean = (1.0f - ENV_MEAN_ALPHA) * g_env_mean + ENV_MEAN_ALPHA * env;
    return (env - g_env_mean);
}

/* ===================== ESTIMACIÓN PROFUNDIDAD (AM MODULATION DEPTH) ===================== */

// Definición típica: m = (Amax - Amin) / (Amax + Amin)
// Se estima sobre ventanas (aquí: 100 ms) usando la envolvente ya decimada a 48 kHz.
static float g_env_min = 1e9f;
static float g_env_max = 0.0f;
static float g_depth_ema = 0.0f; // suavizado de profundidad
static int   g_depth_counter = 0;

#define DEPTH_REPORT_SAMPLES (SAMPLE_RATE_AUDIO / 10)  // 100 ms @ 48k = 4800
#define DEPTH_EMA_ALPHA 0.1f                           // suavizado entre ventanas

static inline void update_am_depth_from_env(float env_decimated) {
    // proteger contra valores no finitos
    if (!isfinite(env_decimated)) return;

    if (env_decimated < g_env_min) g_env_min = env_decimated;
    if (env_decimated > g_env_max) g_env_max = env_decimated;

    g_depth_counter++;

    if (g_depth_counter >= DEPTH_REPORT_SAMPLES) {
        float denom = (g_env_max + g_env_min);
        float m = 0.0f;

        if (denom > 1e-9f) {
            m = (g_env_max - g_env_min) / denom; // 0..~1 (ideal)
            if (m < 0.0f) m = 0.0f;
            if (m > 2.0f) m = 2.0f; // clamp defensivo (por ruido)
        }

        // EMA por ventanas (estable para log)
        g_depth_ema = (1.0f - DEPTH_EMA_ALPHA) * g_depth_ema + DEPTH_EMA_ALPHA * m;

        printf("[AM] Profundidad (pico ventana): %.1f %% | EMA: %.1f %%  (Amin=%.4f Amax=%.4f)\n",
               100.0f * m,
               100.0f * g_depth_ema,
               g_env_min, g_env_max);

        // reset ventana
        g_env_min = 1e9f;
        g_env_max = 0.0f;
        g_depth_counter = 0;
    }
}

/* ===================== HACKRF RX CALLBACK ===================== */

static int rx_callback(hackrf_transfer* transfer) {
    if (g_stop) return 0;

    int8_t* buf = (int8_t*)transfer->buffer;
    int count = transfer->valid_length / 2;

    float sum_env = 0.0f;
    int dec_counter = 0;

    uint8_t opus_out[1500];

    for (int j = 0; j < count; j++) {
        float i = (float)buf[2*j]     / 128.0f;
        float q = (float)buf[2*j + 1] / 128.0f;

        // Recomendado: DC removal en IQ antes de envolvente
        dc_remove_iq(&i, &q);

        // AM envelope (a tasa RF)
        float env = demodulate_am_envelope(i, q);

        // Decimar a 48 kHz acumulando (promedio simple)
        sum_env += env;
        dec_counter++;

        if (dec_counter == DECIMATION) {
            float env_dec = sum_env / (float)DECIMATION; // envolvente a 48k

            // Estimar profundidad usando la envolvente decimada (NO el audio AC)
            update_am_depth_from_env(env_dec);

            // Audio AM = envolvente sin DC (AC-coupling)
            float audio_out = envelope_to_audio(env_dec);

            // PCM frame (Opus)
            g_pcm_frame[g_pcm_idx++] = float_to_i16(audio_out);

            if (g_pcm_idx == FRAME_SAMPLES) {
                int n = opus_encode(g_enc, g_pcm_frame, FRAME_SAMPLES,
                                    opus_out, (opus_int32)sizeof(opus_out));
                if (n < 0) {
                    fprintf(stderr, "[C] opus_encode error: %s\n", opus_strerror(n));
                    g_stop = 1;
                    break;
                }

                if (send_opus_packet(opus_out, (uint16_t)n) != 0) {
                    fprintf(stderr, "[C] Error enviando Opus a Python. Deteniendo.\n");
                    g_stop = 1;
                    break;
                }

                g_pcm_idx = 0;
            }

            sum_env = 0.0f;
            dec_counter = 0;
        }

        if (g_stop) break;
    }

    return 0;
}

/* ===================== TCP CONNECT ===================== */

static int connect_to_python(void) {
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PY_PORT);
    inet_pton(AF_INET, PY_HOST, &addr.sin_addr);

    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(sock_fd);
        sock_fd = -1;
        return -1;
    }
    return 0;
}

/* ===================== MAIN ===================== */

int main(void) {
    printf("[C] Conectando a Python bridge en %s:%d ...\n", PY_HOST, PY_PORT);
    if (connect_to_python() != 0) {
        fprintf(stderr, "[C] No pude conectar a Python. Inicia el bridge primero.\n");
        return 1;
    }
    printf("[C] Conectado.\n");

    int err = 0;
    g_enc = opus_encoder_create(SAMPLE_RATE_AUDIO, 1, OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK || !g_enc) {
        fprintf(stderr, "[C] opus_encoder_create failed: %s\n", opus_strerror(err));
        return 1;
    }
    opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(g_enc, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    opus_encoder_ctl(g_enc, OPUS_SET_VBR(OPUS_VBR));

    if (hackrf_init() != HACKRF_SUCCESS) {
        fprintf(stderr, "[C] hackrf_init failed\n");
        return 1;
    }

    hackrf_device* device = NULL;
    int status = hackrf_open(&device);
    if (status != HACKRF_SUCCESS) {
        fprintf(stderr, "[C] No se encontró HackRF (status %d)\n", status);
        hackrf_exit();
        return 1;
    }

    hackrf_set_sample_rate(device, SAMPLE_RATE_RF);
    hackrf_set_freq(device, FREQ_HZ);

    // Ganancias (ajusta según tu escenario AM real)
    hackrf_set_amp_enable(device, 0);
    hackrf_set_lna_gain(device, 32);
    hackrf_set_vga_gain(device, 28);

    printf("[C] RX AM Opus: %.1f MHz -> TCP -> Python\n", (float)FREQ_HZ / 1e6);
    status = hackrf_start_rx(device, rx_callback, NULL);
    if (status != HACKRF_SUCCESS) {
        fprintf(stderr, "[C] hackrf_start_rx failed (status %d)\n", status);
        hackrf_close(device);
        hackrf_exit();
        return 1;
    }

    printf("[C] Presiona ENTER para detener...\n");
    getchar();

    g_stop = 1;
    hackrf_stop_rx(device);
    hackrf_close(device);
    hackrf_exit();

    if (sock_fd >= 0) close(sock_fd);
    if (g_enc) opus_encoder_destroy(g_enc);

    printf("[C] Finalizado.\n");
    return 0;
}
