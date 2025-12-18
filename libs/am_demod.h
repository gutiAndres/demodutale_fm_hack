// am_demod.h
#ifndef AM_DEMOD_H
#define AM_DEMOD_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    // configuración
    float fs_rf;
    int   decimation;
    float audio_gain;

    // DC removal IQ
    float dc_i;
    float dc_q;

    // envelope + decimation
    float sum_env;
    int   dec_counter;

    // AC coupling
    float env_mean;

    // métricas profundidad
    float env_min;
    float env_max;
    float depth_ema;
    int   depth_counter;

} am_demod_t;

typedef struct {
    bool  ready;
    float depth_peak_pct;
    float depth_ema_pct;
    float env_min;
    float env_max;
} am_depth_report_t;

/* API */
void am_demod_init(am_demod_t *d,
                   float fs_rf,
                   int decimation,
                   float audio_gain);

/* procesa una muestra IQ
 * devuelve true si produjo 1 muestra PCM */
bool am_demod_process_iq(am_demod_t *d,
                         float i, float q,
                         int16_t *pcm_out,
                         am_depth_report_t *rep);

#endif
