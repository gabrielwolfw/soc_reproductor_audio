#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include "hps_0.h"

// Base addresses
#define HW_REGS_BASE 0xFF200000
#define HW_REGS_SPAN 0x00200000

#define SQUARE_TABLE_SIZE 256

// Variables globales (igual que tenías en NIOS II)
static uint32_t square_table[SQUARE_TABLE_SIZE];
static void *virtual_base;
static volatile uint32_t elapsed_ms = 0;
static volatile uint32_t elapsed_seconds = 0;
static volatile uint32_t elapsed_minutes = 0;
static volatile uint32_t is_playing = 0;
static volatile uint32_t current_track = 1;
static volatile uint32_t current_freq = 440;
static volatile uint32_t audio_phase = 0;
static volatile uint32_t should_exit = 0;

// Hardware pointers
static volatile uint32_t *buttons_ptr;
static volatile uint32_t *seven_seg_ptr;
static volatile uint32_t *timer_ptr;
static volatile uint32_t *audio_ptr;

// Threads que simulan interrupciones
static pthread_t timer_thread;
static pthread_t audio_thread;
static pthread_t buttons_thread;

// Patrones para display (igual que tenías)
static const uint8_t seven_seg_patterns[10] = {
    0x40, 0x79, 0x24, 0x30, 0x19,
    0x12, 0x02, 0x78, 0x00, 0x10
};

// Funciones (mismas que tenías en NIOS II)
static void fill_square_table(void);
static void update_seven_segment_display(void);
static void play_audio(void);
static void pause_audio(void);
static void next_track(void);
static void previous_track(void);
static void init_audio(void);

// "Interrupt handlers" simulados con threads
static void* timer_ir_handler(void* context);
static void* audio_ir_handler(void* context); 
static void* buttons_ir_handler(void* context);

static void signal_handler(int sig) {
    printf("\nExiting...\n");
    should_exit = 1;
}

int main()
{
    int fd;
    
    printf("=== HPS Audio Player Starting ===\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Abrir /dev/mem y mapear memoria
    if ((fd = open("/dev/mem", (O_RDWR | O_SYNC))) == -1) {
        printf("ERROR: could not open \"/dev/mem\"...\n");
        return 1;
    }
    
    virtual_base = mmap(NULL, HW_REGS_SPAN, (PROT_READ | PROT_WRITE), 
                       MAP_SHARED, fd, HW_REGS_BASE);
    
    if (virtual_base == MAP_FAILED) {
        printf("ERROR: mmap() failed...\n");
        close(fd);
        return 1;
    }
    
    // Configurar punteros a hardware
    buttons_ptr = (volatile uint32_t*)((char*)virtual_base + BUTTONS_BASE);
    seven_seg_ptr = (volatile uint32_t*)((char*)virtual_base + SEVEN_SEGMENTS_BASE);
    timer_ptr = (volatile uint32_t*)((char*)virtual_base + TIMER_BASE);
    audio_ptr = (volatile uint32_t*)((char*)virtual_base + AUDIO_BASE);
    
    close(fd);
    
    printf("Memory mapping successful\n");
    
    // "Registrar interrupciones" creando threads (equivalente a alt_irq_register)
    if (pthread_create(&timer_thread, NULL, timer_ir_handler, NULL) != 0) {
        printf("Error creating timer thread\n");
        return 1;
    }
    
    if (pthread_create(&audio_thread, NULL, audio_ir_handler, NULL) != 0) {
        printf("Error creating audio thread\n");
        return 1;
    }
    
    if (pthread_create(&buttons_thread, NULL, buttons_ir_handler, NULL) != 0) {
        printf("Error creating buttons thread\n");
        return 1;
    }
    
    printf("Interrupt handlers registered (Timer + Audio + Buttons)\n");
    
    // Configurar e iniciar el timer (igual que hacías)
    timer_ptr[1] = 0x0;  // Stop timer  
    timer_ptr[0] = 0x0;  // Clear TO bit
    timer_ptr[1] = 0x7;  // START | CONT | ITO
    
    printf("Timer started\n");
    
    // Inicializar audio
    init_audio();
    
    // Inicializar variables (igual que tenías)
    elapsed_ms = 0;
    elapsed_seconds = 0;
    elapsed_minutes = 0;
    is_playing = 0;
    current_track = 1;
    current_freq = 440;
    audio_phase = 0;
    
    printf("Audio Player Ready!\n");
    printf("Controls:\n");
    printf("  Button 0: Play/Pause\n");
    printf("  Button 1: Next Track\n");
    printf("  Button 2: Previous Track\n");
    
    // Display inicial
    update_seven_segment_display();
    
    // Loop principal - ahora solo espera (como en kernel module)
    while (!should_exit) {
        sleep(1);
        printf("Track: %u, Freq: %u Hz, Time: %02u:%02u\n", 
               current_track, current_freq, elapsed_minutes, elapsed_seconds);
    }
    
    // Esperar threads
    should_exit = 1;
    pthread_join(timer_thread, NULL);
    pthread_join(audio_thread, NULL); 
    pthread_join(buttons_thread, NULL);
    
    // Cleanup
    munmap(virtual_base, HW_REGS_SPAN);
    printf("System cleanup complete\n");
    
    return 0;
}

// Simulación de timer_ir_handler (equivalente exacto a tu handler de NIOS II)
static void* timer_ir_handler(void* context)
{
    while (!should_exit) {
        usleep(500000); // 500ms como tu timer
        
        // Simular limpiar TO bit
        if (timer_ptr) timer_ptr[0] = 0x0;
        
        if (is_playing) {
            elapsed_ms += 500;
            if (elapsed_ms >= 1000) {  // 1000ms = 1 segundo
                elapsed_ms = 0;
                elapsed_seconds++;
                if (elapsed_seconds >= 60) {  // 60 segundos = 1 minuto
                    elapsed_seconds = 0;
                    elapsed_minutes++;
                    if (elapsed_minutes >= 100) {  // Reset después de 99:59
                        elapsed_minutes = 0;
                    }
                }
                update_seven_segment_display();
            }
        }
    }
    return NULL;
}

// Simulación de audio_ir_handler (equivalente exacto a tu handler de NIOS II)
static void* audio_ir_handler(void* context)
{
    static uint32_t sample_rate = 48000;
    static uint32_t sample_counter = 0;
    
    while (!should_exit) {
        usleep(1000); // 1ms
        
        if (is_playing && audio_ptr) {
            // Verificar espacio en FIFO (igual que tenías)
            uint32_t fifospace = audio_ptr[1];
            uint32_t left_space = (fifospace >> 24) & 0xFF;
            uint32_t right_space = (fifospace >> 16) & 0xFF;
            
            if (left_space > 0 && right_space > 0) {
                // Calcular incremento de fase (igual que tenías)
                uint32_t phase_inc = (current_freq * SQUARE_TABLE_SIZE) / sample_rate;
                
                // Obtener sample de la tabla (igual que tenías)
                uint32_t sample_32bit = square_table[audio_phase];
                
                // Escribir a ambos canales (igual que tenías)
                audio_ptr[2] = sample_32bit; // Left
                audio_ptr[3] = sample_32bit; // Right
                
                // Actualizar fase (igual que tenías)
                audio_phase = (audio_phase + phase_inc) % SQUARE_TABLE_SIZE;
                
                // Debug cada 48000 samples (igual que tenías)
                sample_counter++;
                if (sample_counter >= 48000) {
                    sample_counter = 0;
                    printf("Audio: freq=%u Hz, sample=0x%08X\n",
                          current_freq, sample_32bit);
                }
            }
        }
    }
    return NULL;
}

// Simulación de button interrupt handler
static void* buttons_ir_handler(void* context)
{
    static uint32_t prev_button_state = 0x7;
    
    while (!should_exit) {
        usleep(50000); // 50ms
        
        if (buttons_ptr) {
            // Leer estado actual de los botones (igual que tenías)
            uint32_t button_state = *buttons_ptr;
            
            // Detectar flancos descendentes (igual que tenías)
            uint32_t button_pressed = (~button_state) & prev_button_state;
            
            if (button_pressed & 0x1) {  // Button 0 - Play/Pause
                if (is_playing) {
                    pause_audio();
                } else {
                    play_audio();
                }
            }
            
            if (button_pressed & 0x2) {  // Button 1 - Next track  
                next_track();
            }
            
            if (button_pressed & 0x4) {  // Button 2 - Previous track
                previous_track();
            }
            
            prev_button_state = button_state;
        }
    }
    return NULL;
}

// Todas tus funciones originales sin cambios
static void fill_square_table(void) {
    printf("Filling square wave table (32-bit format)...\n");
    
    for (int i = 0; i < SQUARE_TABLE_SIZE; i++) {
        if (i < SQUARE_TABLE_SIZE / 2) {
            square_table[i] = 0x7FFF0000;  // HIGH
        } else {
            square_table[i] = 0x80000000;  // LOW
        }
    }
    
    printf("Square wave table filled\n");
}

static void update_seven_segment_display(void) {
    uint8_t min_tens = elapsed_minutes / 10;
    uint8_t min_ones = elapsed_minutes % 10;
    uint8_t sec_tens = elapsed_seconds / 10;
    uint8_t sec_ones = elapsed_seconds % 10;
    
    uint32_t display_value =
        ((uint32_t)seven_seg_patterns[min_tens] << 21) |  // HEX3
        ((uint32_t)seven_seg_patterns[min_ones] << 14) |  // HEX2
        ((uint32_t)seven_seg_patterns[sec_tens] << 7) |   // HEX1
        ((uint32_t)seven_seg_patterns[sec_ones]);         // HEX0
    
    *seven_seg_ptr = display_value;
}

static void play_audio(void) {
    is_playing = 1;
    printf("*** PLAYING TRACK %u - %u Hz ***\n", current_track, current_freq);
}

static void pause_audio(void) {
    is_playing = 0;
    printf("*** PAUSED TRACK %u ***\n", current_track);
}

static void next_track(void) {
    uint8_t was_playing = is_playing;
    if (is_playing) {
        pause_audio();
    }
    
    current_track++;
    if (current_track > 10) {
        current_track = 1;
    }
    
    // Cambiar frecuencia según el track (igual que tenías)
    switch(current_track) {
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
    
    // Reset tiempo (igual que tenías)
    elapsed_ms = 0;
    elapsed_seconds = 0;
    elapsed_minutes = 0;
    
    update_seven_segment_display();
    printf("Next track: %u (%u Hz)\n", current_track, current_freq);
    
    if (was_playing) {
        play_audio();
    }
}

static void previous_track(void) {
    uint8_t was_playing = is_playing;
    if (is_playing) {
        pause_audio();
    }
    
    current_track--;
    if (current_track < 1) {
        current_track = 10;
    }
    
    // Cambiar frecuencia según el track (igual que tenías)
    switch(current_track) {
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
    
    // Reset tiempo (igual que tenías)
    elapsed_ms = 0;
    elapsed_seconds = 0;
    elapsed_minutes = 0;
    
    update_seven_segment_display();
    printf("Previous track: %u (%u Hz)\n", current_track, current_freq);
    
    if (was_playing) {
        play_audio();
    }
}

static void init_audio(void) {
    printf("Initializing 32-bit Audio IP...\n");
    
    // Reset del audio IP (igual que tenías)
    audio_ptr[0] = 0x0;
    printf("Audio reset, waiting...\n");
    usleep(500000);
    
    // Habilitar audio IP (igual que tenías)
    audio_ptr[0] = 0x1;
    printf("Audio enabled, waiting...\n");
    usleep(500000);
    
    // Llenar tabla de onda cuadrada
    fill_square_table();
    
    // Llenar FIFO inicial con silencio (igual que tenías)
    printf("Filling initial FIFO with silence...\n");
    for (int i = 0; i < 100; i++) {
        audio_ptr[2] = 0x00000000; // Left
        audio_ptr[3] = 0x00000000; // Right
        usleep(1000);
    }
    
    printf("Audio IP initialization complete!\n");
    
    // Debug info (igual que tenías)
    uint32_t fifospace = audio_ptr[1];
    printf("FIFO Space after init: 0x%08X\n", fifospace);
}