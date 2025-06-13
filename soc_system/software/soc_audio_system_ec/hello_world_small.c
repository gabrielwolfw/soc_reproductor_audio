#include "system.h"
#include "sys/alt_stdio.h"
#include "sys/alt_irq.h"
#include <stdint.h>
#include <unistd.h>
#include "altera_up_avalon_audio.h"

#define SAMPLE_RATE 48000

// Variables globales
alt_up_audio_dev *audio_dev = NULL;
volatile int is_playing = 0;
volatile int current_track = 1;
volatile int current_freq = 440;
volatile int elapsed_ms = 0, elapsed_seconds = 0, elapsed_minutes = 0;

// Variables para generación de audio
static volatile int sample_index = 0;

// Valores de la onda cuadrada (ajustados para 24-bit)
unsigned int sample_high = 0x7FFFFF; // 24-bit max (positivo)
unsigned int sample_low  = 0x800000; // 24-bit min (negativo)

// 7 segmentos: patrones para 0-9
const unsigned char seven_seg_patterns[10] = {
    0x40, 0x79, 0x24, 0x30, 0x19,
    0x12, 0x02, 0x78, 0x00, 0x10
};

// Prototipos
void update_seven_segment_display(void);
void handle_buttons(void);
void generate_audio_samples(void);

// --- Interrupción Timer (500ms) ---
static void timer_isr(void* context) {
    volatile unsigned int* timer_status = (unsigned int*) (TIMER_BASE);
    *timer_status = 0; // Limpia TO

    if (is_playing) {
        elapsed_ms += 500;
        if (elapsed_ms >= 1000) {
            elapsed_ms = 0;
            elapsed_seconds++;
            if (elapsed_seconds >= 60) {
                elapsed_seconds = 0;
                elapsed_minutes++;
                if (elapsed_minutes >= 100)
                    elapsed_minutes = 0;
            }
        }
        update_seven_segment_display();
    }
}

// --- Interrupción Audio usando el driver correcto ---
static void audio_isr(void* context) {
    if (!is_playing || audio_dev == NULL)
        return;

    // Verificar espacio disponible en FIFO usando el driver
    int write_space_left = alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_LEFT);
    int write_space_right = alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_RIGHT);

    // Solo escribir si hay espacio en ambos canales
    if (write_space_left > 0 && write_space_right > 0) {
        // Lógica de generación de onda cuadrada
        int samples_per_cycle = SAMPLE_RATE / current_freq;
        int half_cycle = samples_per_cycle / 2;

        // Genera la onda cuadrada
        unsigned int sample = (sample_index % samples_per_cycle < half_cycle) ? sample_high : sample_low;

        // Escribir usando el driver de Altera
        alt_up_audio_write_fifo(audio_dev, &sample, 1, ALT_UP_AUDIO_LEFT);
        alt_up_audio_write_fifo(audio_dev, &sample, 1, ALT_UP_AUDIO_RIGHT);

        sample_index++;
    }
}

// --- Función para generar audio en polling (alternativa) ---
void generate_audio_samples(void) {
    if (!is_playing || audio_dev == NULL)
        return;

    // Verificar espacio disponible en FIFO
    int write_space = alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_LEFT);

    if (write_space > 0) {
        // Generar múltiples muestras para llenar el buffer
        for (int i = 0; i < write_space && i < 32; i++) {
            int samples_per_cycle = SAMPLE_RATE / current_freq;
            int half_cycle = samples_per_cycle / 2;

            // Genera la onda cuadrada
            unsigned int sample = (sample_index % samples_per_cycle < half_cycle) ? sample_high : sample_low;

            // Escribir a ambos canales
            alt_up_audio_write_fifo(audio_dev, &sample, 1, ALT_UP_AUDIO_LEFT);
            alt_up_audio_write_fifo(audio_dev, &sample, 1, ALT_UP_AUDIO_RIGHT);

            sample_index++;
        }
    }
}

// --- 7 segmentos MMSS ---
void update_seven_segment_display(void) {
    volatile unsigned int * sevenseg = (unsigned int *) SEVEN_SEGMENTS_BASE;
    unsigned char min_tens = elapsed_minutes / 10;
    unsigned char min_ones = elapsed_minutes % 10;
    unsigned char sec_tens = elapsed_seconds / 10;
    unsigned char sec_ones = elapsed_seconds % 10;

    unsigned int display_value =
        ((unsigned int)seven_seg_patterns[min_tens] << 21) |   // HEX3
        ((unsigned int)seven_seg_patterns[min_ones] << 14) |   // HEX2
        ((unsigned int)seven_seg_patterns[sec_tens] << 7)  |   // HEX1
        ((unsigned int)seven_seg_patterns[sec_ones]);          // HEX0

    *sevenseg = display_value;
}

// --- Botones: Play/Pause, Next, Prev ---
void handle_buttons(void) {
    static int prev_button_state = 0x7;
    volatile unsigned int * button_ptr = (unsigned int *) BUTTONS_BASE;
    int button_state = *button_ptr;
    int button_pressed = (~button_state) & prev_button_state;

    if (button_pressed & 0x1) { // KEY0: Play/Pause
        is_playing = !is_playing;
        if (is_playing) {
            sample_index = 0; // Reset sample index when starting
        }
        alt_printf("Play/Pause - Track %d (%d Hz) - %s\n",
                   current_track, current_freq, is_playing ? "Playing" : "Paused");
    }

    if (button_pressed & 0x2) { // KEY1: Next
        current_track++;
        if (current_track > 10) current_track = 1;

        switch (current_track) {
            case 1: current_freq = 440; break;   // A4
            case 2: current_freq = 523; break;   // C5
            case 3: current_freq = 659; break;   // E5
            case 4: current_freq = 784; break;   // G5
            case 5: current_freq = 880; break;   // A5
            case 6: current_freq = 1047; break;  // C6
            case 7: current_freq = 220; break;   // A3
            case 8: current_freq = 330; break;   // E4
            case 9: current_freq = 1175; break;  // D6
            case 10: current_freq = 262; break;  // C4
            default: current_freq = 440; break;
        }

        elapsed_ms = 0; elapsed_seconds = 0; elapsed_minutes = 0;
        sample_index = 0;
        update_seven_segment_display();
        alt_printf("Next track: %d (%d Hz)\n", current_track, current_freq);
    }

    if (button_pressed & 0x4) { // KEY2: Previous
        current_track--;
        if (current_track < 1) current_track = 10;

        switch (current_track) {
            case 1: current_freq = 440; break;
            case 2: current_freq = 523; break;
            case 3: current_freq = 659; break;
            case 4: current_freq = 784; break;
            case 5: current_freq = 880; break;
            case 6: current_freq = 1047; break;
            case 7: current_freq = 220; break;
            case 8: current_freq = 330; break;
            case 9: current_freq = 1175; break;
            case 10: current_freq = 262; break;
            default: current_freq = 440; break;
        }

        elapsed_ms = 0; elapsed_seconds = 0; elapsed_minutes = 0;
        sample_index = 0;
        update_seven_segment_display();
        alt_printf("Previous track: %d (%d Hz)\n", current_track, current_freq);
    }

    prev_button_state = button_state;
}

int main(void) {
    alt_putstr("=== Audio Player (Buttons + 7seg) ===\n");

    // Inicializar audio usando el driver de Altera
    audio_dev = alt_up_audio_open_dev("/dev/AUDIO");
    if (audio_dev == NULL) {
        alt_printf("Error: No se pudo abrir el dispositivo de audio\n");
        return -1;
    } else {
        alt_printf("Dispositivo de audio abierto correctamente\n");
    }

    // --- Registra interrupciones ---
    alt_irq_register(TIMER_IRQ, NULL, timer_isr);
    // Comentar la interrupción de audio si usas polling
    alt_irq_register(AUDIO_IRQ, NULL, audio_isr);

    // --- Configurar Timer ---
    volatile unsigned int* timer_control = (unsigned int*)(TIMER_BASE + 4);
    *timer_control = 0x7; // START | CONT | ITO

    // Inicialización de variables
    is_playing = 0;
    current_track = 1;
    current_freq = 440;
    elapsed_ms = 0;
    elapsed_seconds = 0;
    elapsed_minutes = 0;
    sample_index = 0;

    update_seven_segment_display();
    alt_putstr("Controles: KEY0 Play/Pause | KEY1 Next | KEY2 Prev\n");

    // Loop principal
    while (1) {
        handle_buttons();

        // Generar audio usando polling (más confiable que interrupciones)
        generate_audio_samples();

        // Pequeño delay para evitar saturar el CPU
        for (volatile int i = 0; i < 10000; i++);
    }

    return 0;
}
