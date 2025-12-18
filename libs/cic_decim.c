// libs/cic_decim.c
#include "cic_decim.h"
#include <string.h>

void cic_init(cic_decim_t *c, int R, int N) {
    memset(c, 0, sizeof(*c));
    c->R = R;
    c->N = N;
    c->ctr = 0;
}

void cic_process_one(cic_decim_t *c,
                     int32_t xi, int32_t xq,
                     int32_t *yo_i, int32_t *yo_q,
                     bool *produced)
{
    /* integrator chain */
    int64_t yi = xi;
    int64_t yq = xq;

    for (int s = 0; s < c->N; s++) {
        c->int_i[s] += yi;
        c->int_q[s] += yq;
        yi = c->int_i[s];
        yq = c->int_q[s];
    }

    c->ctr++;
    if (c->ctr < c->R) {
        *produced = false;
        return;
    }
    c->ctr = 0;

    /* comb chain at output rate */
    for (int s = 0; s < c->N; s++) {
        int64_t prev_i = c->comb_i[s];
        int64_t prev_q = c->comb_q[s];
        c->comb_i[s] = yi;
        c->comb_q[s] = yq;
        yi = yi - prev_i;
        yq = yq - prev_q;
    }

    /* normalize by CIC gain ~ R^N */
    int64_t gain = 1;
    for (int k = 0; k < c->N; k++) gain *= c->R;
    if (gain <= 0) gain = 1;

    yi /= gain;
    yq /= gain;

    /* clamp to int8-ish range (stored in int32) */
    if (yi > 127) yi = 127;
    if (yi < -128) yi = -128;
    if (yq > 127) yq = 127;
    if (yq < -128) yq = -128;

    *yo_i = (int32_t)yi;
    *yo_q = (int32_t)yq;
    *produced = true;
}
