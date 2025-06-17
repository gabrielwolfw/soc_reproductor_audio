#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <math.h>
#include "hps_0.h"

// Equivalentes a las definiciones de Altera
#define ALT_UP_AUDIO_LEFT  0
#define ALT_UP_AUDIO_RIGHT 1

// Estructura que simula alt_up_audio_dev
typedef struct {
    volatile uint32_t *base_ptr;
    char *name;
} alt_up_audio_dev;

// Base addresses
#define HW_REGS_BASE 0xFF200000
#define HW_REGS_SPAN 0x00200000

// Variables globales
static void *virtual_base;
static alt_up_audio_dev *audio_dev = NULL;
static volatile uint32_t should_exit = 0;
static volatile uint32_t is_playing = 0;
static volatile uint32_t current_freq = 440;
static volatile uint32_t current_track = 1;

// Generación de tonos
static volatile uint32_t tone_phase = 0;
static uint32_t sample_rate = 48000;

// Hardware pointers adicionales
static volatile uint32_t *buttons_ptr;
static volatile uint32_t *seven_seg_ptr;

// Threads
static pthread_t audio_thread;
static pthread_t buttons_thread;
static pthread_t tone_thread;

// Patrones 7-segmentos
static const uint8_t seven_seg_patterns[10] = {
    0x40, 0x79, 0x24, 0x30, 0x19,
    0x12, 0x02, 0x78, 0x00, 0x10
};

// Frecuencias por track
static const uint32_t track_frequencies[10] = {
    440, 523, 659, 784, 880, 1047, 220, 330, 1175, 262
};

// ============== FUNCIONES EQUIVALENTES A ALTERA ==============

// Equivalente a alt_up_audio_open_dev()
alt_up_audio_dev* alt_up_audio_open_dev(const char* name)
{
    alt_up_audio_dev* dev = malloc(sizeof(alt_up_audio_dev));
    if (!dev) return NULL;
    
    dev->base_ptr = (volatile uint32_t*)((char*)virtual_base + AUDIO_BASE);
    dev->name = "/dev/Audio";
    
    // Inicializar audio IP
    dev->base_ptr[0] = 0x0;  // Reset
    usleep(100000);
    dev->base_ptr[0] = 0x1;  // Enable
    usleep(100000);
    
    printf("Opened audio device: %s\n", name);
    return dev;
}

// Equivalente a alt_up_audio_read_fifo_avail()
int alt_up_audio_read_fifo_avail(alt_up_audio_dev* audio_dev, int channel)
{
    if (!audio_dev) return 0;
    
    uint32_t fifospace = audio_dev->base_ptr[1];
    
    if (channel == ALT_UP_AUDIO_LEFT) {
        return (fifospace >> 8) & 0xFF;  // RARC
    } else { // ALT_UP_AUDIO_RIGHT
        return fifospace & 0xFF;         // RRAC
    }
}

// Equivalente a alt_up_audio_write_fifo_avail()
int alt_up_audio_write_fifo_avail(alt_up_audio_dev* audio_dev, int channel)
{
    if (!audio_dev) return 0;
    
    uint32_t fifospace = audio_dev->base_ptr[1];
    
    if (channel == ALT_UP_AUDIO_LEFT) {
        return (fifospace >> 24) & 0xFF;  // WSLC
    } else { // ALT_UP_AUDIO_RIGHT
        return (fifospace >> 16) & 0xFF;  // WSRC
    }
}

// Equivalente a alt_up_audio_read_fifo()
int alt_up_audio_read_fifo(alt_up_audio_dev* audio_dev, unsigned int* buf, 
                          int num_samples, int channel)
{
    if (!audio_dev || !buf) return 0;
    
    // En el audio IP, tanto left como right se leen del mismo registro
    // porque es un codec que maneja entrada
    *buf = audio_dev->base_ptr[2];  // Leer dato disponible
    
    return 1;
}

// Equivalente a alt_up_audio_write_fifo()
int alt_up_audio_write_fifo(alt_up_audio_dev* audio_dev, unsigned int* buf, 
                           int num_samples, int channel)
{
    if (!audio_dev || !buf) return 0;
    
    if (channel == ALT_UP_AUDIO_LEFT) {
        audio_dev->base_ptr[2] = *buf;  // Left channel
    } else { // ALT_UP_AUDIO_RIGHT
        audio_dev->base_ptr[3] = *buf;  // Right channel
    }
    
    return 1;
}

// Equivalente a alt_printf (simplificado)
#define alt_printf printf

// ============== FUNCIONES DEL PROGRAMA ==============

static void signal_handler(int sig) {
    printf("\nExiting...\n");
    should_exit = 1;
}

static void update_display(void) {
    uint8_t track_tens = current_track / 10;
    uint8_t track_ones = current_track % 10;
    uint8_t freq_hundreds = (current_freq / 100) % 10;
    uint8_t freq_tens = (current_freq / 10) % 10;
    
    uint32_t display_value =
        ((uint32_t)seven_seg_patterns[track_tens] << 21) |   // HEX3 - Track tens
        ((uint32_t)seven_seg_patterns[track_ones] << 14) |   // HEX2 - Track ones  
        ((uint32_t)seven_seg_patterns[freq_hundreds] << 7) | // HEX1 - Freq hundreds
        ((uint32_t)seven_seg_patterns[freq_tens]);           // HEX0 - Freq tens
    
    *seven_seg_ptr = display_value;
}

static void next_track(void) {
    current_track++;
    if (current_track > 10) current_track = 1;
    current_freq = track_frequencies[current_track - 1];
    update_display();
    printf("Track: %u, Freq: %u Hz\n", current_track, current_freq);
}

static void previous_track(void) {
    current_track--;
    if (current_track < 1) current_track = 10;
    current_freq = track_frequencies[current_track - 1];
    update_display();
    printf("Track: %u, Freq: %u Hz\n", current_track, current_freq);
}

// Thread que genera tonos cuando está reproduciendo
static void* tone_generator_thread(void* arg)
{
    while (!should_exit) {
        if (is_playing && audio_dev) {
            // Verificar espacio disponible para escritura
            int write_space_left = alt_up_audio_write_fifo_avail(audio_dev, ALT_UP_AUDIO_LEFT);
            int write_space_right = alt_up_audio_write_fifo_avail(audio_dev, ALT_UP_AUDIO_RIGHT);
            
            if (write_space_left > 0 && write_space_right > 0) {
                // Generar muestra de onda seno
                float phase_radians = (2.0 * M_PI * tone_phase * current_freq) / sample_rate;
                float sine_wave = sin(phase_radians);
                
                // Convertir a formato 16-bit
                int16_t sample_16 = (int16_t)(sine_wave * 16383);  // 50% del rango
                uint32_t sample_32 = ((uint32_t)sample_16 << 16) | (sample_16 & 0xFFFF);
                
                // Escribir a ambos canales
                alt_up_audio_write_fifo(audio_dev, &sample_32, 1, ALT_UP_AUDIO_LEFT);
                alt_up_audio_write_fifo(audio_dev, &sample_32, 1, ALT_UP_AUDIO_RIGHT);
                
                tone_phase++;
                if (tone_phase >= sample_rate) tone_phase = 0;  // Reset cada segundo
            }
        }
        usleep(100);  // 0.1ms
    }
    return NULL;
}

// Thread de audio principal (tu código original adaptado)
static void* audio_echo_thread(void* arg)
{
    unsigned int l_buf;
    unsigned int r_buf;
    
    while (!should_exit) {
        if (audio_dev && !is_playing) {  // Solo echo cuando no reproduce tono
            // Tu código original de Altera adaptado
            int fifospace = alt_up_audio_read_fifo_avail(audio_dev, ALT_UP_AUDIO_RIGHT);
            if (fifospace > 0) { // check if data is available
                // read audio buffer
                alt_up_audio_read_fifo(audio_dev, &r_buf, 1, ALT_UP_AUDIO_RIGHT);
                alt_up_audio_read_fifo(audio_dev, &l_buf, 1, ALT_UP_AUDIO_LEFT);
                
                // write audio buffer (echo)
                alt_up_audio_write_fifo(audio_dev, &r_buf, 1, ALT_UP_AUDIO_RIGHT);
                alt_up_audio_write_fifo(audio_dev, &l_buf, 1, ALT_UP_AUDIO_LEFT);
            }
        }
        usleep(1000);  // 1ms
    }
    return NULL;
}

// Thread de botones
static void* buttons_thread_func(void* arg)
{
    static uint32_t prev_button_state = 0x7;
    
    while (!should_exit) {
        usleep(50000); // 50ms
        
        if (buttons_ptr) {
            uint32_t button_state = *buttons_ptr;
            uint32_t button_pressed = (~button_state) & prev_button_state;
            
            if (button_pressed & 0x1) {  // KEY0 - Play/Pause
                is_playing = !is_playing;
                if (is_playing) {
                    printf("*** ▶ PLAYING Tone %u Hz ***\n", current_freq);
                    tone_phase = 0;  // Reset phase
                } else {
                    printf("*** ⏸ PAUSED - Echo Mode ***\n");
                }
                usleep(200000);
            }
            
            if (button_pressed & 0x2) {  // KEY1 - Next track
                next_track();
                usleep(200000);
            }
            
            if (button_pressed & 0x4) {  // KEY2 - Previous track
                previous_track();
                usleep(200000);
            }
            
            prev_button_state = button_state;
        }
    }
    return NULL;
}

int main(void)
{
    int fd;
    
    printf("=== HPS Audio Player (Altera Style) ===\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Mapear memoria
    if ((fd = open("/dev/mem", (O_RDWR | O_SYNC))) == -1) {
        alt_printf("Error: could not open /dev/mem\n");
        return 1;
    }
    
    virtual_base = mmap(NULL, HW_REGS_SPAN, (PROT_READ | PROT_WRITE), 
                       MAP_SHARED, fd, HW_REGS_BASE);
    
    if (virtual_base == MAP_FAILED) {
        alt_printf("Error: mmap() failed\n");
        close(fd);
        return 1;
    }
    
    // Configurar punteros
    buttons_ptr = (volatile uint32_t*)((char*)virtual_base + BUTTONS_BASE);
    seven_seg_ptr = (volatile uint32_t*)((char*)virtual_base + SEVEN_SEGMENTS_BASE);
    
    close(fd);
    
    // Abrir dispositivo de audio (tu código original)
    audio_dev = alt_up_audio_open_dev("/dev/Audio");
    if (audio_dev == NULL) {
        alt_printf("Error: could not open audio device \n");
        munmap(virtual_base, HW_REGS_SPAN);
        return 1;
    } else {
        alt_printf("Opened audio device \n");
    }
    
    // Inicializar variables
    is_playing = 0;  // Inicia en modo echo
    current_track = 1;
    current_freq = 440;
    tone_phase = 0;
    
    update_display();
    
    printf("Audio Player Ready!\n");
    printf("Controls:\n");
    printf("  KEY0: Play Tone / Echo Mode\n");
    printf("  KEY1: Next Track (change frequency)\n");
    printf("  KEY2: Previous Track\n");
    printf("Mode: Echo (speak into mic, hear in headphones)\n");
    
    // Crear threads
    pthread_create(&audio_thread, NULL, audio_echo_thread, NULL);
    pthread_create(&tone_thread, NULL, tone_generator_thread, NULL);
    pthread_create(&buttons_thread, NULL, buttons_thread_func, NULL);
    
    // Loop principal
    while (!should_exit) {
        sleep(2);
        if (is_playing) {
            printf("♪ Generating %u Hz tone\n", current_freq);
        } else {
            printf("🎤 Echo mode active\n");
        }
    }
    
    // Cleanup
    should_exit = 1;
    pthread_join(audio_thread, NULL);
    pthread_join(tone_thread, NULL);
    pthread_join(buttons_thread, NULL);
    
    if (audio_dev) {
        free(audio_dev);
    }
    
    munmap(virtual_base, HW_REGS_SPAN);
    printf("System cleanup complete\n");
    
    return 0;
}