#include "hackrf_rx.h"
#include "sdr_app.h"
#include <stdatomic.h>

int rx_callback(hackrf_transfer* transfer) {
    sdr_app_t *app = (sdr_app_t*)transfer->rx_ctx;
    if (!app) return 0;
    if (atomic_load(&app->stop)) return 0;

    /* 1) IQ RAW -> decimator */
    size_t wraw = rb_sig_write(&app->iq_raw_rb, transfer->buffer, (size_t)transfer->valid_length);
    if (wraw < (size_t)transfer->valid_length) {
        atomic_fetch_add(&app->iq_raw_drops,
                         (unsigned long)((size_t)transfer->valid_length - wraw));
    }

    /* 2) Rama PSD (solo si activa) */
    if (app->psd_capture_active) {
        size_t w2 = rb_write(&app->psd_rb, transfer->buffer, (size_t)transfer->valid_length);
        if (w2 < (size_t)transfer->valid_length) {
            atomic_fetch_add(&app->psd_drops,
                             (unsigned long)((size_t)transfer->valid_length - w2));
        }
    }

    return 0;
}
