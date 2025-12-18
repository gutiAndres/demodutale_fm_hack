//libs/psd.h
#ifndef PSD_H
#define PSD_H

#include "datatypes.h"
#include "sdr_HAL.h"
#include <stdint.h>
#include <cjson/cJSON.h>

signal_iq_t* load_iq_from_buffer(const int8_t* buffer, size_t buffer_size);
void free_signal_iq(signal_iq_t* signal);
void execute_welch_psd(signal_iq_t* signal_data, const PsdConfig_t* config, double* f_out, double* p_out);
double get_window_enbw_factor(PsdWindowType_t type); 
int scale_psd(double* psd, int nperseg, const char* scale_str);
int parse_psd_config(const char *json_string, DesiredCfg_t *target);
void free_desired_psd(DesiredCfg_t *target);
int find_params_psd(DesiredCfg_t desired, SDR_cfg_t *hack_cfg, PsdConfig_t *psd_cfg, RB_cfg_t *rb_cfg);
void print_config_summary(DesiredCfg_t *des, SDR_cfg_t *hw, PsdConfig_t *psd, RB_cfg_t *rb);

#endif