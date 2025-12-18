//libs/pfb.h
#ifndef PFB_H
#define PFB_H

#include "datatypes.h"
#include <complex.h>
#include <fftw3.h>
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Polyphase Filter Bank Context
 * Uses double precision (standard fftw3) to match psd.c logic.
 */
typedef struct {
    int num_channels;       // M
    int taps_per_phase;     // L
    int total_taps;         // M * L
    
    double *window;         // Prototype Lowpass Filter
    double *poly_matrix;    // Polyphase components [M x L] flattened
    
    // Filter State: Stores previous samples for convolution
    double complex *state;   
    
    // FFTW Resources (Double Precision)
    // We use standard complex pointers here which are compatible with fftw_complex
    double complex *fft_in;
    double complex *fft_out;
    fftw_plan plan;
    
} pfb_t;

/**
 * @brief Initialize PFB context
 * @param num_channels Number of sub-bands (M) (e.g., 256)
 * @param taps_per_phase Overlap factor (L) - typically 4
 */
pfb_t* pfb_create(int num_channels, int taps_per_phase);

/**
 * @brief Process Raw HackRF Data and perform Corner Turn
 * * Takes interleaved int8 IQ data (raw HackRF), channelizes it via PFB, 
 * and transposes it into continuous time-series per channel.
 * * @param p PFB Context
 * @param raw_iq Raw int8 buffer from HackRF
 * @param num_total_samples Total number of IQ pairs in raw_iq (bytes / 2)
 * @param channel_outputs Array of pointers to fill. Must be pre-allocated [M][Time]
 * The output is strictly double complex to match signal_iq_t.
 */
void pfb_execute_bulk(pfb_t *p, const int8_t *raw_iq, size_t num_total_samples, double complex **channel_outputs);

/**
 * @brief Cleanup PFB resources
 */
void pfb_destroy(pfb_t *p);

#endif