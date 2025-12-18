#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float last_phase;
    float audio_gain;     // escala a int16
    int   decimation;

    // acumulador para decimación simple (promedio)
    float sum_audio;
    int   dec_counter;

    // métricas de excursión
    float dev_max_hz;
    float dev_ema_hz;
    int   dev_counter;
    float dev_ema_alpha;
    int   dev_report_samples;

    int   sample_rate_rf;
} fm_demod_t;

typedef struct {
    float dev_peak_khz;
    float dev_ema_khz;
    int   ready; // 1 si hay reporte nuevo
} fm_dev_report_t;

void  fm_demod_init(fm_demod_t *st, int sample_rate_rf, int decimation, float audio_gain);
float fm_demod_phase_diff(fm_demod_t *st, float i, float q);

// Procesa 1 muestra IQ (normalizada [-1,1]).
// Si produce audio: retorna 1 y llena out_s16.
// Si no produce audio aún: retorna 0.
int   fm_demod_process_iq(fm_demod_t *st, float i, float q, int16_t *out_s16);

// Actualiza y opcionalmente produce reporte cada N muestras RF
fm_dev_report_t fm_demod_update_deviation(fm_demod_t *st, float phase_diff);

#ifdef __cplusplus
}
#endif
