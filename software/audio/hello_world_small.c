#include "system.h"
#include "sys/alt_stdio.h"
#include "sys/alt_irq.h"
#include "alt_types.h"

// Variables globales para el reproductor
volatile unsigned int elapsed_ms = 0;
volatile unsigned int elapsed_seconds = 0;
volatile unsigned int elapsed_minutes = 0;
volatile unsigned int is_playing = 0;
volatile unsigned int current_track = 1;
volatile unsigned int button_state = 0;
volatile unsigned int prev_button_state = 0;

// Patrones para display 7-segmentos (0-9) - catodo comun
const unsigned char seven_seg_patterns[10] = {
    0x3F, // 0
    0x06, // 1
    0x5B, // 2
    0x4F, // 3
    0x66, // 4
    0x6D, // 5
    0x7D, // 6
    0x07, // 7
    0x7F, // 8
    0x6F  // 9
};

// Funciones
static void timer_ir_handler(void * context, alt_u32 id);
static void audio_ir_handler(void * context, alt_u32 id);
void update_seven_segment_display(void);
void handle_buttons(void);
void play_audio(void);
void pause_audio(void);
void next_track(void);
void previous_track(void);
void init_audio(void);

int main()
{
    // Punteros a los dispositivos usando las direcciones de tu system.h
    volatile unsigned int * button_ptr = (unsigned int *) BUTTONS_BASE;
    volatile unsigned int * timer_status_ptr = (unsigned int *) TIMER_BASE;
    volatile unsigned int * timer_control_ptr = (unsigned int *) (TIMER_BASE + 4);
    volatile unsigned int * sevenseg_ptr = (unsigned int *) SEVEN_SEGMENTS_BASE;
    volatile unsigned int * audio_ptr = (unsigned int *) AUDIO_BASE;

    alt_putstr("=== Audio Player Starting ===\n");

    // Registrar manejadores de interrupciones
    alt_irq_register(TIMER_IRQ, NULL, timer_ir_handler);

    alt_irq_register(AUDIO_IRQ, NULL, audio_ir_handler);

    alt_putstr("Interrupt handlers registered\n");
    alt_printf("Timer IRQ: %d, Audio IRQ: %d\n", TIMER_IRQ, AUDIO_IRQ);

    // Configurar e iniciar el timer
    *timer_control_ptr = 0x7;  // START | CONT | ITO (bits 2,1,0)

    alt_putstr("Timer started\n");

    // Inicializar audio
    init_audio();

    // Inicializar variables
    elapsed_ms = 0;
    elapsed_seconds = 0;
    elapsed_minutes = 0;
    is_playing = 0;
    current_track = 1;  // Empezar en track 1
    button_state = 0;
    prev_button_state = 0;

    alt_putstr("Audio Player Ready!\n");
    alt_putstr("Controls:\n");
    alt_putstr("  Button 0 (KEY0): Play/Pause\n");
    alt_putstr("  Button 1 (KEY1): Next Track\n");
    alt_putstr("  Button 2 (KEY2): Previous Track\n");
    alt_putstr("  Button 3 (KEY3): Stop/Reset\n");

    // Display inicial
    update_seven_segment_display();

    // Loop principal del reproductor
    while (1) {
        // Manejar botones (polling ya que no tienen IRQ configurado)
        handle_buttons();

        // Pequeña pausa para evitar polling excesivo
        for (volatile int i = 0; i < 10000; i++);
    }

    return 0;
}

// CORREGIDO: Agregar alt_u32 id en la firma
static void timer_ir_handler(void * context, alt_u32 id) {
    // Limpiar la interrupción del timer escribiendo cualquier valor al registro de status
    volatile unsigned int* timer_status_ptr = (unsigned int *) TIMER_BASE;
    *timer_status_ptr = 0;  // Limpiar TO bit

    // Incrementar tiempo solo si está reproduciendo
    if (is_playing) {
        elapsed_ms++;
        if (elapsed_ms >= 1000) {  // 1 segundo
            elapsed_ms = 0;
            elapsed_seconds++;
            if (elapsed_seconds >= 60) {  // 1 minuto
                elapsed_seconds = 0;
                elapsed_minutes++;
                if (elapsed_minutes >= 100) {  // Reset después de 99:59
                    elapsed_minutes = 0;
                }
            }
            // Actualizar display cada segundo
            update_seven_segment_display();
        }
    }
}

// MEJORADO: audio_ir_handler con funcionalidad real
static void audio_ir_handler(void * context, alt_u32 id) {
    volatile unsigned int* audio_ptr = (unsigned int *) AUDIO_BASE;

    // Registros específicos del IP de Audio de Altera
    volatile unsigned int* fifospace_reg = (unsigned int*)(AUDIO_BASE + 4);   // FIFOSPACE
    volatile unsigned int* leftdata_reg = (unsigned int*)(AUDIO_BASE + 8);    // LEFTDATA
    volatile unsigned int* rightdata_reg = (unsigned int*)(AUDIO_BASE + 12);  // RIGHTDATA

    if (is_playing) {
        // Leer espacio disponible en FIFO
        unsigned int fifospace = *fifospace_reg;
        unsigned int wsrc = (fifospace & 0x00FF0000) >> 16;  // Write space right channel
        unsigned int wslc = (fifospace & 0xFF000000) >> 24;  // Write space left channel

        // Si hay espacio en ambos canales, enviar datos
        if (wsrc > 0 && wslc > 0) {
            // Generar audio de prueba (tono simple)
            static unsigned int audio_counter = 0;
            static short amplitude = 8000;  // Amplitud del tono

            // Generar tono de prueba basado en el track actual
            unsigned int frequency = 440 + (current_track - 1) * 110;  // A4 + intervalos
            short sample = (short)(amplitude *
                          (audio_counter % (48000 / frequency) < (48000 / frequency / 2) ? 1 : -1));

            // Enviar muestra a ambos canales
            *leftdata_reg = sample;
            *rightdata_reg = sample;

            audio_counter++;

            // Cambiar frecuencia cada cierto tiempo para crear melodía simple
            if (audio_counter % 48000 == 0) {  // Cada segundo
                amplitude = (amplitude == 8000) ? 4000 : 8000;  // Variar volumen
            }
        }

        // Verificar underrun (FIFO vacío)
        unsigned int rsrc = (fifospace & 0x000000FF);       // Read space right channel
        unsigned int rslc = (fifospace & 0x0000FF00) >> 8;  // Read space left channel

        if (rsrc == 0 || rslc == 0) {
            // FIFO underrun - puede necesitar más datos
            // En una implementación real, aquí cargarías más datos del archivo
        }
    }

    // El IP de Audio de Altera típicamente no requiere limpiar interrupciones manualmente
    // pero si es necesario, sería algo como:
    // *audio_ptr = *audio_ptr;  // Leer para limpiar
}

void handle_buttons(void) {
    volatile unsigned int * button_ptr = (unsigned int *) BUTTONS_BASE;

    // Leer estado actual de los botones
    button_state = *button_ptr;

    // Detectar flancos descendentes (botón presionado)
    unsigned int button_pressed = (~button_state) & prev_button_state;

    if (button_pressed & 0x1) {  // Button 0 (KEY0) - Play/Pause
        if (is_playing) {
            pause_audio();
        } else {
            play_audio();
        }
    }

    if (button_pressed & 0x2) {  // Button 1 (KEY1) - Next track
        next_track();
    }

    if (button_pressed & 0x4) {  // Button 2 (KEY2) - Previous track
        previous_track();
    }

    if (button_pressed & 0x8) {  // Button 3 (KEY3) - Stop/Reset
        pause_audio();
        elapsed_ms = 0;
        elapsed_seconds = 0;
        elapsed_minutes = 0;
        update_seven_segment_display();
        alt_putstr("Playback stopped and reset\n");
    }

    prev_button_state = button_state;
}

void update_seven_segment_display(void) {
    volatile unsigned int * sevenseg_ptr = (unsigned int *) SEVEN_SEGMENTS_BASE;

    // Extraer dígitos para MM:SS format
    unsigned char min_tens = elapsed_minutes / 10;
    unsigned char min_ones = elapsed_minutes % 10;
    unsigned char sec_tens = elapsed_seconds / 10;
    unsigned char sec_ones = elapsed_seconds % 10;

    // El display tiene 28 bits de ancho (4 displays de 7 bits cada uno)
    // HEX3[27:21] | HEX2[20:14] | HEX1[13:7] | HEX0[6:0]
    unsigned int display_value =
        ((unsigned int)seven_seg_patterns[min_tens] << 21) |  // HEX3
        ((unsigned int)seven_seg_patterns[min_ones] << 14) |  // HEX2
        ((unsigned int)seven_seg_patterns[sec_tens] << 7) |   // HEX1
        ((unsigned int)seven_seg_patterns[sec_ones]);         // HEX0

    *sevenseg_ptr = display_value;
}

void init_audio(void) {
    volatile unsigned int * audio_ptr = (unsigned int *) AUDIO_BASE;

    // Configuración básica del IP de audio de Altera
    // Control register (offset 0) - habilitar audio
    *audio_ptr = 0x1;  // Enable bit

    // Limpiar FIFOs - escribir en LEFTDATA y RIGHTDATA para inicializar
    volatile unsigned int* leftdata_reg = (unsigned int*)(AUDIO_BASE + 8);
    volatile unsigned int* rightdata_reg = (unsigned int*)(AUDIO_BASE + 12);

    *leftdata_reg = 0;
    *rightdata_reg = 0;

    alt_putstr("Audio IP initialized\n");
}

void play_audio(void) {
    // Iniciar reproducción
    is_playing = 1;
    alt_printf("Playing track %d\n", current_track);
}

void pause_audio(void) {
    // Pausar reproducción
    is_playing = 0;
    alt_putstr("Audio paused\n");
}

void next_track(void) {
    // Pausar audio actual
    if (is_playing) {
        pause_audio();
    }

    current_track++;
    if (current_track > 10) {  // Máximo 10 tracks por ejemplo
        current_track = 1;
    }

    // Reset tiempo
    elapsed_ms = 0;
    elapsed_seconds = 0;
    elapsed_minutes = 0;

    update_seven_segment_display();

    alt_printf("Next track: %d\n", current_track);

    // Reanudar reproducción automáticamente
    play_audio();
}

void previous_track(void) {
    // Pausar audio actual
    if (is_playing) {
        pause_audio();
    }

    current_track--;
    if (current_track < 1) {
        current_track = 10;  // Wrap around al último track
    }

    // Reset tiempo
    elapsed_ms = 0;
    elapsed_seconds = 0;
    elapsed_minutes = 0;

    update_seven_segment_display();

    alt_printf("Previous track: %d\n", current_track);

    // Reanudar reproducción automáticamente
    play_audio();
}
