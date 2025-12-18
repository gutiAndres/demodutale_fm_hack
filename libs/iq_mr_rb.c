#include "iq_mr_rb.h"
#include <stdlib.h>
#include <string.h>

#define MIN(a,b) ((a)<(b)?(a):(b))

static inline size_t avail_for(const iq_mr_rb_t *r, iq_reader_t who) {
    size_t tail = (who == IQ_READER_DEMOD) ? r->tail_demod : r->tail_psd;
    return r->head - tail;
}
static inline size_t min_tail(const iq_mr_rb_t *r) {
    return (r->tail_demod < r->tail_psd) ? r->tail_demod : r->tail_psd;
}
static inline size_t used_bytes(const iq_mr_rb_t *r) {
    return r->head - min_tail(r);
}
static inline size_t free_bytes(const iq_mr_rb_t *r) {
    size_t used = used_bytes(r);
    return (used >= r->size) ? 0 : (r->size - used);
}

int iq_mr_init(iq_mr_rb_t *r, size_t size_bytes) {
    if (!r) return -1;
    memset(r, 0, sizeof(*r));
    r->buf = (uint8_t*)calloc(1, size_bytes);
    if (!r->buf) return -1;
    r->size = size_bytes;
    if (pthread_mutex_init(&r->mtx, NULL) != 0) return -1;
    if (pthread_cond_init(&r->cv, NULL) != 0) {
        pthread_mutex_destroy(&r->mtx);
        return -1;
    }
    return 0;
}

void iq_mr_free(iq_mr_rb_t *r) {
    if (!r) return;
    if (r->buf) {
        memset(r->buf, 0, r->size);
        free(r->buf);
        r->buf = NULL;
    }
    pthread_mutex_destroy(&r->mtx);
    pthread_cond_destroy(&r->cv);
}

static inline void advance_tail(iq_mr_rb_t *r, iq_reader_t who, size_t bytes) {
    if (who == IQ_READER_DEMOD) r->tail_demod += bytes;
    else r->tail_psd += bytes;
}

static inline void add_drop(iq_mr_rb_t *r, iq_reader_t who, size_t bytes) {
    if (who == IQ_READER_DEMOD) r->drop_demod_bytes += bytes;
    else r->drop_psd_bytes += bytes;
}

// Política: si falta espacio, dropear al lector más atrasado.
// En empate o si PSD es el lento, dropeamos PSD primero (protege demod).
static inline iq_reader_t choose_victim(const iq_mr_rb_t *r) {
    // el más atrasado es el que tiene tail más pequeño
    if (r->tail_psd <= r->tail_demod) return IQ_READER_PSD;
    return IQ_READER_DEMOD;
}

size_t iq_mr_write(iq_mr_rb_t *r, const void *data, size_t len) {
    pthread_mutex_lock(&r->mtx);

    // si el productor escribe más que el buffer, nos quedamos con el final (últimos size bytes)
    if (len > r->size) {
        data = (const uint8_t*)data + (len - r->size);
        len = r->size;
    }

    size_t freeb = free_bytes(r);
    if (freeb < len) {
        size_t need = len - freeb;

        // liberar espacio avanzando tails (dropping) del consumidor lento
        while (need > 0) {
            iq_reader_t victim = choose_victim(r);
            size_t victim_av = avail_for(r, victim);
            if (victim_av == 0) break; // nada que dropear (caso raro)

            size_t step = MIN(need, victim_av);
            advance_tail(r, victim, step);
            add_drop(r, victim, step);
            need -= step;
        }
    }

    // escribir len bytes circular
    size_t head_idx = r->head % r->size;
    size_t chunk1 = MIN(len, r->size - head_idx);
    size_t chunk2 = len - chunk1;

    memcpy(r->buf + head_idx, data, chunk1);
    if (chunk2) memcpy(r->buf, (const uint8_t*)data + chunk1, chunk2);

    r->head += len;

    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mtx);
    return len;
}

size_t iq_mr_read(iq_mr_rb_t *r, iq_reader_t who, void *out, size_t len) {
    pthread_mutex_lock(&r->mtx);

    size_t av = avail_for(r, who);
    size_t to_read = MIN(len, av);
    if (to_read == 0) {
        pthread_mutex_unlock(&r->mtx);
        return 0;
    }

    size_t tail = (who == IQ_READER_DEMOD) ? r->tail_demod : r->tail_psd;
    size_t tail_idx = tail % r->size;

    size_t chunk1 = MIN(to_read, r->size - tail_idx);
    size_t chunk2 = to_read - chunk1;

    memcpy(out, r->buf + tail_idx, chunk1);
    if (chunk2) memcpy((uint8_t*)out + chunk1, r->buf, chunk2);

    advance_tail(r, who, to_read);

    pthread_mutex_unlock(&r->mtx);
    return to_read;
}

size_t iq_mr_read_blocking(iq_mr_rb_t *r, iq_reader_t who, void *out, size_t len,
                           const atomic_int *stop_flag)
{
    pthread_mutex_lock(&r->mtx);

    while (!atomic_load(stop_flag) && avail_for(r, who) < len) {
        pthread_cond_wait(&r->cv, &r->mtx);
    }
    if (atomic_load(stop_flag)) {
        pthread_mutex_unlock(&r->mtx);
        return 0;
    }

    // ya hay len bytes disponibles
    size_t tail = (who == IQ_READER_DEMOD) ? r->tail_demod : r->tail_psd;
    size_t tail_idx = tail % r->size;

    size_t chunk1 = MIN(len, r->size - tail_idx);
    size_t chunk2 = len - chunk1;

    memcpy(out, r->buf + tail_idx, chunk1);
    if (chunk2) memcpy((uint8_t*)out + chunk1, r->buf, chunk2);

    advance_tail(r, who, len);

    pthread_mutex_unlock(&r->mtx);
    return len;
}

size_t iq_mr_available(iq_mr_rb_t *r, iq_reader_t who) {
    pthread_mutex_lock(&r->mtx);
    size_t av = avail_for(r, who);
    pthread_mutex_unlock(&r->mtx);
    return av;
}

void iq_mr_wake_all(iq_mr_rb_t *r) {
    pthread_mutex_lock(&r->mtx);
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mtx);
}

uint64_t iq_mr_drops(iq_mr_rb_t *r, iq_reader_t who) {
    pthread_mutex_lock(&r->mtx);
    uint64_t v = (who == IQ_READER_DEMOD) ? r->drop_demod_bytes : r->drop_psd_bytes;
    pthread_mutex_unlock(&r->mtx);
    return v;
}
