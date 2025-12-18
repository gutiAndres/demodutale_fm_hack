// libs/pipeline_threads.c
#define _GNU_SOURCE
#include "pipeline_threads.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

/* Use CIC library */
#include "cic_decim.h"


/* ---------- Metrics helpers: FM deviation & AM depth ---------- */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Ajusta a tu gusto */
#define DEV_EMA_ALPHA        0.05f
#define DEPTH_EMA_ALPHA      0.05f

/* Reporte cada ~0.5 s (puedes cambiarlo) */
#define DEV_REPORT_SEC       0.5f
#define DEPTH_REPORT_SEC     0.5f


typedef struct {
    float dev_max_hz;
    float dev_ema_hz;
    int   counter;
    int   report_samples; /* en muestras IQ (Fs_demod) */
} fm_dev_state_t;

typedef struct {
    float env_min;
    float env_max;
    float depth_ema;
    int   counter;
    int   report_samples; /* en muestras de audio (Fs_audio) o en “env_dec” */
} am_depth_state_t;

static inline float phase_diff_to_hz_local(float phase_diff_rad, int fs_hz)
{
    /* fi = |dphi| * Fs / (2*pi) */
    return fabsf(phase_diff_rad) * ((float)fs_hz) / (2.0f * (float)M_PI);
}

static inline void fm_dev_init(fm_dev_state_t *st, int fs_demod)
{
    memset(st, 0, sizeof(*st));
    st->dev_max_hz = 0.0f;
    st->dev_ema_hz = 0.0f;
    st->counter    = 0;
    st->report_samples = (int)lrintf((float)fs_demod * DEV_REPORT_SEC);
    if (st->report_samples < 1) st->report_samples = 1;
}

static inline void am_depth_init(am_depth_state_t *st, int fs_audio)
{
    memset(st, 0, sizeof(*st));
    st->env_min = 1e9f;
    st->env_max = 0.0f;
    st->depth_ema = 0.0f;
    st->counter = 0;
    st->report_samples = (int)lrintf((float)fs_audio * DEPTH_REPORT_SEC);
    if (st->report_samples < 1) st->report_samples = 1;
}

static inline float update_fm_deviation_ctx(fm_dev_state_t *st,
                                           float phase_diff_rad,
                                           int fs_demod)
{
    float fi_hz = phase_diff_to_hz_local(phase_diff_rad, fs_demod);

    if (fi_hz > st->dev_max_hz) st->dev_max_hz = fi_hz;

    st->dev_ema_hz = (1.0f - DEV_EMA_ALPHA) * st->dev_ema_hz
                   + (DEV_EMA_ALPHA) * fi_hz;

    st->counter++;
    return st->dev_ema_hz; /* Hz */
}


static inline float update_am_depth_from_env_ctx(am_depth_state_t *st,
                                                 float env_decimated)
{
    if (!isfinite(env_decimated)) return st->depth_ema;

    if (env_decimated < st->env_min) st->env_min = env_decimated;
    if (env_decimated > st->env_max) st->env_max = env_decimated;

    st->counter++;
    if (st->counter >= st->report_samples) {
        float denom = (st->env_max + st->env_min);
        float m = 0.0f;

        if (denom > 1e-9f) {
            m = (st->env_max - st->env_min) / denom;
            if (m < 0.0f) m = 0.0f;
            if (m > 2.0f) m = 2.0f;
        }

        st->depth_ema = (1.0f - DEPTH_EMA_ALPHA) * st->depth_ema
                      + (DEPTH_EMA_ALPHA) * m;

        /* reset ventana */
        st->env_min = 1e9f;
        st->env_max = 0.0f;
        st->counter = 0;
    }

    return st->depth_ema; /* m (0..2) */
}



/* ---------- helpers (local) ---------- */
static const char* mode_str(demod_mode_t m) {
    switch (m) { case DEMOD_AM: return "AM"; case DEMOD_FM: return "FM"; default: return "UNKNOWN"; }
}

/* CSV helper: freq_rel + center_freq => freq_abs */
static int save_results_csv(const char *csv_path,
                            double* freq_array_rel,
                            double* psd_array,
                            int length,
                            const SDR_cfg_t *local_hack,
                            const char *scale_label)
{
    if (!csv_path || !freq_array_rel || !psd_array || length <= 0 || !local_hack) return -1;

    FILE *fp = fopen(csv_path, "w");
    if (!fp) { perror("[CSV] fopen"); return -1; }

    fprintf(fp, "freq_hz,psd_%s\n", (scale_label && scale_label[0]) ? scale_label : "lin");

    for (int i = 0; i < length; i++) {
        double f_abs = freq_array_rel[i] + (double)local_hack->center_freq;
        fprintf(fp, "%.6f,%.12e\n", f_abs, psd_array[i]);
    }

    fclose(fp);
    return 0;
}

/* ---------- thread fns ---------- */

static void* decim_thread_fn(void* arg) {
    pipeline_ctx_t *ctx = (pipeline_ctx_t*)arg;

    fprintf(stderr, "[DECIM] Start | Fs_in=%d -> Fs_demod=%d | R=%d\n",
            ctx->sample_rate_rf_in, ctx->sample_rate_demod, ctx->decim_factor);

    cic_decim_t cic;
    cic_init(&cic, ctx->decim_factor, 3);

    enum { IN_CHUNK = 32768 }; /* bytes (even) */
    uint8_t in_bytes[IN_CHUNK];
    uint8_t out_bytes[IN_CHUNK];

    while (!atomic_load(ctx->stop)) {
        size_t got = rb_sig_read_blocking(ctx->iq_raw_rb, in_bytes, 2, ctx->stop);
        if (got == 0) break;

        got += rb_sig_read(ctx->iq_raw_rb, in_bytes + got, IN_CHUNK - got);
        got = (got / 2) * 2;

        int8_t *b = (int8_t*)in_bytes;
        int n_iq = (int)(got / 2);

        int out_idx = 0;

        for (int k = 0; k < n_iq && !atomic_load(ctx->stop); k++) {
            int32_t xi = (int32_t)b[2*k];
            int32_t xq = (int32_t)b[2*k + 1];

            int32_t yo_i, yo_q;
            bool produced;
            cic_process_one(&cic, xi, xq, &yo_i, &yo_q, &produced);

            if (produced) {
                if (out_idx + 2 <= (int)sizeof(out_bytes)) {
                    out_bytes[out_idx++] = (uint8_t)((int8_t)yo_i);
                    out_bytes[out_idx++] = (uint8_t)((int8_t)yo_q);
                }
            }
        }

        if (out_idx > 0) {
            size_t w = rb_sig_write(ctx->iq_demod_rb, out_bytes, (size_t)out_idx);
            if (w < (size_t)out_idx) {
                atomic_fetch_add(ctx->iq_demod_drops,
                                 (unsigned long)((size_t)out_idx - w));
            }
        }
    }

    fprintf(stderr, "[DECIM] Exit\n");
    return NULL;
}
static void* demod_thread_fn(void* arg) {
    pipeline_ctx_t *ctx = (pipeline_ctx_t*)arg;

    fprintf(stderr, "[DEMOD] Start | mode=%s | Fs_demod=%d | DecimAudio=%d -> %d Hz\n",
            mode_str(ctx->mode),
            ctx->sample_rate_demod,
            ctx->decimation_audio,
            ctx->sample_rate_audio);

    enum { IQ_CHUNK = 16384 };
    uint8_t iq_bytes[IQ_CHUNK];

    fm_demod_t fm;
    am_demod_t am;

    /* ----- Metrics state ----- */
    fm_dev_state_t   fmst;
    am_depth_state_t amst;

    /* Para AM: acumulador de envolvente a tasa Fs_demod -> env_dec a tasa Fs_audio */
    float am_env_sum = 0.0f;
    int   am_env_dec_counter = 0;

    /* Drops para reportar */
    const atomic_ulong *iq_drop_for_metrics  = ctx->iq_demod_drops; /* o ctx->iq_raw_drops */
    const atomic_ulong *pcm_drop_for_metrics = ctx->pcm_drops;

    if (ctx->mode == DEMOD_FM) {
        fm_demod_init(&fm, ctx->sample_rate_demod, ctx->decimation_audio, ctx->fm_audio_bw_or_deemph);
        fm_dev_init(&fmst, ctx->sample_rate_demod);
    } else {
        am_demod_init(&am, ctx->sample_rate_demod, ctx->decimation_audio, ctx->am_audio_bw);
        am_depth_init(&amst, ctx->sample_rate_audio);
    }

    while (!atomic_load(ctx->stop)) {
        size_t got = rb_sig_read_blocking(ctx->iq_demod_rb, iq_bytes, 2, ctx->stop);
        if (got == 0) break;

        got += rb_sig_read(ctx->iq_demod_rb, iq_bytes + got, IQ_CHUNK - got);
        got = (got / 2) * 2;

        int8_t *buf = (int8_t*)iq_bytes;
        int count = (int)(got / 2);

        for (int j = 0; j < count && !atomic_load(ctx->stop); j++) {
            float i = (float)buf[2*j]     / 128.0f;
            float q = (float)buf[2*j + 1] / 128.0f;

            if (ctx->mode == DEMOD_FM) {
                float dphi = fm_demod_phase_diff(&fm, i, q);

                /* ---- Metrics FM: EMA ---- */
                float ema_hz = update_fm_deviation_ctx(&fmst, dphi, ctx->sample_rate_demod);

                /* Print cuando se cumple el periodo de reporte */
                if (fmst.counter >= fmst.report_samples) {
                    fprintf(stderr,
                            "[FM] Excursion pico: %.1f kHz | EMA: %.1f kHz | IQ drops: %lu bytes\n",
                            fmst.dev_max_hz / 1e3f,
                            ema_hz / 1e3f,
                            iq_drop_for_metrics ? (unsigned long)atomic_load(iq_drop_for_metrics) : 0UL);

                    fmst.dev_max_hz = 0.0f;
                    fmst.counter = 0;
                }

                /* Tu audio por decimación (igual que antes) */
                fm.sum_audio += dphi;
                fm.dec_counter++;
                if (fm.dec_counter == fm.decimation) {
                    float audio = fm.sum_audio / (float)fm.decimation;

                    int16_t s16 = (int16_t)lrintf(
                        fmaxf(fminf(audio * fm.audio_gain, 32767.0f), -32768.0f)
                    );

                    size_t wpcm = rb_sig_write(ctx->pcm_rb, &s16, sizeof(s16));
                    if (wpcm < sizeof(s16))
                        atomic_fetch_add(ctx->pcm_drops,
                                         (unsigned long)(sizeof(s16) - wpcm));

                    fm.sum_audio = 0.0f;
                    fm.dec_counter = 0;
                }

            } else {
                /* ---- AM: envolvente ---- */
                float env = sqrtf(i*i + q*q);

                am_env_sum += env;
                am_env_dec_counter++;

                if (am_env_dec_counter >= ctx->decimation_audio) {
                    float env_dec = am_env_sum / (float)ctx->decimation_audio;

                    int prev_counter = amst.counter;   /* para detectar reset */
                    float ema_m = update_am_depth_from_env_ctx(&amst, env_dec);

                    /* Si se reseteó ventana, imprimimos */
                    if (prev_counter != 0 && amst.counter == 0) {
                        float m_inst = 0.0f;
                        /* Nota: el m “instantáneo” se calcula dentro del update justo antes del EMA,
                           aquí imprimimos EMA (lo que quieres), y adicionalmente imprimimos Amin/Amax previos
                           ya no están disponibles porque se resetearon. Si los quieres en el print,
                           hay que guardar copia antes del reset. */

                        (void)m_inst;

                        fprintf(stderr,
                                "[AM] Profundidad EMA: %.1f %% | IQ drops: %lu | PCM drops: %lu\n",
                                100.0f * ema_m,
                                iq_drop_for_metrics ? (unsigned long)atomic_load(iq_drop_for_metrics) : 0UL,
                                pcm_drop_for_metrics ? (unsigned long)atomic_load(pcm_drop_for_metrics) : 0UL);
                    }

                    am_env_sum = 0.0f;
                    am_env_dec_counter = 0;
                }

                /* Tu demod AM + PCM (igual que antes) */
                int16_t pcm;
                am_depth_report_t rep;
                if (am_demod_process_iq(&am, i, q, &pcm, &rep)) {
                    size_t wpcm = rb_sig_write(ctx->pcm_rb, &pcm, sizeof(pcm));
                    if (wpcm < sizeof(pcm))
                        atomic_fetch_add(ctx->pcm_drops,
                                         (unsigned long)(sizeof(pcm) - wpcm));
                }
            }
        }
    }

    fprintf(stderr, "[DEMOD] Exit\n");
    return NULL;
}


static void* net_thread_fn(void* arg) {
    pipeline_ctx_t *ctx = (pipeline_ctx_t*)arg;
    fprintf(stderr, "[NET] Start\n");

    int16_t *frame = (int16_t*)malloc((size_t)ctx->frame_samples * sizeof(int16_t));
    if (!frame) {
        fprintf(stderr, "[NET] malloc frame failed\n");
        atomic_store(ctx->stop, 1);
        return NULL;
    }

    while (!atomic_load(ctx->stop)) {
        size_t need = (size_t)ctx->frame_samples * sizeof(int16_t);
        size_t got = rb_sig_read_blocking(ctx->pcm_rb, frame, need, ctx->stop);
        if (got == 0) break;

        if (opus_tx_send_frame(ctx->tx, frame, ctx->frame_samples) != 0) {
            fprintf(stderr, "[NET] opus_tx_send_frame error -> stop\n");
            atomic_store(ctx->stop, 1);
            break;
        }
    }

    free(frame);
    fprintf(stderr, "[NET] Exit\n");
    return NULL;
}

static void* psd_thread_fn(void* arg) {
    pipeline_ctx_t *ctx = (pipeline_ctx_t*)arg;

    fprintf(stderr, "[PSD] Start | total_bytes=%zu nperseg=%d scale=%s\n",
            (size_t)ctx->rb_cfg->total_bytes,
            ctx->psd_cfg->nperseg,
            (ctx->desired_cfg->scale ? ctx->desired_cfg->scale : "lin"));

    if ((size_t)ctx->rb_cfg->total_bytes > ctx->psd_rb->size) {
        fprintf(stderr, "[PSD] ERROR: total_bytes=%zu > PSD_RB_BYTES=%zu\n",
                (size_t)ctx->rb_cfg->total_bytes, ctx->psd_rb->size);
        atomic_store(ctx->stop, 1);
        return NULL;
    }

    while (!atomic_load(ctx->stop)) {
        rb_reset(ctx->psd_rb);
        *(ctx->psd_capture_active) = true;

        int safety = ctx->psd_wait_timeout_iters;
        while (!atomic_load(ctx->stop) && safety-- > 0) {
            if (rb_available(ctx->psd_rb) >= (size_t)ctx->rb_cfg->total_bytes) break;
            usleep((useconds_t)ctx->psd_wait_sleep_us);
        }

        *(ctx->psd_capture_active) = false;

        if (atomic_load(ctx->stop)) break;
        if (safety <= 0) {
            fprintf(stderr, "[PSD] Timeout waiting bytes (drops=%lu). Will retry.\n",
                    (unsigned long)atomic_load(ctx->psd_drops));
            usleep((useconds_t)ctx->psd_post_sleep_us);
            continue;
        }

        int8_t *linear_buffer = (int8_t*)malloc((size_t)ctx->rb_cfg->total_bytes);
        if (!linear_buffer) {
            fprintf(stderr, "[PSD] malloc linear_buffer failed\n");
            usleep((useconds_t)ctx->psd_post_sleep_us);
            continue;
        }

        rb_read(ctx->psd_rb, linear_buffer, (size_t)ctx->rb_cfg->total_bytes);

        signal_iq_t *sig = load_iq_from_buffer(linear_buffer, (size_t)ctx->rb_cfg->total_bytes);
        free(linear_buffer);

        if (!sig) {
            fprintf(stderr, "[PSD] load_iq_from_buffer failed\n");
            usleep((useconds_t)ctx->psd_post_sleep_us);
            continue;
        }

        double *freq = (double*)malloc((size_t)ctx->psd_cfg->nperseg * sizeof(double));
        double *psd  = (double*)malloc((size_t)ctx->psd_cfg->nperseg * sizeof(double));
        if (!freq || !psd) {
            fprintf(stderr, "[PSD] malloc freq/psd failed\n");
            free(freq); free(psd);
            free_signal_iq(sig);
            usleep((useconds_t)ctx->psd_post_sleep_us);
            continue;
        }

        execute_welch_psd(sig, ctx->psd_cfg, freq, psd);
        scale_psd(psd, ctx->psd_cfg->nperseg, ctx->desired_cfg->scale);

        double half_span = ctx->desired_cfg->span / 2.0;
        int start_idx = 0;
        int end_idx = ctx->psd_cfg->nperseg - 1;

        for (int i = 0; i < ctx->psd_cfg->nperseg; i++) {
            if (freq[i] >= -half_span) { start_idx = i; break; }
        }
        for (int i = start_idx; i < ctx->psd_cfg->nperseg; i++) {
            if (freq[i] > half_span) { end_idx = i - 1; break; }
            end_idx = i;
        }

        int valid_len = end_idx - start_idx + 1;
        if (valid_len > 0) {
            if (save_results_csv(ctx->psd_csv_path,
                                 &freq[start_idx],
                                 &psd[start_idx],
                                 valid_len,
                                 ctx->hack_cfg,
                                 ctx->desired_cfg->scale) == 0) {
                fprintf(stderr, "[PSD] Saved CSV: %s | bins=%d | drops=%lu\n",
                        ctx->psd_csv_path, valid_len,
                        (unsigned long)atomic_load(ctx->psd_drops));
            }
        } else {
            fprintf(stderr, "[PSD] Warning: span crop -> 0 bins\n");
        }

        free(freq);
        free(psd);
        free_signal_iq(sig);

        usleep((useconds_t)ctx->psd_post_sleep_us);
    }

    fprintf(stderr, "[PSD] Exit\n");
    return NULL;
}

/* ---------- public API ---------- */

int pipeline_threads_start(pipeline_threads_t *t, pipeline_ctx_t *ctx) {
    memset(t, 0, sizeof(*t));

    if (pthread_create(&t->th_decim, NULL, decim_thread_fn, ctx) != 0) {
        fprintf(stderr, "[PIPE] pthread_create decim failed\n");
        return -1;
    }
    t->started_decim = true;

    if (pthread_create(&t->th_demod, NULL, demod_thread_fn, ctx) != 0) {
        fprintf(stderr, "[PIPE] pthread_create demod failed\n");
        atomic_store(ctx->stop, 1);
        rb_sig_wake_all(ctx->iq_raw_rb);
        pthread_join(t->th_decim, NULL);
        t->started_decim = false;
        return -1;
    }
    t->started_demod = true;

    if (pthread_create(&t->th_net, NULL, net_thread_fn, ctx) != 0) {
        fprintf(stderr, "[PIPE] pthread_create net failed\n");
        atomic_store(ctx->stop, 1);
        rb_sig_wake_all(ctx->iq_raw_rb);
        rb_sig_wake_all(ctx->iq_demod_rb);
        pthread_join(t->th_decim, NULL);
        pthread_join(t->th_demod, NULL);
        t->started_decim = t->started_demod = false;
        return -1;
    }
    t->started_net = true;

    if (pthread_create(&t->th_psd, NULL, psd_thread_fn, ctx) != 0) {
        fprintf(stderr, "[PIPE] pthread_create psd failed\n");
        atomic_store(ctx->stop, 1);
        rb_sig_wake_all(ctx->iq_raw_rb);
        rb_sig_wake_all(ctx->iq_demod_rb);
        rb_sig_wake_all(ctx->pcm_rb);
        pthread_join(t->th_decim, NULL);
        pthread_join(t->th_demod, NULL);
        pthread_join(t->th_net, NULL);
        t->started_decim = t->started_demod = t->started_net = false;
        return -1;
    }
    t->started_psd = true;

    return 0;
}

void pipeline_threads_stop(pipeline_ctx_t *ctx) {
    atomic_store(ctx->stop, 1);
    rb_sig_wake_all(ctx->iq_raw_rb);
    rb_sig_wake_all(ctx->iq_demod_rb);
    rb_sig_wake_all(ctx->pcm_rb);
    /* psd_rb usa rb_available polling; stop flag basta */
}

void pipeline_threads_join(pipeline_threads_t *t) {
    if (t->started_decim) pthread_join(t->th_decim, NULL);
    if (t->started_demod) pthread_join(t->th_demod, NULL);
    if (t->started_net)   pthread_join(t->th_net, NULL);
    if (t->started_psd)   pthread_join(t->th_psd, NULL);
}
