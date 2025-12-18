//libs/pfb.c
#include "pfb.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

// --- Helper: Kaiser Window Design (Double Precision) ---
// Modified Bessel function of the first kind, order 0
static double bessel_i0(double x) {
    double ax, ans, y;
    if ((ax = fabs(x)) < 3.75) {
        y = x / 3.75; y *= y;
        ans = 1.0 + y * (3.5156229 + y * (3.0899424 + y * (1.2067492 +
              y * (0.2659732 + y * (0.0360768 + y * 0.0045813)))));
    } else {
        y = 3.75 / ax;
        ans = (exp(ax) / sqrt(ax)) * (0.39894228 + y * (0.01328592 +
               y * (0.00225319 + y * (-0.00157565 + y * (0.00916281 +
               y * (-0.02057706 + y * (0.02635537 + y * (-0.01647633 +
               y * 0.00392377))))))));
    }
    return ans;
}

pfb_t* pfb_create(int num_channels, int taps_per_phase) {
    pfb_t *p = calloc(1, sizeof(pfb_t));
    if (!p) return NULL;

    p->num_channels = num_channels;
    p->taps_per_phase = taps_per_phase;
    p->total_taps = num_channels * taps_per_phase;

    // 1. Filter Design (Sinc * Kaiser beta=6.0)
    p->window = malloc(p->total_taps * sizeof(double));
    double beta = 6.0;
    
    for(int i = 0; i < p->total_taps; i++) {
        // Centered Sinc
        double x = (double)(i - p->total_taps/2) / (double)num_channels;
        double sinc = (x == 0) ? 1.0 : sin(M_PI * x) / (M_PI * x);
        
        // Kaiser Window
        double k_arg = 2.0 * beta / p->total_taps * sqrt((double)(i * (p->total_taps - 1 - i)));
        double win = bessel_i0(k_arg) / bessel_i0(beta);
        
        // Apply Gain Correction (x num_channels) to maintain amplitude
        p->window[i] = sinc * win * num_channels; 
    }

    // 2. Polyphase Matrix Decomposition
    // We reverse the taps per phase here to simplify convolution logic later
    p->poly_matrix = malloc(p->total_taps * sizeof(double));
    for(int m = 0; m < num_channels; m++) {
        for(int l = 0; l < taps_per_phase; l++) {
            // Standard PFB mapping: h_m[l] = h[m + l*M]
            p->poly_matrix[m * taps_per_phase + l] = p->window[m + l * num_channels];
        }
    }

    // 3. State Buffer (M rows, L-1 columns)
    p->state = calloc(num_channels * (taps_per_phase - 1), sizeof(double complex));

    // 4. FFTW Init (Double Precision)
    p->fft_in  = fftw_malloc(sizeof(fftw_complex) * num_channels);
    p->fft_out = fftw_malloc(sizeof(fftw_complex) * num_channels);
    
    // Create plan (FFTW_ESTIMATE is usually sufficient and faster to plan)
    p->plan = fftw_plan_dft_1d(num_channels, 
                               (fftw_complex*)p->fft_in, 
                               (fftw_complex*)p->fft_out, 
                               FFTW_FORWARD, FFTW_ESTIMATE);

    return p;
}

// Internal function to process one block of M samples
static void pfb_process_block(pfb_t *p, const double complex *input_block, double complex *output_block) {
    int M = p->num_channels;
    int L = p->taps_per_phase;

    for (int m = 0; m < M; m++) {
        double complex accum = 0;
        
        // 1. Convolution with History (State)
        // state[m] is a row of length L-1
        for (int l = 1; l < L; l++) {
            accum += p->state[m * (L-1) + (l-1)] * p->poly_matrix[m * L + l];
        }
        
        // 2. Convolution with Current Input
        // PFB Commutator Input Logic: input_block is loaded backwards into the filter branches
        // branch m gets input[M - 1 - m]
        accum += input_block[M - 1 - m] * p->poly_matrix[m * L + 0];

        // 3. Update State (Shift & Append)
        // Move [1..L-2] to [0..L-3]
        if (L > 2) {
            memmove(&p->state[m*(L-1)], 
                    &p->state[m*(L-1) + 1], 
                    (L-2) * sizeof(double complex));
        }
        // Insert new sample at end of state
        p->state[m*(L-1) + (L-2)] = input_block[M - 1 - m];

        // 4. Fill FFT Input
        p->fft_in[m] = accum;
    }

    // Execute FFT
    fftw_execute(p->plan);

    // Copy to Output with FFT Shift (DC to center)
    // index mapping: i -> (i + M/2) % M
    for (int i = 0; i < M; i++) {
        int idx = (i + M/2) % M;
        output_block[i] = p->fft_out[idx];
    }
}

void pfb_execute_bulk(pfb_t *p, const int8_t *raw_iq, size_t num_total_samples, double complex **channel_outputs) {
    int M = p->num_channels;
    // Calculate how many full blocks of size M we can process
    size_t num_blocks = num_total_samples / M;
    
    // Temporary buffers for one block
    double complex *input_blk = malloc(M * sizeof(double complex));
    double complex *output_blk = malloc(M * sizeof(double complex));

    for (size_t b = 0; b < num_blocks; b++) {
        
        // 1. Load Raw Data & Normalize
        // Reads M samples (2*M bytes)
        for (int i = 0; i < M; i++) {
            size_t idx = (b * M + i) * 2;
            
            // Convert int8 [-128, 127] to double [-1.0, 1.0]
            double i_val = (double)raw_iq[idx] / 128.0;
            double q_val = (double)raw_iq[idx+1] / 128.0;
            
            input_blk[i] = i_val + I * q_val;
        }

        // 2. Run Polyphase Filter Bank
        pfb_process_block(p, input_blk, output_blk);

        // 3. Corner Turn / Transpose
        // Writes the result into channel-specific continuous arrays
        // channel_outputs is [Channels][Time]
        for (int ch = 0; ch < M; ch++) {
            channel_outputs[ch][b] = output_blk[ch];
        }
    }
    
    free(input_blk);
    free(output_blk);
}

void pfb_destroy(pfb_t *p) {
    if(!p) return;
    
    if (p->plan) fftw_destroy_plan(p->plan);
    if (p->fft_in) fftw_free(p->fft_in);
    if (p->fft_out) fftw_free(p->fft_out);
    
    if (p->window) free(p->window);
    if (p->poly_matrix) free(p->poly_matrix);
    if (p->state) free(p->state);
    
    free(p);
}