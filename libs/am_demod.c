// am_demod.c
#include "am_demod.h"
#include <math.h>

#define DC_ALPHA        0.001f
#define ENV_MEAN_ALPHA  0.0005f
#define DEPTH_EMA_ALPHA 0.1f

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float clampf(float x, float a, float b) {
    if (x < a) return a;
    if (x > b) return b;
    return x;
}

void am_demod_init(am_demod_t *d,
                   float fs_rf,
                   int decimation,
                   float audio_gain)
{
    *d = (am_demod_t){
        .fs_rf = fs_rf,
        .decimation = decimation,
        .audio_gain = audio_gain,
        .dc_i = 0.0f,
        .dc_q = 0.0f,
        .sum_env = 0.0f,
        .dec_counter = 0,
        .env_mean = 0.0f,
        .env_min = 1e9f,
        .env_max = 0.0f,
        .depth_ema = 0.0f,
        .depth_counter = 0
    };
}

bool am_demod_process_iq(am_demod_t *d,
                         float i, float q,
                         int16_t *pcm_out,
                         am_depth_report_t *rep)
{
    if (rep) rep->ready = false;

    /* DC removal en IQ */
    d->dc_i = (1.0f - DC_ALPHA) * d->dc_i + DC_ALPHA * i;
    d->dc_q = (1.0f - DC_ALPHA) * d->dc_q + DC_ALPHA * q;
    i -= d->dc_i;
    q -= d->dc_q;

    /* envelope */
    float env = sqrtf(i*i + q*q);

    /* decimation */
    d->sum_env += env;
    d->dec_counter++;

    if (d->dec_counter < d->decimation)
        return false;

    float env_dec = d->sum_env / (float)d->decimation;
    d->sum_env = 0.0f;
    d->dec_counter = 0;

    /* depth metrics */
    if (env_dec < d->env_min) d->env_min = env_dec;
    if (env_dec > d->env_max) d->env_max = env_dec;
    d->depth_counter++;

    const int DEPTH_REPORT_SAMPLES = 4800; // 100 ms @ 48k

    if (d->depth_counter >= DEPTH_REPORT_SAMPLES) {
        float denom = d->env_max + d->env_min;
        float m = (denom > 1e-9f)
            ? (d->env_max - d->env_min) / denom
            : 0.0f;

        m = clampf(m, 0.0f, 2.0f);
        d->depth_ema = (1.0f - DEPTH_EMA_ALPHA)*d->depth_ema
                     + DEPTH_EMA_ALPHA*m;

        if (rep) {
            rep->ready = true;
            rep->depth_peak_pct = 100.0f * m;
            rep->depth_ema_pct  = 100.0f * d->depth_ema;
            rep->env_min = d->env_min;
            rep->env_max = d->env_max;
        }

        d->env_min = 1e9f;
        d->env_max = 0.0f;
        d->depth_counter = 0;
    }

    /* AC coupling */
    d->env_mean = (1.0f - ENV_MEAN_ALPHA)*d->env_mean
                + ENV_MEAN_ALPHA*env_dec;
    float audio = env_dec - d->env_mean;

    /* scale to PCM */
    float y = audio * d->audio_gain;
    y = clampf(y, -32768.0f, 32767.0f);
    *pcm_out = (int16_t)lrintf(y);

    return true;
}
