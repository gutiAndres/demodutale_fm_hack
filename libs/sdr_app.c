#include "sdr_app.h"
#include <stdio.h>
#include <string.h>

#include "hackrf_rx.h"
#include "threads.h"   /* <-- un solo include para todos los hilos */


const char* mode_str(demod_mode_t m) {
    switch (m) {
        case DEMOD_AM: return "AM";
        case DEMOD_FM: return "FM";
        default: return "UNKNOWN";
    }
}

static int validate_cfg(const app_cfg_t *c) {
    if (!c) return -1;
    if (c->fs_in <= 0 || c->fs_demod <= 0 || c->fs_audio <= 0) return -1;
    if ((c->fs_in % c->fs_demod) != 0) {
        fprintf(stderr, "[CFG] ERROR: fs_in must be multiple of fs_demod\n");
        return -1;
    }
    if ((c->fs_demod % c->fs_audio) != 0) {
        fprintf(stderr, "[CFG] ERROR: fs_demod must be divisible by fs_audio\n");
        return -1;
    }
    return 0;
}

int sdr_app_init(sdr_app_t *app, const app_cfg_t *cfg) {
    if (!app || !cfg) return -1;
    memset(app, 0, sizeof(*app));
    if (validate_cfg(cfg) != 0) return -1;

    app->cfg = *cfg;
    atomic_store(&app->stop, 0);
    app->mode = cfg->mode;
    app->psd_capture_active = false;

    atomic_store(&app->iq_raw_drops, 0);
    atomic_store(&app->iq_demod_drops, 0);
    atomic_store(&app->pcm_drops, 0);
    atomic_store(&app->psd_drops, 0);

    /* init RBs */
    rb_sig_init(&app->iq_raw_rb,   app->cfg.iq_raw_rb_bytes);
    rb_sig_init(&app->iq_demod_rb, app->cfg.iq_demod_rb_bytes);
    rb_sig_init(&app->pcm_rb,      app->cfg.pcm_rb_bytes);
    rb_init(&app->psd_rb,          app->cfg.psd_rb_bytes);

    return 0;
}

int sdr_app_start_threads(sdr_app_t *app) {
    if (!app) return -1;

    if (pthread_create(&app->th_decim, NULL, decim_thread_fn, app) != 0) return -1;
    if (pthread_create(&app->th_demod, NULL, demod_thread_fn, app) != 0) return -1;
    if (pthread_create(&app->th_net,   NULL, net_thread_fn,   app) != 0) return -1;
    if (pthread_create(&app->th_psd,   NULL, psd_thread_fn,   app) != 0) return -1;

    return 0;
}

int sdr_app_start_rx(sdr_app_t *app) {
    if (!app || !app->dev) return -1;
    if (hackrf_start_rx(app->dev, rx_callback, app) != HACKRF_SUCCESS) {
        fprintf(stderr, "[MAIN] hackrf_start_rx failed\n");
        return -1;
    }
    return 0;
}

void sdr_app_request_stop(sdr_app_t *app) {
    if (!app) return;
    atomic_store(&app->stop, 1);

    rb_sig_wake_all(&app->iq_raw_rb);
    rb_sig_wake_all(&app->iq_demod_rb);
    rb_sig_wake_all(&app->pcm_rb);
}

void sdr_app_join(sdr_app_t *app) {
    if (!app) return;
    pthread_join(app->th_decim, NULL);
    pthread_join(app->th_demod, NULL);
    pthread_join(app->th_net, NULL);
    pthread_join(app->th_psd, NULL);
}

void sdr_app_cleanup(sdr_app_t *app) {
    if (!app) return;

    if (app->tx) {
        opus_tx_destroy(app->tx);
        app->tx = NULL;
    }

    rb_sig_free(&app->iq_raw_rb);
    rb_sig_free(&app->iq_demod_rb);
    rb_sig_free(&app->pcm_rb);
    rb_free(&app->psd_rb);
}
