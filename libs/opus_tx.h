#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct opus_tx opus_tx_t;

typedef struct {
    int sample_rate;
    int channels;
    int bitrate;
    int complexity;
    int vbr;
} opus_tx_cfg_t;

// Crea encoder + conecta TCP
opus_tx_t* opus_tx_create(const char *host, int port, const opus_tx_cfg_t *cfg);

// Encode + envía 1 frame PCM (por ejemplo 20ms a 48k => 960 samples)
int  opus_tx_send_frame(opus_tx_t *tx, const int16_t *pcm, int frame_samples);

// Cierra socket y destruye encoder
void opus_tx_destroy(opus_tx_t *tx);

// Opcional: acceso a FD o estado
int  opus_tx_fd(const opus_tx_t *tx);

#ifdef __cplusplus
}
#endif
