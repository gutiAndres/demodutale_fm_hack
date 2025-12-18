#include "opus_tx.h"
#include <opus/opus.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;       // 'OPU0' = 0x4F505530
    uint32_t seq;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t payload_len;
} OpusFrameHeader;
#pragma pack(pop)

struct opus_tx {
    int sock_fd;
    uint32_t seq;
    OpusEncoder *enc;
    opus_tx_cfg_t cfg;
};

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

static int connect_tcp(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

opus_tx_t* opus_tx_create(const char *host, int port, const opus_tx_cfg_t *cfg) {
    if (!host || !cfg) return NULL;

    opus_tx_t *tx = (opus_tx_t*)calloc(1, sizeof(*tx));
    if (!tx) return NULL;

    tx->cfg = *cfg;
    tx->sock_fd = connect_tcp(host, port);
    if (tx->sock_fd < 0) {
        free(tx);
        return NULL;
    }

    int err = 0;
    tx->enc = opus_encoder_create(cfg->sample_rate, cfg->channels, OPUS_APPLICATION_AUDIO, &err);
    if (!tx->enc || err != OPUS_OK) {
        close(tx->sock_fd);
        free(tx);
        return NULL;
    }

    opus_encoder_ctl(tx->enc, OPUS_SET_BITRATE(cfg->bitrate));
    opus_encoder_ctl(tx->enc, OPUS_SET_COMPLEXITY(cfg->complexity));
    opus_encoder_ctl(tx->enc, OPUS_SET_VBR(cfg->vbr));

    tx->seq = 0;
    return tx;
}

int opus_tx_send_frame(opus_tx_t *tx, const int16_t *pcm, int frame_samples) {
    if (!tx || !pcm) return -1;

    uint8_t opus_out[1500];
    int n = opus_encode(tx->enc, pcm, frame_samples, opus_out, (opus_int32)sizeof(opus_out));
    if (n < 0) return -1;

    OpusFrameHeader h;
    h.magic      = htonl(0x4F505530);
    h.seq        = htonl(tx->seq++);
    h.sample_rate= htonl((uint32_t)tx->cfg.sample_rate);
    h.channels   = htons((uint16_t)tx->cfg.channels);
    h.payload_len= htons((uint16_t)n);

    if (send_all(tx->sock_fd, &h, sizeof(h)) != 0) return -1;
    if (send_all(tx->sock_fd, opus_out, (size_t)n) != 0) return -1;
    return 0;
}

void opus_tx_destroy(opus_tx_t *tx) {
    if (!tx) return;
    if (tx->sock_fd >= 0) close(tx->sock_fd);
    if (tx->enc) opus_encoder_destroy(tx->enc);
    free(tx);
}

int opus_tx_fd(const opus_tx_t *tx) {
    return tx ? tx->sock_fd : -1;
}
