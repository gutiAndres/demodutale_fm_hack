#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <libhackrf/hackrf.h>

#include "rb_sig.h"
#include "ring_buffer.h"

#include "opus_tx.h"
#include "fm_demod.h"
#include "am_demod.h"

#include "psd.h"
#include "datatypes.h"
#include "sdr_HAL.h"

typedef enum { DEMOD_FM = 1, DEMOD_AM = 2 } demod_mode_t;

typedef struct {
    /* runtime control */
    atomic_int stop;

    /* mode */
    demod_mode_t mode;

    /* rates */
    int freq_hz;
    int fs_in;
    int fs_demod;
    int fs_audio;
    int decim_factor;         /* fs_in/fs_demod */
    int decim_audio;          /* fs_demod/fs_audio */

    /* demod params */
    float fm_audio_bw;
    float am_audio_bw;

    /* network */
    const char *py_host;
    int py_port;

    /* buffers sizes */
    size_t iq_raw_rb_bytes;
    size_t iq_demod_rb_bytes;
    size_t pcm_rb_bytes;
    size_t psd_rb_bytes;

    /* PSD output */
    const char *psd_csv_path;

    /* PSD thread pacing */
    int psd_wait_timeout_iters;
    int psd_wait_sleep_us;
    int psd_post_sleep_us;

} app_cfg_t;

typedef struct {
    /* device */
    hackrf_device *dev;

    /* opus */
    opus_tx_t *tx;

    /* RBs */
    rb_sig_t iq_raw_rb;
    rb_sig_t iq_demod_rb;
    rb_sig_t pcm_rb;

    ring_buffer_t psd_rb;
    volatile bool psd_capture_active;

    /* drops */
    atomic_ulong iq_raw_drops;
    atomic_ulong iq_demod_drops;
    atomic_ulong pcm_drops;
    atomic_ulong psd_drops;

    /* PSD pipeline config (tu stack existente) */
    DesiredCfg_t desired_cfg;
    SDR_cfg_t    hack_cfg;
    PsdConfig_t  psd_cfg;
    RB_cfg_t     rb_cfg;

    /* threads */
    pthread_t th_decim;
    pthread_t th_demod;
    pthread_t th_net;
    pthread_t th_psd;

    /* cfg copy */
    app_cfg_t cfg;

} sdr_app_t;

const char* mode_str(demod_mode_t m);

/* lifecycle */
int  sdr_app_init(sdr_app_t *app, const app_cfg_t *cfg);
int  sdr_app_start_threads(sdr_app_t *app);
int  sdr_app_start_rx(sdr_app_t *app);
void sdr_app_request_stop(sdr_app_t *app);
void sdr_app_join(sdr_app_t *app);
void sdr_app_cleanup(sdr_app_t *app);
