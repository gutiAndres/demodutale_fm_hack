//libs/ring_buffer.h
#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

typedef struct {
    uint8_t *buffer;
    size_t size;
    size_t head;
    size_t tail;
    pthread_mutex_t lock;
} ring_buffer_t;

void rb_init(ring_buffer_t *rb, size_t size);
void rb_free(ring_buffer_t *rb);
void rb_reset(ring_buffer_t *rb);
size_t rb_write(ring_buffer_t *rb, const void *data, size_t len);
size_t rb_read(ring_buffer_t *rb, void *data, size_t len);
size_t rb_available(ring_buffer_t *rb);

#endif