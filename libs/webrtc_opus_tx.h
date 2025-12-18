#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct webrtc_opus_tx webrtc_opus_tx_t;

typedef struct {
    const char *signaling_host;   /* "127.0.0.1" */
    int         signaling_port;   /* 8008 */
    const char *signaling_path;   /* "/offer" (server recibe POST) */

    /* Opus */
    int sample_rate;              /* 48000 */
    int channels;                 /* 1 */
    int bitrate;                  /* 64000 */
    int complexity;               /* 5 */
    int vbr;                      /* 1 */

    /* RTP */
    uint8_t payload_type;         /* 111 típico para Opus */
    uint32_t ssrc;                /* 0 -> random */
    int frame_ms;                 /* 20 */
} webrtc_opus_tx_cfg_t;

/* Crea y conecta: hace offer/answer vía HTTP con el server y deja la track lista */
webrtc_opus_tx_t* webrtc_opus_tx_create(const webrtc_opus_tx_cfg_t *cfg);

/* Envía un frame PCM (16-bit signed interleaved) de exactamente frame_samples */
int webrtc_opus_tx_send_pcm(webrtc_opus_tx_t *tx, const int16_t *pcm, int frame_samples);

/* Cierra y libera */
void webrtc_opus_tx_destroy(webrtc_opus_tx_t *tx);

#ifdef __cplusplus
}
#endif
