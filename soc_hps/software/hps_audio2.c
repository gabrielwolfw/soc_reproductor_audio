#define _POSIX_C_SOURCE 200809L
#define _USE_MATH_DEFINES
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <math.h>
#include <string.h>
#include "hps_0.h"

// Equivalentes a las definiciones de Altera
#define ALT_UP_AUDIO_LEFT  0
#define ALT_UP_AUDIO_RIGHT 1

// Estructura que simula alt_up_audio_dev
typedef struct {
    volatile uint32_t *base_ptr;
    char *name;
} alt_up_audio_dev;

// Estructura para archivos WAV
typedef struct {
    FILE *file;
    char filename[256];
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t data_size;
    uint32_t samples_played;
    uint32_t total_samples;
    uint8_t is_valid;
} wav_file_t;

// Base addresses
#define HW_REGS_BASE 0xFF200000
#define HW_REGS_SPAN 0x00200000

// Variables globales
static void *virtual_base;
static alt_up_audio_dev *audio_dev = NULL;
static volatile uint32_t should_exit = 0;
static volatile uint32_t is_playing = 0;
static volatile uint32_t current_track = 1;
static volatile uint32_t playback_mode = 0;  // 0=WAV files, 1=Tone generator

// Para el timer MM:SS
static volatile uint32_t elapsed_seconds = 0;
static volatile uint32_t elapsed_minutes = 0;
static volatile uint32_t timer_ms = 0;

// Generación de tonos (modo alternativo)
static volatile uint32_t current_freq = 440;
static volatile uint32_t tone_phase = 0;
static uint32_t sample_rate = 48000;

// WAV files
static wav_file_t songs[3];
static const char* song_paths[] = {
    "/media/sd/songs/song1.wav",
    "/media/sd/songs/song2.wav", 
    "/media/sd/songs/song3.wav"
};

// Hardware pointers
static volatile uint32_t *buttons_ptr;
static volatile uint32_t *seven_seg_ptr;

// Threads
static pthread_t audio_thread;
static pthread_t buttons_thread;
static pthread_t timer_thread;

// Patrones 7-segmentos
static const uint8_t seven_seg_patterns[10] = {
    0x40, 0x79, 0x24, 0x30, 0x19,
    0x12, 0x02, 0x78, 0x00, 0x10
};

// Frecuencias por track (para modo tono)
static const uint32_t track_frequencies[10] = {
    440, 523, 659, 784, 880, 1047, 220, 330, 1175, 262
};

// ============== FUNCIONES EQUIVALENTES A ALTERA ==============

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

#define alt_printf printf

// ============== FUNCIONES WAV ==============

static int parse_wav_header(wav_file_t* wav)
{
    char header[44];
    
    if (fread(header, 1, 44, wav->file) != 44) {
        printf("Error reading WAV header\n");
        return 0;
    }
    
    // Verificar "RIFF"
    if (strncmp(header, "RIFF", 4) != 0) {
        printf("Not a valid WAV file (missing RIFF)\n");
        return 0;
    }
    
    // Verificar "WAVE"
    if (strncmp(header + 8, "WAVE", 4) != 0) {
        printf("Not a valid WAV file (missing WAVE)\n");
        return 0;
    }
    
    // Extraer información
    wav->channels = *(uint16_t*)(header + 22);
    wav->sample_rate = *(uint32_t*)(header + 24);
    wav->bits_per_sample = *(uint16_t*)(header + 34);
    wav->data_size = *(uint32_t*)(header + 40);
    
    // Calcular total de samples
    wav->total_samples = wav->data_size / (wav->channels * (wav->bits_per_sample / 8));
    wav->samples_played = 0;
    
    printf("WAV Info: %u Hz, %u channels, %u bits, %u samples\n",
           wav->sample_rate, wav->channels, wav->bits_per_sample, wav->total_samples);
    
    return 1;
}

static int load_song(int track_num)
{
    if (track_num < 1 || track_num > 3) return 0;
    
    int idx = track_num - 1;
    
    // Cerrar archivo anterior si está abierto
    if (songs[idx].file) {
        fclose(songs[idx].file);
        songs[idx].file = NULL;
    }
    
    // Abrir nuevo archivo
    songs[idx].file = fopen(song_paths[idx], "rb");
    if (!songs[idx].file) {
        printf("Error: Cannot open %s\n", song_paths[idx]);
        songs[idx].is_valid = 0;
        return 0;
    }
    
    strcpy(songs[idx].filename, song_paths[idx]);
    
    if (!parse_wav_header(&songs[idx])) {
        fclose(songs[idx].file);
        songs[idx].file = NULL;
        songs[idx].is_valid = 0;
        return 0;
    }
    
    songs[idx].is_valid = 1;
    printf("Loaded: %s\n", songs[idx].filename);
    return 1;
}

// ============== FUNCIONES DEL PROGRAMA ==============

static void signal_handler(int sig) {
    printf("\nExiting...\n");
    should_exit = 1;
}

static void update_display(void) {
    uint8_t min_tens = elapsed_minutes / 10;
    uint8_t min_ones = elapsed_minutes % 10;
    uint8_t sec_tens = elapsed_seconds / 10;
    uint8_t sec_ones = elapsed_seconds % 10;
    
    uint32_t display_value =
        ((uint32_t)seven_seg_patterns[min_tens] << 21) |   // HEX3 - Minutes tens
        ((uint32_t)seven_seg_patterns[min_ones] << 14) |   // HEX2 - Minutes ones
        ((uint32_t)seven_seg_patterns[sec_tens] << 7) |    // HEX1 - Seconds tens
        ((uint32_t)seven_seg_patterns[sec_ones]);          // HEX0 - Seconds ones
    
    *seven_seg_ptr = display_value;
}

static void reset_timer(void) {
    timer_ms = 0;
    elapsed_seconds = 0;
    elapsed_minutes = 0;
    update_display();
}

static void next_track(void) {
    uint8_t was_playing = is_playing;
    if (is_playing) is_playing = 0;
    
    current_track++;
    if (playback_mode == 0) {  // WAV mode
        if (current_track > 3) current_track = 1;
        load_song(current_track);
    } else {  // Tone mode
        if (current_track > 10) current_track = 1;
        current_freq = track_frequencies[current_track - 1];
    }
    
    reset_timer();
    
    if (playback_mode == 0) {
        printf("Track: %u - %s\n", current_track, songs[current_track-1].filename);
    } else {
        printf("Tone: %u - %u Hz\n", current_track, current_freq);
    }
    
    if (was_playing) is_playing = 1;
}

static void previous_track(void) {
    uint8_t was_playing = is_playing;
    if (is_playing) is_playing = 0;
    
    current_track--;
    if (playback_mode == 0) {  // WAV mode
        if (current_track < 1) current_track = 3;
        load_song(current_track);
    } else {  // Tone mode
        if (current_track < 1) current_track = 10;
        current_freq = track_frequencies[current_track - 1];
    }
    
    reset_timer();
    
    if (playback_mode == 0) {
        printf("Track: %u - %s\n", current_track, songs[current_track-1].filename);
    } else {
        printf("Tone: %u - %u Hz\n", current_track, current_freq);
    }
    
    if (was_playing) is_playing = 1;
}

static void toggle_mode(void) {
    uint8_t was_playing = is_playing;
    is_playing = 0;
    
    playback_mode = !playback_mode;
    current_track = 1;
    
    if (playback_mode == 0) {
        printf("*** WAV MODE - Real Songs ***\n");
        load_song(current_track);
    } else {
        printf("*** TONE MODE - Generated Tones ***\n");
        current_freq = track_frequencies[0];
        tone_phase = 0;
    }
    
    reset_timer();
    
    if (was_playing) is_playing = 1;
}

// Thread del timer (MM:SS)
static void* timer_thread_func(void* arg)
{
    while (!should_exit) {
        usleep(100000);  // 100ms
        
        if (is_playing) {
            timer_ms += 100;
            if (timer_ms >= 1000) {  // 1 segundo
                timer_ms = 0;
                elapsed_seconds++;
                if (elapsed_seconds >= 60) {
                    elapsed_seconds = 0;
                    elapsed_minutes++;
                    if (elapsed_minutes >= 100) {
                        elapsed_minutes = 0;  // Reset a 00:00 después de 99:59
                    }
                }
                update_display();
            }
        }
    }
    return NULL;
}

// Thread de audio principal
static void* audio_playback_thread(void* arg)
{
    while (!should_exit) {
        if (is_playing && audio_dev) {
            
            if (playback_mode == 0) {
                // ===== MODO WAV =====
                int idx = current_track - 1;
                if (songs[idx].is_valid && songs[idx].file) {
                    
                    int write_space_left = alt_up_audio_write_fifo_avail(audio_dev, ALT_UP_AUDIO_LEFT);
                    int write_space_right = alt_up_audio_write_fifo_avail(audio_dev, ALT_UP_AUDIO_RIGHT);
                    
                    if (write_space_left > 0 && write_space_right > 0) {
                        
                        if (songs[idx].bits_per_sample == 16) {
                            int16_t left_sample = 0, right_sample = 0;
                            
                            // Leer samples del archivo
                            if (fread(&left_sample, 2, 1, songs[idx].file) == 1) {
                                if (songs[idx].channels == 2) {
                                    fread(&right_sample, 2, 1, songs[idx].file);
                                } else {
                                    right_sample = left_sample;  // Mono -> Stereo
                                }
                                
                                // Convertir a formato 32-bit
                                uint32_t sample_32 = ((uint32_t)(left_sample & 0xFFFF) << 16) | 
                                                    (right_sample & 0xFFFF);
                                
                                // Escribir a ambos canales
                                alt_up_audio_write_fifo(audio_dev, &sample_32, 1, ALT_UP_AUDIO_LEFT);
                                alt_up_audio_write_fifo(audio_dev, &sample_32, 1, ALT_UP_AUDIO_RIGHT);
                                
                                songs[idx].samples_played++;
                                
                                // Verificar si la canción terminó
                                if (songs[idx].samples_played >= songs[idx].total_samples) {
                                    printf("Song finished, going to next track\n");
                                    next_track();
                                }
                                
                            } else {
                                // Error o fin de archivo
                                printf("End of file or read error\n");
                                next_track();
                            }
                        }
                    }
                }
                
            } else {
                // ===== MODO TONO =====
                int write_space_left = alt_up_audio_write_fifo_avail(audio_dev, ALT_UP_AUDIO_LEFT);
                int write_space_right = alt_up_audio_write_fifo_avail(audio_dev, ALT_UP_AUDIO_RIGHT);
                
                if (write_space_left > 0 && write_space_right > 0) {
                    // Generar muestra de onda seno
                    float phase_radians = (2.0 * M_PI * tone_phase * current_freq) / sample_rate;
                    float sine_wave = sin(phase_radians);
                    
                    // Convertir a formato 16-bit
                    int16_t sample_16 = (int16_t)(sine_wave * 16383);
                    uint32_t sample_32 = ((uint32_t)sample_16 << 16) | (sample_16 & 0xFFFF);
                    
                    // Escribir a ambos canales
                    alt_up_audio_write_fifo(audio_dev, &sample_32, 1, ALT_UP_AUDIO_LEFT);
                    alt_up_audio_write_fifo(audio_dev, &sample_32, 1, ALT_UP_AUDIO_RIGHT);
                    
                    tone_phase++;
                    if (tone_phase >= sample_rate) tone_phase = 0;
                }
            }
        }
        usleep(100);  // 0.1ms
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
                    if (playback_mode == 0) {
                        printf("*** ▶ PLAYING Song %u ***\n", current_track);
                    } else {
                        printf("*** ▶ PLAYING Tone %u Hz ***\n", current_freq);
                    }
                } else {
                    printf("*** ⏸ PAUSED ***\n");
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
            
            if (button_pressed & 0x8) {  // KEY3 - Toggle mode (si tienes 4 botones)
                toggle_mode();
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
    
    printf("=== HPS Audio Player with WAV Support ===\n");
    
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
    
    // Abrir dispositivo de audio
    audio_dev = alt_up_audio_open_dev("/dev/Audio");
    if (audio_dev == NULL) {
        alt_printf("Error: could not open audio device \n");
        munmap(virtual_base, HW_REGS_SPAN);
        return 1;
    } else {
        alt_printf("Opened audio device \n");
    }
    
    // Inicializar variables
    is_playing = 0;
    current_track = 1;
    playback_mode = 0;  // Inicia en modo WAV
    
    // Cargar primera canción
    printf("Loading songs...\n");
    for (int i = 0; i < 3; i++) {
        if (load_song(i + 1)) {
            printf("✓ Song %d loaded\n", i + 1);
        } else {
            printf("✗ Song %d failed to load\n", i + 1);
        }
    }
    
    reset_timer();
    
    printf("Audio Player Ready!\n");
    printf("Mode: WAV Songs\n");
    printf("Controls:\n");
    printf("  KEY0: Play/Pause\n");
    printf("  KEY1: Next Track\n");
    printf("  KEY2: Previous Track\n");
    printf("  KEY3: Toggle WAV/Tone Mode (if available)\n");
    printf("Current: Song %u\n", current_track);
    
    // Crear threads
    pthread_create(&audio_thread, NULL, audio_playback_thread, NULL);
    pthread_create(&buttons_thread, NULL, buttons_thread_func, NULL);
    pthread_create(&timer_thread, NULL, timer_thread_func, NULL);
    
    // Loop principal
    while (!should_exit) {
        sleep(3);
        if (is_playing) {
            if (playback_mode == 0) {
                printf("♪ Playing Song %u - %02u:%02u\n", current_track, elapsed_minutes, elapsed_seconds);
            } else {
                printf("♪ Playing Tone %u Hz - %02u:%02u\n", current_freq, elapsed_minutes, elapsed_seconds);
            }
        } else {
            printf("⏸ Paused - %02u:%02u\n", elapsed_minutes, elapsed_seconds);
        }
    }
    
    // Cleanup
    should_exit = 1;
    pthread_join(audio_thread, NULL);
    pthread_join(buttons_thread, NULL);
    pthread_join(timer_thread, NULL);
    
    // Cerrar archivos WAV
    for (int i = 0; i < 3; i++) {
        if (songs[i].file) {
            fclose(songs[i].file);
        }
    }
    
    if (audio_dev) {
        free(audio_dev);
    }
    
    munmap(virtual_base, HW_REGS_SPAN);
    printf("System cleanup complete\n");
    
    return 0;
}