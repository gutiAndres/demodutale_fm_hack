// libs/cic_decim.h
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int R;              /* decimation factor */
    int N;              /* number of stages */
    int ctr;            /* sample counter */
    int64_t int_i[4];   /* integrator states I (N<=4) */
    int64_t int_q[4];   /* integrator states Q */
    int64_t comb_i[4];  /* comb delay states I */
    int64_t comb_q[4];  /* comb delay states Q */
} cic_decim_t;

/* Initialize CIC decimator (expects N <= 4, R >= 2) */
void cic_init(cic_decim_t *c, int R, int N);

/*
  Process one interleaved IQ sample in int32 domain, produce decimated output
  every R inputs.
  - xi/xq: input (e.g., promoted from int8)
  - yo_i/yo_q: output (int32) (typically clamped)
  - produced: true when an output sample is produced
*/
void cic_process_one(cic_decim_t *c,
                     int32_t xi, int32_t xq,
                     int32_t *yo_i, int32_t *yo_q,
                     bool *produced);

#ifdef __cplusplus
}
#endif
