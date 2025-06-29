#include "altera_up_avalon_audio.h"
#include "sys/alt_stdio.h"
#include <unistd.h>

#define SAMPLE_RATE 48000 // 48kHz típico
#define FREQ        440   // 440 Hz (La)
#define DURATION_MS 500   // Duración del tono en ms

int main(void)
{
    alt_up_audio_dev *audio_dev;
    unsigned int sample_high = 0x7FFFFF; // 24-bit max (positivo)
    unsigned int sample_low  = 0x800000; // 24-bit min (negativo)

    // open the Audio port
    audio_dev = alt_up_audio_open_dev("/dev/AUDIO");
    if (audio_dev == NULL) {
        alt_printf("Error: could not open audio device\n");
        return 1;
    }
    alt_printf("Opened audio device\n");

    // Calcula muestras por ciclo y total de muestras
    int samples_per_cycle = SAMPLE_RATE / FREQ;
    int half_cycle = samples_per_cycle / 2;
    int total_samples = (DURATION_MS * SAMPLE_RATE) / 1000;

    for (int i = 0; i < total_samples; ++i) {
        unsigned int sample = (i % samples_per_cycle < half_cycle) ? sample_high : sample_low;

        // Espera espacio en el FIFO
        while (alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_LEFT) == 0 ||
               alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_RIGHT) == 0);

        // Escribe en ambos canales
        alt_up_audio_write_fifo(audio_dev, &sample, 1, ALT_UP_AUDIO_LEFT);
        alt_up_audio_write_fifo(audio_dev, &sample, 1, ALT_UP_AUDIO_RIGHT);
    }

    alt_printf("Fin del tono\n");

    // Espera un poco antes de salir para asegurar reproducción
    usleep(200000);

    return 0;
}
