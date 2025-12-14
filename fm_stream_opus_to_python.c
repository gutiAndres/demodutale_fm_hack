// fm_stream_opus_to_python.c
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

#define FREQ_HZ            103700000
#define SAMPLE_RATE_RF     1920000
#define SAMPLE_RATE_AUDIO  48000
#define DECIMATION         40   
#define FRAME_MS           20
#define FRAME_SAMPLES      ((SAMPLE_RATE_AUDIO * FRAME_MS) / 1000)

#define PY_HOST            "127.0.0.1"
#define PY_PORT            9000

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t payload_len;
} OpusFrameHeader;
#pragma pack(pop)

/* ===================== ESTADO GLOBAL ===================== */

static float last_phase = 0.0f;
static int sock_fd = -1;
static volatile int g_stop = 0;

static uint32_t g_seq = 0;
static int16_t  g_pcm_frame[FRAME_SAMPLES];
static int      g_pcm_idx = 0;

static const float g_audio_gain = 8000.0f;
static OpusEncoder* g_enc = NULL;

/* ===================== OPUS ===================== */

static const int OPUS_BITRATE = 64000;
static const int OPUS_COMPLEXITY = 5;
static const int OPUS_VBR = 1;

/* ===================== ESTIMACIÓN EXCURSIÓN FM ===================== */

static float g_fm_dev_max = 0.0f;     // pico ventana (Hz)
static float g_fm_dev_ema = 0.0f;     // suavizado (Hz)
static int   g_dev_counter = 0;

#define DEV_EMA_ALPHA 0.01f
#define DEV_REPORT_SAMPLES (SAMPLE_RATE_RF / 10)  // 100 ms

static inline float phase_diff_to_hz(float dphi) {
    return (dphi * SAMPLE_RATE_RF) / (2.0f * M_PI);
}

static inline void update_fm_deviation(float phase_diff) {
    float fi_hz = fabsf(phase_diff_to_hz(phase_diff));

    if (fi_hz > g_fm_dev_max)
        g_fm_dev_max = fi_hz;

    g_fm_dev_ema = (1.0f - DEV_EMA_ALPHA) * g_fm_dev_ema
                   + DEV_EMA_ALPHA * fi_hz;

    g_dev_counter++;

    if (g_dev_counter >= DEV_REPORT_SAMPLES) {
        printf("[FM] Excursion pico: %.1f kHz | EMA: %.1f kHz\n",
               g_fm_dev_max / 1e3,
               g_fm_dev_ema / 1e3);

        g_fm_dev_max = 0.0f;
        g_dev_counter = 0;
    }
}

/* ===================== FM DEMOD ===================== */

static float demodulate_fm(float i, float q) {
    float current_phase = atan2f(q, i);
    float phase_diff = current_phase - last_phase;

    if (phase_diff > M_PI)  phase_diff -= 2.0f * M_PI;
    if (phase_diff < -M_PI) phase_diff += 2.0f * M_PI;

    last_phase = current_phase;
    return phase_diff;
}

static inline int16_t float_to_i16(float x) {
    float y = x * g_audio_gain;
    if (y > 32767.0f) y = 32767.0f;
    if (y < -32768.0f) y = -32768.0f;
    return (int16_t)lrintf(y);
}

/* ===================== TCP ===================== */

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
    h.magic = htonl(0x4F505530);
    h.seq = htonl(g_seq++);
    h.sample_rate = htonl(SAMPLE_RATE_AUDIO);
    h.channels = htons(1);
    h.payload_len = htons(len);

    if (send_all(sock_fd, &h, sizeof(h)) != 0) return -1;
    if (send_all(sock_fd, opus, len) != 0) return -1;
    return 0;
}

/* ===================== HACKRF RX ===================== */

static int rx_callback(hackrf_transfer* transfer) {
    if (g_stop) return 0;

    int8_t* buf = (int8_t*)transfer->buffer;
    int count = transfer->valid_length / 2;

    float sum_audio = 0.0f;
    int dec_counter = 0;
    uint8_t opus_out[1500];

    for (int j = 0; j < count; j++) {
        float i = (float)buf[2*j]     / 128.0f;
        float q = (float)buf[2*j + 1] / 128.0f;

        float fm = demodulate_fm(i, q);

        /* ---- ESTIMACIÓN EXCURSIÓN FM ---- */
        update_fm_deviation(fm);

        sum_audio += fm;
        dec_counter++;

        if (dec_counter == DECIMATION) {
            float audio_out = sum_audio / (float)DECIMATION;
            g_pcm_frame[g_pcm_idx++] = float_to_i16(audio_out);

            if (g_pcm_idx == FRAME_SAMPLES) {
                int n = opus_encode(g_enc, g_pcm_frame, FRAME_SAMPLES,
                                    opus_out, sizeof(opus_out));
                if (n < 0) {
                    fprintf(stderr, "[C] opus_encode error: %s\n",
                            opus_strerror(n));
                    g_stop = 1;
                    break;
                }

                if (send_opus_packet(opus_out, (uint16_t)n) != 0) {
                    fprintf(stderr, "[C] Error enviando Opus\n");
                    g_stop = 1;
                    break;
                }

                g_pcm_idx = 0;
            }

            sum_audio = 0.0f;
            dec_counter = 0;
        }

        if (g_stop) break;
    }

    return 0;
}

/* ===================== MAIN ===================== */

static int connect_to_python(void) {
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) return -1;

    struct sockaddr_in addr = {0};
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

int main(void) {
    printf("[C] Conectando a Python %s:%d ...\n", PY_HOST, PY_PORT);
    if (connect_to_python() != 0) {
        fprintf(stderr, "[C] No pude conectar a Python\n");
        return 1;
    }

    int err = 0;
    g_enc = opus_encoder_create(SAMPLE_RATE_AUDIO, 1,
                                OPUS_APPLICATION_AUDIO, &err);
    opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(g_enc, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    opus_encoder_ctl(g_enc, OPUS_SET_VBR(OPUS_VBR));

    hackrf_init();
    hackrf_device* device = NULL;
    hackrf_open(&device);

    hackrf_set_sample_rate(device, SAMPLE_RATE_RF);
    hackrf_set_freq(device, FREQ_HZ);
    hackrf_set_lna_gain(device, 32);
    hackrf_set_vga_gain(device, 28);
    hackrf_set_amp_enable(device, 0);

    printf("[C] RX FM %.1f MHz\n", (float)FREQ_HZ / 1e6);
    hackrf_start_rx(device, rx_callback, NULL);

    printf("[C] ENTER para detener...\n");
    getchar();

    g_stop = 1;
    hackrf_stop_rx(device);
    hackrf_close(device);
    hackrf_exit();

    if (sock_fd >= 0) close(sock_fd);
    opus_encoder_destroy(g_enc);

    printf("[C] Finalizado\n");
    return 0;
}
