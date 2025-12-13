// ringbuffer.h
#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <stdint.h>

// Tamaño buffer: Potencia de 2. A 48kHz, 65536 son ~1.3 segundos de buffer.
#define RB_SIZE 65536 

typedef struct {
    float buffer[RB_SIZE];
    volatile int head;
    volatile int tail;
    volatile int count;
} RingBuffer;

void rb_init(RingBuffer *rb) {
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
}

void rb_write(RingBuffer *rb, float sample) {
    int next = (rb->head + 1) % RB_SIZE;
    
    // Si se llena, sobrescribimos (o descartamos). Aquí descartamos para no romper el puntero.
    if (next == rb->tail) return; 

    rb->buffer[rb->head] = sample;
    rb->head = next;
    // Incremento atómico simple
    __sync_fetch_and_add(&rb->count, 1);
}

float rb_read(RingBuffer *rb) {
    if (rb->head == rb->tail) return 0.0f; // Silencio si está vacío

    float sample = rb->buffer[rb->tail];
    rb->tail = (rb->tail + 1) % RB_SIZE;
    __sync_fetch_and_sub(&rb->count, 1);
    return sample;
}

int rb_available(RingBuffer *rb) {
    return rb->count;
}

#endif
