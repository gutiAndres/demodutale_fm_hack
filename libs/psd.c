//libs/psd.c
#include "psd.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fftw3.h>
#include <alloca.h>
#include <complex.h>

// =========================================================
// IQ & Memory
// =========================================================

signal_iq_t* load_iq_from_buffer(const int8_t* buffer, size_t buffer_size) {
    size_t n_samples = buffer_size / 2;
    signal_iq_t* signal_data = (signal_iq_t*)malloc(sizeof(signal_iq_t));
    
    signal_data->n_signal = n_samples;
    signal_data->signal_iq = (double complex*)malloc(n_samples * sizeof(double complex));

    for (size_t i = 0; i < n_samples; i++) {
        signal_data->signal_iq[i] = (double)buffer[2 * i] + (double)buffer[2 * i + 1] * I;
    }

    return signal_data;
}

void free_signal_iq(signal_iq_t* signal) {
    if (signal) {
        if (signal->signal_iq) free(signal->signal_iq);
        free(signal);
    }
}

// =========================================================
// Configuration & Parsing
// =========================================================

static PsdWindowType_t get_window_type_from_string(const char *window_str) {
    if (window_str == NULL) return HAMMING_TYPE;
    
    if (strcasecmp(window_str, "hann") == 0) return HANN_TYPE;
    if (strcasecmp(window_str, "rectangular") == 0) return RECTANGULAR_TYPE;
    if (strcasecmp(window_str, "blackman") == 0) return BLACKMAN_TYPE;
    if (strcasecmp(window_str, "hamming") == 0) return HAMMING_TYPE;
    if (strcasecmp(window_str, "flattop") == 0) return FLAT_TOP_TYPE;
    if (strcasecmp(window_str, "kaiser") == 0) return KAISER_TYPE;
    if (strcasecmp(window_str, "tukey") == 0) return TUKEY_TYPE;
    if (strcasecmp(window_str, "bartlett") == 0) return BARTLETT_TYPE;

    return HAMMING_TYPE;
}

int parse_psd_config(const char *json_string, DesiredCfg_t *target) {
    if (json_string == NULL || target == NULL) return -1;

    memset(target, 0, sizeof(DesiredCfg_t));
    target->window_type = HAMMING_TYPE;
    target->antenna_port = 1;

    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) return -1;

    // 1. RF Mode
    cJSON *rf_mode = cJSON_GetObjectItemCaseSensitive(root, "rf_mode");
    if (cJSON_IsString(rf_mode)) {
        if(strcmp(rf_mode->valuestring, "realtime") == 0) target->rf_mode = REALTIME_MODE;
        else if(strcmp(rf_mode->valuestring, "campaign") == 0) target->rf_mode = CAMPAIGN_MODE;
        else if(strcmp(rf_mode->valuestring, "demodulate") == 0) target->rf_mode = DEMODE_MODE;
        else target->rf_mode = REALTIME_MODE;
    }

    // 2. Numeric params
    cJSON *cf = cJSON_GetObjectItemCaseSensitive(root, "center_freq_hz");
    if (cJSON_IsNumber(cf)) target->center_freq = (uint64_t)cf->valuedouble;

    cJSON *span = cJSON_GetObjectItemCaseSensitive(root, "span");
    if (cJSON_IsNumber(span)) target->span = span->valuedouble;

    cJSON *sr = cJSON_GetObjectItemCaseSensitive(root, "sample_rate_hz");
    if (cJSON_IsNumber(sr)) target->sample_rate = sr->valuedouble;

    cJSON *rbw = cJSON_GetObjectItemCaseSensitive(root, "rbw_hz");
    if (cJSON_IsNumber(rbw)) target->rbw = (int)rbw->valuedouble;

    cJSON *ov = cJSON_GetObjectItemCaseSensitive(root, "overlap");
    if (cJSON_IsNumber(ov)) target->overlap = ov->valuedouble;

    // 3. Window
    cJSON *win = cJSON_GetObjectItemCaseSensitive(root, "window");
    if (cJSON_IsString(win)) target->window_type = get_window_type_from_string(win->valuestring);

    // 4. Scale
    cJSON *sc = cJSON_GetObjectItemCaseSensitive(root, "scale");
    if (cJSON_IsString(sc) && (sc->valuestring != NULL)) target->scale = strdup(sc->valuestring);

    // 5. Gains
    cJSON *lna = cJSON_GetObjectItemCaseSensitive(root, "lna_gain");
    if (cJSON_IsNumber(lna)) target->lna_gain = (int)lna->valuedouble;

    cJSON *vga = cJSON_GetObjectItemCaseSensitive(root, "vga_gain");
    if (cJSON_IsNumber(vga)) target->vga_gain = (int)vga->valuedouble;

    // 6. Antenna
    cJSON *amp = cJSON_GetObjectItemCaseSensitive(root, "antenna_amp");
    if (cJSON_IsBool(amp)) target->amp_enabled = cJSON_IsTrue(amp);

    cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "antenna_port");
    if (cJSON_IsNumber(port)) target->antenna_port = (int)port->valuedouble;

    cJSON_Delete(root);
    return 0;
}

int find_params_psd(DesiredCfg_t desired, SDR_cfg_t *hack_cfg, PsdConfig_t *psd_cfg, RB_cfg_t *rb_cfg) {
    double enbw_factor = get_window_enbw_factor(desired.window_type);
    double required_nperseg_val = enbw_factor * (double)desired.sample_rate / (double)desired.rbw;
    int exponent = (int)ceil(log2(required_nperseg_val));
    
    psd_cfg->nperseg = (int)pow(2, exponent);
    psd_cfg->noverlap = psd_cfg->nperseg * desired.overlap;
    psd_cfg->window_type = desired.window_type;
    psd_cfg->sample_rate = desired.sample_rate;

    hack_cfg->sample_rate = desired.sample_rate;
    hack_cfg->center_freq = desired.center_freq;
    hack_cfg->amp_enabled = desired.amp_enabled;
    hack_cfg->lna_gain = desired.lna_gain;
    hack_cfg->vga_gain = desired.vga_gain;
    hack_cfg->ppm_error = desired.ppm_error;

    // Default to ~1 second of data if not specified
    rb_cfg->total_bytes = (size_t)(desired.sample_rate * 2);
    return 0;
}

void print_config_summary(DesiredCfg_t *des, SDR_cfg_t *hw, PsdConfig_t *psd, RB_cfg_t *rb) {
    double capture_duration = (double)rb->total_bytes / 2.0 / hw->sample_rate;

    printf("\n================ [ CONFIGURATION SUMMARY ] ================\n");
    printf("--- ACQUISITION (Hardware) ---\n");
    printf("Center Freq : %lu Hz\n", hw->center_freq);
    printf("Sample Rate : %.2f MS/s\n", hw->sample_rate / 1e6);
    printf("LNA / VGA   : %d dB / %d dB\n", hw->lna_gain, hw->vga_gain);
    printf("Amp / Port  : %s / %d\n", hw->amp_enabled ? "ON" : "OFF", des->antenna_port);
    printf("Buffer Req  : %zu bytes (~%.4f sec)\n", rb->total_bytes, capture_duration);

    printf("\n--- PSD PROCESS (DSP) ---\n");
    printf("Window      : %d (Enum)\n", psd->window_type);
    printf("FFT Size    : %d bins\n", psd->nperseg);
    printf("Overlap     : %d bins\n", psd->noverlap);
    printf("Scale Unit  : %s\n", des->scale ? des->scale : "dBm (Default)");
    printf("===========================================================\n\n");
}

void free_desired_psd(DesiredCfg_t *target) {
    if (target) {
        if (target->scale) {
            free(target->scale);
            target->scale = NULL;
        }
        // If rf_mode was allocated dynamically, free it here. 
        // In current struct it looks like an enum, but check if struct changed.
    }
}

// =========================================================
// DSP Logic
// =========================================================

int scale_psd(double* psd, int nperseg, const char* scale_str) {
    if (!psd) return -1;
    
    const double Z = 50.0; 
    typedef enum { UNIT_DBM, UNIT_DBUV, UNIT_DBMV, UNIT_WATTS, UNIT_VOLTS } Unit_t;
    Unit_t unit = UNIT_DBM;
    
    if (scale_str) {
        if (strcmp(scale_str, "dBuV") == 0) unit = UNIT_DBUV;
        else if (strcmp(scale_str, "dBmV") == 0) unit = UNIT_DBMV;
        else if (strcmp(scale_str, "W") == 0)    unit = UNIT_WATTS;
        else if (strcmp(scale_str, "V") == 0)    unit = UNIT_VOLTS;
    }

    for (int i = 0; i < nperseg; i++) {
        double p_watts = psd[i] / Z;
        if (p_watts < 1.0e-20) p_watts = 1.0e-20; 

        double val_dbm = 10.0 * log10(p_watts * 1000.0);

        switch (unit) {
            case UNIT_DBUV: psd[i] = val_dbm + 107.0; break;
            case UNIT_DBMV: psd[i] = val_dbm + 47.0; break;
            case UNIT_WATTS: psd[i] = p_watts; break;
            case UNIT_VOLTS: psd[i] = sqrt(p_watts * Z); break;
            case UNIT_DBM:
            default: psd[i] = val_dbm; break;
        }
    }
    return 0;
}

double get_window_enbw_factor(PsdWindowType_t type) {
    switch (type) {
        case RECTANGULAR_TYPE: return 1.000;
        case HAMMING_TYPE:     return 1.363;
        case HANN_TYPE:        return 1.500;
        case BLACKMAN_TYPE:    return 1.730;
        default:               return 1.0;
    }
}

static void generate_window(PsdWindowType_t window_type, double* window_buffer, int window_length) {
    for (int n = 0; n < window_length; n++) {
        switch (window_type) {
            case HANN_TYPE:
                window_buffer[n] = 0.5 * (1 - cos((2.0 * M_PI * n) / (window_length - 1)));
                break;
            case RECTANGULAR_TYPE:
                window_buffer[n] = 1.0;
                break;
            case BLACKMAN_TYPE:
                window_buffer[n] = 0.42 - 0.5 * cos((2.0 * M_PI * n) / (window_length - 1)) + 0.08 * cos((4.0 * M_PI * n) / (window_length - 1));
                break;
            case HAMMING_TYPE:
            default:
                window_buffer[n] = 0.54 - 0.46 * cos((2.0 * M_PI * n) / (window_length - 1));
                break;
        }
    }
}

static void fftshift(double* data, int n) {
    int half = n / 2;
    double* temp = (double*)alloca(half * sizeof(double));
    memcpy(temp, data, half * sizeof(double));
    memcpy(data, &data[half], (n - half) * sizeof(double));
    memcpy(&data[n - half], temp, half * sizeof(double));
}

void execute_welch_psd(signal_iq_t* signal_data, const PsdConfig_t* config, double* f_out, double* p_out) {
    double complex* signal = signal_data->signal_iq;
    size_t n_signal = signal_data->n_signal;
    int nperseg = config->nperseg;
    int noverlap = config->noverlap;
    double fs = config->sample_rate;
    
    int nfft = nperseg;
    int step = nperseg - noverlap;
    int k_segments = (n_signal - noverlap) / step;

    double* window = (double*)malloc(nperseg * sizeof(double));
    generate_window(config->window_type, window, nperseg);

    double u_norm = 0.0;
    for (int i = 0; i < nperseg; i++) u_norm += window[i] * window[i];
    u_norm /= nperseg;

    double complex* fft_in = fftw_alloc_complex(nfft);
    double complex* fft_out = fftw_alloc_complex(nfft);
    fftw_plan plan = fftw_plan_dft_1d(nfft, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);

    memset(p_out, 0, nfft * sizeof(double));

    for (int k = 0; k < k_segments; k++) {
        int start = k * step;
        
        for (int i = 0; i < nperseg; i++) {
            fft_in[i] = signal[start + i] * window[i];
        }

        fftw_execute(plan);

        for (int i = 0; i < nfft; i++) {
            double mag = cabs(fft_out[i]);
            p_out[i] += (mag * mag);
        }
    }

    double scale = 1.0 / (fs * u_norm * k_segments * nperseg);
    for (int i = 0; i < nfft; i++) p_out[i] *= scale;

    fftshift(p_out, nfft);

    double df = fs / nfft;
    for (int i = 0; i < nfft; i++) {
        f_out[i] = -fs / 2.0 + i * df;
    }

    free(window);
    fftw_destroy_plan(plan);
    fftw_free(fft_in);
    fftw_free(fft_out);
}