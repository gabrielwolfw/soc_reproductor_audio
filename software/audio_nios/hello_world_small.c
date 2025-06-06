#include "sys/alt_stdio.h"
#include "alt_types.h"
#include "sys/alt_irq.h"
#include "system.h"

// Variables globales para el reproductor
volatile unsigned int elapsed_ms = 0;
volatile unsigned int elapsed_seconds = 0;
volatile unsigned int elapsed_minutes = 0;
volatile unsigned int is_playing = 0;
volatile unsigned int current_track = 0;
volatile unsigned int button_state = 0;
volatile unsigned int prev_button_state = 0;

// Patrones para display 7-segmentos (0-9) - cátodo común
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

// Prototipos de funciones
void timer_ir_handler(void * context);
void audio_ir_handler(void * context);
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

    // Verificar estado inicial del timer
    if (*timer_status_ptr & 0x1) {
        alt_printf("WARNING: Timer TO bit set initially: %x\n", *timer_status_ptr);
    }

    // Registrar manejadores de interrupciones
    alt_ic_isr_register(TIMER_IRQ_INTERRUPT_CONTROLLER_ID, TIMER_IRQ, timer_ir_handler, 0x0, 0x0);
    alt_ic_isr_register(AUDIO_IRQ_INTERRUPT_CONTROLLER_ID, AUDIO_IRQ, audio_ir_handler, 0x0, 0x0);

    alt_putstr("Interrupt handlers registered\n");
    alt_printf("Timer IRQ: %d, Audio IRQ: %d\n", TIMER_IRQ, AUDIO_IRQ);

    // Inicializar el timer para interrupciones de 1ms
    // El timer ya está configurado en Platform Designer para 1ms (TIMER_LOAD_VALUE = 49999)
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

void timer_ir_handler(void * context) {
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
        }
    }
}

void audio_ir_handler(void * context) {
    // Manejar interrupciones del audio IP
    volatile unsigned int* audio_ptr = (unsigned int *) AUDIO_BASE;

    // Leer el registro de status del audio (offset específico del IP de Altera)
    volatile unsigned int* audio_status = audio_ptr + 1; // Offset 1 para status
    unsigned int status = *audio_status;

    // Verificar si es interrupción por buffer vacío o similar
    if (status & 0x1) {  // Ejemplo: bit 0 para FIFO vacío
        // Aquí cargarías más datos de audio si es necesario
        alt_printf("Audio buffer needs data\n");
    }

    // Limpiar la interrupción (método específico del IP)
    *audio_status = 0;
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
    // Los offsets específicos dependen del IP, pero típicamente:

    // Control register (offset 0) - habilitar audio
    *audio_ptr = 0x1;  // Enable bit

    // Limpiar FIFOs si es necesario
    *(audio_ptr + 1) = 0;  // Clear status/control

    alt_putstr("Audio IP initialized\n");
}

void play_audio(void) {
    volatile unsigned int * audio_ptr = (unsigned int *) AUDIO_BASE;

    // Iniciar reproducción
    is_playing = 1;

    // Configurar el audio IP para reproducción
    *audio_ptr |= 0x2;  // Start playback bit (ejemplo)

    alt_printf("Playing track %d\n", current_track);
}

void pause_audio(void) {
    volatile unsigned int * audio_ptr = (unsigned int *) AUDIO_BASE;

    // Pausar reproducción
    is_playing = 0;

    // Pausar el audio IP
    *audio_ptr &= ~0x2;  // Clear playback bit (ejemplo)

    alt_printf("Audio paused\n");
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

    // Aquí cargarías la nueva canción desde SD card
    // load_track_from_sd(current_track);

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

    // Aquí cargarías la nueva canción desde SD card
    // load_track_from_sd(current_track);

    // Reanudar reproducción automáticamente
    play_audio();
}
