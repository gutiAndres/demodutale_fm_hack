//libs/sdr_HAL.h
#ifndef SDR_HAL_H
#define SDR_HAL_H

#include <stdint.h>
#include <stdbool.h>
#include <libhackrf/hackrf.h>

#ifndef TO_MHZ
#define TO_MHZ(x) ((int64_t)(x) * 1000000)
#endif

typedef struct {
    double sample_rate;
    uint64_t center_freq;
    bool amp_enabled;
    int lna_gain;
    int vga_gain;
    int ppm_error;
} SDR_cfg_t;

void hackrf_apply_cfg(hackrf_device* dev, SDR_cfg_t *cfg);

#endif