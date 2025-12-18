// libs/pipeline_threads.h
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "rb_sig.h"
#include "ring_buffer.h"

#include "fm_demod.h"
#include "am_demod.h"
#include "opus_tx.h"

#include "psd.h"
#include "datatypes.h"
#include "sdr_HAL.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { DEMOD_FM = 1, DEMOD_AM = 2 } demod_mode_t;

typedef struct {
    /* stop flag owned by main, but used by threads */
    atomic_int *stop;

    /* demod mode */
    demod_mode_t mode;

    /* runtime params */
    int sample_rate_rf_in;
    int sample_rate_demod;
    int decim_factor;
    int sample_rate_audio;
    int decimation_audio;

    int frame_samples;

    /* RBs */
    rb_sig_t *iq_raw_rb;
    rb_sig_t *iq_demod_rb;
    rb_sig_t *pcm_rb;

    /* drops counters */
    atomic_ulong *iq_raw_drops;
    atomic_ulong *iq_demod_drops;
    atomic_ulong *pcm_drops;

    /* PSD RB + control */
    ring_buffer_t *psd_rb;
    volatile bool *psd_capture_active;
    atomic_ulong *psd_drops;

    /* Opus tx */
    opus_tx_t *tx;

    /* Demod parameters */
    float fm_audio_bw_or_deemph;
    float am_audio_bw;
    

    /* PSD pipeline config */
    DesiredCfg_t *desired_cfg;
    SDR_cfg_t    *hack_cfg;
    PsdConfig_t  *psd_cfg;
    RB_cfg_t     *rb_cfg;

    /* Outputs */
    const char *psd_csv_path;

    /* PSD loop params */
    int  psd_wait_timeout_iters;
    int  psd_wait_sleep_us;
    int  psd_post_sleep_us;
} pipeline_ctx_t;

typedef struct {
    pthread_t th_decim;
    pthread_t th_demod;
    pthread_t th_net;
    pthread_t th_psd;
    bool started_decim;
    bool started_demod;
    bool started_net;
    bool started_psd;
} pipeline_threads_t;



/* start all pipeline threads */
int pipeline_threads_start(pipeline_threads_t *t, pipeline_ctx_t *ctx);

/* request stop and wake RBs (call from main) */
void pipeline_threads_stop(pipeline_ctx_t *ctx);

/* join all started threads */
void pipeline_threads_join(pipeline_threads_t *t);

#ifdef __cplusplus
}
#endif
