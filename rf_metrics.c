#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <inttypes.h>

#include <libhackrf/hackrf.h>

#include "psd.h"
#include "datatypes.h"
#include "sdr_HAL.h"
#include "ring_buffer.h"

// =========================================================
// GLOBAL VARIABLES (MISMAS)
// =========================================================
hackrf_device* device = NULL;
ring_buffer_t rb;

volatile bool stop_streaming = true;
volatile bool config_received = false;

DesiredCfg_t desired_config = {0};
PsdConfig_t psd_cfg = {0};
SDR_cfg_t hack_cfg = {0};
RB_cfg_t rb_cfg = {0};

// =========================================================
// CALLBACKS (MISMAS)
// =========================================================
int rx_callback(hackrf_transfer* transfer) {
    if (stop_streaming) return 0;
    rb_write(&rb, transfer->buffer, transfer->valid_length);
    return 0;
}

int recover_hackrf(void) {
    printf("\n[RECOVERY] Initiating Hardware Reset sequence...\n");
    if (device != NULL) {
        stop_streaming = true;
        hackrf_stop_rx(device);
        usleep(100000);
        hackrf_close(device);
        device = NULL;
    }

    int attempts = 0;
    while (attempts < 3) {
        usleep(500000);
        int status = hackrf_open(&device);
        if (status == HACKRF_SUCCESS) {
            printf("[RECOVERY] Device Re-opened successfully.\n");
            return 0;
        }
        attempts++;
    }
    return -1;
}

// =========================================================
// REEMPLAZO DIRECTO DE publish_results(): ahora CSV
// (misma idea: freq_rel + center_freq => freq_abs)
// =========================================================
static int save_results_csv(const char *csv_path,
                            double* freq_array_rel,
                            double* psd_array,
                            int length,
                            SDR_cfg_t *local_hack,
                            const char *scale_label)
{
    if (!csv_path || !freq_array_rel || !psd_array || length <= 0 || !local_hack) return -1;

    FILE *fp = fopen(csv_path, "w");
    if (!fp) {
        perror("[CSV] fopen");
        return -1;
    }

    // Header simple
    fprintf(fp, "freq_hz,psd_%s\n", (scale_label && scale_label[0]) ? scale_label : "lin");

    for (int i = 0; i < length; i++) {
        double f_abs = freq_array_rel[i] + (double)local_hack->center_freq;
        fprintf(fp, "%.6f,%.12e\n", f_abs, psd_array[i]);
    }

    fclose(fp);
    printf("[CSV] Saved results (%d bins) -> %s\n", length, csv_path);
    return 0;
}

// =========================================================
// MAIN (MISMA LOGICA; solo sin ZMQ)
// =========================================================
int main() {
    // -------------------------
    // 1) DEFINIR PARAMETROS FIJOS EN EL CODIGO
    //    (antes llegaban por ZMQ; ahora quedan hardcodeados)
    // -------------------------
    memset(&desired_config, 0, sizeof(desired_config));
    memset(&hack_cfg, 0, sizeof(hack_cfg));
    memset(&psd_cfg, 0, sizeof(psd_cfg));
    memset(&rb_cfg, 0, sizeof(rb_cfg));

    // ---- EJEMPLO: AJUSTA a tu struct real / hackrf_apply_cfg ----
    desired_config.rbw         = 10000;     // IMPORTANTÍSIMO: no puede ser 0
    desired_config.center_freq = 105700000.0;  // si existe en DesiredCfg_t
    desired_config.sample_rate = 20000000.0;    // si existe
    desired_config.span = 20000000.0;              // 10 MHz
    // Si tu DesiredCfg_t.scale es char* o char[] depende de tu datatypes.h
    // Caso A: si es char[]:
    // snprintf(desired_config.scale, sizeof(desired_config.scale), "%s", "dBm");
    // Caso B: si es char*:
    desired_config.scale = "dBm";

    desired_config.lna_gain = 0;
    desired_config.vga_gain = 0;
    desired_config.antenna_port = 1;
    desired_config.amp_enabled = 1;

    // Reutiliza tu función existente: genera hack_cfg, psd_cfg, rb_cfg
    // EXACTAMENTE como lo hacías tras parsear JSON
    find_params_psd(desired_config, &hack_cfg, &psd_cfg, &rb_cfg);
    print_config_summary(&desired_config, &hack_cfg, &psd_cfg, &rb_cfg);


    config_received = true;  // <- “simula” que ya llegó la config

    // -------------------------
    // 2) INIT HW + RB (IGUAL)
    // -------------------------
    if (hackrf_init() != HACKRF_SUCCESS) return 1;
    if (hackrf_open(&device) != HACKRF_SUCCESS) {
        fprintf(stderr, "[SYSTEM] Warning: Initial Open failed. Will retry in loop.\n");
    }

    size_t FIXED_BUFFER_SIZE = 100 * 1024 * 1024;
    rb_init(&rb, FIXED_BUFFER_SIZE);
    printf("[SYSTEM] Persistent Ring Buffer Initialized (%zu MB)\n", FIXED_BUFFER_SIZE / (1024*1024));

    bool needs_recovery = false;

    SDR_cfg_t local_hack_cfg;
    RB_cfg_t local_rb_cfg;
    PsdConfig_t local_psd_cfg;
    DesiredCfg_t local_desired_cfg;

    // Ruta CSV fija (si quieres, construye con timestamp/center_freq)
    const char *csv_out = "static/last_psd.csv";

    // -------------------------
    // 3) LOOP PRINCIPAL (IGUAL)
    // -------------------------
    while (1) {
        if (!config_received) {
            usleep(10000);
            continue;
        }

        if (device == NULL) {
            needs_recovery = true;
            goto error_handler;
        }

        // SNAPSHOT CONFIG (IGUAL)
        memcpy(&local_hack_cfg, &hack_cfg, sizeof(SDR_cfg_t));
        memcpy(&local_rb_cfg, &rb_cfg, sizeof(RB_cfg_t));
        memcpy(&local_psd_cfg, &psd_cfg, sizeof(PsdConfig_t));
        memcpy(&local_desired_cfg, &desired_config, sizeof(DesiredCfg_t));
        config_received = false;

        if (local_rb_cfg.total_bytes > rb.size) {
            printf("[SYSTEM] Error: Request exceeds buffer size!\n");
            continue;
        }

        // ACQUIRE (IGUAL)
        rb_reset(&rb);
        stop_streaming = false;
        hackrf_apply_cfg(device, &local_hack_cfg);

        if (hackrf_start_rx(device, rx_callback, NULL) != HACKRF_SUCCESS) {
            needs_recovery = true;
            goto error_handler;
        }

        int safety_timeout = 500;
        while (safety_timeout > 0) {
            if (rb_available(&rb) >= local_rb_cfg.total_bytes) break;
            usleep(10000);
            safety_timeout--;
        }

        stop_streaming = true;
        hackrf_stop_rx(device);
        usleep(50000);

        if (safety_timeout <= 0) {
            needs_recovery = true;
            goto error_handler;
        }

        // PROCESS (IGUAL)
        int8_t* linear_buffer = malloc(local_rb_cfg.total_bytes);
        if (linear_buffer) {
            rb_read(&rb, linear_buffer, local_rb_cfg.total_bytes);

            signal_iq_t* sig = load_iq_from_buffer(linear_buffer, local_rb_cfg.total_bytes);

            double* freq = malloc(local_psd_cfg.nperseg * sizeof(double));
            double* psd  = malloc(local_psd_cfg.nperseg * sizeof(double));

            if (freq && psd && sig) {
                // 1) PSD full-band (IGUAL)
                execute_welch_psd(sig, &local_psd_cfg, freq, psd);
                scale_psd(psd, local_psd_cfg.nperseg, local_desired_cfg.scale);

                // 2) SPAN logic (IGUAL)
                double half_span = local_desired_cfg.span / 2.0;
                int start_idx = 0;
                int end_idx = local_psd_cfg.nperseg - 1;

                for (int i = 0; i < local_psd_cfg.nperseg; i++) {
                    if (freq[i] >= -half_span) {
                        start_idx = i;
                        break;
                    }
                }
                for (int i = start_idx; i < local_psd_cfg.nperseg; i++) {
                    if (freq[i] > half_span) {
                        end_idx = i - 1;
                        break;
                    }
                    end_idx = i;
                }

                int valid_len = end_idx - start_idx + 1;

                // 3) ANTES: publish_results(...)
                //    AHORA: guardar CSV con los bins “cropeados”
                if (valid_len > 0) {
                    save_results_csv(csv_out,
                                     &freq[start_idx],
                                     &psd[start_idx],
                                     valid_len,
                                     &local_hack_cfg,
                                     local_desired_cfg.scale);
                } else {
                    printf("[DSP] Warning: Span resulted in 0 bins.\n");
                }
            }

            free(linear_buffer);
            if (freq) free(freq);
            if (psd) free(psd);
            free_signal_iq(sig);
        }

        // Si quieres “una sola adquisición y salir”, descomenta:
        // break;

        continue;

error_handler:
        stop_streaming = true;
        if (needs_recovery) {
            recover_hackrf();
            needs_recovery = false;
            // opcional: reintentar misma config
            config_received = true;
        }
    }

    rb_free(&rb);
    return 0;
}
