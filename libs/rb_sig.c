#include "rb_sig.h"

int rb_sig_init(rb_sig_t *r, size_t size_bytes) {
    if (!r) return -1;
    rb_init(&r->rb, size_bytes);
    if (pthread_mutex_init(&r->mtx, NULL) != 0) return -1;
    if (pthread_cond_init(&r->cv, NULL) != 0) {
        pthread_mutex_destroy(&r->mtx);
        return -1;
    }
    return 0;
}

void rb_sig_free(rb_sig_t *r) {
    if (!r) return;
    rb_free(&r->rb);
    pthread_mutex_destroy(&r->mtx);
    pthread_cond_destroy(&r->cv);
}

size_t rb_sig_write(rb_sig_t *r, const void *data, size_t len) {
    pthread_mutex_lock(&r->mtx);
    size_t w = rb_write(&r->rb, data, len);
    if (w > 0) pthread_cond_signal(&r->cv);
    pthread_mutex_unlock(&r->mtx);
    return w;
}

size_t rb_sig_read(rb_sig_t *r, void *out, size_t len) {
    pthread_mutex_lock(&r->mtx);
    size_t n = rb_read(&r->rb, out, len);
    pthread_mutex_unlock(&r->mtx);
    return n;
}

size_t rb_sig_read_blocking(rb_sig_t *r, void *out, size_t len, const atomic_int *stop_flag) {
    pthread_mutex_lock(&r->mtx);
    while (!atomic_load(stop_flag) && rb_available(&r->rb) < len) {
        pthread_cond_wait(&r->cv, &r->mtx);
    }
    if (atomic_load(stop_flag)) {
        pthread_mutex_unlock(&r->mtx);
        return 0;
    }
    size_t n = rb_read(&r->rb, out, len);
    pthread_mutex_unlock(&r->mtx);
    return n;
}

void rb_sig_wake_all(rb_sig_t *r) {
    pthread_mutex_lock(&r->mtx);
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mtx);
}

size_t rb_sig_available(rb_sig_t *r) {
    pthread_mutex_lock(&r->mtx);
    size_t a = rb_available(&r->rb);
    pthread_mutex_unlock(&r->mtx);
    return a;
}
