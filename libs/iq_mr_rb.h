#pragma once
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

typedef enum {
    IQ_READER_DEMOD = 0,
    IQ_READER_PSD   = 1
} iq_reader_t;

typedef struct {
    uint8_t *buf;
    size_t   size;

    size_t   head;        // monotónico
    size_t   tail_demod;  // monotónico
    size_t   tail_psd;    // monotónico

    uint64_t drop_demod_bytes;
    uint64_t drop_psd_bytes;

    pthread_mutex_t mtx;
    pthread_cond_t  cv;
} iq_mr_rb_t;

int    iq_mr_init(iq_mr_rb_t *r, size_t size_bytes);
void   iq_mr_free(iq_mr_rb_t *r);

// Escritura no bloqueante. Si no hay espacio, dropea al lector más atrasado (por defecto PSD primero).
size_t iq_mr_write(iq_mr_rb_t *r, const void *data, size_t len);

// Lectura bloqueante por lector (cada lector avanza su propio tail).
size_t iq_mr_read_blocking(iq_mr_rb_t *r, iq_reader_t who, void *out, size_t len,
                           const atomic_int *stop_flag);

// Lectura no bloqueante por lector
size_t iq_mr_read(iq_mr_rb_t *r, iq_reader_t who, void *out, size_t len);

// Bytes disponibles para cada lector
size_t iq_mr_available(iq_mr_rb_t *r, iq_reader_t who);

// Despertar a todos los que estén bloqueados
void   iq_mr_wake_all(iq_mr_rb_t *r);

// Métricas
uint64_t iq_mr_drops(iq_mr_rb_t *r, iq_reader_t who);
