// fm_receiver.c
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <libhackrf/hackrf.h>
#include <portaudio.h>
#include "ringbuffer.h"

// --- CONFIGURACIÓN ---
#define FREQ_HZ 105700000     // <--- PON TU FRECUENCIA AQUÍ (Ej: 96.9 MHz)
#define SAMPLE_RATE_RF 1920000 // 1.92 MS/s
#define SAMPLE_RATE_AUDIO 48000
#define DECIMATION 40        // 1920000 / 48000 = 40 (Entero exacto)

// PI para matemáticas
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Estado Global
RingBuffer rb;
float last_phase = 0.0f;     // Para el demodulador FM (memoria de fase anterior)

// --- FUNCIÓN DE DEMODULACIÓN (El corazón DSP) ---
/* 
   Recibe I, Q actuales.
   Calcula el ángulo (fase).
   La señal de audio FM es la DIFERENCIA entre la fase actual y la anterior.
*/
float demodulate_fm(float i, float q) {
    // 1. Calcular fase actual (-PI a PI)
    float current_phase = atan2f(q, i);
    
    // 2. Calcular diferencia (derivada de la fase = frecuencia)
    float phase_diff = current_phase - last_phase;
    
    // 3. Corregir el salto de fase (Wrap around)
    // Si pasamos de +3.14 a -3.14, no es un salto gigante, es continuidad circular.
    if (phase_diff > M_PI)  phase_diff -= 2.0f * M_PI;
    if (phase_diff < -M_PI) phase_diff += 2.0f * M_PI;
    
    last_phase = current_phase;
    
    // 4. Salida normalizada (aprox)
    return phase_diff;
}

// --- CALLBACK HACKRF (Productor) ---
int rx_callback(hackrf_transfer* transfer) {
    // El buffer viene como pares de bytes signed: I, Q, I, Q...
    int8_t* buf = (int8_t*)transfer->buffer;
    int count = transfer->valid_length / 2; // Número de pares IQ
    
    // Variables para el diezmado (filtro promedio simple)
    float sum_audio = 0.0f;
    int dec_counter = 0;

    for (int j = 0; j < count; j++) {
        // 1. Convertir bytes a float (-1.0 a 1.0)
        float i = (float)buf[2*j] / 128.0f;
        float q = (float)buf[2*j+1] / 128.0f;

        // 2. Demodular FM
        float fm_sample = demodulate_fm(i, q);
        
        // 3. Diezmado (Decimation)
        // Acumulamos 40 muestras
        sum_audio += fm_sample;
        dec_counter++;

        if (dec_counter == DECIMATION) {
            // Promediamos
            float audio_out = sum_audio / (float)DECIMATION;
            
            // Filtro de De-emphasis simple (opcional, reduce ruido agudo)
            // audio_out = audio_out * 0.5; // Gain reduction
            
            // 4. Escribir al RingBuffer
            rb_write(&rb, audio_out);
            
            // Reset acumuladores
            sum_audio = 0.0f;
            dec_counter = 0;
        }
    }
    return 0;
}

// --- CALLBACK PORTAUDIO (Consumidor) ---
static int pa_callback(const void *inputBuffer, void *outputBuffer,
                       unsigned long framesPerBuffer,
                       const PaStreamCallbackTimeInfo* timeInfo,
                       PaStreamCallbackFlags statusFlags,
                       void *userData) {
    float *out = (float*)outputBuffer;
    (void) inputBuffer;

    for(unsigned int i=0; i<framesPerBuffer; i++) {
        // Leemos del RingBuffer. Si está vacío, escribimos 0.
        if (rb_available(&rb) > 0) {
            // Multiplicamos por 2.0 o más para Ganancia de Volumen Digital
            *out++ = rb_read(&rb) * 3.0f; 
        } else {
            *out++ = 0.0f;
        }
    }
    return paContinue;
}

int main() {
    // 1. Inicializar Ring Buffer
    rb_init(&rb);

    // 2. Inicializar HackRF
    hackrf_init();
    hackrf_device* device = NULL;
    int status = hackrf_open(&device);
    if (status != HACKRF_SUCCESS) {
        printf("Error: No se encontró HackRF (status %d)\n", status);
        return 1;
    }

    // --- CONFIGURACIÓN DE RADIO (CRÍTICO) ---
    hackrf_set_sample_rate(device, SAMPLE_RATE_RF);
    hackrf_set_freq(device, FREQ_HZ);
    
    // GANANCIAS: Si están en 0 no escucharás nada.
    // LNA (0-40), VGA (0-62)
    hackrf_set_amp_enable(device, 0); // Amplificador externo (generalmente apagado)
    hackrf_set_lna_gain(device, 32);  // Ganancia de entrada RF
    hackrf_set_vga_gain(device, 28);  // Ganancia banda base

    // 3. Inicializar Audio
    Pa_Initialize();
    PaStream *stream;
    Pa_OpenDefaultStream(&stream,
                         0,          // Canales entrada
                         1,          // Canales salida (Mono)
                         paFloat32,  // Formato
                         SAMPLE_RATE_AUDIO,
                         512,        // Frames por buffer (latencia)
                         pa_callback,
                         NULL);
    
    Pa_StartStream(stream);
    hackrf_start_rx(device, rx_callback, NULL);

    printf("=== RADIO FM EN VIVO (C + RingBuffer) ===\n");
    printf("Sintonizando: %.1f MHz\n", (float)FREQ_HZ/1000000.0f);
    printf("Presiona ENTER para salir...\n");

    getchar(); // Esperar usuario

    // 4. Limpieza
    hackrf_stop_rx(device);
    hackrf_close(device);
    hackrf_exit();
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();

    return 0;
}

