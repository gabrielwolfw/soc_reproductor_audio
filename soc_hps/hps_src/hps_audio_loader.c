#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>

// Direcciones para lwhps2fpga bridge
#define HW_REGS_BASE    0xff200000
#define HW_REGS_SPAN    0x00400000

// Offsets en el bridge - Puerto s1 de dual-port memory
#define SHARED_CTRL_OFFSET    0x20000    // s1 base (0x0002_0000)
#define SHARED_AUDIO_OFFSET   0x20400    // s1 + 1KB para estructura de control

// Comandos (coincidir EXACTAMENTE con FPGA)
#define CMD_NONE               0
#define CMD_PLAY               1
#define CMD_PAUSE              2
#define CMD_STOP               3
#define CMD_NEXT               4
#define CMD_PREV               5

#define STATUS_READY           0
#define STATUS_PLAYING         1
#define STATUS_PAUSED          2

// *** ESTRUCTURA UNIFICADA - DEBE COINCIDIR EXACTAMENTE CON NIOS II ***
typedef struct __attribute__((packed)) {
    // Campos básicos de comunicación
    volatile uint32_t command;          // Comando actual
    volatile uint32_t status;           // Estado del sistema
    volatile uint32_t song_id;          // ID de canción actual (0-2)
    
    // Control de chunks
    volatile uint32_t current_chunk;    // Chunk actual en reproducción
    volatile uint32_t total_chunks;     // Total de chunks de la canción
    volatile uint32_t chunk_size;       // Tamaño del chunk actual en bytes
    volatile uint32_t chunk_samples;    // Samples en el chunk actual
    
    // Información de la canción
    volatile uint32_t song_total_size;  // Tamaño total de la canción
    volatile uint32_t song_position;    // Posición actual en bytes
    volatile uint32_t song_duration;    // Duración en samples
    
    // Flags de comunicación - CRÍTICOS
    volatile uint32_t chunk_ready;      // 1=HPS cargó chunk, 0=FPGA consumió
    volatile uint32_t request_next;     // 1=FPGA solicita siguiente chunk
    volatile uint32_t buffer_underrun;  // 1=Buffer vacío (error)
    
    // Sistema de conexión
    volatile uint32_t hps_connected;    // 1=HPS conectado, 0=desconectado
    volatile uint32_t fpga_heartbeat;   // Solo FPGA heartbeat
    
    // Campos de compatibilidad
    volatile uint32_t magic;           // 0xABCD2025
    volatile uint32_t version;         // Protocol version
    volatile uint32_t hps_ready;       // HPS has data ready
    volatile uint32_t fpga_ready;      // FPGA can receive data
    volatile uint32_t data_size;       // Audio data size
    volatile uint32_t sample_rate;     // Current sample rate
    volatile uint32_t channels;        // Number of channels
    volatile uint32_t bits_per_sample; // Bits per sample
    volatile uint32_t current_track;   // Current track
    volatile uint32_t playback_pos;    // Playback position
    volatile uint32_t read_ptr;        // FPGA read pointer
    volatile uint32_t write_ptr;       // HPS write pointer
    volatile uint32_t buffer_level;    // Buffer fill level
    
    // Reservado para expansión futura
    volatile uint32_t reserved[8];
} shared_control_t;

// Global variables
int fd_mem = -1;
void* virtual_base = NULL;
volatile shared_control_t* shared_ctrl = NULL;
volatile uint8_t* shared_audio = NULL;

// Audio configuration
#define AUDIO_CHUNK_SIZE    (120 * 1024)   // 120KB chunks
#define MAX_TRACKS          3

typedef struct {
    char filename[256];
    uint32_t file_size;
    uint32_t num_chunks;
    uint32_t duration_sec;
    FILE* file_handle;
} song_info_t;

song_info_t songs[MAX_TRACKS];
int current_song = 0;
int current_chunk = 0;

void cleanup_and_exit(int sig) {
    printf("\nCleaning up...\n");
    
    if (shared_ctrl) {
        shared_ctrl->command = CMD_STOP;
        shared_ctrl->status = STATUS_READY;
        shared_ctrl->hps_connected = 0;
    }
    
    // Close file handles
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (songs[i].file_handle) {
            fclose(songs[i].file_handle);
        }
    }
    
    // Unmap memory
    if (virtual_base != NULL) {
        munmap(virtual_base, HW_REGS_SPAN);
    }
    
    // Close /dev/mem
    if (fd_mem != -1) {
        close(fd_mem);
    }
    
    exit(0);
}

int map_memory() {
    // Open /dev/mem
    fd_mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_mem == -1) {
        printf("ERROR: Cannot open /dev/mem\n");
        return -1;
    }
    
    // Map lwhps2fpga bridge
    virtual_base = mmap(NULL, HW_REGS_SPAN, PROT_READ | PROT_WRITE, 
                       MAP_SHARED, fd_mem, HW_REGS_BASE);
    
    if (virtual_base == MAP_FAILED) {
        printf("ERROR: Cannot map lwhps2fpga bridge\n");
        close(fd_mem);
        return -1;
    }
    
    printf("lwhps2fpga bridge mapped at virtual address: %p\n", virtual_base);
    
    // Calculate pointers to shared memory regions (puerto s1)
    shared_ctrl = (shared_control_t*)((char*)virtual_base + SHARED_CTRL_OFFSET);
    shared_audio = (uint8_t*)((char*)virtual_base + SHARED_AUDIO_OFFSET);
    
    printf("Shared control at: %p (offset 0x%x)\n", (void*)shared_ctrl, SHARED_CTRL_OFFSET);
    printf("Shared audio at: %p (offset 0x%x)\n", (void*)shared_audio, SHARED_AUDIO_OFFSET);
    printf("Structure size: %zu bytes\n", sizeof(shared_control_t));
    
    // Verificar que la estructura no sea demasiado grande
    if (sizeof(shared_control_t) > 1024) { // 1KB para control
        printf("WARNING: Structure size (%zu bytes) is large\n", sizeof(shared_control_t));
    }
    
    return 0;
}

int load_song_info() {
    printf("Loading song information...\n");
    
    const char* song_paths[MAX_TRACKS] = {
        "/media/sd/songs/song1.wav",
        "/media/sd/songs/song2.wav", 
        "/media/sd/songs/song3.wav"
    };
    
    int loaded_count = 0;
    
    for (int i = 0; i < MAX_TRACKS; i++) {
        FILE* file = fopen(song_paths[i], "rb");
        if (file) {
            // Get file size
            fseek(file, 0, SEEK_END);
            songs[i].file_size = ftell(file);
            fseek(file, 0, SEEK_SET);
            
            // Calculate chunks and duration
            songs[i].num_chunks = (songs[i].file_size + AUDIO_CHUNK_SIZE - 1) / AUDIO_CHUNK_SIZE;
            songs[i].duration_sec = songs[i].file_size / (48000 * 2 * 2); // Approximate
            
            strcpy(songs[i].filename, song_paths[i]);
            songs[i].file_handle = file;
            
            printf("Song %d: %s\n", i+1, songs[i].filename);
            printf("  Size: %.2f MB (%u bytes)\n", songs[i].file_size/1024.0/1024.0, songs[i].file_size);
            printf("  Chunks: %u (%.2f KB each)\n", songs[i].num_chunks, AUDIO_CHUNK_SIZE/1024.0);
            printf("  Duration: ~%.1f seconds\n", (double)songs[i].duration_sec);
            
            loaded_count++;
        } else {
            printf("WARNING: Cannot open %s\n", song_paths[i]);
            songs[i].file_handle = NULL;
            songs[i].file_size = 0;
            songs[i].num_chunks = 0;
        }
    }
    
    printf("\nSuccessfully loaded %d/%d songs\n", loaded_count, MAX_TRACKS);
    return loaded_count > 0 ? 0 : -1;
}

int load_chunk(int song_idx, int chunk_idx) {
    if (song_idx >= MAX_TRACKS || !songs[song_idx].file_handle) {
        printf("ERROR: Invalid song index %d or file not open\n", song_idx);
        return -1;
    }
    
    FILE* file = songs[song_idx].file_handle;
    long offset = chunk_idx * AUDIO_CHUNK_SIZE;
    
    // Verificar que el chunk existe
    if (chunk_idx >= songs[song_idx].num_chunks) {
        printf("ERROR: Chunk %d exceeds total chunks %d\n", chunk_idx, songs[song_idx].num_chunks);
        return -1;
    }
    
    if (fseek(file, offset, SEEK_SET) != 0) {
        printf("ERROR: Cannot seek to chunk %d (offset %ld)\n", chunk_idx, offset);
        return -1;
    }
    
    size_t bytes_read = fread((void*)shared_audio, 1, AUDIO_CHUNK_SIZE, file);
    
    if (bytes_read > 0) {
        // Actualizar información del chunk en estructura compartida
        shared_ctrl->chunk_size = bytes_read;
        shared_ctrl->chunk_samples = bytes_read / 4; // 16-bit stereo = 4 bytes per sample
        shared_ctrl->current_chunk = chunk_idx;
        shared_ctrl->song_position = offset + bytes_read;
        shared_ctrl->data_size = bytes_read;
        shared_ctrl->write_ptr = bytes_read;
        
        // Marcar chunk como listo
        shared_ctrl->chunk_ready = 1;
        shared_ctrl->request_next = 0;
        shared_ctrl->hps_ready = 1;
        
        printf("Loaded chunk %d/%d of song %d (%zu bytes, %u samples)\n", 
               chunk_idx + 1, songs[song_idx].num_chunks, song_idx + 1, 
               bytes_read, shared_ctrl->chunk_samples);
        
        return 0;
    }
    
    printf("ERROR: Failed to read chunk data (read %zu bytes)\n", bytes_read);
    return -1;
}

void update_song_info(int song_idx) {
    if (song_idx >= 0 && song_idx < MAX_TRACKS && songs[song_idx].file_handle) {
        shared_ctrl->song_id = song_idx;
        shared_ctrl->total_chunks = songs[song_idx].num_chunks;
        shared_ctrl->song_total_size = songs[song_idx].file_size;
        shared_ctrl->song_duration = songs[song_idx].duration_sec * 48000; // Convert to samples
        shared_ctrl->current_track = song_idx;
        
        printf("Updated song info: Song %d, %d chunks, %d bytes\n", 
               song_idx + 1, shared_ctrl->total_chunks, shared_ctrl->song_total_size);
    }
}

int main() {
    printf("=== HPS Audio Streaming Loader ===\n");
    printf("Dual-port memory version - HPS using s1 port\n");
    printf("Structure size: %zu bytes\n", sizeof(shared_control_t));
    printf("Compiled: %s %s\n\n", __DATE__, __TIME__);
    
    // Setup signal handler
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);
    
    // Map memory
    printf("Mapping lwhps2fpga bridge to access s1 port...\n");
    if (map_memory() != 0) {
        return -1;
    }
    
    // Load song information
    if (load_song_info() != 0) {
        printf("ERROR: No songs loaded, exiting\n");
        cleanup_and_exit(1);
    }
    
    // Initialize control structure
    printf("Initializing shared control structure...\n");
    
    // Limpiar toda la estructura primero
    memset((void*)shared_ctrl, 0, sizeof(shared_control_t));
    
    // Inicializar campos básicos
    shared_ctrl->magic = 0xABCD2025;
    shared_ctrl->version = 0x00020003;
    shared_ctrl->command = CMD_NONE;
    shared_ctrl->status = STATUS_READY;
    shared_ctrl->hps_connected = 1;  // Marcar HPS como conectado
    shared_ctrl->hps_ready = 1;
    shared_ctrl->sample_rate = 48000;
    shared_ctrl->channels = 2;
    shared_ctrl->bits_per_sample = 16;
    
    // Configurar información de la primera canción
    current_song = 0;
    current_chunk = 0;
    update_song_info(current_song);
    
    printf("Control structure initialized:\n");
    printf("  Magic: 0x%x\n", shared_ctrl->magic);
    printf("  Version: 0x%x\n", shared_ctrl->version);
    printf("  HPS Connected: %d\n", shared_ctrl->hps_connected);
    printf("  Sample Rate: %d\n", shared_ctrl->sample_rate);
    printf("  Channels: %d\n", shared_ctrl->channels);
    printf("  Bits per sample: %d\n", shared_ctrl->bits_per_sample);
    
    // Pre-load first chunk
    printf("Pre-loading first chunk...\n");
    if (load_chunk(current_song, current_chunk) == 0) {
        printf("Ready to play!\n\n");
    } else {
        printf("ERROR: Failed to load first chunk!\n");
        cleanup_and_exit(1);
    }
    
    printf("=== Starting audio streaming loop ===\n");
    printf("Use FPGA buttons to control playback:\n");
    printf("  KEY0 = Play/Pause\n");
    printf("  KEY1 = Next Song\n");
    printf("  KEY2 = Previous Song\n");
    printf("Waiting for FPGA communication...\n\n");
    
    // Main communication loop
    uint32_t last_heartbeat = 0;
    uint32_t loop_counter = 0;
    uint32_t last_command = CMD_NONE;
    
    while (1) {
        // Mantener conexión activa
        shared_ctrl->hps_connected = 1;
        shared_ctrl->hps_ready = 1;
        
        // Verificar heartbeat de FPGA
        if (shared_ctrl->fpga_heartbeat != last_heartbeat) {
            printf("FPGA heartbeat: %u (FPGA active)\n", shared_ctrl->fpga_heartbeat);
            last_heartbeat = shared_ctrl->fpga_heartbeat;
        }
        
        // Check for next chunk request from FPGA
        if (shared_ctrl->request_next && !shared_ctrl->chunk_ready) {
            printf("FPGA requesting next chunk (current: %d/%d)...\n", 
                   current_chunk + 1, songs[current_song].num_chunks);
            
            current_chunk++;
            
            // Check if we need to advance to next song or loop
            if (current_chunk >= songs[current_song].num_chunks) {
                printf("End of song %d reached, advancing to next song\n", current_song + 1);
                current_chunk = 0;
                current_song = (current_song + 1) % MAX_TRACKS;
                
                // Skip songs that failed to load
                int attempts = 0;
                while (!songs[current_song].file_handle && attempts < MAX_TRACKS) {
                    current_song = (current_song + 1) % MAX_TRACKS;
                    attempts++;
                }
                
                if (attempts >= MAX_TRACKS) {
                    printf("ERROR: No valid songs available\n");
                    shared_ctrl->status = STATUS_READY;
                    shared_ctrl->command = CMD_STOP;
                    usleep(100000);
                    continue;
                }
                
                update_song_info(current_song);
                printf("Now playing: Song %d\n", current_song + 1);
            }
            
            // Load next chunk
            if (load_chunk(current_song, current_chunk) == 0) {
                printf("Next chunk loaded successfully\n");
            } else {
                printf("ERROR: Failed to load next chunk\n");
                shared_ctrl->buffer_underrun = 1;
            }
        }
        
        // Manejar comandos de FPGA
        if (shared_ctrl->command != last_command && shared_ctrl->command != CMD_NONE) {
            switch (shared_ctrl->command) {
                case CMD_NEXT:
                    printf("Received NEXT command from FPGA\n");
                    current_song = (current_song + 1) % MAX_TRACKS;
                    
                    // Skip songs that failed to load
                    int attempts = 0;
                    while (!songs[current_song].file_handle && attempts < MAX_TRACKS) {
                        current_song = (current_song + 1) % MAX_TRACKS;
                        attempts++;
                    }
                    
                    current_chunk = 0;
                    update_song_info(current_song);
                    load_chunk(current_song, current_chunk);
                    printf("Switched to song %d\n", current_song + 1);
                    break;
                    
                case CMD_PREV:
                    printf("Received PREV command from FPGA\n");
                    current_song = (current_song - 1 + MAX_TRACKS) % MAX_TRACKS;
                    
                    // Skip songs that failed to load
                    attempts = 0;
                    while (!songs[current_song].file_handle && attempts < MAX_TRACKS) {
                        current_song = (current_song - 1 + MAX_TRACKS) % MAX_TRACKS;
                        attempts++;
                    }
                    
                    current_chunk = 0;
                    update_song_info(current_song);
                    load_chunk(current_song, current_chunk);
                    printf("Switched to song %d\n", current_song + 1);
                    break;
                    
                case CMD_PLAY:
                    printf("Received PLAY command from FPGA\n");
                    shared_ctrl->status = STATUS_PLAYING;
                    break;
                    
                case CMD_PAUSE:
                    printf("Received PAUSE command from FPGA\n");
                    shared_ctrl->status = STATUS_PAUSED;
                    break;
                    
                case CMD_STOP:
                    printf("Received STOP command from FPGA\n");
                    shared_ctrl->status = STATUS_READY;
                    current_chunk = 0;
                    load_chunk(current_song, current_chunk);
                    break;
            }
            
            last_command = shared_ctrl->command;
            // No limpiar el comando aquí - deja que FPGA lo maneje
        }
        
        // Debug info cada 5 segundos
        if ((loop_counter % 500) == 0) {
            printf("=== HPS STATUS ===\n");
            printf("Magic: 0x%x, Connected: %d\n", shared_ctrl->magic, shared_ctrl->hps_connected);
            printf("Command: %d, Status: %d, Song: %d\n", 
                   shared_ctrl->command, shared_ctrl->status, shared_ctrl->song_id);
            printf("Chunk: %d/%d, Ready: %d, Request: %d\n", 
                   shared_ctrl->current_chunk + 1, shared_ctrl->total_chunks,
                   shared_ctrl->chunk_ready, shared_ctrl->request_next);
            printf("Chunk size: %d bytes, Samples: %d\n", 
                   shared_ctrl->chunk_size, shared_ctrl->chunk_samples);
            printf("Buffer level: %d%%, Underrun: %d\n", 
                   shared_ctrl->buffer_level, shared_ctrl->buffer_underrun);
            printf("FPGA heartbeat: %d, Read ptr: %d\n", 
                   shared_ctrl->fpga_heartbeat, shared_ctrl->read_ptr);
            printf("==================\n");
        }
        
        loop_counter++;
        usleep(10000); // 10ms
    }
    
    cleanup_and_exit(0);
    return 0;
}