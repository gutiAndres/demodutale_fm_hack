#pragma once
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>
#include "ring_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ring_buffer_t rb;        // tu ring buffer real
    pthread_mutex_t mtx;     // coordina condvar + operaciones
    pthread_cond_t  cv;      // señalización a consumidores
} rb_sig_t;

int    rb_sig_init(rb_sig_t *r, size_t size_bytes);
void   rb_sig_free(rb_sig_t *r);

size_t rb_sig_write(rb_sig_t *r, const void *data, size_t len);
size_t rb_sig_read(rb_sig_t *r, void *out, size_t len);

// Espera hasta tener >= len o stop_flag=1. Retorna 0 si stop.
size_t rb_sig_read_blocking(rb_sig_t *r, void *out, size_t len, const atomic_int *stop_flag);

// Despierta consumidores bloqueados
void   rb_sig_wake_all(rb_sig_t *r);

// Helpers opcionales
size_t rb_sig_available(rb_sig_t *r);

#ifdef __cplusplus
}
#endif
