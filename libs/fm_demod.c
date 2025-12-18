#include "fm_demod.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline int16_t float_to_i16(float x, float gain) {
    float y = x * gain;
    if (y > 32767.0f) y = 32767.0f;
    if (y < -32768.0f) y = -32768.0f;
    return (int16_t)lrintf(y);
}

static inline float phase_diff_to_hz(float dphi, int fs_rf) {
    return (dphi * (float)fs_rf) / (2.0f * (float)M_PI);
}

void fm_demod_init(fm_demod_t *st, int sample_rate_rf, int decimation, float audio_gain) {
    *st = (fm_demod_t){
        .last_phase = 0.0f,
        .audio_gain = audio_gain,
        .decimation = decimation,
        .sum_audio = 0.0f,
        .dec_counter = 0,
        .dev_max_hz = 0.0f,
        .dev_ema_hz = 0.0f,
        .dev_counter = 0,
        .dev_ema_alpha = 0.01f,
        .dev_report_samples = sample_rate_rf / 10, // ~100ms
        .sample_rate_rf = sample_rate_rf
    };
}

float fm_demod_phase_diff(fm_demod_t *st, float i, float q) {
    float current_phase = atan2f(q, i);
    float d = current_phase - st->last_phase;

    if (d > (float)M_PI)  d -= 2.0f * (float)M_PI;
    if (d < -(float)M_PI) d += 2.0f * (float)M_PI;

    st->last_phase = current_phase;
    return d;
}

int fm_demod_process_iq(fm_demod_t *st, float i, float q, int16_t *out_s16) {
    float dphi = fm_demod_phase_diff(st, i, q);

    st->sum_audio += dphi;
    st->dec_counter++;

    if (st->dec_counter == st->decimation) {
        float audio = st->sum_audio / (float)st->decimation;
        *out_s16 = float_to_i16(audio, st->audio_gain);
        st->sum_audio = 0.0f;
        st->dec_counter = 0;
        return 1;
    }
    return 0;
}

fm_dev_report_t fm_demod_update_deviation(fm_demod_t *st, float phase_diff) {
    fm_dev_report_t r = {0};

    float fi_hz = fabsf(phase_diff_to_hz(phase_diff, st->sample_rate_rf));
    if (fi_hz > st->dev_max_hz) st->dev_max_hz = fi_hz;

    st->dev_ema_hz = (1.0f - st->dev_ema_alpha) * st->dev_ema_hz
                   + (st->dev_ema_alpha) * fi_hz;

    st->dev_counter++;
    if (st->dev_counter >= st->dev_report_samples) {
        r.dev_peak_khz = st->dev_max_hz / 1e3f;
        r.dev_ema_khz  = st->dev_ema_hz / 1e3f;
        r.ready = 1;

        st->dev_max_hz = 0.0f;
        st->dev_counter = 0;
    }
    return r;
}
